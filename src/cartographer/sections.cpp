#include "cartographer/sections.h"

#include "hpi/hpi.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include "util/virtualpath.h"

namespace cart {

void SectionLibrary::scan(const ta::hpi::Vfs& vfs, const std::string& world) {
    sections_.clear();
    cache_.clear();++generation_;
    std::string normalized=world;
    std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](unsigned char c){return char(std::tolower(c));});
    // TA worlds name a planet ("Green planet" -> sections/greenworld).
    if (!vfs.list("sections/" + normalized).size()) normalized = ta::tnt::taSectionFolder(normalized);
    std::string root = "sections/" + normalized + "/";   // VFS keys are lowercased
    for (const std::string& p : vfs.list("sections")) {
        std::string lo = p;
        std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c){return char(std::tolower(c));});
        if (lo.rfind(root, 0) != 0) continue;
        // Kingdoms sections are .tnt; TA's are .sct (Map::loadSct).
        if (ta::vpath::extension(lo) != ".tnt" && ta::vpath::extension(lo) != ".sct") continue;
        // sections/<world>/<category>/<name>.tnt

        SectionRef r;
        r.path = p;
        r.name = ta::vpath::stem(p);
        r.category = std::string(ta::vpath::filename(std::string_view(p).substr(0,p.find_last_of("/\\"))));
        sections_.push_back(std::move(r));
    }
    std::sort(sections_.begin(), sections_.end(), [](const SectionRef& a, const SectionRef& b) {
        return a.category != b.category ? a.category < b.category : a.name < b.name;
    });
}

const ta::tnt::Map* SectionLibrary::load(const ta::hpi::Vfs& vfs, const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end()) return &it->second;
    try {
        auto d = vfs.read(path);
        auto m = ta::vpath::extension(path) == ".sct" ? ta::tnt::Map::loadSct(d, path) : ta::tnt::Map::load(d, path);
        return &cache_.emplace(path, std::move(m)).first->second;
    } catch (const std::exception&) {
        return nullptr;
    }
}

