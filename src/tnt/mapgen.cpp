#include "tnt/mapgen.h"

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

// Per-world ground + sea terrain section JPG keys (terrain/<key:08x>.jpg), verified
// present in terrain.hpi, plus each section's VALID wallpaper period in 32px tiles.
// Ground fills are seamless across their whole grid (16x16; Zhon's is 256px = 8x8).
// The sea JPGs are 512px but only PARTLY art: Aramon/Taros/Zhon carry a 5x5 block
// of flat dark-teal water in the top-left corner and pure-black padding elsewhere
// (only Veruna's dgsea fills all 16x16) -- wallpapering them %16 sampled ~90%
// padding, which is what made generated water look black.
struct WorldArt { uint32_t ground, sea; uint8_t groundK, seaK; };
constexpr std::array<WorldArt, kMapTypes> kWorldArt = {{
    {0x868a8222u, 0x0c2e64b2u, 16, 5},    // Aramon: grass / asea
    {0xca4200f8u, 0xb8524a38u, 16, 5},    // Taros:  low_ground / taros_sea
    // Veruna: sandy-cay land (a0863a34, dunes/sand -- what the Coast Sandy kit's
    // land edges blend into, Athri Cay style) over dgsea (full-art teal sea).
    {0xa0863a34u, 0x9a5c5436u, 16, 16},
    {0x9bb50b09u, 0x59f53dfdu, 8, 5},     // Zhon:   jungletile80 (256px) / H2OTILE
    // Creon (Iron Plague, IPData.hpi): green turf fill mined from the takx19-26
    // mission maps (their other big fill, 93c585b7, is city cobblestone). Every
    // shipped Creon map is DRY -- IP never used a Creon deep-sea fill -- so water
    // borrows Veruna's dgsea (full 16x16 teal). CreWave01-16 exist for shores.
    {0x814dddc1u, 0x9a5c5436u, 16, 16},   // Creon:  turf / (Veruna sea stand-in)
}};

// ---- coast prefab kits ---------------------------------------------------------
// Retail coastlines are whole hand-painted 512px prefab SECTIONS (plain TNT files
// in sections.hpi / IPSections.hpi: one terrain key, an identity col/row tile
// grid, an authored bank heightfield, sometimes beach features). The generator
// lays its coastline out on the section grid and stamps these whole -- that is
// where retail's big sweeping curves come from. Role order below: straights with
// water N/S/E/W of the piece; outer bends (water wraps NW/NE/SE/SW around a land
// tip); inner pockets (water only in the NW/NE/SW/SE corner).
enum ShoreRole { kRN, kRS, kRE, kRW, kONW, kONE, kOSE, kOSW, kINW, kINE, kISW, kISE, kRoles };
struct ShoreKit {
    const char* dir;
    const char* name[kRoles];
    const char* var[3];   // interchangeable cosmetic variants, picked by position
    const char* ext;
};
constexpr std::array<ShoreKit, kMapTypes> kShoreKit = {{
    {"Sections/Aramon/Coast Sandy/",
     {"ascoast01", "ascoast02", "ascoast03", "ascoast04", "ascoast05", "ascoast06",
      "ascoast07", "ascoast08", "ascoast12", "ascoast11", "ascoast09", "ascoast10"},
     {"a_80", "b_80", "c_80"}, ".TNT"},
    {"Sections/Taros/Coast Sandy/",
     {"n", "s", "e", "w", "nw", "ne", "se", "sw", "nwe", "nee", "swe", "see"},
     {"01", "02", "03"}, ".TNT"},
    {"Sections/Veruna/Coast Sandy/",
     {"n", "s", "e", "w", "nw", "ne", "se", "sw", "n_w", "e_n", "s_w", "e_s"},
     {"01", "02", "03"}, ".TNT"},
    // Zhon uses the JUNGLE palette's coast kit so the land edges blend into the
    // jungletile ground fill (plain "Coast Sandy" is for its sand-flat palette).
    {"Sections/Zhon/Coast Sandy Jungle/",
     {"n", "s", "e", "w", "1nw", "1ne", "1se", "1sw", "2nw", "2ne", "2sw", "2se"},
     {"1", "2", "3"}, ".TNT"},
    {"sections/creon/shores/",
     {"n", "s", "e", "w", "nw", "ne", "se", "sw", "n_w", "e_n", "s_w", "e_s"},
     {"01", "02", "03"}, ".tnt"},
}};

