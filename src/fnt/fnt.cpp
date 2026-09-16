#include "fnt/fnt.h"

#include <stdexcept>

namespace ta::fnt {

namespace {

constexpr size_t kHeaderBytes = 4 + 256 * 2;   // height, unknown, offset[256]

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

}  // namespace

int Font::measure(const std::string& text) const {
    int w = 0;
    for (unsigned char c : text)
        if (glyphs[c].present) w += glyphs[c].width;
    return w;
}

void Font::blit(const std::string& text, std::vector<uint8_t>& dst, int dstW, int dstH,
                int x, int y) const {
    if (dstW <= 0 || dstH <= 0) return;
    int pen = x;
    for (unsigned char c : text) {
        const Glyph& g = glyphs[c];
        if (!g.present) continue;
        for (int row = 0; row < height; ++row) {
            int dy = y + row;
            if (dy < 0 || dy >= dstH) continue;
            for (int col = 0; col < g.width; ++col) {
                int dx = pen + col;
                if (dx < 0 || dx >= dstW) continue;
                if (g.bits[size_t(row) * size_t(g.width) + size_t(col)])
                    dst[size_t(dy) * size_t(dstW) + size_t(dx)] = 1;
            }
        }
        pen += g.width;
    }
}

Font parse(const std::vector<uint8_t>& d, const std::string& origin) {
    if (d.size() < kHeaderBytes)
        throw std::runtime_error(origin + ": too short for a .FNT header");

    Font f;
    f.height = int(u16(&d[0]));
    // Every shipped font is 11 or 12 high. A zero or absurd height means this is
    // not a .FNT -- better to say so than to allocate a glyph of that size.
    if (f.height <= 0 || f.height > 64)
        throw std::runtime_error(origin + ": implausible font height " +
                                 std::to_string(f.height));

    for (int c = 0; c < 256; ++c) {
        size_t off = u16(&d[4 + size_t(c) * 2]);
        if (off == 0 || off >= d.size()) continue;   // 0 = character not in this font
        int w = int(d[off]);
        if (w <= 0 || w > 64) continue;              // skip a glyph we cannot trust
        // Packed 1bpp, width*height bits, rows NOT byte-aligned.
        size_t need = (size_t(w) * size_t(f.height) + 7) / 8;
        if (off + 1 + need > d.size()) continue;     // truncated: skip, do not throw

        Glyph& g = f.glyphs[c];
        g.width = w;
        g.present = true;
        g.bits.assign(size_t(w) * size_t(f.height), 0);
        const uint8_t* src = &d[off + 1];
        for (size_t bit = 0; bit < size_t(w) * size_t(f.height); ++bit)
            g.bits[bit] = uint8_t((src[bit >> 3] >> (7 - (bit & 7))) & 1);
    }
    return f;
}

} // namespace ta::fnt
