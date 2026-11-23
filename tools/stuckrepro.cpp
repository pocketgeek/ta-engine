// stuckrepro -- reproduce two user-reported divergences from retail:
//   1) units ordered around a solid obstacle stack/stick against it and never
//      re-path (retail always recovers and flows around);
//   2) the flying conjurer (zonhunt monarch) wanders/circles while conjuring
//      instead of holding station.
// Direct-World harness in the crowdbench mold: synthetic types, flat map with a
// blocked blob, everything readable per tick.
#include "sim/sim.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace tak::sim;

static UnitType soldier() {
    UnitType t{};
    t.name = "sol"; t.id = "sol";
    t.maxVel = Fixed::fromFloat(70.0f / 30.0f); t.turnRate = 10000;
    t.turnInPlaceRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.footX = 2; t.footZ = 2;
    return t;
}
static UnitType monarch() {
    UnitType t{};
    t.name = "mon"; t.id = "mon";
    t.maxVel = Fixed::fromFloat(90.0f / 30.0f); t.turnRate = 8000;
    t.turnInPlaceRate = 8000;
    t.maxHp = 1000; t.canMove = true; t.canFly = true; t.vtolStandby = true;
    t.isBuilder = true; t.buildDist = 90; t.workerTime = 1;
    t.footX = 2; t.footZ = 2;
    return t;
}
static UnitType beast() {
    UnitType t{};
    t.name = "bst"; t.id = "bst";
    t.maxVel = Fixed::fromFloat(60.0f / 30.0f); t.turnRate = 9000;
    t.maxHp = 200; t.canMove = true;
    t.footX = 2; t.footZ = 2;
    t.buildTime = 5;   // quick, so several complete inside the run
    return t;
}

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::string(argv[1]) == "-v";
    const float dt = 1.0f / 30.0f;
    int fails = 0;

    // ---- scenario 1: route AROUND a solid blob ------------------------------
    {
        const int W = 200, H = 200;
        World w;
        w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
        w.setPathService(true);
        // A solid 14x14-cell rock blob (like the crater rocks) at cells (90..103).
        w.blockCells(90, 90, 14, 14, true);
        UnitType s = soldier();
        struct T { int id; float sx, sz; };
        std::vector<T> grp;
        // Group on the left of the blob, goal on the right: the straight line hits it.
        for (int k = 0; k < 12; ++k) {
            float sx = 1200.0f + float(k % 4) * 32, sz = 1480.0f + float(k / 4) * 32;
            grp.push_back({w.spawn(&s, sx, sz, 0, 0), sx, sz});
        }
        for (auto& g : grp) w.order(g.id, 1900.0f, 1550.0f, false);
        int arrived = 0;
        for (int step = 0; step < int(180.0f / dt); ++step) w.tick(dt);
        for (auto& g : grp) {
            const Unit* u = w.unit(g.id);
            if (!u) continue;
            float dx = u->x.toFloat() - 1900.0f, dz = u->z.toFloat() - 1550.0f;
            bool ok = dx * dx + dz * dz < 150.0f * 150.0f;
            arrived += ok;
            if (!ok && verbose)
                std::printf("  STUCK id=%d at (%.0f,%.0f) orders=%zu routeStamp=%d "
                            "blockStreak=%d speed=%.2f\n",
                            g.id, u->x.toFloat(), u->z.toFloat(), u->orders.size(),
                            u->routeStamp, u->bodyBlockStreak, u->speed.toFloat());
        }
        std::printf("[around-blob] arrived %d/12 in 180s%s\n", arrived,
                    arrived == 12 ? "" : "  <-- FAIL (retail flows around)");
        if (arrived != 12) ++fails;
    }

    // ---- scenario 2: conjuring flyer must HOLD STATION (queue path) ---------
    {
        const int W = 120, H = 120;
        World w;
        w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
        w.setPathService(true);
        UnitType m = monarch(), b = beast();
        int mid = w.spawn(&m, 900, 900, 0, 0);
        w.train(mid, &b, 5);   // conjure a queue of beasts
        float sx = 900, sz = 900, maxDrift = 0;
        int ordersSeen = 0;
        for (int step = 0; step < int(60.0f / dt); ++step) {
            w.tick(dt);
            const Unit* u = w.unit(mid);
            if (!u) break;
            float dx = u->x.toFloat() - sx, dz = u->z.toFloat() - sz;
            maxDrift = std::max(maxDrift, std::sqrt(dx * dx + dz * dz));
            if (!u->orders.empty()) ++ordersSeen;
            if (verbose && !u->orders.empty() && (step % 30) == 0)
                std::printf("  t=%.1f monarch has order to (%.0f,%.0f) drift=%.0f queue=%zu\n",
                            step * dt, u->orders.front().x.toFloat(),
                            u->orders.front().z.toFloat(), maxDrift, u->buildQueue.size());
        }
        std::printf("[conjure-queue] maxDrift=%.0fpx orderTicks=%d%s\n", maxDrift, ordersSeen,
                    maxDrift < 40.0f ? "" : "  <-- FAIL (should hold station)");
        if (maxDrift >= 40.0f) ++fails;
    }

    // ---- scenario 3: conjuring flyer must HOLD STATION (ghost-site path) ----
    {
        const int W = 120, H = 120;
        World w;
        w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
        w.setPathService(true);
        UnitType m = monarch(), b = beast();
        int mid = w.spawn(&m, 900, 900, 0, 0);
        w.queueBuild(mid, &b, 940, 900, false);   // place a conjure 40px away
        float maxDrift = 0;
        for (int step = 0; step < int(60.0f / dt); ++step) {
            w.tick(dt);
            const Unit* u = w.unit(mid);
            if (!u) break;
            float dx = u->x.toFloat() - 900, dz = u->z.toFloat() - 900;
            maxDrift = std::max(maxDrift, std::sqrt(dx * dx + dz * dz));
        }
        // 40px order distance + slack; circling would show as ~160+.
        std::printf("[conjure-site] maxDrift=%.0fpx%s\n", maxDrift,
                    maxDrift < 90.0f ? "" : "  <-- FAIL (circling/wandering)");
        if (maxDrift >= 90.0f) ++fails;
    }

    std::printf(fails ? "stuckrepro: %d scenario(s) FAILED\n" : "stuckrepro: all passed\n", fails);
    return fails ? 1 : 0;
}
