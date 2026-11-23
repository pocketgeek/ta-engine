// helpbuild_test -- retail HelpBuild: shared production progress, assist
// eligibility, a guarding builder that helps (protocol 301), and a guard that
// goes after whoever attacks what it guards (protocol 302).
//
// Retail (KINGDOMS.icd, emulated) keeps ONE remaining fraction per unfinished
// unit (+0x108). Every worker -- the factory or conjurer making it and any
// builder helping -- takes its workertime-scaled work off it and pays mana for
// exactly that work (0x429af0), so helpers finish a factory's output sooner,
// the producer is free from that moment, and the total stays the buildcost.
// Ours kept the producer's own tick counter beside the helper's hp growth: an
// assisted output was marked finished early while the producer held its slot,
// paid the full cost again on top of the helper, kept adding hp to the
// finished unit, and on its own completion re-issued the output orders over
// whatever the player had told the unit since.
//
// HelpBuild eligibility (0x519e60): target alive and unfinished, not the
// helper, SAME owner (an ally's site does not qualify), helper a builder with a
// mover, builderlimited helpers need the type on their build menu. No repeat,
// rally or busy test. The sim enforces it, so a crafted command cannot bypass
// the client.
//
// Guard by a mobile builder (Follow_Ground 0x407f90, stage 1): repair the
// guarded unit when it is damaged, help it when it is unfinished, else help /
// repair what it is working on; the job goes ahead of the guard, which resumes
// after it; a look that finds nothing sleeps 15 + rand(15) ticks.
//
// Guard by a flyer (VTOL_Follow 0x418860): the same look for a builder, and
// for every flyer an idle orbit around the guarded unit at 320 px (weapon
// range + 160 when armed), a point controller accepting at 128 px whose angle
// steps back by 0x4000 + rand(0x2000) on each arrival.
//
// The guarded unit's attacker (Follow_Ground stage 1 / VTOL_Follow stage 2,
// first in the look): the hit response 409a50 sends the guarded unit's
// listeners event 0x10; a guard's look woken by it takes the attacker from
// guarded +0xf8 and, when it is an enemy the guard can attack and not of its
// notargetcategory, puts an attack on it ahead of the guard (4d7de0 explicit:
// past Hold Fire / Hold Position, no leash). Ours ignored it, so an attacker
// out of the guard's acquisition range shot what it guarded unanswered.
//
// The ground approach (HelpBuild 0x405cf0 and RepairUnit 0x407190, emulated
// 2026-10-30): both install the rectangle controller round the target's
// footprint, so a helper or repairer walks until its footprint touches the
// target's, from the side it comes from; a failed approach still works from
// within builddistance (retailBuildWithinReach), else a help ends; no
// distance is measured once the work has started. HelpBuild has no timer:
// ours gave up after 8 s without progress. A repair never works on a
// moving target: each look while it moves counts 1 (3 within reach), work
// interrupted by a move counts 3, and past 10 the repair ends; a patrol's
// detour ends once the builder itself is its sightdistance from where it
// left the patrol. Ours walked every helper to one point south of the site,
// worked from builddistance, healed walking units and leashed the target.
//
// Every scenario uses synthetic types (no game data) on a flat map.

#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "sim/retailreclaim.h"
#include "net/lockstep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
    UnitType factory, sol, builder, limited, conjurer, tower, flyer, flylimited, flyconj, flysol,
             ground, slow, slowfac;
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

        builder = sol;
        builder.name = builder.id = "builder";
        builder.isBuilder = true; builder.workerTime = 10; builder.buildDist = 60;
        builder.buildMenu = {"factory"};               // not limited: the menu is not consulted

        limited = builder;
        limited.name = limited.id = "limited";
        limited.builderLimited = true; limited.buildMenu = {"factory"};

        conjurer = builder;                             // a Zhon-style mobile producer
        conjurer.name = conjurer.id = "conjurer";
        conjurer.buildMenu = {"sol"};

        tower = factory;                                // a builder structure (never helps)
        tower.name = tower.id = "tower";

        flyer = builder;                                // a flying builder (arafly-like)
        flyer.name = flyer.id = "flyer";
        flyer.canFly = true; flyer.cruiseAlt = 80;
        flyer.maxVel = Fixed::fromInt(4); flyer.accel = flyer.brake = Fixed::fromInt(1);
        flyer.turnRate = flyer.turnInPlaceRate = 1200;
        flyer.buildDist = 100;

        flylimited = flyer;
        flylimited.name = flylimited.id = "flylimited";
        flylimited.builderLimited = true; flylimited.buildMenu = {"factory"};

        flyconj = flyer;                                // a flying producer (Thirsha-like)
        flyconj.name = flyconj.id = "flyconj";
        flyconj.buildMenu = {"sol"};

        ground = sol;                                   // stands on the terrain (groundY = its height)
        ground.name = ground.id = "ground"; ground.upright = true;

        slow = sol;                                     // 900 ticks of factory work
        slow.name = slow.id = "slow"; slow.buildTime = 300; slow.buildCost = 300;

        slowfac = factory;
        slowfac.name = slowfac.id = "slowfac"; slowfac.buildMenu = {"slow"};

        flysol = flyer;                                 // an unarmed flyer that is no builder
        flysol.name = flysol.id = "flysol";
        flysol.isBuilder = false; flysol.buildMenu.clear();
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

static const TypeRegistry& emptyRegistry() { static TypeRegistry reg; return reg; }

static void command(World& w, tak::net::Cmd kind, int unitId, int targetId, int player = 0) {
    tak::net::Command c;
    c.kind = kind; c.player = uint8_t(player); c.unitId = unitId; c.targetId = targetId;
    applyCommand(w, emptyRegistry(), c);
}

// ---------------------------------------------------------------------------
// One producer making one `sol`, optionally helped once its output exists.
// ---------------------------------------------------------------------------
struct ProductionRun {
    int producer = 0, helper = 0, site = 0;
    int siteTick = -1;        // the tick the output was spawned
    int finishTick = -1;      // the tick it stopped being unfinished
    bool freedAtFinish = false, helperFreedAtFinish = false;
    double manaAtSite = 0, manaSpent = 0;
    float hpAfterDamage = 0, hpLater = 0;
    size_t ordersAtFinish = 0;
    float finalGoalX = 0, finalGoalZ = 0;
    bool moved = false;
    uint64_t hash = 0;
};

static ProductionRun runProduction(bool mobileProducer, bool help, bool moveAfter, bool rally = false) {
    Types ty;
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    ProductionRun r;
    r.producer = mobileProducer ? w.spawn(&ty.conjurer, 1000, 1000, 0.0f, 0) : w.spawn(&ty.factory, 1000, 1000, 0.0f, 0);
    if (mobileProducer) w.spawn(&ty.factory, 300, 300, 0.0f, 0);   // storage for the pool
    if (help) r.helper = w.spawn(&ty.builder, 1000, mobileProducer ? 1110 : 1150, 0.0f, 0);
    if (rally) { tak::net::Command c; c.kind = tak::net::Cmd::Move; c.unitId = r.producer; c.x = 1500; c.z = 1500; applyCommand(w, emptyRegistry(), c); }
    w.train(r.producer, &ty.sol, 1);
    for (int tick = 0; tick < 1200; ++tick) {
        w.tick(1.0f / 30.0f);
        const Unit* p = w.unit(r.producer);
        if (!r.site && p->productionSiteId) {
            r.site = p->productionSiteId; r.siteTick = tick; r.manaAtSite = w.player(0).mana;
            // Measure from the first tick of work: the spawn tick already did one.
            r.manaAtSite += 600.0 / 180.0;
            if (help) w.assist(r.helper, r.site, false);
        }
        const Unit* s = r.site ? w.unit(r.site) : nullptr;
        if (s && r.finishTick < 0 && !s->underConstruction) {
            r.finishTick = tick;
            r.manaSpent = r.manaAtSite - w.player(0).mana;
            r.freedAtFinish = w.unit(r.producer)->productionSiteId != r.site;
            r.helperFreedAtFinish = !help || w.unit(r.helper)->buildSiteId == 0;
            // The producer may finish it after the helper's turn this tick;
            // the helper's job ends on its next look.
            if (help && !r.helperFreedAtFinish) {
                w.tick(1.0f / 30.0f);
                r.helperFreedAtFinish = w.unit(r.helper)->buildSiteId == 0;
            }
            r.ordersAtFinish = s->orders.size();
            if (moveAfter) {
                tak::net::Command c; c.kind = tak::net::Cmd::Move; c.unitId = r.site; c.x = 700; c.z = 1400;
                applyCommand(w, emptyRegistry(), c);
                r.moved = true;
            }
            // Damage it: nothing may add hit points to a finished unit (healtime 0).
            w.unit(r.site)->hp = Fixed::fromInt(100);
        }
        if (r.finishTick >= 0 && tick == r.finishTick + 5) r.hpAfterDamage = w.unit(r.site)->hp.toFloat();
        if (r.finishTick >= 0 && tick >= r.finishTick + 400) break;
    }
    if (const Unit* s = r.site ? w.unit(r.site) : nullptr) {
        r.hpLater = s->hp.toFloat();
        if (!s->orders.empty()) {
            const Order& last = s->orders[World::currentLeg(s->orders)];
            r.finalGoalX = last.x.toFloat(); r.finalGoalZ = last.z.toFloat();
        }
        r.moved = r.moved && std::fabs(s->x.toFloat() - 700) < 40 && std::fabs(s->z.toFloat() - 1400) < 40;
    }
    r.hash = w.stateHash();
    return r;
}

static void sharedProduction(bool mobile) {
    std::printf("%s output, helped vs alone:\n", mobile ? "conjurer (Zhon)" : "factory");
    const auto solo = runProduction(mobile, false, false);
    const auto helped = runProduction(mobile, true, false);
    char d[256];
    std::snprintf(d, sizeof d, "solo: site t=%d finish t=%d (%d ticks) mana %.4f hash %016llx", solo.siteTick, solo.finishTick,
                  solo.finishTick - solo.siteTick, solo.manaSpent, (unsigned long long)solo.hash);
    // 180 ticks of work: the spawn tick does the first one.
    check(solo.finishTick - solo.siteTick == 179, "alone, the output takes exactly its 180 ticks of work", d);
    check(std::fabs(solo.manaSpent - 600) < 0.01, "...and costs exactly its buildcost", d);
    check(solo.freedAtFinish, "...and the producer is freed as it finishes", d);
    std::snprintf(d, sizeof d, "helped: site t=%d finish t=%d (%d ticks) mana %.4f", helped.siteTick, helped.finishTick,
                  helped.finishTick - helped.siteTick, helped.manaSpent);
    check(helped.finishTick > 0 && helped.finishTick - helped.siteTick < 150,
          "an equal helper finishes the output well inside the producer's time", d);
    check(std::fabs(helped.manaSpent - 600) < 0.01, "...for its buildcost in total, not paid twice", d);
    check(helped.freedAtFinish, "...and the producer is freed at that very tick", d);
    check(helped.helperFreedAtFinish, "...as is the helper (by its next look)", d);
    std::snprintf(d, sizeof d, "hp 5 ticks after damage %.2f, 400 ticks after %.2f", helped.hpAfterDamage, helped.hpLater);
    check(std::fabs(helped.hpAfterDamage - 100) < 0.01 && std::fabs(helped.hpLater - 100) < 0.01,
          "no hit points are added to the finished unit", d);
}

