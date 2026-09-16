#include "tnt/mapgen.h"

#include "gaf/gaf.h"
#include "sct/sct.h"
#include "tdf/tdf.h"
#include "util/strcase.h"

#include "hpi/hpi.h"   // coast prefab sections are read through the VFS

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string>

namespace ta::mapgen {

namespace {

// ---- deterministic integer helpers (no float -> identical on every peer) -------

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// Hashed lattice value 0..255 at integer grid point (x,y) for a given seed.
uint32_t latticeVal(uint64_t seed, int x, int y) {
    uint64_t h = seed ^ 0x100000001b3ULL;
    h = (h ^ uint64_t(uint32_t(x))) * 0x9e3779b97f4a7c15ULL;
    h = (h ^ uint64_t(uint32_t(y))) * 0xc2b2ae3d27d4eb4fULL;
    h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ULL; h ^= h >> 32;
    return uint32_t(h & 0xff);
}

// Bilinear value noise 0..255 at cell (cx,cz), lattice spacing L (>0).
uint32_t noise(uint64_t seed, int cx, int cz, int L) {
    int gx = (cx >= 0 ? cx : cx - L + 1) / L, gz = (cz >= 0 ? cz : cz - L + 1) / L;
    int fx = cx - gx * L, fz = cz - gz * L;                 // 0..L-1
    uint32_t v00 = latticeVal(seed, gx, gz),     v10 = latticeVal(seed, gx + 1, gz);
    uint32_t v01 = latticeVal(seed, gx, gz + 1), v11 = latticeVal(seed, gx + 1, gz + 1);
    uint32_t a = (v00 * uint32_t(L - fx) + v10 * uint32_t(fx)) / uint32_t(L);
    uint32_t b = (v01 * uint32_t(L - fx) + v11 * uint32_t(fx)) / uint32_t(L);
    return (a * uint32_t(L - fz) + b * uint32_t(fz)) / uint32_t(L);
}

// Two-octave fractal noise 0..255 (large land masses + medium detail).
uint32_t fractal(uint64_t seed, int cx, int cz, int span) {
    int L1 = std::max(8, span / 5), L2 = std::max(4, span / 11);
    uint32_t a = noise(seed, cx, cz, L1);
    uint32_t b = noise(seed ^ 0xABCDEF, cx, cz, L2);
    return (a * 7 + b * 3) / 10;   // 70% coarse + 30% fine
}

// ---- terrain art -------------------------------------------------------------
//
// Kingdoms maps referenced shared terrain art by key, so a generator only had to
// pick a key. A TA map carries its own tile library, so a generated map has to
// bring its own pixels -- and the only source of real ones is the world's prefab
// SECTIONS in worlds.hpi, which are the same 32px tiles the shipped maps are
// built from. harvestTiles() pulls a ground and a water set out of them.
//
// Classifying a harvested tile as ground or water: the section container records
// no sea level, so the split has to come from the art. Mean BLUE DOMINANCE --
// blue minus the greater of red and green, over the tile's pixels through
// palettes/PALETTE.PAL -- separates them cleanly. Checked against ground truth
// taken from the shipped maps (a tile whose four cells all sit >=6 below that
// map's sea level is water, all >=6 above it is ground): water averages 149 on
// that measure and ground 30, and a threshold of 35 classifies 99.7% of water and
// 99.0% of ground correctly over ~54k labelled tiles.
constexpr int kWaterBlueDominance = 35;

// How many distinct tiles of each class to harvest. Enough that wallpapering does
// not read as an obvious repeat; small enough that the library stays a few KB.
constexpr int kVariants = 8;

// Fallback ramp ends (indices into palettes/PALETTE.PAL) for an install whose
// sections are missing or unreadable. Dithered noise is not retail art, but a
// generator that produces a blank map is worse than one that produces a plain one.
struct RampArt { uint8_t groundLo, groundHi, seaLo, seaHi; };
constexpr RampArt kFallbackRamp = {32, 47, 97, 104};

// Authoring sea/land levels per world, measured from the shipped maps: each map
// was assigned a world by looking its features up in features/**/*.tdf (which
// declare `world=`), then its own sea level and the 75th percentile of its land
// heights were taken, and the median across that world's maps is what is listed.
//
// `land` is floored at sea+16. Archipelago's measured pair is 85/86 -- its maps
// really are flat sand a single height unit above the waterline -- and a
// generator building land one unit proud of the sea produces a map that is all
// shoreline and no ground to stand on.
struct WorldLevels { uint8_t sea, land; };
constexpr std::array<WorldLevels, kMapTypes> kLevels = {{
    {85, 101},   // Archipelago  (measured 85 / 86, floored)
    {45,  86},   // GreenWorld   (measured 45 / 86)
    {24,  98},   // Lava         (measured 24 / 98)
    {55,  87},   // Mars         (measured 55 / 87)
    {75, 139},   // Metal        (measured 75 / 139)
    { 0,  55},   // Moon         (measured  0 / 55 -- no water at all)
}};

// The world's name as it appears in the data (sections/<name>/, features/<name>/).
// VFS keys are lowercased, so these are matched case-insensitively.
constexpr std::array<const char*, kMapTypes> kWorldName = {{
    "archipelago", "greenworld", "lava", "mars", "metal", "moon",
}};

// The feature palette is DISCOVERED from features/<world>/*.tdf rather than
// listed here. Kingdoms' names were predictable enough to hardcode (AraTree01..10,
// AraRock01..07, AraMana01..03 per house); TA's are not -- each world names its
// own art freely, and there are far more worlds than the six that ship sections.
// Reading the TDFs also means a mod's features appear without a code change.
//
// Names are sorted, so the list is identical on every peer with the same install.
struct WorldPalette {
    std::vector<std::string> trees, rocks, metal;
    bool usable() const { return !trees.empty() || !rocks.empty() || !metal.empty(); }
};

// Harvest distinct 32x32 tiles from a world's prefab sections, split into ground
// and water by blue dominance. Sections are visited in sorted VFS order and tiles
// within one in index order, so the result is identical on every peer with the
// same install. Stops as soon as both classes are full, which is typically the
// first section or two.
struct Harvest {
    std::vector<uint8_t> ground, water;   // kVariants * kTileBytes each, when full
    int nGround = 0, nWater = 0;
};

Harvest harvestTiles(const ta::hpi::Vfs& vfs, const std::string& world) {
    Harvest h;
    ta::gaf::Palette pal;
    try {
        pal = ta::gaf::Palette::fromBytes(vfs.read("palettes/PALETTE.PAL"),
                                          "palettes/PALETTE.PAL");
    } catch (const std::exception&) {
        return h;   // no palette, no classification -- caller falls back
    }
    const std::string root = "sections/" + world + "/";
    // Distinct by content, so a section's many repeats of one tile count once.
    std::vector<uint64_t> seen;
    auto hashTile = [](const uint8_t* t) {
        uint64_t x = 1469598103934665603ULL;
        for (int i = 0; i < ta::tnt::kTileBytes; ++i)
            x = (x ^ t[i]) * 1099511628211ULL;
        return x;
    };
    for (const std::string& path : vfs.list("sections")) {
        if (h.nGround >= kVariants && h.nWater >= kVariants) break;
        std::string lo = path;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        if (lo.rfind(root, 0) != 0 || !ta::sct::isSectionPath(lo)) continue;
        ta::tnt::Map sec;
        try { sec = ta::sct::load(vfs.read(path), path); }
        catch (const std::exception&) { continue; }
        for (int t = 0; t < sec.numTiles; ++t) {
            if (h.nGround >= kVariants && h.nWater >= kVariants) break;
            const uint8_t* px = sec.tile(t);
            if (!px) continue;
            uint64_t sig = hashTile(px);
            if (std::find(seen.begin(), seen.end(), sig) != seen.end()) continue;
            seen.push_back(sig);
            // Mean colour of the tile, then blue dominance.
            long r = 0, g = 0, b = 0;
            for (int i = 0; i < ta::tnt::kTileBytes; ++i) {
                const auto& c = pal.rgba[px[i]];
                r += c[0]; g += c[1]; b += c[2];
            }
            const long n = ta::tnt::kTileBytes;
            const long dom = b / n - std::max(r / n, g / n);
            const bool water = dom > kWaterBlueDominance;
            if (water && h.nWater < kVariants) {
                h.water.insert(h.water.end(), px, px + ta::tnt::kTileBytes);
                ++h.nWater;
            } else if (!water && h.nGround < kVariants) {
                h.ground.insert(h.ground.end(), px, px + ta::tnt::kTileBytes);
                ++h.nGround;
            }
        }
    }
    return h;
}

// Read the world's feature palette out of features/<world>/*.tdf (plus the shared
// "all worlds" set), split by the category each def declares. Sorted, so every
// peer with the same install builds the same list in the same order.
WorldPalette discoverFeatures(const ta::hpi::Vfs& vfs, const std::string& world) {
    WorldPalette wp;
    for (const std::string& path : vfs.list("features")) {
        if (!ta::iendsWith(path, ".tdf")) continue;
        std::string lo = path;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        ta::tdf::Node root;
        try { auto b = vfs.read(path); root = ta::tdf::parseText(std::string(b.begin(), b.end()), path); }
        catch (const std::exception&) { continue; }
        for (const auto& key : root.childOrder) {
            const auto* def = root.child(key);
            if (!def) continue;
            std::string w = def->valueOr("world", "");
            std::transform(w.begin(), w.end(), w.begin(), ::tolower);
            // A def's own `world=` decides, not the directory it sits in: the
            // shared "all worlds" files declare allworlds, and a few worlds'
            // features live outside a directory named after them.
            const bool shared = w == "allworlds" || w == "allworld";
            if (w != world && !shared) continue;
            std::string cat = def->valueOr("category", "");
            std::transform(cat.begin(), cat.end(), cat.begin(), ::tolower);
            // The section name IS the feature name the map plane refers to.
            if (cat == "trees") wp.trees.push_back(key);
            else if (cat == "rocks") wp.rocks.push_back(key);
            // Metal patches are the world's own, never the shared set: a shared
            // metal feature would put the wrong world's art on the map.
            else if (cat == "metal" && !shared) wp.metal.push_back(key);
        }
    }
    std::sort(wp.trees.begin(), wp.trees.end());
    std::sort(wp.rocks.begin(), wp.rocks.end());
    std::sort(wp.metal.begin(), wp.metal.end());
    return wp;
}

// Even compass directions (integer, scaled by 1000) for start-position rings.
constexpr std::array<std::pair<int, int>, 8> kCompass = {{
    {0, -1000}, {707, -707}, {1000, 0}, {707, 707},
    {0, 1000}, {-707, 707}, {-1000, 0}, {-707, -707},
}};

}  // namespace

const char* worldName(uint8_t mapType) {
    return kWorldName[mapType < kMapTypes ? mapType : 0];
}

uint8_t worldType(const std::string& name) {
    std::string lo = name;
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    for (uint8_t i = 0; i < kMapTypes; ++i)
        if (lo == kWorldName[i]) return i;
    return Archipelago;
}

void worldLevels(uint8_t mapType, uint8_t& sea, uint8_t& land) {
    const WorldLevels& lv = kLevels[mapType < kMapTypes ? mapType : 0];
    sea = lv.sea;
    land = lv.land;
}

Params sanitize(Params p) {
    if (p.mapType >= kMapTypes) p.mapType = Archipelago;
    // Even cells, multiples of 32 (a 512px section unit), 128..768 cells/side.
    auto fix = [](uint16_t v) -> uint16_t {
        int u = std::clamp(int(v) / 32, 4, 24);   // 4..24 section-units
        return uint16_t(u * 32);
    };
    p.widthCells = fix(p.widthCells);
    p.heightCells = fix(p.heightCells);
    p.players = uint8_t(std::clamp<int>(p.players, 2, 8));
    return p;
}

Result generate(const Params& raw, const ta::hpi::Vfs& vfs) {
    // Unused since the coast-prefab pass went: nothing is read from the install
    // any more. Kept in the signature because sourcing real tiles from worlds.hpi
    // is the next step for this generator and will want it straight back.
    (void)vfs;
    Params p = sanitize(raw);
    Result r;
    ta::tnt::Map& m = r.map;
    const int W = p.widthCells, H = p.heightCells;
    m.width = W; m.height = H;
    m.blocksX = W / 2; m.blocksY = H / 2;
    const int SW = W / 32, SH = H / 32;   // map size in 512px section units

    // ---- coarse land/water mask on the SECTION-CORNER grid ------------------------
    // The coastline is decided at 512px granularity so whole coast prefabs can be
    // stamped -- this is what gives retail's sweeping curves instead of a 32px
    // block staircase. Adjacent sections share corners, so the marching cases mesh.
    const WorldLevels lv = kLevels[p.mapType];
    m.seaLevel = lv.sea;
    const int land = lv.land;
    const int span = std::max(W, H);
    // waterDensity picks how much of the fractal range falls below the shoreline.
    const int seaThresh = std::clamp(40 + int(p.waterDensity) * 85 / 255, 30, 180);
    std::vector<uint8_t> wet(size_t(SW + 1) * (SH + 1));
    for (int j = 0; j <= SH; ++j)
        for (int i = 0; i <= SW; ++i)
            wet[size_t(j) * (SW + 1) + i] =
                uint8_t(int(fractal(p.seed, i * 32, j * 32, span)) < seaThresh);
    auto wetAt = [&](int i, int j) {
        i = std::clamp(i, 0, SW); j = std::clamp(j, 0, SH);
        return int(wet[size_t(j) * (SW + 1) + i]);
    };
    auto scase = [&](int sx, int sy) {
        return wetAt(sx, sy) | (wetAt(sx + 1, sy) << 1) |
               (wetAt(sx, sy + 1) << 2) | (wetAt(sx + 1, sy + 1) << 3);
    };
    // No prefab depicts a diagonal pinch (cases 6/9 -- retail never authors them):
    // dry the offending corner until the mask is clean. Deterministic scan order.
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (int sy = 0; sy < SH; ++sy)
            for (int sx = 0; sx < SW; ++sx) {
                int c = scase(sx, sy);
                if (c == 6) { wet[size_t(sy) * (SW + 1) + sx + 1] = 0; changed = true; }
                if (c == 9) { wet[size_t(sy) * (SW + 1) + sx] = 0; changed = true; }
            }
        if (!changed) break;
    }

