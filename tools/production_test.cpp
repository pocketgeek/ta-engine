// production_test -- what a factory does with the units it makes.
//
// Two reported bugs and one requested feature, all in tickProduction.
//
// WHERE UNITS GO. Both the spawn point and the walk-off point used to be fixed
// offsets from the building: units emerged on one tile (production waited up to 2.5s
// for it to clear, then spawned there anyway) and were ordered to one of five points
// keyed on `id % 5`. So the sixth unit out of a factory was ordered onto the spot the
// first was still standing on. Bodies are solid and the steering has no local
// avoidance -- routing around things is the router's job, and the router cannot route
// INTO an occupied cell either -- so it pushed at its own countryman for ever. The
// no-headway watchdog does not rescue it: replaceLeg resets the tracker and the router
// re-issues about once a second, so the tracker never builds up (see the KNOWN
// LIMITATION note on replaceLeg). Reported as "built units always try to go to the
// same spot, forever".
//
// Measured on this test's 24-unit run, before and after: 15 stuck / 41 overlapping
// pairs -> 0 and 0. The counts are what this file guards, because the failure is
// statistical -- one unlucky unit proves nothing, a fifth of the output stuck proves
// the mechanism.
//
// RALLY. A production building now accepts move / fight-move / patrol / attack orders
// and queues them; it cannot act on them itself, so they describe what its OUTPUT
// should do. The orders are kept in Unit::rally, deliberately NOT in `orders`: a
// structure must never enter the mover, which turns a unit's heading toward its goal
// and is how right-clicking used to spin a keep. The heading assertion below is what
// keeps that from regressing.

#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace tak::sim;

static int g_fail = 0;
static void check(bool cond, const char* what, const std::string& detail = {}) {
    std::printf("  %-62s %s%s%s\n", what, cond ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!cond) ++g_fail;
}

static UnitType factoryType() {
    UnitType t{};
    t.name = "factory"; t.id = "factory";
    t.maxVel = 0;                 // maxVel <= 0 => isStructure()
    t.maxHp = 5000;
    t.footX = 7; t.footZ = 12;
    t.isBuilder = true;
    t.workerTime = 1000;          // builds fast, so the test is short
    return t;
}

static UnitType soldierType() {
    UnitType t{};
    t.name = "sol"; t.id = "sol";
    t.maxVel = 60; t.turnRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.buildTime = 1;
    t.footX = 2; t.footZ = 2;
    return t;
}

static World* makeWorld(int W, int H) {
    World* w = new World();
    w->setVisPlayer(-1);                        // headless, like the referee
    w->setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, /*seaLevel=*/20);
    w->setPathService(true);                    // the real router, as in a game
    return w;
}

static void run(World& w, float seconds) {
    for (int i = 0; i < int(seconds * 30.0f); ++i) w.tick(1.0f / 30.0f);
}

// ---------------------------------------------------------------------------
// 1. A factory running flat out must not pile its output up on one spot.
// ---------------------------------------------------------------------------
static void outputDoesNotJam() {
    std::printf("a factory's output does not jam on one spot:\n");
    World& w = *makeWorld(128, 128);
    UnitType fac = factoryType(), sol = soldierType();
    const int fid = w.spawn(&fac, 1000, 1000, 0, 0);
    w.player(0).mana = 1e9f;
    w.train(fid, &sol, 24);
    run(w, 180.0f);

    // Snapshot, run on, and see who moved. A unit that still holds orders but has not
    // moved is one pushing at something it will never get past.
    std::vector<std::pair<int, std::pair<float, float>>> was;
    for (const auto& u : w.units())
        if (u.alive() && u.type && !u.type->isStructure()) was.push_back({u.id, {u.x, u.z}});
    run(w, 10.0f);

    int total = 0, stuck = 0;
    for (const auto& u : w.units()) {
        if (!u.alive() || !u.type || u.type->isStructure()) continue;
        ++total;
        if (u.orders.empty()) continue;
        for (auto& [id, p] : was)
            if (id == u.id) {
                const float dx = u.x - p.first, dz = u.z - p.second;
                if (std::sqrt(dx * dx + dz * dz) < 8.0f) ++stuck;
                break;
            }
    }
    check(total >= 20, "the factory actually produced its queue",
          std::to_string(total) + " units");
    check(stuck == 0, "no unit is left pushing at a spot it cannot reach",
          std::to_string(stuck) + " of " + std::to_string(total) + " stuck");

    // ...and they are not standing inside one another. Bodies are solid and nothing
    // pulls an overlap apart, so a stack made at spawn time is permanent.
    int overlaps = 0;
    std::vector<const Unit*> live;
    for (const auto& u : w.units())
        if (u.alive() && u.type && !u.type->isStructure()) live.push_back(&u);
    for (size_t a = 0; a < live.size(); ++a)
        for (size_t b = a + 1; b < live.size(); ++b) {
            const float need = (float(std::max(live[a]->type->footX, live[a]->type->footZ)) +
                                float(std::max(live[b]->type->footX, live[b]->type->footZ)))
                               * 8.0f * 0.5f;
            const float dx = live[a]->x - live[b]->x, dz = live[a]->z - live[b]->z;
            if (std::sqrt(dx * dx + dz * dz) < need * 0.6f) ++overlaps;
        }
    check(overlaps == 0, "and no two of them are standing inside each other",
          std::to_string(overlaps) + " overlapping pairs");
    delete &w;
}

