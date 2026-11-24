// reclaim_unit_test -- TotalA.exe's RECLAIMUNIT: a reclaimer taking a live unit
// apart (ReclaimUnit 0x404730, VTOL_ReclaimUnit 0x414a80; World::reclaimUnit,
// tickReclaimUnit).
//
//  - the rate 0x438650 and the permission 0x489960, against the numbers
//    tools/re/reclaimunit_emu.py reads off the retail routines;
//  - Cmd::ReclaimUnit: the order, its Shift toggle and its Ctrl front insert;
//  - a ground reclaimer walks up, waits for its stance and deals a kind-5
//    record of the rate every 16 ticks; the target dies of a reclaim and the
//    reclaimer's player is paid (1 - remaining) x buildcostmetal (0x486c9e);
//  - a commander (cancapture) or an airborne flyer cannot be reclaimed (8);
//  - a target carried out of reach stops the work, which starts over (stage
//    0 after 15 ticks) and follows it;
//  - a flyer takes off, flies over the target and reclaims it from within
//    builddistance.

#include "net/protocol.h"
#include "server/commands.h"
#include "sim/matchsetup.h"
#include "sim/retailreclaimunit.h"
#include "sim/sim.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace ta;
using sim::Fixed;

static int g_fail = 0;
static void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  %-70s %s%s%s\n", what, ok ? "ok" : "FAIL", detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_fail;
}

namespace {
struct Types {
    sim::UnitType con, victim, boss, plane, flyCon, soldier;
    Types() {
        auto mover = [](const char* id) {
            sim::UnitType t;
            t.id = t.name = id; t.maxHp = 100; t.canMove = true;
            t.maxVel = Fixed::fromInt(2); t.turnRate = t.turnInPlaceRate = 10000;
            t.footX = t.footZ = 2; t.buildTime = 1; t.sight = 500; t.upright = true;
            return t;
        };
        // A construction vehicle: canreclamate, workertime 100, builddistance 100.
        con = mover("con"); con.isBuilder = true; con.canReclaim = true; con.workerTime = 100; con.buildDist = 100;
        // 100 x 1 x 500 x 15 / (100 x 300) = 25 a record.
        victim = mover("victim"); victim.maxHp = 500; victim.buildCostMetal = 100; victim.buildCostEnergy = 900;
        boss = mover("boss"); boss.maxHp = 500; boss.canCapture = true;   // a commander
        plane = mover("plane"); plane.canFly = true; plane.cruiseAlt = 80;
        plane.maxVel = Fixed::fromInt(4); plane.accel = plane.brake = Fixed::fromInt(1);
        flyCon = plane; flyCon.id = flyCon.name = "flycon"; flyCon.isBuilder = true; flyCon.canReclaim = true;
        flyCon.workerTime = 100; flyCon.buildDist = 60;
        soldier = mover("soldier");
    }
};

struct Scene {
    std::unique_ptr<sim::World> w = std::make_unique<sim::World>();
    sim::TypeRegistry reg;
    Scene() {
        w->setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        w->setMapPlacementFeatures(std::vector<uint16_t>(64 * 64, 0xffff), {});
        w->setPathService(true);
    }
    void tick(int n = 1) { for (int i = 0; i < n; ++i) w->tick(1.0f / 30); }
    void command(net::Cmd kind, int unit, int target, uint8_t queue = 0) {
        net::Command c; c.kind = kind; c.player = 0; c.unitId = unit; c.targetId = target; c.queue = queue;
        sim::applyCommand(*w, reg, c);
    }
    bool reclaimLeg(int id, int target) const {
        const auto* u = w->unit(id);
        return u && !u->orders.empty() && u->orders.back().reclaimUnit == target && u->orders.back().reclaimFeat == -target;
    }
};
}

// The rate and the permission (the numbers reclaimunit_emu.py reads off
// 0x438650 and 0x489960).
static void arithmetic() {
    std::printf("the rate 0x438650 and the reach:\n");
    struct Row { uint16_t wt, kills; int32_t md; float bcm; int32_t want; };
    const Row rows[] = {{80, 0, 500, 100, 20}, {300, 0, 2000, 55, 545}, {100, 12, 450, 5, 675},
                        {60, 0, 26000, 2400, 32}, {1, 0, 10, 1000, 1}, {200, 4, 3000, 10, 3000},
                        {65535, 0, 1000000, 3, 1257485}};
    bool all = true;
    for (const Row& r : rows) all &= sim::retailReclaimUnitRate(r.wt, r.kills, r.md, r.bcm) == r.want;
    check(all, "retailReclaimUnitRate matches the emulated 0x438650 (32-bit product included)");
    check(sim::retailReclaimUnitRadius(2, 2) == 21 && sim::retailReclaimUnitRadius(4, 4) == 42 &&
          sim::retailReclaimUnitRadius(1, 1) == 10, "the target type's +0x184: (sx + sz) / 3 in whole px");
    const int32_t px = 65536;
    check(sim::retailReclaimUnitInReach(100 * px, 0, 100) && !sim::retailReclaimUnitInReach(101 * px, 0, 100) &&
          sim::retailReclaimUnitInReach(60 * px, 80 * px, 100) && !sim::retailReclaimUnitInReach(61 * px, 80 * px, 100),
          "the reach: whole-pixel squares against reach squared");
    check(sim::retailReclaimCredit(0, 0, 100, 1) == 100 && sim::retailReclaimCredit(10, 0.25f, 100, 1) == 85 &&
          sim::retailReclaimCredit(0, 0, 100, 0.5f) == 50 && sim::retailReclaimCredit(0, 0, 100, 0.7f) == 70,
          "0x486c9e: (1 - remaining) x buildcostmetal, x 0.5 / 0.7 for an easy / medium AI");
}

