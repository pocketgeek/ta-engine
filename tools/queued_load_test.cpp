// queued_load_test -- a Load that waits its turn in the passenger's queue starts
// when that turn comes.
//
// World::loadInto gives the carrier its reciprocal pickup when the Load is
// issued. Retail's carrier handlers (GROUND_PICKUP 408860, VTOL_PICKUP 41a680)
// end the pickup at any wake where the passenger's head mission is not the
// Move_Seek_Pickup on this carrier, and retail can afford that: its order call
// (4d7a30) always gives the passenger's Move_Seek_Pickup as a replacement, so it
// leads from the start. Ours also lets the passenger queue the Load (Shift) or
// put an order ahead of it (Ctrl), and the carrier dropped its pickup while the
// Load was still waiting; when the Load's turn came it found no pickup on the
// carrier and ended at once.
//
// Each case here runs with a ground and a flying carrier, in Retail and Legion
// pathfinding: a Load queued behind a Move, an Attack and another Load, a Move
// put ahead of a running Load, a carrier with several pickups (the ready
// passengers board first), the pickup ending when the passenger's queue is
// replaced or the passenger dies, and the mirror cases (an Unload queued behind
// a Move, and behind a pickup that is waiting for its passenger).
//
// Without the fix 34 of the 44 checks fail in each mode. The other ten hold
// either way and guard what must not change: a plain Load boards, a pickup ends
// with the boarding (and with the passenger aboard another carrier), a pickup
// behind the carrier's own Move ends when it leads, and an Unload queued behind
// a Move lands its passenger.

#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "net/lockstep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

using namespace tak::sim;
namespace net = tak::net;

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

void behindMove() {
    Rig ref;
    const int rp = ref.w.spawn(&ref.t.foot, px, pz + 300, 0, 0), rc = ref.w.spawn(&ref.t.carrier, cx, cz, 0, 0);
    ref.send(net::Cmd::Load, rp, net::kQueueReplace, rc);
    const Walk direct = track(ref, rp, rc, 1500);

    Rig r;
    const int p = r.w.spawn(&r.t.foot, px, pz, 0, 0), c = r.w.spawn(&r.t.carrier, cx, cz, 0, 0);
    r.send(net::Cmd::Move, p, net::kQueueReplace, 0, px, pz + 300);
    r.send(net::Cmd::Load, p, net::kQueueAppend, c);
    bool held = true, early = false;
    float reached = 1e9f, strayed = 0;
    const Walk q = track(r, p, c, 1500, [&](int) {
        const Unit* u = r.w.unit(p);
        const bool moving = !u->orders.empty() && !u->orders[World::currentLeg(u->orders)].load;
        if (moving) {
            held = held && r.pickup(c, p);
            reached = std::min(reached, r.dist(p, px, pz + 300));
            strayed = std::max(strayed, r.dist(c, cx, cz));
        } else early = early || reached > 40;
    });
    std::printf("  direct: boarded at tick %d after %.0f px; behind a Move: at tick %d after %.0f px, %.0f px from "
                "the Move's point, the waiting carrier strayed %.0f px\n", direct.boarded, direct.walked, q.boarded,
                q.walked, reached, strayed);
    check(direct.boarded >= 0, "a plain Load boards");
    check(held && strayed < 8, "the carrier keeps its pickup, and its place, while the Load waits behind a Move");
    check(q.boarded >= 0 && !early, "a Load queued behind a Move boards once the Move has arrived");
    check(q.boarded >= 0 && q.walked <= 300 + direct.walked * 1.1f + 40,
          "the queued Load walks no farther than the Move and the plain Load together");
    check(!r.pickup(c), "the carrier's pickup ends with the boarding");
}

void behindAttack() {
    Rig ref;
    const int rp = ref.w.spawn(&ref.t.archer, px, pz, 0, 0), rc = ref.w.spawn(&ref.t.carrier, cx, cz, 0, 0);
    ref.send(net::Cmd::Load, rp, net::kQueueReplace, rc);
    const Walk direct = track(ref, rp, rc, 1500);

    Rig r;
    const int p = r.w.spawn(&r.t.archer, px, pz, 0, 0), c = r.w.spawn(&r.t.carrier, cx, cz, 0, 0);
    const int e = r.w.spawn(&r.t.weak, px, pz + 150, 0, 1);
    r.send(net::Cmd::Attack, p, net::kQueueReplace, e);
    r.send(net::Cmd::Load, p, net::kQueueAppend, c);
    int died = -1;
    const Walk q = track(r, p, c, 1500, [&](int i) {
        const Unit* enemy = r.w.unit(e);
        if (died < 0 && (!enemy || !enemy->alive())) died = i;
    });
    std::printf("  direct: boarded at tick %d after %.0f px; behind an Attack: enemy dead at tick %d, boarded at "
                "tick %d after %.0f px, min x %.0f z %.0f\n", direct.boarded, direct.walked, died, q.boarded, q.walked,
                q.minX, q.minZ);
    check(died >= 0 && q.boarded > died, "a Load queued behind an Attack boards once the enemy is dead");
    check(q.boarded >= 0 && q.minX > px - 60 && q.minZ > pz - 60 && q.walked <= direct.walked * 1.1f + 60,
          "the queued Load goes straight for its carrier");
}

