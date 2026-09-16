// transport_test -- what unloadAt has to get right about the approach.
//
// The transport used to route itself: unloadAt ran NavGrid::findPath (a synchronous A*
// on the sim thread) and pushed its waypoints ahead of the unload order. That A* was
// the only caller of findPath and resolved its grid with the same navFor() the async
// boundary tracer uses, so it was removed and unloadAt asks the tracer instead.
//
// Two things about that swap are easy to get wrong, and the first version got both.
//
// ONE: what the approach aims at. A drop point is normally LAND -- that is the point of
// the order -- and a boat can never stand on it. tickTransport unloads from
// kUnloadRange (150px) away, but a final move goal completes within max(16px,
// footprint) of its point, so an approach aimed AT the drop point can never complete:
// the boat pushes at unreachable ground while sitting well inside the range it could
// have unloaded from. The old A* hid this by accident -- it required the goal to fit
// the unit, so a land drop point returned no path at all and the bare unload order ran
// under tickTransport's range check. The accident was the real behaviour and it has to
// survive. Hence the three cases: already in range (no approach), a reachable cell
// within range (approach that), nothing suitable (no approach, sail and let the range
// check decide).
//
// TWO: the shape of the queue. The tracer installs routes with replaceLeg(), which
// rebuilds the current leg -- the first order flagged `goal` -- out of route waypoints,
// carrying only the leg's movement flags across. `unload` is not one of them. So if the
// unload order were itself the leg, a route arriving for it would come back as plain
// waypoints, the flag would be gone, and the transport would sail to the drop point and
// sit there with its cargo aboard for ever, with an order list that looks valid.
//
// Neither hazard is visible to a "does it unload?" check on open ground, which is why
// the queue shape is asserted directly and why the coastal case below builds real water
// and runs the path service rather than reusing the flat map.

#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <vector>

using ta::sim::UnitType;
using ta::sim::World;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  %-68s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}

// tickTransport's unload radius. Mirrored here rather than read from the class (it is
// private) -- if they ever disagree, the distance assertions below start failing, which
// is the point.
static constexpr float kRange = 150.0f;

static UnitType boatType() {
    UnitType t{};
    t.name = "boat";
    t.maxVel = 60;              // maxVel > 0 => not a structure
    t.turnRate = 10000;
    t.transportCap = 4;
    t.transportDist = 70;
    t.maxHp = 100;
    return t;
}

static UnitType footType() {
    UnitType t{};
    t.name = "foot";
    t.maxVel = 30;
    t.turnRate = 10000;
    t.transportSize = 1;
    t.maxHp = 100;
    return t;
}

// Board directly rather than driving the load order: the pickup is a separate path and
// every case here is about what happens after it.
static void board(World& w, int tid, int cid) {
    w.unit(cid)->inTransport = tid;
    w.unit(tid)->cargo.push_back(cid);
}

