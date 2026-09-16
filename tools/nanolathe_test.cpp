// nanolathe_test -- TA construction: a builder STREAMS a unit into existence,
// several builders add their power together, and metal/energy are drawn
// continuously as the work happens.
//
// Data-independent, so CI runs it.
//
// Kingdoms builders place a building and it appears; TA's build is a rate. The
// port notes listed this as unfinished for a long time, so what is pinned here
// is the behaviour that makes it "done", each of which fails in a different and
// quiet way:
//
//   * Build TIME is buildTime / workerTime, not buildTime. A builder with
//     workerTime 300 against a buildTime of 3000 takes 10s -- reading either
//     field alone gives 3000 or 300.
//   * Build power is ADDITIVE: a second builder on the same site halves the
//     time. If assist merely re-targeted (or the two fought over the site), the
//     second builder would buy nothing.
//   * Cost is drawn CONTINUOUSLY and totals the unit's cost exactly once,
//     however many builders worked it -- paying per builder would let an
//     assisted job cost double.
//   * A starved job HOLDS rather than progressing free or dying.
//   * An abandoned site DECAYS at the rate it was being built and vanishes,
//     leaving no wreck (it was never finished).

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

// A mobile builder. workerTime 300 against the target's buildTime 3000 = 10s.
static UnitType builderType() {
    UnitType t{};
    t.name = "builder"; t.id = "builder";
    t.maxHp = 500;
    t.maxVel = 60; t.turnRate = 6; t.turnInPlaceRate = 6;
    t.footX = 2; t.footZ = 2;
    t.isBuilder = true;
    t.workerTime = 300;
    t.buildDist = 200;    // reach the site from where it is spawned
    return t;
}

// The thing being built: a plain structure, 100 metal / 200 energy.
static UnitType targetType() {
    UnitType t{};
    t.name = "solar"; t.id = "solar";
    t.maxVel = 0; t.maxHp = 500;
    t.footX = 3; t.footZ = 3;
    t.buildTime = 3000;
    t.buildCostMetal = 100;
    t.buildCostEnergy = 200;
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

// Seconds of simulation until the site stops being under construction, or -1.
static float buildSeconds(World& w, int siteId, float limit) {
    for (int i = 0; i < int(limit * 30.0f); ++i) {
        w.tick(1.0f / 30.0f);
        const Unit* s = w.unit(siteId);
        if (!s || !s->alive()) return -1;
        if (!s->underConstruction) return float(i + 1) / 30.0f;
    }
    return -1;
}

int main() {
    std::printf("nanolathe_test\n");
    UnitType builder = builderType(), target = targetType();

    // --- 1. build time is buildTime / workerTime -----------------------------
    {
        World* w = makeWorld();
        int b = w->spawn(&builder, 300, 300, 0, 0);
        w->player(0).metal.cur = 1000;
        w->player(0).energy.cur = 1000;
        int site = w->startBuild(b, &target, 400, 300);
        check(site > 0, "startBuild places a site");
        float secs = buildSeconds(*w, site, 40.0f);
        near(secs, 10.0f, 1.0f, "one builder takes buildTime/workerTime seconds");
        delete w;
    }

    // --- 2. a second builder halves it (additive build power) ----------------
    {
        World* w = makeWorld();
        int b1 = w->spawn(&builder, 300, 300, 0, 0);
        int b2 = w->spawn(&builder, 300, 360, 0, 0);
        w->player(0).metal.cur = 1000;
        w->player(0).energy.cur = 1000;
        int site = w->startBuild(b1, &target, 400, 300);
        w->assist(b2, site);
        float secs = buildSeconds(*w, site, 40.0f);
        near(secs, 5.0f, 1.0f, "a second builder assisting halves the build time");
        delete w;
    }

    // --- 3. the cost is the unit's cost, however many builders worked --------
    // Paying per BUILDER rather than per unit of work is the natural bug here,
    // and it would make the assisted job above cost twice as much.
    {
        World* w = makeWorld();
        int b1 = w->spawn(&builder, 300, 300, 0, 0);
        int b2 = w->spawn(&builder, 300, 360, 0, 0);
        w->player(0).metal.cur = 1000;
        w->player(0).energy.cur = 1000;
        int site = w->startBuild(b1, &target, 400, 300);
        w->assist(b2, site);
        buildSeconds(*w, site, 40.0f);
        near(1000.0f - w->player(0).metal.cur, target.buildCostMetal, 6.0f,
             "two builders still spend one unit's metal, not two");
        near(1000.0f - w->player(0).energy.cur, target.buildCostEnergy, 12.0f,
             "and one unit's energy");
        delete w;
    }

    // --- 4. cost is drawn CONTINUOUSLY, not at the end -----------------------
    {
        World* w = makeWorld();
        int b = w->spawn(&builder, 300, 300, 0, 0);
        w->player(0).metal.cur = 1000;
        w->player(0).energy.cur = 1000;
        int site = w->startBuild(b, &target, 400, 300);
        run(*w, 5.0f);                       // half way through a 10s job
        const Unit* s = w->unit(site);
        check(s && s->underConstruction, "at the half-way point it is still building");
        float spent = 1000.0f - w->player(0).metal.cur;
        check(spent > target.buildCostMetal * 0.25f &&
              spent < target.buildCostMetal * 0.75f,
              "and roughly half the metal has already been spent");
        delete w;
    }

    // --- 5. a starved job HOLDS ----------------------------------------------
    // Not free progress, and not a cancelled site: spendBuild is all-or-nothing,
    // so with nothing in the bank the site simply stays where it is.
    {
        World* w = makeWorld();
        int b = w->spawn(&builder, 300, 300, 0, 0);
        w->player(0).metal.cur = 0;
        w->player(0).energy.cur = 0;
        int site = w->startBuild(b, &target, 400, 300);
        run(*w, 3.0f);                       // let it walk into range and try
        const Unit* s0 = w->unit(site);
        check(s0 && s0->alive() && s0->underConstruction,
              "with an empty treasury the site still exists and is unfinished");
        float hpAfterArrival = s0 ? s0->hp : 0;
        run(*w, 5.0f);
        const Unit* s1 = w->unit(site);
        check(s1 && s1->underConstruction, "and it is still unfinished later");
        near(s1 ? s1->hp : -1, hpAfterArrival, 1.0f, "having made no progress at all");
        delete w;
    }

    // --- 6. an abandoned site decays away, leaving nothing -------------------
    {
        World* w = makeWorld();
        int b = w->spawn(&builder, 300, 300, 0, 0);
        w->player(0).metal.cur = 1000;
        w->player(0).energy.cur = 1000;
        int site = w->startBuild(b, &target, 400, 300);
        run(*w, 4.0f);                       // partly built
        const Unit* mid = w->unit(site);
        check(mid && mid->underConstruction && mid->hp > 0, "the site got part way up");
        w->cancelBuilds(b);
        w->stop(b);                          // nobody is working it now
        run(*w, 30.0f);
        const Unit* gone = w->unit(site);
        check(!gone || !gone->alive(), "an abandoned site decays away to nothing");
        delete w;
    }

    std::printf(g_fail ? "nanolathe_test: %d FAILURE(S)\n" : "nanolathe_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
