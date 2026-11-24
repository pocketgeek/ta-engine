#pragma once

// Cartographer's section-prefab palette + stamp brush (docs/cartographer-port.md).
// A "section" is a 512px (32x32 cell / 16x16 block) prefab .tnt in sections.hpi
// under Sections/<World>/<Category>/<name>.TNT. Painting = snapping to the block
// grid and copying a section's cells into the map -- the retail brush model.

#include "tnt/tnt.h"

#include <map>
#include <string>
#include <unordered_map>
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
    // Scan Sections/<world>/** for prefab .TNTs (world = a TA planet or its sections/ folder).
    void scan(const ta::hpi::Vfs& vfs, const std::string& world);
    const std::vector<SectionRef>& list() const { return sections_; }
    // Load (and cache) a prefab by VFS path; nullptr if it won't parse.
    const ta::tnt::Map* load(const ta::hpi::Vfs& vfs, const std::string& path);
    // Bumped by scan(), which frees every map load() returned.
    unsigned generation() const { return generation_; }

private:
    unsigned generation_ = 0;
    std::vector<SectionRef> sections_;
    std::map<std::string, ta::tnt::Map> cache_;
};

// Stamp `section` into `map` with its top-left block at (bx, by), clipped to the
// map. Copies the tile plane (keys/cols/rows) + heights, and the feature plane
// REMAPPED by name (a prefab's feature indices point into ITS own name table, so
// each is resolved to a name and re-interned in map.featureNames; the 0xFFFF/
// 0xFFFB/0xFFFC sentinels pass through). Returns false if fully off-map.
// Art and height are coupled (cliff art has baked lighting). When objects are
// protected, preserve existing object anchors, add no prefab objects, and update
// terrain-only road/blocker markers where there is no object anchor.
// TA maps carry their own art: 32x32 8-bit tiles (tileGfx) that blocks index
// (tileIndex, mirrored into tileKeys for the compositor). The interner finds a
// tile already in the map by content, or appends it, so edits that bring tiles
// in (stamps, pastes, mirrored art) share one copy of each.
class TileInterner {
public:
    explicit TileInterner(ta::tnt::Map& map);
    // The map's index of a tile with these 1024 pixels, appended when new. A map
    // already holding 0xFFFF tiles gets 0 for a new one.
    uint16_t intern(const uint8_t* pixels);
private:
    ta::tnt::Map& map_;
    std::unordered_multimap<uint64_t, uint16_t> index_;
};
// True when `map` draws from its own tiles (every TA map; Kingdoms maps key JPGs).
inline bool ownTiles(const ta::tnt::Map& map) {
    return map.tileCount > 0 || (!map.tileIndex.empty() && map.tileIndex.size() == size_t(map.blocksX) * map.blocksY);
}
// Point block `block` at tile `tile`: tileIndex and the tileKeys/cols/rows mirror
// the compositor and the section tools read.
void setBlockTile(ta::tnt::Map& map, size_t block, uint16_t tile);
// Size the TA tile plane (tileIndex + mirror) to the map's blocks, tile 0 everywhere.
void ensureTilePlane(ta::tnt::Map& map);
// Drop tiles no block uses and renumber the rest in their old order. Edits that
// replace art (a symmetrize, a crop) leave unused tiles behind.
void compactTiles(ta::tnt::Map& map);

struct StampLayers { bool objects=true; };
bool stampSection(ta::tnt::Map& map, const ta::tnt::Map& section, int bx, int by,
                  StampLayers layers={});

} // namespace cart
