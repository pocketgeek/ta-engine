#pragma once

// The conjure (build) menu's layout: where GameView::drawPanel puts a selected
// builder's icons above the information bar. One pure function, kept apart from
// GameView so it can be tested without a renderer, and the ONLY place the row's
// geometry is computed: drawing, clicking and hovering all read the rectangles
// drawPanel stores from it, and the chat box's floor (chatbox.h) asks it too.
// Display only: nothing here touches the simulation or the network.
//
// Icons are (barH - 10) x BUILD MENU SCALE tall, in the retail portraits' 4:3
// landscape shape, 6 x scale apart, and sit 5 px above the bar. They may use the
// play area's width only (left of the command panel and its mana bulb). When a long
// menu does not fit one row it wraps into as many rows as it needs, stacked upward,
// in reading order (the first icons on the top row) and balanced (13 icons over two
// rows are 7 + 6, not 11 + 2). Each row is aligned by BUILD MENU: left rows start at
// the area's left, right rows end at its right, and centred rows share one centre --
// the window's, as it always was, moved in only as far as the panel needs. The icons
// keep their size; only a scale too big for the room there is (a 400% row on a short
// or narrow window) is shrunk, to the largest that fits.

#include <algorithm>
#include <cmath>
#include <vector>

namespace tak::buildrow {

enum Align : int { Left = 0, Centered = 1, Right = 2 };

// Clear space between the icons and the window's left edge / the command panel.
inline constexpr float kEdge = 10.0f;

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

struct Layout {
    std::vector<Rect> icons;   // one per menu entry, in menu order
    float iconW = 0, iconH = 0, gap = 0;
    float scale = 1;     // the BUILD MENU SCALE in effect after any shrink to fit
    int rows = 0;
    Rect box;            // the recessed container behind every row (5 px around them)
    float top = 0;       // the top row's top edge
    float textX = 0;     // where the TRAINING line starts (the top row's first icon)
    float bandTop = 0;   // the highest thing the row draws: the hover tooltip's top
};

// `n` icons over a bar at `barTop`, `barH` tall, at BUILD MENU SCALE `buildScale`, in
// a window `winW` wide whose play area (left of the command panel) is `areaW` wide.
inline Layout layout(int n, float barTop, float barH, float buildScale, float winW,
                     float areaW, int align) {
    Layout L;
    if (n <= 0) return L;
    const float left = kEdge, right = std::max(left + 1.0f, areaW - kEdge);
    const float availW = right - left;
    const float availH = std::max(barTop - 10.0f, 24.0f);
    const float unitH = barH - 10.0f, unitW = unitH * (4.0f / 3.0f), unitGap = 6.0f;
    // The fewest rows that hold the menu at the chosen scale; a scale too big for the
    // room there is (in width for its widest row, in height for the stack) is lowered
    // to the largest any row count allows, and the fewest rows that reach it are used.
    float s = 0;
    int rows = 1;
    for (int r = 1; r <= n; ++r) {
        const int c = (n + r - 1) / r;   // the widest row's icons
        const float sw = availW / (c * unitW + (c - 1) * unitGap);
        const float sh = availH / (r * unitH + (r - 1) * unitGap);
        const float sr = std::min({buildScale, sw, sh});
        if (sr > s) { s = sr; rows = r; }
        if (s >= buildScale) break;
    }
    L.scale = s;
    L.rows = rows;
    L.iconH = unitH * s;
    L.iconW = unitW * s;
    L.gap = unitGap * s;
    const float blockH = rows * L.iconH + (rows - 1) * L.gap;
    L.top = barTop - 5.0f - blockH;
    L.bandTop = L.top - 28.0f * s;   // the tooltip's top (the TRAINING line: 24)
    // Balanced rows, the extra icons on the upper ones.
    const int base = n / rows, extra = n % rows;
    auto rowCount = [&](int r) { return base + (r < extra ? 1 : 0); };
    auto rowWidth = [&](int c) { return c > 0 ? c * L.iconW + (c - 1) * L.gap : 0.0f; };
    const float blockW = rowWidth(rowCount(0));
    // Centred rows keep the window's centre (where the single row always sat) unless
    // the panel or the left edge is in the way.
    const float cx = std::max(left + blockW / 2, std::min(winW / 2, right - blockW / 2));
    L.icons.reserve(size_t(n));
    float minX = 1e30f;
    for (int r = 0; r < rows; ++r) {
        const int c = rowCount(r);
        const float w = rowWidth(c);
        float x0 = align == Centered ? cx - w / 2 : align == Right ? right - w : left;
        x0 = std::max(x0, left);
        minX = std::min(minX, x0);
        const float y = L.top + r * (L.iconH + L.gap);
        // Stepped, not multiplied: the very positions the one row always had.
        float x = x0;
        for (int k = 0; k < c; ++k, x += L.iconW + L.gap)
            L.icons.push_back({x, y, L.iconW, L.iconH});
        if (r == 0) L.textX = x0;
    }
    L.box = {minX - 5, L.top - 5, blockW + 10, blockH + 10};
    return L;
}

}  // namespace tak::buildrow
