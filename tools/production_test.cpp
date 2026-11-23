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
#include "sim/legion.h"
#include "client/renderframe.h"
#include "hpi/hpi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <tuple>

using namespace tak::sim;

static PathfindingMode g_pathfinding = PathfindingMode::Retail;
static void setTestPathfinding(World& world) {
    if(g_pathfinding!=PathfindingMode::Retail)world.setPathfindingMode(g_pathfinding);
}
static int g_fail = 0;
static void check(bool cond, const char* what, const std::string& detail = {}) {
    std::printf("  %-62s %s%s%s\n", what, cond ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!cond) ++g_fail;
}

static UnitType factoryType() {
    UnitType t{};
    t.name = "factory"; t.id = "factory";
    t.maxVel = tak::sim::Fixed::fromFloat(0.0f / 30.0f);                 // maxVel <= 0 => isStructure()
    t.maxHp = 5000;
    t.footX = 7; t.footZ = 12;
    t.isBuilder = true;
    t.workerTime = 1000;          // builds fast, so the test is short
    return t;
}

static UnitType soldierType() {
    UnitType t{};
    t.name = "sol"; t.id = "sol";
    t.maxVel = tak::sim::Fixed::fromFloat(60.0f / 30.0f); t.turnRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.buildTime = 1;
    t.footX = 2; t.footZ = 2;
    return t;
}

static std::unique_ptr<World> makeWorld(int W, int H) {
    auto w = std::make_unique<World>();setTestPathfinding(*w);
    w->setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, /*seaLevel=*/20);
    w->setPathService(true);                    // the real router, as in a game
    return w;
}

// Legion mission kinds that served some unit during a run() (--legion proves
// Legion, not the native mover, routed the production exits and rallies).
static bool g_legionSaw[16] = {};
static void run(World& w, float seconds) {
    for (int i = 0; i < int(seconds * 30.0f); ++i) {
        w.tick(1.0f / 30.0f);
        if (auto* legion = w.legionNavigator())
            for (const auto& u : w.units()) g_legionSaw[size_t(legion->mission(u))] = true;
    }
}

// ---------------------------------------------------------------------------
// 1. A factory running flat out must not pile its output up on one spot.
// ---------------------------------------------------------------------------
// PRODUCTION MUST BE PAID FOR. Trivial, and it is here because it was NOT true
// twice: both times the build duration rounded to ZERO ticks, so
// `buildProgress < total` was 0 < 0 and the accumulate branch -- the only place
// mana is checked and deducted -- never ran, and positive-cost units spawned with
// an empty treasury.
//
// The assertion is "too poor to build produces nothing", not "mana went down".
// Mana going down proves little here: a player's pool is clamped to its storage
// cap every tick (max(storage, 100)), so it falls on its own with no production at
// all -- which is exactly what a first version of this test measured, happily
// passing against the bug it was written to catch.
static void productionNeedsMana() {
    std::printf("production is paid for:\n");
    auto ownedWorld = makeWorld(128, 128);
    World& w = *ownedWorld;
    UnitType fac = factoryType(), sol = soldierType();
    sol.buildCost = 500;                 // the default is 0, i.e. free
    fac.storage = 10000;                 // headroom, so the cap is not what limits us
    const int fid = w.spawn(&fac, 1000, 1000, 0, 0);

    w.player(0).mana = 0;                // cannot afford a single one
    w.train(fid, &sol, 4);
    run(w, 60.0f);
    int made = 0;
    for (const auto& u : w.units())
        if (u.alive() && u.type && !u.type->isStructure() && !u.underConstruction) ++made;
    check(made == 0, "a player with no mana produces nothing",
          std::to_string(made) + " unit(s) appeared");

    // EXACTLY one unit's price, and exactly one unit expected. Funding four and
    // asserting "more than none" hid an overcharge: the per-tick cost divided by the
    // UNCLAMPED duration, so a one-tick build billed a 500-cost unit about 1667 and a
    // player holding its stated price could not afford it at all.
    w.player(0).mana = 500;
    run(w, 60.0f);
    int made2 = 0;
    for (const auto& u : w.units())
        if (u.alive() && u.type && !u.type->isStructure() && !u.underConstruction) ++made2;
    check(made2 == 1, "...and a unit costs exactly its buildCost",
          std::to_string(made2) + " unit(s) for exactly 500 mana");
}

// SHORT INCOME IS SHARED. Reported from a Zhon game: a Beast Lord conjured a
// drake for three and a half minutes with zero progress and no conjure effects,
// because four lower-id producers whose per-tick costs together exceeded the
// income each paid in full first, in unit order, and the drake (the costliest
// per tick) never found its whole tick's cost in the pool. Retail divides the
// pool across all construction (51d837 allocation = stored / demand, 429af0
// scales every worker by it), so every concurrent build slows down together.
static void shortIncomeIsShared() {
    std::printf("short income is shared across concurrent production:\n");
    auto ownedWorld = makeWorld(128, 128);
    World& w = *ownedWorld;
    UnitType fac = factoryType(), cheap = soldierType(), costly = soldierType();
    fac.workerTime = 1; fac.storage = 10000;
    cheap.id = "cheap"; cheap.buildTime = 10; cheap.buildCost = 900;     // 300 ticks, 3/tick
    costly.id = "costly"; costly.buildTime = 10; costly.buildCost = 1200; // 300 ticks, 4/tick
    UnitType well{}; well.name = well.id = "well"; well.maxHp = 100;
    well.footX = well.footZ = 2; well.income = 60;                       // 2/tick
    const int first = w.spawn(&fac, 600, 600, 0, 0);    // lower id: runs first
    const int second = w.spawn(&fac, 1400, 600, 0, 0);
    w.spawn(&well, 1000, 1400, 0, 0);
    w.player(0).mana = 0;
    w.train(first, &cheap, 1);
    w.train(second, &costly, 1);
    run(w, 10.0f);
    const auto progress = [&](int id) {
        return double(w.unit(id)->buildProgress) + w.unit(id)->buildFraction / 65536.0;
    };
    const double a = progress(first), b = progress(second);
    char detail[128];
    std::snprintf(detail, sizeof detail, "cheap %.2f ticks, costly %.2f ticks, mana %.3f",
                  a, b, w.player(0).mana);
    check(b > 0, "a costlier later producer is not starved by an earlier one", detail);
    // Both run at income / demand = 2/7 of full speed: 300 ticks of 2/7, give or
    // take the first tick, which has no demand to divide by yet.
    check(std::fabs(a - b) < 1.0 && std::fabs(b - 300.0 * 2.0 / 7.0) < 1.5,
          "...both advance at the same fraction of full speed", detail);
    check(w.player(0).mana >= 0 && w.player(0).mana < 0.01,
          "...and the pool is spent, never overdrawn", detail);
}

// THE MANA BULB'S -N IS MANA REQUESTED. Retail's NegativeM shows 401350, the
// average of the resource object's requested total, which construction work
// (429af0) adds to at full rate before the allocation scales it. Starved
// production therefore reads -210 against +60, not the -60 actually paid.
static void hudShowsRequestedMana() {
    std::printf("the HUD's -N is the mana requested, not what a short pool paid:\n");
    auto ownedWorld = makeWorld(128, 128);
    World& w = *ownedWorld;
    UnitType fac = factoryType(), cheap = soldierType(), costly = soldierType();
    fac.workerTime = 1; fac.storage = 10000;
    cheap.id = "cheap"; cheap.buildTime = 10; cheap.buildCost = 900;     // 90/s at full rate
    costly.id = "costly"; costly.buildTime = 10; costly.buildCost = 1200; // 120/s
    UnitType well{}; well.name = well.id = "well"; well.maxHp = 100;
    well.footX = well.footZ = 2; well.income = 60;
    const int first = w.spawn(&fac, 600, 600, 0, 0);
    const int second = w.spawn(&fac, 1400, 600, 0, 0);
    w.spawn(&well, 1000, 1400, 0, 0);
    w.player(0).mana = 0;
    w.train(first, &cheap, 1);
    w.train(second, &costly, 1);
    const auto hash = [&] { return w.stateHash(); };
    run(w, 3.0f);
    PlayerR shown; shown.captureEconomy(w.player(0));
    char detail[128];
    std::snprintf(detail, sizeof detail, "+%.3f -%.3f, mana %.3f",
                  shown.income, shown.expenditure, w.player(0).mana);
    check(std::fabs(shown.income - 60.0f) < 0.01f, "+N is the 60/s income", detail);
    check(std::fabs(shown.expenditure - 210.0f) < 0.05f,
          "-N is the 210/s both producers request, above +N", detail);
    const auto before = hash();
    for (auto& sample : w.player(0).displayResources.samples) sample[1] = 1e6f;
    check(hash() == before, "...and the request history is not lockstep state");
}

// THE AI'S SHORTFALL TEST READS MANA REQUESTED. Retail's 410810 subtracts 401350
// (the averaged request total, +0x10) from 401330 (income), not what was paid.
// A player earning 5 a tick whose builders ask for 10 but are paid 2 is short,
// with a full store: paid usage alone said it was not.
static void aiShortfallReadsRequests() {
    std::printf("the AI's shortfall test reads mana requested, not paid:\n");
    Player p;
    p.mana = 500;
    for (int t = 0; t < 30; ++t) {
        p.creditMana(5); p.requestMana(10); p.payMana(2);
        p.displayResources.advance();
    }
    check(p.displayResources.shortfall(float(p.mana)), "requests above income are a shortfall, whatever was paid");
    Player calm;
    calm.mana = 500;
    for (int t = 0; t < 30; ++t) { calm.creditMana(5); calm.debitMana(4); calm.displayResources.advance(); }
    check(!calm.displayResources.shortfall(float(calm.mana)), "...and requests below income are not");
}

// CLOAKING SPENDS THE UNIT'S OWN MANA. Retail's cloak step (icd 0x401160)
// takes cloakcost/30 a tick from the unit's pool at +0xd8 after its regeneration,
// never from the player's store; a pool that cannot pay decloaks the unit and
// blocks recloaking for 90 ticks. A spy with 10 mana regenerating 3/s and paying
// 15/s spends its pool in under a second, then sits visible for three.
static void cloakSpendsUnitMana() {
    std::printf("cloaking spends the unit's own mana, never the player's:\n");
    auto ownedWorld = makeWorld(64, 64);
    World& w = *ownedWorld;
    UnitType spyType = soldierType();
    spyType.id = "spy"; spyType.canCloak = true; spyType.cloakCost = 15; spyType.cloakCostMove = 21;
    spyType.maxMana = 10; spyType.manaRegen = 3;
    const int spy = w.spawn(&spyType, 300, 300, 0, 0);
    w.unit(spy)->mana = 10;   // a new unit's pool starts empty (511990)
    w.player(0).mana = 50;
    w.setCloak(spy, true);
    w.tick(1.0f / 30);
    char detail[128];
    std::snprintf(detail, sizeof detail, "unit %.4f, player %.4f", w.unit(spy)->mana, w.player(0).mana);
    check(w.unit(spy)->cloaked && std::fabs(w.unit(spy)->mana - (10.0 - 0.5)) < 1e-4 &&
          w.player(0).mana == 50.0, "a cloaked tick takes 15/30 from the unit, nothing from the store", detail);
    check(w.player(0).displayResources.samples.back()[1] == 0, "the mana bulb's -N never shows cloaking");
    int cloakedTicks = 1, blockedFrom = 0;
    for (int t = 2; t <= 200 && !blockedFrom; ++t) {
        w.tick(1.0f / 30);
        if (w.unit(spy)->cloaked) ++cloakedTicks; else blockedFrom = t;
    }
    // Full pools do not regenerate, so the first tick leaves 9.5; tick k (k >= 2)
    // then has 9.6 - 0.4*(k-2) before paying 0.5: tick 24 has 0.8, tick 25 has 0.4.
    std::snprintf(detail, sizeof detail, "cloaked %d ticks, decloaked at tick %d", cloakedTicks, blockedFrom);
    check(blockedFrom == 25, "the unit decloaks the tick its own pool cannot pay", detail);
    int visible = 1;   // the tick it decloaked
    for (w.tick(1.0f / 30); !w.unit(spy)->cloaked && visible < 200; w.tick(1.0f / 30)) ++visible;
    std::snprintf(detail, sizeof detail, "visible %d ticks", visible);
    check(visible == 90, "...and stays visible 90 ticks before it may recloak", detail);
    check(w.player(0).mana == 50.0, "the player's store paid none of it");
    UnitType noPool = spyType; noPool.id = "nopool"; noPool.maxMana = 0; noPool.manaRegen = 0;
    const int dry = w.spawn(&noPool, 600, 300, 0, 0);
    w.setCloak(dry, true);
    w.tick(1.0f / 30);
    check(!w.unit(dry)->cloaked, "a unit without a mana pool cannot pay to cloak");
}