static void outputOrdersOnce(bool mobile) {
    std::printf("%s output orders are given once:\n", mobile ? "conjurer" : "factory");
    const auto r = runProduction(mobile, true, true);
    char d[160];
    std::snprintf(d, sizeof d, "orders at finish %zu, final goal (%.0f,%.0f), reached=%d", r.ordersAtFinish,
                  r.finalGoalX, r.finalGoalZ, int(r.moved));
    check(r.ordersAtFinish >= 1, "the finished output gets its release order at completion", d);
    check(r.moved, "a Move the player gives it then is never overridden later", d);
    if (!mobile) {
        const auto rr = runProduction(false, true, false, true);
        std::snprintf(d, sizeof d, "orders at finish %zu, final goal (%.0f,%.0f)", rr.ordersAtFinish, rr.finalGoalX, rr.finalGoalZ);
        check(std::fabs(rr.finalGoalX - 1500) < 16 && std::fabs(rr.finalGoalZ - 1500) < 16,
              "a factory rally is handed over once and the output keeps it", d);
    }
}

// ---------------------------------------------------------------------------
// Eligibility (0x519e60), through applyCommand (what a client can send).
// ---------------------------------------------------------------------------
static void eligibility() {
    std::printf("HelpBuild eligibility (sim-enforced):\n");
    Types ty;
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    w.setTeam(1, 0);                                     // player 1 is player 0's ally
    const int fac = w.spawn(&ty.factory, 1000, 1000, 0.0f, 0);
    w.train(fac, &ty.sol, 5);
    const int allyFac = w.spawn(&ty.factory, 1400, 1000, 0.0f, 1);
    w.train(allyFac, &ty.sol, 5);
    const int enemyFac = w.spawn(&ty.factory, 600, 1000, 0.0f, 2);
    w.player(2).mana = 100000;
    w.train(enemyFac, &ty.sol, 5);
    for (int i = 0; i < 10; ++i) w.tick(1.0f / 30.0f);
    const int own = w.unit(fac)->productionSiteId, ally = w.unit(allyFac)->productionSiteId,
              enemy = w.unit(enemyFac)->productionSiteId;
    check(own && ally && enemy, "three factories have outputs under way");
    const int b = w.spawn(&ty.builder, 1000, 1200, 0.0f, 0);
    const int lim = w.spawn(&ty.limited, 1050, 1200, 0.0f, 0);
    const int sol = w.spawn(&ty.sol, 950, 1200, 0.0f, 0);
    const int tower = w.spawn(&ty.tower, 300, 300, 0.0f, 0);
    const int finished = w.spawn(&ty.sol, 900, 1200, 0.0f, 0);
    const auto accepted = [&](int helper, int target) {
        if (Unit* u = w.unit(helper)) u->buildSiteId = 0;
        command(w, tak::net::Cmd::Assist, helper, target);
        return w.unit(helper)->buildSiteId == target;
    };
    check(accepted(b, own), "a mobile builder helps its own factory's output");
    check(!accepted(b, ally), "...but not an ally's");
    check(!accepted(b, enemy), "...nor an enemy's");
    check(!accepted(sol, own), "a non-builder does not help");
    check(!accepted(tower, own), "a builder structure does not help");
    check(!accepted(b, finished), "a finished unit is not helped");
    check(!accepted(b, b), "a builder does not help itself");
    check(!accepted(lim, own), "a builderlimited builder needs the type on its menu");
    // A crafted client could call around the command checks; World::assist
    // applies the same rules.
    w.unit(lim)->buildSiteId = 0;
    w.assist(lim, own, false);
    check(w.unit(lim)->buildSiteId != own, "...and World::assist refuses it too");
    w.assist(b, ally, false);
    check(w.unit(b)->buildSiteId != ally, "World::assist refuses an ally's site");
    UnitType onMenu = ty.limited; onMenu.id = "onmenu"; onMenu.buildMenu = {"sol"};
    const int lim2 = w.spawn(&onMenu, 1100, 1200, 0.0f, 0);
    check(accepted(lim2, own), "a builderlimited builder with the type on its menu helps");
}

// A conjurer on a repeat queue: an Assist is an Assist, a Move is a rally.
static void repeatConjurer() {
    std::printf("a repeating conjurer:\n");
    Types ty;
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);
    const int fac = w.spawn(&ty.factory, 1000, 1000, 0.0f, 0);
    w.train(fac, &ty.sol, 3);
    const int conj = w.spawn(&ty.conjurer, 1000, 1300, 0.0f, 0);
    w.setRepeat(conj, &ty.sol);
    for (int i = 0; i < 10; ++i) w.tick(1.0f / 30.0f);
    const int own = w.unit(fac)->productionSiteId;
    const int mine = w.unit(conj)->productionSiteId;
    tak::net::Command mv; mv.kind = tak::net::Cmd::Move; mv.unitId = conj; mv.x = 200; mv.z = 1800;
    applyCommand(w, emptyRegistry(), mv);
    check(w.unit(conj)->orders.empty() && !w.unit(conj)->rally.empty(), "its Move is still a rally for its output");
    command(w, tak::net::Cmd::Assist, conj, own);
    check(w.unit(conj)->buildSiteId == own, "an Assist on an unfinished own unit is accepted");
    check(w.unit(conj)->repeatType == &ty.sol, "...keeping its repeat queue");
    const int32_t before = w.unit(conj)->buildProgress;
    const Fixed hpBefore = w.unit(mine)->hp;
    for (int i = 0; i < 120; ++i) w.tick(1.0f / 30.0f);
    // Retail runs one mission: the output gets no work while its maker helps.
    // Ours holds it meanwhile (our choice; retail's GetBuilt would decay it).
    check(w.unit(conj)->buildProgress == before && w.unit(conj)->productionSiteId == mine &&
          w.unit(mine)->hp == hpBefore,
          "...and its own output, unworked while it helps, is held");
    for (int i = 0; i < 600 && w.unit(conj)->buildSiteId; ++i) w.tick(1.0f / 30.0f);
    for (int i = 0; i < 30; ++i) w.tick(1.0f / 30.0f);
    check(w.unit(conj)->buildProgress > before || w.unit(conj)->productionSiteId != mine,
          "...and resumes when the help ends");
}

// ---------------------------------------------------------------------------
// Guard (Follow_Ground stage 1).
// ---------------------------------------------------------------------------
static int guardLatch(World& w, int guard, int ticks, int want, bool repair) {
    for (int i = 0; i < ticks; ++i) {
        w.tick(1.0f / 30.0f);
        const Unit* g = w.unit(guard);
        if (repair ? g->repairId == want : g->buildSiteId == want) return i;
    }
    return -1;
}

static bool guardUnder(const World& w, int guard) {
    const Unit* g = w.unit(guard);
    return !g->orders.empty() && g->orders.back().guard;
}

static void guardHelps() {
    std::printf("a guarding builder:\n");
    Types ty;
    char d[160];
    {   // guards a producing factory -> helps its output, guard resumes after
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        const int fac = w.spawn(&ty.factory, 1000, 1000, 0.0f, 0);
        w.train(fac, &ty.sol, 2);
        const int g = w.spawn(&ty.builder, 1060, 1150, 0.0f, 0);
        w.tick(1.0f / 30.0f);
        const int site = w.unit(fac)->productionSiteId;
        command(w, tak::net::Cmd::Guard, g, fac);
        const int at = guardLatch(w, g, 5, site, false);
        std::snprintf(d, sizeof d, "latched after %d ticks", at);
        check(at >= 0, "guarding a busy factory, it helps the factory's output at once", d);
        check(guardUnder(w, g), "...with its guard order kept under the help");
        int finish = -1;
        for (int i = 0; i < 400 && finish < 0; ++i) {
            w.tick(1.0f / 30.0f);
            if (const Unit* s = w.unit(site); s && !s->underConstruction) finish = i;
        }
        check(finish >= 0 && finish < 150, "...which finishes sooner than the factory alone",
              "finished " + std::to_string(finish) + " ticks after latching");
        // The next output starts once the finished one has cleared the pad;
        // the guard's next look (15..29 ticks apart while idle) picks it up.
        int next = 0;
        for (int i = 0; i < 300 && !(next = w.unit(fac)->productionSiteId); ++i) w.tick(1.0f / 30.0f);
        const int again = guardLatch(w, g, 31, next, false);
        check(next && next != site && again >= 0, "...and it helps the next output from its next look",
              "next site " + std::to_string(next) + " latched after " + std::to_string(again));
        for (int i = 0; i < 400 && w.unit(g)->buildSiteId; ++i) w.tick(1.0f / 30.0f);
        for (int i = 0; i < 3; ++i) w.tick(1.0f / 30.0f);
        check(!w.unit(g)->orders.empty() && w.unit(g)->orders.front().guard &&
              w.unit(g)->orders.front().targetId == fac, "with the queue done, the guard order is in front again");
    }
    {   // guards a builder that is helping a site -> helps that site; repair of an ally
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        w.setTeam(1, 0);
        const int fac = w.spawn(&ty.factory, 1000, 1000, 0.0f, 0);
        w.train(fac, &ty.sol, 1);
        const int worker = w.spawn(&ty.builder, 1000, 1150, 0.0f, 0);
        const int g = w.spawn(&ty.builder, 1200, 1300, 0.0f, 0);
        w.tick(1.0f / 30.0f);
        const int site = w.unit(fac)->productionSiteId;
        w.assist(worker, site, false);
        for (int i = 0; i < 20; ++i) w.tick(1.0f / 30.0f);
        command(w, tak::net::Cmd::Guard, g, worker);
        check(guardLatch(w, g, 3, site, false) >= 0, "guarding a helping builder, it helps that same output");
        // ally repair through the guarded unit's RepairUnit
        const int allyUnit = w.spawn(&ty.sol, 700, 1300, 0.0f, 1);
        w.unit(allyUnit)->hp = Fixed::fromInt(100);
        const int g2 = w.spawn(&ty.builder, 600, 1500, 0.0f, 0);
        const int repairer = w.spawn(&ty.builder, 650, 1350, 0.0f, 0);
        w.repair(repairer, allyUnit, false);
        for (int i = 0; i < 3; ++i) w.tick(1.0f / 30.0f);
        command(w, tak::net::Cmd::Guard, g2, repairer);
        check(guardLatch(w, g2, 3, allyUnit, true) >= 0, "guarding a repairer, it repairs the ally being repaired");
        check(guardUnder(w, g2), "...guard order kept under the repair");
        // the guarded unit itself damaged -> repair it
        const int hurt = w.spawn(&ty.sol, 1600, 1600, 0.0f, 0);
        w.unit(hurt)->hp = Fixed::fromInt(50);
        const int g3 = w.spawn(&ty.builder, 1650, 1650, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g3, hurt);
        check(guardLatch(w, g3, 3, hurt, true) >= 0, "a damaged guarded unit is repaired");
        for (int i = 0; i < 900 && w.unit(g3)->repairId; ++i) w.tick(1.0f / 30.0f);
        check(w.unit(hurt)->hp.toFloat() >= 299.9f && !w.unit(g3)->orders.empty() && w.unit(g3)->orders.front().guard,
              "...to full, then the guard resumes");
    }
    {   // a damaged guarded unit that walks away: the guard's repair follows it
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        w.spawn(&ty.factory, 300, 300, 0.0f, 0);       // mana storage
        const int hurt = w.spawn(&ty.sol, 1000, 1000, 0.0f, 0);
        w.unit(hurt)->hp = Fixed::fromInt(50);
        const int g = w.spawn(&ty.builder, 1300, 1000, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g, hurt);
        check(guardLatch(w, g, 3, hurt, true) >= 0, "a damaged guarded unit about to walk off is repaired");
        tak::net::Command mv;
        mv.kind = tak::net::Cmd::Move; mv.player = 0; mv.unitId = hurt; mv.x = 1000; mv.z = 1900;
        applyCommand(w, emptyRegistry(), mv);
        float worst = 0;
        for (int i = 0; i < 900; ++i) {
            w.tick(1.0f / 30.0f);
            const Unit* a = w.unit(g);
            const Unit* b = w.unit(hurt);
            if (i < 120) continue;                       // the first catch-up
            const float dx = (a->x - b->x).toFloat(), dz = (a->z - b->z).toFloat();
            worst = std::max(worst, std::sqrt(dx * dx + dz * dz));
        }
        std::snprintf(d, sizeof d, "worst distance %.0f, hp %.0f", worst, w.unit(hurt)->hp.toFloat());
        check(worst < 250.0f, "...the guard keeps up with it while it moves", d);   // was ~1300
        check(w.unit(hurt)->hp.toFloat() >= 299.9f, "...and repairs it to full once in reach", d);
        for (int i = 0; i < 3; ++i) w.tick(1.0f / 30.0f);
        const Unit* gu = w.unit(g);
        check(!gu->repairId && !gu->orders.empty() && gu->orders.front().guard &&
              gu->orders.front().targetId == hurt, "...then the guard order is in front again");
    }
    {   // negative cases: idle guarded unit, off-menu limited guard, enemy-free
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        const int idle = w.spawn(&ty.sol, 1000, 1000, 0.0f, 0);
        const int g = w.spawn(&ty.builder, 1100, 1100, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g, idle);
        w.tick(1.0f / 30.0f);
        const Unit* gu = w.unit(g);
        const uint32_t wait = gu->guardHelpTick - (w.tickCount() - 1);
        std::snprintf(d, sizeof d, "next look in %u ticks", wait);
        check(!gu->buildSiteId && !gu->repairId && gu->orders.size() == 1 && gu->orders.front().guard,
              "guarding an idle unit, it only escorts", d);
        check(wait >= 15 && wait < 30, "...and looks again after 15 + rand(15) ticks", d);
        bool spread = false;
        uint32_t prev = gu->guardHelpTick;
        for (int i = 0; i < 200; ++i) {
            w.tick(1.0f / 30.0f);
            if (w.unit(g)->guardHelpTick != prev) {
                const uint32_t gap = w.unit(g)->guardHelpTick - prev;
                if (gap < 15 || gap >= 30) { spread = false; break; }
                if (gap != wait) spread = true;
                prev = w.unit(g)->guardHelpTick;
            }
        }
        check(spread, "...each look re-arming 15..29 ticks out, drawn afresh");
        const int fac = w.spawn(&ty.factory, 400, 400, 0.0f, 0);
        w.train(fac, &ty.sol, 1);
        const int lim = w.spawn(&ty.limited, 500, 600, 0.0f, 0);
        w.tick(1.0f / 30.0f);
        command(w, tak::net::Cmd::Guard, lim, fac);
        check(guardLatch(w, lim, 40, w.unit(fac)->productionSiteId, false) < 0,
              "a builderlimited guard does not help an output off its menu");
        // A non-builder guard escorts only.
        const int s2 = w.spawn(&ty.sol, 500, 650, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, s2, fac);
        for (int i = 0; i < 40; ++i) w.tick(1.0f / 30.0f);
        check(!w.unit(s2)->buildSiteId, "a non-builder guard only escorts");
    }
    {   // a builder walking to a placed site is just followed until the site is begun
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        w.spawn(&ty.factory, 300, 1800, 0.0f, 0);         // storage for the pool
        const int walker = w.spawn(&ty.builder, 400, 400, 0.0f, 0);
        const int g = w.spawn(&ty.builder, 330, 480, 0.0f, 0);
        w.queueBuild(walker, &ty.factory, 1600, 1600, false);
        command(w, tak::net::Cmd::Guard, g, walker);
        int site = 0, begunTick = -1, latchTick = -1;
        for (int i = 0; i < 1500 && latchTick < 0; ++i) {
            w.tick(1.0f / 30.0f);
            if (!site) site = w.unit(walker)->buildSiteId;
            const Unit* s = site ? w.unit(site) : nullptr;
            if (s && s->buildBegun && begunTick < 0) begunTick = i;
            if (site && w.unit(g)->buildSiteId == site) latchTick = i;
        }
        std::snprintf(d, sizeof d, "begun t=%d, guard latched t=%d", begunTick, latchTick);
        check(begunTick >= 0 && latchTick >= begunTick, "a builder still walking to its site is followed, then helped", d);
        for (int i = 0; i < 1500 && w.unit(walker)->buildSiteId; ++i) w.tick(1.0f / 30.0f);
        const Unit* s = w.unit(site);
        check(s && !s->underConstruction && !w.unit(g)->buildSiteId && !w.unit(g)->orders.empty() &&
              w.unit(g)->orders.front().guard, "...the site is finished and the guard order resumes");
    }
}


