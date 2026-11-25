// queued_target_test -- a queued order that follows a unit starts toward its
// target, never toward a placeholder point.
//
// World::attack stores an attack leg with x = z = 0: tickCombat puts the target's
// position into it each tick it chases. The tick the leg ahead ended (its target
// died, a guard's ward died, ...) tickCombat dropped it and returned, so the mover
// took the queued attack with its point still at the placeholder and, under
// Retail pathfinding, asked for a route to the map's top-left corner. The next
// tick the chase corrected the leg's point but kept the route's waypoints, and the
// unit walked to the corner first. Shift-right-click and the right-drag attack box
// send exactly this: an Attack, then queued Attacks.
//
// Each case: an order on a unit 200 px south runs first (an Attack until its enemy
// dies, a Guard until its ward dies, a Repair until it is done, or a Move), with a
// second order queued on a target 900 px east. From the tick the queued leg is
// current the unit must close on its target: never more than 48 px farther from it
// than it was then, never near the corner, and walking no more than 10% past the
// same order given directly. Retail and Legion pathfinding.

#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "net/lockstep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace ta::sim;
namespace net = ta::net;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

PathfindingMode mode = PathfindingMode::Retail;

UnitType soldier(const char* id) {
    UnitType t{}; t.id = t.name = id; t.maxVel = Fixed::fromFloat(70.f / 30);
    t.turnRate = t.turnInPlaceRate = 10000; t.maxHp = 300; t.canMove = t.canGuard = t.canPatrol = true;
    t.footX = t.footZ = 2; t.sight = 4096; t.buildTime = 60; t.buildCostMetal= 60; t.upright = true;
    return t;
}
Weapon weapon(bool shortReach) {
    Weapon w; w.name = shortReach ? "queued-test-sword" : "queued-test-bolt";
    w.range = shortReach ? 40 : 200; w.damage = 60; w.reload = 0.2f; w.aimTol = 32767;
    if (!shortReach) w.projVel = 900;   // the sword: an instant short-reach shot (TA has no melee)
    return w;
}

struct Types {
    UnitType sword = soldier("sword"), archer = soldier("archer"), smith = soldier("smith"),
             weak = soldier("weak"), post = soldier("post"), friendly = soldier("friendly"),
             hut = soldier("hut"), flyer = soldier("flyer");
    Types() {
        sword.weapon = weapon(true); sword.weapons = {sword.weapon};
        archer.weapon = weapon(false); archer.weapons = {archer.weapon};
        // An armed builder: it attacks first, then repairs or helps build.
        smith = archer; smith.id = smith.name = "smith";
        smith.isBuilder = smith.canReclaim = true; smith.workerTime = 100; smith.buildDist = 60;
        
        // Mobile, but
        // given no orders: they stand where they are spawned.
        weak.maxHp = 400; post.maxHp = 100000;
        friendly.maxVel = {}; friendly.canMove = false; friendly.maxHp = 10000;
        hut.maxHp = 2000; hut.maxVel = {}; hut.canMove = false; hut.buildTime = 30000; hut.buildCostMetal= 10;
        flyer = archer; flyer.id = flyer.name = "flyer"; flyer.canFly = true; flyer.upright = false;
        flyer.maxVel = Fixed::fromFloat(120.f / 30); flyer.cruiseAlt = 80;
    }
};

struct Rig {
    World w;
    Rig() {
        w.setSerialThreads(true);
        w.setPathfindingMode(mode);
        w.setTerrain(std::vector<uint8_t>(size_t(256) * 256, 100), 256, 256, 20);
        w.setPathService(true);
        w.setPlayerCount(2);
        
    }
    void send(net::Cmd kind, int unit, uint8_t queue, int target, float x = 0, float z = 0) {
        static const TypeRegistry none;
        net::Command c;
        c.kind = kind; c.player = 0; c.unitId = unit; c.x = x; c.z = z; c.targetId = target; c.queue = queue;
        applyCommand(w, none, c);
    }
};

// The order queued behind (and the reference walk's order): on target b, 900 px east.
enum class Kind { AttackShort, AttackRanged, AttackFlyer, Guard, Repair, Assist };
const char* name(Kind k) {
    switch (k) {
    case Kind::AttackShort: return "short-reach Attack";
    case Kind::AttackRanged: return "ranged Attack";
    case Kind::AttackFlyer: return "flyer Attack";
    case Kind::Guard: return "Guard";
    case Kind::Repair: return "Repair";
    case Kind::Assist: return "Assist (help build)";
    }
    return "?";
}
// The order that runs first, on a unit 200 px south, and how it ends: its enemy
// dies, its ward dies, its repair finishes, or (the control) a Move arrives.
enum class First { Attack, Guard, Repair, Move };
const char* name(First f) {
    switch (f) {
    case First::Attack: return "an Attack";
    case First::Guard: return "a Guard whose ward dies";
    case First::Repair: return "a finished Repair";
    case First::Move: return "a Move";
    }
    return "?";
}

