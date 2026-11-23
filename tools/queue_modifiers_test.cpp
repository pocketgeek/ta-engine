// queue_modifiers_test -- retail's order modifiers (KINGDOMS.icd, B.4 of the
// click plan): Ctrl puts an order at the HEAD of the queue without clearing it
// (0x4d7a09 -> 0x4d7750), and Shift or Ctrl on an order of the same kind as a
// queued one, on its target or within 16 px of its point, removes that order
// instead (0x4d7a5b..0x4d7ab2). Through applyCommand, as the client sends
// them (net/lockstep.h kQueueFront, kQueueToggle).
//
// Our rally rule stays (owner C9): a producer -- a factory, or a mobile builder
// repeating production (a Zhon conjurer) -- takes Move, Patrol, Attack and
// fight-move as RALLY orders for its output; Ctrl and the toggle work on that
// rally list, and Shift-queued rally legs still queue. Retail's "a Shift order
// skips a repeat-building builder" is NOT ported.
//
// The AI and scripts send queue 1 without the toggle bit: an append near a
// queued point must still append (the --mpai gates depend on it).
//
// Synthetic types on a flat map, Retail and Legion pathfinding.

#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "net/lockstep.h"

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

using namespace tak::sim;
namespace net = tak::net;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

PathfindingMode mode = PathfindingMode::Retail;

constexpr uint8_t kShift = net::kQueueAppend | net::kQueueToggle;
constexpr uint8_t kCtrl = net::kQueueFront | net::kQueueToggle;

struct Types {
    UnitType sol, gun, builder, factory, conjurer, hut, foe, tower, boat, rider;
    Types() {
        sol.id = sol.name = "sol";
        sol.maxVel = Fixed::fromInt(2); sol.turnRate = sol.turnInPlaceRate = 10000;
        sol.canMove = sol.canPatrol = sol.canGuard = true; sol.maxHp = 300; sol.footX = sol.footZ = 2;
        sol.sight = 400; sol.buildTime = 60; sol.buildCost = 60;

        gun = sol; gun.id = gun.name = "gun";
        Weapon w; w.name = "bolt"; w.range = 200; w.reload = 0.5f; w.damage = 1; w.projVel = 900;
        gun.weapon = w; gun.weapons = {w};

        builder = sol; builder.id = builder.name = "builder";
        builder.isBuilder = builder.canReclaim = true; builder.workerTime = 100; builder.buildDist = 80;
        builder.storage = 100000; builder.upright = true;

        factory.id = factory.name = "factory";
        factory.maxVel = Fixed(); factory.maxHp = 5000; factory.footX = factory.footZ = 6;
        factory.isBuilder = true; factory.workerTime = 10; factory.storage = 1000000; factory.buildMenu = {"sol"};

        conjurer = builder; conjurer.id = conjurer.name = "conjurer";   // Zhon-style mobile producer
        conjurer.buildMenu = {"sol"};

        hut.id = hut.name = "hut"; hut.maxHp = 2000; hut.maxVel = {}; hut.footX = hut.footZ = 2;
        hut.buildTime = 3000; hut.buildCost = 10;

        foe = sol; foe.id = foe.name = "foe"; foe.maxVel = {}; foe.canMove = false; foe.maxHp = 100000;

        tower = gun; tower.id = tower.name = "tower"; tower.maxVel = {}; tower.canMove = false;
        tower.canPatrol = tower.canGuard = false; tower.weapon.range = 600; tower.weapons = {tower.weapon};

        boat = sol; boat.id = boat.name = "boat"; boat.canTransport = true; boat.transportCap = 4;
        boat.transportSizeCap = 16; boat.maxTransportSize = 4; boat.transportDist = 150;
        rider = sol; rider.id = rider.name = "rider"; rider.transportSize = 1; rider.transportLandEligible = true; rider.modelTop = 40 << 16;
    }
};

struct Rig {
    World w;
    Rig() {
        w.setSerialThreads(true);
        w.setPathfindingMode(mode);
        w.setTerrain(std::vector<uint8_t>(size_t(128) * 128, 100), 128, 128, 20);
        w.setPathService(true);
        w.setPlayerCount(2);
        w.player(0).mana = 1e7;
    }
    void ticks(int n) { for (int i = 0; i < n; ++i) w.tick(1.f / 30); }
    void send(net::Cmd kind, int unit, float x, float z, uint8_t queue, int target = 0) {
        static const TypeRegistry none;
        net::Command c;
        c.kind = kind; c.player = 0; c.unitId = unit; c.x = x; c.z = z; c.targetId = target; c.queue = queue;
        applyCommand(w, none, c);
    }
};