// Marching case (bit0 NW, bit1 NE, bit2 SW, bit3 SE wet) -> kit role, -1 = none.
constexpr int kCaseRole[16] = {
    -1,     // 0  all dry
    kINW,   // 1  water pocket NW
    kINE,   // 2  water pocket NE
    kRN,    // 3  water north
    kISW,   // 4  water pocket SW
    kRW,    // 5  water west
    -1,     // 6  diagonal pinch (cleaned away)
    kONW,   // 7  only SE dry: water wraps NW
    kISE,   // 8  water pocket SE
    -1,     // 9  diagonal pinch (cleaned away)
    kRE,    // 10 water east
    kONE,   // 11 only SW dry: water wraps NE
    kRS,    // 12 water south
    kOSW,   // 13 only NE dry: water wraps SW
    kOSE,   // 14 only NW dry: water wraps SE
    -1,     // 15 all wet
};

// Retail authoring levels the coast prefabs assume: {seaLevel, flat land level}.
// Aramon + Creon kits are authored at land 80 (river-map style, sea 40); the
// three sea worlds at land 62 (sea 58). Measured from the shipped prefabs/maps.
struct WorldLevels { uint8_t sea, land; };
constexpr std::array<WorldLevels, kMapTypes> kLevels = {{
    {40, 80}, {58, 62}, {58, 62}, {58, 62}, {40, 80},
}};

// Per-world doodad + mana palette (names verified in features/<world>/*.tdf).
// Trees (category=trees) and rocks (category=rocks) are reclaimable obstacles --
// we vary across ALL the art variants so no one type repeats. A mana deposit is a
// glowing Sacred Stone centre (XxxManaNN, category=Mana + animating=1 -- the
// buildable spot the sim harvests) ringed by static Standing Stones (XxxHengeNN,
// "ruins"), exactly as the shipped maps lay them out. Rocks are ordered small->big
// so placement can bias toward the little ones. The sim skips names it can't
// resolve, so an over-long list is harmless.
struct Span { const char* const* p; int n; };
struct WorldFeatures {
    Span trees, rocks;
    std::array<const char*, 3> sacred;   // weak, medium, strong (sacredsite 1.0/1.5/2.0)
};

constexpr const char* kAraTree[] = {"AraTree01", "AraTree02", "AraTree03", "AraTree04", "AraTree05",
                                    "AraTree06", "AraTree07", "AraTree08", "AraTree09", "AraTree10"};
constexpr const char* kAraRock[] = {"AraRock01", "AraRock02", "AraRock03", "AraRock04",
                                    "AraRock05", "AraRock06", "AraRock07"};

constexpr const char* kTarTree[] = {"TarTree01", "TarTree02", "TarTree03", "TarTree04", "TarTree05",
                                    "TarTree06", "TarTree07", "TarTree08", "TarTree09"};
constexpr const char* kTarRock[] = {"TarRock07", "TarRock06", "TarRock05", "TarRock04",
                                    "TarRock03", "TarRock02", "TarRock01"};   // small->big

constexpr const char* kVerTree[] = {"VerTree01", "VerTree02", "VerTree03", "VerTree04", "VerTree05",
                                    "VerTree06", "VerTree07", "VerTree08", "VerTree09"};
constexpr const char* kVerRock[] = {"VeRock01", "VeRock02", "VeRock05", "VeRock07",
                                    "VeRock04", "VeRock03", "VeRock06"};   // small->big

