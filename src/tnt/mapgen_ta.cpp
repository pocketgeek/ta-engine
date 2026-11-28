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
    // 3 of it (sections are authored to join at one level, give or take a step).
    const int cw = int(w) * 2, ch = int(h) * 2;
    std::vector<int> edge;
    for (int x = 0; x < cw; ++x) { edge.push_back(s.heights[size_t(x)]); edge.push_back(s.heights[size_t((ch - 1) * cw + x)]); }
    for (int z = 0; z < ch; ++z) { edge.push_back(s.heights[size_t(z * cw)]); edge.push_back(s.heights[size_t(z * cw + cw - 1)]); }
    int counts[256] = {};
    for (int e : edge) ++counts[e];
    const int b = int(std::max_element(counts, counts + 256) - counts);
    bool ok = true;
    for (int e : edge) if (std::abs(e - b) > 3) { ok = false; break; }
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
struct TaWorld { const char* sections; const char* features; const char* planet; int sea; };
const TaWorld kTaWorlds[] = {
    {"greenworld", "green", "Green Planet", 0},
    {"mars", "mars", "Red Planet", 0},
    {"archipelago", "archi", "Archipelago", 0},
    {"metal", "metal", "Metal", 0},
    {"moon", "moon", "Moon", 0},
    {"lava", "lava", "Lava", 0},
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
    // The largest family sharing one border height.
    std::map<int, std::vector<size_t>> fam;   // key: border height * 32 + ground texture
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].border >= 0) fam[all[i].border * 32 + all[i].ground].push_back(i);
    // Prefer land (a border above 16): the largest such family, else the largest.
    int border = 0;
    std::vector<size_t> pool;
    for (auto& [k, v] : fam) if (k / 32 >= 16 && v.size() > pool.size()) { pool = v; border = k / 32; }
    if (pool.empty()) for (auto& [k, v] : fam) if (v.size() > pool.size()) { pool = v; border = k / 32; }
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
    const uint16_t filler = uint16_t(addSection(pool[0]) + all[pool[0]].tileMap[0]);
    std::fill(m.tileIndex.begin(), m.tileIndex.end(), filler);
    std::vector<uint8_t> used(size_t(m.blocksX) * m.blocksY, 0);
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

    // Starts: evenly round the map, inset from the edge.
    const int n = std::clamp(int(p.players), 2, kMaxPlayers);
    for (int i = 0; i < n; ++i) {
        const double a = 2.0 * 3.14159265358979323846 * (double(i) + 0.125) / n;
        const int cx = int(m.width / 2 + std::cos(a) * m.width * 0.36);
        const int cz = int(m.height / 2 + std::sin(a) * m.height * 0.36);
        r.starts.push_back({std::clamp(cx, 4, m.width - 5), std::clamp(cz, 4, m.height - 5)});
    }
    // The world's features: TREES (category trees), ROCKS (category rocks, reclaimable
    // metal) and metal deposits (metal > 0 and indestructible -- the patches a
    // TotalA.exe map writes its metal from, 0x4220ab). Shipped maps lay their
    // deposits with the world's metal patches and only now and then its aqua ore,
    // never its glyphs; Metal's own patches are none (its metal is surface metal),
    // so it gets its reclaimable metal features.
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
    auto place = [&](const Feat& f, int cx, int cz, bool atStart) {
        if (cx < 2 || cz < 2 || cx + f.fx + 2 >= m.width || cz + f.fz + 2 >= m.height) return false;
        for (int z = cz; z < cz + f.fz; ++z)
            for (int x = cx; x < cx + f.fx; ++x) {
                const uint8_t t = taken[size_t(z) * m.width + x];
                if (t == 1 || (t == 2 && !atStart)) return false;
                if (m.heights[size_t(z) * m.width + x] <= m.seaLevel) return false;
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
    // METAL: a cluster by every start, and clusters over the map, each a few patches
    // side by side (aqua ore one time in eight where the world has both).
    if (!patches.empty() || !aqua.empty()) {
        auto deposit = [&]() -> const Feat& {
            if (!aqua.empty() && (patches.empty() || roll(rng, 8) == 0)) return aqua[size_t(roll(rng, aqua.size()))];
            return patches[size_t(roll(rng, patches.size()))];
        };
        auto cluster = [&](int cx, int cz, int count, bool atStart) {
            for (int k = 0, tries = 0; k < count && tries < count * 8; ++tries) {
                const Feat& f = deposit();
                if (place(f, cx + rollIn(rng, -4, 4), cz + rollIn(rng, -4, 4), atStart)) ++k;
            }
        };
        const int perStart = 2 + int(p.manaDensity) * 4 / 255;   // 2..6
        for (const auto& [sx, sz] : r.starts) {
            const double a = roll01(rng) * 6.283185307179586;
            cluster(sx + int(std::cos(a) * 9), sz + int(std::sin(a) * 9), perStart, true);
        }
        const int sites = int(area / 8192.0 * (0.5 + 2.5 * p.manaDensity / 255.0));
        for (int k = 0; k < sites; ++k)
            cluster(rollIn(rng, 6, m.width - 10), rollIn(rng, 6, m.height - 10), rollIn(rng, 1, 3), false);
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