namespace {
uint64_t tileHash(const uint8_t* p) {   // FNV-1a over the 1024 pixels
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 1024; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
}  // namespace

TileInterner::TileInterner(ta::tnt::Map& map) : map_(map) {
    map_.tileCount = std::clamp(map_.tileCount, 0, int(map_.tileGfx.size() / 1024));
    map_.tileGfx.resize(size_t(map_.tileCount) * 1024);
    index_.reserve(size_t(map_.tileCount) * 2);
    for (int t = 0; t < map_.tileCount; ++t) index_.emplace(tileHash(map_.tileGfx.data() + size_t(t) * 1024), uint16_t(t));
}

uint16_t TileInterner::intern(const uint8_t* pixels) {
    const uint64_t h = tileHash(pixels);
    const auto [first, last] = index_.equal_range(h);
    for (auto it = first; it != last; ++it)
        if (std::memcmp(map_.tileGfx.data() + size_t(it->second) * 1024, pixels, 1024) == 0) return it->second;
    if (map_.tileCount >= 0xFFFF) return 0;
    const auto t = uint16_t(map_.tileCount++);
    map_.tileGfx.resize(size_t(map_.tileCount) * 1024);   // `pixels` may point into another map, never this one's tileGfx
    std::memcpy(map_.tileGfx.data() + size_t(t) * 1024, pixels, 1024);
    index_.emplace(h, t);
    return t;
}

void setBlockTile(ta::tnt::Map& map, size_t block, uint16_t tile) {
    map.tileIndex[block] = tile;
    map.tileKeys[block] = tile;
    map.tileCols[block] = 0;
    map.tileRows[block] = 0;
}

void ensureTilePlane(ta::tnt::Map& map) {
    const size_t blocks = size_t(map.blocksX) * map.blocksY;
    if (map.tileIndex.size() != blocks) map.tileIndex.assign(blocks, 0);
    map.tileKeys.assign(map.tileIndex.begin(), map.tileIndex.end());
    map.tileCols.assign(blocks, 0);
    map.tileRows.assign(blocks, 0);
}

void compactTiles(ta::tnt::Map& map) {
    if (map.tileCount <= 0) return;
    std::vector<int> remap(size_t(map.tileCount), -1);
    std::vector<uint8_t> used(size_t(map.tileCount), 0);
    for (auto t : map.tileIndex) if (t < used.size()) used[t] = 1;
    int next = 0;
    for (size_t t = 0; t < used.size(); ++t)
        if (used[t]) {
            if (size_t(next) != t) std::memmove(map.tileGfx.data() + size_t(next) * 1024, map.tileGfx.data() + t * 1024, 1024);
            remap[t] = next++;
        }
    if (next == map.tileCount) return;
    map.tileCount = std::max(next, 1);
    if (!next) std::fill_n(map.tileGfx.begin(), 1024, uint8_t(0));   // keep one tile for an all-zero plane
    map.tileGfx.resize(size_t(map.tileCount) * 1024);
    for (size_t b = 0; b < map.tileIndex.size(); ++b)
        setBlockTile(map, b, uint16_t(map.tileIndex[b] < remap.size() && remap[map.tileIndex[b]] >= 0 ? remap[map.tileIndex[b]] : 0));
}

bool stampSection(ta::tnt::Map& map, const ta::tnt::Map& section, int bx, int by, StampLayers layers) {
    if (bx + section.blocksX <= 0 || by + section.blocksY <= 0 ||
        bx >= map.blocksX || by >= map.blocksY)
        return false;

    // A TA map carries its own tiles: copy the section's in (once each, by
    // content) and point the blocks at them. tileKeys mirror tileIndex.
    if (section.tileCount > 0) {
        ensureTilePlane(map);
        TileInterner tiles(map);
        std::vector<int> remap(size_t(section.tileCount), -1);   // section tile -> map tile, on first use
        for (int sy = 0; sy < section.blocksY; ++sy)
            for (int sx = 0; sx < section.blocksX; ++sx) {
                const int mx = bx + sx, my = by + sy;
                if (mx < 0 || my < 0 || mx >= map.blocksX || my >= map.blocksY) continue;
                const size_t si = size_t(sy) * section.blocksX + sx, mi = size_t(my) * map.blocksX + mx;
                const size_t st = section.tileIndex[si];
                if (remap[st] < 0) remap[st] = tiles.intern(section.tileGfx.data() + st * 1024);
                setBlockTile(map, mi, uint16_t(remap[st]));
            }
    } else
    // Tile plane: one entry per 32px block, row-major over blocksX x blocksY.
    for (int sy = 0; sy < section.blocksY; ++sy) {
        int my = by + sy;
        if (my < 0 || my >= map.blocksY) continue;
        for (int sx = 0; sx < section.blocksX; ++sx) {
            int mx = bx + sx;
            if (mx < 0 || mx >= map.blocksX) continue;
            size_t si = size_t(sy) * section.blocksX + sx;
            size_t mi = size_t(my) * map.blocksX + mx;
            map.tileKeys[mi] = section.tileKeys[si];
            map.tileCols[mi] = section.tileCols[si];
            map.tileRows[mi] = section.tileRows[si];
        }
    }

    // Feature-name interning: prefab index -> map index (add unseen names).
    auto internFeature = [&](uint16_t v) -> uint16_t {
        if (v >= 0xFFFA) return v;   // 0xFFFF empty / 0xFFFB road / 0xFFFC blocker
        if (v >= section.featureNames.size()) return 0xFFFF;
        const std::string& name = section.featureNames[v];
        for (size_t i = 0; i < map.featureNames.size(); ++i)
            if (map.featureNames[i] == name) return uint16_t(i);
        map.featureNames.push_back(name);
        return uint16_t(map.featureNames.size() - 1);
    };

    // Heights (u8/cell) + features (u16/cell), one entry per 16px cell. A block
    // is 2 cells, so the cell origin is block*2.
    int cx0 = bx * 2, cy0 = by * 2;
    for (int sy = 0; sy < section.height; ++sy) {
        int my = cy0 + sy;
        if (my < 0 || my >= map.height) continue;
        for (int sx = 0; sx < section.width; ++sx) {
            int mx = cx0 + sx;
            if (mx < 0 || mx >= map.width) continue;
            size_t si = size_t(sy) * section.width + sx;
            size_t mi = size_t(my) * map.width + mx;
            if (si < section.heights.size()) map.heights[mi] = section.heights[si];
            if (si < section.features.size()) {
                const uint16_t source=section.features[si];
                if(layers.objects)map.features[mi]=internFeature(source);
                else if(map.features[mi]>=0xFFFA || map.features[mi]>=map.featureNames.size())
                    map.features[mi]=source>=0xFFFA?source:0xFFFF;
            }
        }
    }
    return true;
}

} // namespace cart