constexpr const char* kZonTree[] = {"ZonTree01", "ZonTree02", "ZonTree03",
                                    "ZonTree04", "ZonTree05", "ZonTree06"};
constexpr const char* kZonRock[] = {"ZonRock07", "ZonRock06", "ZonRock05", "ZonRock04",
                                    "ZonRock03", "ZonRock02", "ZonRock01"};   // small->big

// Creon (Iron Plague): features/creon/*.tdf in IPData.hpi.
constexpr const char* kCreTree[] = {"CreTree01", "CreTree02", "CreTree03", "CreTree04", "CreTree05",
                                    "CreTree06", "CreTree07", "CreTree08", "CreTree09"};
constexpr const char* kCreRock[] = {"CRERock12", "CRERock07", "CRERock09", "CRERock10", "CRERock11",
                                    "CRERock02", "CRERock03", "CRERock04", "CRERock06", "CRERock05",
                                    "CRERock13", "CRERock01", "CRERock08"};   // small->big

constexpr std::array<WorldFeatures, kMapTypes> kWorldFeat = {{
    {{kAraTree, 10}, {kAraRock, 7},  {{"AraMana01", "AraMana02", "AraMana03"}}},
    {{kTarTree, 9},  {kTarRock, 7},  {{"TarMana01", "TarMana02", "TarMana03"}}},
    {{kVerTree, 9},  {kVerRock, 7},  {{"VerMana01", "VerMana02", "VerMana03"}}},
    {{kZonTree, 6},  {kZonRock, 7},  {{"ZonMana01", "ZonMana02", "ZonMana03"}}},
    {{kCreTree, 9},  {kCreRock, 13},  {{"CReMana01", "CREMana02", "CREMana03"}}},
}};

