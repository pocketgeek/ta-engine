// doubleclick_test -- client/doubleclick.h: a double-click selects every one of YOUR units
// of the clicked type drawn inside the map view; Shift adds (the caller's merge, replayed
// here exactly as the client does); off-screen, other types, enemies and unfinished
// buildings are left out; a double-click on an enemy selects just it.
#include "client/doubleclick.h"

#include <algorithm>
#include <cstdio>

namespace dc = tak::client::doubleclick;
static int g_fail = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("  FAIL %s\n", what); ++g_fail; } }
static bool has(const std::vector<int>& v, int id) { return std::find(v.begin(), v.end(), id) != v.end(); }

int main() {
    std::printf("doubleclick_test\n");
    int archer = 0, knight = 0;
    const void* A = &archer; const void* K = &knight;
    const dc::View view{800, 600};
    std::vector<dc::Candidate> all = {
        {1, A, true, true, 100, 100},    // the clicked archer
        {2, A, true, true, 700, 500},    // archer on screen
        {3, A, true, true, 900, 100},    // archer under the right-hand panel
        {4, A, true, true, 100, 650},    // archer under the bottom bar
        {5, A, true, true, -5, 40},      // archer off the left edge
        {6, K, true, true, 200, 200},    // other type
        {7, A, false, true, 300, 300},   // enemy archer
        {8, A, true, false, 400, 400},   // archer still under construction
        {9, A, true, true, 799, 599},    // archer at the corner, inside
    };
    auto r = dc::expand(1, all, view);
    check(r.front() == 1, "clicked unit first");
    check(has(r, 2) && has(r, 9), "on-screen same type selected");
    check(!has(r, 3) && !has(r, 4) && !has(r, 5), "off-screen / under HUD excluded");
    check(!has(r, 6), "other type excluded");
    check(!has(r, 7), "enemy excluded");
    check(!has(r, 8), "unbuilt excluded");
    check(r.size() == 3, "exactly 1,2,9");

    // The clicked unit itself stays even at the very edge outside the rectangle.
    all[0].sx = 801;
    check(has(dc::expand(1, all, view), 1), "hit always kept");
    all[0].sx = 100;

    // An enemy unit: just that one.
    auto e = dc::expand(7, all, view);
    check(e.size() == 1 && e[0] == 7, "enemy double-click selects only it");
    check(dc::expand(-1, all, view).empty(), "no hit, nothing");

    // Shift merge (same as the client's addSel loop) keeps the old selection.
    std::vector<int> sel = {6};
    for (int id : r) if (!has(sel, id)) sel.push_back(id);
    check(has(sel, 6) && has(sel, 1) && has(sel, 2) && has(sel, 9) && sel.size() == 4, "shift adds to selection");

    // Click detection: needs SDL clicks >= 2, the same unit as the previous select-click,
    // and that click being the previous press in SDL's chain.
    check(dc::isDouble(2, 5, 5, 1), "two clicks same unit");
    check(!dc::isDouble(1, 5, 5, 1), "single click");
    check(!dc::isDouble(2, 5, 6, 1), "second click on a neighbour");
    check(!dc::isDouble(2, -1, -1, 1), "empty ground");
    check(dc::isDouble(3, 5, 5, 2), "triple click expands again, not back to one unit");
    check(!dc::isDouble(3, 5, 5, 1), "a press consumed elsewhere breaks the chain");

    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