// ---------------------------------------------------------------------------
// Flying guard (VTOL_Follow stage 2) and its VTOL jobs.
// ---------------------------------------------------------------------------
static float dist(const World& w, int a, int b) {
    const Unit* x = w.unit(a);
    const Unit* y = w.unit(b);
    const float dx = (x->x - y->x).toFloat(), dz = (x->z - y->z).toFloat();
    return std::sqrt(dx * dx + dz * dz);
}

// Runs until `helper`'s first tick of construction work; the distance from
// `target` it was at then, or -1.
static float firstWorkDistance(World& w, int helper, int target, int ticks) {
    const uint32_t before = w.unit(helper)->constructionEmissions[0];
    for (int i = 0; i < ticks; ++i) {
        const float d = dist(w, helper, target);
        w.tick(1.0f / 30.0f);
        if (w.unit(helper)->constructionEmissions[0] != before) return std::max(d, dist(w, helper, target));
    }
    return -1;
}

// Watches `repairer` mend `target`: where the work starts, the farthest it is
// paid from, how far the flyer moves while working, and that it finishes.
static void flyingRepairShape(World& w, int repairer, int target, const char* lead) {
    char d[200];
    float firstWork = -1, worst = 0;
    int mended = -1;
    float lastX = w.unit(repairer)->x.toFloat(), lastZ = w.unit(repairer)->z.toFloat();
    float travel = 0, nearest = 1e9f;
    int workTicks = 0;
    for (int i = 0; i < 900 && mended < 0; ++i) {
        const Fixed hp = w.unit(target)->hp;
        const float dBefore = dist(w, repairer, target);
        w.tick(1.0f / 30.0f);
        if (w.unit(target)->hp > hp) {
            if (firstWork < 0) firstWork = std::max(dBefore, dist(w, repairer, target));
            worst = std::max(worst, dist(w, repairer, target));
            travel += std::fabs(w.unit(repairer)->x.toFloat() - lastX) + std::fabs(w.unit(repairer)->z.toFloat() - lastZ);
            if (++workTicks > 60) nearest = std::min(nearest, dist(w, repairer, target));
        }
        lastX = w.unit(repairer)->x.toFloat(); lastZ = w.unit(repairer)->z.toFloat();
        if (w.unit(target)->hp.toFloat() >= 299.9f) mended = i;
    }
    std::snprintf(d, sizeof d, "first work %.1f px, worst %.1f px, nearest once settled %.1f px, moved %.0f px, mended t=%d",
                  firstWork, worst, nearest, travel, mended);
    const std::string l(lead);
    check(firstWork >= 0 && firstWork < 201.0f, (l + "work starts once inside 2 x builddistance").c_str(), d);
    check(worst <= 201.0f, (l + "and is only paid within 2 x builddistance").c_str(), d);
    check(travel > 50.0f && nearest >= 60.0f && nearest < 1e9f,
          (l + "while the flyer orbits at about builddistance, not parked over it").c_str(), d);
    check(mended >= 0, (l + "to full health").c_str(), d);
}

// A flyer's guard (VTOL_Follow) keeps its next look on its mission.
static uint32_t flyLook(const World& w, int guard) {
    for (const auto& o : w.unit(guard)->orders)
        if (o.guard) return o.mission.deadline;
    return 0;
}