// ---------------------------------------------------------------------------
// 2. A queued rally on the building is adopted by everything it makes.
// ---------------------------------------------------------------------------
static void rallyIsAdopted() {
    std::printf("a production building's rally orders:\n");
    World& w = *makeWorld(160, 160);
    UnitType fac = factoryType(), sol = soldierType();
    check(fac.producesUnits(), "a builder structure can hold a rally");
    check(!sol.producesUnits(), "an ordinary unit cannot");

    const int fid = w.spawn(&fac, 600, 600, 0, 0);
    w.player(0).mana = 1e9f;
    w.train(fid, &sol, 6);                       // queue production FIRST...
    w.order(fid, 1600, 600, /*queue=*/false);    // ...then a two-step rally
    w.attackMove(fid, 1600, 200, /*queue=*/true);

    const Unit* f = w.unit(fid);
    check(f->rally.size() == 2, "both rally steps are stored, queued",
          std::to_string(f->rally.size()));
    check(f->rally.size() == 2 && f->rally[1].attackMove,
          "and the second one kept its fight-move flag");
    // The building must NEVER take these as its own orders: the mover turns a unit's
    // heading toward its goal, so a structure in the mover is a spinning keep.
    check(f->orders.empty(), "the building's own order queue stays empty",
          std::to_string(f->orders.size()));
    check(f->heading == 0.0f, "and its heading is untouched",
          std::to_string(f->heading));
    // Setting a rally must not cancel what it is building.
    check(!f->buildQueue.empty(), "the production queue survives the rally order",
          std::to_string(f->buildQueue.size()) + " queued");

    run(w, 240.0f);
    int n = 0, arrived = 0;
    for (const auto& u : w.units()) {
        if (!u.alive() || !u.type || u.type->isStructure()) continue;
        ++n;
        const float dx = u.x - 1600.0f, dz = u.z - 200.0f;
        if (std::sqrt(dx * dx + dz * dz) < 220.0f) ++arrived;
    }
    check(n >= 5, "the units were produced", std::to_string(n));
    check(n > 0 && arrived == n, "every one of them followed the rally to its last step",
          std::to_string(arrived) + " of " + std::to_string(n));
    delete &w;
}

// ---------------------------------------------------------------------------
// 3. A rally set BEFORE anything is built still applies, and re-setting replaces.
// ---------------------------------------------------------------------------
static void rallyReplaces() {
    std::printf("re-setting a rally replaces it (unqueued):\n");
    World& w = *makeWorld(160, 160);
    UnitType fac = factoryType(), sol = soldierType();
    const int fid = w.spawn(&fac, 600, 600, 0, 0);
    w.player(0).mana = 1e9f;
    w.order(fid, 1600, 600, false);
    w.order(fid, 300, 1400, false);              // unqueued: replaces, not appends
    check(w.unit(fid)->rally.size() == 1, "an unqueued rally order replaces the plan",
          std::to_string(w.unit(fid)->rally.size()));
    w.train(fid, &sol, 3);
    run(w, 240.0f);
    int n = 0, arrived = 0;
    for (const auto& u : w.units()) {
        if (!u.alive() || !u.type || u.type->isStructure()) continue;
        ++n;
        const float dx = u.x - 300.0f, dz = u.z - 1400.0f;
        if (std::sqrt(dx * dx + dz * dz) < 220.0f) ++arrived;
    }
    check(n > 0 && arrived == n, "and the output follows the NEW plan",
          std::to_string(arrived) + " of " + std::to_string(n));
    delete &w;
}

int main() {
    std::printf("production_test\n");
    outputDoesNotJam();
    rallyIsAdopted();
    rallyReplaces();
    std::printf(g_fail ? "production_test: %d FAILURE(S)\n" : "production_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
