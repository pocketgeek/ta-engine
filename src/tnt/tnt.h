#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ta::tnt {

// Total Annihilation's TNT map format (version 0x2000), read out of the retail
// maps and cross-checked so that every byte of the file is accounted for.
//
// A map is W x H cells of 16px. Terrain art is a TILE LIBRARY carried inside the
// .tnt itself: `numTiles` 32x32 8-bit indexed tiles, with one u16 index per 32px
// tile position. (Kingdoms later replaced this with content-addressed truecolour
// JPG sections in a shared terrain archive -- see the 0x4000 notes in git
// history. Nothing but the name survives between the two.)
//
// Header, 16 little-endian u32 words:
//    0: version (0x2000)
//    1: width in 16px cells        2: height in 16px cells
//    3: -> tile indices  (u16 per 32px tile, (W/2) x (H/2))
//    4: -> MapAttr       (4 bytes per 16px CELL -- see below)
//    5: -> tile graphics (numTiles * 1024 bytes)
//    6: numTiles                   7: number of feature names
//    8: -> feature names (132 bytes each: u32 index + 128-byte name)
//    9: sea level (cells at or below this are water)
//   10: -> minimap {u32 w, u32 h, w*h bytes}
//   11: unknown (1 in every retail map seen)
//   12-15: zero padding
//
// MapAttr, per 16px cell, is { u8 height; u16 feature; u8 unused } -- note the
// u16 is UNALIGNED at offset 1, which is why it cannot be read as a struct.
// Confirmed against real cells rather than inferred: a cell carrying feature
// index 1 reads [86, 1, 0, 0].
//
// Word 8's table is named "tile anims" in some third-party format notes; it is
// not. It is the FEATURE name table -- Coast To Coast's 19 entries are
// ArchMetal1/2/3, Palm01-06 and Frond01-07, and every index the feature plane
// uses resolves inside it.

// Feature-plane sentinels. A normal value indexes Map::featureNames.
constexpr uint16_t kNoFeature = 0xFFFF;
// The cell is inside a multi-cell feature whose ANCHOR cell holds the real
// index. Retail TA stores this in the file; Kingdoms instead derived the same
// state at load time, which is the one place the two formats' spare feature
// values genuinely diverge. A 3x3 metal patch is one anchor plus eight of these.
constexpr uint16_t kFeatureCovered = 0xFFFE;

constexpr int kTileBytes = 32 * 32;   // one 8-bit indexed tile

struct Map {
    int width = 0, height = 0;       // in 16px cells
    int seaLevel = 0;                // cells at or below this are water
    int blocksX = 0, blocksY = 0;    // in 32px tiles (width/2, height/2)

    std::vector<uint8_t> heights;    // width*height (MapAttr byte 0)
    // Per-cell feature plane: an index into featureNames, or one of the
    // sentinels above. This doubles as TA's METAL map -- a metal patch is just a
    // feature (ArchMetal1/2/3), so an extractor's yield is a property of the
    // feature under its footprint rather than of a separate density plane.
    std::vector<uint16_t> features;  // width*height
    std::vector<uint16_t> tiles;     // blocksX*blocksY: index into the tile library
    std::vector<uint8_t> tileGfx;    // numTiles * kTileBytes, 8-bit palette indices
    int numTiles = 0;

    int minimapW = 0, minimapH = 0;
    std::vector<uint8_t> minimap;    // 8-bit indexed (252x252 in retail maps)
    std::vector<std::string> featureNames;   // indexed by feature-plane values

    // Pointer to tile `index`'s 1024 bytes, or nullptr if out of range. Callers
    // composite terrain through this rather than indexing tileGfx themselves --
    // a map can reference a tile the library does not hold.
    const uint8_t* tile(int index) const {
        if (index < 0 || index >= numTiles) return nullptr;
        size_t off = size_t(index) * kTileBytes;
        return off + kTileBytes <= tileGfx.size() ? &tileGfx[off] : nullptr;
    }

    static Map load(const std::filesystem::path& file);
    // Parse from an in-memory buffer (a VFS-resolved archive entry). `origin`
    // names the source in error messages.
    static Map load(const std::vector<uint8_t>& d, const std::string& origin = "<memory>");

    // Serialize back to the retail TNT byte layout. Emits the planes in the
    // physical order retail writes them (tiles, MapAttr, tile graphics, feature
    // names, minimap), so a load->save round trip is byte-identical for a map
    // that came in unmodified.
    std::vector<uint8_t> save() const;
};

} // namespace ta::tnt
