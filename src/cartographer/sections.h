#pragma once

// Cartographer's section-prefab palette + stamp brush (docs/cartographer-port.md).
// A "section" is a 512px (32x32 cell / 16x16 block) prefab .tnt in sections.hpi
// under Sections/<World>/<Category>/<name>.TNT. Painting = snapping to the block
// grid and copying a section's cells into the map -- the retail brush model.

#include "tnt/tnt.h"

#include <map>
#include <string>
#include <vector>

namespace tak::hpi { class Vfs; }

namespace cart {

struct SectionRef {
    std::string category;   // e.g. "High Flats"
    std::string name;       // e.g. "cobb_200"
    std::string path;       // VFS path to the .TNT
};

class SectionLibrary {
public:
    // Scan Sections/<world>/** for prefab .TNTs (world = aramon/taros/veruna/zhon).
    void scan(const tak::hpi::Vfs& vfs, const std::string& world);
    const std::vector<SectionRef>& list() const { return sections_; }
    // Load (and cache) a prefab by VFS path; nullptr if it won't parse.
    const tak::tnt::Map* load(const tak::hpi::Vfs& vfs, const std::string& path);
    // Bumped by scan(), which frees every map load() returned.
    unsigned generation() const { return generation_; }

private:
    unsigned generation_ = 0;
    std::vector<SectionRef> sections_;
    std::map<std::string, tak::tnt::Map> cache_;
};

// Stamp `section` into `map` with its top-left block at (bx, by), clipped to the
// map. Copies the tile plane (keys/cols/rows) + heights, and the feature plane
// REMAPPED by name (a prefab's feature indices point into ITS own name table, so
// each is resolved to a name and re-interned in map.featureNames; the 0xFFFF/
// 0xFFFB/0xFFFC sentinels pass through). Returns false if fully off-map.
// Art and height are coupled (cliff art has baked lighting). When objects are
// protected, preserve existing object anchors, add no prefab objects, and update
// terrain-only road/blocker markers where there is no object anchor.
struct StampLayers { bool objects=true; };
bool stampSection(tak::tnt::Map& map, const tak::tnt::Map& section, int bx, int by,
                  StampLayers layers={});

} // namespace cart
