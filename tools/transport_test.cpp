// transport_test -- unloadAt must leave a routable move leg IN FRONT of the unload.
//
// The transport used to route itself: unloadAt ran NavGrid::findPath (a synchronous A*
// on the sim thread) and pushed its waypoints ahead of the unload order. That A* was
// the only caller of findPath and resolved its grid with the same navFor() the async
// boundary tracer uses, so it was removed and unloadAt now asks the tracer like every
// other move order does.
//
// That swap has one hazard worth a test. The tracer installs its route with
// replaceLeg(), which rebuilds the CURRENT leg -- the first order flagged `goal` -- out
// of path waypoints, carrying only the leg's movement flags across. `unload` is not one
// of them. So if the unload order were itself the leg, a route arriving for it would
// come back as plain waypoints, the flag would be gone, and the transport would sail to
// the drop point and sit there with its cargo aboard for ever. Nothing else in the sim
// would complain: the orders would look perfectly valid.
//
// Hence the shape unloadAt builds: a plain move order carrying `goal`, with the unload
// queued BEHIND it, where replaceLeg preserves it untouched. The first two checks below
// pin exactly that, because it is a property of the order queue that no gameplay test
// would notice breaking. The rest drive a real unload through to disembark.

#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <vector>

using tak::sim::UnitType;
using tak::sim::World;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  %-68s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}

int main() {
    std::printf("transport_test\n");

    // 64x64 cells of flat land above sea level. Cargo needs somewhere walkable to
    // stand on when it disembarks; the transport's own domain is not what is under
    // test here, so keeping the map uniform keeps the test about the order queue.
    const int W = 64, H = 64;
    World w;
    w.setVisPlayer(-1);                       // headless, like the referee
    w.setTerrain(std::vector<uint8_t>(size_t(W) * H, 100), W, H, /*seaLevel=*/20);

    UnitType boat{};
    boat.name = "boat";
    boat.maxVel = 60;                         // maxVel > 0 => not a structure
    boat.turnRate = 10000;
    boat.transportCap = 4;
    boat.transportDist = 70;
    boat.maxHp = 100;

    UnitType foot{};
    foot.name = "foot";
    foot.maxVel = 30;
    foot.turnRate = 10000;
    foot.transportSize = 1;
    foot.maxHp = 100;

    const int tid = w.spawn(&boat, 100, 100);
    const int cid = w.spawn(&foot, 100, 100);

    // Board directly rather than driving the load order: the pickup is a separate
    // path and this test is about what happens after.
    {
        tak::sim::Unit* t = w.unit(tid);
        tak::sim::Unit* c = w.unit(cid);
        c->inTransport = tid;
        t->cargo.push_back(cid);
    }

    const float dropX = 600, dropZ = 600;
    w.unloadAt(tid, dropX, dropZ);

    // ---- the shape of the queue ----
    {
        const tak::sim::Unit* t = w.unit(tid);
        check(t->orders.size() == 2, "unloadAt queues a move leg plus the unload");
        if (t->orders.size() == 2) {
            const tak::sim::Order& leg = t->orders[0];
            const tak::sim::Order& un = t->orders[1];
            // The leg replaceLeg() would rebuild is the FIRST order flagged `goal`.
            // It must be the move, never the unload -- see the header.
            check(leg.goal && !leg.unload,
                  "the routable leg is the move order, not the unload");
            check(!un.goal && un.unload,
                  "the unload sits BEHIND the leg, where replaceLeg preserves it");
            check(un.x == dropX && un.z == dropZ,
                  "the unload keeps the drop point it was given");
        } else {
            g_fail += 3;
        }
    }

    // ---- and it still actually unloads ----
    //
    // With no path service running this is the straight-line case: the transport
    // sails at the leg, consumes it, the unload reaches the front and tickTransport
    // disembarks. (The spliced-route case cannot desync from this one -- replaceLeg
    // only ever rewrites the leg, and the checks above are what guarantee the unload
    // is not the leg.)
    bool disembarked = false;
    for (int i = 0; i < 3000 && !disembarked; ++i) {
        w.tick(1.0f / 30.0f);
        disembarked = w.unit(tid)->cargo.empty();
    }
    check(disembarked, "the transport sails to the drop point and disembarks");
    if (disembarked) {
        const tak::sim::Unit* c = w.unit(cid);
        check(c->inTransport == 0, "the cargo unit is off the transport");
        const float dx = c->x - dropX, dz = c->z - dropZ;
        check(std::sqrt(dx * dx + dz * dz) < 250,
              "the cargo is put down near the drop point, not where it boarded");
        check(w.unit(tid)->orders.empty(), "the unload order is consumed, not left stuck");
    } else {
        g_fail += 3;
    }

    std::printf(g_fail ? "transport_test: %d FAILURE(S)\n" : "transport_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