// REVIVING COSTS NOTHING; THE ECONOMY ONLY PACES IT. Retail's Resurrect task
// (icd 0x4068b0) never pays or requests mana. It sets its work to 65536 x
// buildtime / (workertime x 0.03333333), x0.3 for a resurrect but not for an
// animate, takes allocation x 65536 a tick, and at the end a computer player
// whose allocation is under 0.7 abandons it. A buildtime-100 corpse under a
// workertime-10 priest is 90 ticks at full allocation and 180 at half; ours
// took 300 and debited the corpse's buildcost across them. Counted from the
// tick the channel starts, whose wake takes the first allocation (stage 3
// returns 1 and stage 4 runs in the same wake).
struct ReviveRun { int ticks = -1; double mana = 0; float request = 0; int hp = 0; bool raised = false; };
// shortFrom: the allocation holds at 1 until the channel has run that many
// ticks, then drops to `allocation` (-1: short from the start).
static ReviveRun reviveRun(bool animate, double allocation, bool computer, int shortFrom = -1) {
    auto ownedWorld = makeWorld(64, 64);
    World& w = *ownedWorld;
    FeatType body; body.name = "victim_dead"; body.resurrectable = true; body.animatable = true;
    body.decomposeTicks = 100000;
    w.setFeatureTypes({body});
    UnitType victim = soldierType(); victim.id = "victim"; victim.buildTime = 100; victim.buildCost = 400;
    UnitType ghoul = soldierType(); ghoul.id = "ghoul"; ghoul.buildTime = 30; ghoul.buildCost = 400;
    UnitType priest = soldierType(); priest.id = "priest"; priest.workerTime = 10;
    if (animate) { priest.canAnimate = true; priest.animateType = &ghoul; }
    else priest.canResurrect = true;
    w.mapCorpse(&victim, 0);
    w.player(0).automaticGates = computer;
    const int caster = w.spawn(&priest, 300, 300, 0, 0);
    const int dead = w.spawn(&victim, 330, 300, 0, animate ? 1 : 0);
    w.unit(dead)->hp = {}; w.unit(dead)->deathType = 9;
    w.resurrect(caster, dead, false);   // a revive is only ever an order
    const UnitType* out = animate ? &ghoul : &victim;
    auto raised = [&]() -> const Unit* {
        for (const auto& u : w.units()) if (u.alive() && u.type == out && u.player == 0) return &u;
        return nullptr;
    };
    auto channelling = [&] {
        for (const auto& u : w.units()) if (u.type == &priest && u.reviveTarget) return true;
        return false;
    };
    ReviveRun r;
    w.player(0).mana = 80;
    int started = -1;
    for (int t = 0; t < 600; ++t) {
        // A demand the store cannot meet sets the next tick's allocation
        // (stored / demand); nothing spends the store.
        const bool isShort = shortFrom < 0 || (started >= 0 && t - started >= shortFrom);
        w.player(0).constructionDemand = allocation < 1 && isShort ? w.player(0).mana / allocation : 0;
        w.tick(1.0f / 30);
        r.request = std::max(r.request, w.player(0).displayResources.samples.back()[1]);
        if (started < 0 && channelling()) started = t;
        if (const Unit* u = raised()) { r.raised = true; r.ticks = t - started + 1; r.hp = int(u->hp.toFloat() + 0.5f); break; }
        if (started >= 0 && !channelling()) { r.ticks = t - started + 1; break; }   // abandoned
    }
    r.mana = w.player(0).mana;
    return r;
}

static void reviveCostsNothing() {
    std::printf("reviving costs nothing; the allocation paces it:\n");
    char detail[160];
    const ReviveRun full = reviveRun(false, 1.0, false), half = reviveRun(false, 0.5, false);
    std::snprintf(detail, sizeof detail, "full %d ticks, mana %.1f, request %.1f; half %d ticks, mana %.1f",
                  full.ticks, full.mana, full.request, half.ticks, half.mana);
    check(full.raised && full.mana == 80.0 && full.request == 0, "a resurrect pays and requests no mana", detail);
    check(full.ticks == 90, "...takes 0.3 x buildtime / workertime at full allocation", detail);
    check(half.raised && half.mana == 80.0 && half.ticks == 180, "...and twice that at half allocation", detail);
    check(full.hp == 10, "a resurrected unit returns at 10% health", detail);
    const ReviveRun ghoul = reviveRun(true, 1.0, false);
    std::snprintf(detail, sizeof detail, "raised %d after %d ticks, hp %d, mana %.1f", ghoul.raised, ghoul.ticks,
                  ghoul.hp, ghoul.mana);
    check(ghoul.raised && ghoul.ticks == 90 && ghoul.hp == 100 && ghoul.mana == 80.0,
          "an animate takes the full buildtime, free, and rises at full health", detail);
    // Short from the channel's 30th tick (the allocation follows the demand
    // a tick later): 31 ticks of full work, the other 59 at half take 119.
    const ReviveRun aiShort = reviveRun(false, 0.5, true, 30), aiFull = reviveRun(false, 1.0, true);
    std::snprintf(detail, sizeof detail, "short: raised %d after %d ticks; full: raised %d",
                  aiShort.raised, aiShort.ticks, aiFull.raised);
    check(!aiShort.raised && aiShort.ticks == 150 && aiFull.raised,
          "a computer player under 0.7 allocation abandons it at the end", detail);
}

// AN ABANDONED REVIVE IS NOT TAKEN UP AGAIN. Retail's revive is an order;
// after 0x406e79 abandons it nothing gives it again. Ours was an idle caster's
// search, which took the same corpse up again on the next tick and abandoned
// it at every end for as long as the economy stayed short. Since 0.9.12 the
// revive is an order here too: abandoned, the order is over, the caster
// stands, and the body lies there even after the allocation recovers.
static void abandonedReviveWaits() {
    std::printf("an abandoned revive is not taken up again:\n");
    auto ownedWorld = makeWorld(64, 64);
    World& w = *ownedWorld;
    FeatType body; body.name = "victim_dead"; body.resurrectable = true; body.decomposeTicks = 100000;
    w.setFeatureTypes({body});
    UnitType victim = soldierType(); victim.id = "victim"; victim.buildTime = 100; victim.buildCost = 400;
    UnitType priest = soldierType(); priest.id = "priest"; priest.workerTime = 10; priest.canResurrect = true;
    w.mapCorpse(&victim, 0);
    w.player(0).automaticGates = true;
    const int caster = w.spawn(&priest, 300, 300, 0, 0);
    const int dead = w.spawn(&victim, 330, 300, 0, 0);
    w.unit(dead)->hp = {}; w.unit(dead)->deathType = 9;
    w.resurrect(caster, dead, false);
    w.player(0).mana = 80;
    auto raised = [&] {
        for (const auto& u : w.units()) if (u.alive() && u.type == &victim) return true;
        return false;
    };
    // Full until the channel starts, then short: it is abandoned at its end.
    int started = -1, abandoned = -1, restarts = 0, raisedAt = -1;
    bool was = false;
    for (int t = 0; t < 1500 && raisedAt < 0; ++t) {
        const bool isShort = started >= 0 && t < 900;
        w.player(0).constructionDemand = isShort ? w.player(0).mana / 0.5 : 0;
        w.tick(1.0f / 30);
        const bool now = w.unit(caster)->reviveTarget != 0;
        if (now && started < 0) started = t;
        if (was && !now && !raised() && abandoned < 0) abandoned = t;
        else if (now && !was && abandoned >= 0 && t < 900) ++restarts;
        was = now;
        if (raised()) raisedAt = t;
    }
    char detail[160];
    std::snprintf(detail, sizeof detail, "started %d, abandoned %d, restarts while short %d, raised %d",
                  started, abandoned, restarts, raisedAt);
    check(started >= 0 && abandoned > started, "a computer caster that turns short abandons the revive", detail);
    check(restarts == 0 && w.unit(caster)->orders.empty(), "...its order ends, and it does not take the corpse up again", detail);
    check(raisedAt < 0 && w.unit(dead)->deadFor < w.unit(dead)->corpseUntil,
          "...not even once the allocation recovers: the body stays", detail);
}

// REVIVES RESPECT THE UNIT LIMIT. Retail's revive end (0x406f0c) creates the
// unit through 0x512060, which refuses it past the player's unit limit (and
// the type's totalallowed): the revive ends unraised (NOMOREUNITS) and the
// corpse stays. Ours spawned it regardless.
static void reviveRespectsUnitLimit() {
    std::printf("revives respect the unit limit:\n");
    FeatType body; body.name = "victim_dead"; body.resurrectable = true; body.animatable = true;
    body.decomposeTicks = 100000;
    UnitType victim = soldierType(); victim.id = "victim"; victim.buildTime = 100; victim.buildCost = 400;
    UnitType ghoul = soldierType(); ghoul.id = "ghoul"; ghoul.buildTime = 30; ghoul.buildCost = 400;
    UnitType filler = soldierType(); filler.id = "filler";
    struct Run { bool started = false, raised = false, corpse = false; int count = 0; };
    // capAt: the unit limit; fillAt: the channel tick at which a unit is
    // added (-1: none); animate: an animate (ghoul) instead of a resurrect.
    auto run = [&](int cap, int fillAt, bool animate, int ghoulLimit) {
        auto ownedWorld = makeWorld(64, 64);
        World& w = *ownedWorld;
        w.setFeatureTypes({body});
        UnitType priest = soldierType(); priest.id = "priest"; priest.workerTime = 10;
        UnitType g = ghoul; g.totalAllowed = ghoulLimit;
        if (animate) { priest.canAnimate = true; priest.animateType = &g; } else priest.canResurrect = true;
        w.mapCorpse(&victim, 0);
        w.setUnitCap(cap);
        const int caster = w.spawn(&priest, 300, 300, 0, 0);
        const int dead = w.spawn(&victim, 330, 300, 0, 0);
        if (ghoulLimit > 0) w.spawn(&g, 900, 900, 0, 0);
        w.unit(dead)->hp = {}; w.unit(dead)->deathType = 9;
        w.resurrect(caster, dead, false);
        const UnitType* out = animate ? static_cast<const UnitType*>(&g) : &victim;
        Run r;
        int started = -1;
        for (int t = 0; t < 400; ++t) {
            w.tick(1.0f / 30);
            if (started < 0 && w.unit(caster)->reviveTarget) { started = t; r.started = true; }
            if (fillAt >= 0 && started >= 0 && t - started == fillAt) w.spawn(&filler, 900, 300, 0, 0);
        }
        for (const auto& u : w.units()) {
            if (u.alive() && u.type == out && u.id != dead && u.player == 0 && (u.x.toFloat() < 600)) r.raised = true;
            if (u.alive() && u.player == 0) ++r.count;
        }
        r.corpse = w.unit(dead) && !w.unit(dead)->corpseFading && w.unit(dead)->deadFor < w.unit(dead)->corpseUntil;
        return r;
    };
    char detail[160];
    const Run room = run(3, -1, false, 0);
    std::snprintf(detail, sizeof detail, "started %d raised %d units %d", room.started, room.raised, room.count);
    check(room.started && room.raised && room.count == 2, "below the limit the corpse is resurrected", detail);
    const Run full = run(1, -1, false, 0);
    std::snprintf(detail, sizeof detail, "started %d raised %d units %d", full.started, full.raised, full.count);
    check(!full.raised && full.count == 1, "at the unit limit no corpse is resurrected", detail);
    const Run late = run(2, 30, false, 0);
    std::snprintf(detail, sizeof detail, "started %d raised %d units %d corpse %d", late.started, late.raised,
                  late.count, late.corpse);
    check(late.started && !late.raised && late.count == 2 && late.corpse,
          "a revive that reaches the limit while channelling ends unraised, the corpse kept", detail);
    const Run ghoulFull = run(1, -1, true, 0);
    std::snprintf(detail, sizeof detail, "started %d raised %d units %d", ghoulFull.started, ghoulFull.raised,
                  ghoulFull.count);
    check(!ghoulFull.raised && ghoulFull.count == 1, "at the unit limit no corpse is animated", detail);
    const Run ghoulCap = run(0, -1, true, 1);
    std::snprintf(detail, sizeof detail, "started %d raised %d units %d", ghoulCap.started, ghoulCap.raised,
                  ghoulCap.count);
    check(!ghoulCap.raised && ghoulCap.count == 2, "an animate past its type's totalallowed is refused", detail);
}

