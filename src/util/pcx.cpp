#include "util/pcx.h"

namespace ta::pcx {
namespace {

uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8); }

}  // namespace

Image load(const std::vector<uint8_t>& d, const std::string&) {
    Image img;
    // 128-byte header, then RLE pixel data, then 769 bytes of palette
    // (a 0x0C marker + 256 RGB triples).
    constexpr size_t kHeader = 128, kPalette = 769;
    if (d.size() < kHeader + kPalette) return img;
    if (d[0] != 0x0A) return img;                    // not a PCX
    if (d[2] != 1) return img;                       // only RLE encoding ships
    if (d[3] != 8 || d[65] != 1) return img;         // only 8bpp, single plane

    const int xmin = u16(&d[4]), ymin = u16(&d[6]);
    const int xmax = u16(&d[8]), ymax = u16(&d[10]);
    const int w = xmax - xmin + 1, h = ymax - ymin + 1;
    const int bytesPerLine = u16(&d[66]);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || bytesPerLine < w) return img;

    // The palette sits at the END of the file, so its marker byte is what
    // confirms this really is a 256-colour PCX rather than a truncated one.
    const size_t palOff = d.size() - kPalette;
    if (d[palOff] != 0x0C) return img;
    const uint8_t* pal = &d[palOff + 1];

    std::vector<uint8_t> idx(size_t(w) * size_t(h), 0);
    size_t p = kHeader;
    for (int y = 0; y < h; ++y) {
        int x = 0;                                   // decoded bytes on this line
        while (x < bytesPerLine) {
            if (p >= palOff) return img;             // ran into the palette: truncated
            uint8_t b = d[p++];
            int run = 1;
            if ((b & 0xC0) == 0xC0) {                // top two bits set = run length
                run = b & 0x3F;
                if (p >= palOff) return img;
                b = d[p++];
            }
            for (int i = 0; i < run && x < bytesPerLine; ++i, ++x)
                if (x < w) idx[size_t(y) * size_t(w) + size_t(x)] = b;
        }
    }

    img.width = w;
    img.height = h;
    img.rgba.assign(size_t(w) * size_t(h) * 4, 0);
    for (size_t i = 0; i < idx.size(); ++i) {
        const uint8_t* c = &pal[size_t(idx[i]) * 3];
        img.rgba[i * 4 + 0] = c[0];
        img.rgba[i * 4 + 1] = c[1];
        img.rgba[i * 4 + 2] = c[2];
        img.rgba[i * 4 + 3] = 255;
    }
    return img;
}

}  // namespace ta::pcx
