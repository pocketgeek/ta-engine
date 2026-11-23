// Map drags (client/areadrag.h): when a dragged box becomes an area command, and how
// its press corner stays on the ground the player pressed while the camera moves.
#include "client/areadrag.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace tak::client::areadrag;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() try {
    // A press at screen (400,300) with the camera at (1000,2000), zoom 1.
    const Anchor a = anchorAt(400, 300, 1000, 2000, 1);
    check(near(a.x, 1400) && near(a.z, 2300), "anchor is the press's map point");
    const ScreenPoint still = toScreen(a, 1000, 2000, 1);
    check(near(still.x, 400) && near(still.y, 300), "unmoved camera: anchor is the press point");
    const auto area = [&](ScreenPoint now, float px, float py, float ui = 1) {
        return isAreaBox(a, now, px, py, ui);
    };
    // Click or area, unmoved camera, 100% UI scale.
    check(!area(still, 400, 300), "no movement is a click");
    check(!area(still, 405, 304), "a twitch is a click");
    check(!area(still, 431, 331), "31 px each way is still a click");
    check(area(still, 432, 332), "32 px each way is an area");
    check(area(still, 368, 268), "dragging up-left counts the same");
    check(!area(still, 600, 310), "a long thin sliver is a click, however long");
    check(!area(still, 405, 700), "so is a tall thin one");
    // The threshold is in UI pixels: a 200% UI (or a first-run Retina / high-DPI scale)
    // needs twice the output pixels, a 50% UI half.
    check(!area(still, 440, 340, 2), "40 px is under the 64 px area at 200%");
    check(area(still, 464, 364, 2), "64 px is the area at 200%");
    check(area(still, 416, 316, 0.5f), "16 px is the area at 50%");
    check(near(threshold(kAreaCommandMinPx, 1), 32), "area threshold is 32 UI px");
    // The selection marquee keeps its small either-axis box test.
    check(!isSelectBox(a, still, 405, 305, 1), "select: jitter is a click");
    check(isSelectBox(a, still, 406, 300, 1), "select: 6 px one way is a box");
    check(!isSelectBox(a, still, 410, 300, 2), "select: scales with the UI too");

    // Anchoring: pan right/down 100 world px; the pressed ground moves up-left.
    const ScreenPoint panned = toScreen(a, 1100, 2100, 1);
    check(near(panned.x, 300) && near(panned.y, 200), "pan: the corner follows the map");
    // Pan alone, pointer held still: the drawn box is 100 px but the pointer never
    // dragged -- a click (edge scroll while right-clicking near the edge).
    check(!area(panned, 400, 300), "pan alone never makes an area command");
    check(!isSelectBox(a, panned, 400, 300, 1), "pan alone never makes a box select");
    // Pointer dragged 40 px while the map moved along with it (pointer kept over the
    // pressed ground): the drawn box is empty.
    const ScreenPoint followed = toScreen(a, 960, 1960, 1);   // the pressed ground at 440,340
    check(!area(followed, 440, 340), "a drag the camera followed is no area either");
    // A box stretched by edge scroll: the pointer runs 150 px along the bottom edge
    // while the map scrolls 300 px down -- a real 150x310 area though the pointer
    // moved only 10 px vertically.
    check(area(toScreen(a, 1000, 2300, 1), 550, 310), "a box extended by edge scroll is an area");
    // A real drag with a pan: the box covers the pressed ground to the pointer.
    check(area(panned, 450, 350), "real drag across a pan is an area");
    // A pan back under the pointer shrinks the drawn box below the area: a click.
    check(!area(toScreen(a, 980, 1980, 1), 432, 332), "pan that shrinks the drawn box: click");
    // Zoom keeps the anchor's map position.
    const float z = 0.5f, ox = 1400 - 200 / z, oy = 2300 - 150 / z;   // pressed ground at (200,150)
    const ScreenPoint zoomed = toScreen(a, ox, oy, z);
    check(near(zoomed.x, 200) && near(zoomed.y, 150), "zoom: corner stays on the same ground");
    const Anchor b = anchorAt(zoomed.x, zoomed.y, ox, oy, z);
    check(near(b.x, a.x) && near(b.z, a.z), "anchor and screen point round-trip");
    std::puts("PASS: area drag threshold and map anchoring");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
