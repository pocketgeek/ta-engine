// ambush_test -- SetMission "wa": WaitForAttack (icd 0x4017a0). The unit holds
// until the unit it watches (itself, or the placed unit a `wa NAME` names) is HIT
// -- event 0x10, which the hit response 0x409a50 sends to the victim's listeners
// -- or is gone (0x08). Hits count from the moment the order is given.
//
// WHY THIS EXISTS. Ours used to release the hold as soon as any non-allied unit
// came within sight, and checked that with a scan of every unit in range each
// tick. Retail's mission has no sight test at all: an enemy can walk right up to
// an ambusher, and only the first blow springs it. (Its weapons still fire at
// whatever comes into reach while it waits; that is the auto slots, not the
// mission.) The cases below pin both halves.

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
    t.maxVel = tak::sim::Fixed::fromFloat(70.0f / 30.0f); t.turnRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.footX = 2; t.footZ = 2;
    t.sight = 200;
    return t;
}

static UnitType tower() {
    UnitType t{};
    t.name = "twr"; t.id = "twr";
    t.maxVel = tak::sim::Fixed::fromFloat(0.0f / 30.0f);                      // isStructure() == maxVel <= 0
    t.maxHp = 500;
    t.footX = 3; t.footZ = 3;
    return t;
}

static World* makeWorld() {
    World* w = new World();
    w->setTerrain(std::vector<uint8_t>(size_t(128) * 128, 100), 128, 128, /*seaLevel=*/20);
    return w;
}

static void run(World& w, float seconds) {
    for (int i = 0; i < int(seconds * 30); ++i) w.tick(1.0f / 30.0f);
}

// What the hit response stamps on a struck unit (World::retailHitResponse).
static void strike(World& w, int id) { w.unit(id)->attackedTick = w.tickCount() + 1; }

int main() {
    std::printf("ambush_test\n");
    UnitType sol = soldier(), twr = tower();

    // FAR ENEMY: the ambush holds.
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

    // NEAR ENEMY THAT NEVER STRIKES: it still holds (it used to spring on sight).
    {
        World& w = *makeWorld();
        const int amb = w.spawn(&sol, 1000, 1000, 0, 0);
        w.spawn(&sol, 1120, 1000, 0, 1);          // 120px, inside 200 sight
        w.orderWaitAttack(amb, false);
        run(w, 3.0f);
        const Unit* u = w.unit(amb);
        check(u && !u->orders.empty(), "an enemy within sight does not spring it",
              u ? std::to_string(u->orders.size()) + " order(s) left" : "gone");
        delete &w;
    }

    // AN ENEMY BUILDING within sight does not either.
    {
        World& w = *makeWorld();
        const int amb = w.spawn(&sol, 1000, 1000, 0, 0);
        w.spawn(&twr, 1120, 1000, 0, 1);
        w.orderWaitAttack(amb, false);
        run(w, 3.0f);
        const Unit* u = w.unit(amb);
        check(u && !u->orders.empty(), "nor does an enemy building within sight");
        delete &w;
    }

    // THE FIRST BLOW springs it.
    {
        World& w = *makeWorld();
        const int amb = w.spawn(&sol, 1000, 1000, 0, 0);
        w.orderWaitAttack(amb, false);
        run(w, 1.0f);
        strike(w, amb);
        run(w, 0.2f);
        const Unit* u = w.unit(amb);
        check(u && u->orders.empty(), "a hit springs it",
              u ? std::to_string(u->orders.size()) + " order(s) left" : "gone");
        delete &w;
    }

    // A HIT BEFORE ITS TURN counts: the listener is there from the order on.
    {
        World& w = *makeWorld();
        const int amb = w.spawn(&sol, 1000, 1000, 0, 0);
        w.orderWait(amb, 2.0f, false);
        w.orderWaitAttack(amb, true);
        run(w, 0.5f);
        strike(w, amb);
        run(w, 2.0f);
        const Unit* u = w.unit(amb);
        check(u && u->orders.empty(), "a hit during the wait before it springs it when it comes up",
              u ? std::to_string(u->orders.size()) + " order(s) left" : "gone");
        delete &w;
    }

    // WATCHING ANOTHER UNIT: its hit, not ours, springs it; and so does its death.
    {
        World& w = *makeWorld();
        const int amb = w.spawn(&sol, 1000, 1000, 0, 0);
        const int ward = w.spawn(&sol, 1300, 1000, 0, 0);
        w.orderWaitAttack(amb, false, ward);
        strike(w, amb);
        run(w, 1.0f);
        check(!w.unit(amb)->orders.empty(), "watching another unit, its own hit does not count");
        strike(w, ward);
        run(w, 0.2f);
        check(w.unit(amb)->orders.empty(), "the watched unit's hit does");
        w.orderWaitAttack(amb, false, ward);
        run(w, 0.5f);
        w.unit(ward)->hp = tak::sim::Fixed();
        run(w, 0.2f);
        check(w.unit(amb)->orders.empty(), "and so does the watched unit's death");
        delete &w;
    }

    std::printf(fails ? "ambush_test: %d FAILED\n" : "ambush_test: all passed\n", fails);
    return fails ? 1 : 0;
}
