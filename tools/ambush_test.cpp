// ambush_test -- SetMission "wa": hold until a non-allied unit is within sight.
//
// WHY THIS EXISTS. The threat check scanned every unit in the world, for every ambushing
// unit, every tick -- O(n^2) in the number of ambushers, paid on the referee as well as
// on every client. Routing it through the spatial grid is meant to be an EXACT
// substitution: the grid holds all alive units, buildings included, and the precise
// distance test is unchanged. Nothing covered this verb, so there was nothing to say
// whether the substitution preserved it. This does.

#include "sim/sim.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace tak::sim;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("  %-58s %s%s%s\n", what.c_str(), ok ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++fails;
}

static UnitType soldier() {
    UnitType t{};
    t.name = "sol"; t.id = "sol";
    t.maxVel = 70; t.turnRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.footX = 2; t.footZ = 2;
    t.sight = 200;
    return t;
}

static UnitType tower() {
    UnitType t{};
    t.name = "twr"; t.id = "twr";
    t.maxVel = 0;                      // isStructure() == maxVel <= 0
    t.maxHp = 500;
    t.footX = 3; t.footZ = 3;
    return t;
}

static World* makeWorld() {
    World* w = new World();
    w->setVisPlayer(-1);
    w->setTerrain(std::vector<uint8_t>(size_t(128) * 128, 100), 128, 128, /*seaLevel=*/20);
    return w;
}

static void run(World& w, float seconds) {
    for (int i = 0; i < int(seconds * 30); ++i) w.tick(1.0f / 30.0f);
}

int main() {
    std::printf("ambush_test\n");
    UnitType sol = soldier(), twr = tower();

    // FAR ENEMY: the ambush holds. 200 sight, enemy at 1200px.
    {
        World& w = *makeWorld();
        const int amb = w.spawn(&sol, 1000, 1000, 0, 0);
        w.spawn(&sol, 2200, 1000, 0, 1);
        w.orderWaitAttack(amb, false);
        run(w, 3.0f);
        const Unit* u = w.unit(amb);
        check(u && !u->orders.empty(), "a distant enemy does not spring the ambush",
              u ? std::to_string(u->orders.size()) + " order(s) held" : "gone");
        delete &w;
    }

    // NEAR ENEMY: it springs.
    {
        World& w = *makeWorld();
        const int amb = w.spawn(&sol, 1000, 1000, 0, 0);
        w.spawn(&sol, 1120, 1000, 0, 1);          // 120px, inside 200 sight
        w.orderWaitAttack(amb, false);
        run(w, 3.0f);
        const Unit* u = w.unit(amb);
        check(u && u->orders.empty(), "an enemy within sight springs it",
              u ? std::to_string(u->orders.size()) + " order(s) left" : "gone");
        delete &w;
    }

    // AN ALLY DOES NOT SPRING IT, however close.
    {
        World& w = *makeWorld();
        const int amb = w.spawn(&sol, 1000, 1000, 0, 0);
        w.spawn(&sol, 1040, 1000, 0, 0);          // same player, 40px away
        w.orderWaitAttack(amb, false);
        run(w, 3.0f);
        const Unit* u = w.unit(amb);
        check(u && !u->orders.empty(), "an ally standing next to it does not");
        delete &w;
    }

    // A BUILDING COUNTS. The grid holds structures too, and the original scan made no
    // exception for them -- so substituting the grid must not quietly introduce one.
    {
        World& w = *makeWorld();
        const int amb = w.spawn(&sol, 1000, 1000, 0, 0);
        w.spawn(&twr, 1120, 1000, 0, 1);          // enemy STRUCTURE inside sight
        w.orderWaitAttack(amb, false);
        run(w, 3.0f);
        const Unit* u = w.unit(amb);
        check(u && u->orders.empty(), "an enemy BUILDING within sight springs it too",
              u ? std::to_string(u->orders.size()) + " order(s) left" : "gone");
        delete &w;
    }

    // AT THE EDGE: just outside holds, just inside springs. The grid visits whole cells,
    // so a sloppy substitution would spring early on someone a cell away but out of range.
    {
        World& w = *makeWorld();
        const int amb = w.spawn(&sol, 1000, 1000, 0, 0);
        w.spawn(&sol, 1000 + 260, 1000, 0, 1);    // 260px: outside 200, inside the cells swept
        w.orderWaitAttack(amb, false);
        run(w, 3.0f);
        const Unit* u = w.unit(amb);
        check(u && !u->orders.empty(),
              "someone in a swept cell but out of range does not spring it");
        delete &w;
    }

    std::printf(fails ? "ambush_test: %d FAILED\n" : "ambush_test: all passed\n", fails);
    return fails ? 1 : 0;
}
