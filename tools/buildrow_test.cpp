// buildrow_test -- the conjure (build) menu's layout (client/buildrow.h).
//
// The one function drawPanel, the click and hover tests and the chat box's floor share.
// For every icon count x window size x UI scale x BUILD MENU SCALE x BUILD MENU
// alignment: no icon reaches under the command panel (and its mana bulb) or off the
// left edge, none overlaps another or the information bar, the container holds them
// all; a menu too long for one row wraps into balanced rows stacked upward at full icon
// size, in menu order; and wherever one row fits, it is exactly the row it always was.

#include "client/buildrow.h"
#include "client/chatbox.h"

#include <cmath>
#include <cstdio>

namespace br = tak::buildrow;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

static bool near(float a, float b) { return std::fabs(a - b) <= 1e-3f; }

int main() {
    std::printf("buildrow_test\n");
    constexpr float eps = 1e-3f;
    // Icons are stepped across a row (as drawPanel always did), so the last one of a
    // long row may carry float rounding of a few thousandths of a pixel.
    constexpr float edgeEps = 1e-2f;

    // ---- the sweep: nothing ever covers HUD chrome ----
    bool inside = true, noOverlap = true, inBox = true, order = true, balanced = true,
         fullSize = true, fewest = true, centred = true;
    int wrapped = 0;
    for (float winW : {640.0f, 800.0f, 1024.0f, 1280.0f, 1366.0f, 1600.0f, 1920.0f, 2560.0f, 3840.0f})
        for (float winH : {480.0f, 600.0f, 720.0f, 768.0f, 1080.0f, 1440.0f})
            for (float ui : {0.5f, 1.0f, 1.5f, 2.0f})
                for (float panelW : {140.0f, 204.0f, 270.0f})
                    for (float bs : {0.5f, 0.75f, 1.0f, 2.0f, 4.0f})
                        for (int align = 0; align < 3; ++align)
                            for (int n = 1; n <= 30; ++n) {
                                const float barH = float(int(72 * ui)), barTop = winH - barH;
                                const float areaW = std::max(64.0f, winW - panelW);
                                if (areaW < 200) continue;   // no play area to speak of
                                const auto L = br::layout(n, barTop, barH, bs, winW, areaW, align);
                                if (int(L.icons.size()) != n) { inside = false; continue; }
                                if (L.rows > 1) ++wrapped;
                                for (int i = 0; i < n; ++i) {
                                    const auto& r = L.icons[size_t(i)];
                                    if (r.x < br::kEdge - edgeEps || r.x + r.w > areaW - br::kEdge + edgeEps ||
                                        r.y < -eps || r.y + r.h > barTop - 5 + eps)
                                        inside = false;
                                    if (r.x < L.box.x || r.y < L.box.y ||
                                        r.x + r.w > L.box.x + L.box.w + eps ||
                                        r.y + r.h > L.box.y + L.box.h + eps)
                                        inBox = false;
                                    for (int j = i + 1; j < n; ++j) {
                                        const auto& q = L.icons[size_t(j)];
                                        if (r.x < q.x + q.w - eps && q.x < r.x + r.w - eps &&
                                            r.y < q.y + q.h - eps && q.y < r.y + r.h - eps)
                                            noOverlap = false;
                                    }
                                    // menu order: left to right, then the next row down
                                    if (i > 0) {
                                        const auto& p = L.icons[size_t(i - 1)];
                                        if (!(near(p.y, r.y) ? p.x < r.x : p.y < r.y)) order = false;
                                    }
                                }
                                // the box stays clear of the panel too (its 5 px frame)
                                if (L.box.x + L.box.w > areaW - 5 + eps || L.box.x < 5 - eps)
                                    inside = false;
                                // balanced rows, the extra icons on the upper ones; centred
                                // rows share one centre
                                int prevCount = -1;
                                float prevMid = 0;
                                for (int i = 0; i < n;) {
                                    int c = 0;
                                    const float y = L.icons[size_t(i)].y;
                                    const float x0 = L.icons[size_t(i)].x;
                                    while (i < n && near(L.icons[size_t(i)].y, y)) ++i, ++c;
                                    const float mid = (x0 + L.icons[size_t(i - 1)].x + L.iconW) / 2;
                                    if (prevCount >= 0 && (c > prevCount || prevCount - c > 1))
                                        balanced = false;
                                    if (align == br::Centered && prevCount >= 0 && !near(mid, prevMid))
                                        centred = false;
                                    prevCount = c;
                                    prevMid = mid;
                                }
                                // the chosen scale is used whenever any number of rows fits it
                                const float unitH = barH - 10, unitW = unitH * 4 / 3;
                                const float availW = areaW - 2 * br::kEdge;
                                const int perRow = int((availW + 6 * bs) / ((unitW + 6) * bs));
                                bool canFit = perRow >= 1;
                                int needRows = canFit ? (n + perRow - 1) / perRow : 0;
                                if (canFit && needRows * unitH * bs + (needRows - 1) * 6 * bs >
                                                  std::max(barTop - 10, 24.0f))
                                    canFit = false;
                                if (canFit && !near(L.iconH, unitH * bs)) fullSize = false;
                                if (canFit && L.rows != needRows) fewest = false;
                            }
    std::printf("  (%d wrapped layouts in the sweep)\n", wrapped);
    check(wrapped > 1000, "the sweep covers wrapped menus");
    check(inside, "every icon left of the command panel, right of the edge, over the bar");
    check(noOverlap, "no icon overlaps another");
    check(inBox, "the container holds every row");
    check(order, "icons in menu order: across, then the next row down");
    check(balanced, "rows balanced (differ by at most one, upper rows fuller)");
    check(centred, "centred rows share one centre");
    check(fullSize, "icons keep their size wherever the rows fit");
    check(fewest, "and take the fewest rows that hold them");

    // ---- one row, where it fits: exactly the row drawPanel always drew ----
    {
        bool same = true;
        for (float winW : {1600.0f, 1920.0f, 2560.0f, 3840.0f})
            for (float ui : {1.0f, 1.5f})
                for (int align : {int(br::Left), int(br::Centered)})
                    for (int n = 1; n <= 13; ++n) {
                        const float barH = float(int(72 * ui)), barTop = 1440 - barH;
                        const float areaW = winW - 204;
                        const float iconSz = barH - 10, iconW = iconSz * 4 / 3, gap = 6;
                        const float rowW = (n - 1) * (iconW + gap) + iconW;
                        if (rowW > areaW - 20) continue;   // that is the case that wraps now
                        float x0 = align == br::Centered ? (winW - rowW) / 2 : 10;
                        x0 = std::max(x0, 10.0f);
                        if (x0 + rowW > areaW - 10) continue;   // that one ran under the panel
                        const auto L = br::layout(n, barTop, barH, 1, winW, areaW, align);
                        if (L.rows != 1 || !near(L.icons[0].x, x0) ||
                            !near(L.icons[0].y, barTop - iconSz - 5) || !near(L.iconH, iconSz) ||
                            !near(L.box.x, x0 - 5) || !near(L.box.w, rowW + 10) ||
                            !near(L.box.h, iconSz + 10) || !near(L.bandTop, barTop - iconSz - 5 - 28))
                            same = false;
                        for (int i = 0; i < n; ++i)
                            if (!near(L.icons[size_t(i)].x, x0 + i * (iconW + gap))) same = false;
                    }
        check(same, "a menu that fits one row: the same row as before (left, centred)");
        // Right: the row now ends at the panel instead of the window (it ran under it).
        const auto R = br::layout(5, 1368, 72, 1, 2560, 2356, br::Right);
        check(near(R.icons.back().x + R.iconW, 2346), "a right-aligned row ends 10 px left of the panel");
    }

    // ---- the user's case: 1920 x 768, the Zhon conjurer's 13 icons at 160% ----
    {
        const float barH = 72, barTop = 768 - barH, areaW = 1920 - 204;
        for (int align = 0; align < 3; ++align) {
            const auto L = br::layout(13, barTop, barH, 1.6f, 1920, areaW, align);
            char what[96];
            std::snprintf(what, sizeof what, "1920x768, 13 icons, align %d: two rows of 7 + 6, full size",
                          align);
            check(L.rows == 2 && near(L.iconH, 62 * 1.6f) && near(L.icons[6].y, L.icons[0].y) &&
                      L.icons[7].y > L.icons[6].y && near(L.icons[12].y, barTop - 5 - 62 * 1.6f),
                  what);
            float right = 0;
            for (const auto& r : L.icons) right = std::max(right, r.x + r.w);
            std::snprintf(what, sizeof what, "  align %d: clear of the command panel (x %.0f < %.0f)",
                          align, right, areaW);
            check(right <= areaW - 10 + eps, what);
        }
    }

    // ---- the chat box floor follows a wrapped menu up ----
    {
        const auto L = br::layout(13, 696, 72, 1.6f, 1920, 1716, br::Centered);
        const float floorY = tak::chatbox::hudFloor(696, 72, 1.6f, L.bandTop, true, 10);
        check(L.rows == 2 && floorY <= L.top - 28 * 1.6f + eps &&
                  floorY < tak::chatbox::buildRowTop(696, 72, 1.6f),
              "the chat box floor sits over the top row's tooltip");
        check(near(tak::chatbox::buildRowTop(533, 72, 1), 438), "one-row estimate unchanged (605 high)");
    }

    // ---- a scale too big for the window is shrunk to the largest that fits ----
    {
        const auto L = br::layout(13, 768 - 144, 144, 4, 1024, 1024 - 270, br::Centered);
        const float blockH = L.rows * L.iconH + (L.rows - 1) * L.gap;
        check(L.scale < 4 && blockH <= 768 - 144 - 10 + eps && L.top >= 0,
              "400% on 1024x768 at UI 200%: shrunk to fit over the bar");
        // and no larger scale would have fit with any row count
        bool largest = true;
        for (int r = 1; r <= 13; ++r) {
            const int c = (13 + r - 1) / r;
            const float sw = (1024 - 270 - 20) / (c * 134 * 4 / 3.0f + (c - 1) * 6);
            const float sh = (768 - 144 - 10) / (r * 134.0f + (r - 1) * 6);
            if (std::min(sw, sh) > L.scale + 1e-4f) largest = false;
        }
        check(largest, "  the largest scale any row count allows");
    }

    check(br::layout(0, 700, 72, 1, 1920, 1716, br::Centered).icons.empty(), "no menu: nothing");

    std::printf(g_fail ? "buildrow_test: %d FAILED\n" : "buildrow_test: all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