// ---- mana-deposit henge rings --------------------------------------------------
// Mined from ALL 101 shipped maps (1207 deposits, 3230 henges): each henge
// variant is an ARC SEGMENT authored for ONE compass slot at a canonical anchor
// offset from the sacred stone (3-6 cells -- tighter than our old 5-8 scatter).
// A retail deposit is ONE sacred stone + 2-4 henges from DISTINCT slots
// (canonical trio NW+NE+S where the world has it), each at its variant's
// canonical offset with +-1 cell of jitter (+-2 on Zhon, the loosest world).
// Weights are the shipped occurrence counts.
enum RingSlot { rN, rNE, rE, rSE, rS, rSW, rW, rNW };
struct RingPiece { const char* name; int8_t dx, dz; uint8_t slot; uint8_t w; };
struct RingSpec {
    const RingPiece* p; int n;
    uint8_t cnt[6];      // cumulative % thresholds: ring size k when roll < cnt[k]
    int8_t jitter;       // +- cells around the canonical offset
    uint8_t order[8];    // slot preference order (canonical trio first)
};
constexpr RingPiece kAraRing[] = {
    {"AraHenge01", -4, -4, rNW, 95}, {"AraHenge08", -5, -4, rNW, 47},
    {"AraHenge09", +3, -3, rNE, 89}, {"AraHenge02", +3, -3, rNE, 61},
    {"AraHenge05", +3, -4, rNE, 19}, {"AraHenge03", +4, -1, rE, 17},
    {"AraHenge07", -1, +6, rS, 117}, {"AraHenge04",  0, +5, rS, 56},
    {"AraHenge06", -5, -1, rW, 49},
};
constexpr RingPiece kTarRing[] = {
    {"TarHenge09", -4, -3, rNW, 135}, {"TarHenge01", -4, -4, rNW, 25},
    {"TarHenge02", -1, -4, rN, 47},   {"TarHenge10", +1, -5, rN, 38},
    {"TarHenge11", +4, -3, rNE, 137}, {"TarHenge04", +3, -3, rNE, 33},
    {"TarHenge05", +4,  0, rE, 59},   {"TarHenge06", +3, +3, rSE, 38},
    {"TarHenge12", +3, +3, rSE, 19},  {"TarHenge13",  0, +5, rS, 156},
    {"TarHenge14", -4, +3, rSW, 62},  {"TarHenge08", -4,  0, rW, 28},
    {"TarHenge03", -5,  0, rW, 15},   {"TarHenge07", -7, +2, rW, 11},
};
constexpr RingPiece kVerRing[] = {
    {"VerHenge10", -4, -3, rNW, 68},  {"VerHenge01b", -5, -4, rNW, 38},
    {"VerHenge01", -6, -4, rNW, 26},  {"VerHenge02", -1, -5, rN, 36},
    {"VerHenge11", +3, -2, rNE, 72},  {"VerHenge04", +3, -3, rNE, 24},
    {"VerHenge05b", +4, 0, rE, 31},   {"VerHenge05", +4,  0, rE, 24},
    {"VerHenge09", -1, +4, rS, 102},  {"VerHenge07",  0, +4, rS, 19},
    {"VerHenge08", -4, +2, rSW, 54},
};
constexpr RingPiece kZonRing[] = {
    {"ZonHenge02", -3, -5, rNW, 46},  {"ZonHenge01", -4, -4, rNW, 25},
    {"ZonHenge10", -4, -3, rNW, 16},  {"ZonHenge06",  0, -5, rN, 41},
    {"ZonHenge03", +2, -5, rN, 36},   {"ZonHenge07", +3, -4, rNE, 34},
    {"ZonHenge04", +5,  0, rE, 31},   {"ZonHenge05", +3, +2, rSE, 48},
    {"ZonHenge09", +1, +4, rS, 49},   {"ZonHenge11", -6, +4, rSW, 41},
    {"ZonHenge08", -5, +1, rW, 40},
};
constexpr RingPiece kCreRing[] = {
    {"CREHenge17", -1, -5, rN, 18},   {"CREHenge16", -3, -4, rN, 9},
    {"CREHenge22", +3, -4, rNE, 16},  {"CREHenge21", +4, -2, rNE, 9},
    {"CREHenge23", +4, -3, rNE, 8},   {"CREHenge09", +4, +1, rE, 7},
    {"CREHenge14",  0, +5, rS, 11},   {"CREHenge07",  0, +4, rS, 7},
    {"CREHenge15", -3, +4, rSW, 21},  {"CREHenge05", -3, +3, rSW, 10},
    {"CREHenge19", -5, +1, rW, 32},   {"CREHenge11", -6, -1, rW, 7},
    {"CREHenge06", -3, -5, rNW, 7},   {"CREHenge03", -5, -4, rNW, 6},
};
constexpr std::array<RingSpec, kMapTypes> kRing = {{
    // cnt: shipped ring-size distribution (cumulative %); order: canonical first.
    {kAraRing, 9,  {0, 0, 30, 93, 100, 100}, 1, {rNW, rNE, rS, rW, rE, rSW, rN, rSE}},
    {kTarRing, 14, {0, 0, 23, 67, 88, 100},  1, {rNW, rNE, rS, rSW, rE, rN, rW, rSE}},
    {kVerRing, 11, {0, 19, 58, 94, 99, 100}, 1, {rNW, rNE, rS, rSW, rN, rE, rW, rSE}},
    {kZonRing, 11, {0, 5, 24, 66, 91, 100},  2, {rNW, rSE, rN, rSW, rW, rNE, rE, rS}},
    // 21% of shipped Creon deposits are BARE (no ring at all) -- reproduced.
    {kCreRing, 14, {21, 27, 71, 96, 100, 100}, 1, {rW, rN, rNE, rSW, rS, rE, rNW, rSE}},
}};

// Even compass directions (integer, scaled by 1000) for start-position rings.
constexpr std::array<std::pair<int, int>, 8> kCompass = {{
    {0, -1000}, {707, -707}, {1000, 0}, {707, 707},
    {0, 1000}, {-707, 707}, {-1000, 0}, {-707, -707},
}};

}  // namespace