static void commands(const Types& t) {
    std::printf("Cmd::ReclaimUnit:\n");
    Scene s;
    const int con = s.w->spawn(&t.con, 200, 300, 0, 0);
    const int soldier = s.w->spawn(&t.soldier, 200, 400, 0, 0);
    const int a = s.w->spawn(&t.victim, 600, 300, 0, 1);
    const int b = s.w->spawn(&t.victim, 600, 500, 0, 1);
    const int own = s.w->spawn(&t.victim, 300, 500, 0, 0);
    s.command(net::Cmd::ReclaimUnit, con, a);
    check(s.w->unit(con)->orders.size() == 1 && s.reclaimLeg(con, a), "a reclaimer gets one RECLAIMUNIT leg on the unit");
    s.w->order(soldier, 600, 600, false);
    s.command(net::Cmd::ReclaimUnit, soldier, a);
    check(s.w->unit(soldier)->orders.size() == 1 && !s.w->unit(soldier)->orders.front().reclaimUnit,
          "a unit that cannot reclaim keeps its own order");
    const uint8_t shift = net::kQueueAppend | net::kQueueToggle, ctrl = net::kQueueFront | net::kQueueToggle;
    s.command(net::Cmd::ReclaimUnit, con, b, shift);
    check(s.w->unit(con)->orders.size() == 2 && s.reclaimLeg(con, b), "Shift queues a second one behind it");
    s.command(net::Cmd::ReclaimUnit, con, b, shift);
    check(s.w->unit(con)->orders.size() == 1 && s.reclaimLeg(con, a), "Shift on the queued one takes it back (4d7a30)");
    s.command(net::Cmd::ReclaimUnit, con, own, ctrl);
    const auto& o = s.w->unit(con)->orders;
    check(o.size() == 2 && o.front().reclaimUnit == own && o.back().reclaimUnit == a,
          "Ctrl puts one on our own unit at the head, the rest behind it");
    s.command(net::Cmd::ReclaimUnit, con, con);
    check(s.w->unit(con)->orders.size() == 2, "the reclaimer itself is refused, its queue kept");

    // The wire: the server admits it.
    net::Command c; c.kind = net::Cmd::ReclaimUnit; c.unitId = con; c.targetId = a;
    net::Writer wr; wr.u32(1); wr.cmd(c);
    std::vector<net::Command> parsed;
    check(srv::parsePlayerCommands(wr.b, parsed) && parsed.size() == 1 && parsed[0].kind == net::Cmd::ReclaimUnit &&
          parsed[0].targetId == a, "the server parses it as an ordinary verb");
    net::Command d; d.kind = net::Cmd::DGun; d.unitId = con; d.targetId = a;
    net::Writer wd; wd.u32(1); wd.cmd(d);
    check(srv::parsePlayerCommands(wd.b, parsed), "...and the D-gun (339)");
}

// The ground mission end to end.
static void groundReclaim(const Types& t) {
    std::printf("a ground reclaimer takes an enemy apart:\n");
    Scene s;
    const int con = s.w->spawn(&t.con, 200, 300, 0, 0);
    const int victim = s.w->spawn(&t.victim, 520, 300, 0, 1);
    // The player's books settle (taSettle) once it has its base storage.
    auto& books = s.w->player(0).books;
    books.hasBase = true; books.base[0] = books.base[1] = 1000;
    const double before = books.produced[1];
    s.command(net::Cmd::ReclaimUnit, con, victim);
    int first = -1, second = -1, deadAt = -1, hp0 = 500;
    for (int i = 0; i < 2000 && deadAt < 0; ++i) {
        s.tick();
        const sim::Unit* v = s.w->unit(victim);
        if (!v->alive() || v->hp <= Fixed()) { deadAt = i; break; }
        const int hp = v->hp.floorInt();
        if (hp != hp0) {
            if (first < 0) first = i; else if (second < 0) second = i;
            hp0 = hp;
        }
    }
    char detail[200];
    std::snprintf(detail, sizeof detail, "first record at %d, second at %d, dead at %d", first, second, deadAt);
    check(first > 0 && second - first == 16, "a record every 16 ticks", detail);
    check(deadAt > 0 && deadAt - first == 16 * 19, "20 records of 25 take its 500 HP", detail);
    const sim::Unit* v = s.w->unit(victim);
    check(v->deathType == sim::Unit::kDeathReclaim && v->lastHitBy == con, "it dies of the reclaim, the reclaimer its hitter");
    s.tick(31);
    const double paid = s.w->player(0).books.produced[1] - before;
    std::snprintf(detail, sizeof detail, "metal produced %.1f", paid);
    check(paid == 100, "the reclaimer's player gets its buildcostmetal back, no energy", detail);
    check(s.w->player(0).books.produced[0] == 0, "...and no energy");
    check(s.w->unit(con)->orders.empty(), "the order is done");
}