static void flyingGuardHelps() {
    std::printf("a guarding flying builder:\n");
    Types ty;
    char d[200];
    {   // a busy factory 1300 px away: latches at once, flies in, works only inside 2 x builddistance
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        const int fac = w.spawn(&ty.slowfac, 600, 600, 0.0f, 0);
        w.train(fac, &ty.slow, 1);
        const int g = w.spawn(&ty.flyer, 1500, 1600, 0.0f, 0);
        w.tick(1.0f / 30.0f);
        const int site = w.unit(fac)->productionSiteId;
        command(w, tak::net::Cmd::Guard, g, fac);
        const int at = guardLatch(w, g, 3, site, false);
        std::snprintf(d, sizeof d, "latched after %d ticks, %.0f px away", at, dist(w, g, site));
        check(at >= 0, "guarding a distant busy factory, it helps the output at once (no distance gate)", d);
        check(guardUnder(w, g), "...with its guard order kept under the help");
        const uint32_t wait = flyLook(w, g) - w.tickCount();
        std::snprintf(d, sizeof d, "next look in %u ticks", wait);
        check(wait >= 14 && wait < 45, "...and the guard sleeps 15 + rand(30) after the insert", d);
        const int start = int(w.tickCount());
        const float wd = firstWorkDistance(w, g, site, 900);
        const int flown = int(w.tickCount()) - start;
        std::snprintf(d, sizeof d, "first work at %.1f px after %d ticks (2 x builddistance = 200)", wd, flown);
        check(wd >= 0 && wd < 201.0f, "...it works only once flown inside 2 x builddistance", d);
        int finish = -1;
        float worst = 0;
        for (int i = 0; i < 900 && finish < 0; ++i) {
            w.tick(1.0f / 30.0f);
            if (i > 60) worst = std::max(worst, std::fabs(dist(w, g, site) - 100.0f));
            if (const Unit* s = w.unit(site); s && !s->underConstruction) finish = i;
        }
        std::snprintf(d, sizeof d, "finished %d ticks after the first work (alone: %d), worst orbit error %.0f px",
                      finish, 899 - flown, worst);
        check(finish >= 0 && finish < (899 - flown) * 2 / 3, "...finishing the output sooner than the factory alone", d);
        check(worst < 40.0f, "...while orbiting it at about builddistance", d);
        for (int i = 0; i < 5; ++i) w.tick(1.0f / 30.0f);
        check(!w.unit(g)->buildSiteId && !w.unit(g)->orders.empty() && w.unit(g)->orders.back().guard &&
              w.unit(g)->orders.back().targetId == fac, "...then the guard order is back in charge");
    }
    {   // a flying producer: helped by a flying guard, never by a ground guard
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        w.spawn(&ty.factory, 300, 300, 0.0f, 0);       // storage for the pool
        const int prod = w.spawn(&ty.flyconj, 1000, 1000, 0.0f, 0);
        w.train(prod, &ty.sol, 3);
        const int fg = w.spawn(&ty.flyer, 1200, 1200, 0.0f, 0);
        const int gg = w.spawn(&ty.builder, 800, 1200, 0.0f, 0);
        for (int i = 0; i < 5 && !w.unit(prod)->productionSiteId; ++i) w.tick(1.0f / 30.0f);
        const int site = w.unit(prod)->productionSiteId;
        check(site != 0, "a flying producer has its output under way");
        command(w, tak::net::Cmd::Guard, fg, prod);
        command(w, tak::net::Cmd::Guard, gg, prod);
        check(guardLatch(w, fg, 3, site, false) >= 0, "a flying guard helps a flying producer's output");
        int latched = 0;
        for (int i = 0; i < 90; ++i) { w.tick(1.0f / 30.0f); latched |= w.unit(gg)->buildSiteId; }
        check(!latched, "...a ground guard does not (Follow_Ground lists no VTOL mission)");
    }
    {   // the guarded unit's repair target: an ally, flying guard yes; ground guard of a flying repairer no
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        w.setTeam(1, 0);
        w.spawn(&ty.factory, 300, 300, 0.0f, 0);
        const int ally = w.spawn(&ty.ground, 1000, 1000, 1.0f, 1);
        w.unit(ally)->hp = Fixed::fromInt(30);
        const int repairer = w.spawn(&ty.builder, 1050, 1050, 0.0f, 0);
        const int fg = w.spawn(&ty.flyer, 1400, 1400, 0.0f, 0);
        w.repair(repairer, ally, false);
        for (int i = 0; i < 3; ++i) w.tick(1.0f / 30.0f);
        command(w, tak::net::Cmd::Guard, fg, repairer);
        check(guardLatch(w, fg, 3, ally, true) >= 0, "a flying guard repairs the ally its guarded builder repairs");
        check(guardUnder(w, fg), "...guard order kept under the repair");
        const int ally2 = w.spawn(&ty.ground, 400, 1600, 1.0f, 1);
        w.unit(ally2)->hp = Fixed::fromInt(30);
        const int flyRepairer = w.spawn(&ty.flyer, 450, 1650, 0.0f, 0);
        const int gg = w.spawn(&ty.builder, 600, 1700, 0.0f, 0);
        w.repair(flyRepairer, ally2, false);
        for (int i = 0; i < 3; ++i) w.tick(1.0f / 30.0f);
        command(w, tak::net::Cmd::Guard, gg, flyRepairer);
        check(guardLatch(w, gg, 60, ally2, true) < 0,
              "a ground guard does not join a flyer's VTOL_RepairUnit");
    }
    {   // the guarded unit itself damaged: flown in, orbited, mended; the repair holds 2 x builddistance
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        w.spawn(&ty.factory, 300, 300, 0.0f, 0);
        const int hurt = w.spawn(&ty.ground, 1000, 1000, 0.0f, 0);
        w.unit(hurt)->hp = Fixed::fromInt(50);
        const int g = w.spawn(&ty.flyer, 1600, 1600, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g, hurt);
        check(guardLatch(w, g, 3, hurt, true) >= 0, "a damaged guarded unit is repaired by a flying guard");
        flyingRepairShape(w, g, hurt, "...");
        for (int i = 0; i < 5; ++i) w.tick(1.0f / 30.0f);
        check(!w.unit(g)->repairId && !w.unit(g)->orders.empty() && w.unit(g)->orders.front().guard,
              "...then the guard resumes");
    }
    {   // negatives and the look cadence
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        const int idle = w.spawn(&ty.sol, 1000, 1000, 0.0f, 0);
        const int g = w.spawn(&ty.flyer, 1100, 1100, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g, idle);
        w.tick(1.0f / 30.0f);
        const uint32_t wait = flyLook(w, g) - (w.tickCount() - 1);
        std::snprintf(d, sizeof d, "next look in %u ticks", wait);
        check(!w.unit(g)->buildSiteId && !w.unit(g)->repairId && w.unit(g)->orders.size() == 1,
              "a flying guard of an idle unit only escorts", d);
        int looks = 0, early = 0;
        bool fixed = true;
        uint32_t prev = flyLook(w, g);
        for (int i = 0; i < 200; ++i) {
            const bool arrived = (w.unit(g)->orders.front().mission.pending & 0x100) != 0;
            w.tick(1.0f / 30.0f);
            if (flyLook(w, g) != prev) {
                // Every look re-arms 15 ticks out; one woken by the orbit
                // point's arrival comes before its deadline.
                fixed = fixed && flyLook(w, g) == w.tickCount() + 15 &&
                        (flyLook(w, g) - prev == 15 || arrived);
                early += flyLook(w, g) - prev != 15;
                prev = flyLook(w, g);
                ++looks;
            }
        }
        std::snprintf(d, sizeof d, "%d looks, %d woken early by an arrival", looks, early);
        check(fixed && looks >= 12, "...re-looking 15 ticks after each look, or on reaching its orbit point", d);
        const int fac = w.spawn(&ty.factory, 400, 400, 0.0f, 0);
        w.train(fac, &ty.sol, 1);
        const int lim = w.spawn(&ty.flylimited, 500, 600, 0.0f, 0);
        const int nb = w.spawn(&ty.flysol, 550, 600, 0.0f, 0);
        w.tick(1.0f / 30.0f);
        command(w, tak::net::Cmd::Guard, lim, fac);
        command(w, tak::net::Cmd::Guard, nb, fac);
        check(guardLatch(w, lim, 40, w.unit(fac)->productionSiteId, false) < 0,
              "a builderlimited flying guard does not help an output off its menu");
        check(!w.unit(nb)->buildSiteId, "a flyer that is no builder only escorts");
        UnitType onMenu = ty.flylimited; onMenu.id = "flyonmenu"; onMenu.buildMenu = {"sol"};
        const int lim2 = w.spawn(&onMenu, 450, 650, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, lim2, fac);
        check(guardLatch(w, lim2, 3, w.unit(fac)->productionSiteId, false) >= 0,
              "...one with the output's type on its menu does");
    }
    {   // a repair target below the sea is not taken on by a flyer
        auto ownedWorld = std::make_unique<World>();
        World& w = *ownedWorld;
        w.setTerrain(std::vector<uint8_t>(size_t(128) * 128, 0), 128, 128, 60);   // all deep water
        w.setPathService(true);
        w.player(0).mana = 100000;
        UnitType sub = ty.ground; sub.id = "sub"; sub.modelTop = Fixed::fromInt(10).v;
        const int hurt = w.spawn(&sub, 1000, 1000, 0.0f, 0);
        w.unit(hurt)->hp = Fixed::fromInt(50);
        const int g = w.spawn(&ty.flyer, 1100, 1100, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g, hurt);
        std::snprintf(d, sizeof d, "target ground y %d, sea 60", w.unit(hurt)->groundY.floorInt());
        check(guardLatch(w, g, 40, hurt, true) < 0, "a flying guard does not repair a unit under the sea", d);
        const int hurtFlyer = w.spawn(&ty.flysol, 1300, 1000, 0.0f, 0);
        w.order(hurtFlyer, 1300, 1300, false);           // up to its cruise height
        for (int i = 0; i < 120; ++i) w.tick(1.0f / 30.0f);
        w.unit(hurtFlyer)->hp = Fixed::fromInt(50);
        const int g2 = w.spawn(&ty.flyer, 1400, 1100, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g2, hurtFlyer);
        std::snprintf(d, sizeof d, "flyer y %d", w.unit(hurtFlyer)->flightY.floorInt());
        check(guardLatch(w, g2, 3, hurtFlyer, true) >= 0, "...but does repair a flyer above it", d);
        const int f3 = w.spawn(&ty.flyer, 1050, 1150, 0.0f, 0);
        w.repair(f3, hurt, false);
        check(w.unit(f3)->orders.empty() && !w.unit(f3)->repairId,
              "a flyer told to repair a unit under the sea refuses (VTOL_RepairUnit stage 0)");
        // VTOL_Follow: the refused repair is no repair, so the look goes on to
        // what the guarded unit is doing -- here, repairing a flyer above.
        const int hurtFlyer2 = w.spawn(&ty.flysol, 400, 400, 0.0f, 0);
        w.order(hurtFlyer2, 400, 420, false);
        for (int i = 0; i < 120; ++i) w.tick(1.0f / 30.0f);
        w.unit(hurtFlyer2)->hp = Fixed::fromInt(50);
        UnitType subBuilder = sub; subBuilder.id = "subbuilder";
        subBuilder.isBuilder = true; subBuilder.workerTime = 1; subBuilder.buildDist = 300;
        const int diver = w.spawn(&subBuilder, 420, 440, 0.0f, 0);
        w.unit(diver)->hp = Fixed::fromInt(50);
        w.repair(diver, hurtFlyer2, false);
        w.tick(1.0f / 30.0f);
        const int g3 = w.spawn(&ty.flyer, 700, 700, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g3, diver);
        std::snprintf(d, sizeof d, "diver y %d, its repair %d", w.unit(diver)->groundY.floorInt(), w.unit(diver)->repairId);
        check(w.unit(diver)->repairId == hurtFlyer2 && guardLatch(w, g3, 3, hurtFlyer2, true) >= 0,
              "a damaged guarded unit under the sea: the flying guard joins its repair instead", d);
    }
    {   // a plain Repair order to a flyer (VTOL_RepairUnit): flown in, orbited
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        w.spawn(&ty.factory, 300, 300, 0.0f, 0);
        const int hurt = w.spawn(&ty.ground, 1000, 1000, 0.0f, 0);
        w.unit(hurt)->hp = Fixed::fromInt(50);
        const int f = w.spawn(&ty.flyer, 1500, 1200, 0.0f, 0);
        w.repair(f, hurt, false);
        flyingRepairShape(w, f, hurt, "a flyer told to repair: ");
    }
    {   // a plain Assist by a flyer on a site already begun: no work from afar
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        const int fac = w.spawn(&ty.slowfac, 500, 500, 0.0f, 0);
        w.train(fac, &ty.slow, 1);
        const int f = w.spawn(&ty.flyer, 1700, 1700, 0.0f, 0);
        for (int i = 0; i < 5 && !w.unit(fac)->productionSiteId; ++i) w.tick(1.0f / 30.0f);
        const int site = w.unit(fac)->productionSiteId;
        w.assist(f, site, false);
        const float wd = firstWorkDistance(w, f, site, 900);
        std::snprintf(d, sizeof d, "first work at %.1f px", wd);
        check(wd >= 0 && wd < 201.0f, "a flyer assisting a begun site flies within 2 x builddistance before working", d);
    }
}

// ---------------------------------------------------------------------------
// VTOL_Follow's idle orbit (0x419053): every flyer on guard, armed or not.
// ---------------------------------------------------------------------------
// The guard order's orbit point, relative to the guarded unit: its distance
// and its angle (retail BAM: the point is the guarded unit less (sin, cos)).
struct OrbitPoint { bool set = false; float distance = 0; int angle = 0; int16_t radius = 0; uint16_t flags = 0; float x = 0, z = 0; };
static OrbitPoint orbitPoint(const World& w, int guard, int guarded) {
    OrbitPoint p;
    const Unit* g = w.unit(guard);
    const Unit* t = w.unit(guarded);
    if (g->orders.empty() || !g->orders.front().guard || !g->orders.front().flightGoal) return p;
    const auto& goal = *g->orders.front().flightGoal;
    p.set = true;
    p.x = Fixed::raw(goal.point.x).toFloat(); p.z = Fixed::raw(goal.point.z).toFloat();
    const float dx = t->x.toFloat() - p.x, dz = t->z.toFloat() - p.z;
    p.distance = std::sqrt(dx * dx + dz * dz);
    p.angle = int(std::lround(std::atan2(dx, dz) / (2 * 3.14159265358979) * 65536.0)) & 0xffff;
    p.radius = goal.radius; p.flags = goal.flags;
    return p;
}