// THE REVIVE COUNTER WRAPS LIKE _FTOL. Retail's 5d3d54 keeps the low word of
// a 64-bit fistp, so araking (workertime 10) resurrecting a buildtime-100000
// vermage starts at 1603273293 (5898240589 mod 2^32) and takes 24464 ticks;
// ours answered INT32_MIN for anything past 2^31 and raised it at once.
static void reviveWorkWrapsLikeFtol() {
    std::printf("the revive counter wraps like _ftol:\n");
    auto ownedWorld = makeWorld(64, 64);
    World& w = *ownedWorld;
    FeatType body; body.name = "victim_dead"; body.resurrectable = true; body.decomposeTicks = 100000;
    w.setFeatureTypes({body});
    UnitType victim = soldierType(); victim.id = "victim"; victim.buildTime = 100000;
    UnitType priest = soldierType(); priest.id = "priest"; priest.workerTime = 10; priest.canResurrect = true;
    w.mapCorpse(&victim, 0);
    const int id = w.spawn(&priest, 300, 300, 0, 0);
    const int dead = w.spawn(&victim, 330, 300, 0, 0);
    w.unit(dead)->hp = {}; w.unit(dead)->deathType = 9;
    w.resurrect(id, dead, false);
    int32_t first = 0; int t = 0;
    for (; t < 600 && !first; ++t) { w.tick(1.0f / 30); if (w.unit(id)->reviveTarget) first = w.unit(id)->reviveWork; }
    for (int n = 0; n < 60; ++n) w.tick(1.0f / 30);
    char detail[160];
    // The channel's first wake has already taken one tick's allocation (65536).
    std::snprintf(detail, sizeof detail, "work %d after the first wake, still channelling %d", first,
                  w.unit(id)->reviveTarget != 0);
    check(first == 1603273293 - 65536 && w.unit(id)->reviveTarget != 0,
          "a counter past 2^31 keeps ftol's low word and keeps channelling", detail);
}

// WORK AND SHOTS KEEP A CLOAKER VISIBLE. Retail sets the unit's cloak block
// (+0xcc) to max(+0xcc, now + N) on every shot (600 ticks, 0x52af41), on every
// MobileBuild/HelpBuild work tick (300, 0x405bc8/0x4060bb), on every RECLAIM
// wake with countdown left (300, 0x406576), on every resurrect channel tick
// (300, 0x406e45) and on every RepairUnit work tick (150, 0x4076ae). The cloak
// step decloaks while the block lies ahead.
static UnitType cloakerType(const char* id) {
    UnitType t = soldierType();
    t.id = id; t.canCloak = true; t.cloakCost = 3; t.cloakCostMove = 3;
    t.maxMana = 1000; t.manaRegen = 300;
    return t;
}
// Runs `w` until `done`, tracking the largest block written ahead of the
// tick and whether the unit was ever cloaked while `working` once the work
// stage had begun (the first block).
struct BlockRun { uint32_t ahead = 0; bool cloakedWhileWorking = false; int ticks = 0; };
template <class Working, class Done>
static BlockRun watchBlock(World& w, int id, int limit, Working working, Done done) {
    BlockRun r;
    for (; r.ticks < limit && !done(); ++r.ticks) {
        const uint32_t before = w.unit(id)->cloakBlockedUntil;
        w.tick(1.0f / 30);
        const Unit& u = *w.unit(id);
        if (u.cloakBlockedUntil != before) r.ahead = std::max(r.ahead, u.cloakBlockedUntil - w.tickCount());
        if (r.ahead && working() && u.cloaked) r.cloakedWhileWorking = true;
    }
    return r;
}

static void workAndShotsBlockCloak() {
    std::printf("shots and work keep a cloaker visible:\n");
    char detail[160];
    {
        auto ownedWorld = makeWorld(64, 64);
        World& w = *ownedWorld;
        UnitType spy = cloakerType("spy");
        Weapon weapon; weapon.name = "cloak-test-weapon"; weapon.range = 300; weapon.damage = 1;
        weapon.reload = 1000; weapon.aimTol = 32767; spy.weapon = weapon; spy.weapons.push_back(weapon);
        UnitType dummy = soldierType(); dummy.id = "dummy"; dummy.maxHp = 30000;
        // Upright, so both stand on the terrain (surfaceHeight): Standby's
        // scan (4d8370) takes only an enemy its owner sees, and a body left
        // at y 0 under height-100 ground projects onto the wrong sight cells.
        spy.upright = dummy.upright = true;
        const int id = w.spawn(&spy, 300, 300, 0, 0);
        w.setCloak(id, true);
        for (int t = 0; t < 5; ++t) w.tick(1.0f / 30);
        const bool cloakedBefore = w.unit(id)->cloaked;
        w.spawn(&dummy, 400, 300, 0, 1);
        int shot = -1, visible = 0;
        for (int t = 0; t < 300 && shot < 0; ++t) {
            w.tick(1.0f / 30);
            if (w.unit(id)->cloakBlockedUntil) shot = t;
        }
        const uint32_t ahead = shot >= 0 ? w.unit(id)->cloakBlockedUntil - w.tickCount() : 0;
        if (shot >= 0) {
            visible = 1;   // the tick it fired
            for (w.tick(1.0f / 30); !w.unit(id)->cloaked && visible < 1000; w.tick(1.0f / 30)) ++visible;
        }
        std::snprintf(detail, sizeof detail, "cloaked before %d, shot at %d, block %u ahead, visible %d ticks",
                      cloakedBefore, shot, ahead, visible);
        check(cloakedBefore && shot >= 0 && ahead == 600 && visible == 600,
              "a cloaked unit that fires is visible for 600 ticks", detail);
    }
    {
        auto ownedWorld = makeWorld(128, 128);
        World& w = *ownedWorld;
        UnitType mason = cloakerType("mason"); mason.isBuilder = true; mason.workerTime = 10;
        UnitType hall = factoryType(); hall.id = "hall"; hall.isBuilder = false; hall.footX = hall.footZ = 2;
        hall.buildTime = 100000; hall.buildCost = 10;
        const int id = w.spawn(&mason, 500, 500, 0, 0);
        w.setCloak(id, true);
        w.tick(1.0f / 30);
        w.player(0).mana = 90;
        const int site = w.startBuild(id, &hall, 540, 500, World::Approach::None);
        const BlockRun r = watchBlock(w, id, 120, [&] { return w.unit(id)->buildSiteId != 0; }, [] { return false; });
        std::snprintf(detail, sizeof detail, "site %d, block %u ahead, cloaked while building %d",
                      site, r.ahead, r.cloakedWhileWorking);
        check(site && r.ahead == 300 && !r.cloakedWhileWorking, "construction work blocks cloaking for 300 ticks", detail);
    }
    {
        auto ownedWorld = makeWorld(128, 128);
        World& w = *ownedWorld;
        UnitType mender = cloakerType("mender"); mender.isBuilder = true; mender.workerTime = 1;
        UnitType hurt = soldierType(); hurt.id = "hurt"; hurt.maxHp = 30000; hurt.buildTime = 100000; hurt.buildCost = 1;
        const int id = w.spawn(&mender, 500, 500, 0, 0);
        const int patient = w.spawn(&hurt, 530, 500, 0, 0);
        w.unit(patient)->hp = Fixed::fromInt(10);
        w.setCloak(id, true);
        w.tick(1.0f / 30);
        w.repair(id, patient, false);
        const BlockRun r = watchBlock(w, id, 120, [&] { return w.unit(id)->repairId != 0; }, [] { return false; });
        std::snprintf(detail, sizeof detail, "block %u ahead, cloaked while repairing %d", r.ahead, r.cloakedWhileWorking);
        check(r.ahead == 150 && !r.cloakedWhileWorking, "repair work blocks cloaking for 150 ticks", detail);
    }
    {
        auto ownedWorld = makeWorld(128, 128);
        World& w = *ownedWorld;
        UnitType clearer = cloakerType("clearer"); clearer.isBuilder = true; clearer.canReclaim = true;
        clearer.footX = clearer.footZ = 1; clearer.turnInPlaceRate = 0; clearer.workerTime = 10;
        w.addFeature(1, 20 * 16 + 8, 20 * 16 + 8, 250, 1, 1, false);
        const int id = w.spawn(&clearer, 19 * 16 + 8, 20 * 16 + 8, 0.0f, 0);
        w.setCloak(id, true);
        w.tick(1.0f / 30);
        w.reclaim(id, 1, false);
        const BlockRun r = watchBlock(w, id, 400, [&] { return w.unit(id)->reclaimId != 0; },
                                      [&] { return !w.feature(1)->alive; });
        std::snprintf(detail, sizeof detail, "block %u ahead, cloaked while reclaiming %d, %d ticks",
                      r.ahead, r.cloakedWhileWorking, r.ticks);
        check(r.ahead == 300 && !r.cloakedWhileWorking, "reclaim work blocks cloaking for 300 ticks", detail);
    }
    {
        auto ownedWorld = makeWorld(64, 64);
        World& w = *ownedWorld;
        FeatType body; body.name = "victim_dead"; body.resurrectable = true; body.decomposeTicks = 100000;
        w.setFeatureTypes({body});
        UnitType victim = soldierType(); victim.id = "victim"; victim.buildTime = 100;
        UnitType priest = cloakerType("priest"); priest.workerTime = 10; priest.canResurrect = true;
        w.mapCorpse(&victim, 0);
        const int id = w.spawn(&priest, 300, 300, 0, 0);
        w.setCloak(id, true);
        w.tick(1.0f / 30);
        const int dead = w.spawn(&victim, 330, 300, 0, 0);
        w.unit(dead)->hp = {}; w.unit(dead)->deathType = 9;
        w.resurrect(id, dead, false);
        const BlockRun r = watchBlock(w, id, 200, [&] { return w.unit(id)->reviveTarget != 0; }, [] { return false; });
        std::snprintf(detail, sizeof detail, "block %u ahead, cloaked while channelling %d", r.ahead, r.cloakedWhileWorking);
        check(r.ahead == 300 && !r.cloakedWhileWorking, "a resurrect channel blocks cloaking for 300 ticks", detail);
    }
}

// THE CLOAK STEP'S OWN TESTS. Retail charges cloakcostmoving whenever the
// unit's MoveRate tier (+0x130 bits 2-3) is not 0 -- any motion at all, where
// ours wanted more than 3 px/s -- and nothing but its own mana and the block
// decloaks it: mincloakdistance only draws a circle (0x4d6530).
static void cloakStepFollowsRetail() {
    std::printf("the cloak step follows retail:\n");
    char detail[160];
    {
        auto ownedWorld = makeWorld(64, 64);
        World& w = *ownedWorld;
        UnitType creeper = soldierType(); creeper.id = "creeper";
        creeper.maxVel = Fixed::fromFloat(1.5f / 30.0f);   // 1.5 px/s
        creeper.canCloak = true; creeper.cloakCost = 3; creeper.cloakCostMove = 30;
        creeper.maxMana = 1000; creeper.manaRegen = 0;
        const int id = w.spawn(&creeper, 300, 300, 0, 0);
        w.unit(id)->mana = 1000;   // a new unit's pool starts empty (511990)
        w.setCloak(id, true);
        w.order(id, 300, 800, false);
        for (int t = 0; t < 30; ++t) w.tick(1.0f / 30);
        const double before = w.unit(id)->mana;
        w.tick(1.0f / 30);
        const double paid = before - w.unit(id)->mana;
        std::snprintf(detail, sizeof detail, "speed %.3f px/s, paid %.4f a tick",
                      w.unit(id)->speed.toFloat() * 30, paid);
        check(w.unit(id)->speed.v > 0 && w.unit(id)->cloaked && std::fabs(paid - 1.0) < 1e-4,
              "a slow walker pays the moving cost", detail);
    }
    {
        auto ownedWorld = makeWorld(64, 64);
        World& w = *ownedWorld;
        UnitType spy = soldierType(); spy.id = "spy";
        spy.canCloak = true; spy.cloakCost = 3; spy.cloakCostMove = 3; spy.minCloakDist = 80;
        spy.maxMana = 1000; spy.manaRegen = 300;
        UnitType enemy = soldierType(); enemy.id = "enemy";
        const int id = w.spawn(&spy, 300, 300, 0, 0);
        w.unit(id)->mana = 1000;   // a new unit's pool starts empty (511990)
        w.spawn(&enemy, 330, 300, 0, 1);
        w.setCloak(id, true);
        int cloaked = 0;
        for (int t = 0; t < 30; ++t) { w.tick(1.0f / 30); cloaked += w.unit(id)->cloaked; }
        std::snprintf(detail, sizeof detail, "cloaked %d of 30 ticks with an enemy 30 px away", cloaked);
        check(cloaked == 30, "an enemy inside mincloakdistance does not decloak", detail);
    }
}

