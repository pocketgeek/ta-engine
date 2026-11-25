#pragma once

// TA's under-construction look (the "nanoframe"), TotalA.exe 0x458dd0.
//
// Retail draws the unit as usual (textured, into an off-screen object bitmap
// whose byte z-buffer holds each pixel's HEIGHT: model-up y >> 16, plus 50 --
// 125 for a digger, FBI `digger` -- see the vertex set-up at
// 0x458fff..0x459051; the byte wraps, so model heights past 205 (130 for a
// digger) come round under the low ones), every pixel kept where its byte is
// not below the buffer's (client/nanoraster.h, which GameView::buildNanoframe
// rasterises with), then:
//
//  1. 0x458d30 recolours every covered pixel by that height n against a cut
//     level L (one byte), in three bands:
//        n <  L-4        -> `below`  (L-4 clamps at 0, 0x458d38)
//        L-4 <= n < L    -> `band`
//        n >= L          -> `above`
//     where each band is either keep the textured pixel (-1), make it
//     transparent (-2), or a flat palette index.
//  2. 0x458fa0 outlines every primitive of every visible piece (primitive 0
//     excepted when the piece has a selection primitive, 0x45908a: the load
//     swapped the selection primitive there, 0x4cb370 -- client/primitivedraw.h,
//     which also gives the drawing order) in the
//     second shimmer green, depth-tested against the same height buffer and
//     writing it (span edges only, 0x4c0820 / 0x4c0a90: the left and right
//     pixel of each row, a polygon facing away drawing none).
//
// The stage and L come from v = ftol(unit+0x104 build-remaining * 255.0)
// (0x458e8e..0x458f6e); the two shimmer greens are triangle waves over the
// palette's green ramp 160..175 driven by the game tick (0x458e11..0x458e8c).
// 0x458dd0 returns at once for a finished unit (remaining == 0.0).

#include <algorithm>
#include <cstdint>

namespace ta::nanoframe {

constexpr int kKeep = -1;          // keep the textured pixel
constexpr int kTransparent = -2;   // clear the pixel to the bitmap's transparent key
constexpr int kHeightBias = 50;    // z-buffer height offset (0x459a3c: 0x32)
constexpr int kDiggerHeightBias = 125;   // a digger type's (0x459a39: 0x32 + 0x4b)

struct Bands {
    int level = 0;                 // L, 0..255
    int below = kKeep, band = kKeep, above = kKeep;   // palette index or kKeep/kTransparent
};

// The palette index of a shimmer green: phase bit 4 runs the ramp back down
// (0x458e56..0x458e8c), so it is a 32-step triangle wave over 160..175.
inline int shimmerIndex(uint32_t phase) {
    return (phase & 0x10) ? 0xaf - int(phase & 0xf) : 0xa0 + int(phase & 0xf);
}

// Colours `first` (esi) and `second` (ebx) for unit id `unitIndex` (unit+0xa8)
// at game tick `tick` (0x511de8+0x38a47).
inline void shimmer(uint32_t unitIndex, uint32_t tick, int& first, int& second) {
    const uint32_t id = unitIndex & 0xffff;
    first = shimmerIndex((id ^ 5) + uint32_t(uint64_t(tick) * 33 / 30));
    second = shimmerIndex((id ^ 9) + uint32_t(uint64_t(tick) * 57 / 30));
}

// The stage for build-remaining `remaining` (unit+0x104), with the shimmer
// greens `first`/`second`. Stages, as the unit goes up:
//   v 255..236  a `first` band scans down the empty frame        (0x458eaa)
//   v 235..201  the same scan again, slower                       (0x458ed1)
//   v 200..116  solid `first` rises from the bottom, `second` cut, nothing above (0x458efc)
//   v 115..31   texture rises under a `second` cut, solid `first` above        (0x458f28)
//   v  30..1    textured, a last `first` band scans down          (0x458f4f)
// (The wireframe in `second` is drawn over every stage.)
inline Bands bands(float remaining, int first, int second) {
    const int v = int(remaining * 255.0f);   // _ftol: truncation
    Bands b;
    if (v > 235) {
        b.level = ((v - 235) * 255 / 20) & 0xff;
        b.below = kTransparent; b.band = first; b.above = kTransparent;
    } else if (v > 200) {
        b.level = ((v - 200) * 255 / 35) & 0xff;
        b.below = kTransparent; b.band = first; b.above = kTransparent;
    } else if (v > 115) {
        b.level = ((115 - v) * 255 / 85 - 1) & 0xff;   // `dec dl`: the low byte only
        b.below = first; b.band = second; b.above = kTransparent;
    } else if (v > 30) {
        b.level = ((30 - v) * 255 / 85 - 1) & 0xff;
        b.below = kKeep; b.band = second; b.above = first;
    } else {
        b.level = (v * 255 / 30) & 0xff;
        b.below = kKeep; b.band = first; b.above = kKeep;
    }
    return b;
}

// The two cut heights (z-buffer units) separating below/band/above.
inline int lowerCut(const Bands& b) { return b.level >= 4 ? b.level - 4 : 0; }

} // namespace ta::nanoframe
