// upkeep_test -- standing MetalUse/EnergyUse, and what happens when a base can't
// pay it.
//
// Data-independent, so CI runs it.
//
// TA's metal maker converts energy into metal, and the point of an energy stall
// is that it stops. The engine used to settle all standing upkeep IN BULK: pay
// what the balance covers, zero it, and then credit EVERY unit's output in full.
// So a player whose energy was pinned at zero kept receiving the metal their
// makers "produced" -- free metal, for exactly as long as the stall lasted,
// which is the situation a stall exists to punish.
//
// Billing is per unit and all-or-nothing, in unit (creation) order so which units
// go dark is deterministic and identical on every peer. What this pins:
//
//   * A unit that can pay produces.
//   * A unit that cannot pay produces NOTHING and is charged NOTHING -- not a
//     partial payment, and not free output.
//   * It is not switched off by browning out: it resumes by itself.
//   * The displayed drain still reports what the base ASKED for, so the HUD shows
//     a deficit rather than silently reporting a balanced economy.

#include "sim/sim.h"

#include <cstdio>
#include <string>

using namespace ta::sim;

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("  %-70s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}
static void near(float got, float want, float tol, const std::string& what) {
    bool ok = got >= want - tol && got <= want + tol;
    std::printf("  %-70s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) { std::printf("      got %.3f, want %.3f (+-%.3f)\n", got, want, tol); ++g_fail; }
}

// A metal maker: burns energy, makes metal. ARMMAKR's shape.
static UnitType makerType() {
    UnitType t{};
    t.name = "maker"; t.id = "maker";
    t.maxVel = 0; t.maxHp = 500;
    t.footX = 3; t.footZ = 3;
    t.energyUse = 20;
    t.makesMetal = 1;
    return t;
}

// A plain generator: no upkeep, so it goes through the unmetered pass.
static UnitType solarType() {
    UnitType t{};
    t.name = "solar"; t.id = "solar";
    t.maxVel = 0; t.maxHp = 500;
    t.footX = 3; t.footZ = 3;
    t.energyMake = 20;
    return t;
}

static World* makeWorld() {
    World* w = new World();
    w->setVisPlayer(-1);
    w->setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, /*seaLevel=*/20);
    w->setPlayerCount(2);
    return w;
}

static void run(World& w, float seconds) {
    for (int i = 0; i < int(seconds * 30.0f); ++i) w.tick(1.0f / 30.0f);
}

int main() {
    std::printf("upkeep_test\n");
    UnitType maker = makerType(), solar = solarType();

    // --- 1. paid upkeep produces --------------------------------------------
    {
        World* w = makeWorld();
        w->spawn(&solar, 200, 200, 0, 0);   // 20 e/s in
        w->spawn(&maker, 300, 200, 0, 0);   // 20 e/s out, 1 m/s in
        w->player(0).energy.cur = 1000;
        w->player(0).metal.cur = 0;
        run(*w, 10.0f);
        near(w->player(0).metal.cur, 10.0f, 1.0f,
             "a maker that can pay its energy produces metal");
        near(w->player(0).energy.cur, 1000.0f, 5.0f,
             "and the solar exactly covers it, so energy holds steady");
        delete w;
    }

    // --- 2. unpaid upkeep produces NOTHING -----------------------------------
    // This is the case the bulk settle got wrong: no energy at all, yet metal
    // still accrued.
    {
        World* w = makeWorld();
        w->spawn(&maker, 300, 200, 0, 0);
        w->player(0).energy.cur = 0;        // no generation anywhere
        w->player(0).metal.cur = 0;
        run(*w, 10.0f);
        near(w->player(0).metal.cur, 0.0f, 0.001f,
             "a maker with no energy produces NO metal");
        near(w->player(0).energy.cur, 0.0f, 0.001f,
             "and is charged nothing, because it produced nothing");
        // The HUD must still show the deficit rather than a balanced economy.
        near(w->player(0).energy.drain, 20.0f, 0.001f,
             "the displayed drain still reports what the base asked for");
        delete w;
    }

    // --- 3. it is not switched off: it recovers by itself ---------------------
    {
        World* w = makeWorld();
        w->spawn(&maker, 300, 200, 0, 0);
        w->player(0).energy.cur = 0;
        w->player(0).metal.cur = 0;
        run(*w, 5.0f);
        near(w->player(0).metal.cur, 0.0f, 0.001f, "stalled: no metal");
        // Now give it a generator and let it run again.
        w->spawn(&solar, 200, 200, 0, 0);
        run(*w, 10.0f);
        check(w->player(0).metal.cur > 8.0f,
              "once energy returns the maker resumes on its own (not latched off)");
        delete w;
    }

    // --- 4. partial supply feeds SOME makers, all-or-nothing each ------------
    // One solar (20 e/s) against three makers (20 e/s each). Exactly one maker
    // can be paid per tick, so the metal rate is one maker's worth -- not three
    // makers each running at a third, which is what a proportional split would
    // give and what bulk settling effectively produced.
    {
        World* w = makeWorld();
        w->spawn(&solar, 200, 200, 0, 0);
        w->spawn(&maker, 300, 200, 0, 0);
        w->spawn(&maker, 400, 200, 0, 0);
        w->spawn(&maker, 500, 200, 0, 0);
        w->player(0).energy.cur = 0;
        w->player(0).metal.cur = 0;
        run(*w, 10.0f);
        near(w->player(0).metal.cur, 10.0f, 1.5f,
             "3 makers on 1 solar earn ONE maker's output, not three thirds");
        delete w;
    }

    // --- 5. an owner's upkeep never touches another player's balance ---------
    {
        World* w = makeWorld();
        w->spawn(&maker, 300, 200, 0, 0);   // player 0
        w->player(0).energy.cur = 0;
        w->player(1).energy.cur = 1000;
        w->player(1).metal.cur = 0;
        run(*w, 10.0f);
        near(w->player(1).energy.cur, 1000.0f, 0.001f,
             "player 1's energy is untouched by player 0's stalled maker");
        near(w->player(1).metal.cur, 0.0f, 0.001f, "and earns them nothing");
        delete w;
    }

    // --- 6. a switched-off unit neither pays nor produces ---------------------
    {
        UnitType onoff = makerType();
        onoff.id = "onoffmaker"; onoff.onOffable = true;
        World* w = makeWorld();
        w->spawn(&solar, 200, 200, 0, 0);
        const int id = w->spawn(&onoff, 300, 200, 0, 0);
        w->player(0).energy.cur = 0;
        w->player(0).metal.cur = 0;
        w->setActive(id, false);
        run(*w, 10.0f);
        near(w->player(0).metal.cur, 0.0f, 0.001f, "a unit switched off produces nothing");
        check(w->player(0).energy.cur > 190.0f,
              "and its energy goes unspent, which is what the toggle is for");
        delete w;
    }

    std::printf(g_fail ? "upkeep_test: %d FAILURE(S)\n" : "upkeep_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
