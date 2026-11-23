// factory_death_test -- a factory that dies takes its unfinished output with it.
//
// Retail (KINGDOMS.icd, emulated 2026-10-28): a unit's death clears every one of
// its missions (0x512860 -> 0x4d6a50(unit, 1)), and the mission destructor
// 0x4d6da0 calls a started mission's handler with flag 2. BuildingBuild
// (0x401c20, the production mission of a producer without a mover) then deals
// its unfinished output 30000 damage of type 11 from the factory (0x51a140):
// the output dies with no corpse, no death weapon and no kill credit. MobileBuild,
// VTOL_MobileBuild and HelpBuild only stop the build animation on flag 2, so a
// mobile (Zhon) producer's output and a builder's construction site survive
// their maker and decay as an abandoned site.
//
// Ours left a dead factory's output standing to decay at its build rate.
//
// Every scenario uses synthetic types (no game data) on a flat map.

#include "sim/sim.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace tak::sim;

static int g_fail = 0;
static void check(bool cond, const char* what, const std::string& detail = {}) {
    std::printf("  %-66s %s%s%s\n", what, cond ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!cond) ++g_fail;
}

struct Types {
    UnitType factory, sol, conjurer, builder, house, enemy;
    Types() {
        factory.name = factory.id = "factory";
        factory.maxVel = Fixed();                       // a structure
        factory.maxHp = 5000; factory.footX = 6; factory.footZ = 6;
        factory.isBuilder = true; factory.workerTime = 10; factory.storage = 1000000;
        factory.buildMenu = {"sol"};
        factory.buildTime = 60; factory.buildCost = 600;

        sol.name = sol.id = "sol";
        sol.maxVel = Fixed::fromFloat(60.0f / 30.0f); sol.turnRate = 10000; sol.turnInPlaceRate = 10000;
        sol.canMove = true; sol.maxHp = 300; sol.footX = sol.footZ = 2;
        sol.buildTime = 60; sol.buildCost = 600;        // 180 ticks at workertime 10
        sol.healTime = 0;

        conjurer = sol;                                 // a Zhon-style mobile producer
        conjurer.name = conjurer.id = "conjurer";
        conjurer.isBuilder = true; conjurer.workerTime = 10; conjurer.buildDist = 60;
        conjurer.buildMenu = {"sol"};

        builder = conjurer;                             // a mobile builder of structures
        builder.name = builder.id = "builder";
        builder.buildMenu = {"house"};

        house = factory;                                // an ordinary structure
        house.name = house.id = "house";
        house.isBuilder = false; house.buildMenu.clear(); house.footX = house.footZ = 4;
        house.maxHp = 1000;

        enemy = sol;
        enemy.name = enemy.id = "enemy";
    }
};

static std::unique_ptr<World> makeWorld() {
    auto w = std::make_unique<World>();
    w->setTerrain(std::vector<uint8_t>(size_t(128) * 128, 100), 128, 128, 20);
    w->setPathService(true);
    w->player(0).mana = 100000;
    w->player(1).mana = 100000;
    return w;
}

static void tickN(World& w, int n) { for (int i = 0; i < n; ++i) w.tick(1.0f / 30.0f); }

// Kill `id` as a weapon would: no hp left, the last hit from `killer`.
static void kill(World& w, int id, int killer) {
    Unit* u = w.unit(id);
    const Unit* k = w.unit(killer);
    u->hp = Fixed(); u->deathType = 1;
    u->lastHitBy = killer; u->lastHitPlayer = k ? k->player : -1;
}

// Run a producer until its output is `ticks` of work in; returns the output id.
static int produceUntil(World& w, int producer, int ticks) {
    for (int t = 0; t < 600; ++t) {
        w.tick(1.0f / 30.0f);
        if (const Unit* p = w.unit(producer); p && p->productionSiteId) {
            tickN(w, ticks);
            return w.unit(producer)->productionSiteId;
        }
    }
    return 0;
}

