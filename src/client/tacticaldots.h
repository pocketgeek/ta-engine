#pragma once

// TACTICAL DOTS (Options > Graphics; a non-retail extra, default off): when the
// camera is zoomed far out, every unit is drawn as a flat player-coloured dot,
// the way the minimap shows it, instead of its 3D model. Pure display -- the
// sim, the lockstep hash and the network never see it.
//
// This header is the policy, kept free of GameView so it can be tested on its
// own: when dots are on (zoom threshold + hysteresis) and how big one is.

#include <algorithm>
#include <cmath>

namespace tak::tacticaldots {

// WHEN dots show is the player's TACTICAL DOTS ZOOM slider, 0-100%. It is a
// position along this window's zoom-out range, not an absolute zoom, because the
// range differs per window and map: the view cannot zoom out past the point where
// the map fills the world view (MapView::minZoom of the window less the right panel
// and the bottom bar -- 0.73 on Ulasem Arena at 7680x2160, ~0.17 at 1920x1080),
// so a fixed "20% zoom" would never be reachable on a wide display.
//   0%   -> dots only when fully zoomed out (at that floor)
//   100% -> dots from normal size (zoom 1.0) outwards
//   p    -> floor * (1/floor)^(p/100), i.e. measured in WHEEL NOTCHES: each notch
//           is a x1.118 zoom step, so every notch moves the same share of the range.
// When the floor is already >= 1 (a small map on a wide window) every setting
// means "at the floor".
//
// Hysteresis: dots come on at or below the threshold and go off again only above
// threshold * kBuffer. The band is wider than one notch, so a notch back and forth
// across the boundary (or the benchmark camera's slow zoom) cannot flicker.
inline constexpr int   kDefaultPercent = 20;
inline constexpr float kNotch  = 1.118f;
inline constexpr float kBuffer = 1.17f;    // > one notch
inline constexpr float kSlack  = 1.02f;    // float slack at the floor itself
inline constexpr float kMinZoom = 0.05f;   // MapView's absolute zoom-out limit
inline constexpr float kCellWorldPx = 16.0f;

inline float thresholdZoom(int percent, float floorZoom) {
    const float f = floorZoom > 0.0f ? floorZoom : kMinZoom;
    const float top = std::max(1.0f, f);
    const float p = float(std::clamp(percent, 0, 100)) / 100.0f;
    return f * std::pow(top / f, p);
}

// Smallest dot edge, in screen pixels at UI SCALE 100%. The minimap's dots are
// 3px; the world view is far larger, so a little more keeps a lone scout visible.
inline constexpr float kMinDotPx = 4.0f;

class Switch {
public:
    // Feed the option, the slider (0-100), the frame's zoom and the window's
    // zoom-out floor (MapView::minZoom); returns whether dots draw.
    bool update(bool enabled, int percent, float zoom, float floorZoom) {
        const float t = thresholdZoom(percent, floorZoom) * kSlack;
        if (!enabled) on_ = false;
        else on_ = on_ ? zoom <= t * kBuffer : zoom <= t;
        return on_;
    }
    bool on() const { return on_; }

private:
    bool on_ = false;
};

// Share of the footprint a dot covers, so a tight formation still reads as
// separate dots rather than one solid block.
inline constexpr float kFootFill = 0.75f;
// Zoom at which dots reach their largest size (a 2x2-cell soldier ~13px).
inline constexpr float kCompactZoom = 0.35f;

// Edge of a unit's square dot in screen pixels: its larger footprint side at
// this zoom (times kFootFill), so buildings come out as larger squares than
// infantry. Floored so a dot never shrinks out of sight, and capped at its size
// at kCompactZoom so dots shown at a high zoom (a wide window, or a high slider
// setting) are compact markers, not huge slabs. The floor and cap follow UI SCALE, the setting a
// high-DPI player already uses to make small things legible. The minimap draws
// every unit as a square too, so the shape matches it.
inline float dotSide(int footX, int footZ, float zoom, float uiScale = 1.0f) {
    const float cells = float(std::max({footX, footZ, 1}));
    const float ui = std::clamp(uiScale, 0.5f, 2.0f);
    const float lo = kMinDotPx * ui;
    const float hi = std::max(lo, cells * kCellWorldPx * kCompactZoom * kFootFill * ui);
    return std::clamp(cells * kCellWorldPx * zoom * kFootFill, lo, hi);
}

}  // namespace tak::tacticaldots
