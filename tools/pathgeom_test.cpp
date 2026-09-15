// pathgeom_test -- the geometry the route installer relies on.
//
// Synthetic NavGrids, so the only thing under test is the arithmetic: does a validated
// straight line really correspond to the line the unit will travel, and does a diagonal
// step mean the same thing to the search, to the shortcut, and to the mover?
//
// WHY THIS TEST EXISTS. Route validation ran through three checks that quietly disagreed
// about what line was being asked about:
//
//   lineFits    cell centre -> cell centre
//   losBetween  takes WORLD endpoints, converts them to cells on entry, then walks cell
//               centres -- so the sub-cell position it was handed is discarded and it
//               answers about a different line from the one the unit travels
//   segmentFits the real segment, every cell it crosses
//
// The shortcut installer used losBetween believing it was the exact-position check. It
// was not, and a body standing near a cell edge could be handed a shortcut straight
// through an obstacle that no check had looked at.

#include "sim/sim.h"
#include "sim/pathsearch.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace tak::sim;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
    if (!ok) ++fails;
}

// A flat, fully walkable grid of w x h cells; block cells explicitly.
static NavGrid flatGrid(int w, int h) {
    std::vector<uint8_t> heights(size_t(w + 1) * size_t(h + 1), 100);
    return NavGrid(heights, w, h, 0, NavGrid::Limits{});
}

