// Random maps for Total Annihilation data: assembled from the world's terrain
// sections (worlds.hpi sections/<world>/**/*.sct, the retail map editor's prefab
// pieces). TotalA.exe itself has no random maps; this is the engine's feature,
// built only from retail art.
//
// SCT v2 (checked on the shipped sections): u32 version 2, u32 -> minimap,
// u32 tile count, u32 -> tiles (count x 1024), u32 width, u32 height (32px blocks),
// u32 -> data: u16 tile index per block, then 8 bytes per 16px cell (byte 0 the
// height). SCT v3 (worlds.hpi's Metal sections) is the same but for 4 bytes per
// cell: the height, the u16 feature (0xffff none), 0. Both are parsed by
// tnt::Map::loadSct. A section family that shares
// one border height tiles seamlessly.
#include "tnt/mapgen.h"

#include "hpi/hpi.h"
#include "tdf/tdf.h"
#include "util/virtualpath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <random>

namespace ta::mapgen {

namespace {

// Draws that are the same on every standard library: the std distributions are
// implementation-defined, and every peer rolls the map from its recipe.
uint64_t roll(std::mt19937_64& rng, uint64_t n) { return n ? rng() % n : 0; }
int rollIn(std::mt19937_64& rng, int lo, int hi) { return hi < lo ? lo : lo + int(roll(rng, uint64_t(hi - lo + 1))); }
double roll01(std::mt19937_64& rng) { return double(rng() >> 11) * 0x1p-53; }

// A world feature the generator may place (features/<world>/*.tdf).
struct Feat { std::string name; int fx = 1, fz = 1; };

struct Sct {
    int w = 0, h = 0;                 // blocks
    std::vector<uint8_t> tiles;       // count * 1024
    std::vector<uint16_t> tileMap;    // w*h
    std::vector<uint8_t> heights;     // (2w)*(2h)
    int border = -1;                  // the common border height, or -1
    int ground = 0;                   // mean colour index of the corner tile / 16 (ground texture)
};

bool parseSct(const std::vector<uint8_t>& d, Sct& s, const std::vector<uint8_t>& pal) {
    tnt::Map m;
    try { m = tnt::Map::loadSct(d); } catch (const std::exception&) { return false; }   // v2 and v3
    if (m.blocksX > 64 || m.blocksY > 64 || m.tileCount > 4096) return false;
    const uint32_t w = uint32_t(m.blocksX), h = uint32_t(m.blocksY);
    s.w = m.blocksX; s.h = m.blocksY;
    s.tiles = std::move(m.tileGfx);
    s.tileMap = std::move(m.tileIndex);
    s.heights = std::move(m.heights);
    // Border height: the edge's most common height when every edge cell is within
    // 6 of it (sections are authored to join at one level, give or take a step;
    // Lava's land pieces join between 43 and 49).
    const int cw = int(w) * 2, ch = int(h) * 2;
    std::vector<int> edge;
    for (int x = 0; x < cw; ++x) { edge.push_back(s.heights[size_t(x)]); edge.push_back(s.heights[size_t((ch - 1) * cw + x)]); }
    for (int z = 0; z < ch; ++z) { edge.push_back(s.heights[size_t(z * cw)]); edge.push_back(s.heights[size_t(z * cw + cw - 1)]); }
    int counts[256] = {};
    for (int e : edge) ++counts[e];
    const int b = int(std::max_element(counts, counts + 256) - counts);
    bool ok = true;
    for (int e : edge) if (std::abs(e - b) > 6) { ok = false; break; }
    s.border = ok ? b : -1;
    {   // The ground texture the section joins with: its corner tile's mean
        // luminance through the game palette, in steps of 24.
        const size_t t = s.tileMap[0];
        uint32_t sum = 0;
        for (int i = 0; i < 1024; ++i) {
            const size_t c = size_t(s.tiles[t * 1024 + size_t(i)]) * 4;
            sum += pal.size() >= 1024 ? (uint32_t(pal[c]) * 3 + uint32_t(pal[c + 1]) * 6 + uint32_t(pal[c + 2])) / 10
                                      : s.tiles[t * 1024 + size_t(i)];
        }
        s.ground = int(sum / 1024 / 24);
    }
    return true;
}

// The engine's six world slots onto TA's section and feature folders.
// seaShare: with WATER, the sea level's share of the way from the sea floor to the
// land, as the world's shipped maps hold it (Green Planet 75 under land at 84,
// Red Planet about 50 under 85, Archipelago 84 under 86, Lava 23 under 43).
struct TaWorld { const char* sections; const char* features; const char* planet; int sea; int seaShare; };
const TaWorld kTaWorlds[] = {
    {"greenworld", "green", "Green Planet", 0, 89},
    {"mars", "mars", "Red Planet", 0, 62},
    {"archipelago", "archi", "Archipelago", 0, 97},
    {"metal", "metal", "Metal", 0, 85},
    {"moon", "moon", "Moon", 0, 85},
    {"lava", "lava", "Lava", 0, 53},
};

}  // namespace

bool taSectionsAvailable(const hpi::Vfs& vfs) {
    for (const auto& p : vfs.list("sections"))
        if (ta::vpath::extension(p) == ".sct") return true;
    return false;
}

const char* taPlanetFor(uint8_t world) { return kTaWorlds[world % 6].planet; }

std::vector<std::string> assetPaths(const hpi::Vfs& vfs) {
    std::vector<std::string> paths;
    for (const auto& w : kTaWorlds)
        for (const auto& path : vfs.list(std::string("sections/") + w.sections))
            if (ta::vpath::extension(path) == ".sct") paths.push_back(path);
    if (vfs.has("palettes/palette.pal")) paths.push_back("palettes/palette.pal");
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

Result generateTa(const Params& p, const hpi::Vfs& vfs) {
    const TaWorld& W = kTaWorlds[p.mapType % 6];
    std::vector<Sct> all;
    std::vector<uint8_t> pal;
    try { pal = vfs.read("palettes/palette.pal"); } catch (...) {}
    for (const auto& path : vfs.list(std::string("sections/") + W.sections)) {
        if (ta::vpath::extension(path) != ".sct") continue;
        Sct s;
        try { if (parseSct(vfs.read(path), s, pal)) all.push_back(std::move(s)); } catch (...) {}
    }
    // The largest family joining at one height (within 3: Lava's land pieces join at
    // 43 to 45) over one ground texture. Prefer land (a border above 16): the largest
    // such family, else the largest.
    int border = 0;
    std::vector<size_t> pool;
    std::map<int, int> heightsSeen;   // border * 32 + ground -> sections
    for (const auto& c : all) if (c.border >= 0) ++heightsSeen[c.border * 32 + c.ground];
    for (bool landOnly : {true, false}) {
        for (const auto& [key, unused] : heightsSeen) {
            (void)unused;
            const int b = key / 32, g = key % 32;
            if (landOnly && b < 16) continue;
            std::vector<size_t> family;
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i].border >= 0 && all[i].ground == g && std::abs(all[i].border - b) <= 3) family.push_back(i);
            if (family.size() > pool.size()) { pool = std::move(family); border = b; }
        }
        if (!pool.empty()) break;
    }
    if (pool.empty()) throw std::runtime_error(std::string("no tileable TA sections for ") + W.sections);

