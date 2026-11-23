#pragma once

// TEXTURE FILTERING (Options > Graphics): how the GAME AREA samples its art when the
// camera magnifies it. The interface, cursors and movies have their own settings
// (Smooth GUI Art, Smooth Movies) and are never touched by this one.
//
//   Off        nothing smoothed -- retail's default look.
//   Units      retail's BiLinearFilter: unit model textures and unit shadows sample
//              LINEAR, everything else nearest (docs/retail-engine.md §BiLinearFilter).
//   Everything Units, plus terrain, feature sprites (trees, rocks, ...) and effect,
//              projectile and ground-shadow sprites -- a non-retail extension, since
//              retail had no camera zoom to magnify them with.
//
// Everything only smooths that art once the view is zoomed IN past kSmoothZoom. At
// 1.0-1.25x a nearest texel is a block of one or two screen pixels: there is no
// staircase to hide and filtering would only soften the picture. Units keep the
// Units behaviour at every zoom. This header is the policy, free of SDL and GameView
// so it can be tested on its own (tools/texture_filter_test.cpp).

namespace tak::texturefilter {

enum Level : int { Off = 0, Units = 1, Everything = 2 };

inline constexpr int clampLevel(int level) { return level < Off ? Off : level > Everything ? Everything : level; }

// Unit model textures, unit atlases and unit shadow pages (retail's switch).
inline constexpr bool unitsLinear(int level) { return clampLevel(level) >= Units; }

// Terrain, feature and effect art turns LINEAR above kSmoothZoom and back to nearest
// only below kSmoothZoom - kSmoothZoomBand, so a wheel notch either side of the line
// (x1.118 per notch) or a slow benchmark zoom cannot flicker between the two.
inline constexpr float kSmoothZoom = 1.5f;
inline constexpr float kSmoothZoomBand = 0.1f;

// Whether that world art samples LINEAR this frame; `wasLinear` is last frame's answer.
inline constexpr bool worldLinear(int level, float zoom, bool wasLinear) {
    if (clampLevel(level) != Everything) return false;
    return wasLinear ? zoom >= kSmoothZoom - kSmoothZoomBand : zoom > kSmoothZoom;
}

}  // namespace tak::texturefilter