static void flyingGuardOrbit() {
    std::printf("a guarding flyer's idle orbit (VTOL_Follow):\n");
    Types ty;
    char d[240];
    UnitType armed = ty.flysol;
    armed.name = armed.id = "armedfly";
    Weapon spear; spear.name = "spear"; spear.range = 200; spear.damage = 10;
    armed.weapons.push_back(spear);
    armed.weapon = spear;
    struct Case { const UnitType* type; float radius; const char* what; };
    const Case cases[] = {
        {&ty.flysol, 320.0f, "an unarmed flyer that is no builder"},
        {&ty.flyer, 320.0f, "a flying builder with nothing to help"},
        {&armed, 360.0f, "an armed flyer (weapon range 200)"},
    };
    for (const Case& c : cases) {
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        const int idle = w.spawn(&ty.sol, 1000, 1000, 0.0f, 0);
        const int g = w.spawn(c.type, 1200, 1100, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g, idle);
        w.tick(1.0f / 30.0f);
        const OrbitPoint p = orbitPoint(w, g, idle);
        std::snprintf(d, sizeof d, "point %.1f px out, accepts at %d (flags %#x)", p.distance, p.radius, p.flags);
        const std::string l(c.what);
        check(p.set && std::fabs(p.distance - c.radius) < 1.5f,
              (l + (c.radius > 320 ? ": orbits at weapon range + 160" : ": orbits at 320 px")).c_str(), d);
        check(p.set && p.radius == 128 && (p.flags & 0x10), "...a point controller accepting at 128 px", d);
        // Watch it fly: each arrival (inside 128 of its point) steps the angle
        // back by 0x4000 + rand(0x2000) at the next tick's look; nothing else does.
        int arrivals = 0, steps = 0, badSteps = 0, badArrive = 0, offRadius = 0;
        float minFly = 1e9f, maxFly = 0;
        OrbitPoint last = p;
        for (int i = 0; i < 1500; ++i) {
            const float px = w.unit(g)->x.toFloat(), pz = w.unit(g)->z.toFloat();
            w.tick(1.0f / 30.0f);
            const Unit* u = w.unit(g);
            const OrbitPoint now = orbitPoint(w, g, idle);
            if (!now.set) { ++badSteps; break; }
            if (std::fabs(now.distance - c.radius) > 1.5f) ++offRadius;
            const bool arrived = (u->orders.front().mission.pending & 0x100) != 0;
            if (arrived) {
                const float dx = px - now.x, dz = pz - now.z;
                if (std::sqrt(dx * dx + dz * dz) >= 128.5f) ++badArrive;
            }
            if (now.angle != last.angle) {
                const int delta = (last.angle - now.angle) & 0xffff;
                ++steps;
                if (delta < 0x4000 - 0x100 || delta > 0x6000 + 0x100) ++badSteps;
            }
            if (arrived) {
                ++arrivals;
                // The arrival wakes the look on the very next tick.
                w.tick(1.0f / 30.0f);
                const OrbitPoint next = orbitPoint(w, g, idle);
                const int delta = (now.angle - next.angle) & 0xffff;
                if (!next.set || delta < 0x4000 - 0x100 || delta > 0x6000 + 0x100) ++badSteps;
                ++steps;
                last = next;
                continue;
            }
            last = now;
            if (i > 200) {
                const float dx = u->x.toFloat() - 1000, dz = u->z.toFloat() - 1000;
                minFly = std::min(minFly, std::sqrt(dx * dx + dz * dz));
                maxFly = std::max(maxFly, std::sqrt(dx * dx + dz * dz));
            }
        }
        std::snprintf(d, sizeof d, "%d arrivals, %d steps (%d bad), %d late arrivals, %d off radius; flew %.0f..%.0f px out",
                      arrivals, steps, badSteps, badArrive, offRadius, minFly, maxFly);
        check(arrivals >= 4 && steps == arrivals && !badSteps && !badArrive && !offRadius,
              "...steps its angle by 0x4000..0x5fff only on reaching its point", d);
        // Between points 0x4000..0x6000 apart it cuts inside the ring, and it
        // turns for the next point 128 px short of each.
        check(minFly > 0.2f * c.radius && maxFly < c.radius + 40,
              "...and keeps circling the guarded unit, never parking over it", d);
    }
    {   // the orbit follows a guarded unit that moves, from look to look
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        const int walker = w.spawn(&ty.sol, 500, 1000, 0.0f, 0);
        const int g = w.spawn(&ty.flysol, 500, 800, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g, walker);
        w.tick(1.0f / 30.0f);
        w.order(walker, 1500, 1000, false);
        float worstPoint = 0, worstFlyer = 0;
        for (int i = 0; i < 600; ++i) {
            w.tick(1.0f / 30.0f);
            const OrbitPoint p = orbitPoint(w, g, walker);
            // The point is set at the look (at most 15 ticks old): the walker
            // has moved up to 15 x 2 px since.
            if (p.set) worstPoint = std::max(worstPoint, std::fabs(p.distance - 320.0f));
            if (i > 150) worstFlyer = std::max(worstFlyer, dist(w, g, walker));
        }
        std::snprintf(d, sizeof d, "point off its ring by up to %.0f px; flyer at most %.0f px from the walker",
                      worstPoint, worstFlyer);
        check(worstPoint < 40.0f && worstFlyer < 520.0f, "a flying guard's orbit point follows a moving guarded unit", d);
    }
    {   // a flying builder's job ends: the guard resumes its orbit
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        w.spawn(&ty.factory, 300, 300, 0.0f, 0);
        const int hurt = w.spawn(&ty.ground, 1000, 1000, 0.0f, 0);
        w.unit(hurt)->hp = Fixed::fromInt(250);
        const int g = w.spawn(&ty.flyer, 1300, 1300, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g, hurt);
        int mended = -1;
        for (int i = 0; i < 900 && mended < 0; ++i) {
            w.tick(1.0f / 30.0f);
            if (w.unit(hurt)->hp.toFloat() >= 299.9f) mended = i;
        }
        for (int i = 0; i < 60; ++i) w.tick(1.0f / 30.0f);
        const OrbitPoint p = orbitPoint(w, g, hurt);
        std::snprintf(d, sizeof d, "mended t=%d, then point %.1f px out", mended, p.distance);
        check(mended >= 0 && p.set && std::fabs(p.distance - 320.0f) < 1.5f,
              "after its repair a flying guard goes back to its 320 px orbit", d);
    }
}

// A guard VTOL_Follow has not started yet: a flyer that takes the order off
// the map first flies back onto it (0x4188bc) and keeps the guard, and a new
// guard mission starts at once, whatever an earlier guard's look left behind.
static void flyingGuardStarts() {
    std::printf("a flying guard's mission starts (VTOL_Follow stage 0):\n");
    Types ty;
    char d[240];
    {   // ordered to guard while off the map
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        const int idle = w.spawn(&ty.sol, 1000, 1000, 0.0f, 0);
        const int g = w.spawn(&ty.flysol, 2, 1000, 0.0f, 0);
        const Unit* f = w.unit(g);
        const bool off = footprintOrigin(f->x, f->type->footX) < 0;
        command(w, tak::net::Cmd::Guard, g, idle);
        bool lost = false;
        for (int i = 0; i < 900 && !lost; ++i) {
            w.tick(1.0f / 30.0f);
            lost = w.unit(g)->orders.empty() || !w.unit(g)->orders.front().guard;
        }
        const OrbitPoint p = orbitPoint(w, g, idle);
        std::snprintf(d, sizeof d, "off map at start: %d; guard lost: %d; point %.1f px out (stage %d)",
                      int(off), int(lost), p.distance, lost ? -1 : int(w.unit(g)->orders.front().mission.stage));
        check(off && !lost && p.set && std::fabs(p.distance - 320.0f) < 1.5f,
              "a flyer told to guard off the map flies back, keeps its guard and orbits at 320 px", d);
    }
    {   // a queued guard becomes current between an earlier guard's looks
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        const int first = w.spawn(&ty.sol, 1000, 1000, 0.0f, 0);
        const int second = w.spawn(&ty.sol, 1400, 1000, 0.0f, 0);
        const int g = w.spawn(&ty.flysol, 1100, 1100, 0.0f, 0);
        command(w, tak::net::Cmd::Guard, g, first);
        w.guard(g, second, true);
        for (int i = 0; i < 3; ++i) w.tick(1.0f / 30.0f);
        w.destroy(first);   // a self-destruct countdown: the guard ends at its expiry
        for (int i = 0; i < 300 && w.unit(g)->orders.front().targetId == first; ++i) w.tick(1.0f / 30.0f);
        int started = -1;
        for (int i = 0; i < 40 && started < 0; ++i) {
            w.tick(1.0f / 30.0f);
            const Unit* u = w.unit(g);
            if (!u->orders.empty() && u->orders.front().guard && u->orders.front().targetId == second &&
                u->orders.front().mission.stage == 2) started = i;
        }
        const OrbitPoint p = orbitPoint(w, g, second);
        std::snprintf(d, sizeof d, "second guard started %d ticks after the first ended; point %.1f px out", started, p.distance);
        check(started >= 0 && started <= 2 && p.set && std::fabs(p.distance - 320.0f) < 1.5f,
              "a queued flying guard starts its mission at once (no earlier guard's sleep)", d);
    }
}

// ---------------------------------------------------------------------------
// A guard answers its guarded unit's attacker.
// ---------------------------------------------------------------------------
struct DefendRun {
    int firstHit = -1;        // tick the guarded unit was first hit
    int answered = -1;        // tick the guard's front order became an attack on the attacker
    bool guardKept = false;   // the guard order stayed queued under that attack
    int killed = -1;          // tick the attacker died
    bool resumed = false;     // the guard order is in front again afterwards
};
static DefendRun defendRun(const UnitType& guardType, float attackerDist, int fireState) {
    Types ty;
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    UnitType archer = ty.sol;                       // a ranged enemy that outranges the guard's acquisition
    archer.name = archer.id = "archer";
    Weapon bow; bow.name = "bow"; bow.range = attackerDist + 40; bow.damage = 3; bow.reload = 1.0f; bow.aimTol = 32767;
    archer.weapons.push_back(bow); archer.weapon = bow;
    const int guarded = w.spawn(&ty.sol, 1000, 1000, 0.0f, 0);
    const int g = w.spawn(&guardType, 1000, 1060, 0.0f, 0);
    w.unit(g)->fireState = uint8_t(fireState);
    const int enemy = w.spawn(&archer, 1000 + attackerDist, 1000, 0.0f, 1);
    command(w, tak::net::Cmd::Guard, g, guarded);
    w.attack(enemy, guarded, false);
    DefendRun r;
    for (int i = 0; i < 2400; ++i) {
        w.tick(1.0f / 30.0f);
        const Unit* v = w.unit(guarded);
        if (r.firstHit < 0 && v && v->hp < Fixed::fromInt(v->type->maxHp)) r.firstHit = i;
        const Unit* u = w.unit(g);
        if (r.answered < 0 && !u->orders.empty() && !u->orders.front().guard && u->orders.front().targetId == enemy) {
            r.answered = i;
            r.guardKept = u->orders.back().guard && u->orders.back().targetId == guarded;
        }
        const Unit* e = w.unit(enemy);
        if (r.killed < 0 && (!e || !e->alive())) r.killed = i;
        if (r.killed >= 0 && i > r.killed + 5) {
            r.resumed = !u->orders.empty() && u->orders.front().guard && u->orders.front().targetId == guarded;
            break;
        }
    }
    return r;
}

