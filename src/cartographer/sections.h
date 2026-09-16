#pragma once

// Cartographer's section-prefab palette + stamp brush (docs/cartographer-port.md).
// A "section" is a 512px (32x32 cell / 16x16 block) prefab .tnt in sections.hpi
// under Sections/<World>/<Category>/<name>.TNT. Painting = snapping to the block
// grid and copying a section's cells into the map -- the retail brush model.

#include "tnt/tnt.h"

#include <map>
#include <string>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace cart {

struct SectionRef {
    std::string category;   // e.g. "High Flats"
    std::string name;       // e.g. "cobb_200"
    std::string path;       // VFS path to the .TNT
};

class SectionLibrary {
public:
    // Scan Sections/<world>/** for prefab .TNTs (world = aramon/taros/veruna/zhon).
    void scan(const ta::hpi::Vfs& vfs, const std::string& world);
    const std::vector<SectionRef>& list() const { return sections_; }
    // Load (and cache) a prefab by VFS path; nullptr if it won't parse.
    const ta::tnt::Map* load(const ta::hpi::Vfs& vfs, const std::string& path);

private:
    std::vector<SectionRef> sections_;
    std::map<std::string, ta::tnt::Map> cache_;
};

// Stamp `section` into `map` with its top-left block at (bx, by), clipped to the
// map. Copies the tile plane (keys/cols/rows) + heights, and the feature plane
// REMAPPED by name (a prefab's feature indices point into ITS own name table, so
// each is resolved to a name and re-interned in map.featureNames; the 0xFFFF/
// 0xFFFB/0xFFFC sentinels pass through). Returns false if fully off-map.
bool stampSection(ta::tnt::Map& map, const ta::tnt::Map& section, int bx, int by);

} // namespace cart