static void factoryDies(bool scripted) {
    std::printf("factory %s mid-production:\n", scripted ? "destroyed by a script" : "killed");
    Types ty;
    auto owned = makeWorld();
    World& w = *owned;
    const int factory = w.spawn(&ty.factory, 1000, 1000, 0.0f, 0);
    const int foe = w.spawn(&ty.enemy, 1600, 1600, 0.0f, 1);
    w.train(factory, &ty.sol, 2);
    // The first output finishes and walks away; the second is mid-way.
    int first = produceUntil(w, factory, 0);
    for (int t = 0; t < 400 && w.unit(first)->underConstruction; ++t) w.tick(1.0f / 30.0f);
    const int second = produceUntil(w, factory, 60);
    char d[200];
    std::snprintf(d, sizeof d, "first %d (unfinished %d), second %d (unfinished %d)", first,
                  int(w.unit(first)->underConstruction), second,
                  second ? int(w.unit(second)->underConstruction) : -1);
    check(first && !w.unit(first)->underConstruction && second && second != first &&
          w.unit(second)->underConstruction, "setup: one output finished, the next unfinished", d);
    const int lossesBefore = w.player(0).losses, killsBefore = w.player(1).kills;
    if (scripted) w.scenarioDestroy(factory);
    else { kill(w, factory, foe); w.tick(1.0f / 30.0f); }
    // A body with nothing left to show is compacted out of units() at once.
    const Unit* f = w.unit(factory);
    const Unit* s = w.unit(second);
    std::snprintf(d, sizeof d, "factory %s, output %s deadFor %d deathType %d hp %.1f",
                  !f ? "gone" : f->alive() ? "alive" : "dead", !s ? "gone" : s->alive() ? "alive" : "dead",
                  s ? s->deadFor : 0, s ? int(s->deathType) : 0, s ? s->hp.toFloat() : 0.f);
    check(!f || !f->alive(), "the factory is dead", d);
    check(!s || !s->alive(), "its unfinished output dies on the same tick", d);
    // 512860 then drops the factory as +0xf8: its object retired first. The
    // player (+0xfc) stays; kind 11 credits nobody either way.
    check(!s || (s->deathType == 11 && s->lastHitPlayer == 0),
          "...from the factory, damage type 11 (0x51a140)", d);
    check(!s || !s->deathHasCorpse, "...with no corpse", d);
    std::snprintf(d, sizeof d, "losses %d -> %d, enemy kills %d -> %d", lossesBefore, w.player(0).losses,
                  killsBefore, w.player(1).kills);
    // 512ae0 counts kinds 1-3, 5, 7, 8, 13-15 (check_kill_credit.py losses):
    // the factory killed by a weapon is a loss, a scripted removal (0) and
    // the output's kind 11 are not.
    check(w.player(0).losses == lossesBefore + (scripted ? 0 : 1), "only the factory killed by a weapon is a loss", d);
    check(w.player(1).kills == killsBefore + (scripted ? 0 : 1), "the factory's killer is credited with the factory only", d);
    check(w.unit(first)->alive() && w.unit(first)->hp.toFloat() > 299, "the finished output is untouched", d);
    tickN(w, 60);
    check(!w.unit(second) || !w.unit(second)->alive(), "the output stays gone", d);
    size_t records = 0;
    for (const auto& u : w.units()) if (u.id == second) ++records;
    check(records == 0 && w.unit(first) && w.unit(first)->alive(), "...and its record is retired", d);
}

static void emptyYard() {
    std::printf("factory killed with an empty yard:\n");
    Types ty;
    auto owned = makeWorld();
    World& w = *owned;
    const int factory = w.spawn(&ty.factory, 1000, 1000, 0.0f, 0);
    const int bystander = w.spawn(&ty.sol, 1300, 1000, 0.0f, 0);
    const int foe = w.spawn(&ty.enemy, 1600, 1600, 0.0f, 1);
    tickN(w, 10);
    const size_t count = w.units().size();
    kill(w, factory, foe);
    tickN(w, 2);
    int aliveOwn = 0;
    for (const auto& u : w.units()) if (u.alive() && u.player == 0) ++aliveOwn;
    char d[120];
    std::snprintf(d, sizeof d, "units %zu -> %zu, own alive %d, losses %d", count, w.units().size(), aliveOwn,
                  w.player(0).losses);
    check((!w.unit(factory) || !w.unit(factory)->alive()) && w.unit(bystander)->alive() && aliveOwn == 1 && w.player(0).losses == 1,
          "only the factory dies; nothing else is touched", d);
}

