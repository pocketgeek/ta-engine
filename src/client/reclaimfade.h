#pragma once
// How a reclaimed tree or rock is drawn while it fades out (display only).
//
// Retail (icd, Glide path): the reclaim's last stage (494fd0) starts a fade
// object (494ce0 mode 1: +0x2e set, alpha +0x30 = 1.0f). Each tick 4959c0
// subtracts 0.03f (5f03cc) and, once the alpha is <= 0, takes the feature off
// the plane (494f00): 34 decrements, kReclaimFadeTicks in the sim. While it
// fades, the feature draw 4fd430 replaces the sprite with a SILHOUETTE: it
// copies the current frame, sets every texel that is not the frame's
// transparent index to 0xff (white in every shipped palette), and draws it as
// a colour-keyed quad with vertex colour (ftol(alpha * 255.0f) << 24) |
// 0xffffff under source-alpha blending (renderer state 6, 5b77b0 ->
// 5b7f70). No shadow, no flames: that path skips the rest of the draw. So a
// reclaimed tree turns white and fades away in about a second.
//
// The sim keeps Feature::fadeLeft (hashed); the client draws from it.

#include "client/hiddenunits.h"
#include "sim/retailreclaim.h"

#include <cstdint>
#include <vector>

namespace tak::reclaimfade {

// The alpha after `steps` of retail's per-tick float subtraction, as the
// vertex alpha byte. The float rounding is retail's: fst stores each step back
// to a float, and the x87 product with 255.0f is exact before ftol truncates.
inline uint8_t alphaByte(int steps) {
    float alpha = 1.0f;
    for (int i = 0; i < steps; ++i) alpha = alpha - 0.03f;
    if (!(alpha > 0.0f)) return 0;
    return uint8_t(int(double(alpha) * 255.0));
}

// The tick count at which a feature seen with `fadeLeft` at tick `tick`
// leaves the plane: the sim takes one off fadeLeft every tick, so this stays
// the same whichever tick the client happened to read it on. 0 = not fading.
inline uint32_t fadeEnd(uint32_t tick, uint8_t fadeLeft) {
    return fadeLeft ? tick + fadeLeft : 0;
}

// How many decrements the fade has taken in the snapshot of game tick
// `gameTick`, or 0 when that snapshot predates the fade (draw it as usual).
inline int fadeSteps(uint32_t end, uint32_t gameTick) {
    if (!end) return 0;
    const int64_t left = int64_t(end) - int64_t(gameTick);
    const int64_t steps = int64_t(sim::kReclaimFadeTicks) - left;
    if (steps <= 0) return 0;
    return steps >= sim::kReclaimFadeTicks ? sim::kReclaimFadeTicks - 1 : int(steps);
}

// The silhouette texture: opaque white wherever the frame is opaque,
// transparent elsewhere (RGBA, width x height). The clear texels are white too,
// so a filtered edge does not pull in black.
inline std::vector<uint8_t> silhouette(const hiddenunits::AlphaMask& mask) {
    const int w = mask.width(), h = mask.height();
    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (!mask.opaque(x, y)) rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] = 0;
    return rgba;
}

} // namespace tak::reclaimfade
