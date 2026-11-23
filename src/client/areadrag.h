// areadrag -- the shared rules for a mouse drag that draws a box on the map: the
// right-drag area clear, the lodestone box and the unit-selection marquee.
//
// Anchor. A drag's start is kept on the MAP, not on the screen: the press point's
// position on the flat tile mosaic (offX + sx/zoom). Tiles are drawn flat, so this is
// exactly the ground the player pressed on, and projecting it back with the current
// camera puts the box corner on that same ground after any pan (edge scroll, keys,
// minimap, middle drag) or zoom during the drag. Retail does the same: its drag box
// corners are world points re-projected every frame (KINGDOMS.icd 0x5202c0).
//
// Click or area. An area command needs BOTH: the pointer really dragged (moved at
// least kAreaCommandMinPx on the screen since the press, on either axis) AND the box
// drawn from the re-projected anchor to the pointer is that big on BOTH sides. So a
// pan alone (edge scroll while clicking near the edge) never turns a click into an
// area clear, a drag that the camera follows (pointer kept over the pressed ground)
// does not make one either, a box stretched along one axis by edge scroll while the
// pointer runs along the edge still counts, and the preview shows exactly when the
// release will be an area. Below it the release is a click AT THE PRESS.
// Retail's test (0x527bf0) is 32 world pixels on EITHER axis, or a press held 25
// clock ticks; a twitch sideways then made a sliver box, so here both sides count.
#pragma once

#include <algorithm>
#include <cmath>

namespace tak::client::areadrag {

// Smallest side, in UI pixels at 100% UI scale, of a dragged box that becomes an area
// command (area clear, lodestone box). Retail's distance, required of both sides.
inline constexpr float kAreaCommandMinPx = 32.0f;
// The selection marquee keeps its own small click/box split: a thin box is still a
// real selection, and a click must stay a click under a few pixels of jitter.
inline constexpr float kSelectBoxMinPx = 6.0f;

// A drag start: the press's point on the map plane (flat tile mosaic, world units)
// and on the screen it was pressed on.
struct Anchor {
    float x = 0, z = 0;     // map plane
    float sx = 0, sy = 0;   // screen at the press
};

inline Anchor anchorAt(float sx, float sy, float offX, float offY, float zoom) {
    return {offX + sx / zoom, offY + sy / zoom, sx, sy};
}

struct ScreenPoint {
    float x = 0, y = 0;
};

// The anchor on today's screen.
inline ScreenPoint toScreen(const Anchor& a, float offX, float offY, float zoom) {
    return {(a.x - offX) * zoom, (a.z - offY) * zoom};
}

// The output-pixel size of a UI pixel: the HUD's own scale, which already carries the
// first-run OS display scale (Windows DPI, macOS Retina backing scale).
inline float threshold(float minPx, float uiScale) {
    return minPx * std::max(uiScale, 0.25f);
}

inline bool bothAxes(float ax, float ay, float bx, float by, float t) {
    return std::fabs(bx - ax) >= t && std::fabs(by - ay) >= t;
}
inline bool eitherAxis(float ax, float ay, float bx, float by, float t) {
    return std::fabs(bx - ax) >= t || std::fabs(by - ay) >= t;
}

// The release at screen (px,py) is an area command. `now` is the anchor on the
// current screen (toScreen).
inline bool isAreaBox(const Anchor& a, ScreenPoint now, float px, float py, float uiScale) {
    const float t = threshold(kAreaCommandMinPx, uiScale);
    return eitherAxis(a.sx, a.sy, px, py, t) && bothAxes(now.x, now.y, px, py, t);
}

// The marquee is a box select rather than a click select: the same two tests, with
// the small either-axis size.
inline bool isSelectBox(const Anchor& a, ScreenPoint now, float px, float py, float uiScale) {
    const float t = threshold(kSelectBoxMinPx, uiScale);
    return eitherAxis(a.sx, a.sy, px, py, t) && eitherAxis(now.x, now.y, px, py, t);
}

}  // namespace tak::client::areadrag
