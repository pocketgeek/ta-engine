#include "cartographer/sections.h"

#include "hpi/hpi.h"
#include "sct/sct.h"
#include "util/strcase.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>

namespace cart {

std::vector<std::string> SectionLibrary::worlds(const ta::hpi::Vfs& vfs) {
    std::vector<std::string> out;
    for (const std::string& p : vfs.list("sections")) {
        std::string lo = p;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        // sections/<world>/... -- take the segment after the root.
        const std::string root = "sections/";
        if (lo.rfind(root, 0) != 0) continue;
        size_t e = lo.find('/', root.size());
        if (e == std::string::npos) continue;
        std::string w = lo.substr(root.size(), e - root.size());
        if (w.empty()) continue;
        if (std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void SectionLibrary::scan(const ta::hpi::Vfs& vfs, const std::string& world) {
    sections_.clear();
    cache_.clear();
    std::string root = "sections/" + world + "/";   // VFS keys are lowercased
    for (const std::string& p : vfs.list("sections")) {
        std::string lo = p;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        if (lo.rfind(root, 0) != 0) continue;
        // TA keeps its prefabs in ".sct" containers; Kingdoms used plain TNTs.
        // Accept both -- load() below decodes either into a tnt::Map, so nothing
        // downstream has to know which it came from.
        if (!ta::iendsWith(lo, ".tnt") && !ta::iendsWith(lo, ".sct")) continue;
        // sections/<world>/<category>/<name>.tnt
        std::filesystem::path fp(p);
        SectionRef r;
        r.path = p;
        r.name = fp.stem().string();
        r.category = fp.parent_path().filename().string();
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
        auto m = ta::sct::isSectionPath(path) ? ta::sct::load(d, path)
                                              : ta::tnt::Map::load(d, path);
        return &cache_.emplace(path, std::move(m)).first->second;
    } catch (const std::exception&) {
        return nullptr;
    }
}

bool stampSection(ta::tnt::Map& map, const ta::tnt::Map& section, int bx, int by) {
    if (bx + section.blocksX <= 0 || by + section.blocksY <= 0 ||
        bx >= map.blocksX || by >= map.blocksY)
        return false;

    // Tile interning. A tile INDEX means nothing outside the map that owns the
    // library it indexes, so -- exactly as with feature names below -- each tile
    // the section references is copied into the destination's library and deduped
    // by content. Copying the index straight across (which is all Kingdoms had to
    // do, its art being shared and content-addressed) would silently repaint the
    // stamped area with whatever tiles happened to sit at those indices.
    //
    // The content index is built once per stamp and keyed by an FNV-1a hash of
    // the tile's 1024 bytes, with a full compare on hit so a collision cannot
    // alias two different tiles together.
    auto tileHash = [](const uint8_t* p) {
        uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < ta::tnt::kTileBytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    };
    std::unordered_multimap<uint64_t, uint16_t> byContent;
    for (int i = 0; i < map.numTiles; ++i)
        if (const uint8_t* p = map.tile(i)) byContent.emplace(tileHash(p), uint16_t(i));

    auto internTile = [&](uint16_t si) -> uint16_t {
        const uint8_t* src = section.tile(si);
        if (!src) return 0;
        uint64_t h = tileHash(src);
        auto [lo, hi] = byContent.equal_range(h);
        for (auto it = lo; it != hi; ++it) {
            const uint8_t* have = map.tile(it->second);
            if (have && std::equal(have, have + ta::tnt::kTileBytes, src)) return it->second;
        }
        uint16_t idx = uint16_t(map.numTiles);
        map.tileGfx.insert(map.tileGfx.end(), src, src + ta::tnt::kTileBytes);
        ++map.numTiles;
        byContent.emplace(h, idx);
        return idx;
    };

    // Tile plane: one entry per 32px block, row-major over blocksX x blocksY.
    for (int sy = 0; sy < section.blocksY; ++sy) {
        int my = by + sy;
        if (my < 0 || my >= map.blocksY) continue;
        for (int sx = 0; sx < section.blocksX; ++sx) {
            int mx = bx + sx;
            if (mx < 0 || mx >= map.blocksX) continue;
            size_t si = size_t(sy) * section.blocksX + sx;
            size_t mi = size_t(my) * map.blocksX + mx;
            map.tiles[mi] = internTile(section.tiles[si]);
        }
    }

    // Feature-name interning: prefab index -> map index (add unseen names).
    auto internFeature = [&](uint16_t v) -> uint16_t {
        if (v >= 0xFFFA) return v;   // 0xFFFF empty / 0xFFFE covered by a neighbour
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
            if (si < section.features.size())
                map.features[mi] = internFeature(section.features[si]);
        }
    }
    return true;
}

} // namespace cart