// A LODESTONE UPGRADE FINISHES UNDER A SHORT SHARE. Playtest (protocol 273):
// a Zhon Shaman's divine lodestone sat at full hp, completedWork 1-2^-53, for
// 3.5 minutes, and finished the tick after the player stopped one producer.
// The replacement path clamped the tick's work to what was left and THEN scaled
// it by the share, so the last stretch shrank geometrically and, one ulp from
// done, a share <= 0.5 rounded the step away. Retail (429af0) scales first and
// completes at remaining <= 0.
static void lodestoneUpgradeFinishesUnderShortShare() {
    std::printf("a lodestone upgrade finishes while income is short:\n");
    auto ownedWorld = makeWorld(128, 128);
    World& w = *ownedWorld;
    w.setManaSpots({{512, 512}});
    UnitType lode{}, upgrade{};
    lode.name = lode.id = "zonlode"; lode.side = "ZON"; lode.maxHp = 1000;
    lode.footX = lode.footZ = 2; lode.onMana = true; lode.buildTime = 1; lode.buildCost = 1;
    lode.maxVel = tak::sim::Fixed();                                     // a structure
    upgrade = lode; upgrade.name = upgrade.id = "zonmana";
    upgrade.buildTime = 10; upgrade.buildCost = 1200;                      // 300 ticks, 4/tick
    UnitType shaman = soldierType(); shaman.id = "shaman";
    shaman.isBuilder = true; shaman.workerTime = 1; shaman.buildDist = 64;
    // A long, costly second build keeps the other demand at twice the income,
    // so the allocation stays below one half even when the upgrade asks for
    // nothing -- the playtest's shape (other demand 8.2/tick on 3.5 of income).
    UnitType hall{}; hall.name = hall.id = "hall"; hall.maxVel = tak::sim::Fixed();
    hall.maxHp = 1000; hall.footX = hall.footZ = 2;
    hall.buildTime = 200; hall.buildCost = 48000;                        // 6000 ticks, 8/tick
    UnitType well{}; well.name = well.id = "well"; well.maxHp = 100;
    well.footX = well.footZ = 2; well.income = 96;                       // 3.2/tick
    well.storage = 10000;
    const int bid = w.spawn(&shaman, 512, 560, 0, 0);
    const int helper = w.spawn(&shaman, 1000, 1048, 0, 0);
    w.spawn(&well, 1400, 1400, 0, 0);
    const int old = w.startBuild(bid, &lode, 512, 512, World::Approach::None);
    check(old != 0, "the base lodestone is placed");
    if (!old) return;
    w.unit(old)->underConstruction = false;
    w.unit(old)->hp = tak::sim::Fixed::fromInt(lode.maxHp);
    w.cancelBuilds(bid); w.stop(bid);
    w.player(0).mana = 0;
    check(w.startBuild(helper, &hall, 1000, 1000, World::Approach::None) != 0,
          "a second builder starts a costly building");
    const int site = w.startBuild(bid, &upgrade, 512, 512, World::Approach::None);
    check(site != 0 && w.unit(site)->lodestoneReplacement,
          "the upgrade replaces the base lodestone");
    if (!site || !w.unit(site)->lodestoneReplacement) return;
    // Demand 12/tick on 3.2/tick of income: allocation 0.267, so 300 / 0.267 =
    // 1125 ticks, give or take the first tick, which has no demand to divide by.
    const int expected = 1125;
    int ticks = 0;
    for (; ticks < 3 * expected && w.unit(site)->underConstruction; ++ticks) w.tick(1.0f / 30.0f);
    char detail[160];
    std::snprintf(detail, sizeof detail, "%d ticks (expected ~%d), completedWork %.17g, allocation %.3f",
                  ticks, expected, w.unit(site)->lodestoneReplacement ?
                  w.unit(site)->lodestoneReplacement->completedWork : -1.0,
                  w.player(0).constructionAllocation);
    check(w.unit(site)->alive() && !w.unit(site)->underConstruction &&
          w.unit(site)->lodestoneReplacement->completedWork == 1.0,
          "the upgrade completes at allocation 0.27", detail);
    check(ticks <= expected + 10, "...at the shared rate, with no stalled last tick", detail);
    check(w.player(0).mana >= 0, "...and the pool is never overdrawn", detail);
}

static void outputExistsDuringConstruction() {
    auto ownedWorld = makeWorld(128,128);
    World& w = *ownedWorld;
    UnitType fac=factoryType(),sol=soldierType();
    fac.workerTime=1; sol.buildTime=2; sol.buildCost=60;
    fac.storage=10000;
    const int fid=w.spawn(&fac,1000,1000,0,0);
    w.player(0).mana=10;
    w.train(fid,&sol,1);
    w.tick(1.0f/30.0f);
    check(w.player(0).displayResources.samples.back()[1]>0,
          "factory spending reaches HUD accounting without a mobile buildSiteId");
    const int siteId=w.unit(fid)->productionSiteId;
    check(siteId && w.unit(siteId)->underConstruction && w.unit(siteId)->buildBegun,
          "factory output exists and is unfinished while construction proceeds");
    if (!siteId) return;
    const auto hash=w.stateHash();
    w.player(0).displayResources.samples[0][0]+=100;
    check(w.stateHash()==hash,"HUD resource history does not affect lockstep state");
    ++w.unit(fid)->buildProgress;
    check(w.stateHash()!=hash,"factory progress participates in the lockstep checksum");
    --w.unit(fid)->buildProgress;
    run(w,1);
    check(w.unit(fid)->productionSiteId==siteId && w.unit(siteId)->underConstruction &&
          w.unit(fid)->buildProgress==10,"mana starvation pauses work on the same unfinished unit");
    w.player(0).mana=50;
    run(w,2);
    check(!w.unit(siteId)->underConstruction && w.unit(fid)->productionSiteId==0 &&
          w.unit(fid)->buildQueue.empty() && w.player(0).mana==0,
          "completion releases the same unit for exactly its remaining cost");
    w.train(fid,&sol,1);
    w.tick(1.0f/30.0f);
    const int canceled=w.unit(fid)->productionSiteId;
    w.dequeue(fid,&sol,1);
    run(w,1);
    check(canceled && !w.unit(canceled)->alive() && w.unit(fid)->productionSiteId==0,
          "canceling production releases the unfinished site to decay");
    w.setUnitCap(3); // producer, earlier output, and one unfinished output
    w.player(0).mana=60;
    w.train(fid,&sol,1);
    w.tick(1.0f/30.0f);
    const int damaged=w.unit(fid)->productionSiteId;
    if (damaged) w.unit(damaged)->hp=Fixed();
    w.tick(1.0f/30.0f);
    check(damaged && !w.unit(damaged)->alive() && w.unit(fid)->productionSiteId==0,
          "construction cannot resurrect output killed before the death sweep");
    w.setUnitCap(2);
    w.player(0).mana=60;
    run(w,3);
    int complete=0;
    for (const auto& u:w.units())
        if (u.alive() && u.type==&sol && !u.underConstruction) ++complete;
    // One output already completed earlier in this fixture, so raise the cap
    // below to admit a replacement and verify completion at that cap.
    w.setUnitCap(3);
    run(w,3);
    int after=0;
    for (const auto& u:w.units())
        if (u.alive() && u.type==&sol && !u.underConstruction) ++after;
    check(after==complete+1,"an admitted construction site can finish at the unit cap");
}

static void outputDoesNotJam() {
    std::printf("a factory's output does not jam on one spot:\n");
    auto ownedWorld = makeWorld(128, 128);
    World& w = *ownedWorld;
    UnitType fac = factoryType(), sol = soldierType();
    const int fid = w.spawn(&fac, 1000, 1000, 0, 0);
    w.player(0).mana = 1e9f;
    w.train(fid, &sol, 24);
    int firstOverlapTick = -1;
    for (int tick = 0; tick < 180 * 30; ++tick) {
        w.tick(1.0f / 30.0f);
        if (firstOverlapTick >= 0) continue;
        const auto& units = w.units();
        for (size_t a = 0; a < units.size(); ++a) {
            if (!units[a].alive() || units[a].type->isStructure()) continue;
            for (size_t b = a + 1; b < units.size(); ++b) {
                if (!units[b].alive() || units[b].type->isStructure()) continue;
                // All output in this fixture is the same 2x2 soldier. Check
                // cell ownership throughout the run, not just a final snapshot
                // after an illegal overlap might have unwound by itself.
                const int dx = std::abs(footprintOrigin(units[a].x, sol.footX) - footprintOrigin(units[b].x, sol.footX));
                const int dz = std::abs(footprintOrigin(units[a].z, sol.footZ) - footprintOrigin(units[b].z, sol.footZ));
                if (dx < sol.footX && dz < sol.footZ) firstOverlapTick = tick;
            }
        }
    }
    check(firstOverlapTick < 0, "moving output never shares occupied footprint cells",
          firstOverlapTick < 0 ? "5400 ticks checked" : "first overlap at tick " + std::to_string(firstOverlapTick));

    // Snapshot, run on, and see who moved. A unit that still holds orders but has not
    // moved is one pushing at something it will never get past.
    std::vector<std::pair<int, std::pair<float, float>>> was;
    for (const auto& u : w.units())
        if (u.alive() && u.type && !u.type->isStructure()) was.push_back({u.id, {u.x.toFloat(), u.z.toFloat()}});
    // FORTY seconds, not ten: a pair wedged head-on unwedges when one of them
    // re-routes around the other, and the re-ask runs on the randomised deadlines
    // (up to ~(58 x halfCellTicks) ticks for the success shape). A 10s window
    // sampled inside one cadence interval and called retail's own pacing "stuck".
    run(w, 40.0f);

    int total = 0, stuck = 0, lost = 0;
    for (const auto& u : w.units()) {
        if (!u.alive() || !u.type || u.type->isStructure()) continue;
        ++total;
        if (u.orders.empty()) continue;
        for (auto& [id, p] : was)
            if (id == u.id) {
                const float dx = u.x.toFloat() - p.first, dz = u.z.toFloat() - p.second;
                if (std::sqrt(dx * dx + dz * dz) < 8.0f) {
                    ++stuck;
                    // A stationary unit with a live order is RETAIL behaviour in one
                    // place only: pressing at its own goal ring, waiting on a spot a
                    // countryman is parked on (the order stays alive; if the spot
                    // clears it walks in). Stationary anywhere ELSE is the jam this
                    // test exists to catch. The ring is retail's goal-crowding
                    // tolerance, 50 / halfCellTicks cells Chebyshev (0x414563).
                    const auto& legEnd = u.orders[World::currentLeg(u.orders)];
                    const float tolPx =
                        float((u.type->halfCellTicks > 0 ? 50 / u.type->halfCellTicks : 0) * 16 + 16);
                    const float ch = std::max(std::fabs(u.x.toFloat() - legEnd.x.toFloat()),
                                              std::fabs(u.z.toFloat() - legEnd.z.toFloat()));
                    if (ch > tolPx) ++lost;
                }
                break;
            }
    }
    check(total >= 20, "the factory actually produced its queue",
          std::to_string(total) + " units");
    check(lost == 0, "no unit is stalled anywhere but its own goal ring",
          std::to_string(lost) + " of " + std::to_string(total) + " lost (" +
              std::to_string(stuck) + " pressing at their ring, which retail does)");

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
            const float dx = live[a]->x.toFloat() - live[b]->x.toFloat(), dz = live[a]->z.toFloat() - live[b]->z.toFloat();
            if (std::sqrt(dx * dx + dz * dz) < need * 0.6f) {
                ++overlaps;
                std::printf("    overlap: units %d/%d at (%.2f,%.2f)/(%.2f,%.2f)\n",
                            live[a]->id, live[b]->id, live[a]->x.toFloat(),
                            live[a]->z.toFloat(), live[b]->x.toFloat(), live[b]->z.toFloat());
            }
        }
    check(overlaps == 0, "and no two of them are standing inside each other",
          std::to_string(overlaps) + " overlapping pairs");
}