// The issued legs' points, as removeQueuedMatch reads them.
std::vector<std::pair<int, int>> goals(const std::vector<Order>& list) {
    std::vector<std::pair<int, int>> out;
    for (const Order& o : list) {
        if (!o.goal) continue;
        Fixed x = o.x, z = o.z;
        if (o.missionTarget) { x = o.missionTarget->first; z = o.missionTarget->second; }
        else if (o.groundMission) { x = o.clickX; z = o.clickZ; }
        out.push_back({x.floorInt(), z.floorInt()});
    }
    return out;
}
using Pts = std::vector<std::pair<int, int>>;

// Ctrl on a walking unit: the new move goes first, the interrupted one and the
// Shift-queued one follow in order, and the unit really walks them in that order.
void frontMove() {
    Types t; Rig r;
    const int id = r.w.spawn(&t.sol, 300, 1000, 0, 0);
    r.send(net::Cmd::Move, id, 1300, 1000, 0);
    r.send(net::Cmd::Move, id, 1300, 1500, kShift);
    r.ticks(60);
    const float startX = r.w.unit(id)->x.toFloat();
    check(startX > 320, "front: the unit is under way on its first move");
    r.send(net::Cmd::Move, id, 300, 400, kCtrl);
    check(goals(r.w.unit(id)->orders) == Pts{{300, 400}, {1300, 1000}, {1300, 1500}},
          "Ctrl move: the new leg at the head, the running one and the Shift-queued one kept behind it");
    // Visit order: the Ctrl point first, then the interrupted move, then the queued one.
    std::vector<int> visits;
    for (int i = 0; i < 6000 && !r.w.unit(id)->orders.empty(); ++i) {
        r.ticks(1);
        const Unit& u = *r.w.unit(id);
        const auto near = [&](float x, float z) {
            return std::abs(u.x.toFloat() - x) < 40 && std::abs(u.z.toFloat() - z) < 40;
        };
        const int at = near(300, 400) ? 0 : near(1300, 1000) ? 1 : near(1300, 1500) ? 2 : -1;
        if (at >= 0 && (visits.empty() || visits.back() != at)) visits.push_back(at);
    }
    check(visits == std::vector<int>{0, 1, 2} && r.w.unit(id)->orders.empty(),
          "Ctrl move: the unit walks the front leg, then resumes the interrupted move, then the queued one");
}

void frontEmptyIsMove() {
    Types t; Rig a, b;
    const int ia = a.w.spawn(&t.sol, 300, 1000, 0, 0), ib = b.w.spawn(&t.sol, 300, 1000, 0, 0);
    a.send(net::Cmd::Move, ia, 900, 900, kCtrl);
    b.send(net::Cmd::Move, ib, 900, 900, 0);
    a.ticks(200); b.ticks(200);
    check(a.w.stateHash() == b.w.stateHash(), "Ctrl on an idle unit is an ordinary move (same state hash)");
}

void toggleMoves() {
    Types t; Rig r;
    const int id = r.w.spawn(&t.sol, 300, 1000, 0, 0);
    r.send(net::Cmd::Move, id, 1300, 1000, 0);
    r.send(net::Cmd::Move, id, 1300, 1500, kShift);
    r.send(net::Cmd::Move, id, 1800, 1500, kShift);
    r.ticks(10);
    r.send(net::Cmd::Move, id, 1312, 1490, kShift);   // within 16 px of the second
    check(goals(r.w.unit(id)->orders) == Pts{{1300, 1000}, {1800, 1500}},
          "Shift on a queued move's point (within 16 px) removes it and adds nothing");
    r.send(net::Cmd::Move, id, 1800, 1517, kShift);   // 17 px off: a new leg
    check(goals(r.w.unit(id)->orders) == Pts{{1300, 1000}, {1800, 1500}, {1800, 1517}},
          "a Shift move 17 px from a queued one appends");
    r.send(net::Cmd::AttackMove, id, 1800, 1500, kShift);   // other kind: appends
    check(goals(r.w.unit(id)->orders).size() == 4, "Shift fight-move on a queued Move's point appends (kind differs)");
    r.send(net::Cmd::AttackMove, id, 1800, 1500, kCtrl);    // Ctrl toggles too
    check(goals(r.w.unit(id)->orders).size() == 3, "Ctrl on a queued fight-move's point removes it");
    // The AI's append (no toggle bit) never removes.
    r.send(net::Cmd::Move, id, 1800, 1500, net::kQueueAppend);
    check(goals(r.w.unit(id)->orders) == Pts{{1300, 1000}, {1800, 1500}, {1800, 1517}, {1800, 1500}},
          "an append without the toggle bit (AI, scripts) appends on a queued point");
    // The current leg: dropped with its route; the unit goes on to the next.
    r.send(net::Cmd::Move, id, 1300, 1000, kShift);
    check(goals(r.w.unit(id)->orders) == Pts{{1800, 1500}, {1800, 1517}, {1800, 1500}},
          "Shift on the running move's point drops it; the next leg runs");
    r.ticks(30);
    const Unit& u = *r.w.unit(id);
    check(!u.orders.empty() && u.orders.front().groundMission, "the next leg takes over");
}