int main() {
    std::printf("pathgeom_test\n");

    // THE COUNTEREXAMPLE. (15,1) -> (24,56) really passes through cell (1,0): leaving
    // x=15 for x=16 happens at z~7, which is still row 0. The cell-to-cell line between
    // the same endpoints' cells never visits (1,0), so an obstacle there is invisible to
    // it -- while the body drives straight through the obstacle.
    {
        NavGrid g = flatGrid(8, 8);
        g.block(1, 0, 1, 1, true);
        check(!g.segmentFits(15, 1, 24, 56, 1),
              "exact traversal sees the cell a cell-centred walk skips");
        check(g.losBetween(15, 1, 24, 56, 0, 0),
              "...and losBetween does not (which is why it was the wrong check)");
    }

    // EDGE-OF-CELL STARTS. Two bodies in the same cell but at opposite edges face
    // genuinely different lines; a check that starts from the cell centre cannot tell
    // them apart, which is the whole failure mode.
    {
        NavGrid g = flatGrid(8, 8);
        g.block(1, 0, 1, 1, true);
        // BOTH starts are inside cell (0,0) and both aim at the same point, so a
        // cell-centred check cannot tell them apart. The real lines differ: from the top
        // of the cell the segment climbs into row 1 before it ever reaches column 1, and
        // misses the blocked cell entirely; from the bottom it crosses straight through.
        const bool nearTop = g.segmentFits(1, 1, 60, 40, 1);
        const bool nearBottom = g.segmentFits(1, 14, 60, 40, 1);
        check(!nearTop && nearBottom,
              "sub-cell position changes the answer",
              std::string("(1,1)=") + (nearTop ? "fits" : "blocked") +
                  " (1,14)=" + (nearBottom ? "fits" : "blocked"));
    }

    // DIAGONAL CORNERS. A gap that is only open diagonally must be refused: the body
    // passes between the two blocked cells, and the movers will not cut that corner.
    {
        NavGrid g = flatGrid(8, 8);
        g.block(1, 0, 1, 1, true);
        g.block(0, 1, 1, 1, true);
        check(!g.segmentFits(8, 8, 24, 24, 1),
              "a diagonal squeeze between two blocked cells is refused");
    }

    // ...and the SEARCH must refuse the same squeeze, rather than planning a route the
    // shortcut and the mover will both reject.
    {
        // (0,0) to (2,2) with (1,0) and (0,1) blocked: the only direct link is the
        // diagonal corner. A legal route has to go the long way round.
        struct G {
            std::vector<std::string> rows;
            int score(int x, int z) const {
                if (z < 0 || z >= int(rows.size())) return 0;
                if (x < 0 || x >= int(rows[z].size())) return 0;
                return rows[z][x] == '#' ? 0 : kCellGround;
            }
        } g{{
            ".#..",
            "#...",
            "....",
            "....",
        }};
        PathSearch ps;
        ps.reset(4, 4);
        ps.start = {0, 0}; ps.goal = {2, 2}; ps.cur = ps.start;
        auto sc = [&](int x, int z) { return g.score(x, z); };
        PathSearch::Result r = PathSearch::Result::Suspended;
        for (int t = 0; t < 64 && r == PathSearch::Result::Suspended; ++t) {
            int cap = 4000;
            r = ps.step(sc, cap);
        }
        // Whatever it returns, it must not claim a route whose first move is the
        // illegal diagonal out of (0,0).
        bool cutsCorner = false;
        if (r == PathSearch::Result::Arrived && !ps.out.empty()) {
            const PathCell f = ps.out.front();
            if (f.x == 1 && f.z == 1) cutsCorner = true;
        }
        check(!cutsCorner, "the search does not plan a diagonal the mover would refuse");
    }

    // A CLEAR LINE STILL PASSES -- the checks above must not have made everything fail.
    {
        NavGrid g = flatGrid(8, 8);
        check(g.segmentFits(8, 8, 120, 120, 1), "open ground is still traversable");
        check(g.segmentFits(15, 1, 24, 56, 1), "the counterexample line is fine when clear");
    }

    // FOOTPRINT CLEARANCE. A 3-cell body must not be handed a line through a 1-cell gap.
    {
        NavGrid g = flatGrid(12, 12);
        for (int z = 0; z < 12; ++z)
            if (z != 6) g.block(5, z, 1, 1, true);        // wall with a one-cell door
        check(g.segmentFits(8, 104, 184, 104, 1), "a 1-cell body fits the 1-cell door");
        check(!g.segmentFits(8, 104, 184, 104, 3), "a 3-cell body does not");
    }

    // THE 64-CORNER CAP MUST NOT EAT THE DESTINATION. A goal that is neither cardinal
    // nor 45 degrees makes the march alternate two directions, so every step is a
    // direction change and every one becomes a corner -- a staircase whose length grows
    // with the trip. Reconstruction used to truncate that to 64 BEFORE the world smoothed
    // it, so the route ended at a fixed (64,32) however far away the goal was:
    //
    //   160x160 -> goal (159,79)   route ended (64,32)   95,47 cells discarded
    //
    // Every one of those corners is on open ground, so smoothing collapses the whole trip
    // to a single segment -- the cap only ever bit because it was applied first.
    {
        for (int n : {80, 160, 220}) {
            const int gx = n - 1, gz = (n - 1) / 2;
            auto sc = [&](int x, int z) {
                return (x < 0 || z < 0 || x >= n || z >= n) ? 0 : kCellGround;
            };
            PathSearch ps;
            ps.reset(n, n);
            ps.start = {0, 0}; ps.goal = {gx, gz}; ps.cur = ps.start;
            PathSearch::Result r = PathSearch::Result::Suspended;
            for (int t = 0; t < 8000 && r == PathSearch::Result::Suspended; ++t) {
                int cap = 1 << 28;
                r = ps.step(sc, cap);
            }
            const bool atGoal = r == PathSearch::Result::Arrived && !ps.out.empty() &&
                                ps.out.back().x == gx && ps.out.back().z == gz;
            char what[96];
            std::snprintf(what, sizeof what,
                          "a %dx%d open staircase still ends at its destination", n, n);
            char detail[96];
            if (ps.out.empty())
                std::snprintf(detail, sizeof detail, "no route");
            else
                std::snprintf(detail, sizeof detail, "%zu corners, ends (%d,%d), goal (%d,%d)",
                              ps.out.size(), ps.out.back().x, ps.out.back().z, gx, gz);
            check(atGoal, what, detail);
        }
    }

    std::printf(fails ? "pathgeom_test: %d FAILED\n" : "pathgeom_test: all passed\n", fails);
    return fails ? 1 : 0;
}