// `direct` gives the queued order alone, as the reference walk.
struct Run { int promoted = -1; float startDist = 0, worst = 0, minX = 1e9f, minZ = 1e9f, walked = 0; };
Run run(Kind kind, First first, bool direct) {
    Types t; Rig r;
    const float ax = 2000, az = 2000, bx = 2900, bz = 2000;
    const UnitType* self = kind == Kind::AttackShort ? &t.sword : kind == Kind::AttackFlyer ? &t.flyer
                         : kind == Kind::AttackRanged && first == First::Attack ? &t.archer : &t.smith;
    const int a = r.w.spawn(self, ax, az, 0, 0);
    int e1 = 0;
    switch (first) {
    case First::Attack: e1 = r.w.spawn(&t.weak, ax, az + 200, 0, 1); break;
    case First::Guard: e1 = r.w.spawn(&t.friendly, ax, az + 200, 0, 0); break;
    case First::Repair:
        e1 = r.w.spawn(&t.friendly, ax, az + 200, 0, 0);
        r.w.unit(e1)->hp = Fixed::fromFloat(t.friendly.maxHp) - Fixed::fromInt(20);
        break;
    case First::Move: break;
    }
    int b = 0;
    net::Cmd cmd = net::Cmd::Attack;
    switch (kind) {
    case Kind::AttackShort: case Kind::AttackRanged: case Kind::AttackFlyer:
        b = r.w.spawn(&t.post, bx, bz, 0, 1); break;
    case Kind::Guard: b = r.w.spawn(&t.friendly, bx, bz, 0, 0); cmd = net::Cmd::Guard; break;
    case Kind::Repair:
        b = r.w.spawn(&t.friendly, bx, bz, 0, 0); r.w.unit(b)->hp = Fixed::fromInt(100);
        cmd = net::Cmd::Repair; break;
    case Kind::Assist: {
        const int other = r.w.spawn(&t.smith, bx, bz + 200, 0, 0);
        b = r.w.startBuild(other, &t.hut, bx, bz, World::Approach::None);
        cmd = net::Cmd::Assist; break;
    }
    }
    if (!direct) {
        switch (first) {
        case First::Attack: r.send(net::Cmd::Attack, a, 0, e1); break;
        case First::Guard: r.send(net::Cmd::Guard, a, 0, e1); break;
        case First::Repair: r.send(net::Cmd::Repair, a, 0, e1); break;
        case First::Move: r.send(net::Cmd::Move, a, 0, 0, ax, az + 150); break;
        }
    }
    r.send(cmd, a, direct ? 0 : net::kQueueAppend, b);
    Run out;
    float px = r.w.unit(a)->x.toFloat(), pz = r.w.unit(a)->z.toFloat();
    for (int i = 0; i < 1500; ++i) {
        // The ward dies a second in, once the guard is under way.
        if (first == First::Guard && !direct && i == 30) r.w.unit(e1)->hp = Fixed();
        r.w.tick(1.f / 30);
        const Unit* u = r.w.unit(a);
        if (!u || !u->alive()) break;
        const float x = u->x.toFloat(), z = u->z.toFloat();
        bool current = false;
        if (!u->orders.empty()) {
            const Order& leg = u->orders[World::currentLeg(u->orders)];
            current = kind == Kind::Repair ? leg.repairTarget == b : kind == Kind::Assist ? leg.helpSite == b
                    : leg.targetId == b;
        }
        if (out.promoted < 0 && current) {
            out.promoted = i; out.startDist = std::hypot(x - bx, z - bz); px = x; pz = z;
        }
        if (out.promoted >= 0) {
            const float d = std::hypot(x - bx, z - bz);
            out.worst = std::max(out.worst, d - out.startDist);
            out.minX = std::min(out.minX, x); out.minZ = std::min(out.minZ, z);
            out.walked += std::hypot(x - px, z - pz); px = x; pz = z;
            if (d < 60 || i - out.promoted > 600) break;
        }
    }
    return out;
}

void queued(Kind kind, First first) {
    const Run ref = run(kind, first, true), q = run(kind, first, false);
    std::printf("  %s, %s then %s: direct walked %.0f px from %.0f px off; queued: current at tick %d, %.0f px off, "
                "drifted %.0f px away, min x %.0f z %.0f, walked %.0f px\n", pathfindingModeName(mode), name(first),
                name(kind), ref.walked, ref.startDist, q.promoted, q.startDist, q.worst, q.minX, q.minZ, q.walked);
    char what[200];
    std::snprintf(what, sizeof what, "%s: %s queued behind %s goes straight for its target",
                  pathfindingModeName(mode), name(kind), name(first));
    // It closes on the target, never near the corner, and walks no more than 10%
    // past the direct walk (plus the difference in where the two set out).
    check(q.promoted >= 0 && q.worst < 48 && q.minX > 1500 && q.minZ > 1500 &&
          q.walked <= ref.walked * 1.1f + std::abs(q.startDist - ref.startDist), what);
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--legion")) mode = PathfindingMode::Legion;
    for (Kind k : {Kind::AttackShort, Kind::AttackRanged, Kind::AttackFlyer, Kind::Guard, Kind::Repair, Kind::Assist})
        queued(k, First::Attack);
    for (First f : {First::Guard, First::Repair, First::Move}) queued(Kind::AttackRanged, f);
    if (failures) std::printf("%d FAILED\n", failures);
    else std::printf("all passed\n");
    return failures ? 1 : 0;
}
