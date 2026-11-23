#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ta::tnt {

// Total Annihilation TNT map format (version 0x2000), read from TotalA.exe's
// loader (0x483610) and checked against every shipped map. A map is W x H
// cells of 16px; graphics are 32x32 8-bit palette tiles stored IN the map.
//
// Header (u32 little-endian, offsets relative to the file start):
//   +0x00 version 0x2000          +0x04 width (16px cells)  +0x08 height
//   +0x0c -> tile map (u16 tile index per 32px block, (W/2)x(H/2))
//   +0x10 -> cell attributes (4 B/cell: u8 height, u16 feature, u8 unused)
//   +0x14 -> tile graphics (count x 1024 bytes)   +0x18 tile count
//   +0x1c feature-name count      +0x20 -> feature names (132 B: u32 idx, char[128])
//   +0x24 sea level               +0x28 -> minimap {u32 w, u32 h, w*h bytes}
//   +0x2c bit 0: minimap present ("TED GENERATED PIC")
// Feature values >= 0xFFFB are not feature indices (0x483b2e); 0xFFFC is placed
// as an impassable void marker (0x483aca), 0xFFFF = empty. Retail also accepts an
// older 0x1020 layout (0x48363c) that no shipped map uses.

// TA's planet= names (OTA [GlobalHeader]) onto the features/ and sections/
// folders: "Green planet" -> green / greenworld, "Red Planet" -> mars, ... A name
// that is already a folder (or unknown) comes back lowercased.
std::string taFeatureFolder(const std::string& planet);
std::string taSectionFolder(const std::string& planet);

struct Map {
    // Runtime render provenance, not part of the TNT file format. Generated maps
    // use retail sections; unrelated downloaded-map reskins must not override them.
    bool stockTerrain = false;
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
    std::vector<uint16_t> tileIndex; // blocksX*blocksY: index into tileGfx
    std::vector<uint8_t> tileGfx;    // tileCount * 1024 (32x32, 8-bit palette)
    int tileCount = 0;
    std::vector<uint8_t> cellExtra;  // width*height: 4th attribute byte (unused by retail)
    // Legacy TAK section-key planes (unused for TA maps; kept empty until the
    // section-based tools are ported).
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
    // A TA terrain section (worlds.hpi sections/<world>/**/*.sct, v2): its own
    // tiles, tile map and per-cell heights, as a Map with no features.
    static Map loadSct(const std::vector<uint8_t>& d, const std::string& origin = "<memory>");

    // Serialize to the retail TNT 0x2000 byte layout (round-trips a loaded map):
    // 64-byte header, tile map, attributes, tile graphics, feature names, minimap.
    std::vector<uint8_t> save() const;
};

} // namespace ta::tnt
