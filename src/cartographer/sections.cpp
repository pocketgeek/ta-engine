#include "cartographer/sections.h"

#include "hpi/hpi.h"

#include <algorithm>
#include <filesystem>

namespace cart {

void SectionLibrary::scan(const ta::hpi::Vfs& vfs, const std::string& world) {
    sections_.clear();
    cache_.clear();
    std::string root = "sections/" + world + "/";   // VFS keys are lowercased
    for (const std::string& p : vfs.list("sections")) {
        std::string lo = p;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        if (lo.rfind(root, 0) != 0) continue;
        if (std::filesystem::path(lo).extension() != ".tnt") continue;
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
        auto m = ta::tnt::Map::load(d, path);
        return &cache_.emplace(path, std::move(m)).first->second;
    } catch (const std::exception&) {
        return nullptr;
    }
}

bool stampSection(ta::tnt::Map& map, const ta::tnt::Map& section, int bx, int by) {
    if (bx + section.blocksX <= 0 || by + section.blocksY <= 0 ||
        bx >= map.blocksX || by >= map.blocksY)
        return false;

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
            if (si < section.features.size())
                map.features[mi] = internFeature(section.features[si]);
        }
    }
    return true;
}

} // namespace cart