void attackAndGuard() {
    Types t; Rig r;
    const int id = r.w.spawn(&t.gun, 300, 1000, 0, 0);
    const int e1 = r.w.spawn(&t.foe, 1500, 1000, 0, 1), e2 = r.w.spawn(&t.foe, 1500, 1400, 0, 1);
    const int pal = r.w.spawn(&t.sol, 400, 1300, 0, 0);
    r.send(net::Cmd::Move, id, 800, 1000, 0);
    r.send(net::Cmd::Attack, id, 0, 0, kShift, e1);
    r.send(net::Cmd::Attack, id, 0, 0, kShift, e2);
    r.send(net::Cmd::Attack, id, 0, 0, kShift, e1);   // toggles e1 off
    {
        const auto& o = r.w.unit(id)->orders;
        int attacks = 0, onE1 = 0;
        for (const auto& x : o) if (x.goal && x.targetId && !x.guard) { ++attacks; onE1 += x.targetId == e1; }
        check(attacks == 1 && onE1 == 0, "Shift Attack on an already queued target removes that attack");
    }
    r.send(net::Cmd::Guard, id, 0, 0, kShift, pal);
    r.send(net::Cmd::Guard, id, 0, 0, kShift, pal);
    {
        int guards = 0;
        for (const auto& x : r.w.unit(id)->orders) guards += x.guard;
        check(guards == 0, "Shift Guard on a queued guard of the same unit removes it");
    }
    r.ticks(20);
    r.send(net::Cmd::Attack, id, 0, 0, kCtrl, e1);
    const auto& o = r.w.unit(id)->orders;
    check(!o.empty() && o.front().goal && o.front().targetId == e1 && World::queuedKind(o.front()) == World::QueuedKind::Attack,
          "Ctrl Attack goes ahead of the running move");
    check(goals(o).size() == 3 && o.back().targetId == e2, "...and the move and the queued attack stay behind it");
}

// Armed ATTACK at a point (Cmd::AttackGround, SUPPRESS): Shift on a queued
// point takes it back (its kind only; +0x22 within 16 px), Ctrl puts it at the
// head, and a unit the resolver refuses is left alone by both.
std::vector<std::pair<int, int>> suppressPoints(const std::vector<Order>& list) {
    std::vector<std::pair<int, int>> out;
    for (const Order& o : list)
        if (o.suppress) out.push_back({o.attackPointX.floorInt(), o.attackPointZ.floorInt()});
    return out;
}
void attackGround() {
    Types t; Rig r;
    const int id = r.w.spawn(&t.gun, 300, 1000, 0, 0);
    r.send(net::Cmd::Move, id, 1300, 1000, 0);
    r.send(net::Cmd::AttackGround, id, 1300, 1500, kShift);
    r.send(net::Cmd::AttackGround, id, 1800, 1500, kShift);
    check(World::queuedKind(r.w.unit(id)->orders.back()) == World::QueuedKind::AttackGround,
          "AttackGround: a SUPPRESS order is its own queued kind");
    r.send(net::Cmd::Move, id, 1300, 1500, kShift);   // other kind on its point: appends
    check(suppressPoints(r.w.unit(id)->orders).size() == 2 && goals(r.w.unit(id)->orders).size() == 4,
          "AttackGround: a Shift Move on a queued SUPPRESS point appends (kind differs)");
    r.send(net::Cmd::AttackGround, id, 1310, 1490, kShift);   // within 16 px of the first
    check(suppressPoints(r.w.unit(id)->orders) == Pts{{1800, 1500}},
          "AttackGround: Shift on a queued SUPPRESS point (within 16 px) removes it and adds nothing");
    r.send(net::Cmd::AttackGround, id, 600, 1000, kCtrl);
    {
        const auto& o = r.w.unit(id)->orders;
        check(!o.empty() && o.front().suppress && o.front().attackPointX.floorInt() == 600,
              "AttackGround: Ctrl puts the SUPPRESS at the head");
        check(goals(o).size() == 4 && World::queuedKind(o[1]) == World::QueuedKind::Move,
              "...and the interrupted move and the queued orders stay behind it");
    }
    // A refused unit (an anti-air weapon): Ctrl and Shift leave its queue alone.
    UnitType aa = t.gun; aa.id = aa.name = "aa"; aa.weapons[0].toAir = true; aa.weapon = aa.weapons[0];
    const int a = r.w.spawn(&aa, 300, 600, 0, 0);
    r.send(net::Cmd::Move, a, 1300, 600, 0);
    r.send(net::Cmd::Move, a, 1300, 900, kShift);
    r.ticks(20);
    const auto before = goals(r.w.unit(a)->orders);
    r.send(net::Cmd::AttackGround, a, 700, 600, kCtrl);
    r.send(net::Cmd::AttackGround, a, 1300, 900, kShift);
    check(goals(r.w.unit(a)->orders) == before && suppressPoints(r.w.unit(a)->orders).empty(),
          "AttackGround: an anti-air unit given it with Ctrl or Shift keeps its queue as it was");
}

