// queued_load_test -- TotalA.exe's LOAD and the queue modifiers on it.
//
// LOAD is the carrier's order (0x43f701 -> 0x43fb61: GROUND_PICKUP or
// VTOL_PICKUP); TA has no passenger mission, so Cmd::Load (unitId the
// passenger, targetId the carrier) puts a pickup in the CARRIER's list and
// leaves the passenger's alone. Shift appends it there, Ctrl puts it at the
// head, and the player's own Shift Load on a queued one takes it back. The
// passenger's orders run on until the carrier's script attaches it (BeCarried
// then clears them, 0x4384a0). Each case runs with a ground and a flying
// carrier, in Retail and Legion pathfinding.

#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "net/lockstep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

using namespace ta::sim;
namespace net = ta::net;

namespace {
int failures = 0;
PathfindingMode mode = PathfindingMode::Retail;
bool air = false;

void check(bool ok, const char* what) {
    std::printf("%s %s, %s carrier: %s\n", ok ? "PASS" : "FAIL", pathfindingModeName(mode), air ? "flying" : "ground",
                what);
    failures += !ok;
}

struct Types {
    UnitType carrier{}, foot{}, archer{}, weak{};
    Types() {
        carrier.id = carrier.name = "carrier"; carrier.maxVel = Fixed::fromFloat(60.f / 30);
        carrier.turnRate = carrier.turnInPlaceRate = 10000; carrier.maxHp = 1000; carrier.canMove = true;
        carrier.footX = carrier.footZ = 2; carrier.sight = 4096;
        carrier.canTransport = true; carrier.transportCap = 4; carrier.transportSizeCap = 16;
        carrier.maxTransportSize = 4; carrier.transportDist = 150;
        if (air) { carrier.canFly = true; carrier.cruiseAlt = 100; }
        // TA's land-going carriers hover (armthovr): GROUND_UNLOAD stops them on
        // a circle round the drop point (TotalA.exe 0x406a23) instead of on it.
        else carrier.canHover = true;
        foot.id = foot.name = "foot"; foot.maxVel = Fixed::fromFloat(70.f / 30);
        foot.turnRate = foot.turnInPlaceRate = 10000; foot.maxHp = 300; foot.canMove = true;
        foot.footX = foot.footZ = 2; foot.sight = 4096; foot.upright = true; foot.transportSize = 1;
        // It stops inside the carrier's retries (a transfer waits for a passenger at rest).
        foot.accel = foot.brake = Fixed::fromFloat(0.25f);
        archer = foot; archer.id = archer.name = "archer";
        Weapon bolt; bolt.name = "queued-load-bolt"; bolt.range = 200; bolt.damage = 60; bolt.reload = 0.2f;
        bolt.aimTol = 32767; bolt.projVel = 900;
        archer.weapon = bolt; archer.weapons = {bolt};
        weak = foot; weak.id = weak.name = "weak"; weak.maxHp = 400;
    }
};

struct Rig {
    Types t;
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
    bool pickup(int carrier, int passenger = 0) const {
        const Unit* c = w.unit(carrier);
        return c && std::any_of(c->orders.begin(), c->orders.end(), [&](const Order& o) {
            return o.transportPickup && (!passenger || o.targetId == passenger);
        });
    }
    bool aboard(int passenger, int carrier) const {
        const Unit* p = w.unit(passenger);
        return p && p->inTransport == carrier;
    }
    float dist(int unit, float x, float z) const {
        const Unit* u = w.unit(unit);
        return std::hypot(u->x.toFloat() - x, u->z.toFloat() - z);
    }
};

// Where the passenger and the carrier start, 400 px apart.
const float px = 2000, pz = 2000, cx = 2400, cz = 2000;

// The passenger until it is aboard carrier `c`: the tick it boards, how far it
// walks and how far toward the map's corner it gets.
struct Walk { int boarded = -1; float walked = 0, minX = 1e9f, minZ = 1e9f; };
Walk track(Rig& r, int p, int c, int ticks, const std::function<void(int)>& each = {}) {
    Walk out;
    float lx = r.w.unit(p)->x.toFloat(), lz = r.w.unit(p)->z.toFloat();
    for (int i = 0; i < ticks && out.boarded < 0; ++i) {
        if (each) each(i);
        r.w.tick(1.f / 30);
        const Unit* u = r.w.unit(p);
        if (!u || !u->alive()) break;
        if (u->inTransport == c) { out.boarded = i; break; }
        const float x = u->x.toFloat(), z = u->z.toFloat();
        out.walked += std::hypot(x - lx, z - lz); lx = x; lz = z;
        out.minX = std::min(out.minX, x); out.minZ = std::min(out.minZ, z);
    }
    return out;
}

// A plain Load: the carrier's pickup; the passenger is never given an order.
void plainLoad() {
    Rig r;
    const int p = r.w.spawn(&r.t.foot, px, pz), c = r.w.spawn(&r.t.carrier, cx, cz);
    r.send(net::Cmd::Load, p, net::kQueueReplace, c);
    check(r.pickup(c, p) && r.w.unit(p)->orders.empty(), "a Load is the carrier's pickup; the passenger gets nothing");
    bool ordered = false;
    const Walk w = track(r, p, c, 3000, [&](int) { ordered |= !r.w.unit(p)->orders.empty(); });
    check(w.boarded >= 0 && w.walked < 1.f, "the carrier fetches a standing passenger, which never moves");
    check(!ordered, "the passenger stays without an order until it is aboard");
    // VTOL_PICKUP climbs back to cruisealt first (stage 4 -> 5, 0x4114c3).
    for (int i = 0; i < 600 && r.pickup(c); ++i) r.w.tick(1.f / 30);
    check(!r.pickup(c), "the carrier's pickup ends with the passenger aboard");
}

// Shift: the Load queues behind the carrier's own Move; Ctrl puts it ahead.
void carrierQueue() {
    for (bool front : {false, true}) {
        Rig r;
        const int p = r.w.spawn(&r.t.foot, px, pz), c = r.w.spawn(&r.t.carrier, cx, cz);
        r.w.order(c, cx + 300, cz + 300, false);
        r.send(net::Cmd::Load, p, front ? net::kQueueFront : net::kQueueAppend, c);
        const Unit* carrier = r.w.unit(c);
        check(carrier->orders.size() >= 2, front ? "Ctrl puts the pickup in the carrier's list" :
                                                   "Shift appends the pickup to the carrier's list");
        check(front == carrier->orders.front().transportPickup,
              front ? "Ctrl puts the pickup at the head" : "Shift leaves the carrier's Move first");
        float nearest = 1e9f;
        const Walk w = track(r, p, c, 6000, [&](int) { nearest = std::min(nearest, r.dist(c, cx + 300, cz + 300)); });
        check(w.boarded >= 0, "the queued pickup boards its passenger");
        check(front ? nearest > 100.f : nearest < (air ? 96.f : 40.f),
              front ? "the pickup ahead runs before the Move" : "the carrier finishes its Move before the pickup");
        if (front) {
            for (int i = 0; i < 600 && r.pickup(c); ++i) r.w.tick(1.f / 30);
            const Unit* after = r.w.unit(c);
            check(!after->orders.empty() && !after->orders.front().transportPickup,
                  "the Move goes on after the pickup put ahead of it");
        }
    }
}

// The player's own Shift Load on a queued pickup takes it back (4d7a30).
void toggle() {
    Rig r;
    const int p = r.w.spawn(&r.t.foot, px, pz), c = r.w.spawn(&r.t.carrier, cx, cz);
    r.w.order(c, cx + 300, cz + 300, false);
    r.send(net::Cmd::Load, p, net::kQueueAppend, c);
    check(r.pickup(c, p), "the Shift Load queues a pickup");
    r.send(net::Cmd::Load, p, net::kQueueAppend | net::kQueueToggle, c);
    check(!r.pickup(c, p), "the player's Shift Load on it again takes it back");
}

// The passenger's own orders run on under the pickup; the attach clears them.
void passengerKeepsOrders() {
    Rig r;
    const int p = r.w.spawn(&r.t.foot, px, pz), c = r.w.spawn(&r.t.carrier, cx, cz);
    r.w.order(p, px - 200, pz, false);
    r.send(net::Cmd::Load, p, net::kQueueReplace, c);
    check(!r.w.unit(p)->orders.empty() && !r.w.unit(p)->orders.front().load,
          "a Load leaves the passenger's own Move in place");
    const Walk w = track(r, p, c, 6000);
    check(w.boarded >= 0 && w.walked > 50.f, "the passenger walks its Move and the carrier still fetches it");
    check(r.w.unit(p)->orders.empty(), "the attach clears the passenger's orders (BeCarried, 0x4384a0)");
}

// The passenger's death fails the pickup at its next wake.
void passengerDies() {
    Rig r;
    const int p = r.w.spawn(&r.t.foot, px, pz), c = r.w.spawn(&r.t.carrier, cx, cz);
    r.send(net::Cmd::Load, p, net::kQueueReplace, c);
    r.w.tick(1.f / 30);
    r.w.unit(p)->deadFor = 0;
    for (int i = 0; i < 200 && r.pickup(c); ++i) r.w.tick(1.f / 30);
    check(!r.pickup(c), "the pickup ends when its passenger dies");
}

// An Unload queued behind the pickup carries the passenger to the drop point.
void unloadBehindPickup() {
    Rig r;
    const int p = r.w.spawn(&r.t.foot, px, pz), c = r.w.spawn(&r.t.carrier, cx, cz);
    r.send(net::Cmd::Load, p, net::kQueueReplace, c);
    r.send(net::Cmd::Unload, c, net::kQueueAppend, 0, px, pz + 600);
    bool boarded = false, released = false;
    for (int i = 0; i < 8000 && !released; ++i) {
        r.w.tick(1.f / 30);
        boarded |= r.aboard(p, c);
        released = boarded && !r.aboard(p, c);
    }
    check(released && r.dist(p, px, pz + 600) < 40.f, "an Unload queued behind the pickup lands the passenger at its point");
}

void all() {
    plainLoad();
    carrierQueue();
    toggle();
    passengerKeepsOrders();
    passengerDies();
    unloadBehindPickup();
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--legion")) mode = PathfindingMode::Legion;
    for (bool flying : {false, true}) { air = flying; all(); }
    if (failures) std::printf("%d FAILED\n", failures);
    else std::printf("all passed\n");
    return failures ? 1 : 0;
}
