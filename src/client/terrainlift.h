#pragma once
// The terrain-height lift: where retail draws a thing standing on the heightmap.
//
// Terrain is a flat mosaic of 32px tiles with the relief painted in; nothing about
// the tiles moves. Retail draws every object at (KINGDOMS.icd)
//     screenX = x - cameraX
//     screenY = z - (y >> 1) - cameraY        (units 0x4fcd1f, features 0x421dad)
// with y the object's own ABSOLUTE height. For an upright ground unit y is the
// terrain sample under it (0x51b2a0 -> 0x51b220 -> 0x511170); a floater stands at
// sea - waterline (0x51b2fa). No per-map reference is subtracted, so a unit on a
// plateau of height 220 is drawn 110px up-screen even when that plateau is most of
// the map, and the shipped maps' heightmaps are authored to that rule.
//
// Header-only and free of SDL so a test can pin it (tools/terrainlift_test.cpp).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "sim/retailheight.h"

namespace tak::client::lift {

// Screen-Y per height unit (retail's `sar $1`).
inline constexpr float kScaleY = 0.5f;

// Retail's terrain sampler (0x511170, shared with the sim's unit Y): the height
// of cell (cx, cz) belongs to its north-west CORNER at world (16cx, 16cz), the
// sub-pixel fraction is discarded and each interpolation truncates toward zero.
// Retail answers -1 for the last row/column; for drawing we clamp the point into
// the map instead, so edge rows hold their height (inferred: retail never draws
// a mover there).
inline float terrainHeight(const uint8_t* heights, int width, int height, float wx, float wz) {
    if (!heights || width < 2 || height < 2) return 0.0f;
    const auto pixel = [](float w, int cells) {
        if (!(w > 0.0f)) return 0;
        return std::min(int(std::floor(std::min(w, 1e6f))), cells * 16 - 17);
    };
    const int32_t x = int32_t(pixel(wx, width)) * 65536, z = int32_t(pixel(wz, height)) * 65536;
    return float(tak::sim::retailTerrainHeight(x, z, width, height, [&](int cx, int cz) {
        return int(heights[size_t(cz) * size_t(width) + size_t(cx)]);
    }));
}

// The height a grounded body stands at (0x51b2a0): an upright unit stands on the
// terrain; a canhover unit on the terrain but never below sea - waterline
// (0x51b220); a floater at sea - waterline whatever the seabed does (0x51b2fa).
inline float bodyHeight(float terrain, int seaLevel, int waterline, bool canHover, bool floater) {
    const float surface = float(int(uint8_t(seaLevel)) - int(uint8_t(waterline)));
    if (floater) return surface;
    if (canHover) return std::max(terrain, surface);
    return terrain;
}

// Screen-Y lift (world px, subtracted from z) of a body at integer height y, as
// retail's `sar $1`: odd heights round down, negative ones toward -inf.
inline float screenLift(int y) { return float(y >> 1); }

}  // namespace tak::client::lift