    // ---- heights -------------------------------------------------------------------
    // Interior land sections get the procedural terraces (base plateau = the
    // world's authored flat level, so it meets the prefab land edges exactly);
    // water sections sit at 0 (retail deep-water floor); shoreline sections get
    // the prefab's authored bank heights when stamped below.
    const int landRange = std::max(1, 255 - seaThresh);
    // reliefDensity scales how much of the interior rises into plateaus: 0 = dead
    // flat, 128 ~= the previous default (~40% of land raised), 255 = mostly mesa.
    const int raised = int(p.reliefDensity) * 79 / 255;            // % of land raised
    const int t1 = seaThresh + landRange * (100 - raised) / 100;   // base plateau
    const int t2 = t1 + (255 - t1) * 5 / 8;                        // mid vs high split
    const int kL1 = std::min(250, land + 42), kL2 = std::min(250, land + 92);
    // Raised terraces only where the whole 3x3 section neighbourhood is land, so
    // the blur can never bleed a hill into a stamped prefab's edge rows.
    std::vector<uint8_t> interior(size_t(SW) * SH, 0);
    for (int sy = 0; sy < SH; ++sy)
        for (int sx = 0; sx < SW; ++sx) {
            bool ok = true;
            for (int dy = -1; dy <= 1 && ok; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (scase(std::clamp(sx + dx, 0, SW - 1),
                              std::clamp(sy + dy, 0, SH - 1)) != 0) { ok = false; break; }
            interior[size_t(sy) * SW + sx] = uint8_t(ok);
        }
    m.heights.resize(size_t(W) * H);
    for (int z = 0; z < H; ++z)
        for (int x = 0; x < W; ++x) {
            int c = scase(x >> 5, z >> 5);
            uint8_t h;
            if (c == 15) h = 0;
            else if (c != 0 || !interior[size_t(z >> 5) * SW + (x >> 5)]) h = uint8_t(land);
            else {
                int rr = int(fractal(p.seed, x, z, span));
                h = uint8_t(rr < t1 ? land : rr < t2 ? kL1 : kL2);
            }
            m.heights[size_t(z) * W + x] = h;
        }
    // Box-blur the terrace steps into walkable ramps (interior only by
    // construction; shoreline sections are overwritten by the prefab stamp).
    auto hAt = [&](int x, int z) {
        x = std::clamp(x, 0, W - 1); z = std::clamp(z, 0, H - 1);
        return int(m.heights[size_t(z) * W + x]);
    };
    std::vector<uint8_t> tmp(m.heights.size());
    for (int pass = 0; pass < 6; ++pass) {
        for (int z = 0; z < H; ++z)
            for (int x = 0; x < W; ++x)
                tmp[size_t(z) * W + x] = uint8_t((4 * hAt(x, z) + hAt(x - 1, z) + hAt(x + 1, z)
                                                  + hAt(x, z - 1) + hAt(x, z + 1)) / 8);
        m.heights.swap(tmp);
    }

    // ---- terrain tiles: harvest a library, then wallpaper it ------------------
    // A TA map owns its art, so build the tile library first: kVariants tiles per
    // class, ground then sea. They are REAL tiles, lifted out of this world's
    // prefab sections, so a generated map is painted with the same pixels the
    // shipped maps are. If the sections cannot be read (a stripped install), fall
    // back to dithering a palette ramp with the same integer lattice noise the
    // heightfield uses -- not retail art, but a plain map beats a blank one.
    const Harvest harvest = harvestTiles(vfs, kWorldName[p.mapType]);
    const bool realArt = harvest.nGround >= kVariants && harvest.nWater >= kVariants;
    m.numTiles = kVariants * 2;
    m.tileGfx.assign(size_t(m.numTiles) * ta::tnt::kTileBytes, 0);
    if (realArt) {
        std::copy(harvest.ground.begin(), harvest.ground.end(), m.tileGfx.begin());
        std::copy(harvest.water.begin(), harvest.water.end(),
                  m.tileGfx.begin() + size_t(kVariants) * ta::tnt::kTileBytes);
    } else {
        for (int t = 0; t < m.numTiles; ++t) {
            const bool sea = t >= kVariants;
            const uint8_t lo = sea ? kFallbackRamp.seaLo : kFallbackRamp.groundLo;
            const uint8_t hi = sea ? kFallbackRamp.seaHi : kFallbackRamp.groundHi;
            uint8_t* px = &m.tileGfx[size_t(t) * ta::tnt::kTileBytes];
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x) {
                    // Offset the lattice per variant so the variants differ, and
                    // sample at tile-local coordinates so each tile is self-contained.
                    uint32_t n = latticeVal(p.seed ^ (uint64_t(t) << 24), x, y);
                    px[y * 32 + x] = uint8_t(lo + (n * uint32_t(hi - lo + 1)) / 256u);
                }
        }
    }