// ---------------------------------------------------------------------------
// 2. A queued rally on the building is adopted by everything it makes.
// ---------------------------------------------------------------------------
static void rallyIsAdopted() {
    std::printf("a production building's rally orders:\n");
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
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
    check(f->heading == tak::sim::Bam(0), "and its heading is untouched",
          std::to_string(f->heading.v));
    // Setting a rally must not cancel what it is building.
    check(!f->buildQueue.empty(), "the production queue survives the rally order",
          std::to_string(f->buildQueue.size()) + " queued");

    run(w, 240.0f);
    int n = 0, arrived = 0;
    for (const auto& u : w.units()) {
        if (!u.alive() || !u.type || u.type->isStructure()) continue;
        ++n;
        const float dx = u.x.toFloat() - 1600.0f, dz = u.z.toFloat() - 200.0f;
        if (std::sqrt(dx * dx + dz * dz) < 220.0f) ++arrived;
    }
    check(n >= 5, "the units were produced", std::to_string(n));
    check(n > 0 && arrived == n, "every one of them followed the rally to its last step",
          std::to_string(arrived) + " of " + std::to_string(n));
}

// ---------------------------------------------------------------------------
// 3. A rally set BEFORE anything is built still applies, and re-setting replaces.
// ---------------------------------------------------------------------------
static void mobileRallyIsAdopted() {
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
    UnitType builder = factoryType(), sol = soldierType();
    builder.maxVel = sol.maxVel; builder.canMove = true;
    const int id = w.spawn(&builder, 600, 600, 0, 0);
    w.player(0).mana = 1e9f;
    w.setRepeat(id, &sol);
    w.order(id, 1500, 600, false);
    w.patrolTo(id, 1500, 1000, true);
    const auto queue = w.unit(id)->buildQueue;
    check(queue.size()==1 && w.unit(id)->orders.empty(), "mobile rally preserves active production");
    int child = 0;
    for (int tick=0; tick<900 && !child; ++tick) {
        w.tick(1.0f / 30.0f);
        for (const auto& u : w.units())
            if (u.id!=id && u.alive() && !u.underConstruction) child=u.id;
    }
    const Unit* output = w.unit(child);
    check(output && output->orders.size()>=2 && output->orders.back().patrol &&
          output->orders.back().x==Fixed::fromInt(1500) && output->orders.back().z==Fixed::fromInt(1000),
          "mobile producer output inherits queued move and patrol rally");
    check(w.unit(id)->x==Fixed::fromInt(600) && w.unit(id)->z==Fixed::fromInt(600),
          "producing builder does not follow output rally");
    w.stop(id);
    check(!w.unit(id)->repeatType && w.unit(id)->buildQueue.empty(), "Stop cancels mobile repeat production");
}

// A mobile builder holds a rally only while it repeats. Ending the repeat any
// other way than Stop (the +++ toggle, or subtracting the last copy) gives the
// builder back too, so its plan must not linger for a later limited queue.
static void mobileRallyEndsWithRepeat() {
    std::printf("mobile producer: ending the repeat drops its rally:\n");
    UnitType builder = factoryType(), sol = soldierType();
    builder.maxVel = sol.maxVel; builder.canMove = true;
    for (bool toggle : {true, false}) {
        auto ownedWorld = makeWorld(160, 160);
        World& w = *ownedWorld;
        const int id = w.spawn(&builder, 600, 600, 0, 0);
        w.player(0).mana = 1e9f;
        w.setRepeat(id, &sol);
        w.order(id, 1500, 600, false);
        check(w.unit(id)->rally.size() == 1, "a repeating builder's Move sets its rally");
        if (toggle) w.setRepeat(id, &sol); else w.dequeue(id, &sol, 1);
        check(!w.unit(id)->repeatType && w.unit(id)->rally.empty(),
              toggle ? "toggling the repeat off clears the rally" : "subtracting the last copy clears the rally",
              std::to_string(w.unit(id)->rally.size()));
        w.order(id, 900, 900, false);
        check(!w.unit(id)->orders.empty() && w.unit(id)->rally.empty(),
              "the builder's next Move commands the builder itself");
    }
}

static void rallyReplaces() {
    std::printf("re-setting a rally replaces it (unqueued):\n");
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
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
        const float dx = u.x.toFloat() - 300.0f, dz = u.z.toFloat() - 1400.0f;
        if (std::sqrt(dx * dx + dz * dz) < 220.0f) ++arrived;
    }
    check(n > 0 && arrived == n, "and the output follows the NEW plan",
          std::to_string(arrived) + " of " + std::to_string(n));
}

// ---------------------------------------------------------------------------
// 4. Rally PLANS: Move, fight-move, patrol and attack, Shift-queued, on a
//    factory and on a mobile builder repeating production. Every output runs
//    the whole plan in order; a queued patrol step loops as a unit's own does.
// ---------------------------------------------------------------------------
static UnitType producerType(bool mobile) {
    UnitType t = factoryType();
    if (mobile) { t.maxVel = soldierType().maxVel; t.canMove = true; }
    return t;
}

static UnitType armedSoldierType() {
    UnitType t = soldierType();
    Weapon weapon; weapon.name = "rally-test-weapon"; weapon.range = 120; weapon.damage = 100;
    weapon.reload = 0.1f; weapon.aimTol = 32767; t.weapon = weapon; t.weapons.push_back(weapon);
    return t;
}

// The first finished output of `producer`, after which production stops so a
// stream of later outputs does not crowd the route being watched.
static int firstOutput(World& w, int producer, int maxTicks) {
    for (int n = 0; n < maxTicks; ++n) {
        w.tick(1.0f / 30.0f);
        for (const auto& u : w.units())
            if (u.id != producer && u.alive() && u.type && !u.type->isStructure() &&
                !u.underConstruction && u.player == w.unit(producer)->player) {
                Unit* p = w.unit(producer);
                p->repeatType = nullptr; p->buildQueue.clear();
                return u.id;
            }
    }
    return 0;
}

static bool within(const Unit& u, float x, float z, float r) {
    const float dx = u.x.toFloat() - x, dz = u.z.toFloat() - z;
    return dx * dx + dz * dz < r * r;
}

static void rallyPlanIsFollowed(bool mobile) {
    std::printf("%s: queued move, fight-move and patrol rally:\n", mobile ? "mobile producer" : "factory");
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
    UnitType prod = producerType(mobile), sol = soldierType();
    const int id = w.spawn(&prod, 600, 600, 0, 0);
    w.player(0).mana = 1e9f;
    w.setRepeat(id, &sol);
    w.order(id, 1100, 600, false);           // A
    w.attackMove(id, 1600, 600, true);       // Shift: fight-move B
    w.queuePatrol(id, 1600, 1100);           // Shift: patrol C
    const auto& r = w.unit(id)->rally;
    check(r.size() == 3 && !r[0].attackMove && !r[0].patrol && r[1].attackMove && !r[1].patrol &&
          r[2].patrol && r[2].patrolOpen, "the plan is stored in order: move, fight-move, open patrol",
          std::to_string(r.size()));
    check(w.unit(id)->orders.empty() && w.unit(id)->repeatType == &sol,
          "the producer itself keeps producing and does not move");
    const int child = firstOutput(w, id, 900);
    check(child != 0, "an output is produced");
    if (!child) return;
    // A, then B, then C, then back to B and C again: the patrol loops B <-> C.
    const std::pair<float, float> stops[] = {{1100, 600}, {1600, 600}, {1600, 1100}, {1600, 600}, {1600, 1100}};
    size_t stage = 0;
    for (int n = 0; n < 30 * 180 && stage < 5; ++n) {
        w.tick(1.0f / 30.0f);
        if (const Unit* u = w.unit(child); u && within(*u, stops[stage].first, stops[stage].second, 96.0f)) ++stage;
    }
    check(stage == 5, "the output visits A, B, C, then loops the patrol B <-> C",
          "reached " + std::to_string(stage) + " of 5 stops");
    check(w.unit(id)->x == Fixed::fromInt(600) && w.unit(id)->z == Fixed::fromInt(600),
          "the producer stayed where it was");
}

static void rallyAttackThenMove(bool mobile) {
    std::printf("%s: attack rally, then a queued move:\n", mobile ? "mobile producer" : "factory");
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
    UnitType prod = producerType(mobile), sol = armedSoldierType(), dummy = soldierType();
    dummy.maxVel = Fixed();  dummy.id = "dummy";                           // stands still
    const int id = w.spawn(&prod, 600, 600, 0, 0);
    const int enemy = w.spawn(&dummy, 1400, 600, 0, 1);
    w.player(0).mana = 1e9f;
    w.setRepeat(id, &sol);
    w.attack(id, enemy, false);
    w.order(id, 1400, 1200, true);
    const auto& r = w.unit(id)->rally;
    check(r.size() == 2 && r[0].targetId == enemy && !r[1].targetId,
          "attack is a rally step and the move queues behind it", std::to_string(r.size()));
    check(w.unit(id)->orders.empty(), "the producer does not attack itself");
    const int child = firstOutput(w, id, 900);
    check(child != 0, "an output is produced");
    if (!child) return;
    bool killed = false, arrived = false;
    for (int n = 0; n < 30 * 120 && !arrived; ++n) {
        w.tick(1.0f / 30.0f);
        const Unit* e = w.unit(enemy);
        killed |= !e || !e->alive();
        if (const Unit* u = w.unit(child); killed && u && within(*u, 1400, 1200, 96.0f)) arrived = true;
    }
    check(killed && arrived, "the output kills the target, then moves on",
          std::string(killed ? "killed" : "alive") + (arrived ? ", arrived" : ", not arrived"));
}

static void rallyQueueStopAndHash(bool mobile) {
    std::printf("%s: rally replace, Stop and lockstep hash:\n", mobile ? "mobile producer" : "factory");
    auto make = [&](World& w, UnitType& prod, UnitType& sol) {
        const int id = w.spawn(&prod, 600, 600, 0, 0);
        w.player(0).mana = 1e9f;
        w.setRepeat(id, &sol);
        return id;
    };
    UnitType prod = producerType(mobile), sol = soldierType();
    auto a = makeWorld(160, 160), b = makeWorld(160, 160);
    const int ia = make(*a, prod, sol), ib = make(*b, prod, sol);
    check(a->stateHash() == b->stateHash(), "two identical worlds hash alike");
    a->order(ia, 1100, 600, false);
    check(a->stateHash() != b->stateHash(), "a rally plan is part of the lockstep hash");
    a->attackMove(ia, 1600, 600, true);
    a->queuePatrol(ia, 1600, 1100);
    b->order(ib, 1100, 600, false);
    b->attackMove(ib, 1600, 600, true);
    b->queuePatrol(ib, 1600, 1100);
    for (int n = 0; n < 30 * 40; ++n) { a->tick(1.0f / 30.0f); b->tick(1.0f / 30.0f); }
    check(a->stateHash() == b->stateHash(), "the same plan runs to the same hash twice");
    a->order(ia, 300, 1400, false);
    check(a->unit(ia)->rally.size() == 1, "an unqueued order replaces the whole plan",
          std::to_string(a->unit(ia)->rally.size()));
    a->stop(ia);
    check(a->unit(ia)->rally.empty(), "Stop clears the rally plan",
          std::to_string(a->unit(ia)->rally.size()));
}

static void scriptControlsReadinessAndPosition() {
    std::printf("factory script readiness and exact piece position:\n");
    auto ownedWorld = makeWorld(160,160);
    World& w = *ownedWorld;
    UnitType fac=factoryType(),sol=soldierType();
    sol.buildTime=100000;
    // The pad lies on the Barracks' door rows, which the script opens (YARD_OPEN)
    // before it reports ready, as the shipped factories do: BUILD stage 2
    // refuses a site on a solid or closed yard cell.
    fac.yardMap=std::string(7*2,'.')+std::string(7*6,'o')+std::string(7*4,'c');
    auto script=std::make_shared<tak::cob::File>();
    script->pieces={"emit","root"};
    script->scripts={{"Create",0},{"QueryBuildInfo",19}};
    script->code={0x10021001,5,0x10021001,0,0x10082000,
                  0x10021001,100,0x10013000,
                  0x10021001,18,0x10021001,1,0x10082000,
                  0x10021001,5,0x10021001,1,0x10082000,0x10065000,
                  0x10021001,0,0x10023002,0,0x10065000};
    fac.productionScript=script;
    fac.productionModel={{{0,0,0},-1,1},{{17,1024,64*65536+37},0,0}};
    const int fid=w.spawn(&fac,600,600,3.14159265f,0);
    w.player(0).mana=100000;
    w.train(fid,&sol,1);
    for (int i=0;i<3;++i) w.tick(1.0f/30.0f);
    check(w.unit(fid)->productionSiteId==0,"sleeping factory script prevents early creation");
    w.tick(1.0f/30.0f);
    const Unit* site=w.unit(w.unit(fid)->productionSiteId);
    check(site && site->underConstruction,"ready signal creates the output on that update");
    check(site && site->x.v==600*65536-17 && site->z.v==664*65536+37 &&
          site->flightY.v==100*65536+1024,"QueryBuildInfo retains all raw coordinate bits");
}

