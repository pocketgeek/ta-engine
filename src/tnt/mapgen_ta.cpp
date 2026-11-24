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
            if (std::uniform_real_distribution<double>(0, 1)(rng) > density) continue;
            const size_t si = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng)];
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
    const int n = std::clamp(int(p.players), 2, 8);
    for (int i = 0; i < n; ++i) {
        const double a = 2.0 * 3.14159265358979323846 * (double(i) + 0.125) / n;
        const int cx = int(m.width / 2 + std::cos(a) * m.width * 0.36);
        const int cz = int(m.height / 2 + std::sin(a) * m.height * 0.36);
        r.starts.push_back({std::clamp(cx, 4, m.width - 5), std::clamp(cz, 4, m.height - 5)});
    }
    // Ore: the world's metal features, a few near every start and a scatter.
    std::vector<std::string> ores;
    for (const auto& path : vfs.list(std::string("features/") + W.features)) {
        if (ta::vpath::extension(path) != ".tdf") continue;
        try {
            auto b = vfs.read(path);
            auto root = tdf::parseText(std::string(b.begin(), b.end()), path);
            for (const auto& name : root.childOrder) {
                const tdf::Node* f = root.child(name);
                if (f && f->numberOr("metal", 0) > 0 && f->numberOr("indestructible", 0) != 0) ores.push_back(name);
            }
        } catch (...) {}
    }
    // Metal has no ore deposits (its metal is the map's surfacemetal): scatter the
    // world's reclaimable metal features (towers, pipes) instead.
    if (ores.empty())
        for (const auto& path : vfs.list(std::string("features/") + W.features)) {
            if (ta::vpath::extension(path) != ".tdf") continue;
            try {
                auto b = vfs.read(path);
                auto root = tdf::parseText(std::string(b.begin(), b.end()), path);
                for (const auto& name : root.childOrder) {
                    const tdf::Node* f = root.child(name);
                    if (f && f->numberOr("metal", 0) > 0) ores.push_back(name);
                }
            } catch (...) {}
        }
    if (!ores.empty()) {
        auto place = [&](int cx, int cz) {
            if (cx < 2 || cz < 2 || cx + 4 >= m.width || cz + 4 >= m.height) return;
            const std::string& name = ores[std::uniform_int_distribution<size_t>(0, ores.size() - 1)(rng)];
            auto it = std::find(m.featureNames.begin(), m.featureNames.end(), name);
            uint16_t idx;
            if (it == m.featureNames.end()) { idx = uint16_t(m.featureNames.size()); m.featureNames.push_back(name); }
            else idx = uint16_t(it - m.featureNames.begin());
            m.features[size_t(cz) * m.width + cx] = idx;
        };
        for (const auto& [sx, sz] : r.starts)
            for (int k = 0; k < 4; ++k) {
                const double a = 2.0 * 3.14159265358979323846 * k / 4 + 0.6;
                place(sx + int(std::cos(a) * 10), sz + int(std::sin(a) * 10));
            }
        const int scatter = 2 + (m.width * m.height) / 4096;
        for (int k = 0; k < scatter; ++k)
            place(std::uniform_int_distribution<int>(4, m.width - 8)(rng),
                  std::uniform_int_distribution<int>(4, m.height - 8)(rng));
    }
    return r;
}

}  // namespace ta::mapgen
