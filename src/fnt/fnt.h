#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ta::fnt {

// Total Annihilation's `.FNT` bitmap fonts (fonts/ARMBUTT.FNT, CONSOLE.FNT, ...).
// SIDEDATA names two per side -- `font=` and `fontgui=` -- and the .gui files
// name more through a type-7 gadget's `filename=`.
//
// Layout, decoded from the shipped fonts rather than from format notes:
//
//   u16 height          pixel height, shared by every glyph (11 in most)
//   u16 unknown         1 in every shipped font
//   u16 offset[256]     byte offset of each character's glyph; 0 = not present
//   ...glyphs...
//
// A glyph is a width byte followed by a PACKED 1-bit-per-pixel bitmap of
// width x height bits, MSB first, row-major -- and rows are NOT byte-aligned, so
// a 7-wide glyph spends 77 bits in 10 bytes with rows straddling byte
// boundaries. (That packing is what makes the per-glyph stride vary with width:
// 4 bytes for '!' at width 2, 12 for 'A' at width 8.)
//
// Colour is the caller's: a glyph is a coverage mask, and retail draws it in
// whatever palette index the gadget's `colorf` asks for.

struct Glyph {
    int width = 0;
    std::vector<uint8_t> bits;   // width*height, 1 byte per pixel: 0 or 1
    bool present = false;
};

struct Font {
    int height = 0;
    Glyph glyphs[256];

    // Rendered width of `text`, skipping characters the font does not define.
    int measure(const std::string& text) const;
    // Blit `text` into an 8-bit coverage buffer `dst` of `dstW` x `dstH`, with the
    // glyph's top-left at (x, y). Pixels outside the buffer are dropped.
    void blit(const std::string& text, std::vector<uint8_t>& dst, int dstW, int dstH,
              int x, int y) const;
};

// Parse a .FNT. Throws std::runtime_error if the bytes are not one.
Font parse(const std::vector<uint8_t>& bytes, const std::string& origin = "<memory>");

} // namespace ta::fnt