Params sanitize(Params p) {
    if (p.mapType >= kMapTypes) p.mapType = Aramon;
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

    // ---- terrain tiles: fills, then whole coast prefabs -----------------------------
    // Land/water sections WALLPAPER the world's ground/sea fill (col=bx%K,row=by%K;
    // K = the fill's VALID art region, see kWorldArt). Shoreline sections are then
    // stamped whole from the world's Coast Sandy prefab kit: tiles, the authored
    // bank HEIGHTS (overwriting the placeholder -- this is what puts the sim
    // waterline exactly on the painted shoreline), and any beach features the
    // prefab carries.
    const WorldArt art = kWorldArt[p.mapType];
    const int kGround = art.groundK, kSea = art.seaK;
    size_t blocks = size_t(m.blocksX) * m.blocksY;
    m.tileKeys.resize(blocks);
    m.tileCols.resize(blocks);
    m.tileRows.resize(blocks);
    for (int by = 0; by < m.blocksY; ++by)
        for (int bx = 0; bx < m.blocksX; ++bx) {
            size_t i = size_t(by) * m.blocksX + bx;
            int c = scase(bx >> 4, by >> 4);
            bool water = (c == 15);   // shore blocks get stamped; fill land-ish meanwhile
            m.tileKeys[i] = water ? art.sea : art.ground;
            int K = water ? kSea : kGround;
            m.tileCols[i] = uint8_t(bx % K);
            m.tileRows[i] = uint8_t(by % K);
        }
    m.features.assign(size_t(W) * H, 0xFFFF);

    // Prefab loader: cached per path, tolerant of a missing variant (falls back to
    // the next; a fully missing piece leaves the fill -- same files on every peer,
    // so the fallback is byte-identical too).
    const ShoreKit& kit = kShoreKit[p.mapType];
    std::map<std::string, std::unique_ptr<ta::tnt::Map>> pieceCache;
    auto loadPiece = [&](int role, uint64_t vh) -> const ta::tnt::Map* {
        for (int attempt = 0; attempt < 3; ++attempt) {
            int v = int((vh + uint64_t(attempt)) % 3);
            std::string path = std::string(kit.dir) + kit.name[role] + kit.var[v] + kit.ext;
            auto it = pieceCache.find(path);
            if (it == pieceCache.end()) {
                std::unique_ptr<ta::tnt::Map> pm;
                try {
                    auto d = vfs.read(path);
                    auto loaded = ta::tnt::Map::load(d, path);
                    if (loaded.width == 32 && loaded.height == 32)
                        pm = std::make_unique<ta::tnt::Map>(std::move(loaded));
                } catch (const std::exception&) {}
                it = pieceCache.emplace(std::move(path), std::move(pm)).first;
            }
            if (it->second) return it->second.get();
        }
        return nullptr;
    };
    struct PendingFeat { int cx, cz; std::string name; };
    std::vector<PendingFeat> prefabFeats;
    for (int sy = 0; sy < SH; ++sy)
        for (int sx = 0; sx < SW; ++sx) {
            int role = kCaseRole[scase(sx, sy)];
            if (role < 0) continue;
            uint64_t vh = p.seed ^ (uint64_t(sy) * 1000003u + uint64_t(sx) * 7919u);
            const ta::tnt::Map* pc = loadPiece(role, splitmix(vh));   // vh is advanced in place
            if (!pc) continue;
            // heights + features: 32x32 cells at (sx*32, sy*32)
            for (int z = 0; z < 32; ++z)
                for (int x = 0; x < 32; ++x) {
                    int cx = sx * 32 + x, cz = sy * 32 + z;
                    m.heights[size_t(cz) * W + cx] = pc->heights[size_t(z) * 32 + x];
                    uint16_t fi = pc->features[size_t(z) * 32 + x];
                    if (fi != 0xFFFF && fi < pc->featureNames.size())
                        prefabFeats.push_back({cx, cz, pc->featureNames[fi]});
                }
            // tiles: 16x16 blocks at (sx*16, sy*16)
            for (int bz = 0; bz < 16; ++bz)
                for (int bxl = 0; bxl < 16; ++bxl) {
                    size_t bi = size_t(sy * 16 + bz) * m.blocksX + (sx * 16 + bxl);
                    size_t pi = size_t(bz) * 16 + bxl;
                    m.tileKeys[bi] = pc->tileKeys[pi];
                    m.tileCols[bi] = pc->tileCols[pi];
                    m.tileRows[bi] = pc->tileRows[pi];
                }
        }

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
    const WorldFeatures& wf = kWorldFeat[p.mapType];
    m.featureNames.clear();
    auto featIdx = [&](const std::string& nm) -> uint16_t {
        for (size_t i = 0; i < m.featureNames.size(); ++i)
            if (m.featureNames[i] == nm) return uint16_t(i);
        m.featureNames.emplace_back(nm);
        return uint16_t(m.featureNames.size() - 1);
    };
    // The stamped coast prefabs' own features (beach rocks etc.) go in first, with
    // their names remapped into this map's table; their cells are claimed below so
    // the procedural scatter avoids them.
    for (const auto& pf : prefabFeats)
        m.features[size_t(pf.cz) * W + pf.cx] = featIdx(pf.name);
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
    for (const auto& pf : prefabFeats)   // prefab features block the scatter
        claim[size_t(pf.cz) * W + pf.cx] = 1;
    auto fits = [&](int cx, int cz, int fx, int fz) {
        for (int dz = 0; dz < fz; ++dz)
            for (int dx = 0; dx < fx; ++dx) {
                int nx = cx + dx - fx / 2, nz = cz + dz - fz / 2;
                if (!isLand(nx, nz) || claim[size_t(nz) * W + nx]) return false;
            }
        return true;
    };
    auto place = [&](int cx, int cz, const char* nm, int fx, int fz) {
        m.features[size_t(cz) * W + cx] = featIdx(nm);
        for (int dz = 0; dz < fz; ++dz)
            for (int dx = 0; dx < fx; ++dx) {
                int nx = cx + dx - fx / 2, nz = cz + dz - fz / 2;
                if (nx >= 0 && nz >= 0 && nx < W && nz < H) claim[size_t(nz) * W + nx] = 1;
            }
    };
    uint64_t frng = p.seed ^ 0x5eed1234abcdULL;

    // One mana deposit: a Sacred Stone centre (weak most common) ringed by 2..5
    // Standing Stones -- the "mana ruins" the shipped maps cluster around each spot.
    std::vector<std::pair<int, int>> depots;
    auto okDepot = [&](int cx, int cz, int minSp) {
        for (auto& [dx0, dz0] : depots) {
            int dx = cx - dx0, dz = cz - dz0;
            if (dx * dx + dz * dz < minSp * minSp) return false;
        }
        return true;
    };
    auto placeDeposit = [&](int cx, int cz) {
        // Sacred stone tier at the shipped 01:02:03 mix (223:446:537 of 1206).
        uint32_t sr = uint32_t(splitmix(frng) % 1206);
        int tier = sr < 223 ? 0 : (sr < 669 ? 1 : 2);
        place(cx, cz, wf.sacred[size_t(tier)], 2, 2);
        depots.push_back({cx, cz});
        // Ring the stone the retail way (see kRing): draw the ring size from the
        // world's shipped distribution, walk the slot-preference order (skipping
        // a slot ~12% of the time for variety when spares remain), and place each
        // slot's arc-segment variant at its canonical offset + jitter. A henge
        // whose spot doesn't fit is simply omitted, as retail maps do.
        const RingSpec& rs = kRing[p.mapType];
        int roll = int(splitmix(frng) % 100), want = 5;
        for (int k = 0; k < 6; ++k) if (roll < rs.cnt[k]) { want = k; break; }
        uint64_t skipBits = splitmix(frng);
        int placedH = 0, remaining = 0;
        bool slotHas[8] = {};
        for (int i = 0; i < rs.n; ++i) slotHas[rs.p[i].slot] = true;
        for (int oi = 0; oi < 8; ++oi) if (slotHas[rs.order[oi]]) ++remaining;
        for (int oi = 0; oi < 8 && placedH < want; ++oi) {
            int slot = rs.order[oi];
            if (!slotHas[slot]) continue;
            --remaining;
            if (remaining >= want - placedH && ((skipBits >> oi) & 7) == 0)
                continue;   // variety: occasionally pass over a canonical slot
            int total = 0;
            for (int i = 0; i < rs.n; ++i) if (rs.p[i].slot == slot) total += rs.p[i].w;
            int pickW = int(splitmix(frng) % uint64_t(total));
            const RingPiece* pc = nullptr;
            for (int i = 0; i < rs.n; ++i)
                if (rs.p[i].slot == slot) { if ((pickW -= rs.p[i].w) < 0) { pc = &rs.p[i]; break; } }
            int jr = rs.jitter;
            int hx = cx + pc->dx + int(splitmix(frng) % uint64_t(2 * jr + 1)) - jr;
            int hz = cz + pc->dz + int(splitmix(frng) % uint64_t(2 * jr + 1)) - jr;
            ++placedH;   // the slot is consumed even if the piece doesn't fit
            if (fits(hx, hz, 3, 3)) place(hx, hz, pc->name, 3, 3);
        }
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
            if (!isLand(cx, cz) || !fits(cx, cz, 2, 2) || !okDepot(cx, cz, 18)) continue;
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
    int scatter = std::clamp(area / std::max(1, 22000 - int(p.manaDensity) * 70), 0, 48);
    for (int placed = 0, tries = 0; placed < scatter && tries < scatter * 200 + 400; ++tries) {
        int cx = int(splitmix(frng) % uint64_t(W)), cz = int(splitmix(frng) % uint64_t(H));
        if (cx < 3 || cz < 3 || cx >= W - 3 || cz >= H - 3) continue;
        if (!isLand(cx, cz) || nearStart(cx, cz, 14) || !fits(cx, cz, 2, 2) || !okDepot(cx, cz, 22))
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
            if (int(roll % 10000) < treeThresh) {          // tree (any variant)
                const char* nm = wf.trees.p[(roll >> 24) % uint64_t(wf.trees.n)];
                if (fits(cx, cz, 2, 2)) place(cx, cz, nm, 2, 2);
            } else if (int((roll >> 13) % 10000) < rockThresh) {   // rock (small-biased)
                int a = int((roll >> 24) % uint64_t(wf.rocks.n));
                int b = int((roll >> 33) % uint64_t(wf.rocks.n));
                if (fits(cx, cz, 3, 3)) place(cx, cz, wf.rocks.p[std::min(a, b)], 3, 3);
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
    b.push_back(char(p.manaDensity));
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
            p.manaDensity = u8(18);
            p.waterDensity = u8(19);
            p.reliefDensity = u8(20);
        } else {   // v1 ids: one doodad slider drove both, no relief control
            p.treeDensity = u8(16);
            p.rockDensity = uint8_t(std::min(255, int(u8(16)) * 3 / 4));
            p.manaDensity = u8(17);
            p.waterDensity = u8(18);
            p.reliefDensity = 128;
        }
    }
    return sanitize(p);
}

std::string friendlyLabel(const Params& pin) {
    Params p = sanitize(pin);
    static const char* kNames[kMapTypes] = {"Aramon", "Taros", "Veruna", "Zhon", "Creon"};
    int u = p.widthCells / 32, v = p.heightCells / 32;
    // ASCII only: the lobby draws this with the 5x7 block font, which has no glyph
    // for a middot and would render each byte of one as a blank.
    return "Random " + std::to_string(u) + "x" + std::to_string(v) + " " +
           std::to_string(int(p.players)) + "P " + kNames[p.mapType];
}

}  // namespace ta::mapgen