static void mobileProducerDies() {
    std::printf("mobile (Zhon) producer killed mid-production -- retail keeps the output:\n");
    Types ty;
    auto owned = makeWorld();
    World& w = *owned;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);   // storage for the pool
    const int conj = w.spawn(&ty.conjurer, 1000, 1000, 0.0f, 0);
    const int foe = w.spawn(&ty.enemy, 1600, 1600, 0.0f, 1);
    w.train(conj, &ty.sol, 1);
    const int site = produceUntil(w, conj, 60);
    check(site && w.unit(site)->underConstruction, "setup: the output is unfinished");
    if (!site) return;
    const float hp = w.unit(site)->hp.toFloat();
    kill(w, conj, foe);
    tickN(w, 2);
    char d[120];
    std::snprintf(d, sizeof d, "producer alive %d, output alive %d hp %.1f (was %.1f)", int(w.unit(conj) && w.unit(conj)->alive()),
                  int(w.unit(site) && w.unit(site)->alive()), w.unit(site) ? w.unit(site)->hp.toFloat() : 0.f, hp);
    check(!(w.unit(conj) && w.unit(conj)->alive()) && w.unit(site) && w.unit(site)->alive() && w.unit(site)->underConstruction,
          "the output outlives its producer (MobileBuild ignores flag 2)", d);
}

static void builderDies() {
    std::printf("mobile builder killed while constructing -- retail keeps the site:\n");
    Types ty;
    auto owned = makeWorld();
    World& w = *owned;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);   // storage for the pool
    const int b = w.spawn(&ty.builder, 1000, 1000, 0.0f, 0);
    const int foe = w.spawn(&ty.enemy, 1800, 1800, 0.0f, 1);
    const int site = w.startBuild(b, &ty.house, 1000, 1100);
    int t = 0;
    for (; t < 600 && !(site && w.unit(site)->buildBegun && w.unit(site)->hp.toFloat() > 50); ++t)
        w.tick(1.0f / 30.0f);
    check(site && w.unit(site)->underConstruction, "setup: the site is under construction");
    if (!site) return;
    kill(w, b, foe);
    tickN(w, 2);
    char d[120];
    std::snprintf(d, sizeof d, "builder alive %d, site alive %d hp %.1f", int(w.unit(b) && w.unit(b)->alive()),
                  int(w.unit(site) && w.unit(site)->alive()), w.unit(site) ? w.unit(site)->hp.toFloat() : 0.f);
    check(!(w.unit(b) && w.unit(b)->alive()) && w.unit(site) && w.unit(site)->alive() && w.unit(site)->underConstruction,
          "the site outlives its builder", d);
}

// ---------------------------------------------------------------------------
// Cancelling (user rule 2026-10-28; retail subbuild 0x4d7ca0 frees the type's
// BuildingBuild once its whole count goes, and the abort above kills the
// output). Every way ours drops the unit in the yard kills it at once; a
// queued entry that has not started is only removed. No mana comes back.
// ---------------------------------------------------------------------------
enum class Cancel { Unqueue, UnqueueQueued, Stop, RepeatOff, Transfer, ConjurerUnqueue };

static bool gone(const World& w, int id) { const Unit* u = w.unit(id); return !u || !u->alive(); }

