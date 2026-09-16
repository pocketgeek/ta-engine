#pragma once

// Cartographer's section-prefab palette + stamp brush (docs/cartographer-port.md).
// A "section" is a 512px (32x32 cell / 16x16 block) prefab laid into the map by
// snapping to the block grid and copying its cells -- the retail brush model.
//
// TA keeps its prefabs in worlds.hpi under the same sections/<World>/<Category>/
// <name> layout, but in a ".sct" container rather than a TNT (see sct.h for the
// two versions of it). scan() accepts both extensions and load() decodes either
// into a tnt::Map, so stampSection -- which is format-independent -- keeps
// working between any two loaded maps.

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
    // Scan sections/<world>/** for prefabs (.sct in TA, .tnt in Kingdoms).
    void scan(const ta::hpi::Vfs& vfs, const std::string& world);
    const std::vector<SectionRef>& list() const { return sections_; }
    // The worlds this install actually ships, read off the sections/ tree rather
    // than from a fixed list. TA's are Archipelago, GreenWorld, Lava, Mars, Metal
    // and MOON; Kingdoms' were the five houses. Hardcoding either set makes the
    // editor unusable against the other, and makes a mod's world invisible.
    // Returned lowercased (VFS keys are) and sorted, so the order is stable.
    static std::vector<std::string> worlds(const ta::hpi::Vfs& vfs);
    // Load (and cache) a prefab by VFS path, in either container; nullptr if it
    // won't parse.
    const ta::tnt::Map* load(const ta::hpi::Vfs& vfs, const std::string& path);

private:
    std::vector<SectionRef> sections_;
    std::map<std::string, ta::tnt::Map> cache_;
};

// Stamp `section` into `map` with its top-left block at (bx, by), clipped to the
// map. Copies heights, and REMAPS both index planes into the destination's own
// tables: tiles are interned into map.tileGfx by content, features by name into
// map.featureNames (the 0xFFFF/0xFFFE sentinels pass through). Both planes index
// per-map tables, so copying either index verbatim would corrupt the stamp.
// Returns false if fully off-map.
bool stampSection(ta::tnt::Map& map, const ta::tnt::Map& section, int bx, int by);

} // namespace cart
