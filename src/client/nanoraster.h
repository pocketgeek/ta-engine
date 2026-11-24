#pragma once

// TotalA.exe's object-bitmap polygon walk, as the under-construction draw
// (client/nanoframe.h) uses it. Pure: no SDL.
//
// Retail draws a unit that is being built into an off-screen bitmap with a
// byte z-buffer that holds each pixel's HEIGHT (model-up y, whole pixels, plus
// a bias), not its depth. Every polygon is walked the same way, the textured
// one (0x4c8760 -> spans 0x4c7a20) and the outline (0x4c0820 -> 0x4c0a90):
//
//  * vertices are whole pixels: x = X>>16, y = -(Z>>16) - ((Y>>16)>>1),
//    height = (Y>>16) + bias (0x459027..0x459051, 0x4599bb..0x459a6e);
//  * the rows run from the smallest y (the first vertex holding it) up to,
//    not including, the largest; the left edge is the chain walked BACKWARD
//    from the top vertex, the right edge the chain walked forward
//    (0x4c08bb / 0x4c0981), each edge starting at x<<16 + 0xffff and stepping
//    by the truncated (dx<<16)/dy, the height and texture coordinates likewise
//    from v<<16;
//  * a row is drawn only when right - left > 0 (0x4c0a5a, 0x4c8b76), so a
//    polygon wound the other way on screen draws nothing: the walk is the cull;
//  * every pixel compares heights as BYTES: it is drawn when the buffer's byte
//    is not above its own (`cmp bl, cl; ja skip`, 0x4c0acf; 0x4c7b53), and
//    then writes its byte. Heights above 255 wrap (a bias of 50 leaves model
//    heights 0..205 in order).
//
// The textured span fills [left, right); the outline draws only the two end
// pixels of each row, left and right (0x4c0a90), so a polygon's top and bottom
// edges show only where they slope.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ta::nanoframe {

struct RasterVertex {
    int32_t x = 0, y = 0;   // whole screen pixels
    int32_t h = 0;          // height (biased), not yet wrapped
    int32_t u = 0, v = 0;   // texel corner
};

struct RasterRow {
    int32_t left = 0, right = 0;          // x: left edge, right edge
    int32_t hl = 0, hr = 0;               // 16.16 heights at the two edges
    int32_t ul = 0, vl = 0, ur = 0, vr = 0;   // 16.16 texel coordinates
};

// Walk a polygon of n vertices; row(y, RasterRow) for every row whose right
// edge is past its left (retail's spans).
template <class Row>
inline void walkPolygon(const RasterVertex* v, int n, std::vector<RasterRow>& rows, Row&& row) {
    if (n < 2) return;
    int top = 0, bottom = 0;
    int32_t ymin = 999999, ymax = -999999;   // 0xf423f / 0xfff0bdc1
    for (int i = 0; i < n; ++i) {
        if (v[i].y < ymin) { ymin = v[i].y; top = i; }
        if (v[i].y > ymax) { ymax = v[i].y; bottom = i; }
    }
    if (ymax == ymin) return;
    rows.assign(size_t(ymax - ymin), RasterRow{});
    const auto chain = [&](int step, bool left) {
        int i = top;
        size_t r = 0;
        while (i != bottom) {
            int j = i + step;
            if (j < 0) j = n - 1;
            if (j >= n) j = 0;
            const RasterVertex& a = v[i];
            const RasterVertex& b = v[j];
            if (a.y < b.y) {
                const int32_t dy = b.y - a.y;
                const int32_t dx = int32_t((int64_t(b.x - a.x) << 16) / dy);
                const int32_t dh = int32_t(((int64_t(b.h) << 16) - (int64_t(a.h) << 16)) / dy);
                const int32_t du = int32_t(((int64_t(b.u) << 16) - (int64_t(a.u) << 16)) / dy);
                const int32_t dv = int32_t(((int64_t(b.v) << 16) - (int64_t(a.v) << 16)) / dy);
                int32_t x = int32_t((uint32_t(a.x) << 16) + 0xffffu);
                int32_t h = int32_t(uint32_t(a.h) << 16), u = int32_t(uint32_t(a.u) << 16),
                        w = int32_t(uint32_t(a.v) << 16);
                for (int32_t k = 0; k < dy && r < rows.size(); ++k, ++r) {
                    RasterRow& out = rows[r];
                    if (left) { out.left = x >> 16; out.hl = h; out.ul = u; out.vl = w; }
                    else { out.right = x >> 16; out.hr = h; out.ur = u; out.vr = w; }
                    x += dx; h += dh; u += du; w += dv;
                }
            }
            i = j;
        }
    };
    chain(-1, true);
    chain(+1, false);
    for (size_t r = 0; r < rows.size(); ++r)
        if (rows[r].right - rows[r].left > 0) row(ymin + int32_t(r), rows[r]);
}

// The byte a 16.16 height leaves in the z-buffer.
inline uint8_t heightByte(int32_t h16) { return uint8_t(uint32_t(h16 >> 16) & 0xffu); }

} // namespace ta::nanoframe