// The SUPPRESS order's approach (stage 4's ring or circle, a move ahead of
// it) is part of the order: Ctrl rewinds it away, Shift on the point takes
// both back.
void attackGroundApproach() {
    Types t; Rig r;
    const int id = r.w.spawn(&t.gun, 300, 1000, 0, 0);
    r.send(net::Cmd::AttackGround, id, 1300, 1000, 0);   // 1000 px off, range 200
    bool approach = false;
    for (int i = 0; i < 60 && !approach; ++i) {
        r.ticks(1);
        const auto& o = r.w.unit(id)->orders;
        approach = o.size() >= 2 && o[0].suppressApproach && o[1].suppress;
    }
    check(approach, "AttackGround out of reach: an approach goes ahead of the SUPPRESS order");
    check(World::queuedKind(r.w.unit(id)->orders.front()) == World::QueuedKind::None,
          "...and the approach is no queued kind of its own");
    r.ticks(10);
    r.send(net::Cmd::Move, id, 300, 1600, kCtrl);
    {
        const auto& o = r.w.unit(id)->orders;
        int approaches = 0;
        for (const auto& x : o) approaches += x.suppressApproach;
        check(goals(o).size() == 2 && World::queuedKind(o.front()) == World::QueuedKind::Move && o[1].suppress &&
              approaches == 0,
              "Ctrl Move during the approach: the move first, the SUPPRESS behind it, its approach gone");
    }
    Rig r2;
    const int u2 = r2.w.spawn(&t.gun, 300, 1000, 0, 0);
    r2.send(net::Cmd::AttackGround, u2, 1300, 1000, 0);
    for (int i = 0; i < 60 && !(r2.w.unit(u2)->orders.size() >= 2 && r2.w.unit(u2)->orders[0].suppressApproach); ++i)
        r2.ticks(1);
    r2.send(net::Cmd::AttackGround, u2, 1305, 1005, kShift);
    check(r2.w.unit(u2)->orders.empty(), "Shift on the running SUPPRESS point takes back the order and its approach");
    r2.ticks(30);
    check(r2.w.unit(u2)->orders.empty() && r2.w.unit(u2)->x.toFloat() < 700,
          "...and the unit stops instead of walking on to the point");
}

// A tower is no producer: Ctrl puts its Attack at the head of its own list
// (it was appended behind the target it was on).
void towerFront() {
    Types t; Rig r;
    const int tw = r.w.spawn(&t.tower, 1000, 1000, 0, 0);
    const int e1 = r.w.spawn(&t.foe, 1300, 1000, 0, 1), e2 = r.w.spawn(&t.foe, 1000, 1300, 0, 1);
    r.send(net::Cmd::Attack, tw, 0, 0, 0, e1);
    r.ticks(10);
    r.send(net::Cmd::Attack, tw, 0, 0, kCtrl, e2);
    std::vector<int> targets;
    for (const auto& o : r.w.unit(tw)->orders) if (o.goal && o.targetId && !o.autoTarget) targets.push_back(o.targetId);
    check(targets == std::vector<int>{e2, e1}, "tower: Ctrl Attack goes ahead of the target it is on");
}