    size_t blocks = size_t(m.blocksX) * m.blocksY;
    m.tiles.assign(blocks, 0);
    for (int by = 0; by < m.blocksY; ++by)
        for (int bx = 0; bx < m.blocksX; ++bx) {
            size_t i = size_t(by) * m.blocksX + bx;
            bool water = (scase(bx >> 4, by >> 4) == 15);
            int variant = int(latticeVal(p.seed ^ 0x71a7e, bx, by) % uint32_t(kVariants));
            m.tiles[i] = uint16_t((water ? kVariants : 0) + variant);
        }
    m.features.assign(size_t(W) * H, 0xFFFF);

    // ---- start positions: N spread around a ring, snapped to the nearest solid
    //      land, kept off the edges. Deterministic (integer compass table) -------
    auto isLand = [&](int cx, int cz) {
        if (cx < 0 || cz < 0 || cx >= W || cz >= H) return false;
        // Dry (not shallows) AND on an interior land section -- keeps starts,
        // mana and doodads off the stamped prefab beaches.
        return int(m.heights[size_t(cz) * W + cx]) > m.seaLevel + 3 &&
               scase(cx >> 5, cz >> 5) == 0;
    };
    // Nearest land to (cx,cz) via an expanding square scan (bounded), else the point.
    auto snapLand = [&](int cx, int cz) -> std::pair<int, int> {
        for (int rad = 0; rad < std::max(W, H); ++rad) {
            for (int dz = -rad; dz <= rad; ++dz)
                for (int dx = -rad; dx <= rad; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dz)) != rad) continue;   // ring only
                    if (isLand(cx + dx, cz + dz)) return {cx + dx, cz + dz};
                }
        }
        return {cx, cz};
    };
    int ccx = W / 2, ccz = H / 2;
    int ringR = std::min(W, H) * 35 / 100;   // cells
    int margin = 8;
    for (int k = 0; k < int(p.players); ++k) {
        int ci = k * 8 / int(p.players);      // even-ish pick from the 8-way compass
        auto [dx, dz] = kCompass[size_t(ci)];
        int rx = std::clamp(ccx + dx * ringR / 1000, margin, W - 1 - margin);
        int rz = std::clamp(ccz + dz * ringR / 1000, margin, H - 1 - margin);
        r.starts.push_back(snapLand(rx, rz));
    }

    // ---- features ----------------------------------------------------------------
    // Mana deposits (each a glowing Sacred Stone centre ringed by Standing Stones)
    // and doodads (varied trees + rocks by density). Anchor cell holds the feature
    // index; a local claim-map reserves footprints so nothing overlaps. All draws
    // come from one integer PRNG in a fixed order => byte-identical on every peer.
    const WorldPalette wp = discoverFeatures(vfs, kWorldName[p.mapType]);
    m.featureNames.clear();
    auto featIdx = [&](const std::string& nm) -> uint16_t {
        for (size_t i = 0; i < m.featureNames.size(); ++i)
            if (m.featureNames[i] == nm) return uint16_t(i);
        m.featureNames.emplace_back(nm);
        return uint16_t(m.featureNames.size() - 1);
    };
    auto nearStart = [&](int cx, int cz, int pad) {
        for (auto& [sx, sz] : r.starts) {
            int dx = cx - sx, dz = cz - sz;
            if (dx * dx + dz * dz < pad * pad) return true;
        }
        return false;
    };
    // Footprint reservation, separate from features[] (which stores only anchors):
    // fits() checks a nominal footprint is clear land; place() writes the anchor and
    // claims its footprint (centred on the anchor, matching the sim's nav blocking)
    // so later features avoid it.
    std::vector<uint8_t> claim(size_t(W) * H, 0);
    auto fits = [&](int cx, int cz, int fx, int fz) {
        for (int dz = 0; dz < fz; ++dz)
            for (int dx = 0; dx < fx; ++dx) {
                int nx = cx + dx - fx / 2, nz = cz + dz - fz / 2;
                if (!isLand(nx, nz) || claim[size_t(nz) * W + nx]) return false;
            }
        return true;
    };
    auto place = [&](int cx, int cz, const std::string& nm, int fx, int fz) {
        m.features[size_t(cz) * W + cx] = featIdx(nm);
        for (int dz = 0; dz < fz; ++dz)
            for (int dx = 0; dx < fx; ++dx) {
                int nx = cx + dx - fx / 2, nz = cz + dz - fz / 2;
                if (nx >= 0 && nz >= 0 && nx < W && nz < H) claim[size_t(nz) * W + nx] = 1;
            }
    };
    uint64_t frng = p.seed ^ 0x5eed1234abcdULL;

    // One metal patch. TA's is a single 3x3 feature -- ArchMetal1/2/3 and each
    // world's equivalents -- and nothing else: no ring, no centre-plus-ruins
    // arrangement. (Kingdoms' mana deposit was a Sacred Stone centre ringed by
    // arc-segment Standing Stones at canonical compass offsets, reproduced here
    // from the shipped maps; TA has no analogue, so that whole apparatus is gone
    // rather than left placing features no TA install can resolve.)
    //
    // The variant is drawn uniformly from the world's metal features, which are
    // its richness tiers: ArchMetal1/2/3 carry metal=187/373/746, so a uniform
    // draw is what gives a map a mix of poor and rich patches.
    std::vector<std::pair<int, int>> depots;
    auto okDepot = [&](int cx, int cz, int minSp) {
        for (auto& [dx0, dz0] : depots) {
            int dx = cx - dx0, dz = cz - dz0;
            if (dx * dx + dz * dz < minSp * minSp) return false;
        }
        return true;
    };
    auto placeDeposit = [&](int cx, int cz) {
        if (wp.metal.empty()) return;
        const std::string& nm = wp.metal[size_t(splitmix(frng) % wp.metal.size())];
        place(cx, cz, nm, 3, 3);
        depots.push_back({cx, cz});
    };
    // Try to drop a deposit within [rmin,rmax] cells of (tx,tz), on spaced land.
    auto tryDepositNear = [&](int tx, int tz, int rmin, int rmax) {
        for (int t = 0; t < 80; ++t) {
            int dx = int(splitmix(frng) % uint64_t(2 * rmax + 1)) - rmax;
            int dz = int(splitmix(frng) % uint64_t(2 * rmax + 1)) - rmax;
            int d2 = dx * dx + dz * dz;
            if (d2 < rmin * rmin || d2 > rmax * rmax) continue;
            int cx = tx + dx, cz = tz + dz;
            if (cx < 3 || cz < 3 || cx >= W - 3 || cz >= H - 3) continue;
            if (!isLand(cx, cz) || !fits(cx, cz, 3, 3) || !okDepot(cx, cz, 18)) continue;
            placeDeposit(cx, cz);
            return true;
        }
        return false;
    };

    // >=3 deposits near every start, then a density-scaled scatter across the map.
    for (auto& [sx, sz] : r.starts)
        for (int placed = 0, t = 0; placed < 3 && t < 12; ++t)
            if (tryDepositNear(sx, sz, 16, 44)) ++placed;
    int area = W * H;
    int scatter = std::clamp(area / std::max(1, 22000 - int(p.metalDensity) * 70), 0, 48);
    for (int placed = 0, tries = 0; placed < scatter && tries < scatter * 200 + 400; ++tries) {
        int cx = int(splitmix(frng) % uint64_t(W)), cz = int(splitmix(frng) % uint64_t(H));
        if (cx < 3 || cz < 3 || cx >= W - 3 || cz >= H - 3) continue;
        if (!isLand(cx, cz) || nearStart(cx, cz, 14) || !fits(cx, cz, 3, 3) || !okDepot(cx, cz, 22))
            continue;
        placeDeposit(cx, cz);
        ++placed;
    }

    // Doodads: independent per-cell probabilities per TYPE (each has its own
    // slider). Every placement picks a fresh variant so nothing repeats; rocks are
    // small-biased via min-of-two draws. One PRNG draw per cell, fixed order =>
    // byte-identical on every peer.
    const int treeThresh = int(p.treeDensity) * 5 / 4;   // /10000 (max ~3% of land)
    const int rockThresh = int(p.rockDensity) * 5 / 8;   // /10000 (max ~1.6%)
    for (int cz = 0; cz < H; ++cz)
        for (int cx = 0; cx < W; ++cx) {
            uint64_t roll = splitmix(frng);   // one draw per cell (keeps order deterministic)
            if (!isLand(cx, cz) || claim[size_t(cz) * W + cx] || nearStart(cx, cz, 6)) continue;
            if (!wp.trees.empty() && int(roll % 10000) < treeThresh) {
                const std::string& nm = wp.trees[(roll >> 24) % wp.trees.size()];
                if (fits(cx, cz, 2, 2)) place(cx, cz, nm, 2, 2);
            } else if (!wp.rocks.empty() && int((roll >> 13) % 10000) < rockThresh) {
                // Small-biased: min of two draws. The list is sorted by name, which
                // for TA's rock art runs small->big within a world's numbering.
                size_t a = (roll >> 24) % wp.rocks.size();
                size_t b = (roll >> 33) % wp.rocks.size();
                if (fits(cx, cz, 3, 3)) place(cx, cz, wp.rocks[std::min(a, b)], 3, 3);
            }
        }
    return r;
}

