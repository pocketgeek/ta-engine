// site_decay_test -- an abandoned construction site decays as retail's does,
// in every game (KINGDOMS.icd, emulated by tools/re/check_site_decay.py).
//
// The site's own GetBuilt (402220) sleeps 300 ticks when it is new and 30 after
// any work event; a timer wake with no work since runs 429d70 -> 429af0(site,
// site, -0.5f) once a tick: 0.5 / buildtime of the build comes off the site and
// float(that * buildcost) goes back to the SITE's owner. When no work would be
// left the site dies instead (kind 11, its own hitter, no refund). Ours used to
// bleed hp at the rate the site was last built from the first tick nobody
// worked it, and gave nothing back.
//
// Synthetic types on a flat map.

#include "sim/sim.h"

#include <cstdio>
#include <vector>

using namespace tak::sim;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

struct Types {
    UnitType builder, hut, factory, sol;
    Types() {
        builder.id = builder.name = "builder";
        builder.maxVel = Fixed::fromInt(2); builder.turnRate = builder.turnInPlaceRate = 10000;
        builder.canMove = true; builder.maxHp = 300; builder.footX = builder.footZ = 2;
        builder.isBuilder = true; builder.workerTime = 100; builder.buildDist = 80;
        builder.storage = 100000; builder.upright = true;

        hut.id = hut.name = "hut"; hut.maxHp = 2000; hut.maxVel = {}; hut.footX = hut.footZ = 2;
        hut.buildTime = 300; hut.buildCost = 600;

        factory.id = factory.name = "factory";
        factory.maxVel = Fixed(); factory.maxHp = 5000; factory.footX = factory.footZ = 6;
        factory.isBuilder = true; factory.workerTime = 10; factory.storage = 100000; factory.buildMenu = {"sol"};

        sol = builder; sol.id = sol.name = "sol"; sol.isBuilder = false; sol.storage = 0;
        sol.buildTime = 60; sol.buildCost = 300; sol.maxHp = 1000;
    }
};

struct Rig {
    World w;
    Rig() {
        w.setSerialThreads(true);
        w.setTerrain(std::vector<uint8_t>(size_t(128) * 128, 100), 128, 128, 20);
        w.setPathService(true);
        w.setPlayerCount(2);
        w.player(0).mana = 50000;
    }
    void ticks(int n) { for (int i = 0; i < n; ++i) w.tick(1.f / 30); }
};

// What one decay tick gives back and takes off, as 429af0 computes it.
double refund(const UnitType& t) {
    const double progress = 0.5 * double(float(1.0 / double(t.buildTime)));
    return double(float(progress * double(t.buildCost)));
}
Fixed hpDrop(const UnitType& t) {
    return Fixed::fromFloat(float(double(t.maxHp) * 0.95 * 0.5 * double(float(1.0 / double(t.buildTime)))));
}

// A worked site, abandoned: 30 quiet ticks after the last work, then a fixed
// slice every tick with its mana back, until it dies with nothing left.
void abandoned() {
    Types t; Rig r;
    const int id = r.w.spawn(&t.builder, 600, 1000, 0, 0);
    const double before = r.w.player(0).mana;
    r.w.queueBuild(id, &t.hut, 660, 1000, false);
    int site = 0;
    for (int i = 0; i < 900 && !(site && r.w.unit(site)->buildBegun); ++i) {
        r.ticks(1); site = r.w.unit(id)->buildSiteId;
    }
    r.ticks(20);
    const double paid = before - r.w.player(0).mana;
    check(site && r.w.unit(site)->underConstruction && paid > 0, "the builder works the site");
    r.w.cancelBuilds(id);
    const Fixed hp = r.w.unit(site)->hp;
    const double mana = r.w.player(0).mana;
    r.ticks(29);
    check(r.w.unit(site)->hp == hp && r.w.player(0).mana == mana,
          "29 ticks after its last work the site is untouched");
    r.ticks(1);
    check(r.w.unit(site)->hp == hp - hpDrop(t.hut) && r.w.player(0).mana == mana + refund(t.hut),
          "the 30th decays it by 0.5/buildtime and gives that much of its cost back");
    r.ticks(1);
    check(r.w.unit(site)->hp == hp - hpDrop(t.hut) - hpDrop(t.hut) && r.w.player(0).mana == mana + 2 * refund(t.hut),
          "and so on every tick");
    int steps = 2;
    while (r.w.unit(site)->alive() && steps < 5000) { r.ticks(1); ++steps; }
    const Unit* s = r.w.unit(site);
    const double back = r.w.player(0).mana - mana;
    check(s && !s->alive() && s->deathType == 11 && s->lastHitBy == site,
          "with no work left it dies of kind 11, its own hitter");
    check(back <= paid && back > paid - 2 * refund(t.hut),
          "the refunds come to what was paid, less the last slice");
}

