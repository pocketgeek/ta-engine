#include "cartographer/sections.h"

#include "hpi/hpi.h"

#include <algorithm>
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

bool stampSection(ta::tnt::Map& map, const ta::tnt::Map& section, int bx, int by, StampLayers layers) {
    if (bx + section.blocksX <= 0 || by + section.blocksY <= 0 ||
        bx >= map.blocksX || by >= map.blocksY)
        return false;

    // A TA map carries its own tiles: copy the section's in (once each, by
    // content) and point the blocks at them. tileKeys mirror tileIndex.
    if (map.tileCount > 0 && section.tileCount > 0) {
        std::unordered_map<std::string_view, uint16_t> have;
        auto tileView = [](const std::vector<uint8_t>& gfx, size_t t) {
            return std::string_view(reinterpret_cast<const char*>(gfx.data()) + t * 1024, 1024);
        };
        std::vector<uint16_t> remap(size_t(section.tileCount), 0);
        std::vector<std::pair<size_t, size_t>> pending;   // (section tile, slot) added this stamp
        for (size_t t = 0; t < size_t(map.tileCount); ++t) have.emplace(tileView(map.tileGfx, t), uint16_t(t));
        for (size_t t = 0; t < size_t(section.tileCount); ++t) {
            const auto sv = tileView(section.tileGfx, t);
            if (auto it = have.find(sv); it != have.end()) { remap[t] = it->second; continue; }
            if (map.tileCount + pending.size() >= 0xFFFF) { remap[t] = 0; continue; }
            remap[t] = uint16_t(map.tileCount + int(pending.size()));
            pending.push_back({t, remap[t]});
            have.emplace(tileView(section.tileGfx, t), remap[t]);   // keyed into section memory until copied
        }
        for (const auto& [t, slot] : pending)
            map.tileGfx.insert(map.tileGfx.end(), section.tileGfx.begin() + std::ptrdiff_t(t * 1024),
                               section.tileGfx.begin() + std::ptrdiff_t(t * 1024 + 1024));
        map.tileCount += int(pending.size());
        for (int sy = 0; sy < section.blocksY; ++sy)
            for (int sx = 0; sx < section.blocksX; ++sx) {
                const int mx = bx + sx, my = by + sy;
                if (mx < 0 || my < 0 || mx >= map.blocksX || my >= map.blocksY) continue;
                const size_t si = size_t(sy) * section.blocksX + sx, mi = size_t(my) * map.blocksX + mx;
                const uint16_t t = remap[section.tileIndex[si]];
                map.tileIndex[mi] = t;
                map.tileKeys[mi] = t;
                map.tileCols[mi] = 0;
                map.tileRows[mi] = 0;
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