static void refused(const Types& t) {
    std::printf("what 0x489960 refuses:\n");
    Scene s;
    const int con = s.w->spawn(&t.con, 200, 300, 0, 0);
    const int boss = s.w->spawn(&t.boss, 400, 300, 0, 1);
    s.command(net::Cmd::ReclaimUnit, con, boss);
    s.tick(2);
    check(s.w->unit(con)->orders.empty() && s.w->unit(boss)->hp == Fixed::fromInt(500),
          "a commander (cancapture): \"That unit cannot be reclaimed\", the order goes");
    const int plane = s.w->spawn(&t.plane, 400, 400, 0, 1);
    s.w->unit(plane)->flightGroundMode = 2;
    check(!s.w->reclaimUnitAllowed(*s.w->unit(con), *s.w->unit(plane)), "an airborne flyer cannot be reclaimed");
    s.w->unit(plane)->flightGroundMode = 1;
    check(s.w->reclaimUnitAllowed(*s.w->unit(con), *s.w->unit(plane)), "...a landed one can");
}

static void outOfReach(const Types& t) {
    std::printf("a target carried out of reach:\n");
    Scene s;
    const int con = s.w->spawn(&t.con, 200, 300, 0, 0);
    const int victim = s.w->spawn(&t.victim, 400, 300, 0, 1);
    s.command(net::Cmd::ReclaimUnit, con, victim);
    int hit = -1;
    for (int i = 0; i < 600 && hit < 0; ++i) {
        s.tick();
        if (s.w->unit(victim)->hp.floorInt() < 500) hit = i;
    }
    check(hit > 0, "the work starts");
    // It walks off: once past builddistance + 21 the wakes stop dealing
    // records, and the reclaimer starts over (stage 0) and follows it.
    s.w->order(victim, 1000, 300, false);
    int gap = 0, longest = 0, dead = -1, hp = s.w->unit(victim)->hp.floorInt();
    for (int i = 0; i < 3000 && dead < 0; ++i) {
        s.tick();
        const sim::Unit* v = s.w->unit(victim);
        if (!v->alive() || v->hp <= Fixed()) { dead = i; break; }
        if (v->hp.floorInt() != hp) { hp = v->hp.floorInt(); gap = 0; }
        else longest = std::max(longest, ++gap);
    }
    const float dx = s.w->unit(con)->x.toFloat() - 1000;
    char detail[160];
    std::snprintf(detail, sizeof detail, "longest pause %d ticks, dead after %d ticks, reclaimer %.0f px off",
                  longest, dead, dx);
    check(longest > 16, "out of reach no record is dealt", detail);
    check(dead > 0 && dx > -130 && dx < 130, "it walks after it and finishes the job", detail);
}

static void flyingReclaim(const Types& t) {
    std::printf("a flyer (VTOL_ReclaimUnit):\n");
    Scene s;
    const int fly = s.w->spawn(&t.flyCon, 200, 300, 0, 0);
    const int victim = s.w->spawn(&t.victim, 600, 300, 0, 1);
    s.command(net::Cmd::ReclaimUnit, fly, victim);
    int first = -1, dead = -1;
    for (int i = 0; i < 3000 && dead < 0; ++i) {
        s.tick();
        const sim::Unit* v = s.w->unit(victim);
        if (!v->alive() || v->hp <= Fixed()) { dead = i; break; }
        if (first < 0 && v->hp.floorInt() < 500) first = i;
    }
    const sim::Unit* f = s.w->unit(fly);
    const float dx = f->x.toFloat() - 600, dz = f->z.toFloat() - 300;
    char detail[160];
    std::snprintf(detail, sizeof detail, "first record %d, dead %d, flyer %.1f,%.1f px off, mode %d",
                  first, dead, dx, dz, int(f->flightGroundMode));
    check(first > 0 && dead - first == 16 * 19, "it flies over the target and deals a record every 16 ticks", detail);
    check(dx * dx + dz * dz <= 60.0f * 60.0f && f->flightGroundMode == 2, "...from the air, within builddistance", detail);
    check(s.w->unit(victim)->deathType == sim::Unit::kDeathReclaim, "the target dies of the reclaim");
}

int main() {
    const Types t;
    arithmetic();
    commands(t);
    groundReclaim(t);
    refused(t);
    outOfReach(t);
    flyingReclaim(t);
    if (g_fail) { std::printf("reclaim_unit_test: %d FAILED\n", g_fail); return 1; }
    std::printf("reclaim_unit_test: all passed\n");
    return 0;
}
