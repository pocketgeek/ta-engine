#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ta::tnt {

// TAK TNT map format (version 0x4000), reverse-engineered from the GOG data.
// A map is W x H cells of 16px. Terrain graphics come from *content-addressed*
// JPG section images in terrain.hpi (terrain/<hexkey>.jpg): the map carries,
// per 32px block (W/2 x H/2 grid), a u32 JPG key plus column/row bytes
// selecting the 32px piece of that JPG.
//
// Header (u32 little-endian):
//   0: version (0x4000)      1: width in cells      2: height in cells
//   3: unknown               4: -> heights (1 B/cell)
//   5: -> features (u16/cell, 0xFFFF = none)   6,7: unknown
//   8: -> jpg keys (u32/block)   9: -> columns (1 B/block)
//  10: -> rows (1 B/block)      11: -> minimap {u32 w, u32 h, w*h bytes}
//  12: -> overview image {u32 w, u32 h, w*h bytes}

struct Map {
    int width = 0, height = 0;       // in 16px cells
    int seaLevel = 0;                // heights below this are water
    int blocksX = 0, blocksY = 0;    // in 32px blocks (width/2, height/2)

    std::vector<uint8_t> heights;    // width*height
    // Per-cell feature plane. Normal values index featureNames; retail also
    // packs per-cell ATTRIBUTES in as special values (icd map loader 0x50f0f0):
    //   0xFFFF = empty
    //   0xFFFB = ROAD cell (roadmultiplier speed bonus, COB walk_road gait;
    //            retail converts it to a cell flag at load)
    //   0xFFFC = hard BLOCKER (impassable + no building; under wall/gate art)
    //   0xFFFD / 0xFFFE never appear in files -- retail generates them at
    //   runtime (border margin / covered-by-multi-cell-feature).
    std::vector<uint16_t> features;  // width*height
    std::vector<uint32_t> tileKeys;  // blocksX*blocksY
    std::vector<uint8_t> tileCols;   // blocksX*blocksY
    std::vector<uint8_t> tileRows;   // blocksX*blocksY

    int minimapW = 0, minimapH = 0;
    std::vector<uint8_t> minimap;    // 8-bit indexed (small, header word 11; 126x126)
    int overviewW = 0, overviewH = 0;
    std::vector<uint8_t> overview;   // 8-bit indexed (large, header word 12; per-block)
    std::vector<std::string> featureNames;   // indexed by feature-layer values

    static Map load(const std::filesystem::path& file);
    // Parse from an in-memory buffer (a VFS-resolved archive entry). `origin`
    // names the source in error messages.
    static Map load(const std::vector<uint8_t>& d, const std::string& origin = "<memory>");

    // Serialize to the retail TNT byte layout (version 0x4000). Round-trips a
    // loaded map (Cartographer save format, RE'd from Cartographer.exe 0x41ba70):
    // 52-byte header + heights/features/feature-names/keys/cols/rows/small- and
    // large-minimap sections in that physical order. See docs/cartographer-port.md.
    std::vector<uint8_t> save() const;
};

} // namespace ta::tnt
