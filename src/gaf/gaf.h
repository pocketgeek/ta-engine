#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ta::gaf {

// GAF/TAF sprite banks. Both use the same container (version 0x00010100):
//
//   u32 version, u32 numEntries, u32 unused, u32 entryPtr[numEntries]
//   entry: u16 numFrames, u16 unk, u32 unk, char name[32],
//          { u32 framePtr, u32 unk } × numFrames
//   frame: u16 w, u16 h, s16 xoff, s16 yoff, u8 transparencyIndex,
//          u8 encoding, u16 numSubframes, u32 unk, u32 dataPtr, u32 unk
//
// encoding: 0 = raw 8-bit indexed, 1 = RLE 8-bit indexed (classic TA),
//           4 = raw ARGB4444 (TAF), 5 = raw ARGB1555 (TAF).
// numSubframes > 0: dataPtr points at u32 framePtr[numSubframes]; the frame
// is the composite of its subframes aligned by their offsets.

// 256-entry palette; entries are 4 bytes (R,G,B,x) in .pal files.
struct Palette {
    uint8_t rgba[256][4];
    static Palette load(const std::filesystem::path& palFile);
    static Palette fromBytes(const std::vector<uint8_t>& d, const std::string& origin = "<memory>");
};

struct Frame {
    int width = 0, height = 0;
    int xoff = 0, yoff = 0;          // anchor point within the frame
    int delayTicks = 2;              // display duration in 30Hz engine ticks (the u32
                                     // after each frame pointer; waves ship 2 -> 15fps)
    std::vector<uint8_t> rgba;       // width*height*4
};

struct Sequence {
    std::string name;
    std::vector<Frame> frames;
};

// Decode a whole GAF/TAF file. `pal` is used for 8-bit entries; TAF
// truecolor entries ignore it. `transparentIndex` >= 0 makes that palette
// index fully transparent (render.tdf: transparentcolor=5 for 3DO textures).
std::vector<Sequence> load(const std::filesystem::path& file, const Palette& pal,
                           int transparentIndex = -1);
// Decode from an in-memory buffer (a VFS-resolved archive entry).
std::vector<Sequence> load(const std::vector<uint8_t>& d, const Palette& pal,
                           int transparentIndex = -1, const std::string& origin = "<memory>");

} // namespace ta::gaf