static void guardDefends() {
    std::printf("a guard answers its guarded unit's attacker:\n");
    Types ty;
    char d[240];
    UnitType soldier = ty.sol;                     // a ground guard; acquisition reach 120 + 90
    soldier.name = soldier.id = "soldier";
    Weapon sword; sword.name = "sword"; sword.range = 120; sword.damage = 100; sword.reload = 0.5f; sword.aimTol = 32767;
    soldier.weapons.push_back(sword); soldier.weapon = sword;
    UnitType hawk = ty.flysol;                     // a flying guard; acquisition reach 150 + 90, orbit 310
    hawk.name = hawk.id = "hawk";
    Weapon claw; claw.name = "claw"; claw.range = 150; claw.damage = 100; claw.reload = 0.5f; claw.aimTol = 32767;
    hawk.weapons.push_back(claw); hawk.weapon = claw;
    auto report = [&](const DefendRun& r) {
        std::snprintf(d, sizeof d, "hit %d, answered %d (guard kept %d), attacker dead %d, guard resumed %d",
                      r.firstHit, r.answered, int(r.guardKept), r.killed, int(r.resumed));
    };
    {
        const DefendRun r = defendRun(soldier, 400, 2);
        report(r);
        check(r.firstHit >= 0 && r.answered >= r.firstHit && r.answered <= r.firstHit + 2,
              "a ground guard goes after an attacker beyond its acquisition at the first hit", d);
        check(r.guardKept, "...with its guard order kept under the attack", d);
        check(r.killed >= 0 && r.resumed, "...kills it, and the guard order is in front again", d);
    }
    {
        const DefendRun r = defendRun(soldier, 400, 0);
        report(r);
        check(r.answered >= 0 && r.killed >= 0, "a ground guard on Hold Fire still answers (4d7de0 explicit)", d);
    }
    {
        const DefendRun r = defendRun(ty.sol, 400, 2);
        report(r);
        check(r.firstHit >= 0 && r.answered < 0, "an unarmed guard has nothing to answer with", d);
    }
    {
        const DefendRun r = defendRun(hawk, 800, 2);
        report(r);
        check(r.firstHit >= 0 && r.answered >= r.firstHit && r.answered <= r.firstHit + 16,
              "a flying guard goes after an attacker beyond its orbit and acquisition", d);
        check(r.guardKept, "...with its guard order kept under the attack", d);
        check(r.killed >= 0 && r.resumed, "...kills it, and goes back to guarding", d);
    }
}

static uint64_t defendRunHash() {
    Types ty;
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    UnitType soldier = ty.sol;
    soldier.name = soldier.id = "soldier";
    Weapon sword; sword.name = "sword"; sword.range = 120; sword.damage = 40; sword.reload = 0.5f; sword.aimTol = 32767;
    soldier.weapons.push_back(sword); soldier.weapon = sword;
    UnitType hawk = ty.flysol;
    hawk.name = hawk.id = "hawk";
    hawk.weapons.push_back(sword); hawk.weapon = sword;
    UnitType archer = ty.sol;
    archer.name = archer.id = "archer";
    Weapon bow; bow.name = "bow"; bow.range = 500; bow.damage = 3; bow.reload = 1.0f; bow.aimTol = 32767;
    archer.weapons.push_back(bow); archer.weapon = bow;
    const int guarded = w.spawn(&ty.sol, 1000, 1000, 0.0f, 0);
    for (int i = 0; i < 4; ++i)
        command(w, tak::net::Cmd::Guard, w.spawn(i % 2 ? &hawk : &soldier, 950.0f + 30 * i, 1080, 0.0f, 0), guarded);
    for (int i = 0; i < 3; ++i) w.attack(w.spawn(&archer, 1450, 900.0f + 100 * i, 0.0f, 1), guarded, false);
    for (int i = 0; i < 1500; ++i) w.tick(1.0f / 30.0f);
    return w.stateHash();
}

static uint64_t orbitRun() {
    Types ty;
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    UnitType armed = ty.flysol;
    armed.name = armed.id = "armedfly";
    Weapon spear; spear.name = "spear"; spear.range = 150; spear.damage = 10;
    armed.weapons.push_back(spear); armed.weapon = spear;
    const int walker = w.spawn(&ty.sol, 600, 1000, 0.0f, 0);
    const int idle = w.spawn(&ty.sol, 1400, 1400, 0.0f, 0);
    std::vector<int> guards;
    for (int i = 0; i < 6; ++i)
        guards.push_back(w.spawn(i % 3 == 0 ? &ty.flysol : i % 3 == 1 ? &armed : &ty.flyer, 900.0f + 70 * i, 900, 0.0f, 0));
    for (int i = 0; i < 6; ++i) command(w, tak::net::Cmd::Guard, guards[size_t(i)], i < 3 ? walker : idle);
    for (int i = 0; i < 1200; ++i) {
        w.tick(1.0f / 30.0f);
        if (i == 100) w.order(walker, 1600, 600, false);
        if (i == 500) w.unit(idle)->hp = Fixed::fromInt(100);
    }
    return w.stateHash();
}

static uint64_t flyingRun() {
    Types ty;
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    w.setTeam(1, 0);
    const int fac = w.spawn(&ty.factory, 1000, 1000, 0.0f, 0);
    w.train(fac, &ty.sol, 3);
    const int prod = w.spawn(&ty.flyconj, 1500, 600, 0.0f, 0);
    w.train(prod, &ty.sol, 2);
    const int hurt = w.spawn(&ty.ground, 600, 1500, 0.0f, 1);
    w.unit(hurt)->hp = Fixed::fromInt(40);
    const int repairer = w.spawn(&ty.builder, 650, 1450, 0.0f, 0);
    w.repair(repairer, hurt, false);
    std::vector<int> guards;
    for (int i = 0; i < 4; ++i) guards.push_back(w.spawn(&ty.flyer, 1200.0f + 80 * i, 1600, 0.0f, 0));
    command(w, tak::net::Cmd::Guard, guards[0], fac);
    command(w, tak::net::Cmd::Guard, guards[1], prod);
    command(w, tak::net::Cmd::Guard, guards[2], repairer);
    command(w, tak::net::Cmd::Guard, guards[3], guards[0]);
    for (int i = 0; i < 1500; ++i) {
        w.tick(1.0f / 30.0f);
        if (i == 400) w.unit(guards[0])->hp = Fixed::fromInt(60);
    }
    return w.stateHash();
}


// ---------------------------------------------------------------------------
// The ground approach: HelpBuild (0x405cf0) and RepairUnit (0x407190).
// ---------------------------------------------------------------------------
static std::unique_ptr<World> makeModeWorld(PathfindingMode mode) {
    auto w = std::make_unique<World>();
    w->setPathfindingMode(mode);
    w->setTerrain(std::vector<uint8_t>(size_t(128) * 128, 100), 128, 128, 20);
    w->setPathService(true);
    w->player(0).mana = 100000;
    w->player(1).mana = 100000;
    return w;
}

static const char* modeName(PathfindingMode mode) { return mode == PathfindingMode::Retail ? "Retail" : "Legion"; }

// A 4x4 building left unfinished (no builder of its own): 1800 ticks of
// work at workertime 10.
static UnitType hutType() {
    UnitType t;
    t.name = t.id = "hut";
    t.maxVel = Fixed(); t.maxHp = 1000; t.footX = t.footZ = 4;
    t.buildTime = 600; t.buildCost = 100;
    return t;
}

static int spawnSite(World& w, const UnitType& t, float x, float z) {
    const int id = w.spawn(&t, x, z, 0.0f, 0);
    Unit* s = w.unit(id);
    s->underConstruction = true; s->buildBegun = true;
    // Half built: nobody works it until a helper arrives, so it must outlast
    // its GetBuilt's 330 ticks and some decay (a site with no work at all is
    // removed by the first decay tick).
    s->hp = Fixed::fromInt(t.maxHp / 20 + t.maxHp * 19 / 40);
    w.blockFoot(t, x, z, true);
    return id;
}

// Footprints touching at an edge or a corner, no gap, no overlap (the
// rectangle controller's own acceptance).
static bool touching(const Unit& b, const Unit& t) {
    return retailReclaimRectangle(footprintOrigin(t.x, t.type->footX), footprintOrigin(t.z, t.type->footZ),
                                  t.type->footX, t.type->footZ, b.type->footX, b.type->footZ)
        .accepts(footprintOrigin(b.x, b.type->footX), footprintOrigin(b.z, b.type->footZ));
}

// A closed wall `thick` cells deep whose outer edge is the cells
// [x0, x1] x [z0, z1].
static void wallRing(World& w, int x0, int z0, int x1, int z1, int thick) {
    w.blockCells(x0, z0, x1 - x0 + 1, thick, true);
    w.blockCells(x0, z1 - thick + 1, x1 - x0 + 1, thick, true);
    w.blockCells(x0, z0, thick, z1 - z0 + 1, true);
    w.blockCells(x1 - thick + 1, z0, thick, z1 - z0 + 1, true);
}

static bool hasHelpLeg(const Unit& b, int site) {
    return std::any_of(b.orders.begin(), b.orders.end(), [&](const Order& o) { return o.helpSite == site; });
}

// Several helpers from four sides: each walks to the side it comes from and
// starts work only with its footprint touching the site's.
static void approachHelpersSpread(PathfindingMode mode) {
    std::printf("helpers round a site (%s):\n", modeName(mode));
    Types ty;
    const UnitType hut = hutType();
    auto ownedWorld = makeModeWorld(mode);
    World& w = *ownedWorld;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);       // storage for the pool
    const int site = spawnSite(w, hut, 1008, 1008);
    struct From { float x, z; const char* side; };
    const From from[4] = {{1008, 704, "north"}, {1008, 1312, "south"}, {1312, 1008, "east"}, {704, 1008, "west"}};
    int ids[4];
    for (int i = 0; i < 4; ++i) ids[i] = w.spawn(&ty.builder, from[i].x, from[i].z, 0.0f, 0);
    for (int id : ids) command(w, tak::net::Cmd::Assist, id, site);
    uint32_t emitted[4];
    int started[4] = {-1, -1, -1, -1};
    bool touchAtStart[4] = {}, ownSide[4] = {};
    float startDistance[4] = {};
    for (int i = 0; i < 4; ++i) emitted[i] = w.unit(ids[i])->constructionEmissions[0];
    for (int tick = 0; tick < 900; ++tick) {
        w.tick(1.0f / 30.0f);
        const Unit* s = w.unit(site);
        for (int i = 0; i < 4; ++i) {
            const Unit* b = w.unit(ids[i]);
            if (started[i] >= 0 || b->constructionEmissions[0] == emitted[i]) continue;
            started[i] = tick;
            touchAtStart[i] = touching(*b, *s);
            const float dx = (b->x - s->x).toFloat(), dz = (b->z - s->z).toFloat();
            startDistance[i] = std::sqrt(dx * dx + dz * dz);
            ownSide[i] = i == 0 ? dz < 0 && std::fabs(dz) >= std::fabs(dx) :
                         i == 1 ? dz > 0 && std::fabs(dz) >= std::fabs(dx) :
                         i == 2 ? dx > 0 && std::fabs(dx) >= std::fabs(dz) :
                                  dx < 0 && std::fabs(dx) >= std::fabs(dz);
        }
    }
    char d[200];
    for (int i = 0; i < 4; ++i) {
        std::snprintf(d, sizeof d, "first work t=%d at %.1f px from the centre, touching %d, own side %d",
                      started[i], startDistance[i], int(touchAtStart[i]), int(ownSide[i]));
        check(started[i] >= 0 && touchAtStart[i] && ownSide[i],
              (std::string("the helper from the ") + from[i].side + " works touching the site on its own side").c_str(), d);
    }
    bool apart = true;
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            apart &= dist(w, ids[i], ids[j]) >= 32.0f;
    check(apart, "...each where it arrived, none of them sharing a spot");
}