// Ctrl pickup: the carrier's pickup goes to the head of the CARRIER's list too
// (the command names the passenger), not behind its queued orders.
void transportFront() {
    Types t; Rig r;
    const int boat = r.w.spawn(&t.boat, 1000, 1000, 0, 0), p = r.w.spawn(&t.rider, 1100, 1000, 0, 0);
    r.send(net::Cmd::Move, boat, 1500, 1000, 0);
    r.send(net::Cmd::Move, boat, 1500, 1500, kShift);
    r.send(net::Cmd::Move, p, 700, 1000, 0);
    r.ticks(5);
    r.send(net::Cmd::Load, p, 0, 0, kCtrl, boat);
    const auto& bo = r.w.unit(boat)->orders;
    const auto& po = r.w.unit(p)->orders;
    check(!bo.empty() && bo.front().transportPickup && bo.front().targetId == p && goals(bo).size() == 3,
          "Ctrl pickup: the carrier fetches the passenger first, its two moves kept behind");
    check(!po.empty() && po.front().load && po.front().targetId == boat && goals(po).size() == 2,
          "Ctrl pickup: the passenger boards first, its move kept behind");
}

// Ctrl Unload on an empty transport is refused, as a plain one is: nothing can
// come ahead of it to fill the hold.
void emptyUnloadFront() {
    Types t; Rig r;
    const int boat = r.w.spawn(&t.boat, 1000, 1000, 0, 0);
    r.send(net::Cmd::Move, boat, 1500, 1000, 0);
    r.send(net::Cmd::Unload, boat, 1200, 1200, kCtrl);
    bool unload = false;
    for (const auto& o : r.w.unit(boat)->orders) unload |= o.unload;
    check(!unload && goals(r.w.unit(boat)->orders).size() == 1, "Ctrl Unload on an empty transport adds nothing");
    r.send(net::Cmd::Unload, boat, 1200, 1200, kShift);
    for (const auto& o : r.w.unit(boat)->orders) unload |= o.unload;
    check(unload, "...while Shift still queues it (a pickup may come first)");
}

// Ctrl Patrol on an idle unit and on an idle factory is the plain patrol.
void frontPatrolIdle() {
    Types t;
    {
        Rig a, b;
        const int ia = a.w.spawn(&t.sol, 300, 1000, 0, 0), ib = b.w.spawn(&t.sol, 300, 1000, 0, 0);
        a.send(net::Cmd::Patrol, ia, 900, 900, kCtrl);
        b.send(net::Cmd::Patrol, ib, 900, 900, 0);
        check(goals(a.w.unit(ia)->orders) == goals(b.w.unit(ib)->orders) && goals(a.w.unit(ia)->orders).size() == 2,
              "Ctrl Patrol on an idle unit: the plain two-leg loop");
        a.ticks(300); b.ticks(300);
        check(a.w.stateHash() == b.w.stateHash(), "...same state hash as a plain patrol");
    }
    {
        Rig a, b;
        const int fa = a.w.spawn(&t.factory, 1000, 1000, 0, 0), fb = b.w.spawn(&t.factory, 1000, 1000, 0, 0);
        a.send(net::Cmd::Patrol, fa, 1500, 1500, kCtrl);
        b.send(net::Cmd::Patrol, fb, 1500, 1500, 0);
        check(goals(a.w.unit(fa)->rally) == goals(b.w.unit(fb)->rally) && goals(a.w.unit(fa)->rally).size() == 2,
              "Ctrl Patrol on a factory with no rally: the plain two-step patrol rally");
    }
}

// The hut sites of player 0 that stand at (x, z): finished or not.
std::vector<int> hutsAt(const World& w, const UnitType* hut, float x, float z) {
    std::vector<int> out;
    for (const Unit& u : w.units())
        if (u.alive() && u.type == hut && std::abs(u.x.toFloat() - x) < 16 && std::abs(u.z.toFloat() - z) < 16)
            out.push_back(u.id);
    return out;
}

