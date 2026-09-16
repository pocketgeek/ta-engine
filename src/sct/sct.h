#pragma once

#include "tnt/tnt.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ta::sct {

// worlds.hpi's ".sct" section prefabs -- the editor's stamp brush palette.
//
// A section is a close relative of the TNT: the same 32px tile library, the same
// per-16px-cell attribute plane, plus a fixed 128x128 thumbnail for the palette.
// It is NOT a TNT, though: the header is 7 or 8 words rather than 16, the planes
// sit in a different order, and their sizes are derived rather than pointed at.
// Two versions ship, and they differ in more than a number.
//
//   version 2 (607 of the 837 shipped sections)
//     u32 0: version = 2
//     u32 1: -> thumbnail
//     u32 2: numTiles
//     u32 3: -> tile graphics (always 28, i.e. immediately after the header)
//     u32 4: width  in 32px TILES        u32 5: height in 32px tiles
//     u32 6: -> tile indices (== 28 + numTiles*1024)
//
//   version 3 (the other 230) -- the graphics move to the BACK and the planes to
//   the front, so the two headers cannot be read with the same field order:
//     u32 0: version = 3
//     u32 1: -> thumbnail
//     u32 2: numTiles
//     u32 3: -> tile graphics
//     u32 4: width in tiles              u32 5: height in tiles
//     u32 6: -> tile indices (always 28)
//     u32 7: flags (65536 in 223 of them; 131073 in 6; 16908545 in one)
//
// After the tile indices (one u16 per tile, row-major) comes the attribute plane:
// one record per 16px CELL, flat and row-major across the whole section -- NOT
// grouped by tile. That was worth establishing rather than assuming, because the
// per-tile grouping is the same size to the byte; the flat reading produces
// terrain with a little under half the total height variation of either per-tile
// ordering across every shipped section, which is the difference between smooth
// hillsides and a field sheared by one cell per row.
//
// The record is 4 bytes in version 3 -- byte-identical to the TNT's MapAttr,
// { u8 height; u16 feature unaligned at byte 1; u8 unused }, with the feature
// holding 0xFFFF everywhere, as befits a prefab that places none.
//
// In version 2 it is 8 bytes, and only byte 0 (the height) is determinable: the
// other seven hold 01 FF 00 00 00 00 00 in all 664,144 cell records of all 607
// v2 sections, so nothing in the shipped data distinguishes their meaning. They
// are read as opaque and not guessed at.
//
// Sections carry no sea level; the loader leaves it 0. Nothing downstream needs
// it -- stampSection copies heights, tiles and features, and the destination map
// owns the sea level.

// Decode a section into a tnt::Map, so the stamp brush and the rest of the
// cartographer can treat a prefab and a map as the same kind of thing.
// `origin` names the source in error messages. Throws on a malformed file.
tnt::Map load(const std::vector<uint8_t>& d, const std::string& origin = "<memory>");

// Does this look like a section container (by extension)? Case-insensitive.
bool isSectionPath(const std::string& path);

} // namespace ta::sct