// A repairer starts repairing only with its footprint touching the target's.
static void approachRepairTouching(PathfindingMode mode) {
    std::printf("a repairer walks up to touch (%s):\n", modeName(mode));
    Types ty;
    auto ownedWorld = makeModeWorld(mode);
    World& w = *ownedWorld;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);
    const int hurt = w.spawn(&ty.sol, 1000, 1000, 0.0f, 0);
    w.unit(hurt)->hp = Fixed::fromInt(100);
    const int rep = w.spawn(&ty.builder, 1300, 1000, 0.0f, 0);
    w.repair(rep, hurt, false);
    int first = -1;
    bool touch = false;
    float at = 0;
    for (int tick = 0; tick < 600 && first < 0; ++tick) {
        const Fixed hp = w.unit(hurt)->hp;
        w.tick(1.0f / 30.0f);
        if (w.unit(hurt)->hp > hp) { first = tick; touch = touching(*w.unit(rep), *w.unit(hurt)); at = dist(w, rep, hurt); }
    }
    char d[160];
    std::snprintf(d, sizeof d, "first repair t=%d at %.1f px (builddistance 60), touching %d", first, at, int(touch));
    check(first >= 0 && touch, "the first repair is done with the footprints touching", d);
    int mended = -1;
    for (int tick = 0; tick < 600 && mended < 0; ++tick) {
        w.tick(1.0f / 30.0f);
        if (w.unit(hurt)->hp.toFloat() >= 299.9f) mended = tick;
    }
    check(mended >= 0 && !w.unit(rep)->repairId && w.unit(rep)->orders.empty(), "...to full health, then the job ends");
}

// A helper that makes no headway for longer than 8 s keeps its job.
static void approachStuckHelper() {
    std::printf("a held-up helper keeps trying:\n");
    Types ty;
    const UnitType hut = hutType();
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);
    const int site = spawnSite(w, hut, 1008, 1008);
    const int helper = w.spawn(&ty.builder, 1008, 600, 0.0f, 0);
    command(w, tak::net::Cmd::Assist, helper, site);
    w.unit(helper)->paralyzedFor = 420;            // 14 s without a step
    int lost = -1;
    for (int tick = 0; tick < 420 && lost < 0; ++tick) {
        w.tick(1.0f / 30.0f);
        const Unit* b = w.unit(helper);
        if (b->buildSiteId != site || !hasHelpLeg(*b, site)) lost = tick;
    }
    char d[120];
    std::snprintf(d, sizeof d, "job lost at t=%d (-1: kept)", lost);
    check(lost < 0, "a helper held still for 14 s keeps its help (no 8 s give-up)", d);
    const uint32_t before = w.unit(helper)->constructionEmissions[0];
    for (int tick = 0; tick < 600 && w.unit(helper)->constructionEmissions[0] == before; ++tick) w.tick(1.0f / 30.0f);
    check(w.unit(helper)->constructionEmissions[0] != before && touching(*w.unit(helper), *w.unit(site)),
          "...and helps once it can move again");
}

// Once working, a builder pushed away keeps working: the work stages never
// measure distance.
static void approachBumped() {
    std::printf("a bumped builder keeps working:\n");
    Types ty;
    const UnitType hut = hutType();
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);
    const int site = spawnSite(w, hut, 1008, 1008);
    const int helper = w.spawn(&ty.builder, 1008, 704, 0.0f, 0);
    command(w, tak::net::Cmd::Assist, helper, site);
    uint32_t emitted = w.unit(helper)->constructionEmissions[0];
    for (int tick = 0; tick < 600 && w.unit(helper)->constructionEmissions[0] == emitted; ++tick) w.tick(1.0f / 30.0f);
    check(w.unit(helper)->constructionEmissions[0] != emitted, "a helper starts working on the site");
    w.unit(helper)->z = w.unit(helper)->z - Fixed::fromInt(200);   // shoved 200 px off
    const Fixed bumpedZ = w.unit(helper)->z;
    emitted = w.unit(helper)->constructionEmissions[0];
    const Fixed hp = w.unit(site)->hp;
    for (int tick = 0; tick < 30; ++tick) w.tick(1.0f / 30.0f);
    char d[160];
    std::snprintf(d, sizeof d, "%u work ticks in 30, site hp %.1f -> %.1f, moved %.1f px",
                  w.unit(helper)->constructionEmissions[0] - emitted, hp.toFloat(), w.unit(site)->hp.toFloat(),
                  std::fabs((w.unit(helper)->z - bumpedZ).toFloat()));
    check(w.unit(helper)->constructionEmissions[0] - emitted == 30 && w.unit(site)->hp > hp && w.unit(helper)->z == bumpedZ,
          "...and pushed 200 px away it keeps working where it stands", d);

    const int hurt = w.spawn(&ty.sol, 1600, 1600, 0.0f, 0);
    w.unit(hurt)->hp = Fixed::fromInt(30);
    const int rep = w.spawn(&ty.builder, 1600, 1700, 0.0f, 0);
    w.repair(rep, hurt, false);
    for (int tick = 0; tick < 600 && w.unit(hurt)->hp == Fixed::fromInt(30); ++tick) w.tick(1.0f / 30.0f);
    w.unit(rep)->x = w.unit(rep)->x + Fixed::fromInt(200);
    const Fixed before = w.unit(hurt)->hp;
    for (int tick = 0; tick < 30; ++tick) w.tick(1.0f / 30.0f);
    std::snprintf(d, sizeof d, "target hp %.1f -> %.1f, repair job %d", before.toFloat(), w.unit(hurt)->hp.toFloat(),
                  w.unit(rep)->repairId);
    check(w.unit(hurt)->hp > before + Fixed::fromInt(30) && w.unit(rep)->repairId == hurt,
          "a repairer pushed 200 px away keeps repairing", d);
}

// A failed approach (the site walled off): within builddistance the work
// starts from where the route ended; out of it the help ends, unworked.
static void approachFailed() {
    std::printf("a helper whose approach fails:\n");
    Types ty;
    // The help gives up after some 1200 ticks of walking, which do not keep
    // the site: a slow build decays slowly enough to outlast them.
    UnitType hut = hutType(); hut.buildTime = 6000;
    char d[160];
    for (const bool inReach : {true, false}) {
        auto ownedWorld = makeWorld();
        World& w = *ownedWorld;
        w.spawn(&ty.factory, 300, 300, 0.0f, 0);
        const int site = spawnSite(w, hut, 1008, 1008);            // cells 61..64
        // Outside a wall ending two cells out the builder stands 96 px from
        // the centre (96 - 22 - 45 = 29 <= 60); seven cells out, 176 px
        // (109 > 60).
        if (inReach) wallRing(w, 58, 58, 67, 67, 2); else wallRing(w, 53, 53, 72, 72, 3);
        const int helper = w.spawn(&ty.builder, 1400, 1008, 0.0f, 0);
        command(w, tak::net::Cmd::Assist, helper, site);
        const Fixed hp = w.unit(site)->hp;
        int worked = -1, ended = -1;
        for (int tick = 0; tick < 3000 && worked < 0 && ended < 0; ++tick) {
            const uint32_t before = w.unit(helper)->constructionEmissions[0];
            w.tick(1.0f / 30.0f);
            if (w.unit(helper)->constructionEmissions[0] != before) worked = tick;
            if (!w.unit(helper)->buildSiteId) ended = tick;
        }
        std::snprintf(d, sizeof d, "worked t=%d, ended t=%d, %.1f px from the centre", worked, ended, dist(w, helper, site));
        if (inReach)
            check(worked >= 0 && ended < 0, "within builddistance of the site it works from outside the wall", d);
        else
            check(ended >= 0 && worked < 0 && w.unit(site)->hp <= hp && !hasHelpLeg(*w.unit(helper), site),
                  "out of builddistance the help ends, the site unworked (decaying)", d);
    }
    // A still repair target it cannot reach is never given up on.
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);
    const int hurt = w.spawn(&ty.sol, 1008, 1008, 0.0f, 0);       // cells 62..63
    w.unit(hurt)->hp = Fixed::fromInt(50);
    wallRing(w, 56, 56, 69, 69, 3);
    const int rep = w.spawn(&ty.builder, 1400, 1008, 0.0f, 0);
    w.repair(rep, hurt, false);
    bool kept = true;
    for (int tick = 0; tick < 900; ++tick) {
        w.tick(1.0f / 30.0f);
        kept &= w.unit(rep)->repairId == hurt || tick == 0;
    }
    check(kept && w.unit(hurt)->hp == Fixed::fromInt(50), "a still repair target it cannot reach is retried, never given up");
}

// RepairUnit and a moving target: no repair while it moves; a short walk
// pauses the repair, a long one ends it after the looks.
static void approachMovingRepair(PathfindingMode mode) {
    std::printf("repairing a unit that walks (%s):\n", modeName(mode));
    Types ty;
    UnitType stepper = ty.sol;                     // starts and stops at once
    stepper.name = stepper.id = "stepper";
    stepper.accel = stepper.brake = Fixed::fromInt(2);
    char d[200];
    for (const bool far : {false, true}) {
        auto ownedWorld = makeModeWorld(mode);
        World& w = *ownedWorld;
        w.spawn(&ty.factory, 300, 300, 0.0f, 0);
        const int hurt = w.spawn(&stepper, 1000, 600, 0.0f, 0);
        w.unit(hurt)->hp = Fixed::fromInt(50);
        const int rep = w.spawn(&ty.builder, 1000, 640, 0.0f, 0);
        w.repair(rep, hurt, false);
        for (int tick = 0; tick < 300 && w.unit(hurt)->hp == Fixed::fromInt(50); ++tick) w.tick(1.0f / 30.0f);
        const bool began = w.unit(hurt)->hp > Fixed::fromInt(50);
        w.order(hurt, 1000, far ? 2400.0f : 560.0f, false);
        Fixed hp = w.unit(hurt)->hp;
        bool healedMoving = false;
        int ended = -1, mended = -1;
        for (int tick = 0; tick < 900 && ended < 0; ++tick) {
            const bool moving = w.unit(hurt)->speed != Fixed();
            w.tick(1.0f / 30.0f);
            healedMoving |= moving && w.unit(hurt)->hp > hp;
            hp = w.unit(hurt)->hp;
            if (mended < 0 && hp.toFloat() >= 299.9f) mended = tick;
            if (!w.unit(rep)->repairId) ended = tick;
        }
        const Unit* t = w.unit(hurt);
        const Unit* r = w.unit(rep);
        std::snprintf(d, sizeof d, "began %d, ended t=%d, hp %.1f, target at z %.0f, %zu orders left",
                      int(began), ended, t->hp.toFloat(), t->z.toFloat(), r->orders.size());
        check(began && !healedMoving, far ? "no repair is done while the target walks away" :
                                            "no repair is done while the target steps aside", d);
        if (far)
            check(ended >= 0 && ended < 300 && t->z.toFloat() < 2300.0f && r->orders.empty() && t->hp.toFloat() < 299.9f,
                  "...and after a few looks at it walking the repair ends, unfinished", d);
        else
            check(mended >= 0 && ended >= mended, "...and once it stands still the repair resumes and finishes", d);
    }
}