static void mobileScriptRandomRunsInWorld() {
    auto ownedWorld = makeWorld(64,64);
    World& w = *ownedWorld;
    UnitType mobile=soldierType();
    auto script=std::make_shared<tak::cob::File>();
    script->scripts={{"Create",0}};
    script->code={0x10021001,100,0x10013000,0x10021001,0,0x10021001,9,
                  0x10041000,0x10024000,0x10065000};
    mobile.simulationScript=script;
    const int id=w.spawn(&mobile,256,256);
    std::vector<World::RngObservation> draws;
    w.setRngObserver([&](const auto& draw){if (draw.bound==10) draws.push_back(draw);});
    for (int i=0;i<3;++i) w.tick(1.0f/30.0f);
    check(draws.empty(),"mobile COB sleep waits on simulation ticks");
    w.tick(1.0f/30.0f);
    check(draws.size()==1 && draws[0].bound==10 && draws[0].tick==4,
          "mobile COB random shares gameplay RNG without a factory script");
    w.unit(id)->hp=Fixed();w.tick(1.0f/30.0f);
    check(draws.size()==1,"dead mobile script stops consuming random state");
}

static void mobileProducerFacesSite() {
    std::printf("mobile conjurer faces its production site:\n");
    for (bool flying : {false,true}) {
        auto ownedWorld = makeWorld(128,128);
        World& w = *ownedWorld;
        UnitType conjurer=soldierType(), output=soldierType();
        conjurer.isBuilder=true;conjurer.canFly=flying;conjurer.workerTime=1;
        conjurer.turnInPlaceRate=1000;conjurer.turnRate=400;conjurer.buildDist=100;
        output.buildTime=10000;
        const int id=w.spawn(&conjurer,1000,1000,1.57079632679f,0);
        w.train(id,&output,1);
        run(w,3.0f);
        const auto* producer=w.unit(id);
        const auto* site=w.unit(producer->productionSiteId);
        check(site && site->underConstruction,"conjure site remains active during facing check");
        if (site) {
            const auto want=retailHeadingToPort(uint16_t(retailDirection(producer->x-site->x,producer->z-site->z).v));
            check(flying ? std::abs(bamDiff(want,producer->heading))<8192 : bamDiff(want,producer->heading)==0,flying ? "flying conjurer faces queued output" : "ground conjurer faces queued output",
                  "heading error="+std::to_string(bamDiff(want,producer->heading)));
        }
    }
}

static void repairParticles() {
    World w;setTestPathfinding(w);
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    UnitType builder;builder.id="repair-worker";builder.maxHp=100;
    builder.isBuilder=builder.canMove=true;builder.buildDist=200;builder.workerTime=10;
    builder.modelTop=32*65536;
    UnitType target;target.id="repair-target";target.maxHp=1000;
    target.buildTime=100;target.buildCost=100;target.modelTop=32*65536;
    // The worker touches the target: RepairUnit works only once the
    // footprints touch (its rectangle approach arrives at once here).
    const int bid=w.spawn(&builder,234,200),tid=w.spawn(&target,250,200);
    w.unit(tid)->hp=Fixed::fromInt(100);w.player(0).mana=0;
    w.repair(bid,tid,false);
    for(int i=0;i<10;++i)w.tick(1.f/30);
    check(w.unit(bid)->constructionEmissions[0]==0 &&
          w.unit(tid)->constructionEmissions[1]==0,
          "unfunded repair emits no worker or target particles");
    w.player(0).mana=1000;
    for(int i=0;i<10;++i)w.tick(1.f/30);
    const auto* b=w.unit(bid);const auto* t=w.unit(tid);
    check(t->hp>Fixed::fromInt(100) && b->constructionEmissions[0]>0 &&
          b->constructionEmissions[0]==t->constructionEmissions[1] &&
          b->cosmeticConstructionEmitter && t->cosmeticConstructionEmitter &&
          !b->cosmeticConstructionEmitter->particles.empty() &&
          !t->cosmeticConstructionEmitter->particles.empty(),
          "successful repair emits particles at both worker and target");
    const auto emitted=b->constructionEmissions[0];
    w.unit(tid)->hp=Fixed::fromInt(1000);
    for(int i=0;i<180;++i)w.tick(1.f/30);
    check(b->constructionEmissions[0]==emitted &&
          t->constructionEmissions[1]==emitted &&
          (!b->cosmeticConstructionEmitter || b->cosmeticConstructionEmitter->particles.empty()) &&
          (!t->cosmeticConstructionEmitter || t->cosmeticConstructionEmitter->particles.empty()),
          "finished repair stops emission and lets existing particles drain");
}

// Construction sparkle sizing (retail 4ee377..4ee40e) and ceiling (4f1310). A
// building's ring sits on its nearest footprint edge, R = min(half-sides), and holds
// R sparkles; bmcode 1 divides that by 4. The FBI parser defaults bmcode to 0 and 49
// shipped buildings omit it, so a building with no bmcode line must take the
// building branch. The rising ceiling is the owner's world Y plus the model height.
static void constructionParticleSizing() {
    auto files=std::make_shared<tak::hpi::Vfs::Files>();
    auto fbi=[&](const char* id,const char* fields) {
        const std::string text=std::string("[UNITINFO]\n{\nunitname=")+id+";\nobjectname="+id+
            ";\nside=ARA;\nmaxdamage=1000;\nbuildtime=100;\nbuildcost=100;\n"+fields+"\n}\n";
        (*files)[std::string("units/")+id+".fbi"]={text.begin(),text.end()};
    };
    fbi("plainhall","footprintx=4;\nfootprintz=6;");
    tak::hpi::Vfs vfs;vfs.setMapFiles(files);
    TypeRegistry reg;reg.loadDir(vfs,"units");
    check(reg.find("plainhall") && reg.find("plainhall")->buildMovementCode==0,
          "a building with no bmcode line reads bmcode 0 (retail's default)");
    if (!reg.find("plainhall")) return;
    UnitType target=*reg.find("plainhall");target.modelTop=32*65536;
    World w;setTestPathfinding(w);
    std::vector<uint8_t> heights(64*64);
    for (int z=0;z<64;++z) for (int x=0;x<64;++x) heights[size_t(z)*64+x]=uint8_t(40+x*3);
    w.setTerrain(heights,64,64,20);
    UnitType builder;builder.id="sizing-worker";builder.maxHp=100;
    builder.isBuilder=builder.canMove=true;builder.buildDist=200;builder.workerTime=10;
    builder.modelTop=32*65536;
    const int bid=w.spawn(&builder,232,200),tid=w.spawn(&target,268,200);   // touching its west side
    w.unit(tid)->hp=Fixed::fromInt(100);w.player(0).mana=1000;
    w.repair(bid,tid,false);
    for(int i=0;i<20;++i)w.tick(1.f/30);
    const auto* t=w.unit(tid);
    const auto* e=t && t->cosmeticConstructionEmitter ? &*t->cosmeticConstructionEmitter : nullptr;
    check(e && e->radius==32*65536 && e->capacity==32,
          "a 4x6 building without bmcode: ring R = min half-side (32 px), capacity 32",
          e ? "radius="+std::to_string(e->radius/65536.0)+" capacity="+std::to_string(e->capacity) : "no emitter");
    bool ceilings=e && !e->particles.empty();
    if (e) for (const auto& p:e->particles)
        ceilings&=p.speed<0 || p.ceiling==int32_t(uint32_t(t->groundY.v)+uint32_t(e->height));
    check(ceilings,"rising sparkles stop at the owner's world Y plus the model height");
}

// Structures must never reach navigation steering at the edge of weapon range.
static void defensiveBodyStaysFixed() {
    for (bool canMove : {false, true}) for (float distance : {200.f,490.f,700.f}) {
        auto ownedWorld = makeWorld(128,128);
        World& w = *ownedWorld;
        UnitType tower=factoryType(),enemy=soldierType();
        tower.isBuilder=false;tower.canMove=canMove;tower.turnRate=500;
        tower.sight=1000;
        Weapon weapon;weapon.range=500;weapon.damage=1;weapon.reload=1;
        tower.weapon=weapon;tower.weapons.push_back(weapon);
        enemy.maxHp=10000;
        w.setPlayerCount(2);
        const int id=w.spawn(&tower,1000,1000,0,0);
        const int target=w.spawn(&enemy,1000+distance,1000,0,1);
        const auto heading=w.unit(id)->heading;
        w.attack(id,target,false);
        bool fixed=true;
        for(int tick=0;tick<90;++tick) {
            w.tick(1.f/30);
            const auto* u=w.unit(id);
            fixed &= u->heading==heading && u->x==Fixed::fromInt(1000) &&
                     u->z==Fixed::fromInt(1000);
        }
        check(fixed,"defensive body stays fixed at every attack distance",
              std::to_string(distance)+(canMove ? " canmove" : " immobile"));
    }
}

// ---------------------------------------------------------------------------
// 5. A rally starts at once: a producer's output takes its first rally step as
//    its first order, with no walk to an exit spot ahead of it. A mobile
//    producer's output used to step south first (reported on a repeating Zhon
//    builder); a factory's never did, and the factory run pins that. The rally
//    lies north-east, so the old step shows as a walk the wrong way.
// ---------------------------------------------------------------------------
enum class RallyKind { Move, Fight, Patrol, Attack, Chain };
constexpr float kRallyX = 1500, kRallyZ = 200;

static std::pair<float, float> legPoint(const Order& leg) {
    const auto point = leg.missionTarget.value_or(std::pair{leg.x, leg.z});
    return {point.first.toFloat(), point.second.toFloat()};
}

static bool legAimsAt(const Order& leg, float x, float z) {
    const auto [lx, lz] = legPoint(leg);
    return std::abs(lx - x) <= 16 && std::abs(lz - z) <= 16;   // a footprint cell's snap
}

static float rallyDistance(const Unit& u) {
    return std::hypot(u.x.toFloat() - kRallyX, u.z.toFloat() - kRallyZ);
}

static void rallyStartsAtOnce(bool mobile, RallyKind kind) {
    const char* name = kind == RallyKind::Move ? "move" : kind == RallyKind::Fight ? "fight-move" :
                       kind == RallyKind::Patrol ? "patrol" : kind == RallyKind::Attack ? "live attack" : "Shift-queued";
    std::printf("%s: a %s rally starts at once:\n", mobile ? "mobile producer" : "factory", name);
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
    UnitType prod = producerType(mobile), sol = armedSoldierType(), dummy = soldierType();
    dummy.maxVel = Fixed(); dummy.id = "dummy";
    const int id = w.spawn(&prod, 600, 600, 0, 0);
    const int enemy = kind == RallyKind::Attack ? w.spawn(&dummy, kRallyX, kRallyZ, 0, 1) : 0;
    w.player(0).mana = 1e9f;
    w.setRepeat(id, &sol);
    if (kind == RallyKind::Move) w.order(id, kRallyX, kRallyZ, false);
    else if (kind == RallyKind::Fight) w.attackMove(id, kRallyX, kRallyZ, false);
    else if (kind == RallyKind::Patrol) w.patrol(id, kRallyX, kRallyZ);
    else if (kind == RallyKind::Attack) w.attack(id, enemy, false);
    else { w.order(id, kRallyX, kRallyZ, false); w.attackMove(id, 1500, 800, true); w.queuePatrol(id, 1200, 800); }
    const int child = firstOutput(w, id, 900);
    check(child != 0, "an output is produced");
    if (!child) return;
    const Unit& u = *w.unit(child);
    check(!u.orders.empty(), "it holds the rally when finished");
    if (u.orders.empty()) return;
    check(std::none_of(u.orders.begin(), u.orders.end(), [](const Order& o) { return bool(o.productionExit); }),
          "no leg is an automatic exit");
    const Order& leg = u.orders[World::currentLeg(u.orders)];
    const auto [lx, lz] = legPoint(leg);
    if (kind == RallyKind::Attack) {
        check(leg.targetId == enemy, "its first order is the attack", std::to_string(leg.targetId));
    } else {
        check(legAimsAt(leg, kRallyX, kRallyZ), "its first order is the rally point",
              std::to_string(lx) + "," + std::to_string(lz));
        const bool fight = kind == RallyKind::Fight || kind == RallyKind::Patrol;
        check(leg.attackMove == fight && leg.patrol == (kind == RallyKind::Patrol),
              "the first leg carries the rally's kind");
    }
    if (kind == RallyKind::Chain) {
        check(u.orders.size() == 3 && u.orders[1].attackMove && !u.orders[1].patrol && u.orders[2].patrol,
              "the rest of the plan queues behind it", std::to_string(u.orders.size()));
    }
    const float birthZ = u.z.toFloat(), birthDistance = rallyDistance(u);
    for (int n = 0; n < 60; ++n) w.tick(1.0f / 30.0f);
    const Unit& later = *w.unit(child);
    // The route round the producer's corner may dip one cell; the old step was 60+ px.
    check(later.z.toFloat() <= birthZ + 24, "it does not walk south first",
          std::to_string(birthZ) + " -> " + std::to_string(later.z.toFloat()));
    check(rallyDistance(later) < birthDistance, "two seconds on it is nearer the rally",
          std::to_string(birthDistance) + " -> " + std::to_string(rallyDistance(later)));
}