// A Load on one carrier, a second queued on another. The first carrier dies:
// the second Load's turn comes. The first boards: the second carrier is left
// without its pickup.
void behindLoad() {
    for (bool firstDies : {true, false}) {
        Rig r;
        const int p = r.w.spawn(&r.t.foot, px, pz, 0, 0);
        const int c1 = r.w.spawn(&r.t.carrier, px, pz + 500, 0, 0), c2 = r.w.spawn(&r.t.carrier, cx, cz, 0, 0);
        r.send(net::Cmd::Load, p, net::kQueueReplace, c1);
        r.send(net::Cmd::Load, p, net::kQueueAppend, c2);
        bool boarded = false, held = true;
        int cleared = -1;
        for (int i = 0; i < 1500; ++i) {
            if (firstDies && i == 20) r.w.unit(c1)->hp = Fixed();
            r.w.tick(1.f / 30);
            if (firstDies) {
                const Unit* first = r.w.unit(c1);
                if (first && first->alive()) held = held && r.pickup(c2, p);
                if (r.aboard(p, c2)) { boarded = true; break; }
            } else if (r.aboard(p, c1)) {
                boarded = true;
                if (!r.pickup(c2)) { cleared = i; break; }
            } else held = held && r.pickup(c2, p);
        }
        if (firstDies) {
            check(held, "the second carrier keeps its pickup while the first Load runs");
            check(boarded, "a Load queued behind a Load boards its carrier when the first carrier dies");
        } else {
            check(held && boarded, "a Load with a second Load queued behind it boards its own carrier");
            check(cleared >= 0 && r.w.unit(c2)->orders.empty(),
                  "the second carrier's pickup ends once the passenger is aboard the first");
        }
    }
}

// Ctrl puts a Move ahead of a Load that is under way, while the passenger
// walks to its carrier or while the transfer runs: the Load starts again when
// the Move ends.
void moveAhead() {
    for (bool transfer : {false, true}) {
        Rig r;
        const int p = r.w.spawn(&r.t.foot, px, pz, 0, 0);
        const int c = r.w.spawn(&r.t.carrier, transfer ? px + 100 : cx + 300, cz, 0, 0);
        r.send(net::Cmd::Load, p, net::kQueueReplace, c);
        bool sent = false, shape = false;
        float reached = 1e9f;
        const Walk q = track(r, p, c, 3000, [&](int i) {
            const Unit* carrier = r.w.unit(c);
            const bool now = transfer ? !carrier->orders.empty() && carrier->orders.front().transportMission.stage == 2 &&
                                        carrier->orders.front().transportTicks >= 5
                                      : i == 40;
            if (!sent && now) {
                r.send(net::Cmd::Move, p, net::kQueueFront, 0, px, pz + 700);
                const Unit* u = r.w.unit(p);
                shape = u->orders.size() == 2 && !u->orders.front().load && u->orders.back().load;
                sent = true;
            }
            if (sent) reached = std::min(reached, r.dist(p, px, pz + 700));
        });
        check(sent && shape && reached < 40 && q.boarded >= 0,
              transfer ? "a Load boards after a Move put ahead of it during the transfer (Ctrl)"
                       : "a Load boards after a Move put ahead of it on the way to the carrier (Ctrl)");
    }
}

// One carrier, four Loads: two passengers still have a Move to finish, two
// are free. The free ones board while the others walk, although the walkers'
// pickups lead the carrier's list; the walkers board when their Moves end.
void pickupQueue() {
    Rig r;
    const int c = r.w.spawn(&r.t.carrier, cx, cz, 0, 0);
    const int far = r.w.spawn(&r.t.foot, px, pz, 0, 0), near = r.w.spawn(&r.t.foot, px, pz + 80, 0, 0);
    const int free1 = r.w.spawn(&r.t.foot, cx - 200, cz + 80, 0, 0), free2 = r.w.spawn(&r.t.foot, cx - 200, cz - 80, 0, 0);
    r.send(net::Cmd::Move, far, net::kQueueReplace, 0, px - 800, pz);
    r.send(net::Cmd::Move, near, net::kQueueReplace, 0, px, pz + 600);
    r.send(net::Cmd::Load, far, net::kQueueAppend, c);
    r.send(net::Cmd::Load, near, net::kQueueAppend, c);
    r.send(net::Cmd::Load, free1, net::kQueueAppend, c);
    r.send(net::Cmd::Load, free2, net::kQueueAppend, c);
    int at[4] = {-1, -1, -1, -1}, turn[2] = {-1, -1};
    const int ids[4] = {far, near, free1, free2};
    for (int i = 0; i < 4000; ++i) {
        r.w.tick(1.f / 30);
        bool all = true;
        for (int k = 0; k < 4; ++k) {
            const Unit* u = r.w.unit(ids[k]);
            if (k < 2 && turn[k] < 0 && !u->orders.empty() && u->orders.front().load) turn[k] = i;
            if (at[k] < 0 && r.aboard(ids[k], c)) at[k] = i;
            all = all && at[k] >= 0;
        }
        if (all) break;
    }
    std::printf("  boarded at ticks: far %d (its Move ended at %d), near %d (at %d), free %d and %d\n", at[0], turn[0],
                at[1], turn[1], at[2], at[3]);
    check(at[0] >= 0 && at[1] >= 0 && at[2] >= 0 && at[3] >= 0, "every queued passenger boards");
    check(at[2] >= 0 && at[3] >= 0 && turn[0] >= 0 && turn[1] >= 0 && std::max(at[2], at[3]) < std::min(turn[0], turn[1]),
          "the passengers that are free board while the others still walk");
    check(!r.pickup(c) && r.w.unit(c)->cargo.size() == 4, "the carrier ends with its four passengers and no pickup");
}