// A builder at work on a site, interrupted by Ctrl: the work stops and the
// build waits behind the new order, holding its site. The detour is long
// enough for the site to decay away (an unattended site's GetBuilt decays it
// 30 ticks after its last work, at 0.5 / buildtime of its build a tick), so
// the resumed build places it again and the hut still gets built.
void frontInterruptsBuild() {
    Types t; Rig r;
    const int id = r.w.spawn(&t.builder, 600, 1000, 0, 0);
    r.w.queueBuild(id, &t.hut, 800, 1000, false);
    r.send(net::Cmd::Move, id, 1400, 1400, kShift);
    int site = 0;
    for (int i = 0; i < 900 && !site; ++i) { r.ticks(1); site = r.w.unit(id)->buildSiteId; }
    check(site != 0, "build: the builder places its site");
    for (int i = 0; i < 900 && site && !r.w.unit(site)->buildBegun; ++i) r.ticks(1);
    r.ticks(30);
    check(site && r.w.unit(site)->buildBegun && r.w.unit(site)->hp > Fixed(), "build: the builder is at work on it");
    r.send(net::Cmd::Move, id, 600, 1300, kCtrl);
    {
        const Unit& b = *r.w.unit(id);
        const auto& o = b.orders;
        check(b.buildSiteId == 0 && !b.retailBuild, "Ctrl move stops the work on the site");
        check(goals(o).size() == 3 && o.front().groundMission && World::queuedKind(o.front()) == World::QueuedKind::Move &&
                  o[World::currentLeg(o) + 1].buildType == &t.hut && o[World::currentLeg(o) + 1].resumeSite == site &&
                  o.back().clickX == Fixed::fromInt(1400),
              "the interrupted build waits behind the move holding its site, the queued move behind it");
    }
    bool detour = false, gone = false;
    for (int i = 0; i < 12000 && !r.w.unit(id)->orders.empty(); ++i) {
        r.ticks(1);
        const Unit& b = *r.w.unit(id);
        detour |= std::abs(b.x.toFloat() - 600) < 40 && std::abs(b.z.toFloat() - 1300) < 40;
        gone |= !r.w.unit(site) || !r.w.unit(site)->alive();
    }
    const Unit& b = *r.w.unit(id);
    check(gone, "the unattended site decays away during the detour");
    check(detour && b.orders.empty() && std::abs(b.x.toFloat() - 1400) < 60 && std::abs(b.z.toFloat() - 1400) < 60,
          "the builder walks the detour first and works through the rest of its queue");
    const auto huts = hutsAt(r.w, &t.hut, 800, 1000);
    check(huts.size() == 1 && !r.w.unit(huts[0])->underConstruction,
          "the decayed site is placed again and the hut is finished");
}

// A short Ctrl detour: the site still stands when the builder comes back, so
// the resumed build helps that same site to completion (no second site).
void frontResumesStandingSite() {
    Types t; Rig r;
    const int id = r.w.spawn(&t.builder, 600, 1000, 0, 0);
    r.w.queueBuild(id, &t.hut, 800, 1000, false);
    int site = 0;
    for (int i = 0; i < 900 && !site; ++i) { r.ticks(1); site = r.w.unit(id)->buildSiteId; }
    for (int i = 0; i < 900 && site && !r.w.unit(site)->buildBegun; ++i) r.ticks(1);
    r.ticks(450);
    check(site && r.w.unit(site)->underConstruction && r.w.unit(site)->hp > Fixed::fromInt(600),
          "standing: the builder is well into the site");
    const float bx = r.w.unit(id)->x.toFloat(), bz = r.w.unit(id)->z.toFloat();
    r.send(net::Cmd::Move, id, bx, bz + 64, kCtrl);
    for (int i = 0; i < 6000 && !r.w.unit(id)->orders.empty(); ++i) r.ticks(1);
    const Unit* s = r.w.unit(site);
    check(s && s->alive() && !s->underConstruction && hutsAt(r.w, &t.hut, 800, 1000) == std::vector<int>{site},
          "standing: the builder comes back to the same site and finishes it");
}

