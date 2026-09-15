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
    float gx = 0, gz = 0;     // the spot this unit was ASSIGNED (its final order)
    float cx = 0, cz = 0;     // the point the ORDER named (the group's centre)
    float straight = 0;       // straight-line distance at the start
    float travelled = 0;
    float px = 0, pz = 0;
    float arrivedAt = -1;     // reached its assigned spot (seconds, -1 = never)
    float arrivedArea = -1;   // reached the ordered AREA around the centre
    float areaR = 0;          // that area's radius, from the DESTINATION's own geometry
};

struct Result {
    std::string name;
    int n = 0, arrived = 0, arrivedArea = 0;
    float t50 = -1, t95 = -1;
    float travelRatio = 0;
    uint64_t work = 0, completions = 0, failures = 0, requests = 0;
};

// Run a world until everyone arrives or `seconds` elapse.
Result run(const std::string& name, World& w, std::vector<Tracked>& group, float seconds,
           float arriveR = 70.0f) {
    const float dt = 1.0f / 30.0f;
    const int steps = int(seconds / dt);
    // THE AREA IS THE DESTINATION'S OWN GEOMETRY, not a number scaled off the tracked
    // population. The radius used to grow with the WHOLE group even when that group held
    // two separate destinations -- 223.6px for columns, where a final move goal actually
    // completes within 16px -- so units stalled hundreds of pixels from their orders
    // counted as arrivals and the stranding this benchmark exists to catch was hidden.
    //
    // A crowd that legitimately packs around one point occupies a disc: N bodies of width
    // w fill a radius of about w*sqrt(N)/2. That plus the sim's own completion radius is
    // the area a correctly-arrived member can be standing in, and nothing wider.
    for (auto& g : group) {
        const Unit* u = w.unit(g.id);
        int sharing = 0;
        for (const auto& o : group) {
            const float ddx = o.gx - g.gx, ddz = o.gz - g.gz;
            if (ddx * ddx + ddz * ddz <= 64.0f * 64.0f) ++sharing;
        }
        const float body = u && u->type
            ? float(std::max(u->type->footX, u->type->footZ)) * 16.0f : 32.0f;
        g.areaR = std::max(16.0f, body * 0.5f) + body * std::sqrt(float(sharing)) * 0.5f;
    }
    for (auto& g : group) {
        const Unit* u = w.unit(g.id);
        // MEASURE AGAINST THE SPOT THE UNIT WAS ASSIGNED, not the pixel the order named.
        // A group order is an AREA: the sim spreads its members over a lattice so they do
        // not all queue for one point, so a member that lands correctly is legitimately
        // some way from the centre. Measuring everyone against the centre marks a
        // correctly-spread crowd as having failed to arrive -- it counted 28/32 before the
        // spread and 20/32 after, while travel distance and arrival TIME both improved,
        // which is the metric being wrong rather than the crowd doing worse.
        g.cx = g.gx; g.cz = g.gz;         // what the order named
        if (!u->orders.empty()) {
            const Order& fin = u->orders.back();
            g.gx = fin.x; g.gz = fin.z;   // where this member was actually sent
        }
        g.px = u->x; g.pz = u->z;
        g.straight = std::sqrt((g.cx - u->x) * (g.cx - u->x) + (g.cz - u->z) * (g.cz - u->z));
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
            // ...and, separately, whether it reached the ordered AREA. A group order is
            // an area, so a member standing one body-width off the centre has arrived by
            // any sensible reading. Both are reported because neither alone is honest:
            // spreading the crowd moves members away from the centre (hurting the first)
            // while giving each a precise spot it may not settle on (hurting the second).
            if (g.arrivedArea < 0) {
                const float dx = u->x - g.cx, dz = u->z - g.cz;
                if (std::sqrt(dx * dx + dz * dz) < g.areaR) g.arrivedArea = t;
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
        if (g.arrivedArea >= 0) ++r.arrivedArea;
        if (g.straight > 1.0f) { ratioSum += g.travelled / g.straight; ++ratioN; }
    }
    // PERCENTILES OF THE GROUP, not of the units that happened to make it. Ranking only
    // the arrivals means a run where one unit of 32 arrives reports that unit's time as
    // both t50 and t95 -- so a change that strands the other 31 shows BETTER arrival
    // times than one that lands everybody. The milestone is simply not reached when too
    // few arrive, and saying so is the honest answer.
    std::sort(times.begin(), times.end());
    const size_t need50 = size_t(group.size()) / 2;          // rank of the median member
    const size_t need95 = size_t(float(group.size() - 1) * 0.95f);
    if (times.size() > need50) r.t50 = times[need50];
    if (times.size() > need95) r.t95 = times[need95];
    r.travelRatio = ratioN ? ratioSum / float(ratioN) : 0;
    r.work = w.pathStats().workSpent();
    r.completions = w.pathStats().completions();
    r.failures = w.pathStats().failures();
    r.requests = w.pathStats().requests();
    return r;
}

// Why did the stragglers not arrive? A count alone cannot tell a unit stuck against a
// wall from one still walking when the clock ran out, and those want opposite fixes.
void reportStranded(const World& w, const std::vector<Tracked>& group) {
    int shown = 0;
    for (const auto& g : group) {
        if (g.arrivedAt >= 0) continue;
        const Unit* u = w.unit(g.id);
        if (!u || !u->alive()) { std::printf("      unit %d: dead\n", g.id); continue; }
        const float dx = u->x - g.gx, dz = u->z - g.gz;
        const float left = std::sqrt(dx * dx + dz * dz);
        std::printf("      unit %d: %6.0fpx short of goal, travelled %6.0f (straight %.0f), "
                    "speed %.1f jamT %.2f orders %zu\n",
                    g.id, left, g.travelled, g.straight, u->speed, u->jamT, u->orders.size());
        if (++shown >= 8) { std::printf("      ...\n"); break; }
    }
}

void report(const Result& r) {
    std::printf("  %-22s spot %3d/%-3d  area %3d/%-3d  t50 %6s  t95 %6s  travel x%.2f  "
                "work %8llu  searches %llu (%llu failed, %llu asked)\n",
                r.name.c_str(), r.arrived, r.n, r.arrivedArea, r.n,
                r.t50 < 0 ? "--" : (std::to_string(int(r.t50 * 10) / 10.0f).substr(0, 4)).c_str(),
                r.t95 < 0 ? "--" : (std::to_string(int(r.t95 * 10) / 10.0f).substr(0, 4)).c_str(),
                r.travelRatio,
                (unsigned long long)r.work, (unsigned long long)r.completions,
                (unsigned long long)r.failures, (unsigned long long)r.requests);
}

// Shortest route the grid actually permits, by BFS over the same 8-neighbour adjacency
// the movers use (no diagonal corner-cutting), in world units.
//
// This is the honest denominator. "travel x6.24" only means the pathfinder is doing badly
// if a much shorter route EXISTS -- in a serpentine the straight line is not available to
// anybody, so comparing against it overstates the fault. Measuring the optimum says how
// much of that 6.24 is the maze and how much is us.
float shortestPath(const World& w, const UnitType* t, float sx, float sz, float gx, float gz) {
    const NavGrid& g = w.navFor(t);
    if (g.empty()) return 0;
    const int W = g.width(), H = g.height();
    const int foot = std::max(1, std::max(t->footX, t->footZ));
    const int s = int(sx) / 16, sZ = int(sz) / 16;
    const int e = int(gx) / 16, eZ = int(gz) / 16;
    if (s < 0 || sZ < 0 || e < 0 || eZ < 0 || s >= W || sZ >= H || e >= W || eZ >= H) return 0;
    // Dijkstra on an 8-grid with 10/14 costs (integer, so no float drift).
    const int kInf = 1 << 29;
    std::vector<int> dist(size_t(W) * size_t(H), kInf);
    std::vector<int> heap;
    auto idx = [&](int x, int z) { return size_t(z) * size_t(W) + size_t(x); };
    dist[idx(s, sZ)] = 0;
    heap.push_back(int(idx(s, sZ)));
    // Simple bucket-free Dijkstra: repeatedly scan (grids here are small enough).
    std::vector<uint8_t> done(size_t(W) * size_t(H), 0);
    for (;;) {
        int best = -1, bestD = kInf;
        for (size_t i = 0; i < dist.size(); ++i)
            if (!done[i] && dist[i] < bestD) { bestD = dist[i]; best = int(i); }
        if (best < 0) break;
        done[size_t(best)] = 1;
        const int cx = best % W, cz = best / W;
        if (cx == e && cz == eZ) return float(bestD) * 1.6f;   // 10 cost == 16 world units
        static const int dx8[8] = {0, 1, 0, -1, 1, 1, -1, -1};
        static const int dz8[8] = {-1, 0, 1, 0, -1, 1, 1, -1};
        for (int k = 0; k < 8; ++k) {
            const int nx = cx + dx8[k], nz = cz + dz8[k];
            if (nx < 0 || nz < 0 || nx >= W || nz >= H) continue;
            if (!g.fits(nx, nz, foot)) continue;
            const bool diag = dx8[k] != 0 && dz8[k] != 0;
            if (diag && (!g.fits(cx + dx8[k], cz, foot) || !g.fits(cx, cz + dz8[k], foot)))
                continue;      // the movers refuse to cut a corner; so does this
            const int nd = bestD + (diag ? 14 : 10);
            if (nd < dist[idx(nx, nz)]) dist[idx(nx, nz)] = nd;
        }
    }
    return 0;   // unreachable
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
        // 240s, not 120: at 120 two units were still walking at full speed when the
        // clock stopped, and a censored run reads exactly like a stranded one. The
        // limit has to be past the tail or the arrival rate measures the clock.
        report(run("opposing columns", w, group, 240.0f));
        reportStranded(w, group);
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
        report(run("serpentine maze", w, group, 360.0f));
    }

    std::printf("\n  (arrival rate is the headline; travel x1.00 is the straight line)\n");
    return 0;
}
