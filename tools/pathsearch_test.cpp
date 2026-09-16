// Does the ported search actually get around things? Synthetic grids, so the
// only thing under test is the algorithm.
#include "sim/pathsearch.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ta::sim;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
    if (!ok) ++fails;
}

// A grid of '.', '#' (wall), 'o' (occupied body, score 4)
struct Grid {
    std::vector<std::string> rows;
    int score(int x, int z) const {
        if (z < 0 || z >= int(rows.size())) return 0;
        if (x < 0 || x >= int(rows[z].size())) return 0;
        char c = rows[z][x];
        if (c == '#') return 0;
        if (c == 'o') return kCellOccupied;
        if (c == '=') return kCellRoad;
        return kCellGround;
    }
};

static PathSearch::Result run(const Grid& g, PathCell s, PathCell e,
                              PathSearch& ps, int budgetPerTick, int maxTicks,
                              int* ticks) {
    ps.reset(int(g.rows[0].size()), int(g.rows.size()));
    ps.start = s; ps.goal = e; ps.cur = s;
    auto sc = [&](int x, int z) { return g.score(x, z); };
    PathSearch::Result r = PathSearch::Result::Suspended;
    *ticks = 0;
    for (int i = 0; i < maxTicks; ++i) {
        ps.work = 0;                       // fresh quantum each tick
        r = ps.step(sc, budgetPerTick);
        ++(*ticks);
        if (r != PathSearch::Result::Suspended &&
            r != PathSearch::Result::Waypoint)
            return r;
    }
    return r;
}