// A Ctrl detour after which the standing site cannot be reached (walled off
// while the builder was away): the help the resumed build gives ends, and the
// build with it, as a help job does. It used to give the help again and again,
// every few ticks in Legion, for as long as the site stood.
void frontResumeUnreachable() {
    Types t; Rig r;
    const int id = r.w.spawn(&t.builder, 600, 1000, 0, 0);
    r.w.queueBuild(id, &t.hut, 800, 1000, false);
    int site = 0;
    for (int i = 0; i < 900 && !site; ++i) { r.ticks(1); site = r.w.unit(id)->buildSiteId; }
    for (int i = 0; i < 900 && site && !r.w.unit(site)->buildBegun; ++i) r.ticks(1);
    r.ticks(450);
    r.send(net::Cmd::Move, id, 600, 1700, kCtrl);
    for (int i = 0; i < 2000 && r.w.unit(id)->z.toFloat() < 1300; ++i) r.ticks(1);
    check(site && r.w.unit(site)->underConstruction && r.w.unit(id)->z.toFloat() >= 1300,
          "unreachable: the builder has left its half-built site");
    for (int x = 800 - 192; x <= 800 + 192; x += 32)
        for (int z = 1000 - 192; z <= 1000 + 192; z += 32) {
            if (x != 800 - 192 && x != 800 + 192 && z != 1000 - 192 && z != 1000 + 192) continue;
            r.w.spawn(&t.foe, float(x), float(z), 0, 1);
            r.w.blockFoot(t.foe, float(x), float(z), true);
        }
    int helps = 0;
    bool wasHelp = false;
    for (int i = 0; i < 4000 && r.w.unit(site) && r.w.unit(site)->alive(); ++i) {
        r.ticks(1);
        const auto& o = r.w.unit(id)->orders;
        const bool help = !o.empty() && o[World::currentLeg(o)].helpSite == site;
        helps += help && !wasHelp;
        wasHelp = help;
    }
    check(helps == 1, "unreachable: the resumed build helps its standing site once");
}

// Ctrl in the same tick the site goes down, before any work: the unstarted
// site is taken away (cancelBuilds), and the resumed build puts it back.
void frontBeforeWorkKeepsBuild() {
    Types t; Rig r;
    const int id = r.w.spawn(&t.builder, 600, 1000, 0, 0);
    r.w.queueBuild(id, &t.hut, 800, 1000, false);
    int site = 0;
    for (int i = 0; i < 900 && !site; ++i) { r.ticks(1); site = r.w.unit(id)->buildSiteId; }
    check(site && !r.w.unit(site)->buildBegun, "unstarted: the site is placed, no work yet");
    r.send(net::Cmd::Move, id, 600, 1300, kCtrl);
    check(!r.w.unit(site) || !r.w.unit(site)->alive(), "unstarted: Ctrl takes the unstarted site away");
    for (int i = 0; i < 12000 && !r.w.unit(id)->orders.empty(); ++i) r.ticks(1);
    const auto huts = hutsAt(r.w, &t.hut, 800, 1000);
    check(huts.size() == 1 && !r.w.unit(huts[0])->underConstruction, "unstarted: the build still happens after the detour");
}

// Builds in the unit's own queue: Ctrl placement goes first; Shift on a queued
// site removes it.
void buildQueue() {
    Types t; Rig r;
    const int id = r.w.spawn(&t.builder, 600, 1000, 0, 0);
    r.w.queueBuild(id, &t.hut, 1200, 1000, false);
    r.w.queueBuild(id, &t.hut, 1200, 1300, true);
    World::QueuedMatch m;
    m.kind = World::QueuedKind::Build; m.type = &t.hut; m.x = Fixed::fromInt(1205); m.z = Fixed::fromInt(1296);
    check(r.w.removeQueuedMatch(id, false, m), "toggle: a build of the same type on the same site is found");
    {
        int builds = 0;
        for (const auto& o : r.w.unit(id)->orders) builds += o.buildType != nullptr;
        check(builds == 1, "...and removed");
    }
    r.w.insertOrderFront(id, false, [&] { r.w.queueBuild(id, &t.hut, 600, 1300, true); });
    const auto& o = r.w.unit(id)->orders;
    check(o.size() >= 2 && o.front().buildType && o.front().buildZ.floorInt() / 16 == 1300 / 16 &&
          o.back().buildType && o.back().buildX.floorInt() / 16 == 1200 / 16,
          "front: a build put at the head, the queued build behind it");
}

// A factory's rally list.
void factoryRally() {
    Types t; Rig r;
    const int f = r.w.spawn(&t.factory, 1000, 1000, 0, 0);
    const int e = r.w.spawn(&t.foe, 1600, 1600, 0, 1);
    r.send(net::Cmd::Move, f, 1500, 1000, 0);
    r.send(net::Cmd::Move, f, 1500, 1500, kShift);
    r.send(net::Cmd::Move, f, 1200, 1500, kCtrl);
    check(goals(r.w.unit(f)->rally) == Pts{{1200, 1500}, {1500, 1000}, {1500, 1500}},
          "factory: Ctrl puts the rally point at the head of the rally list");
    r.send(net::Cmd::Move, f, 1490, 1010, kShift);
    check(goals(r.w.unit(f)->rally) == Pts{{1200, 1500}, {1500, 1500}},
          "factory: Shift on a rally point removes it");
    r.send(net::Cmd::Attack, f, 0, 0, kCtrl, e);
    check(r.w.unit(f)->rally.size() == 3 && r.w.unit(f)->rally.front().targetId == e,
          "factory: Ctrl Attack rallies the output on the target first");
    r.send(net::Cmd::Attack, f, 0, 0, kShift, e);
    check(r.w.unit(f)->rally.size() == 2 && r.w.unit(f)->rally.front().targetId == 0,
          "factory: Shift Attack on the rallied target takes it back");
    check(r.w.unit(f)->orders.empty(), "factory: its own order list stays empty");
}