// The worker in a later unit slot than the site: retail dispatches the site
// before it each tick, so the site's GetBuilt hears of the last work a tick
// late and the decay starts on the 31st tick after it, not the 30th.
void laterSlot() {
    Types t; Rig r;
    const int site = r.w.spawn(&t.hut, 660, 1000, 0, 0);
    Unit* s = r.w.unit(site);
    s->underConstruction = s->buildBegun = true;
    s->hp = Fixed::fromInt(1000);
    r.w.blockFoot(t.hut, 660, 1000, true);
    const int id = r.w.spawn(&t.builder, 600, 1000, 0, 0);
    check(id > site, "later slot: the helper comes after the site");
    r.w.assist(id, site, false);
    Fixed last = r.w.unit(site)->hp;
    int worked = 0;
    for (int i = 0; i < 300 && worked < 20; ++i) {
        r.ticks(1);
        if (r.w.unit(site)->hp > last) ++worked;
        last = r.w.unit(site)->hp;
    }
    check(worked >= 20 && r.w.unit(site)->underConstruction, "later slot: the helper works the site");
    r.w.cancelBuilds(id);
    const double mana = r.w.player(0).mana;
    r.ticks(30);
    check(r.w.unit(site)->hp == last && r.w.player(0).mana == mana,
          "later slot: 30 ticks after its last work the site is untouched");
    r.ticks(1);
    check(r.w.unit(site)->hp == last - hpDrop(t.hut) && r.w.player(0).mana == mana + refund(t.hut),
          "later slot: the 31st decays it");
}

// A site nobody has worked: 300 ticks, then 30 more, then the decay.
void neverWorked() {
    Types t; Rig r;
    r.w.spawn(&t.builder, 300, 1000, 0, 0);   // storage for the pool
    const int site = r.w.spawn(&t.hut, 600, 1000, 0, 0);
    Unit* s = r.w.unit(site);
    s->underConstruction = s->buildBegun = true;
    s->hp = Fixed::fromInt(1000);
    const double mana = r.w.player(0).mana;
    r.ticks(330);
    check(r.w.unit(site)->hp == Fixed::fromInt(1000) && r.w.player(0).mana == mana,
          "an unworked site waits 330 ticks");
    r.ticks(1);
    check(r.w.unit(site)->hp == Fixed::fromInt(1000) - hpDrop(t.hut) && r.w.player(0).mana == mana + refund(t.hut),
          "then decays");
}

// A site that changes sides is a new object, created finished, that takes
// the old one's remaining work: it has no GetBuilt at all.
void changedSides() {
    Types t; Rig r;
    r.w.spawn(&t.builder, 300, 1000, 0, 1);   // storage for the new owner
    const int site = r.w.spawn(&t.hut, 600, 1000, 0, 0);
    Unit* s = r.w.unit(site);
    s->underConstruction = s->buildBegun = true;
    s->hp = Fixed::fromInt(1000);
    r.ticks(200);
    check(r.w.scenarioTransfer(site, 1), "changed sides: the site goes over");
    const double mana = r.w.player(1).mana;
    // 514da0 makes the new object finished and copies the remaining work
    // over after: it carries no GetBuilt, so nothing ever decays it.
    r.ticks(1200);
    const Unit* now = r.w.unit(site);
    check(now && now->alive() && now->player == 1 && now->underConstruction && now->hp == Fixed::fromInt(1000),
          "changed sides: the new object has no GetBuilt and never decays");
    check(r.w.player(1).mana == mana, "changed sides: no refund either");
}

// A factory's output that its factory stops working is HELD, not decayed:
// our choice, not retail's (whose GetBuilt would decay it once the work
// stops). It keeps its progress and costs nothing back or again. Only an
// output its producer no longer holds decays, 30 ticks after it let go.
void factoryOutput() {
    Types t; Rig r;
    const int f = r.w.spawn(&t.factory, 600, 1000, 0, 0);
    r.w.train(f, &t.sol, 1);
    int site = 0;
    for (int i = 0; i < 300 && !site; ++i) { r.ticks(1); site = r.w.unit(f)->productionSiteId; }
    r.ticks(100);
    check(site && r.w.unit(site)->underConstruction, "the factory works its output");
    const int32_t progress = r.w.unit(f)->buildProgress;
    const Fixed hp = r.w.unit(site)->hp;
    const double mana = r.w.player(0).mana;
    r.w.unit(f)->paralyzedFor = 400;
    r.ticks(360);
    const Unit* held = r.w.unit(site);
    const bool kept = held && held->alive() && held->hp == hp && r.w.unit(f)->productionSiteId == site;
    check(kept && r.w.unit(f)->buildProgress == progress && r.w.player(0).mana == mana,
          "a stopped factory's output is held: no decay, no refund");
    if (!kept) return;
    r.w.unit(f)->productionSiteId = 0;   // the producer lets go of it
    r.ticks(29);
    check(r.w.unit(site)->hp == hp && r.w.player(0).mana == mana,
          "an output its producer let go of waits 30 ticks");
    r.ticks(1);
    check(r.w.unit(site)->hp == hp - hpDrop(t.sol) && r.w.player(0).mana == mana + refund(t.sol),
          "then decays as any site");
}
}  // namespace

int main() {
    abandoned();
    laterSlot();
    neverWorked();
    changedSides();
    factoryOutput();
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