// A patrol's repair detour ends once the builder itself is its sightdistance
// from where it left the patrol (mission +0x56 against +0x2e/+0x30, >=);
// the work never measures how far the target is.
static void approachPatrolLeash() {
    std::printf("a patrol's repair leash:\n");
    Types ty;
    UnitType bt = ty.builder;
    bt.name = bt.id = "patroller"; bt.sight = 200; bt.canPatrol = true;
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);
    const int b = w.spawn(&bt, 600, 1000, 0.0f, 0);
    const int hurt = w.spawn(&ty.sol, 700, 1060, 0.0f, 0);
    w.unit(hurt)->hp = Fixed::fromInt(30);
    w.patrol(b, 1600, 1000);
    for (int tick = 0; tick < 600 && w.unit(hurt)->hp == Fixed::fromInt(30); ++tick) w.tick(1.0f / 30.0f);
    const Order* job = nullptr;
    for (const auto& o : w.unit(b)->orders) if (o.patrolRepair) job = &o;
    check(job && w.unit(b)->repairId == hurt && w.unit(hurt)->hp > Fixed::fromInt(30), "a patrolling builder repairs a damaged unit it passes");
    if (!job) return;
    const Fixed ox = job->clickX, oz = job->clickZ;
    w.unit(b)->x = ox + Fixed::fromInt(199); w.unit(b)->z = oz;
    Fixed hp = w.unit(hurt)->hp;
    w.tick(1.0f / 30.0f);
    char d[160];
    std::snprintf(d, sizeof d, "target %.0f px from the leash origin", std::hypot((w.unit(hurt)->x - ox).toFloat(), (w.unit(hurt)->z - oz).toFloat()));
    check(w.unit(b)->repairId == hurt && w.unit(hurt)->hp > hp, "199 px from where it left the patrol it keeps repairing", d);
    w.unit(b)->x = ox + Fixed::fromInt(200);
    hp = w.unit(hurt)->hp;
    w.tick(1.0f / 30.0f);
    const Unit* u = w.unit(b);
    check(!u->repairId && w.unit(hurt)->hp == hp && !u->orders.empty() && u->orders.front().patrol,
          "200 px away (its sightdistance) the repair ends and the patrol resumes", d);
}

// An Assist queued behind a move is the job only once its leg is current.
static void approachQueuedAssist() {
    std::printf("a queued assist:\n");
    Types ty;
    const UnitType hut = hutType();
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);
    const int site = spawnSite(w, hut, 1008, 1008);
    const int b = w.spawn(&ty.builder, 1008, 1500, 0.0f, 0);
    w.order(b, 1500, 1500, false);
    tak::net::Command c;
    c.kind = tak::net::Cmd::Assist; c.player = 0; c.unitId = b; c.targetId = site; c.queue = 1;
    applyCommand(w, emptyRegistry(), c);
    bool early = w.unit(b)->buildSiteId != 0;
    int latched = -1;
    for (int tick = 0; tick < 900 && latched < 0; ++tick) {
        w.tick(1.0f / 30.0f);
        if (w.unit(b)->buildSiteId == site) latched = tick;
        else early |= w.unit(b)->buildSiteId != 0;
    }
    char d[120];
    std::snprintf(d, sizeof d, "latched t=%d, builder at (%.0f,%.0f)", latched, w.unit(b)->x.toFloat(), w.unit(b)->z.toFloat());
    check(!early && latched >= 0 && w.unit(b)->x.toFloat() > 1400, "the help starts only once the move before it is done", d);
    const uint32_t before = w.unit(b)->constructionEmissions[0];
    for (int tick = 0; tick < 900 && w.unit(b)->constructionEmissions[0] == before; ++tick) w.tick(1.0f / 30.0f);
    check(w.unit(b)->constructionEmissions[0] != before && touching(*w.unit(b), *w.unit(site)), "...then it walks over and helps");

    // An assist queued on the site the builder is building itself waits its
    // turn: the build goes on (the latch is the MobileBuild's), and the help
    // ends with the finished site.
    auto ownWorld = makeWorld();
    World& o = *ownWorld;
    o.spawn(&ty.factory, 300, 300, 0.0f, 0);
    UnitType own = hut; own.buildTime = 60;
    const int ob = o.spawn(&ty.builder, 1008, 1200, 0.0f, 0);
    o.queueBuild(ob, &own, 1008, 1008, false);
    int ownSite = 0;
    for (int tick = 0; tick < 600 && !ownSite; ++tick) { o.tick(1.0f / 30.0f); ownSite = o.unit(ob)->buildSiteId; }
    for (int tick = 0; tick < 30; ++tick) o.tick(1.0f / 30.0f);
    o.assist(ob, ownSite, true);
    const float hp0 = ownSite ? o.unit(ownSite)->hp.toFloat() : 0;
    for (int tick = 0; tick < 30; ++tick) o.tick(1.0f / 30.0f);
    const float hp1 = ownSite ? o.unit(ownSite)->hp.toFloat() : 0;
    bool finished = false;
    for (int tick = 0; tick < 900 && ownSite && !finished; ++tick) {
        o.tick(1.0f / 30.0f);
        finished = !o.unit(ownSite)->underConstruction;
    }
    for (int tick = 0; tick < 5; ++tick) o.tick(1.0f / 30.0f);
    std::snprintf(d, sizeof d, "site %d hp %.1f -> %.1f, finished %d, %zu orders left", ownSite, hp0, hp1,
                  int(finished), o.unit(ob)->orders.size());
    check(ownSite && hp1 > hp0 && finished && o.unit(ob)->orders.empty() && !o.unit(ob)->buildSiteId,
          "an assist queued on its own build site lets the build go on", d);
}

// The step out of a factory's yard after a help is for a builder with
// nothing else to do: a repeating conjurer's Move is its output's rally
// (World::order), which the step-out must not overwrite.
static void approachYardRepeat() {
    std::printf("a repeating conjurer helping inside a factory's yard:\n");
    Types ty;
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);
    w.spawn(&ty.factory, 1000, 1000, 0.0f, 0);
    w.blockFoot(ty.factory, 1000, 1000, false);   // a walkable yard
    UnitType small = ty.sol; small.name = small.id = "small"; small.buildTime = 6;
    const int site = w.spawn(&small, 1000, 1000, 0.0f, 0);
    w.unit(site)->underConstruction = true; w.unit(site)->buildBegun = true; w.unit(site)->hp = Fixed::fromInt(50);
    const int conj = w.spawn(&ty.conjurer, 1000, 1032, 0.0f, 0);   // touching it, in the yard
    w.setRepeat(conj, &ty.sol);
    tak::net::Command mv; mv.kind = tak::net::Cmd::Move; mv.unitId = conj; mv.x = 200; mv.z = 1800;
    applyCommand(w, emptyRegistry(), mv);
    w.tick(1.0f / 30.0f);
    w.assist(conj, site, false);
    int ended = -1;
    for (int tick = 0; tick < 600 && ended < 0; ++tick) {
        w.tick(1.0f / 30.0f);
        if (!w.unit(conj)->buildSiteId) ended = tick;
    }
    const Unit* c = w.unit(conj);
    const bool kept = c->rally.size() == 1 && c->rally.back().x == Fixed::fromInt(200) && c->rally.back().z == Fixed::fromInt(1800);
    char d[120];
    std::snprintf(d, sizeof d, "help ended t=%d, rally (%.0f,%.0f)", ended,
                  c->rally.empty() ? 0.0f : c->rally.back().x.toFloat(), c->rally.empty() ? 0.0f : c->rally.back().z.toFloat());
    check(ended >= 0 && !w.unit(site)->underConstruction && kept, "its rally is kept when the help ends", d);
}

static uint64_t approachRun() {
    Types ty;
    const UnitType hut = hutType();
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    w.spawn(&ty.factory, 300, 300, 0.0f, 0);
    const int site = spawnSite(w, hut, 1008, 1008);
    for (int i = 0; i < 6; ++i) command(w, tak::net::Cmd::Assist, w.spawn(&ty.builder, 700.0f + 120 * i, 700, 0.0f, 0), site);
    const int hurt = w.spawn(&ty.sol, 1500, 1500, 0.0f, 0);
    w.unit(hurt)->hp = Fixed::fromInt(40);
    const int rep = w.spawn(&ty.builder, 1500, 1650, 0.0f, 0);
    w.repair(rep, hurt, false);
    for (int i = 0; i < 1200; ++i) {
        w.tick(1.0f / 30.0f);
        if (i == 120) w.order(hurt, 1700, 1500, false);
        if (i == 500) w.repair(rep, hurt, false);
    }
    return w.stateHash();
}

// ---------------------------------------------------------------------------
// Determinism: the same mixed scenario twice gives the same hash.
// ---------------------------------------------------------------------------
static uint64_t mixedRun() {
    Types ty;
    auto ownedWorld = makeWorld();
    World& w = *ownedWorld;
    const int fac = w.spawn(&ty.factory, 1000, 1000, 0.0f, 0);
    w.train(fac, &ty.sol, 4);
    const int conj = w.spawn(&ty.conjurer, 1500, 1000, 0.0f, 0);
    w.train(conj, &ty.sol, 3);
    std::vector<int> guards;
    for (int i = 0; i < 3; ++i) guards.push_back(w.spawn(&ty.builder, 1100.0f + 60 * i, 1400, 0.0f, 0));
    const int helper = w.spawn(&ty.builder, 1500, 1200, 0.0f, 0);
    command(w, tak::net::Cmd::Guard, guards[0], fac);
    command(w, tak::net::Cmd::Guard, guards[1], conj);
    command(w, tak::net::Cmd::Guard, guards[2], guards[0]);
    for (int i = 0; i < 1500; ++i) {
        w.tick(1.0f / 30.0f);
        if (i % 97 == 0 && w.unit(conj)->productionSiteId) w.assist(helper, w.unit(conj)->productionSiteId, false);
    }
    return w.stateHash();
}

static void determinism() {
    std::printf("determinism:\n");
    const uint64_t a = mixedRun(), b = mixedRun();
    char d[64];
    std::snprintf(d, sizeof d, "%016llx / %016llx", (unsigned long long)a, (unsigned long long)b);
    check(a == b, "guard-help, assist and shared production hash the same twice", d);
    const uint64_t c = flyingRun(), e = flyingRun();
    std::snprintf(d, sizeof d, "%016llx / %016llx", (unsigned long long)c, (unsigned long long)e);
    check(c == e, "flying guards' help and repair hash the same twice", d);
    const uint64_t f = orbitRun(), h = orbitRun();
    std::snprintf(d, sizeof d, "%016llx / %016llx", (unsigned long long)f, (unsigned long long)h);
    check(f == h, "flying guards' orbits hash the same twice", d);
    const uint64_t k = defendRunHash(), m = defendRunHash();
    std::snprintf(d, sizeof d, "%016llx / %016llx", (unsigned long long)k, (unsigned long long)m);
    check(k == m, "guards answering attackers hash the same twice", d);
    const uint64_t n = approachRun(), p = approachRun();
    std::snprintf(d, sizeof d, "%016llx / %016llx", (unsigned long long)n, (unsigned long long)p);
    check(n == p, "ground help and repair approaches hash the same twice", d);
}

int main() {
    sharedProduction(false);
    sharedProduction(true);
    outputOrdersOnce(false);
    outputOrdersOnce(true);
    eligibility();
    repeatConjurer();
    guardHelps();
    flyingGuardHelps();
    flyingGuardOrbit();
    flyingGuardStarts();
    guardDefends();
    for (const PathfindingMode mode : {PathfindingMode::Retail, PathfindingMode::Legion}) {
        approachHelpersSpread(mode);
        approachRepairTouching(mode);
        approachMovingRepair(mode);
    }
    approachStuckHelper();
    approachBumped();
    approachFailed();
    approachPatrolLeash();
    approachQueuedAssist();
    approachYardRepeat();
    determinism();
    std::printf(g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