// A Zhon-style mobile builder on infinite production: its rally list, its own
// position untouched, Shift-queued patrol legs still open, and no repeat skip.
void conjurerRally() {
    Types t; Rig r;
    const int c = r.w.spawn(&t.conjurer, 1000, 1000, 0, 0);
    r.w.spawn(&t.factory, 300, 300, 0, 0);   // storage
    r.w.setRepeat(c, &t.sol);
    check(r.w.unit(c)->repeatType == &t.sol, "conjurer: repeating production");
    r.send(net::Cmd::Move, c, 1500, 1000, 0);
    r.send(net::Cmd::Patrol, c, 1500, 1500, kShift);
    {
        const auto& rl = r.w.unit(c)->rally;
        check(rl.size() == 2 && rl.back().patrol && rl.back().patrolOpen,
              "conjurer: a Shift-queued patrol rally leg is still added, open");
    }
    r.send(net::Cmd::Move, c, 1200, 600, kCtrl);
    {
        const auto& rl = r.w.unit(c)->rally;
        check(goals(rl) == Pts{{1200, 600}, {1500, 1000}, {1500, 1500}} && rl.back().patrolOpen,
              "conjurer: Ctrl puts the rally move first, the queued legs (the open patrol) behind");
    }
    r.send(net::Cmd::Patrol, c, 1505, 1505, kShift);
    check(goals(r.w.unit(c)->rally) == Pts{{1200, 600}, {1500, 1000}},
          "conjurer: Shift patrol on the queued patrol point removes it");
    r.send(net::Cmd::Move, c, 1700, 1700, kShift);
    check(goals(r.w.unit(c)->rally).size() == 3, "conjurer: a Shift move still queues a rally leg (no repeat skip)");
    const Unit& u = *r.w.unit(c);
    check(u.orders.empty() && u.repeatType == &t.sol && u.x == Fixed::fromInt(1000) && u.z == Fixed::fromInt(1000),
          "conjurer: rally modifiers leave it stationary and repeating");
    // Once it stops repeating, Ctrl works on its own orders.
    r.w.setRepeat(c, &t.sol);
    if (!r.w.unit(c)->repeatType) {
        r.send(net::Cmd::Move, c, 1400, 1000, 0);
        r.send(net::Cmd::Move, c, 600, 1000, kCtrl);
        check(goals(r.w.unit(c)->orders) == Pts{{600, 1000}, {1400, 1000}},
              "conjurer: not repeating, Ctrl puts the move at the head of its own queue");
    }
}

// The same scenario twice gives the same hash; a Ctrl order and an append
// differ (the queue order is hashed state).
uint64_t scenario(uint8_t mod) {
    Types t; Rig r;
    const int id = r.w.spawn(&t.sol, 300, 1000, 0, 0);
    r.send(net::Cmd::Move, id, 1300, 1000, 0);
    r.ticks(30);
    r.send(net::Cmd::Move, id, 300, 1500, mod);
    r.ticks(600);
    return r.w.stateHash();
}
void determinism() {
    const uint64_t a = scenario(kCtrl), b = scenario(kCtrl), c = scenario(kShift);
    std::printf("hash ctrl=%016llx shift=%016llx\n", (unsigned long long)a, (unsigned long long)c);
    check(a == b, "determinism: the same Ctrl scenario gives the same hash");
    check(a != c, "determinism: Ctrl and Shift give different states");
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--legion")) mode = PathfindingMode::Legion;
    std::printf("queue_modifiers mode=%s\n", mode == PathfindingMode::Legion ? "legion" : "retail");
    frontMove();
    frontEmptyIsMove();
    toggleMoves();
    attackAndGuard();
    attackGround();
    attackGroundApproach();
    frontInterruptsBuild();
    frontResumesStandingSite();
    frontResumeUnreachable();
    frontBeforeWorkKeepsBuild();
    buildQueue();
    factoryRally();
    conjurerRally();
    towerFront();
    transportFront();
    emptyUnloadFront();
    frontPatrolIdle();
    determinism();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