// The passenger's queue is replaced, or the passenger dies, before the Load's
// turn: the carrier's pickup goes. A pickup that is itself still queued behind
// the carrier's Move goes when it leads, and the carrier does not wait for it.
void pickupEnds() {
    for (int how = 0; how < 4; ++how) {
        Rig r;
        const int p = r.w.spawn(&r.t.foot, px, pz, 0, 0), c = r.w.spawn(&r.t.carrier, cx, cz, 0, 0);
        const bool busy = how == 3;
        if (busy) r.send(net::Cmd::Move, c, net::kQueueReplace, 0, cx, cz + 200);
        r.send(net::Cmd::Move, p, net::kQueueReplace, 0, px - 600, pz);
        r.send(net::Cmd::Load, p, net::kQueueAppend, c);
        for (int i = 0; i < 40; ++i) r.w.tick(1.f / 30);
        const bool held = r.pickup(c, p);
        if (how == 0) r.send(net::Cmd::Move, p, net::kQueueReplace, 0, px - 600, pz + 100);
        else if (how == 2) r.w.unit(p)->hp = Fixed();
        else r.send(net::Cmd::Stop, p, net::kQueueReplace, 0);
        int gone = -1;
        float reached = 1e9f;
        for (int i = 0; i < (busy ? 600 : 60) && gone < 0; ++i) {
            r.w.tick(1.f / 30);
            reached = std::min(reached, r.dist(c, cx, cz + 200));
            if (!r.pickup(c)) gone = i;
        }
        const Unit* carrier = r.w.unit(c);
        // (A flyer's Move with an order behind it hands over inside its arrival circle.)
        check(held && gone >= 0 && carrier->orders.empty() && carrier->cargo.empty() && (!busy || reached < 160),
              how == 0 ? "the pickup ends when the passenger's queue is replaced by a Move"
            : how == 1 ? "the pickup ends when the passenger is stopped"
            : how == 2 ? "the pickup ends when the passenger dies before its turn"
                       : "a pickup queued behind the carrier's Move ends when it leads, its passenger stopped");
    }
}

// The mirror: an Unload queued behind a Move, and behind a pickup that is
// still waiting for its passenger.
void queuedUnload() {
    {
        Rig r;
        const int c = r.w.spawn(&r.t.carrier, cx, cz, 0, 0), p = r.w.spawn(&r.t.foot, cx, cz, 0, 0);
        r.w.unit(p)->inTransport = c; r.w.unit(c)->cargo.push_back(p);
        r.send(net::Cmd::Move, c, net::kQueueReplace, 0, cx, cz + 400);
        r.send(net::Cmd::Unload, c, net::kQueueAppend, 0, cx + 500, cz + 400);
        float reached = 1e9f;
        bool out = false;
        for (int i = 0; i < 3000 && !out; ++i) {
            r.w.tick(1.f / 30);
            if (r.w.unit(p)->embarked()) reached = std::min(reached, r.dist(c, cx, cz + 400));
            else out = true;
        }
        check(out && reached < 200 && r.dist(p, cx + 500, cz + 400) < 200,
              "an Unload queued behind a Move lands its passenger at the drop point after the Move");
    }
    {
        Rig r;
        const int p = r.w.spawn(&r.t.foot, px, pz, 0, 0), c = r.w.spawn(&r.t.carrier, cx, cz, 0, 0);
        r.send(net::Cmd::Move, p, net::kQueueReplace, 0, px, pz + 300);
        r.send(net::Cmd::Load, p, net::kQueueAppend, c);
        r.send(net::Cmd::Unload, c, net::kQueueAppend, 0, cx + 600, cz);
        bool boarded = false, out = false;
        for (int i = 0; i < 4000 && !out; ++i) {
            r.w.tick(1.f / 30);
            boarded = boarded || r.aboard(p, c);
            out = boarded && !r.w.unit(p)->embarked();
        }
        check(boarded && out && r.dist(p, cx + 600, cz) < 200,
              "an Unload queued behind a waiting pickup carries the passenger to the drop point");
    }
}

void all() {
    behindMove();
    behindAttack();
    behindLoad();
    moveAhead();
    pickupQueue();
    pickupEnds();
    queuedUnload();
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
