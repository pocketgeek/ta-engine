// pathblock_test -- a unit must get PAST other units standing in its way.
//
// Reported as "they don't go around larger obstacles, they try their damndest to go
// over or through them first". Terrain was only half of it. The other half was other
// units, and it had two causes that had to be fixed together -- fixing either alone
// leaves the unit stuck, which is why this test exists rather than a comment.
//
// ONE: the SEARCH routed through them. cellScore grades a parked body kCellOccupied,
// which EQUALS kCellThreshold, so it is passable-but-costly -- retail's model, where
// every icd call site compares against 4. Our mover has no local avoidance and bodies
// are solid, so a route through a standing crowd is a route into a wall.
//
// TWO: the route SHORTCUT straightened it back through them. Once the search goes
// around, a shortcut that only checks terrain undoes the detour.
//
// Measured on the wall below before the fix: every thickness from 1 to 8 cells STUCK,
// and it stayed stuck with the shortcut disabled entirely -- which is how the first
// diagnosis (blame the shortcut) was shown to be wrong. After: every thickness passes,
// travelling 1262-1329 against a 1100 straight line.

#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace tak::sim;

static int g_fail = 0;
static void check(bool cond, const std::string& what, const std::string& detail = {}) {
    std::printf("  %-56s %s%s%s\n", what.c_str(), cond ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!cond) ++g_fail;
}

static UnitType soldier() {
    UnitType t{};
    t.name = "sol"; t.id = "sol";
    t.maxVel = 70; t.turnRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.footX = 2; t.footZ = 2;
    return t;
}

// A wall of PARKED bodies across the direct line, `thick` rows deep, with open ground
// round both ends. Returns the distance travelled, or -1 if the unit never arrived.
static float runWall(int thick) {
    const int W = 160, H = 160;
    World w;
    w.setVisPlayer(-1);
    w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, /*seaLevel=*/20);
    w.setPathService(true);
    UnitType s = soldier();
    // Player 1's units, left standing still -- a neutral crowd, not a fight.
    for (int t = 0; t < thick; ++t)
        for (int k = 0; k < 40; ++k)
            w.spawn(&s, 600.0f + float(k) * 32.0f, 1000.0f + float(t) * 32.0f, 0, 1);
    for (auto& u : w.units()) u.speed = 0.0f;
    const int id = w.spawn(&s, 900, 500, 0, 0);
    w.order(id, 900, 1600, /*queue=*/false);
    float px = w.unit(id)->x, pz = w.unit(id)->z, travelled = 0;
    for (int i = 0; i < 30 * 200; ++i) {
        w.tick(1.0f / 30.0f);
        const Unit* q = w.unit(id);
        travelled += std::sqrt((q->x - px) * (q->x - px) + (q->z - pz) * (q->z - pz));
        px = q->x; pz = q->z;
        const float dx = q->x - 900.0f, dz = q->z - 1600.0f;
        if (std::sqrt(dx * dx + dz * dz) < 60.0f) return travelled;
    }
    return -1.0f;
}

// A wall of units that are all COMMANDED to move into a dead end, so every one of them
// is wedged: they carry a positive speed (the mover clamps a blocked unit rather than
// stopping it, so it resumes the instant the way clears) while displacing nothing.
//
// Occupancy used to ask `speed == 0` to decide whether a body holds its cell against a
// search. By that test this crowd is "traffic under way", so searches plan straight
// through the middle of a jam that nothing can pass. jamT measures actual displacement
// and sees it for what it is.
static bool jammedCrowdHoldsItsCells() {
    const int W = 160, H = 160;
    World w;
    w.setVisPlayer(-1);
    w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, /*seaLevel=*/20);
    w.setPathService(true);
    UnitType s = soldier();
    // A dense block packed against a wall of other bodies: ordered to push south-west
    // into each other, so they jam rather than park.
    std::vector<int> ids;
    for (int t = 0; t < 6; ++t)
        for (int k = 0; k < 10; ++k)
            ids.push_back(w.spawn(&s, 800.0f + float(k) * 24.0f, 900.0f + float(t) * 24.0f, 0, 1));
    // Everyone is told to move onto the same spot, which nobody can reach: the pile
    // wedges itself.
    for (int id : ids) w.order(id, 820, 920, /*queue=*/false);
    for (int i = 0; i < 30 * 6; ++i) w.tick(1.0f / 30.0f);

    // The class that matters is the INTERSECTION: bodies that hold their cell by
    // displacement while still carrying a positive speed. Those are exactly the ones the
    // old `speed == 0` test called "under way" and let searches route through.
    int missedByOldTest = 0, parked = 0;
    for (int id : ids) {
        const Unit* u = w.unit(id);
        if (!u || !u->alive()) continue;
        const bool holdsNow = u->speed == 0.0f || u->jamT >= World::kJamHoldsCell;
        const bool holdsOld = u->speed == 0.0f;
        if (u->speed == 0.0f) ++parked;
        if (holdsNow && !holdsOld) ++missedByOldTest;
    }
    std::printf("    %zu bodies: %d parked, %d wedged-but-moving "
                "(invisible to the old speed==0 test)\n",
                ids.size(), parked, missedByOldTest);
    return missedByOldTest > 0;
}

int main() {
    std::printf("pathblock_test\n");
    std::printf("a unit routes around a wall of parked bodies:\n");
    for (int thick = 1; thick <= 8; ++thick) {
        const float travelled = runWall(thick);
        check(travelled > 0,
              "wall " + std::to_string(thick) + " rows deep is passed",
              travelled > 0 ? std::to_string(int(travelled)) + "px travelled"
                            : "STUCK against it");
        // ...and it goes AROUND rather than wandering: the detour is bounded. The
        // straight line is 1100px; a sane way round the end of the wall is well under
        // three times that. Before the fix a stuck unit racked up 2515-3636px going
        // nowhere, so this also catches "arrives eventually, by accident".
        if (travelled > 0)
            check(travelled < 3300.0f,
                  "  and does not wander to get there",
                  std::to_string(int(travelled)) + "px for a 1100px straight line");
    }

    // The goal itself being occupied must still work: ordering a unit to where
    // something already stands is ordinary, and the search's goal exemption is what
    // keeps that from failing to route at all.
    std::printf("the goal can be occupied:\n");
    {
        const int W = 120, H = 120;
        World w;
        w.setVisPlayer(-1);
        w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
        w.setPathService(true);
        UnitType s = soldier();
        for (int k = 0; k < 6; ++k) w.spawn(&s, 1200.0f + float(k) * 24.0f, 1200.0f, 0, 1);
        for (auto& u : w.units()) u.speed = 0.0f;
        const int id = w.spawn(&s, 400, 400, 0, 0);
        w.order(id, 1200, 1200, false);
        bool close = false;
        for (int i = 0; i < 30 * 120 && !close; ++i) {
            w.tick(1.0f / 30.0f);
            const Unit* q = w.unit(id);
            const float dx = q->x - 1200.0f, dz = q->z - 1200.0f;
            close = std::sqrt(dx * dx + dz * dz) < 140.0f;
        }
        check(close, "a unit still reaches a destination others are standing on");
    }

    std::printf("a jammed crowd is not mistaken for traffic:\n");
    check(jammedCrowdHoldsItsCells(),
          "wedged movers are detected by displacement, not commanded speed");

    std::printf(g_fail ? "pathblock_test: %d FAILURE(S)\n" : "pathblock_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