// An attack step on a dead target is skipped: the output starts the next step
// that survives, still with nothing ahead of it.
static void rallyPartlyDropped(bool mobile) {
    std::printf("%s: dead attack step, then a move:\n", mobile ? "mobile producer" : "factory");
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
    UnitType prod = producerType(mobile), sol = armedSoldierType(), dummy = soldierType();
    dummy.maxVel = Fixed(); dummy.id = "dummy";
    const int id = w.spawn(&prod, 600, 600, 0, 0);
    const int enemy = w.spawn(&dummy, 1400, 1200, 0, 1);
    w.player(0).mana = 1e9f;
    w.setRepeat(id, &sol);
    w.attack(id, enemy, false);
    w.order(id, kRallyX, kRallyZ, true);
    w.unit(enemy)->hp = Fixed(); w.unit(enemy)->deadFor = 0;
    const int child = firstOutput(w, id, 900);
    check(child != 0, "an output is produced");
    if (!child) return;
    const Unit& u = *w.unit(child);
    check(u.orders.size() == 1 && !u.orders.front().productionExit && legAimsAt(u.orders.front(), kRallyX, kRallyZ),
          "it starts the step that survives", std::to_string(u.orders.size()));
}

// A rally that hands over nothing -- every step dropped -- is no rally: a
// repeating mobile producer's output parks round it, or it would stand where
// the next one is built. Deciding on the rally being empty would leave it there. Passes
// without the fix too: this is the naive fix's guard.
static void rallyHandsOverNothing(bool deadTarget) {
    std::printf("mobile producer: rally that hands over nothing (%s):\n", deadTarget ? "dead target" : "unarmed output");
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
    UnitType prod = producerType(true), sol = deadTarget ? armedSoldierType() : soldierType(), dummy = soldierType();
    dummy.maxVel = Fixed(); dummy.id = "dummy";
    const int id = w.spawn(&prod, 600, 600, 0, 0);
    const int enemy = w.spawn(&dummy, 1400, 1200, 0, 1);
    w.player(0).mana = 1e9f;
    w.setRepeat(id, &sol);
    w.attack(id, enemy, false);
    if (deadTarget) { w.unit(enemy)->hp = Fixed(); w.unit(enemy)->deadFor = 0; }
    check(w.unit(id)->rally.size() == 1, "the rally holds the attack step");
    const int child = firstOutput(w, id, 900);
    check(child != 0, "an output is produced");
    if (!child) return;
    const Unit& u = *w.unit(child);
    check(u.orders.size() == 1, "it holds one order", std::to_string(u.orders.size()));
    if (u.orders.empty()) return;
    // A repeating mobile producer's output parks on the factory ring round it
    // (engine deviation), as a factory's does, rather than the exit step.
    check(u.orders.front().park && u.orders.front().park->target == id, "the order parks it round its producer");
}

// An attack step the output has no weapon for (here a ground-only weapon against a
// flyer in the air) goes on the output's first tick. It must not be handed over:
// the output would be left with nothing on the spot it was made, and the steps
// behind it must start at once. The exit step, or PARK, stands in for a plan left
// empty.
static void rallyStepItCannotDo(bool mobile, bool moveBehind) {
    std::printf("%s: attack rally on a target out of its weapon's layer%s:\n",
                mobile ? "mobile producer" : "factory", moveBehind ? ", then a move" : "");
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
    UnitType prod = producerType(mobile), sol = armedSoldierType(), flyer = soldierType();
    sol.weapons.front().noAir = true; sol.weapon.noAir = true;
    flyer.id = "flyer"; flyer.canFly = true;
    const int id = w.spawn(&prod, 600, 600, 0, 0);
    const int enemy = w.spawn(&flyer, 1400, 1200, 0, 1);
    w.player(0).mana = 1e9f;
    w.setRepeat(id, &sol);
    w.attack(id, enemy, false);
    if (moveBehind) w.order(id, kRallyX, kRallyZ, true);
    w.unit(enemy)->flightGroundMode = 2;
    const int child = firstOutput(w, id, 900);
    check(child != 0, "an output is produced");
    if (!child) return;
    check(w.unit(enemy)->flightGroundMode == 2, "the target is still in the air");
    const Unit& u = *w.unit(child);
    check(u.orders.size() == 1, "it holds one order", std::to_string(u.orders.size()));
    if (u.orders.empty()) return;
    if (moveBehind) check(legAimsAt(u.orders.front(), kRallyX, kRallyZ), "it starts the move at once");
    else if (mobile) check(legPoint(u.orders.front()).second >= u.z.toFloat() + 40, "it takes the exit step");
    else check(u.orders.front().park.has_value(), "it parks");
}

// PARK's stage 1 (407d02..407d55) ends the park for an order behind it only when
// the output is selectable: one a mission script has taken (the parse cleared
// +0x130 & 0x20) parks on until its tries run out.
static void parkYieldsOnlyWhenSelectable() {
    std::printf("factory: a parked output and an order behind it:\n");
    for (int scripted = 0; scripted < 2; ++scripted) {
        auto ownedWorld = makeWorld(160, 160);
        World& w = *ownedWorld;
        UnitType prod = producerType(false), sol = soldierType();
        const int id = w.spawn(&prod, 600, 600, 0, 0);
        w.player(0).mana = 1e9f;
        w.setRepeat(id, &sol);
        const int child = firstOutput(w, id, 900);
        check(child != 0, "an output is produced");
        if (!child) return;
        Unit* u = w.unit(child);
        if (u->orders.empty() || !u->orders.front().park) { check(false, "it parks"); return; }
        u->selectable = scripted == 0;
        w.order(child, kRallyX, kRallyZ, true);
        // The park's next wake: its route failed (0x200, in its wait mask 0x2708).
        u->orders.front().mission.pending |= 0x200;
        for (int n = 0; n < 2; ++n) w.tick(1.0f / 30.0f);
        u = w.unit(child);
        const bool parking = !u->orders.empty() && u->orders.front().park.has_value();
        check(scripted ? parking : !parking,
              scripted ? "unselectable, it is still parking (407d1e)" : "selectable, the park gives way to the order");
    }
}

// A structure holds its orders as its own rally, so a mobile producer's rally lands
// there and its orders stay empty. That is a handed-over plan, not an empty one:
// the exit step would replace it. With no rally the exit step is all it gets.
static void rallyOnAStructureOutput(bool withRally) {
    std::printf("mobile producer: structure output, %s:\n", withRally ? "a rally" : "no rally");
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
    UnitType prod = producerType(true), keep = factoryType();
    keep.id = "keep"; keep.buildTime = 1;
    const int id = w.spawn(&prod, 600, 600, 0, 0);
    w.player(0).mana = 1e9f;
    w.setRepeat(id, &keep);
    if (withRally) { w.order(id, 1500, 1500, false); w.order(id, 1700, 1500, true); }
    int child = 0;
    for (int n = 0; n < 900 && !child; ++n) {
        w.tick(1.0f / 30.0f);
        for (const auto& u : w.units())
            if (u.id != id && u.alive() && !u.underConstruction) child = u.id;
    }
    check(child != 0, "the structure is produced");
    if (!child) return;
    const Unit& u = *w.unit(child);
    check(u.orders.empty(), "it holds no orders", std::to_string(u.orders.size()));
    if (withRally) {
        check(u.rally.size() == 2 && u.rally[0].x == Fixed::fromInt(1500) && u.rally[1].x == Fixed::fromInt(1700),
              "it keeps the player's plan, with nothing in front of it", std::to_string(u.rally.size()));
    } else {
        check(u.rally.size() == 1 && u.rally[0].z > u.z, "it keeps the exit step", std::to_string(u.rally.size()));
    }
}

// How many outputs a repeating mobile producer makes in `seconds`, one every
// `paceTicks`, all with nowhere to go: the rally is `rally` (or none).
struct RoomRun { int made = 0, moved = 0; uint32_t searched = 0; uint64_t hash = 0; };
static RoomRun runOutOfRoom(int foot, bool rally, float rx, float rz, int seconds, UnitType* extra = nullptr) {
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
    UnitType prod = producerType(true), big = soldierType();
    prod.workerTime = 100; prod.storage = 10000000;
    big.id = "big"; big.footX = big.footZ = foot; big.buildTime = 30;
    const int id = w.spawn(&prod, 1200, 1200, 0, 0);
    const int bystander = extra ? w.spawn(extra, 400, 400, 0, 0) : 0;
    w.player(0).mana = 1e9f;
    w.setRepeat(id, &big);
    if (rally) w.order(id, rx, rz, false);
    RoomRun r;
    for (int n = 0; n < 30 * seconds; ++n) {
        w.tick(1.0f / 30.0f);
        r.searched |= w.unit(id)->productionRetryTick;
    }
    for (const auto& u : w.units())
        if (u.id != id && u.id != bystander && u.alive() && !u.underConstruction) ++r.made;
    if (bystander) r.moved = w.unit(bystander)->x != Fixed::fromInt(400) || w.unit(bystander)->z != Fixed::fromInt(400);
    r.hash = w.stateHash();
    return r;
}

// The area the next output may be made in is 12 cells round the point south of a
// mobile producer. With the exit step gone, an output whose rally ends in it stands
// in it. The idle ones must move, or the producer stops for good, repeat on and no
// error: 28-52 outputs here without them (61-67 Retail, 41 Legion with the exit step
// that used to clear the way).
static void rallyLeavesRoomForTheNext() {
    std::printf("mobile producer: a rally that ends beside the producer does not stop production:\n");
    constexpr int kMade = 70;
    for (const auto& [rx, rz, where] : {std::tuple{1200.f, 1300.f, "it outbuilds the area, rally on the birth spot"},
                                        std::tuple{1264.f, 1364.f, "it outbuilds the area, rally 64 px away"}}) {
        const RoomRun r = runOutOfRoom(4, true, rx, rz, 240);
        check(r.made >= kMade, where, std::to_string(r.made));
        check(r.searched != 0, "...by the room search");
    }
    // The choice is the same on every peer: two runs, one hash.
    check(runOutOfRoom(4, true, 1200, 1300, 120).hash == runOutOfRoom(4, true, 1200, 1300, 120).hash,
          "the same run gives the same hash");
    // A unit well outside the area is not moved for it.
    UnitType bystander = soldierType();
    bystander.id = "bystander";
    check(!runOutOfRoom(4, true, 1200, 1300, 120, &bystander).moved, "a unit far from the area stays where it is");
    // With no rally the search never runs, however full the area: as before.
    check(!runOutOfRoom(4, false, 0, 0, 240).searched, "a producer with no rally never searches");
}

// A scripted mobile producer has one pad, and an output with a rally on it stands
// on it. The pad is cleared as a build site is, or the first output is the last.
static void rallyOnAScriptedPad() {
    std::printf("mobile producer with a pad: a rally on the pad does not stop production:\n");
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
    UnitType prod = producerType(true), sol = soldierType();
    auto script = std::make_shared<tak::cob::File>();
    script->pieces = {"emit", "root"};
    script->scripts = {{"Create", 0}, {"QueryBuildInfo", 19}};   // as scriptControlsReadinessAndPosition's
    script->code = {0x10021001, 5, 0x10021001, 0, 0x10082000,
                    0x10021001, 100, 0x10013000,
                    0x10021001, 18, 0x10021001, 1, 0x10082000,
                    0x10021001, 5, 0x10021001, 1, 0x10082000, 0x10065000,
                    0x10021001, 0, 0x10023002, 0, 0x10065000};
    prod.productionScript = script;
    prod.productionModel = {{{0, 0, 0}, -1, 1}, {{0, 0, 128 * 65536}, 0, 0}};
    prod.workerTime = 100; prod.storage = 10000000; sol.buildTime = 30;
    const int id = w.spawn(&prod, 600, 600, 3.14159265f, 0);
    w.player(0).mana = 1e9f;
    w.setRepeat(id, &sol);
    w.order(id, 600, 728, false);
    run(w, 20.0f);
    int made = 0;
    for (const auto& u : w.units()) made += u.id != id && u.alive() && !u.underConstruction;
    check(made >= 5, "it keeps producing", std::to_string(made));
}

