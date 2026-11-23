// duration_test -- how LONG things take, and what they COST.
//
// Every test in this tree asserts on counts, arrivals and positions. That axis is
// well covered and it is not where the bugs have been. Converting the sim's timers
// from float seconds to integer ticks produced eight defects in one run, every one
// of them a duration or a price, and the suite stayed green through all of them:
//
//   * a dropped bomb expired on the tick it was released (0.8s -> 0 ticks)
//   * a resurrect that should take 170s took 5.7 (seconds assigned into ticks)
//   * emote timers that never ran down (max(0.0f, x - dt) truncating to the same int)
//   * VERMAGE, the most expensive unit in the game, built INSTANTLY AND FREE
//     (its 300,000-tick duration overflowed 16.16 and went negative, so the
//     comparison that gates the mana deduction was false on the first tick)
//   * and the same free-production hole at the other end, where a fast factory
//     rounded its duration down to zero ticks
//
// Only one of those was caught by a test, and only because it happened to move a
// bomb somewhere visible. So this file asserts the boring things directly: a build
// takes about as long as its data says, a long build does not finish early, a short
// one still costs money, and a countdown lasts its stated seconds.
//
// The tolerances are loose on purpose -- a tick either way is fine. What is being
// caught is a factor of 30, a sign flip, or a zero.

#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>

using namespace tak::sim;

static int g_fail = 0;
static void check(bool cond, const std::string& what, const std::string& detail = {}) {
    std::printf("  %-54s %s%s%s\n", what.c_str(), cond ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!cond) ++g_fail;
}

static UnitType factoryType(float workerTime) {
    UnitType t{};
    t.name = "factory"; t.id = "factory";
    t.maxVel = Fixed();            // maxVel <= 0 => isStructure()
    t.maxHp = 5000;
    t.footX = 7; t.footZ = 12;
    t.isBuilder = true;
    t.workerTime = workerTime;
    t.storage = 1000000;           // never let the cap be what stops us
    return t;
}

static UnitType soldierType(float buildTime, float buildCost) {
    UnitType t{};
    t.name = "sol"; t.id = "sol";
    t.maxVel = Fixed::fromFloat(60.0f / 30.0f);
    t.turnRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.footX = 2; t.footZ = 2;
    t.buildTime = buildTime;
    t.buildCost = buildCost;
    t.selfDestructCountdown = 2;
    return t;
}

static std::unique_ptr<World> makeWorld() {
    auto w = std::make_unique<World>();
    w->setTerrain(std::vector<uint8_t>(128 * 128, 100), 128, 128, /*seaLevel=*/20);
    return w;
}

// Ticks until a mobile unit is finished, rather than its construction site appearing.
static int ticksToBuildOne(float workerTime, float buildTime, float buildCost,
                           double mana, int maxTicks) {
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    UnitType fac = factoryType(workerTime);
    UnitType sol = soldierType(buildTime, buildCost);
    const int fid = w.spawn(&fac, 1000, 1000, 0, 0);
    w.player(0).mana = mana;
    w.train(fid, &sol, 1);
    for (int i = 0; i < maxTicks; ++i) {
        w.tick(1.0f / 30.0f);
        for (const auto& u : w.units())
            if (u.alive() && u.type && !u.type->isStructure() && !u.underConstruction) return i + 1;
    }
    return -1;
}

// A finished unit at half health with the given FBI numbers: the hit points it regains
// per second over `seconds` of simulation.
static double selfRepairPerSecond(bool structure, int maxHp, float buildTime, float healTime,
                                  int seconds) {
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    UnitType t = structure ? factoryType(0) : soldierType(buildTime, 0);
    t.isBuilder = false;
    t.maxHp = maxHp;
    t.buildTime = buildTime;
    t.healTime = healTime;
    const int id = w.spawn(&t, 1000, 1000, 0, 0);
    w.unit(id)->hp = Fixed::fromInt(maxHp / 2);
    const Fixed start = w.unit(id)->hp;
    for (int i = 0; i < 30 * seconds; ++i) w.tick(1.0f / 30.0f);
    return double((w.unit(id)->hp - start).toFloat()) / seconds;
}

// NOT COVERED HERE, and worth saying so rather than leaving a gap unmarked: the
// projectile-lifetime fix (rounding flight time UP so the final segment is still
// collision-tested) has no test. An attempt at one is in the history of this file
// and was removed because the synthetic shooter never acquired a target -- auto
// acquisition has gates (fire state, targeting, line of sight) that a hand-built
// UnitType does not satisfy, and reverse-engineering them here was costing more
// than the test was worth. The right home is retailgap_test, which already fires
// weapons successfully using shipped data.