static void cancelCase(Cancel how, const char* label) {
    std::printf("%s:\n", label);
    Types ty;
    auto owned = makeWorld();
    World& w = *owned;
    const bool conj = how == Cancel::ConjurerUnqueue;
    if (conj) w.spawn(&ty.factory, 300, 300, 0.0f, 0);   // storage for the pool
    const int producer = conj ? w.spawn(&ty.conjurer, 1000, 1000, 0.0f, 0) : w.spawn(&ty.factory, 1000, 1000, 0.0f, 0);
    w.spawn(&ty.enemy, 1800, 1800, 0.0f, 1);
    int first = 0;
    if (!conj) {   // one finished output first
        w.train(producer, &ty.sol, 1);
        first = produceUntil(w, producer, 0);
        for (int t = 0; t < 400 && w.unit(first)->underConstruction; ++t) w.tick(1.0f / 30.0f);
    }
    if (how == Cancel::RepeatOff) w.setRepeat(producer, &ty.sol);
    else w.train(producer, &ty.sol, how == Cancel::UnqueueQueued ? 2 : 1);
    const int site = produceUntil(w, producer, 60);
    char d[200];
    check(site && site != first && w.unit(site)->underConstruction, "setup: an output is unfinished in the yard");
    if (!site) return;
    const int losses = w.player(0).losses, kills = w.player(1).kills;
    const double mana = w.player(0).mana;
    const size_t queued = w.unit(producer)->buildQueue.size();
    switch (how) {
        case Cancel::Unqueue: case Cancel::UnqueueQueued: case Cancel::ConjurerUnqueue:
            w.dequeue(producer, &ty.sol, 1); break;
        case Cancel::Stop: w.stop(producer); break;
        case Cancel::RepeatOff: w.setRepeat(producer, &ty.sol); break;
        case Cancel::Transfer: w.scenarioTransfer(producer, 1); break;
    }
    const bool dies = how != Cancel::UnqueueQueued && how != Cancel::ConjurerUnqueue;
    std::snprintf(d, sizeof d, "output %s, losses %d -> %d, enemy kills %d -> %d, mana %.2f -> %.2f, queue %zu -> %zu",
                  gone(w, site) ? "gone" : "alive", losses, w.player(0).losses, kills, w.player(1).kills, mana,
                  w.player(0).mana, queued, w.unit(producer)->buildQueue.size());
    if (dies) {
        check(gone(w, site), "the unfinished output dies at once (no tick)", d);
        check(w.player(0).losses == losses && w.player(1).kills == kills, "...as nobody's kill and no loss (kind 11)", d);
        check(w.player(0).mana == mana, "...and no mana comes back", d);
        tickN(w, 2);
        check(gone(w, site) && !w.unit(producer)->productionSiteId, "...and stays gone; the producer holds no output", d);
    } else {
        check(!gone(w, site) && w.unit(site)->underConstruction, how == Cancel::UnqueueQueued
              ? "cancelling a queued, unstarted entry leaves the output alone"
              : "a conjurer's cancelled output stays (MobileBuild)", d);
        if (how == Cancel::UnqueueQueued)
            check(queued == 2 && w.unit(producer)->buildQueue.size() == 1 && w.unit(producer)->productionSiteId == site,
                  "...the entry leaves the queue; production goes on", d);
    }
    if (first) check(!gone(w, first) && w.unit(first)->player == 0, "the finished output is untouched", d);
    if (how == Cancel::Transfer) check(w.unit(producer)->player == 1, "the factory changed hands", d);
}

int main() {
    factoryDies(false);
    factoryDies(true);
    emptyYard();
    mobileProducerDies();
    builderDies();
    cancelCase(Cancel::Unqueue, "cancel the unit in production");
    cancelCase(Cancel::UnqueueQueued, "cancel a queued entry that has not started");
    cancelCase(Cancel::Stop, "Stop a factory mid-production");
    cancelCase(Cancel::RepeatOff, "toggle repeat production off mid-production");
    cancelCase(Cancel::Transfer, "factory changes owner mid-production (retail 514da0)");
    cancelCase(Cancel::ConjurerUnqueue, "cancel a conjurer's unit in production -- kept");
    std::printf(g_fail ? "factory_death_test: %d FAILED\n" : "factory_death_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
