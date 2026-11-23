// Does the ported search actually get around things? Synthetic grids, so the
// only thing under test is the algorithm.
#include "sim/pathsearch.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace tak::sim;

namespace tak::sim {
struct RetailReplayProbe {
    static bool counts(const PathService& service) {
        std::array<int,10> pending{},priority{};
        for(const auto& [id,entry]:service.q_) {
            ++pending[size_t(entry.player)];priority[size_t(entry.player)]+=entry.priority;
        }
        return pending==service.pendingByPlayer_ && priority==service.priorityByPlayer_;
    }
};
}

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
    if (!ok) ++fails;
}

// A grid of '.', '#' (wall), 'o' (a parked body). Retail grades a parked body 0 --
// the same as a wall (icd 0x509020) -- so the search treats the two identically and
// routes around either; 'o' is kept distinct in the fixtures only so they still read
// as "a body here", and the Monarch case below asserts the route bends around one.
struct Grid {
    std::vector<std::string> rows;
    int score(int x, int z) const {
        if (z < 0 || z >= int(rows.size())) return 0;
        if (x < 0 || x >= int(rows[z].size())) return 0;
        char c = rows[z][x];
        if (c == '#') return 0;
        if (c == 'o') return kCellImpassable;
        if (c == '=') return kCellRoad;
        if (c == 't') return kCellSameWay;
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
    {
        PathService service;service.setBudget(80);
        uint32_t rng=31;
        auto next=[&]{rng=rng*1664525u+1013904223u;return rng;};
        bool valid=true;int delivered=0;
        auto tick=[&]{service.tick([](int,int x,int z){return x<0 || z<0 || x>=24 || z>=24 ? 0:6;},
            [&](int,const std::vector<PathCell>&,Fixed,Fixed,bool,bool,bool,bool){++delivered;});};
        for(int i=0;i<4000 && valid;++i) {
            const int id=int(next()%64),owner=int(next()%10);
            const int operation=int(next()%12);
            if(operation==0)service.clear();
            else if(operation<4)service.cancel(id);
            else {
                const int goal=operation<8 ? 20 : 18;
                service.request(id,{2,2},{goal,20},24,24,Fixed::fromInt(goal*16),Fixed::fromInt(320),owner,false);
                if(operation&1)service.request(id,{2,2},{goal,20},24,24,
                    Fixed::fromInt(goal*16),Fixed::fromInt(320),owner,true);
            }
            valid=RetailReplayProbe::counts(service);
            tick();valid &= RetailReplayProbe::counts(service);
        }
        service.setBudget(12000);
        for(int i=0;i<1000 && service.pendingCount();++i){tick();valid &= RetailReplayProbe::counts(service);}
        check(valid && delivered>0 && !service.pendingCount(),
              "scheduler counts survive promotions, replacements, owner changes, cancellations, clear and completion");
    }
    {
        // Compare phase-2 handoff with the former reset-then-replace sequence,
        // including trace flags/directions outside the cells visited by cost search.
        std::vector<RetailCostSearch::Cell> plane(32*32);
        for(size_t i=0;i<plane.size();++i)plane[i].direction=uint8_t(i&7);
        plane[30*32+30].flags=4;
        plane[3*32+3].flags=0x18;
        RetailCostSearch reference,transferred;
        reference.reset(32,32,3*32+3,2,500);
        reference.cells=plane;
        reference.cells[3*32+3].flags|=1;
        reference.cells[3*32+3].direction=2;
        reference.cells[3*32+3].node=0;
        const auto* storage=plane.data();
        transferred.reset(32,32,3*32+3,2,500,98304,std::move(plane));
        bool equal=transferred.cells.data()==storage;
        auto same=[&] {
            if(reference.heap!=transferred.heap || reference.processed!=transferred.processed ||
               reference.endpoint!=transferred.endpoint)return false;
            for(size_t i=0;i<reference.cells.size();++i) {
                const auto& a=reference.cells[i];const auto& b=transferred.cells[i];
                if(a.flags!=b.flags || a.direction!=b.direction || a.node!=b.node)return false;
            }
            return true;
        };
        equal &= same();
        auto grade=[](int x,int z){return x==15 && z<25 ? 0:6;};
        auto distance=[](int x,int z){return retailGoalDistance(30-x,30-z);};
        for(int i=0;i<600 && !reference.empty();++i) {
            const auto a=reference.pop(grade,distance,0,4);
            const auto b=transferred.pop(grade,distance,0,4);
            equal &= a==b && same();
            if(a!=RetailCostSearch::Result::Searching)break;
        }
        check(equal,"phase-2 transfers trace storage without changing search state or expansion");
    }
    std::printf("[retail direction boundaries]\n");
    check(pathDirFromDelta(12, 5) == 5 && pathDirFromDelta(13, 5) == 6,
          "24:10 boundary stays diagonal; beyond it becomes cardinal");
    check(pathDirFromDelta(-12, 5) == 3 && pathDirFromDelta(-13, 5) == 2,
          "negative x uses the same direction threshold");
    check(pathDirFromDelta(5, -12) == 7 && pathDirFromDelta(5, -13) == 0,
          "vertical threshold preserves the north diagonal");
    check(pathDirFromDelta(0, 0) == 5, "zero delta retains retail direction 5");
    std::printf("[traffic relative to search origin]\n");
    for (const auto& traffic : std::vector<std::vector<int>>{{3}, {19}, {3, 4}, {3, 19}}) {
        Grid g{{std::string(24, '#'), std::string(24, '.'), std::string(24, '#')}};
        for (int x : traffic) g.rows[1][size_t(x)] = 't';
        PathSearch ps; ps.tolCells = 8;
        int ticks = 0;
        auto result = run(g, {2, 1}, {20, 1}, ps, 12000, 100, &ticks);
        const bool near = traffic.front() == 3;
        const bool far = traffic.front() == 19 || traffic.size() > 1;
        check(result == PathSearch::Result::Arrived, "traffic corridor reaches its destination");
        check(ps.goalCrowded == near && ps.sawTraffic == far,
              "delivered route preserves retail origin-relative traffic flags",
              "first traffic=" + std::to_string(traffic.front()) + " count=" + std::to_string(traffic.size()));
        check(ps.visNear == near && ps.visFar == far,
              "trace visit flags measure from origin and latch subsequent traffic");
    }
    std::printf("[unreachable destinations]\n");
    {
        Grid g{{"....#.....", "....#.....", "....#.....", "....#.....", "....#....."}};
        PathSearch ps; int ticks = 0;
        auto result = run(g, {0, 2}, {9, 2}, ps, 12000, 100, &ticks);
        check(result == PathSearch::Result::Failed, "a sealed wall reports failure");
        check(!ps.out.empty() && ps.out.back().x == ps.bestCell.x && ps.out.back().z == ps.bestCell.z,
              "failed search ends at its reachable closest approach");
        bool reachable = true;
        for (auto point : ps.out) reachable &= point.x < 4 && g.score(point.x, point.z) >= kCellThreshold;
        check(reachable, "failed route never appends the destination across the wall");

        Grid boxed{{"#####", "#.###", "#####", "###.#", "#####"}};
        result = run(boxed, {1, 1}, {3, 3}, ps, 12000, 100, &ticks);
        check(result == PathSearch::Result::Failed && ps.out.empty(),
              "boxed-in start produces no fabricated straight segment");
    }
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

    // Admission may consume the last scan budget before execution starts.
    // A suspended request must not call the admission hook again.
    {
        PathService svc;
        svc.setBudget(7);
        svc.setEntityPool(0,1,1);
        svc.request(1,{0,0},{7,7},8,8,Fixed::fromInt(112),Fixed::fromInt(112),0,false);
        int admissions=0,stamp=0;
        const auto score=[](int,int,int) { return 6; };
        const auto done=[](int,const std::vector<PathCell>&,Fixed,Fixed,bool,bool,bool,bool) {};
        const auto admit=[&](int id,uint32_t tick) {
            if (tick<15) return false;
            ++admissions; stamp=int(tick); return id==1;
        };
        for (uint32_t tick=1;tick<=15;++tick) svc.tick(score,done,{},tick,admit);
        check(admissions==1 && stamp==15 && svc.pending(1) && svc.requests()==0,
              "admission is stamped before a budget-suspended search starts");
        svc.tick(score,done,{},16,admit);
        check(admissions==1 && stamp==15 && svc.requests()==1,
              "resuming an admitted search preserves its admission stamp");
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
            svc.request(1000 + i, {0, 0}, {9, 4}, int(g.rows[0].size()), int(g.rows.size()), Fixed::fromInt(0), Fixed::fromInt(0), 0, false);

        std::vector<bool> served(size_t(kReqs), false);
        auto score = [&](int, int cx, int cz) { return g.score(cx, cz); };
        int ticks = 0, peak = 0;
        while (svc.pendingCount() > 0 && ticks < 4000) {
            svc.tick(score, [&](int id, const std::vector<PathCell>& route,
                                Fixed, Fixed, bool, bool, bool, bool) {
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
        // Every retail initialization costs 500 before any probing. The old
        // two-tick limit assumed initialization was free and is not a retail
        // throughput invariant. Verify that the mandatory work is accounted.
        check(svc.workSpent() >= uint64_t(kReqs)*500 && ticks < kReqs,
              "initialization is charged and several requests can finish per tick",
              std::to_string(svc.workSpent())+" work in "+std::to_string(ticks)+" ticks");
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
            svc.request(5000 + i, {0, 0}, {9, 4}, int(g.rows[0].size()), int(g.rows.size()), Fixed::fromInt(0), Fixed::fromInt(0), 0, false);
        auto score = [&](int, int cx, int cz) { return g.score(cx, cz); };
        int firstTick = 0, ticks = 0;
        while (svc.pendingCount() > 0 && ticks < 4000) {
            int served = 0;
            svc.tick(score, [&](int, const std::vector<PathCell>&, Fixed, Fixed, bool, bool, bool, bool) { ++served; });
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
                svc.request(1, {0, 0}, {9, 2}, int(g.rows[0].size()), int(g.rows.size()), Fixed::fromInt(0), Fixed::fromInt(0), 0, false);
            svc.tick(score, [](int, const std::vector<PathCell>&, Fixed, Fixed, bool, bool, bool, bool) {});
        }
        svc.clear();
        // After clear() the pool must be fully available again.
        for (int i = 0; i < kMaxActiveSearches; ++i)
            svc.request(2000 + i, {0, 0}, {9, 2}, int(g.rows[0].size()), int(g.rows.size()), Fixed::fromInt(0), Fixed::fromInt(0), 0, false);
        int done = 0;
        for (int t = 0; t < 200 && svc.pendingCount(); ++t)
            svc.tick(score, [&](int, const std::vector<PathCell>&, Fixed, Fixed, bool, bool, bool, bool) { ++done; });
        check(done == kMaxActiveSearches,
              "slots are handed back on completion, cancel and clear",
              "served " + std::to_string(done) + "/" +
                  std::to_string(kMaxActiveSearches) + " after 200 re-requests");
    }

    // A partial-search notification belongs to its controller and precedes
    // route delivery. This is what lets a mission cancel a phase-2 search.
    {
        PathService svc;
        svc.setBudget(1);
        auto request = [&](uint64_t owner) {
            svc.request(1,{1,10},{18,10},20,20,Fixed::fromInt(288),Fixed::fromInt(160),
                        0,false,0,0,{},{},0,owner);
        };
        auto score = [](int, int x, int z) {
            return x<0 || z<0 || x>=20 || z>=20 || x==10 ? 0 : int(kCellGround);
        };
        bool done=false, early=false;
        auto delivered = [&](int,const std::vector<PathCell>&,Fixed,Fixed,bool,bool,bool,bool) { done=true; };
        request(101);
        for (int tick=0; tick<1000 && !done && !early; ++tick) {
            svc.tick(score,delivered);
            for (const auto& n:svc.takeNotifications())
                early |= n.unitId==1 && n.controller==101 && n.events==0x2000 && svc.pending(1);
        }
        check(early && !done, "partial search notifies its controller before route completion");
        const auto admissions=svc.requests();
        request(101);
        svc.tick(score,delivered);
        check(svc.requests()==admissions, "same controller re-request retains the running search");
        request(102);
        const bool retiredNotifications=svc.takeNotifications().empty();
        // A one-unit budget can expire scanning an empty entity slot before
        // the replacement is admitted on the following scheduler pass.
        for (int tick=0; tick<20 && svc.requests()==admissions; ++tick)
            svc.tick(score,delivered);
        check(retiredNotifications && svc.requests()==admissions+1 && svc.takeNotifications().empty(),
              "new controller restarts same-goal search without old notifications");
        svc.cancel(1);
        for (int tick=0; tick<10; ++tick) svc.tick(score,delivered);
        check(!done && !svc.pending(1) && svc.takeNotifications().empty(),
              "mission cancellation prevents later delivery of its retired search");
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
        base.request(1, {0, 0}, {47, 23}, W, H, Fixed::fromInt(0), Fixed::fromInt(0), 0, false);
        int aloneTicks = 0; bool aloneDone = false;
        for (int t = 0; t < 4000 && !aloneDone; ++t) {
            base.tick(score, [&](int, const std::vector<PathCell>&, Fixed, Fixed, bool, bool, bool, bool) { aloneDone = true; });
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
                svc.request(1, {0, 0}, {47, 23}, W, H, Fixed::fromInt(0), Fixed::fromInt(0), 0, false);   // same goal, same cell
            svc.tick(score, [&](int, const std::vector<PathCell>&, Fixed, Fixed, bool, bool, bool, bool) { done = true; });
        }
        check(done, "a search re-asked every 3 ticks still completes",
              done ? ("finished in " + std::to_string(ticks) + " ticks")
                   : "never completed -- the re-request restarted it each time");
    }

    // A pending search is left alone however far its unit has moved. Native
    // 4e4f50(1) returns at 4e4f68 once the navigator's pending bit is set, and
    // 416430 reads the start only when it admits a request. Production setup:
    // no fixture entity pool. A rectangle/ring destination is the controller
    // shape itself, so the anchor cell projected from the unit may move too.
    std::printf("[service: pending re-request after drift keeps the search]\n");
    for (int shape = 0; shape < 3; ++shape) for (int drift : {2, 9}) {
        PathService svc;
        svc.setBudget(40);
        int finishes = 0;
        svc.setGradeHost({{}, {}, [&](int) { ++finishes; }});
        auto score = [](int, int x, int z) { return x<0 || z<0 || x>=48 || z>=24 ? 0 : int(kCellGround); };
        const RetailRectGoal rect{40, 44, 8, 12};
        const RetailRingGoal ring{42, 10, 2, 40, retailCircleRadiusSquared(40)};
        auto ask = [&](int x, PathCell goal) {
            svc.request(1, {x, 10}, goal, 48, 24, Fixed::fromInt(goal.x*16), Fixed::fromInt(goal.z*16),
                        0, false, 0, 0, {}, {}, 4, 77, 0,
                        shape==1 ? std::optional{rect} : std::nullopt,
                        shape==2 ? std::optional{ring} : std::nullopt);
        };
        ask(2, {40, 10});
        bool done = false;
        auto delivered = [&](int, const std::vector<PathCell>&, Fixed, Fixed, bool, bool, bool, bool) { done = true; };
        for (int t = 0; t < 3; ++t) svc.tick(score, delivered);
        const bool running = svc.requests() == 1 && !done && svc.pending(1);
        // The anchor is a projection only for the shaped controllers.
        ask(2 + drift, shape ? PathCell{40, 10 + drift % 3} : PathCell{40, 10});
        const bool kept = svc.requests() == 1 && finishes == 0 && svc.pending(1);
        for (int t = 0; t < 4000 && !done; ++t) svc.tick(score, delivered);
        check(running && kept && done && svc.requests() == 1,
              std::string(shape==0 ? "circle" : shape==1 ? "rectangle" : "ring") +
                  " search survives a re-request after " + std::to_string(drift) + "-cell drift",
              "running=" + std::to_string(running) + " kept=" + std::to_string(kept) +
                  " admissions=" + std::to_string(svc.requests()) + " finishes=" + std::to_string(finishes));
        // A different destination, by contrast, is a different question.
        if (shape == 0) {
            ask(2, {40, 10});
            svc.tick(score, delivered);
            const auto before = svc.requests();
            ask(4, {40, 14});
            for (int t = 0; t < 20 && svc.requests() == before; ++t) svc.tick(score, delivered);
            check(svc.requests() == before + 1, "a changed destination restarts the pending search");
        }
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
        svc.request(7, {0, 0}, {9, 5}, W, H, Fixed::fromFloat(150.0f), Fixed::fromFloat(90.0f), 0, false);   // first order
        svc.request(7, {0, 0}, {9, 5}, W, H, Fixed::fromFloat(158.0f), Fixed::fromFloat(82.0f), 0, false);   // same cell, new point
        float gotX = -1, gotZ = -1; bool done = false;
        for (int t = 0; t < 4000 && !done; ++t)
            svc.tick(score, [&](int, const std::vector<PathCell>&, Fixed gx, Fixed gz, bool, bool, bool, bool) {
                gotX = gx.toFloat(); gotZ = gz.toFloat(); done = true;
            });
        check(done && gotX == 158.0f && gotZ == 82.0f,
              "the finished route carries the NEWEST exact destination",
              "got (" + std::to_string(gotX) + ", " + std::to_string(gotZ) +
                  "), expected (158, 82)");
    }

    {
        PathService svc;
        svc.request(1,{2,2},{20,20},24,24,{}, {},0,false,0,0,{},{},0,123);
        bool delivered=false,partial=true,notified=false;
        for (int tick=0;tick<100 && !delivered;++tick) {
            svc.tick([](int,int x,int z){return x==2 && z==2 ? 6:0;},
                [&](int,const std::vector<PathCell>& route,Fixed,Fixed,bool failed,bool,bool,bool) {
                    delivered=route.empty();partial=failed;
                });
            for (const auto& event:svc.takeNotifications()) notified|=event.events==0x2000;
        }
        check(delivered && !partial && notified && svc.failures()==1 && !svc.pending(1),
              "boxed-in search notifies failure without inventing a partial-route flag");
    }
    // A build site blocks its interior. Any reachable perimeter cell is a
    // valid approach, even when the initially selected corner is obstructed.
    for (PathCell offset : {PathCell{0,0},PathCell{1,1},PathCell{1,3},PathCell{3,1}}) {
        PathService svc;
        const RetailRectGoal rectangle{10,14,8,12};
        svc.request(1,{2+offset.x,10+offset.z},{10+offset.x,10+offset.z},24,24,
                    Fixed::fromInt(160),Fixed::fromInt(160),0,false,
                    0,0,{},offset,0,55,0,rectangle);
        bool delivered=false;
        for (int tick=0;tick<100 && !delivered;++tick)
            svc.tick([&](int,int x,int z) {
                x-=offset.x; z-=offset.z;
                if (x<0 || z<0 || x>=24 || z>=24) return 0;
                return (x>=10 && x<=14 && z>=9 && z<=11) ? 0 : 6;
            },[&](int,const std::vector<PathCell>& route,Fixed,Fixed,bool failed,bool,bool,bool) {
                delivered=true;
                check(!failed && !route.empty() && rectangle.accepts(route.back().x-offset.x,route.back().z-offset.z),
                      "rectangular approach finds a reachable edge instead of the blocked initial point");
            });
        check(delivered,"rectangular approach completes with footprint coordinate offset");
    }
    {
        PathService svc;
        svc.request(1,{2,10},{20,10},24,24,Fixed::fromInt(320),Fixed::fromInt(160),0,false);
        bool delivered=false;
        for (int tick=0;tick<100 && !delivered;++tick)
            svc.tick([](int,int x,int z) {
                return x<0 || z<0 || x>=24 || z>=24 || (x==10 && z<19) ? 0 : 6;
            },[&](int,const std::vector<PathCell>& route,Fixed,Fixed,bool failed,bool,bool,bool detour) {
                delivered=true;
                check(!failed && detour && !route.empty(),
                      "successful long detour retains its separate delivery outcome");
            });
        check(delivered,"long detour search completes");
    }
    // Cached grading belongs to the singleton worker, including cancellation
    // after admission but before initialization has consumed any work.
    {
        PathService svc;
        svc.setBudget(7);
        int prepared=0,finished=0,active=-1;
        bool valid=true;
        svc.setGradeHost({
            [&](int id,bool) { valid &= active<0 || active==id; active=id; ++prepared; },
            [&](int id,int x,int z,int,PathCell) {
                valid &= active==id;
                return x<0 || z<0 || x>=24 || z>=24 ? 0 : 6;
            },
            [&](int id) { valid &= active==id; active=-1; ++finished; }
        });
        auto request=[&](int id) { svc.request(id,{2,2},{20,20},24,24,
            Fixed::fromInt(320),Fixed::fromInt(320),0,false); };
        auto tick=[&] { svc.tick([](int,int,int) { return 0; },
            [](int,const std::vector<PathCell>&,Fixed,Fixed,bool,bool,bool,bool) {}); };
        request(1); tick();
        check(prepared==0,"admission may consume the budget before cache initialization");
        svc.cancel(1);
        request(2);
        for (int n=0;n<20 && !prepared;++n) tick();
        check(valid && prepared==1 && active==2 && finished==0,
              "cancelling an admitted request leaves the cache with its replacement owner");
        svc.cancel(2);
        check(valid && finished==1 && active==-1,"active cancellation finishes its cache exactly once");
        request(1); request(2); svc.setBudget(10000);
        for (int n=0;n<100 && svc.pendingCount();++n) tick();
        check(valid && !svc.pendingCount() && finished==3 && active==-1,
              "successive searches prepare and finish a single shared cache");
    }
    for(bool cancel:{false,true}) {
        PathService svc;svc.setBudget(cancel ? 8 : 12000);
        bool finished=false;
        auto request=[&](int id) {svc.request(id,{2,2},{20,20},24,24,{}, {},0,false);};
        svc.setGradeHost({{}, {}, [&](int id) {
            if(id!=1)return;
            finished=true;
            // Force the lookup table to grow while the original entry is held.
            for(int other=10;other<522;++other)request(other);
        }});
        request(1);
        auto tick=[&] {svc.tick([](int,int x,int z){return x<0 || z<0 || x>=24 || z>=24 ? 0:6;},
            [](int,const std::vector<PathCell>&,Fixed,Fixed,bool,bool,bool,bool){});};
        if(cancel) {
            for(int n=0;n<20 && !svc.requests();++n)tick();
            svc.cancel(1);
        } else for(int n=0;n<20 && svc.pending(1);++n)tick();
        check(finished && !svc.pending(1) && svc.pendingCount()==512 && RetailReplayProbe::counts(svc),
              cancel ? "cancellation callback can grow the request table" :
                       "completion callback can grow the request table");
    }
    std::printf("\n%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
