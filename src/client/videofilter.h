// videofilter -- deblocking for the Bink clips.
//
// The videos are 640x360 at 15 fps and get stretched across the whole window: on a
// 7680-wide display that is about 12x horizontally. At that magnification the limit on
// how they look is NOT the pixel count, it is the 1999 compression -- Bink's 8x8
// transform blocks leave small steps at every block boundary, and stretching magnifies
// each step into a visible seam.
//
// Sharpening is exactly the wrong instinct here, and that is measured rather than
// assumed: running the static-art edge-directed upscaler over a decoded frame took the
// blockiness ratio (mean luma step across block boundaries / mean step elsewhere) from
// 1.276 to 2.697. The filter cannot tell a block seam from a real edge, so it faithfully
// reconstructs the artifacts. Anything that reinforces edges will do the same.
//
// So: smooth ACROSS the block boundaries instead, and only where the step looks like an
// artifact. The test is the one a deblocking filter always uses -- an artifact is a
// SMALL step between two FLAT neighbourhoods, while a real edge is either a large step
// or sits in busy detail. Both sides must be flat and the step must be small, or the
// pixels are left alone. That is what keeps it from turning the picture to soup.
//
// Runs on the decoded 640x360 frame before upload, which is ~1 ms against a 66 ms budget
// at 15 fps. Nothing here scales with window size.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace tak::video {

// On/off, sampled from Settings::videoDeblock. A global for the same reason
// art::g_smoothArt is one: the door-video path (MainMenu::setDoorTex) has no settings
// pointer to hand, and threading one through every Bink call site to carry a single bool
// would be worse than this. Refreshed wherever a clip can START -- app startup, entering
// the menu, opening the load screen -- so the toggle reaches the next clip rather than
// needing a restart.
inline bool g_deblock = false;
inline void setDeblock(bool on) { g_deblock = on; }

// Deblock an RGBA32 frame in place. `strength` 1..3 widens the "this is an artifact"
// window; 0 disables. Block size is 8, Bink's transform size.
inline void deblock(std::vector<uint8_t>& px, int w, int h, int strength) {
    if (strength <= 0 || w < 4 || h < 4) return;
    if (px.size() < size_t(w) * size_t(h) * 4) return;
    const int alpha = 6 * strength;   // largest step still considered an artifact
    const int beta  = 3 * strength;   // largest variation still considered "flat"
    auto at = [&](int x, int y) { return &px[(size_t(y) * size_t(w) + size_t(x)) * 4]; };
    auto luma = [](const uint8_t* p) {
        return (299 * int(p[0]) + 587 * int(p[1]) + 114 * int(p[2])) / 1000;
    };
    // One pixel each side of the seam is adjusted, from a 4-pixel window p1 p0 | q0 q1.
    // Writing only p0/q0 keeps the filter local: a seam cannot bleed into the block.
    auto filterLine = [&](uint8_t* p1, uint8_t* p0, uint8_t* q0, uint8_t* q1) {
        const int lp1 = luma(p1), lp0 = luma(p0), lq0 = luma(q0), lq1 = luma(q1);
        if (std::abs(lp0 - lq0) >= alpha) return;   // real edge: leave it
        if (std::abs(lp1 - lp0) >= beta) return;    // detail on the p side
        if (std::abs(lq1 - lq0) >= beta) return;    // detail on the q side
        for (int k = 0; k < 3; ++k) {               // alpha is untouched
            const int a = p1[k], b = p0[k], c = q0[k], d = q1[k];
            p0[k] = uint8_t((a + 2 * b + c + 2) / 4);
            q0[k] = uint8_t((b + 2 * c + d + 2) / 4);
        }
    };
    for (int x = 8; x + 1 < w; x += 8)              // vertical seams
        for (int y = 0; y < h; ++y)
            filterLine(at(x - 2, y), at(x - 1, y), at(x, y), at(x + 1, y));
    for (int y = 8; y + 1 < h; y += 8)              // horizontal seams
        for (int x = 0; x < w; ++x)
            filterLine(at(x, y - 2), at(x, y - 1), at(x, y), at(x, y + 1));
}

}  // namespace tak::video
