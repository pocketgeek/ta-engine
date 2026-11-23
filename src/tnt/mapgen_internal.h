#pragma once

// Shared deterministic palette and noise helpers for the versioned generators.
#include "tnt/mapgen.h"
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tak::hpi { class Vfs; }

namespace tak::mapgen::detail {


// ---- deterministic integer helpers (no float -> identical on every peer) -------

inline uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// Hashed lattice value 0..255 at integer grid point (x,y) for a given seed.
inline uint32_t latticeVal(uint64_t seed, int x, int y) {
    uint64_t h = seed ^ 0x100000001b3ULL;
    h = (h ^ uint64_t(uint32_t(x))) * 0x9e3779b97f4a7c15ULL;
    h = (h ^ uint64_t(uint32_t(y))) * 0xc2b2ae3d27d4eb4fULL;
    h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ULL; h ^= h >> 32;
    return uint32_t(h & 0xff);
}

// Bilinear value noise 0..255 at cell (cx,cz), lattice spacing L (>0).
inline uint32_t noise(uint64_t seed, int cx, int cz, int L) {
    int gx = (cx >= 0 ? cx : cx - L + 1) / L, gz = (cz >= 0 ? cz : cz - L + 1) / L;
    int fx = cx - gx * L, fz = cz - gz * L;                 // 0..L-1
    uint32_t v00 = latticeVal(seed, gx, gz),     v10 = latticeVal(seed, gx + 1, gz);
    uint32_t v01 = latticeVal(seed, gx, gz + 1), v11 = latticeVal(seed, gx + 1, gz + 1);
    uint32_t a = (v00 * uint32_t(L - fx) + v10 * uint32_t(fx)) / uint32_t(L);
    uint32_t b = (v01 * uint32_t(L - fx) + v11 * uint32_t(fx)) / uint32_t(L);
    return (a * uint32_t(L - fz) + b * uint32_t(fz)) / uint32_t(L);
}

// Two-octave fractal noise 0..255 (large land masses + medium detail).
inline uint32_t fractal(uint64_t seed, int cx, int cz, int span) {
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
// fills: whole 512px ground sections laid over the fill, one picked per dry section
// for variety (the path up to the variant number, "1".."fillCount").
struct WorldArt { uint32_t ground, sea; uint8_t groundK, seaK; const char* fills = nullptr; int fillCount = 0; };
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
    // Volcano (IPSections, sections/volcano/flats): base01's ash plateau / lava_512.
    {0xdb07f175u, 0x001000d8u, 16, 16, "sections/volcano/flats/base0", 3},
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
    // The volcano's lava rim, named as Zhon's (checked on the authored heights); two
    // variants each.
    {"sections/volcano/base/",
     {"n", "s", "e", "w", "1nw", "1ne", "1se", "1sw", "2nw", "2ne", "2sw", "2se"},
     {"1", "2", "1"}, ".tnt"},
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
// The volcano's ground is a plateau at 220 over lava at 0, under takx20's sea 56.
struct WorldLevels { uint8_t sea, land; };
constexpr std::array<WorldLevels, kMapTypes> kLevels = {{
    {40, 80}, {58, 62}, {58, 62}, {58, 62}, {40, 80}, {56, 220},
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

// Volcano (features/volcano in IPData.hpi): no trees or rocks; takx20 and takx21
// scatter Creon works, whole and ruined, which stand in for the rocks here.
constexpr const char* kVolRock[] = {"CreInvent01a", "CreInvent04a", "CreInvent01", "CreInvent04", "CreInvent03a",
                                    "CreInvent03", "CreInvent02a", "CreInvent02"};   // small->big

constexpr std::array<WorldFeatures, kMapTypes> kWorldFeat = {{
    {{kAraTree, 10}, {kAraRock, 7},  {{"AraMana01", "AraMana02", "AraMana03"}}},
    {{kTarTree, 9},  {kTarRock, 7},  {{"TarMana01", "TarMana02", "TarMana03"}}},
    {{kVerTree, 9},  {kVerRock, 7},  {{"VerMana01", "VerMana02", "VerMana03"}}},
    {{kZonTree, 6},  {kZonRock, 7},  {{"ZonMana01", "ZonMana02", "ZonMana03"}}},
    {{kCreTree, 9},  {kCreRock, 13},  {{"CReMana01", "CREMana02", "CREMana03"}}},
    {{nullptr, 0},   {kVolRock, 8},   {{"CReMana01", "CREMana02", "CREMana03"}}},
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
    // The volcano ships no standing stones: its deposits are bare, as takx20's are.
    {nullptr, 0, {100, 100, 100, 100, 100, 100}, 0, {rN, rNE, rE, rSE, rS, rSW, rW, rNW}},
}};

// Volcano Industrial Zone: retail's walkway sections (sections/volcano/roads), one
// per section, named for the edges their walkway reaches (e_n_s01, end_w; trans_n
// climbs from a plateau to its south), and the lava pads (isle1..4) that dot the rest.
constexpr const char* kIndustrialRoads = "sections/volcano/roads/";
constexpr const char* kIndustrialIsle = "sections/volcano/base/isle";

// Standalone retail relief patches; candidates are checked against their world's
// low-ground boundary before stamping artwork and heights together.
constexpr const char* kReliefDir[]={"Sections/Aramon/Low Hills/","Sections/Taros/Low Specials/",
    "Sections/Veruna/Low Specials/","Sections/Zhon/Low Specials Jungle/","sections/creon/low tile breakers/",
    nullptr};   // the volcano's tile breakers all rise off its plating: none sits level as a hill

// ---- v10 retail terrain kits ---------------------------------------------------
// Every retail kit below is a set of whole 512px sections authored to interlock on
// the section grid by marching squares: a piece's case says which of its corners
// (bit0 NW, bit1 NE, bit2 SW, bit3 SE) lie inside the shape -- high ground for a
// plateau edge, mountain for a range, road-paved town for a settlement. Plateau
// edges are classified from their authored corner HEIGHTS, towns from their ROAD
// cells, and mountains (whose blockers are too ragged to classify) from the
// retail role names. See docs/random-map-generation.md for the survey.
struct KitSpec {
    uint8_t low, high;              // the kit's authored low-ground and plateau levels
    const char* edges[2];           // cliffs + hillsides (straight walkable ones become ramps)
    const char* top;                // plateau fill
    const char* mountains;          // named marching kit standing on the plateau
    const char* mountainFill;       // interior piece, when the kit has no centre role
    const char* snowLine;           // Creon: 256px snow-line kit around its snowbound mountains
    const char* snowFill;
    const char* peaks;              // Zhon: standalone mountain sections (no marching kit)
    const char* lowTowns; const char* lowTownFill;
    const char* highTowns; const char* highTownFill;
    Span buildings;                 // town feature palette (real TDF footprints decide fit)
    // Hillside ramps as retail lays them, for rims of case 3, 5, 10, 12: a run of
    // straight hillside pieces in rim order (west to east, or north to south) whose
    // ends meet the cliffs cleanly. Retail's hillside variants are end pieces: each
    // joins a cliff on one side only. nullptr: no hillside runs (a single hillside
    // between straight cliffs, if the kit has no better ramp).
    std::array<const char*, 4> rampRuns{};
};
constexpr const char* kAraTown[] = {"AraHut01","AraHut02","AraHut03","AraHut04","AraHut05","AraHut06","AraHut07",
    "AraHut08","AraHut09","AraHut10","AraHut11","AraHut12","AraBuild01","AraBuild02","AraBuild03",
    "AraWell01","AraWell02","AraWell03","AraWell04","AraCart01","AraCart02","AraCart03","AraCart04","AraFount01"};
constexpr const char* kTarTown[] = {"TarHut01","TarHut02","TarHut03","TarHut04","TarHut05","TarBuild01","TarBuild02",
    "TarBuild03","TarDev04","TarDev05","TarDev06","TarDev07","TarDev08","TarDev09"};
constexpr const char* kVerTown[] = {"VerHut01","VerHut02","VerHut03","VerHut04","VerHut05","VerHut06","VerHut07",
    "VerHut08","VerHut09","VerHut10","VerBuild05","VerBuild08","VerWell","VerLamp01","VerFountain01"};
// Zhon has no settlement sections: its villages are retail tents, huts and bonfires.
constexpr const char* kZonTown[] = {"ZonHut01","ZonHut02","ZonHut03","ZonHut04","ZonHut05","ZonHut06","ZonHut07",
    "ZonHut08","ZonHut09","ZonHut10","ZonHut11","ZonFire01","ZonFire02"};
constexpr const char* kCreTown[] = {"CreHouse01","CreHouse02","CreHouse03","CreHouse05","CreHouse07","CreBuild06",
    "CreBuild07","CreBuild08","CreBuild09","CreCart01"};
constexpr std::array<KitSpec, kMapTypes> kKitSpec = {{
    {80, 200, {"Sections/Aramon/Low to High Cliffs/", "Sections/Aramon/Low to High Hillsides/"},
     "Sections/Aramon/High Flats/alightgr_200.TNT", "Sections/Aramon/High Mountains/", nullptr, nullptr, nullptr, nullptr,
     "Sections/Aramon/Low Towns/", "Sections/Aramon/Low Flats/cobb_80.TNT",
     "Sections/Aramon/High Towns/", "Sections/Aramon/High Flats/cobb_200.TNT", {kAraTown, 24}},
    {62, 235, {"Sections/Taros/Low to High Cliffs/", "Sections/Taros/Low to High Hillsides/"},
     "Sections/Taros/High Flats/high_ground.TNT", "Sections/Taros/High Mountains/", nullptr, nullptr, nullptr, nullptr,
     "Sections/Taros/Low Towns/", "Sections/Taros/Low Flats/low cobbles.TNT",
     "Sections/Taros/High Towns/", "Sections/Taros/High Flats/high cobbles.TNT", {kTarTown, 14},
     // Measured on the section art (tools/mapgen_seams pairs): every join of these
     // runs scores like retail's own boundaries; a lone e/w/s/n hillside does not.
     {"sse ssw", "ese ene", "wsw wnw", "nnw"}},
    // Veruna ships no mountain kit; its High Specials are craters and outcrops.
    {62, 200, {"Sections/Veruna/Low to High Cliffs/", nullptr},
     "Sections/Veruna/High Flats/int_200.TNT", nullptr, nullptr, nullptr, nullptr, nullptr,
     "Sections/Veruna/Low Towns/", "Sections/Veruna/Low Flats/s_cobbflat.TNT",
     "Sections/Veruna/High Towns/", "Sections/Veruna/High Flats/i_cobbflat.TNT", {kVerTown, 15}},
    // Zhon's jungle cliffs rise to its mountain plateau; its mountain-top kit is a
    // ridge set rather than a marching kit, so the standalone spires stand alone.
    {63, 215, {"Sections/Zhon/Low to High Cliffs Jungle/", nullptr},
     "Sections/Zhon/High Flats/mountainplateau230.TNT", nullptr, nullptr, nullptr, nullptr, "Sections/Zhon/High Specials/mt",
     nullptr, nullptr, nullptr, nullptr, {kZonTown, 13}},
    // Creon's hillsides are walkable slopes; its mountains stand on snow.
    {80, 250, {"sections/creon/hillsides/", nullptr},
     "sections/creon/flatties/highland512.tnt", "sections/creon/mountains/", "sections/creon/flatties/intmt512.tnt",
     "sections/creon/snow line/", "sections/creon/flatties/snow256.tnt", nullptr,
     "sections/creon/low towns/", "sections/creon/flatties/chunglo.tnt",
     "sections/creon/high towns/", "sections/creon/flatties/chunghi.tnt", {kCreTown, 10}},
    // The volcano ships no cliffs, mountains or towns: its plateau is the ground.
    {220, 220, {nullptr, nullptr}, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, {nullptr, 0}},
}};
// Every directory or file the kits read, for the multiplayer data hash.
std::vector<std::string> kitDirectories();

// Sorted, cached section loads (independent of archive enumeration order).
class SectionCache {
public:
    explicit SectionCache(const hpi::Vfs& vfs) : vfs_(vfs) {}
    const tnt::Map* load(const std::string& path);
    std::vector<std::string> list(const std::string& prefix) const;   // sorted *.tnt
private:
    const hpi::Vfs& vfs_;
    std::map<std::string, std::unique_ptr<tnt::Map>> cache_;
};

// walkable: the generator's army class (6 cells, slope 16) climbs it; climbable: the
// builder/infantry class (GROUND3: 3 cells, slope 30) does. alternate: from a kit's
// second edge family (hillsides), used only as a ramp.
struct KitPiece { const tnt::Map* map = nullptr; bool walkable = false, gate = false, alternate = false, climbable = false;
                 std::string name;   // lower-case file stem, e.g. "sse"
};
using CaseKit = std::array<std::vector<KitPiece>, 16>;
inline bool kitEmpty(const CaseKit& k) { for (const auto& v : k) if (!v.empty()) return false; return true; }
// Plateau edges by corner height; straight pieces a large army can climb are walkable.
CaseKit edgeKit(SectionCache& cache, const KitSpec& spec);
// Mountain / snow-line / town kits by retail role name (towns also by road corners).
CaseKit namedKit(SectionCache& cache, const char* dir, int size, bool towns);
// Standalone sections (Zhon spires) from a path prefix.
std::vector<const tnt::Map*> standaloneKit(SectionCache& cache, const char* prefix);

// Even compass directions (integer, scaled by 1000) for start-position rings.
constexpr std::array<std::pair<int, int>, 8> kCompass = {{
    {0, -1000}, {707, -707}, {1000, 0}, {707, 707},
    {0, 1000}, {-707, 707}, {-1000, 0}, {-707, -707},
}};

// The closest two starts may be, in cells (generateBalanced refuses a closer plan).
constexpr int kMinStartGap = 72;
// Whether the start plan keeps kMinStartGap for every rotation the seed can pick
// (seed % 8): a size that holds here holds for any seed. sanitize asks it.
bool startsSpacedForAnySeed(const Params& p);


} // namespace tak::mapgen::detail