// A rally point on the birth spot or inside the mobile producer's own rectangle:
// nothing walks the outputs clear first any more, so they must clear each other
// by the site search alone, and every one must be built and come to rest.
static void rallyBesideThePad(float x, float z, const char* where) {
    std::printf("mobile producer: rally %s:\n", where);
    auto ownedWorld = makeWorld(160, 160);
    World& w = *ownedWorld;
    UnitType prod = producerType(true), sol = soldierType();
    const int id = w.spawn(&prod, 600, 600, 0, 0);
    w.player(0).mana = 1e9f;
    w.setRepeat(id, &sol);
    w.order(id, x, z, false);
    constexpr int kOutputs = 8;
    std::vector<int> outputs;
    bool stopped = false;
    const auto finished = [&] {
        outputs.clear();
        for (const auto& u : w.units())
            if (u.id != id && u.alive() && !u.underConstruction) outputs.push_back(u.id);
    };
    const auto rest = [&] {
        return stopped && std::all_of(outputs.begin(), outputs.end(), [&](int o) { return w.unit(o)->orders.empty(); });
    };
    int tick = 0;
    for (; tick < 30 * 300 && !rest(); ++tick) {
        w.tick(1.0f / 30.0f);
        finished();
        if (!stopped && int(outputs.size()) >= kOutputs) { w.stop(id); stopped = true; }
    }
    finished();
    check(stopped && int(outputs.size()) >= kOutputs, "all eight outputs are built", std::to_string(outputs.size()));
    check(rest(), "all of them stop", "tick " + std::to_string(tick));
    int overlaps = 0, onProducer = 0;
    const Unit& p = *w.unit(id);
    const int px = footprintOrigin(p.x, prod.footX), pz = footprintOrigin(p.z, prod.footZ);
    for (size_t a = 0; a < outputs.size(); ++a) {
        const Unit& u = *w.unit(outputs[a]);
        const int ux = footprintOrigin(u.x, sol.footX), uz = footprintOrigin(u.z, sol.footZ);
        onProducer += ux < px + prod.footX && ux + sol.footX > px && uz < pz + prod.footZ && uz + sol.footZ > pz;
        for (size_t b = a + 1; b < outputs.size(); ++b) {
            const Unit& v = *w.unit(outputs[b]);
            const int vx = footprintOrigin(v.x, sol.footX), vz = footprintOrigin(v.z, sol.footZ);
            overlaps += std::abs(ux - vx) < sol.footX && std::abs(uz - vz) < sol.footZ;
        }
    }
    check(overlaps == 0, "no two outputs rest on each other", std::to_string(overlaps));
    check(onProducer == 0, "none rests on the producer", std::to_string(onProducer));
}

// A FINISHED OUTPUT JOINS ITS MAKER'S GROUP, for human owners only. Retail's
// GetBuilt (40238a) gives a factory's output the factory's group as it
// releases it -- even none, replacing a group the unfinished unit was put in
// -- when the owner is human (+0xea 1); a computer player's AI assigns its
// units itself. Ours used to join at construction start, for every owner.
// A unit a mobile builder (or a repeating mobile producer) makes joins the
// builder's group when it is finished (ours: retail's MobileBuild has no
// join). Both pathfinding modes.
static void outputJoinsGroup() {
    std::printf("a finished output joins its maker's group (%s):\n",
                g_pathfinding == PathfindingMode::Retail ? "Retail" : "Legion");
    const auto output = [](const World& w, int maker, bool finished) {
        for (const auto& u : w.units())
            if (u.id != maker && u.alive() && u.type && !u.type->isStructure() &&
                u.underConstruction != finished) return u.id;
        return 0;
    };
    for (bool computer : {false, true}) {
        const char* who = computer ? "computer" : "human";
        char what[160];
        // A factory in formation 2: nothing while it is built, the formation at release.
        {
            auto ownedWorld = makeWorld(128, 128);
            World& w = *ownedWorld;
            w.player(0).automaticGates = computer;
            UnitType fac = factoryType(), sol = soldierType();
            fac.workerTime = 20; sol.buildTime = 60;
            const int fid = w.spawn(&fac, 1000, 1000, 0, 0);
            w.setSquad(fid, -2);
            w.player(0).mana = 1e9f;
            w.train(fid, &sol, 1);
            int site = 0, siteSquad = -99;
            for (int t = 0; t < 900 && !output(w, fid, true); ++t) {
                w.tick(1.0f / 30.0f);
                if (!site) site = output(w, fid, false);
                if (site && w.unit(site)->underConstruction) siteSquad = w.unit(site)->squad;
            }
            const int done = output(w, fid, true);
            std::snprintf(what, sizeof what, "%s: a factory's unfinished output is in no group", who);
            check(site && siteSquad == 0, what, std::to_string(siteSquad));
            std::snprintf(what, sizeof what, computer ? "%s: its released output is in no group"
                                                      : "%s: its released output joins formation 2", who);
            check(done && w.unit(done)->squad == (computer ? 0 : -2), what,
                  done ? std::to_string(w.unit(done)->squad) : "none");
        }
        // An ungrouped factory: an unfinished output put in group 3 leaves it at release.
        if (!computer) {
            auto ownedWorld = makeWorld(128, 128);
            World& w = *ownedWorld;
            UnitType fac = factoryType(), sol = soldierType();
            fac.workerTime = 20; sol.buildTime = 60;
            const int fid = w.spawn(&fac, 1000, 1000, 0, 0);
            w.player(0).mana = 1e9f;
            w.train(fid, &sol, 1);
            for (int t = 0; t < 900 && !output(w, fid, true); ++t) {
                if (const int site = output(w, fid, false)) w.setSquad(site, 3);
                w.tick(1.0f / 30.0f);
            }
            const int done = output(w, fid, true);
            check(done && w.unit(done)->squad == 0,
                  "human: an ungrouped factory's output leaves the group it was put in while built",
                  done ? std::to_string(w.unit(done)->squad) : "none");
        }
        // A mobile builder in group 1 conjuring a unit: joins when finished.
        {
            auto ownedWorld = makeWorld(128, 128);
            World& w = *ownedWorld;
            w.player(0).automaticGates = computer;
            UnitType builder = soldierType(), sol = soldierType();
            builder.id = builder.name = "builder"; builder.isBuilder = true; builder.workerTime = 20;
            sol.buildTime = 60;
            const int bid = w.spawn(&builder, 1000, 1000, 0, 0);
            w.setSquad(bid, 1);
            w.player(0).mana = 1e9f;
            w.queueBuild(bid, &sol, 1060, 1000, false);
            int site = 0, siteSquad = -99;
            for (int t = 0; t < 1500 && !output(w, bid, true); ++t) {
                w.tick(1.0f / 30.0f);
                if (!site) site = output(w, bid, false);
                if (site && w.unit(site) && w.unit(site)->underConstruction) siteSquad = w.unit(site)->squad;
            }
            const int done = output(w, bid, true);
            std::snprintf(what, sizeof what, "%s: a builder's unfinished conjure is in no group", who);
            check(site && siteSquad == 0, what, std::to_string(siteSquad));
            std::snprintf(what, sizeof what, computer ? "%s: the finished unit is in no group"
                                                      : "%s: the finished unit joins the builder's group 1", who);
            check(done && w.unit(done)->squad == (computer ? 0 : 1), what,
                  done ? std::to_string(w.unit(done)->squad) : "none");
        }
        // A repeating mobile producer in formation 4.
        {
            auto ownedWorld = makeWorld(160, 160);
            World& w = *ownedWorld;
            w.player(0).automaticGates = computer;
            UnitType builder = factoryType(), sol = soldierType();
            builder.maxVel = sol.maxVel; builder.canMove = true;
            const int id = w.spawn(&builder, 600, 600, 0, 0);
            w.setSquad(id, -4);
            w.player(0).mana = 1e9f;
            w.setRepeat(id, &sol);
            for (int t = 0; t < 900 && !output(w, id, true); ++t) w.tick(1.0f / 30.0f);
            const int done = output(w, id, true);
            std::snprintf(what, sizeof what, computer ? "%s: a mobile producer's output is in no group"
                                                      : "%s: a mobile producer's output joins its formation 4", who);
            check(done && w.unit(done)->squad == (computer ? 0 : -4), what,
                  done ? std::to_string(w.unit(done)->squad) : "none");
        }
    }
}

int main(int argc,char** argv) {
    if(argc==2 && !std::strcmp(argv[1],"--legion"))g_pathfinding=PathfindingMode::Legion;
    else if(argc!=1)return 2;
    std::printf("production_test\n");
    {
        World world;setTestPathfinding(world);world.setPlayerCount(2);
        UnitType limited=soldierType();limited.totalAllowed=1;
        UnitType ordinary=soldierType();ordinary.id="ordinary";
        const int existing=world.spawn(&limited,100,100,0,0);
        world.unit(existing)->underConstruction=true;
        world.setBenchmarkPlan({{1,{{&limited,200,100,0},{&limited,300,100,1},
                                    {&limited,400,100,1},{&ordinary,500,100,0}}},
                                {2,{{&limited,200,200,0},{&limited,300,200,0}}}});
        world.benchmarkClock(1,false);   // the server's clock: the first stage is due
        world.tick(1.0f/30);
        auto count=[&](int player,const UnitType* type) {
            int n=0;for (const auto& u:world.units())
                n+=u.alive() && u.player==player && u.type==type;
            return n;
        };
        check(count(0,&limited)==1 && count(1,&limited)==1 && count(0,&ordinary)==1,
              "benchmark caps are per player, count construction, and preserve ordinary spawns");
        world.unit(existing)->deadFor=0;
        world.benchmarkClock(2,false);
        world.tick(1.0f/30);
        check(count(0,&limited)==1 && count(1,&limited)==1,
              "benchmark can replace a dead limited unit without exceeding its cap");
    }
    defensiveBodyStaysFixed();
    repairParticles();
    constructionParticleSizing();
    mobileProducerFacesSite();
    productionNeedsMana();
    shortIncomeIsShared();
    hudShowsRequestedMana();
    cloakSpendsUnitMana();
    reviveCostsNothing();
    abandonedReviveWaits();
    reviveRespectsUnitLimit();
    reviveWorkWrapsLikeFtol();
    workAndShotsBlockCloak();
    cloakStepFollowsRetail();
    aiShortfallReadsRequests();
    lodestoneUpgradeFinishesUnderShortShare();
    outputExistsDuringConstruction();
    scriptControlsReadinessAndPosition();
    mobileScriptRandomRunsInWorld();
    outputDoesNotJam();
    rallyIsAdopted();
    outputJoinsGroup();
    rallyReplaces();
    mobileRallyIsAdopted();
    mobileRallyEndsWithRepeat();
    for (bool mobile : {false, true}) {
        rallyPlanIsFollowed(mobile);
        rallyAttackThenMove(mobile);
        rallyQueueStopAndHash(mobile);
        for (RallyKind kind : {RallyKind::Move, RallyKind::Fight, RallyKind::Patrol, RallyKind::Attack, RallyKind::Chain})
            rallyStartsAtOnce(mobile, kind);
        rallyPartlyDropped(mobile);
        for (bool moveBehind : {false, true}) rallyStepItCannotDo(mobile, moveBehind);
    }
    for (bool deadTarget : {true, false}) rallyHandsOverNothing(deadTarget);
    for (bool withRally : {true, false}) rallyOnAStructureOutput(withRally);
    parkYieldsOnlyWhenSelectable();
    rallyBesideThePad(600, 716, "on the birth spot");
    rallyBesideThePad(600, 600, "inside the producer's rectangle");
    rallyLeavesRoomForTheNext();
    rallyOnAScriptedPad();
    if (g_pathfinding == PathfindingMode::Legion) {
        std::printf("Legion routed:");
        for (size_t k = 0; k < 16; ++k) if (g_legionSaw[k]) std::printf(" %zu", k);
        std::printf("\n");
        // A factory releases its output with GetBuilt's PARK, not the exit step.
        check(g_legionSaw[size_t(LegionMission::Park)], "Legion routes a factory output's PARK release");
        check(g_legionSaw[size_t(LegionMission::Move)], "Legion routes the produced units' rally moves");
    }
    std::printf(g_fail ? "production_test: %d FAILURE(S)\n" : "production_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
