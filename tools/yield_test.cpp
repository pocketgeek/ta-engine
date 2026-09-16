// yield_test -- WHICH unit is asked to give way, and what happens when it cannot.
//
// WHY THIS EXISTS. When the yield's neighbour search moved onto the spatial grid, the
// selection changed silently: forEachNear visits cells spatially and, within a cell, in
// reverse insertion order, while the scan it replaced walked units_ forwards -- i.e.
// always the lowest id. That is not merely a different unit standing aside. The chosen
// candidate is then tested for its yield cooldown and the search STOPS either way, so
// picking a cooled-down unit means nobody yields where somebody otherwise would have.
//
// Review asked for this test and the first four attempts at it failed, every one because
// the scenario dissolved before a yield could fire -- units passed each other, or arrived,
// or jammed only once their orders were already gone (the yield sits inside the has-orders
// branch). What produces the state reliably is a corridor exactly one body wide: a
// head-on pair cannot pass, and pathExists is terrain-only so the goal stays "reachable"
// and the leg is never dropped.

#include "sim/sim.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ta::sim;

namespace {

int fails = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("  %-58s %s%s%s\n", what.c_str(), ok ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++fails;
}

UnitType soldier() {
    UnitType t{};
    t.name = "sol"; t.id = "sol";
    t.maxVel = 70; t.turnRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.footX = 2; t.footZ = 2; t.sight = 200;
    return t;
}

// A corridor wide enough for TWO bodies abreast and no more: rows 60..64 open. Two
// candidates side by side are both within the yield's search radius, which is what the
// selection rule needs to be exercised at all.
//
// A one-body corridor cannot do it -- candidates queue up behind one another and the
// radius is only about two body widths, so the far one never qualifies and there is only
// ever one candidate to choose from. The first version of this test made exactly that
// mistake and "failed" on geometry rather than on behaviour.
World* corridorWorld() {
    World* w = new World();
    w->setVisPlayer(-1);
    w->setTerrain(std::vector<uint8_t>(size_t(128) * 128, 100), 128, 128, /*seaLevel=*/20);
    for (int x = 0; x < 128; ++x) {
        w->blockCells(x, 59, 1, 1, true);
        w->blockCells(x, 65, 1, 1, true);
    }
    w->setPathService(true);
    return w;
}

}  // namespace

int main() {
    std::printf("yield_test\n");
    UnitType sol = soldier();

    // TWO QUALIFYING CANDIDATES -> the LOWER id is the one asked to give way. Recorded as
    // the FIRST unit to have yieldT set, so the answer cannot depend on when we look.
    {
        World& w = *corridorWorld();
        // THE LOWER ID IS PLACED IN THE LATER GRID CELL, deliberately. forEachNear walks
        // cells in a fixed spatial order, so "first grid match" and "lowest id" only give
        // different answers when the lower id sits in a cell the sweep reaches second.
        // Put the lower id first and both rules agree -- the test then passes against the
        // very bug it exists for, which is what the first version of it did.
        const int lowA  = w.spawn(&sol, 1040, 1022, 0, 1);   // id 1, LATER cell
        const int lowB  = w.spawn(&sol, 1040,  990, 0, 1);   // id 2, earlier cell
        const int mover = w.spawn(&sol, 1092, 1006, 0, 0);   // highest id: it proceeds
        w.order(lowA, 1600, 1022, false);
        w.order(lowB, 1600,  990, false);
        w.order(mover, 600, 1006, false);                    // head-on
        int firstYielder = -1;
        for (int i = 0; i < 30 * 20 && firstYielder < 0; ++i) {
            w.tick(1.0f / 30.0f);
            for (int id : {lowA, lowB, mover}) {
                const Unit* q = w.unit(id);
                if (q && q->yieldT > 0.0f) { firstYielder = id; break; }
            }
        }
        check(firstYielder == lowA,
              "of two candidates, the LOWER id is asked to yield",
              firstYielder < 0 ? "nobody yielded" : "id " + std::to_string(firstYielder));
        check(firstYielder != mover, "...and never the unit that is proceeding");
        delete &w;
    }

    // A COOLDOWN IS RESPECTED. Isolated to ONE candidate on purpose: with two, the pair
    // jostles, the lower one drifts out of the search radius within a few seconds, and
    // the higher one becomes the lowest QUALIFYING candidate quite legitimately -- so an
    // assertion about "the duty is not passed on" cannot tell a real regression from the
    // geometry moving. One candidate and one mover holds still enough to mean something.
    {
        for (int cooled = 0; cooled < 2; ++cooled) {
            World& w = *corridorWorld();
            const int low   = w.spawn(&sol, 1040, 1006, 0, 1);
            const int mover = w.spawn(&sol, 1092, 1006, 0, 0);
            w.order(low, 1600, 1006, false);
            w.order(mover, 600, 1006, false);
            bool yielded = false;
            for (int i = 0; i < 30 * 20; ++i) {
                if (cooled)
                    if (Unit* a = w.unit(low)) { a->yieldCool = 99.0f; a->yieldT = 0.0f; }
                w.tick(1.0f / 30.0f);
                if (const Unit* a = w.unit(low); a && a->yieldT > 0.0f) yielded = true;
            }
            if (cooled)
                check(!yielded, "a candidate on cooldown is not asked to yield");
            else
                check(yielded, "...and the same candidate off cooldown is");
            delete &w;
        }
    }

    std::printf(fails ? "yield_test: %d FAILED\n" : "yield_test: all passed\n", fails);
    return fails ? 1 : 0;
}