    Result r;
    tnt::Map& m = r.map;
    m.width = std::max(32, int(p.widthCells) & ~1);
    m.height = std::max(32, int(p.heightCells) & ~1);
    m.blocksX = m.width / 2; m.blocksY = m.height / 2;
    m.seaLevel = W.sea;
    m.heights.assign(size_t(m.width) * m.height, uint8_t(border));
    m.features.assign(size_t(m.width) * m.height, 0xFFFF);
    m.cellExtra.assign(size_t(m.width) * m.height, 0);
    m.tileIndex.assign(size_t(m.blocksX) * m.blocksY, 0);
    std::mt19937_64 rng(p.seed ^ 0x7a9e1d5cULL);

    // Filler: the corner tile of the family's first section (its corner sits at the border height).
    std::map<size_t, uint16_t> baseOf;   // section index -> first tile in m.tileGfx
    auto addSection = [&](size_t si) -> uint16_t {
        auto it = baseOf.find(si);
        if (it != baseOf.end()) return it->second;
        const uint16_t base = uint16_t(m.tileGfx.size() / 1024);
        m.tileGfx.insert(m.tileGfx.end(), all[si].tiles.begin(), all[si].tiles.end());
        baseOf[si] = base;
        return base;
    };
    // Open ground: not one tile over and over but a random mix of the family's flat
    // tiles -- blocks of its sections whose four height samples sit within 2 of the
    // border, so the ground stays level -- from up to 12 sections, no tile twice beside
    // itself. A family with none falls back to its first section's corner tile.
    std::vector<uint16_t> plain;
    {
        std::vector<size_t> order(pool);
        for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[size_t(roll(rng, i))]);
        int sections = 0;
        for (size_t si : order) {
            if (sections >= 12) break;
            const Sct& c = all[si];
            std::vector<int> blocks;
            for (int y = 0; y < c.h; ++y)
                for (int x = 0; x < c.w; ++x) {
                    bool flat = true;
                    for (int dz = 0; dz < 2 && flat; ++dz)
                        for (int dx = 0; dx < 2 && flat; ++dx)
                            flat = std::abs(int(c.heights[size_t(y * 2 + dz) * c.w * 2 + x * 2 + dx]) - border) <= 2;
                    if (flat) blocks.push_back(y * c.w + x);
                }
            if (blocks.size() < 4) continue;
            const uint16_t base = addSection(si);
            for (int b : blocks) plain.push_back(uint16_t(base + c.tileMap[size_t(b)]));
            ++sections;
        }
        std::sort(plain.begin(), plain.end());
        plain.erase(std::unique(plain.begin(), plain.end()), plain.end());
        // Only tiles near the mix's usual brightness: a darker or lighter variant
        // stands out as a square on open ground.
        if (pal.size() >= 1024 && plain.size() > 8) {
            std::vector<std::pair<int, uint16_t>> lum;
            for (uint16_t t : plain) {
                int sum = 0;
                for (int i = 0; i < 1024; ++i) {
                    const uint8_t c = m.tileGfx[size_t(t) * 1024 + size_t(i)];
                    sum += pal[size_t(c) * 4] * 3 + pal[size_t(c) * 4 + 1] * 6 + pal[size_t(c) * 4 + 2];   // RGBA entries
                }
                lum.push_back({sum / 1024, t});
            }
            std::vector<int> v;
            for (const auto& [l, t] : lum) v.push_back(l);
            std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
            const int median = v[v.size() / 2];
            std::vector<uint16_t> kept;
            for (const auto& [l, t] : lum) if (std::abs(l - median) * 100 <= median * 8) kept.push_back(t);
            if (kept.size() >= 4) plain = std::move(kept);
        }
    }
    if (plain.empty()) plain.push_back(uint16_t(addSection(pool[0]) + all[pool[0]].tileMap[0]));
    for (int by = 0; by < m.blocksY; ++by)
        for (int bx = 0; bx < m.blocksX; ++bx) {
            uint16_t t = plain[size_t(roll(rng, plain.size()))];
            for (int tries = 0; tries < 4 && plain.size() > 2; ++tries) {
                const bool left = bx > 0 && m.tileIndex[size_t(by) * m.blocksX + bx - 1] == t;
                const bool up = by > 0 && m.tileIndex[size_t(by - 1) * m.blocksX + bx] == t;
                if (!left && !up) break;
                t = plain[size_t(roll(rng, plain.size()))];
            }
            m.tileIndex[size_t(by) * m.blocksX + bx] = t;
        }
    // Starts: evenly round the map, inset from the edge.
    const int n = std::clamp(int(p.players), 2, kMaxPlayers);
    for (int i = 0; i < n; ++i) {
        const double a = 2.0 * 3.14159265358979323846 * (double(i) + 0.125) / n;
        const int cx = int(m.width / 2 + std::cos(a) * m.width * 0.36);
        const int cz = int(m.height / 2 + std::sin(a) * m.height * 0.36);
        r.starts.push_back({std::clamp(cx, 4, m.width - 5), std::clamp(cz, 4, m.height - 5)});
    }
    std::vector<uint8_t> used(size_t(m.blocksX) * m.blocksY, 0);

    // WATER: the world's coast sections, all 16 x 16 blocks, each edge all land (the
    // land border), all sea (the floor, about 0) or one shore across it -- so each is
    // four corners, land or sea, a marching-squares set. A coarse grid of 16-block
    // cells gets a corner of land or sea from a smoothed random field (the slider
    // sets how much is sea; the start cells and their neighbours stay land), and each
    // cell takes a section with its corners: an all-sea one (or a coast's sea corner
    // tile), a coast, or land for the hills below. A corner pattern the world has no
    // section for is made land. The sea is set 85% of the way up from the floor to the
    // land, about where the shipped maps hold it (Green Planet: 75 under land at 84).
    constexpr int S = 16;
    std::vector<size_t> byPattern[16];   // corners NW 1, NE 2, SW 4, SE 8: set = land
    int seaFloor = 255;
    for (size_t i = 0; i < all.size(); ++i) {
        const Sct& c = all[i];
        if (c.w != S || c.h != S) continue;
        const int cw = c.w * 2, ch = c.h * 2;
        const auto at = [&](int x, int z) { return int(c.heights[size_t(z) * cw + x]); };
        const auto land = [&](int h) { return h >= border - 10; };
        const auto sea = [&](int h) { return h <= 12; };
        const int corner[4] = {at(0, 0), at(cw - 1, 0), at(0, ch - 1), at(cw - 1, ch - 1)};
        int pattern = 0;
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            if (land(corner[k])) pattern |= 1 << k;
            else if (!sea(corner[k])) ok = false;
        }
        // Each edge agrees with its corners: all land, all sea, or a shore.
        const auto edgeOk = [&](int x0, int z0, int dx, int dz, int len, int a, int b) {
            const bool la = pattern >> a & 1, lb = pattern >> b & 1;
            for (int k = 0; k < len; ++k) {
                const int h = at(x0 + dx * k, z0 + dz * k);
                if (la && lb && !land(h)) return false;
                if (!la && !lb && !sea(h)) return false;
            }
            return true;
        };
        ok = ok && edgeOk(0, 0, 1, 0, cw, 0, 1) && edgeOk(0, ch - 1, 1, 0, cw, 2, 3) &&
             edgeOk(0, 0, 0, 1, ch, 0, 2) && edgeOk(cw - 1, 0, 0, 1, ch, 1, 3);
        if (!ok || pattern == 15) continue;
        byPattern[pattern].push_back(i);
        for (auto h : c.heights) seaFloor = std::min(seaFloor, int(h));
    }
    // The sea tile when the world has no all-sea section: a coast's sea corner block.
    int seaTileSection = -1, seaTileBlock = 0;
    for (int pt = 1; pt < 15 && seaTileSection < 0; ++pt)
        for (size_t si : byPattern[pt]) {
            for (int k = 0; k < 4; ++k)
                if (!(pt >> k & 1)) {
                    seaTileSection = int(si);
                    seaTileBlock = (k >= 2 ? (S - 1) * S : 0) + (k & 1 ? S - 1 : 0);
                    break;
                }
            break;
        }
    // The four straight coasts and some sea, with the coasts' land at the height the
    // land sections join at (Metal's coasts stand at 139 under walls at 255: they would
    // meet in cliffs, so Metal stays dry). On Lava the sea is lava (lavaWorld). A
    // corner the set lacks is made land by the pass below.
    bool coasts = seaTileSection >= 0;
    for (int pt : {3, 5, 10, 12}) coasts = coasts && !byPattern[pt].empty();
    if (coasts) {
        const Sct& c = all[byPattern[3].front()];   // north land: its NW corner
        coasts = std::abs(int(c.heights[0]) - border) <= 4;
    }
    const int gx = m.blocksX / S, gz = m.blocksY / S;
    std::vector<uint8_t> landAt(size_t(gx + 1) * (gz + 1), 1);
    if (coasts && p.waterDensity && gx >= 2 && gz >= 2) {
        std::vector<double> field(landAt.size());
        for (auto& v : field) v = roll01(rng);
        for (int pass = 0; pass < 2; ++pass) {   // smoothed, so the sea runs in bodies
            std::vector<double> next(field.size());
            for (int z = 0; z <= gz; ++z)
                for (int x = 0; x <= gx; ++x) {
                    double sum = 0; int cnt = 0;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx, nz = z + dz;
                            if (nx < 0 || nz < 0 || nx > gx || nz > gz) continue;
                            sum += field[size_t(nz) * (gx + 1) + nx]; ++cnt;
                        }
                    next[size_t(z) * (gx + 1) + x] = sum / cnt;
                }
            field = std::move(next);
        }
        std::vector<double> sorted = field;
        std::sort(sorted.begin(), sorted.end());
        const double share = 0.7 * p.waterDensity / 255.0;
        const double cut = sorted[std::min(sorted.size() - 1, size_t(share * double(sorted.size())))];
        for (size_t k = 0; k < field.size(); ++k) landAt[k] = share > 0 && field[k] < cut ? 0 : 1;
        // Every start's cell and its neighbours stay land.
        for (const auto& [sx, sz] : r.starts) {
            const int cx = sx / (S * 2), cz = sz / (S * 2);
            for (int z = cz - 1; z <= cz + 2; ++z)
                for (int x = cx - 1; x <= cx + 2; ++x)
                    if (x >= 0 && z >= 0 && x <= gx && z <= gz) landAt[size_t(z) * (gx + 1) + x] = 1;
        }
        // A pattern with no section (a diagonal, say) is made land, until none is left.
        for (bool changed = true; changed;) {
            changed = false;
            for (int z = 0; z < gz; ++z)
                for (int x = 0; x < gx; ++x) {
                    uint8_t* c[4] = {&landAt[size_t(z) * (gx + 1) + x], &landAt[size_t(z) * (gx + 1) + x + 1],
                                     &landAt[size_t(z + 1) * (gx + 1) + x], &landAt[size_t(z + 1) * (gx + 1) + x + 1]};
                    const int pt = *c[0] | *c[1] << 1 | *c[2] << 2 | *c[3] << 3;
                    if (pt == 15 || pt == 0 || !byPattern[pt].empty()) continue;
                    for (auto* v : c) *v = 1;
                    changed = true;
                }
        }
        bool any = false;
        for (int z = 0; z < gz; ++z)
            for (int x = 0; x < gx; ++x) {
                const int pt = landAt[size_t(z) * (gx + 1) + x] | landAt[size_t(z) * (gx + 1) + x + 1] << 1 |
                               landAt[size_t(z + 1) * (gx + 1) + x] << 2 | landAt[size_t(z + 1) * (gx + 1) + x + 1] << 3;
                if (pt == 15) continue;
                any = true;
                const int bx = x * S, by = z * S;
                if (pt == 0 && byPattern[0].empty()) {   // open sea from a coast's sea tile
                    const uint16_t base = addSection(size_t(seaTileSection));
                    const uint16_t tile = uint16_t(base + all[size_t(seaTileSection)].tileMap[size_t(seaTileBlock)]);
                    for (int yy = 0; yy < S; ++yy)
                        for (int xx = 0; xx < S; ++xx) {
                            used[size_t(by + yy) * m.blocksX + bx + xx] = 1;
                            m.tileIndex[size_t(by + yy) * m.blocksX + bx + xx] = tile;
                        }
                    for (int zz = 0; zz < S * 2; ++zz)
                        for (int xx = 0; xx < S * 2; ++xx)
                            m.heights[size_t(by * 2 + zz) * m.width + bx * 2 + xx] = uint8_t(seaFloor);
                    continue;
                }
                const auto& list = byPattern[pt];
                const size_t si = list[size_t(roll(rng, list.size()))];
                const Sct& c = all[si];
                const uint16_t base = addSection(si);
                for (int yy = 0; yy < S; ++yy)
                    for (int xx = 0; xx < S; ++xx) {
                        used[size_t(by + yy) * m.blocksX + bx + xx] = 1;
                        m.tileIndex[size_t(by + yy) * m.blocksX + bx + xx] = uint16_t(base + c.tileMap[size_t(yy) * S + xx]);
                    }
                for (int zz = 0; zz < S * 2; ++zz)
                    for (int xx = 0; xx < S * 2; ++xx)
                        m.heights[size_t(by * 2 + zz) * m.width + bx * 2 + xx] = c.heights[size_t(zz) * S * 2 + xx];
            }
        if (any) m.seaLevel = uint8_t(seaFloor + (border - seaFloor) * W.seaShare / 100);
    }
    const double density = 0.35 + 0.5 * (p.reliefDensity / 255.0);
    for (int by = 0; by < m.blocksY; ++by)
        for (int bx = 0; bx < m.blocksX; ++bx) {
            if (used[size_t(by) * m.blocksX + bx]) continue;
            if (roll01(rng) > density) continue;
            const size_t si = pool[size_t(roll(rng, pool.size()))];
            const Sct& s = all[si];
            if (bx + s.w > m.blocksX || by + s.h > m.blocksY) continue;
            bool free = true;
            for (int y = 0; y < s.h && free; ++y)
                for (int x = 0; x < s.w && free; ++x) free = !used[size_t(by + y) * m.blocksX + bx + x];
            if (!free) continue;
            const uint16_t base = addSection(si);
            for (int y = 0; y < s.h; ++y)
                for (int x = 0; x < s.w; ++x) {
                    used[size_t(by + y) * m.blocksX + bx + x] = 1;
                    m.tileIndex[size_t(by + y) * m.blocksX + bx + x] = uint16_t(base + s.tileMap[size_t(y) * s.w + x]);
                }
            for (int z = 0; z < s.h * 2; ++z)
                for (int x = 0; x < s.w * 2; ++x)
                    m.heights[size_t(by * 2 + z) * m.width + bx * 2 + x] = s.heights[size_t(z) * s.w * 2 + x];
        }
    m.tileCount = int(m.tileGfx.size() / 1024);
    m.tileKeys.assign(m.tileIndex.begin(), m.tileIndex.end());
    m.tileCols.assign(m.tileIndex.size(), 0);
    m.tileRows.assign(m.tileIndex.size(), 0);

    // The world's features: TREES (category trees), ROCKS (category rocks, reclaimable
    // metal) and metal deposits (metal > 0 and indestructible -- the patches a
    // TotalA.exe map writes its metal from, 0x4220ab). Shipped maps lay their land
    // deposits with the world's metal patches and their sea-floor ones with its aqua
    // ore (the blue crystals), never its glyphs; Metal's own patches are none (its
    // metal is surface metal), so it gets its reclaimable metal features.
    std::vector<Feat> trees, rocks, patches, aqua, salvage;
    for (const auto& path : vfs.list(std::string("features/") + W.features)) {
        if (ta::vpath::extension(path) != ".tdf") continue;
        try {
            auto b = vfs.read(path);
            auto root = tdf::parseText(std::string(b.begin(), b.end()), path);
            for (const auto& name : root.childOrder) {
                const tdf::Node* f = root.child(name);
                if (!f) continue;
                std::string cat = f->valueOr("category", ""), low = name;
                for (char& c : cat) c = char(std::tolower(static_cast<unsigned char>(c)));
                for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
                const Feat ft{name, std::clamp(int(f->numberOr("footprintx", 1)), 1, 8),
                              std::clamp(int(f->numberOr("footprintz", 1)), 1, 8)};
                const bool metal = f->numberOr("metal", 0) > 0, hard = f->numberOr("indestructible", 0) != 0;
                if (metal && hard && low.find("glyph") == std::string::npos)
                    (low.find("aquaore") != std::string::npos ? aqua : patches).push_back(ft);
                else if (metal && !hard && f->numberOr("reclaimable", 0) != 0 && cat.find("rock") != std::string::npos)
                    rocks.push_back(ft);
                else if (!metal && !hard && cat.find("tree") != std::string::npos)
                    trees.push_back(ft);
                else if (metal && f->numberOr("reclaimable", 0) != 0) salvage.push_back(ft);
            }
        } catch (...) {}
    }
    if (patches.empty() && aqua.empty()) patches = salvage;   // Metal

    // One feature to a cell, footprints apart, never in the sea; trees and rocks also
    // keep clear of the starts.
    std::vector<uint8_t> taken(size_t(m.width) * m.height, 0);
    for (const auto& [sx, sz] : r.starts)
        for (int z = sz - 6; z <= sz + 6; ++z)
            for (int x = sx - 6; x <= sx + 6; ++x)
                if (x >= 0 && z >= 0 && x < m.width && z < m.height) taken[size_t(z) * m.width + x] = 2;   // start ground
    // A deposit must take an extractor: its 3 x 3 footprint centred on the deposit,
    // with the one-cell border retail grades round a site (0x508cd0), within the
    // extractor's MaxSlope (high - low, 0x508bc1): on land all above the sea and
    // within 10 (armmex, cormex: MaxSlope 10, MaxWaterDepth 0); on the sea floor
    // at least 19 under it and within 20 (armuwmex MinWaterDepth 19, MaxSlope 24;
    // coruwmex 10, 20).
    auto buildable = [&](const Feat& f, int cx, int cz, bool sea) {
        const int mx = cx + f.fx / 2, mz = cz + f.fz / 2;
        if (mx < 3 || mz < 3 || mx + 3 >= m.width || mz + 3 >= m.height) return false;
        int lo = 255, hi = 0;
        for (int z = mz - 2; z <= mz + 2; ++z)
            for (int x = mx - 2; x <= mx + 2; ++x) {
                const int h = m.heights[size_t(z) * m.width + x];
                lo = std::min(lo, h); hi = std::max(hi, h);
                if (taken[size_t(z) * m.width + x] == 1) return false;   // a tree or a rock in the way
            }
        return sea ? hi + 19 <= int(m.seaLevel) && hi - lo <= 20 : lo > int(m.seaLevel) && hi - lo <= 10;
    };
    auto place = [&](const Feat& f, int cx, int cz, bool atStart, bool sea = false) {
        if (cx < 2 || cz < 2 || cx + f.fx + 2 >= m.width || cz + f.fz + 2 >= m.height) return false;
        for (int z = cz; z < cz + f.fz; ++z)
            for (int x = cx; x < cx + f.fx; ++x) {
                const uint8_t t = taken[size_t(z) * m.width + x];
                if (t == 1 || (t == 2 && !atStart)) return false;
                if (!sea && m.heights[size_t(z) * m.width + x] <= m.seaLevel) return false;
            }
        for (int z = cz; z < cz + f.fz; ++z)
            for (int x = cx; x < cx + f.fx; ++x) taken[size_t(z) * m.width + x] = 1;
        auto it = std::find(m.featureNames.begin(), m.featureNames.end(), f.name);
        uint16_t idx;
        if (it == m.featureNames.end()) { idx = uint16_t(m.featureNames.size()); m.featureNames.push_back(f.name); }
        else idx = uint16_t(it - m.featureNames.begin());
        m.features[size_t(cz) * m.width + cx] = idx;
        return true;
    };
    const double area = double(m.width) * m.height;
    // METAL: a cluster by every start, and clusters over the land and the sea floor,
    // each a few deposits apart: patches on land, aqua ore under the sea.
    if (!patches.empty() || !aqua.empty()) {
        auto deposit = [&](bool sea) -> const Feat& {
            if (sea || patches.empty()) return aqua[size_t(roll(rng, aqua.size()))];
            return patches[size_t(roll(rng, patches.size()))];
        };
        // Deposits keep 6 cells between their corners, so each takes an extractor of
        // its own with room round it, as on the shipped maps.
        std::vector<std::pair<int, int>> deposits;
        auto spaced = [&](int x, int z) {
            for (const auto& [dx, dz] : deposits)
                if (std::abs(dx - x) < 6 && std::abs(dz - z) < 6) return false;
            return true;
        };
        auto cluster = [&](int cx, int cz, int count, bool atStart, bool sea) {
            if (sea && aqua.empty()) return;
            for (int k = 0, tries = 0; k < count && tries < count * 30; ++tries) {
                const Feat& f = deposit(sea);
                const int x = cx + rollIn(rng, -7, 7), z = cz + rollIn(rng, -7, 7);
                if (!spaced(x, z) || !buildable(f, x, z, sea) || !place(f, x, z, atStart, sea)) continue;
                deposits.push_back({x, z});
                // The extractor's ground (and its border) stays clear of trees and rocks.
                const int mx = x + f.fx / 2, mz = z + f.fz / 2;
                for (int zz = mz - 2; zz <= mz + 2; ++zz)
                    for (int xx = mx - 2; xx <= mx + 2; ++xx) taken[size_t(zz) * m.width + xx] = 1;
                ++k;
            }
        };
        const int perStart = 2 + int(p.manaDensity) * 4 / 255;   // 2..6
        for (const auto& [sx, sz] : r.starts) {
            const double a = roll01(rng) * 6.283185307179586;
            cluster(sx + int(std::cos(a) * 9), sz + int(std::sin(a) * 9), perStart, true, false);
        }
        // Each site lands on the land or the sea floor as it falls (a lava sea has
        // no ore: nothing is built in it).
        const bool seaOre = !aqua.empty() && m.seaLevel > 0 && !lavaWorld(p.mapType);
        const int sites = int(area / 8192.0 * (0.5 + 2.5 * p.manaDensity / 255.0));
        for (int k = 0; k < sites; ++k) {
            const int cx = rollIn(rng, 6, m.width - 10), cz = rollIn(rng, 6, m.height - 10);
            const bool sea = m.heights[size_t(cz) * m.width + cx] <= m.seaLevel;
            if (sea && !seaOre) continue;
            cluster(cx, cz, rollIn(rng, 1, 3), false, sea);
        }
    }
    // TREES: woods, each a clump round a centre.
    if (!trees.empty() && p.treeDensity) {
        const int woods = int(area / 2048.0 * p.treeDensity / 255.0);
        for (int k = 0; k < woods; ++k) {
            const int cx = rollIn(rng, 4, m.width - 6), cz = rollIn(rng, 4, m.height - 6);
            const int n = rollIn(rng, 6, 24), radius = rollIn(rng, 3, 8);
            for (int j = 0; j < n; ++j)
                place(trees[size_t(roll(rng, trees.size()))], cx + rollIn(rng, -radius, radius),
                      cz + rollIn(rng, -radius, radius), false);
        }
    }
    // ROCKS: scattered one by one.
    if (!rocks.empty() && p.rockDensity) {
        const int count = int(area / 1024.0 * p.rockDensity / 255.0);
        for (int k = 0; k < count; ++k)
            place(rocks[size_t(roll(rng, rocks.size()))], rollIn(rng, 4, m.width - 8), rollIn(rng, 4, m.height - 8), false);
    }
    return r;
}

}  // namespace ta::mapgen