int main() {
    std::printf("[open ground]\n");
    {
        Grid g{{"..........",
                "..........",
                "..........",
                "..........",
                ".........."}};
        PathSearch ps; int t = 0;
        auto r = run(g, {0, 2}, {9, 2}, ps, 12000, 50, &t);
        check(r == PathSearch::Result::Arrived,
              "a clear straight line arrives",
              "ticks=" + std::to_string(t));
    }

    std::printf("[a wall with a gap]\n");
    {
        Grid g{{"....#.....",
                "....#.....",
                "..........",     // the gap
                "....#.....",
                "....#....."}};
        PathSearch ps; int t = 0;
        auto r = run(g, {0, 0}, {9, 0}, ps, 12000, 200, &t);
        check(r == PathSearch::Result::Arrived,
              "a wall with a gap is routed around",
              "ticks=" + std::to_string(t) +
                  " waypoints=" + std::to_string(ps.out.size()));
        // "Arrived" is not enough on its own: the first cut of the breadcrumb
        // backtrack reported success while handing back a 4-cell cycle repeated
        // to the 64-waypoint clamp. Check the route is actually a route.
        bool sane = !ps.out.empty() &&
                    ps.out.back().x == 9 && ps.out.back().z == 0 &&
                    ps.out.size() <= 16;
        for (size_t i = 1; sane && i < ps.out.size(); ++i) {
            int dx = ps.out[i].x - ps.out[i - 1].x;
            int dz = ps.out[i].z - ps.out[i - 1].z;
            // consecutive corners must be joined by a straight run
            if (!(dx == 0 || dz == 0 || std::abs(dx) == std::abs(dz))) sane = false;
        }
        std::string got;
        for (auto& c : ps.out)
            got += "(" + std::to_string(c.x) + "," + std::to_string(c.z) + ")";
        check(sane, "...and the route it hands back is a real path", got);
    }

    std::printf("[a body in the way -- the Monarch case]\n");
    {
        Grid g{{"..........",
                "..........",
                "....o.....",     // one parked body dead ahead
                "..........",
                ".........."}};
        PathSearch ps; int t = 0;
        auto r = run(g, {0, 2}, {9, 2}, ps, 12000, 200, &t);
        check(r == PathSearch::Result::Arrived,
              "a single parked body does not stop the search",
              "ticks=" + std::to_string(t) +
                  " waypoints=" + std::to_string(ps.out.size()));
    }

    std::printf("[fully enclosed]\n");
    {
        Grid g{{"##########",
                "#........#",
                "#...####.#",
                "#...#..#.#",
                "#####..###"}};
        PathSearch ps; int t = 0;
        auto r = run(g, {1, 1}, {6, 3}, ps, 12000, 400, &t);
        check(r == PathSearch::Result::Arrived || r == PathSearch::Result::Failed,
              "an awkward pocket terminates rather than spinning",
              std::string("result=") +
                  (r == PathSearch::Result::Arrived ? "arrived" :
                   r == PathSearch::Result::Failed ? "failed" : "still running"));
    }

    std::printf("[budget slicing]\n");
    {
        Grid g{{"....#.....",
                "....#.....",
                "..........",
                "....#.....",
                "....#....."}};
        PathSearch a, b; int ta = 0, tb = 0;
        auto ra = run(g, {0, 0}, {9, 0}, a, 12000, 400, &ta);   // one big slice
        auto rb = run(g, {0, 0}, {9, 0}, b, 16, 4000, &tb);     // many tiny ones
        bool same = (ra == rb) && (a.out.size() == b.out.size());
        for (size_t i = 0; same && i < a.out.size(); ++i)
            same = a.out[i].x == b.out[i].x && a.out[i].z == b.out[i].z;
        check(same,
              "the result does not depend on how the work is sliced",
              "big-slice ticks=" + std::to_string(ta) +
                  " small-slice ticks=" + std::to_string(tb));
    }

    // The service admits at most kMaxActiveSearches at a time. Everything past
    // that waits, so the things to prove are that nobody starves, that the queue
    // drains, and that a queued request costs no per-cell scratch.
    std::printf("[service: bounded concurrency]\n");
    {
        Grid g{{"....#.....",
                "....#.....",
                "..........",
                "....#.....",
                "....#....."}};
        const int kReqs = kMaxActiveSearches * 4;   // well past the pool
        PathService svc;
        for (int i = 0; i < kReqs; ++i)
            svc.request(1000 + i, {0, 0}, {9, 4}, int(g.rows[0].size()), int(g.rows.size()), 0, 0, false);

        std::vector<bool> served(size_t(kReqs), false);
        auto score = [&](int, int cx, int cz) { return g.score(cx, cz); };
        int ticks = 0, peak = 0;
        while (svc.pendingCount() > 0 && ticks < 4000) {
            svc.tick(score, [&](int id, const std::vector<PathCell>& route,
                                float, float) {
                (void)route;
                int i = id - 1000;
                if (i >= 0 && i < kReqs) served[size_t(i)] = true;
            });
            peak = std::max(peak, int(svc.pendingCount()));
            ++ticks;
        }
        int unserved = 0;
        for (bool b : served) if (!b) ++unserved;
        check(unserved == 0 && svc.pendingCount() == 0,
              "every request past the pool limit is eventually served",
              std::to_string(kReqs) + " requests, unserved=" +
                  std::to_string(unserved) + ", drained in " +
                  std::to_string(ticks) + " ticks");
        check(peak <= kReqs,
              "the queue drains rather than growing",
              "peak pending=" + std::to_string(peak));
        // CHEAP REQUESTS MUST NOT COST A TICK EACH. A slot freed by a search that
        // finished early is refilled within the same tick while work budget remains, so
        // throughput is limited by the BUDGET rather than by the pool size. Before that,
        // this same case took four ticks: exactly kMaxActiveSearches per tick however
        // trivial the searches were, with ~7% of the budget spent.
        check(ticks <= 2,
              "cheap requests are not rationed to one pool-full per tick",
              std::to_string(kReqs) + " served in " + std::to_string(ticks) + " tick(s)");
    }

    // ...and the budget is still a REAL cap. Refilling slots must not turn into
    // admitting everything: enough cheap requests still have to spill into a second
    // tick, or the per-tick budget has stopped meaning anything and a big order becomes
    // a frame spike instead of a short queue.
    {
        Grid g{{"..........",
                "..........",
                "..........",
                "..........",
                ".........."}};
        PathService svc;
        const int kMany = 400;
        for (int i = 0; i < kMany; ++i)
            svc.request(5000 + i, {0, 0}, {9, 4}, int(g.rows[0].size()), int(g.rows.size()), 0, 0, false);
        auto score = [&](int, int cx, int cz) { return g.score(cx, cz); };
        int firstTick = 0, ticks = 0;
        while (svc.pendingCount() > 0 && ticks < 4000) {
            int served = 0;
            svc.tick(score, [&](int, const std::vector<PathCell>&, float, float) { ++served; });
            if (ticks == 0) firstTick = served;
            ++ticks;
        }
        check(firstTick < kMany && ticks > 1,
              "the per-tick budget still bounds how much is served",
              std::to_string(firstTick) + " of " + std::to_string(kMany) +
                  " in the first tick, " + std::to_string(ticks) + " ticks total");
    }

    // Re-requesting for a unit that already holds a slot must restart it in
    // place, not leak the slot -- leak it and the pool starves after a few
    // hundred order changes.
    std::printf("[service: slot reuse]\n");
    {
        Grid g{{"..........",
                "..........",
                ".........."}};
        PathService svc;
        auto score = [&](int, int cx, int cz) { return g.score(cx, cz); };
        for (int round = 0; round < 200; ++round) {
            for (int i = 0; i < kMaxActiveSearches; ++i)
                svc.request(1, {0, 0}, {9, 2}, int(g.rows[0].size()), int(g.rows.size()), 0, 0, false);
            svc.tick(score, [](int, const std::vector<PathCell>&, float, float) {});
        }
        svc.clear();
        // After clear() the pool must be fully available again.
        for (int i = 0; i < kMaxActiveSearches; ++i)
            svc.request(2000 + i, {0, 0}, {9, 2}, int(g.rows[0].size()), int(g.rows.size()), 0, 0, false);
        int done = 0;
        for (int t = 0; t < 200 && svc.pendingCount(); ++t)
            svc.tick(score, [&](int, const std::vector<PathCell>&, float, float) { ++done; });
        check(done == kMaxActiveSearches,
              "slots are handed back on completion, cancel and clear",
              "served " + std::to_string(done) + "/" +
                  std::to_string(kMaxActiveSearches) + " after 200 re-requests");
    }

    // A re-request for the SAME goal from the SAME cell must not throw the running
    // search away. The sim re-asks every kPathRetryTicks for a unit making no headway,
    // and restarting each time meant a search that needed longer than that interval
    // could never finish: measured in a 3300-unit battle, 19,985 requests produced 996
    // completions and 1445 units sat permanently queued.
    std::printf("[service: re-request keeps progress]\n");
    {
        Grid g;                                    // open field, big enough to take work
        for (int z = 0; z < 24; ++z) g.rows.push_back(std::string(48, '.'));
        auto score = [&](int, int cx, int cz) { return g.score(cx, cz); };
        const int W = int(g.rows[0].size()), H = int(g.rows.size());

        // Establish how long this search needs when left alone, with a small budget so
        // it spans many ticks (the point is to re-ask while it is still running).
        PathService base;
        base.setBudget(40);
        base.request(1, {0, 0}, {47, 23}, W, H, 0, 0, false);
        int aloneTicks = 0; bool aloneDone = false;
        for (int t = 0; t < 4000 && !aloneDone; ++t) {
            base.tick(score, [&](int, const std::vector<PathCell>&, float, float) { aloneDone = true; });
            ++aloneTicks;
        }
        check(aloneDone && aloneTicks > 4,
              "the probe search spans several ticks (so re-asking can interrupt it)",
              "finished in " + std::to_string(aloneTicks) + " ticks");

        // Now re-ask every 3 ticks, as a stuck unit does. It must still finish.
        PathService svc;
        svc.setBudget(40);
        bool done = false;
        int ticks = 0;
        for (; ticks < 4000 && !done; ++ticks) {
            if (ticks % 3 == 0)
                svc.request(1, {0, 0}, {47, 23}, W, H, 0, 0, false);   // same goal, same cell
            svc.tick(score, [&](int, const std::vector<PathCell>&, float, float) { done = true; });
        }
        check(done, "a search re-asked every 3 ticks still completes",
              done ? ("finished in " + std::to_string(ticks) + " ticks")
                   : "never completed -- the re-request restarted it each time");
    }

    // ...but a re-request must still adopt the new EXACT destination. The search works
    // in cells, so two different points inside one 16px cell are the same search -- yet
    // goalX/goalZ ride along to the callback, which installs them as the order's point.
    // Keeping the old pair sent the unit to the previous order's spot.
    std::printf("[service: same-cell re-target updates the exact goal]\n");
    {
        Grid g;
        for (int z = 0; z < 8; ++z) g.rows.push_back(std::string(12, '.'));
        auto score = [&](int, int cx, int cz) { return g.score(cx, cz); };
        const int W = int(g.rows[0].size()), H = int(g.rows.size());
        PathService svc;
        svc.setBudget(40);
        svc.request(7, {0, 0}, {9, 5}, W, H, 150.0f, 90.0f, false);   // first order
        svc.request(7, {0, 0}, {9, 5}, W, H, 158.0f, 82.0f, false);   // same cell, new point
        float gotX = -1, gotZ = -1; bool done = false;
        for (int t = 0; t < 4000 && !done; ++t)
            svc.tick(score, [&](int, const std::vector<PathCell>&, float gx, float gz) {
                gotX = gx; gotZ = gz; done = true;
            });
        check(done && gotX == 158.0f && gotZ == 82.0f,
              "the finished route carries the NEWEST exact destination",
              "got (" + std::to_string(gotX) + ", " + std::to_string(gotZ) +
                  "), expected (158, 82)");
    }

    std::printf("\n%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