int main() {
    std::printf("duration_test -- how long things take, and what they cost\n");

    // A BUILD TAKES buildTime / workerTime SECONDS. 60/10 = 6s = 180 ticks.
    {
        const int got = ticksToBuildOne(/*workerTime=*/10, /*buildTime=*/60,
                                        /*cost=*/10, /*mana=*/1e6, /*maxTicks=*/30 * 60);
        const int want = 180;
        check(got > 0 && std::abs(got - want) <= 3,
              "a build takes buildTime / workerTime seconds",
              std::to_string(got) + " ticks, expected ~" + std::to_string(want));
    }

    // TWICE THE WORK TAKES TWICE AS LONG. Catches a duration that is constant,
    // clamped, or saturated rather than actually derived from the data.
    {
        const int a = ticksToBuildOne(10, 60, 10, 1e6, 30 * 60);
        const int b = ticksToBuildOne(10, 120, 10, 1e6, 30 * 120);
        const double ratio = (a > 0 && b > 0) ? double(b) / double(a) : 0.0;
        char d[96];
        std::snprintf(d, sizeof d, "%d then %d ticks = x%.2f", a, b, ratio);
        check(ratio > 1.8 && ratio < 2.2, "...and doubling the work doubles the time", d);
    }

    // A VERY LONG BUILD DOES NOT FINISH EARLY. VERMAGE's shipped numbers:
    // buildtime 100000 against workertime 10 is 10,000 seconds. Held as 16.16 ticks
    // that overflowed NEGATIVE and the unit appeared on tick one, free.
    {
        const int got = ticksToBuildOne(/*workerTime=*/10, /*buildTime=*/100000,
                                        /*cost=*/10, /*mana=*/1e9, /*maxTicks=*/30 * 30);
        check(got < 0, "a 10,000-second build has not finished 30 seconds in",
              got < 0 ? "still building" : ("appeared after " + std::to_string(got) + " ticks"));
    }

    // A VERY SHORT BUILD IS STILL PAID FOR. Rounding the duration to zero ticks
    // skipped the branch that checks and deducts mana.
    {
        const int got = ticksToBuildOne(/*workerTime=*/1000, /*buildTime=*/1,
                                        /*cost=*/500, /*mana=*/0.0, /*maxTicks=*/30 * 10);
        check(got < 0, "a sub-tick build still cannot be had for free",
              got < 0 ? "nothing built with no mana"
                      : ("appeared after " + std::to_string(got) + " ticks"));
    }

    // A COUNTDOWN LASTS ITS STATED SECONDS. selfDestructCountdown is 2, so the unit
    // should still be alive at 1s and gone by 3s.
    {
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        UnitType sol = soldierType(60, 10);
        const int id = w.spawn(&sol, 500, 500, 0, 0);
        w.destroy(id);                       // arms the countdown
        for (int i = 0; i < 30; ++i) w.tick(1.0f / 30.0f);
        const bool aliveAt1s = w.unit(id) && w.unit(id)->alive();
        for (int i = 0; i < 30 * 2; ++i) w.tick(1.0f / 30.0f);
        const bool goneAt3s = !w.unit(id) || !w.unit(id)->alive();
        check(aliveAt1s, "a 2-second self-destruct has not fired after 1s");
        check(goneAt3s, "...and has fired by 3s");
    }

    // SELF-REPAIR REGAINS maxdamage * healtime / buildtime HIT POINTS A SECOND.
    // healtime is the unit's own worker time: every eighth tick retail puts
    // healtime * 8/30 build-seconds of work into a finished unit below full health
    // (KINGDOMS.icd 0x51e1a8 -> 0x429d90). The data is authored that way -- most troops
    // at exactly healtime = buildtime/240 (full health in four minutes), many buildings
    // at buildtime/480 (eight). We read it as seconds per hit point: the Crusades Aramon
    // Keep (25116 HP, buildtime 1622, healtime 3.17083) regained 0.32 HP/s instead of
    // 49.1. The rates below are retail's, emulated: tools/re/check_self_repair.py.
    {
        struct Case { const char* what; bool structure; int maxHp; float buildTime, healTime; double want; };
        const Case cases[] = {
            {"Crusades Aramon Keep", true, 25116, 1622, 3.17083f, 49.083},
            {"Crusades Aramon Barracks", true, 17162, 600, 1.25f, 35.733},
            {"Aramon Lodestone", true, 5680, 92, 0.283333333f, 17.483},
            {"Aramon archer", false, 1100, 125, 0.520833333f, 4.567},
        };
        for (const Case& c : cases) {
            const double got = selfRepairPerSecond(c.structure, c.maxHp, c.buildTime, c.healTime, 60);
            char d[96];
            std::snprintf(d, sizeof d, "%.3f HP/s, retail %.3f", got, c.want);
            check(std::abs(got - c.want) <= 0.02, std::string(c.what) + " self-repairs at retail's rate", d);
        }
        // healtime 0 is no repair at all (the Crusades wall), and repair stops at full.
        check(selfRepairPerSecond(true, 3000, 40, 0, 10) == 0.0, "healtime 0 does not self-repair");
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        UnitType keep = factoryType(0);
        keep.isBuilder = false; keep.maxHp = 25116; keep.buildTime = 1622; keep.healTime = 3.17083f;
        const int id = w.spawn(&keep, 1000, 1000, 0, 0);
        w.unit(id)->hp = Fixed::fromInt(25116 - 30);
        for (int i = 0; i < 30 * 5; ++i) w.tick(1.0f / 30.0f);
        check(w.unit(id)->hp == Fixed::fromInt(25116), "self-repair stops exactly at full health",
              std::to_string(w.unit(id)->hp.toFloat()));
    }

    std::printf(g_fail ? "duration_test: %d FAILURE(S)\n" : "duration_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