// ---- mapId carrier: "~gen1~" + hex of the packed Params -----------------------

namespace {
constexpr const char* kMagic = "~gen1~";

void put16(std::string& b, uint16_t v) { b.push_back(char(v)); b.push_back(char(v >> 8)); }
void put64(std::string& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(char(v >> (8 * i))); }
std::string toHex(const std::string& raw) {
    static const char* H = "0123456789abcdef";
    std::string s; s.reserve(raw.size() * 2);
    for (unsigned char c : raw) { s.push_back(H[c >> 4]); s.push_back(H[c & 15]); }
    return s;
}
int hexNib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

bool isGeneratedMapId(const std::string& id) {
    return id.size() >= 6 && id.compare(0, 6, kMagic) == 0;
}

std::string encodeMapId(const Params& pin) {
    Params p = sanitize(pin);
    std::string b;
    put16(b, p.formatVer);
    put64(b, p.seed);
    b.push_back(char(p.mapType));
    put16(b, p.widthCells); put16(b, p.heightCells);
    b.push_back(char(p.players));
    b.push_back(char(p.treeDensity));
    b.push_back(char(p.rockDensity));
    b.push_back(char(p.metalDensity));
    b.push_back(char(p.waterDensity));
    b.push_back(char(p.reliefDensity));
    return std::string(kMagic) + toHex(b);
}

Params decodeMapId(const std::string& id) {
    Params p;
    if (!isGeneratedMapId(id)) return p;
    // De-hex the payload.
    std::string raw;
    for (size_t i = 6; i + 1 < id.size(); i += 2) {
        int hi = hexNib(id[i]), lo = hexNib(id[i + 1]);
        if (hi < 0 || lo < 0) return sanitize(p);
        raw.push_back(char((hi << 4) | lo));
    }
    auto u8 = [&](size_t o) -> uint8_t { return o < raw.size() ? uint8_t(raw[o]) : 0; };
    auto u16 = [&](size_t o) -> uint16_t { return uint16_t(u8(o) | (u8(o + 1) << 8)); };
    if (raw.size() >= 19) {
        p.formatVer = u16(0);
        uint64_t s = 0; for (int i = 0; i < 8; ++i) s |= uint64_t(u8(2 + i)) << (8 * i);
        p.seed = s;
        p.mapType = u8(10);
        p.widthCells = u16(11); p.heightCells = u16(13);
        p.players = u8(15);
        if (p.formatVer >= 2 && raw.size() >= 21) {
            p.treeDensity = u8(16);
            p.rockDensity = u8(17);
            p.metalDensity = u8(18);
            p.waterDensity = u8(19);
            p.reliefDensity = u8(20);
        } else {   // v1 ids: one doodad slider drove both, no relief control
            p.treeDensity = u8(16);
            p.rockDensity = uint8_t(std::min(255, int(u8(16)) * 3 / 4));
            p.metalDensity = u8(17);
            p.waterDensity = u8(18);
            p.reliefDensity = 128;
        }
    }
    return sanitize(p);
}

std::string friendlyLabel(const Params& pin) {
    Params p = sanitize(pin);
    static const char* kNames[kMapTypes] = {"Archipelago", "GreenWorld", "Lava",
                                            "Mars", "Metal", "Moon"};
    int u = p.widthCells / 32, v = p.heightCells / 32;
    // ASCII only: the lobby draws this with the 5x7 block font, which has no glyph
    // for a middot and would render each byte of one as a blank.
    return "Random " + std::to_string(u) + "x" + std::to_string(v) + " " +
           std::to_string(int(p.players)) + "P " + kNames[p.mapType];
}

}  // namespace ta::mapgen