static bool runUntilUnloaded(World& w, int tid, int ticks = 4000) {
    for (int i = 0; i < ticks; ++i) {
        w.tick(1.0f / 30.0f);
        if (w.unit(tid)->cargo.empty()) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1. Open ground: the queue shape, in isolation.
// ---------------------------------------------------------------------------
static void openGround() {
    std::printf("open ground -- the order queue unloadAt builds:\n");
    const int W = 64, H = 64;
    World w;
    w.setVisPlayer(-1);                       // headless, like the referee
    w.setTerrain(std::vector<uint8_t>(size_t(W) * H, 100), W, H, /*seaLevel=*/20);

    UnitType boat = boatType(), foot = footType();
    const int tid = w.spawn(&boat, 100, 100);
    const int cid = w.spawn(&foot, 100, 100);
    board(w, tid, cid);

    const float dropX = 600, dropZ = 600;
    w.unloadAt(tid, dropX, dropZ);

    const ta::sim::Unit* t = w.unit(tid);
    check(t->orders.size() == 2, "an out-of-range drop queues an approach plus the unload");
    if (t->orders.size() == 2) {
        const ta::sim::Order& leg = t->orders[0];
        const ta::sim::Order& un = t->orders[1];
        // The leg replaceLeg() would rebuild is the FIRST order flagged `goal`. It must
        // be the approach, never the unload -- see hazard TWO in the header.
        check(leg.goal && !leg.unload, "the routable leg is the approach, not the unload");
        check(!un.goal && un.unload,
              "the unload sits BEHIND the leg, where replaceLeg preserves it");
        check(un.x == dropX && un.z == dropZ, "the unload keeps the drop point it was given");
        // Hazard ONE: the approach must land inside the unload radius, or arriving at it
        // does not let the unload fire.
        const float dx = leg.x - dropX, dz = leg.z - dropZ;
        check(std::sqrt(dx * dx + dz * dz) <= kRange,
              "the approach point is within unloading range of the drop point");
    } else {
        g_fail += 4;
    }

    const bool done = runUntilUnloaded(w, tid);
    check(done, "the transport sails to the drop point and disembarks");
    if (done) {
        const ta::sim::Unit* c = w.unit(cid);
        check(c->inTransport == 0, "the cargo unit is off the transport");
        const float dx = c->x - dropX, dz = c->z - dropZ;
        check(std::sqrt(dx * dx + dz * dz) < 250,
              "the cargo is put down near the drop point, not where it boarded");
        check(w.unit(tid)->orders.empty(), "the unload order is consumed, not left stuck");
    } else {
        g_fail += 3;
    }
}

// ---------------------------------------------------------------------------
// 2. Already in range: no approach leg at all.
// ---------------------------------------------------------------------------
static void alreadyInRange() {
    std::printf("already within unloading range:\n");
    const int W = 64, H = 64;
    World w;
    w.setVisPlayer(-1);
    w.setTerrain(std::vector<uint8_t>(size_t(W) * H, 100), W, H, /*seaLevel=*/20);

    UnitType boat = boatType(), foot = footType();
    const int tid = w.spawn(&boat, 500, 500);
    const int cid = w.spawn(&foot, 500, 500);
    board(w, tid, cid);

    // 100px away -- inside kRange, so there is nothing to approach.
    w.unloadAt(tid, 600, 500);
    const ta::sim::Unit* t = w.unit(tid);
    check(t->orders.size() == 1 && t->orders[0].unload,
          "a drop point already in range queues the unload alone, no approach");

    // And it fires promptly rather than waiting on a move that was never needed.
    bool quick = false;
    for (int i = 0; i < 30 && !quick; ++i) {
        w.tick(1.0f / 30.0f);
        quick = w.unit(tid)->cargo.empty();
    }
    check(quick, "it unloads within a second instead of driving at the drop point");
}

// A coastline: cells [0,shoreCell) are water, the rest land. Water units need
// minWaterDepth 13 (sea - height >= 13); ground units are blocked by maxWaterDepth 20.
// sea 40 with height 10 / 100 puts each side firmly in one.
static std::vector<uint8_t> coastHeights(int W, int H, int shoreCell) {
    std::vector<uint8_t> hts(size_t(W) * H, 100);
    for (int z = 0; z < H; ++z)
        for (int x = 0; x < shoreCell; ++x) hts[size_t(z) * W + x] = 10;
    return hts;
}

// ---------------------------------------------------------------------------
// 3. A real coastline, with the path service running.
//
// Water on the left, land on the right, drop point on the land just past the shore.
// This is the case the flat map cannot express: the transport's own domain STOPS before
// the drop point, so an approach aimed at the drop point is unreachable and the boat
// has to settle for the nearest water within range.
// ---------------------------------------------------------------------------
static void coastline() {
    std::printf("coastline, path service on:\n");
    const int W = 96, H = 96;
    const int shoreCell = 25;                 // cells [0,25) water, [25,..) land
    World w;
    w.setVisPlayer(-1);
    w.setTerrain(coastHeights(W, H, shoreCell), W, H, /*seaLevel=*/40);
    w.setPathService(true);                   // the async boundary tracer, as in a game

    UnitType boat = boatType(), foot = footType();
    boat.domain = UnitType::Domain::Water;    // navFor() hands it the water grid
    const int tid = w.spawn(&boat, 80, 700);  // out at sea, well down-map
    const int cid = w.spawn(&foot, 80, 700);
    board(w, tid, cid);

    const float shoreX = float(shoreCell) * 16;          // 400
    const float dropX = shoreX + 64, dropZ = 300;        // on land, 64px past the shore
    w.unloadAt(tid, dropX, dropZ);

    const ta::sim::Unit* t = w.unit(tid);
    check(t->orders.size() == 2, "a coastal drop still queues an approach plus the unload");
    if (t->orders.size() == 2) {
        const ta::sim::Order& leg = t->orders[0];
        // The approach must be WATER -- somewhere this boat can actually be. Aiming it
        // at the drop point is the regression this case exists for.
        check(leg.x < shoreX, "the approach point is on water, not the land drop point");
        const float dx = leg.x - dropX, dz = leg.z - dropZ;
        check(std::sqrt(dx * dx + dz * dz) <= kRange,
              "the approach point is within unloading range of the drop point");
        check(leg.goal && !leg.unload && t->orders[1].unload, "the queue shape still holds");
    } else {
        g_fail += 3;
    }

    // Drive it. The route arrives asynchronously and is spliced in by replaceLeg, so
    // this also covers the flag surviving a real route installation.
    const bool done = runUntilUnloaded(w, tid);
    check(done, "the transport crosses the water, reaches the shore and disembarks");
    if (done) {
        const ta::sim::Unit* c = w.unit(cid);
        const ta::sim::Unit* tr = w.unit(tid);
        check(c->inTransport == 0, "the cargo unit is off the transport");
        check(c->x >= shoreX, "the cargo is put down on LAND, not in the water");
        check(tr->x < shoreX, "the transport itself stayed in its own domain");
        const float dx = c->x - dropX, dz = c->z - dropZ;
        check(std::sqrt(dx * dx + dz * dz) < 250, "the cargo lands near the drop point");
    } else {
        g_fail += 4;
    }
}

// ---------------------------------------------------------------------------
// 4. Far inland: nothing to approach, so queue nothing.
// ---------------------------------------------------------------------------
static void farInland() {
    std::printf("drop point far inland -- no reachable approach:\n");
    const int W = 96, H = 96;
    const int shoreCell = 25;
    World w;
    w.setVisPlayer(-1);
    w.setTerrain(coastHeights(W, H, shoreCell), W, H, /*seaLevel=*/40);
    w.setPathService(true);

    UnitType boat = boatType(), foot = footType();
    boat.domain = UnitType::Domain::Water;
    const int tid = w.spawn(&boat, 80, 700);
    const int cid = w.spawn(&foot, 80, 700);
    board(w, tid, cid);

    // 900px inland: no water cell is within kRange of it.
    w.unloadAt(tid, 1300, 300);
    const ta::sim::Unit* t = w.unit(tid);
    // An approach leg here could only be unreachable, which is strictly worse than
    // none: with none the boat sails at the drop point and the range check governs,
    // which is exactly what the old code did by accident.
    check(t->orders.size() == 1 && t->orders[0].unload,
          "no reachable approach => the unload alone, never an unreachable leg");
}

// ---------------------------------------------------------------------------
// 5. A route already in flight when the unload is issued.
//
// unloadAt clears the order list, but a search requested for the orders it just threw
// away is still queued in the PathService. When the new order list needs NO approach
// leg (cases 1 and 3) nothing re-requests, so that stale search survives -- and when it
// lands, replaceLeg() rebuilds the current leg from its waypoints. With a bare unload
// order that leg IS the unload: currentLeg() falls through to the last order when
// nothing carries `goal`, and replaceLeg carries only movement flags across. The unload
// flag is dropped, the transport drives off along a route to somewhere it was told to go
// before, and the cargo never leaves.
// ---------------------------------------------------------------------------
static void retargetWithPendingRoute() {
    std::printf("unload issued while a route is still in flight:\n");
    const int W = 96, H = 96, shoreCell = 25;
    World w;
    w.setVisPlayer(-1);
    w.setTerrain(coastHeights(W, H, shoreCell), W, H, /*seaLevel=*/40);
    w.setPathService(true);

    UnitType boat = boatType(), foot = footType();
    boat.domain = UnitType::Domain::Water;
    const int tid = w.spawn(&boat, 80, 200);
    const int cid = w.spawn(&foot, 80, 200);
    board(w, tid, cid);

    // A long move down the coast, so a search is genuinely outstanding...
    w.order(tid, 200, 1400, /*queue=*/false);
    // ...and the player changes their mind immediately: unload right here. Close enough
    // that no approach is needed, which is precisely the case that re-requests nothing.
    w.unloadAt(tid, 220, 200);

    const ta::sim::Unit* t = w.unit(tid);
    check(t->orders.size() == 1 && t->orders[0].unload,
          "the new order list is the bare unload");

    // Let any in-flight search land. If it is allowed to, it rewrites the unload.
    bool stillUnloading = true;
    for (int i = 0; i < 120 && stillUnloading; ++i) {
        w.tick(1.0f / 30.0f);
        const ta::sim::Unit* u = w.unit(tid);
        if (u->cargo.empty()) break;                       // unloaded: fine
        stillUnloading = !u->orders.empty() && u->orders.back().unload;
    }
    check(stillUnloading, "a superseded route does not overwrite the unload order");
    check(w.unit(tid)->cargo.empty() || w.unit(tid)->orders.back().unload,
          "the transport is still on the unload job, not off following the old route");
}

// ---------------------------------------------------------------------------
// 6. An isolated pond nearer the drop point than the sea.
//
// approachCell scans outward for a cell the transport FITS. Fitting is not reaching: a
// pond the boat could sit in if it were magically placed there is useless if no water
// connects it to where the boat is. Ring order makes this worse than a coin flip --
// the pond is nearer the drop point by construction, so it is found FIRST and a
// perfectly good sea cell one ring further out never gets considered.
// ---------------------------------------------------------------------------
static void isolatedPond() {
    std::printf("an isolated pond closer than the open sea:\n");
    const int W = 96, H = 96, shoreCell = 25;
    std::vector<uint8_t> hts = coastHeights(W, H, shoreCell);
    // A 3x3 landlocked pond at cells x 27..29, z 19..21 -- one cell of land between it
    // and the sea, so it is genuinely a separate body of water.
    for (int z = 19; z <= 21; ++z)
        for (int x = 27; x <= 29; ++x) hts[size_t(z) * W + x] = 10;

    World w;
    w.setVisPlayer(-1);
    w.setTerrain(hts, W, H, /*seaLevel=*/40);
    w.setPathService(true);

    UnitType boat = boatType(), foot = footType();
    boat.domain = UnitType::Domain::Water;
    const int tid = w.spawn(&boat, 80, 700);              // out at sea
    const int cid = w.spawn(&foot, 80, 700);
    board(w, tid, cid);

    // Drop on the strip of land between the sea and the pond: cell (26,20). The pond is
    // one ring away, the sea two -- so the wrong answer is also the nearer one.
    const float dropX = 26 * 16 + 8, dropZ = 20 * 16 + 8;
    const float shoreX = float(shoreCell) * 16;           // 400
    w.unloadAt(tid, dropX, dropZ);

    const ta::sim::Unit* t = w.unit(tid);
    check(t->orders.size() == 2, "a reachable sea cell IS in range, so an approach is queued");
    if (t->orders.size() == 2) {
        const ta::sim::Order& leg = t->orders[0];
        check(leg.x < shoreX,
              "the approach is the open sea the boat can reach, not the nearer pond");
        check(w.pathExists(&boat, leg.x, leg.z, w.unit(tid)->x, w.unit(tid)->z),
              "the approach point is actually reachable from where the transport is");
    } else {
        g_fail += 2;
    }

    check(runUntilUnloaded(w, tid), "and it gets there and disembarks");
}

int main() {
    std::printf("transport_test\n");
    openGround();
    alreadyInRange();
    coastline();
    farInland();
    retargetWithPendingRoute();
    isolatedPond();
    std::printf(g_fail ? "transport_test: %d FAILURE(S)\n" : "transport_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
