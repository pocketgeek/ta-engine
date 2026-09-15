// crowdbench -- measure how a CROWD fares, not whether one unit arrives.
//
//   crowdbench [scenario...]        (default: all)
//
// WHY THIS EXISTS. The headless --mpai harness reports one number, a state hash, and it
// answers exactly one question: did anything change. It cannot say whether a change made
// crowds better or worse, and for three separate pathfinding changes in a row it reported
// the same hash -- the AI scenario never queues enough searches, never routes long enough
// to hit the corner cap, and never jams hard enough to matter. "No regression" was all it
// could ever have said.
//
// So the experimental work (deterministic yielding at chokepoints, distinct arrival
// positions for group orders, a bounded A* fallback for difficult terrain) needs numbers
// of its own. These are those numbers. Each scenario reports:
//
//   arrived      how many of the group reached their destination in the time allowed
//   t50 / t95    seconds by which half / almost all of them had arrived
//   travel       mean distance travelled against the straight-line distance (ratio)
//   work         pathfinder work units consumed, and completed searches
//
// Arrival RATE is the headline: a change that shortens travel while stranding units is a
// regression, and one that costs more search work but lands everyone is usually a win.
// Report all of them so that trade is visible rather than hidden behind one figure.

#include "sim/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tak::sim;

namespace {

UnitType soldier() {
    UnitType t{};
    t.name = "sol"; t.id = "sol";
    t.maxVel = 70; t.turnRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.footX = 2; t.footZ = 2;
    return t;
}

struct Tracked {
    int id = 0;
    float gx = 0, gz = 0;     // destination
    float straight = 0;       // straight-line distance at the start
    float travelled = 0;
    float px = 0, pz = 0;
    float arrivedAt = -1;     // seconds, -1 = never
};

struct Result {
    std::string name;
    int n = 0, arrived = 0;
    float t50 = -1, t95 = -1;
    float travelRatio = 0;
    uint64_t work = 0, completions = 0;
};

// Run a world until everyone arrives or `seconds` elapse.
Result run(const std::string& name, World& w, std::vector<Tracked>& group, float seconds,
           float arriveR = 70.0f) {
    const float dt = 1.0f / 30.0f;
    const int steps = int(seconds / dt);
    for (auto& g : group) {
        const Unit* u = w.unit(g.id);
        g.px = u->x; g.pz = u->z;
        g.straight = std::sqrt((g.gx - u->x) * (g.gx - u->x) + (g.gz - u->z) * (g.gz - u->z));
    }
    for (int i = 0; i < steps; ++i) {
        w.tick(dt);
        const float t = float(i + 1) * dt;
        for (auto& g : group) {
            const Unit* u = w.unit(g.id);
            if (!u || !u->alive()) continue;
            g.travelled += std::sqrt((u->x - g.px) * (u->x - g.px) + (u->z - g.pz) * (u->z - g.pz));
            g.px = u->x; g.pz = u->z;
            if (g.arrivedAt < 0) {
                const float dx = u->x - g.gx, dz = u->z - g.gz;
                if (std::sqrt(dx * dx + dz * dz) < arriveR) g.arrivedAt = t;
            }
        }
    }
    Result r;
    r.name = name;
    r.n = int(group.size());
    std::vector<float> times;
    float ratioSum = 0; int ratioN = 0;
    for (const auto& g : group) {
        if (g.arrivedAt >= 0) { ++r.arrived; times.push_back(g.arrivedAt); }
        if (g.straight > 1.0f) { ratioSum += g.travelled / g.straight; ++ratioN; }
    }
    std::sort(times.begin(), times.end());
    if (!times.empty()) {
        r.t50 = times[times.size() / 2];
        r.t95 = times[size_t(float(times.size() - 1) * 0.95f)];
    }
    r.travelRatio = ratioN ? ratioSum / float(ratioN) : 0;
    r.work = w.pathStats().workSpent();
    r.completions = w.pathStats().completions();
    return r;
}

void report(const Result& r) {
    std::printf("  %-22s arrived %3d/%-3d  t50 %6s  t95 %6s  travel x%.2f  work %8llu (%llu searches)\n",
                r.name.c_str(), r.arrived, r.n,
                r.t50 < 0 ? "--" : (std::to_string(int(r.t50 * 10) / 10.0f).substr(0, 4)).c_str(),
                r.t95 < 0 ? "--" : (std::to_string(int(r.t95 * 10) / 10.0f).substr(0, 4)).c_str(),
                r.travelRatio,
                (unsigned long long)r.work, (unsigned long long)r.completions);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> want;
    for (int i = 1; i < argc; ++i) want.push_back(argv[i]);
    auto wanted = [&](const char* n) {
        return want.empty() || std::find(want.begin(), want.end(), n) != want.end();
    };
    std::printf("crowdbench\n");

    // OPPOSING COLUMNS. Two groups swap ends of the same corridor, so each has to get
    // through the other. The classic failure is both columns stalling nose to nose.
    if (wanted("columns")) {
        const int W = 200, H = 200;
        World w;
        w.setVisPlayer(-1);
        w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
        w.setPathService(true);
        UnitType s = soldier();
        std::vector<Tracked> group;
        for (int k = 0; k < 16; ++k) {
            Tracked a; a.id = w.spawn(&s, 600.0f + float(k % 4) * 32, 800.0f + float(k / 4) * 32, 0, 0);
            a.gx = 2000; a.gz = 900; group.push_back(a);
            Tracked b; b.id = w.spawn(&s, 2000.0f + float(k % 4) * 32, 800.0f + float(k / 4) * 32, 0, 1);
            b.gx = 600; b.gz = 900; group.push_back(b);
        }
        for (auto& g : group) w.order(g.id, g.gx, g.gz, false);
        report(run("opposing columns", w, group, 120.0f));
    }

    // CHOKEPOINT. One narrow door in a wall, a crowd on one side, the goal on the other.
    if (wanted("choke")) {
        const int W = 200, H = 200;
        World w;
        w.setVisPlayer(-1);
        w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
        for (int z = 0; z < H; ++z)
            if (z < 98 || z > 101) w.blockCells(100, z, 1, 1, true);
        w.setPathService(true);
        UnitType s = soldier();
        std::vector<Tracked> group;
        for (int k = 0; k < 24; ++k) {
            Tracked a;
            a.id = w.spawn(&s, 1200.0f + float(k % 6) * 32, 1500.0f + float(k / 6) * 32, 0, 0);
            a.gx = 1900; a.gz = 1600; group.push_back(a);
        }
        for (auto& g : group) w.order(g.id, g.gx, g.gz, false);
        report(run("chokepoint (1 door)", w, group, 180.0f));
    }

    // GROUP ORDER TO ONE POINT. Everyone is sent to the SAME pixel; the question is how
    // many settle rather than fighting over it.
    if (wanted("blob")) {
        const int W = 200, H = 200;
        World w;
        w.setVisPlayer(-1);
        w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
        w.setPathService(true);
        UnitType s = soldier();
        std::vector<Tracked> group;
        for (int k = 0; k < 32; ++k) {
            Tracked a;
            a.id = w.spawn(&s, 600.0f + float(k % 8) * 32, 600.0f + float(k / 8) * 32, 0, 0);
            a.gx = 1800; a.gz = 1800; group.push_back(a);
        }
        for (auto& g : group) w.order(g.id, g.gx, g.gz, false);
        report(run("group order, one point", w, group, 150.0f, /*arriveR=*/120.0f));
    }

    // OPEN FIELD. The control: nothing in the way, so any cost here is overhead.
    if (wanted("open")) {
        const int W = 200, H = 200;
        World w;
        w.setVisPlayer(-1);
        w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
        w.setPathService(true);
        UnitType s = soldier();
        std::vector<Tracked> group;
        for (int k = 0; k < 24; ++k) {
            Tracked a;
            a.id = w.spawn(&s, 400.0f + float(k % 6) * 32, 400.0f + float(k / 6) * 32, 0, 0);
            a.gx = 2600; a.gz = 2600; group.push_back(a);
        }
        for (auto& g : group) w.order(g.id, g.gx, g.gz, false);
        report(run("open field", w, group, 150.0f));
    }

    // DIFFICULT TERRAIN. A serpentine the boundary tracer has to wind through -- the
    // case item 7's bounded A* fallback is meant for.
    if (wanted("maze")) {
        const int W = 200, H = 200;
        World w;
        w.setVisPlayer(-1);
        w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
        // Horizontal baffles with alternating gaps.
        for (int row = 0; row < 6; ++row) {
            const int z = 40 + row * 20;
            const int gapAt = (row % 2) ? 20 : 170;
            for (int x = 10; x < 190; ++x)
                if (x < gapAt || x > gapAt + 5) w.blockCells(x, z, 1, 1, true);
        }
        w.setPathService(true);
        UnitType s = soldier();
        std::vector<Tracked> group;
        for (int k = 0; k < 12; ++k) {
            Tracked a;
            a.id = w.spawn(&s, 500.0f + float(k % 4) * 32, 300.0f + float(k / 4) * 32, 0, 0);
            a.gx = 1600; a.gz = 2600; group.push_back(a);
        }
        for (auto& g : group) w.order(g.id, g.gx, g.gz, false);
        report(run("serpentine maze", w, group, 240.0f));
    }

    std::printf("\n  (arrival rate is the headline; travel x1.00 is the straight line)\n");
    return 0;
}
