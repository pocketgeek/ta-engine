// legion_route_test -- the route-query interface (src/sim/legion_route.h, Legion 10,000 stage 5): what a RouteView
// answers on a building, paused, complete, bounded and stale route. Unknown is never Unreachable; a tentative
// estimate is never a known distance; coverage and progress follow their stated rules.
#include "sim/legion_route.h"
#include <cstdio>
#include <cstdlib>

using namespace tak::sim;

namespace {
int failures = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            std::fprintf(stderr, "legion_route_test:%d: FAILED %s\n", __LINE__, #c); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

// A 10 x 10 map; the route's window is x 2..7, z 2..7. Seed at (2,2); potentials are 5 per orthogonal step along
// x then z (a stand-in: the view does not care how they were made). The heuristic aims at (7,7).
RouteGrid grid() {
    RouteGrid g;
    g.initTiles(10, 2, 2, 6, 6, 2);   // 4 x 4 tiles: the window spans parts of four
    for (int z = 0; z < 6; ++z)
        for (int x = 0; x < 6; ++x) g.slot(2 + x, 2 + z) = uint32_t(5 * (x + z));
    g.aimed = true; g.tx = 7; g.tz = 7;
    g.serial = 41; g.epoch = 9; g.work = 100;
    return g;
}
size_t cell(int x, int z) { return size_t(z * 10 + x); }
}  // namespace

int main() {
    {   // A complete, unbounded route on the current geometry.
        RouteGrid g = grid();
        g.done = true;
        g.slot(5, 5) = route::kNone;   // (5,5): not reached
        const RouteView v(g);
        CHECK(v.complete() && v.usable() && !v.paused());
        CHECK(v.estimate(cell(4, 4)) == 20 && v.known(cell(4, 4)) == 20 && v.settled(cell(4, 4)));
        CHECK(v.reach(cell(4, 4)) == Reach::Reachable);
        CHECK(v.reach(cell(5, 5)) == Reach::Unreachable);   // proven: complete, spans the component, fresh
        CHECK(v.reach(cell(0, 0)) == Reach::Unreachable);   // outside the component's window
        g.epoch = 3;                                           // a later change elsewhere (another island or plane)
        CHECK(v.reach(cell(5, 5)) == Reach::Unreachable);     // did not touch it: still proven
        g.touched = true;                                      // a change reached the window
        CHECK(v.reach(cell(5, 5)) == Reach::Unknown);          // unknown, not unreachable
        CHECK(v.reach(cell(4, 4)) == Reach::Reachable);
        g.epoch = 9; g.touched = false;
        CHECK(v.covered(cell(4, 4), 1000000));                 // a complete route covers every reached cell
        CHECK(!v.covered(cell(5, 5), 0));
        const auto pr = v.progress(cell(4, 4));
        CHECK(pr && pr->value == 20 && pr->metric == 41);
        CHECK(!v.progress(cell(5, 5)));
        CHECK(v.metric() == 41 && v.geometryEpoch() == 9 && v.version() == 100);
    }
    {   // A complete but bounded route: an unreached cell is unknown (the window may cut the way).
        RouteGrid g = grid();
        g.done = true; g.bounded = true;
        g.slot(5, 5) = route::kNone;
        const RouteView v(g);
        CHECK(v.reach(cell(5, 5)) == Reach::Unknown);
        CHECK(v.reach(cell(0, 0)) == Reach::Unknown);
    }
    {   // A build in progress: current key 40. key(x,z) = 5(x-2 + z-2) + h, h = octile to (7,7).
        RouteGrid g = grid();
        g.current = 40;
        const RouteView v(g);
        CHECK(!v.complete() && !v.usable() && !v.paused());
        // (2,2): potential 0, h = 5*5 + 2*5 = 35 -> key 35 <= 40: settled.
        CHECK(v.settled(cell(2, 2)) && v.known(cell(2, 2)) == 0);
        // (4,4): potential 20, h = 3*5 + 2*3 = 21 -> key 41 > 40: reached, not settled.
        CHECK(v.estimate(cell(4, 4)) == 20 && !v.settled(cell(4, 4)) && v.known(cell(4, 4)) == route::kNone);
        CHECK(v.reach(cell(4, 4)) == Reach::Reachable);      // a finite estimate is a real way
        CHECK(!v.progress(cell(4, 4)));                          // progress only from settled cells
        // (2,2) has 5 of key to spare.
        CHECK(v.covered(cell(2, 2), 5) && !v.covered(cell(2, 2), 6));
        // Unreached in a building route: unknown, whatever the window.
        g.slot(5, 5) = route::kNone;
        CHECK(RouteView(g).reach(cell(5, 5)) == Reach::Unknown);
    }
    {   // Paused: usable, still not complete; nothing unsettled becomes known.
        RouteGrid g = grid();
        g.current = 40; g.covered = true;
        const RouteView v(g);
        CHECK(v.usable() && v.paused() && !v.complete());
        CHECK(v.known(cell(4, 4)) == route::kNone);
        g.slot(5, 5) = route::kNone;
        CHECK(RouteView(g).reach(cell(5, 5)) == Reach::Unknown);
    }
    {   // An empty view: no state queries crash, nothing is usable.
        const RouteView v;
        CHECK(!v && !v.complete() && !v.usable() && !v.paused());
    }
    {   // Sparse storage (stage 6): only the tiles written are allocated; an unwritten tile reads kNone.
        RouteGrid g;
        g.initTiles(100, 10, 10, 80, 80, 4);   // 16 x 16 tiles: the window spans sectors 0..5 each way
        CHECK(g.sw == 6 && g.sh == 6 && g.ownTiles == 0 && g.cells() == 6400);
        CHECK(g.cellAt(50, 50) == route::kNone);
        g.slot(50, 50) = 7;
        CHECK(g.ownTiles == 1 && g.cellAt(50, 50) == 7 && g.cellAt(51, 50) == route::kNone && g.cellAt(70, 70) == route::kNone);
        g.slot(89, 89) = 3;
        CHECK(g.ownTiles == 2 && g.cellAt(89, 89) == 3);
        CHECK(g.cellAt(90, 90) == route::kNone && g.cellAt(9, 50) == route::kNone);   // outside the window
        const RouteView v(g);
        CHECK(v.estimate(size_t(50 * 100 + 50)) == 7 && v.estimate(size_t(60 * 100 + 60)) == route::kNone);
    }
    {   // Demand: one entry per cell, the widest margin, arrival order, no cap.
        RouteDemand d;
        CHECK(d.need(30, 10) && !d.need(30, 5) && d.entries()[0].second == 10);
        CHECK(!d.need(30, 40) && d.entries()[0].second == 40 && d.size() == 1);
        for (int c = 0; c < 1000; ++c) d.need(1000 + c, uint32_t(c));
        CHECK(d.size() == 1001);                                    // the 32-entry list kept 32
        d.drop([](int cell, uint32_t) { return cell % 2 == 0; });   // drop even cells, keep the order
        CHECK(d.size() == 500 && d.entries()[0].first == 1001 && d.entries()[1].first == 1003);
        CHECK(!d.need(1003, 0) && d.size() == 500);                 // the index follows the compaction
        CHECK(d.need(1002, 7) && d.entries().back() == std::make_pair(1002, 7u));
        d.clear();
        CHECK(d.empty() && d.need(5, 1));
    }
    if (failures) {
        std::fprintf(stderr, "legion_route_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("legion_route_test: ok\n");
    return 0;
}
