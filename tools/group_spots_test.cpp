// Group orders and Alt+N formations, the engine features kept in both pathfinding
// modes (docs/ta-port-plan.md "Known deviations"). Asset-free.
//
//  - groupSpots (sim/groupspots.h): units within TotalA.exe's reach (0x48cf30,
//    sqrt(3000 n) px of the centre) keep TA's goal; everyone else gets the
//    nearest free footprint spot to the click, so no two units share one.
//  - A group move, fight-move and patrol issued as the client issues them
//    (tools/legion_issue_selection.h: per-unit commands in Retail, one explicit
//    order in Legion) gives every member its own destination in each mode:
//    its order's goal, or Legion's slot for the surface movers Legion plans.
//  - An Alt+1 formation (Unit::squad -1) of fast and slow walkers, and one of
//    fast and slow flyers, arrives together, paced to its slowest member,
//    where the same units without the formation arrive apart; an idle
//    straggler rejoins.
//
//   group_spots_test [retail|legion]   (no argument: both)
#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "sim/groupspots.h"
#include "legion_issue_selection.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ta::sim;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += !ok;
}
const char* modeName(PathfindingMode m) { return isLegionPathfinding(m) ? "Legion" : "Retail"; }

// --- groupSpots on its own ------------------------------------------------
struct Box { int64_t x0, z0, x1, z1; };
Box boxOf(const GroupSpotMember& m, const std::array<int32_t, 2>& g) {
    const int64_t cx = int64_t(g[0]) >> 16, cz = int64_t(g[1]) >> 16;
    return {cx - m.footX * 8, cz - m.footZ * 8, cx + m.footX * 8, cz + m.footZ * 8};
}
bool overlap(const Box& a, const Box& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.z0 < b.z1 && b.z0 < a.z1; }
bool disjoint(const std::vector<GroupSpotMember>& ms, const std::vector<std::array<int32_t, 2>>& gs) {
    for (size_t i = 0; i < ms.size(); ++i)
        for (size_t j = i + 1; j < ms.size(); ++j)
            if (ms[i].place && ms[j].place && overlap(boxOf(ms[i], gs[i]), boxOf(ms[j], gs[j]))) return false;
    return true;
}
int32_t px(int v) { return int32_t(v) * 65536; }

void pureSpots() {
    // A 6x5 block 40 px apart, all within reach: TA's goal (click + offset) each.
    std::vector<GroupSpotMember> block;
    for (int i = 0; i < 30; ++i) block.push_back({px(400 + i % 6 * 40), px(400 + i / 6 * 40), 2, 2});
    const int32_t clickX = px(1500), clickZ = px(900);
    auto goals = groupSpots(block, clickX, clickZ, 4096, 4096);
    FlyerGroupCentre centre;
    for (const auto& m : block) centre.add(m.x, m.z);
    bool ta = true;
    for (size_t i = 0; i < block.size(); ++i) {
        int32_t gx, gz;
        centre.goal(block[i].x, block[i].z, clickX, clickZ, gx, gz);
        ta &= goals[i][0] == gx && goals[i][1] == gz;
    }
    check(ta, "a block within TA's reach keeps TotalA.exe's goals (the click plus each offset)");
    // The block plus ten stragglers far away (retail: all ten to the click) and
    // four flyers stacked on one point.
    auto mixed = block;
    for (int i = 0; i < 10; ++i) mixed.push_back({px(3000 + i * 30), px(3000), 2, 2});
    for (int i = 0; i < 4; ++i) mixed.push_back({px(500), px(300), 3, 3});
    goals = groupSpots(mixed, clickX, clickZ, 4096, 4096);
    std::set<std::pair<int32_t, int32_t>> distinct;
    for (const auto& g : goals) distinct.insert({g[0], g[1]});
    check(distinct.size() == mixed.size(), "stragglers and a stack of flyers: every member its own spot");
    check(disjoint(mixed, goals), "no two spots' footprints overlap");
    int atClick = 0;
    for (const auto& g : goals) atClick += g[0] == clickX && g[1] == clickZ;
    check(atClick <= 1, "at most one member stands on the click itself");
    // The map's edge: a click in the corner keeps every spot on the map.
    goals = groupSpots(mixed, px(10), px(10), 4096, 4096);
    bool onMap = true;
    for (size_t i = 0; i < mixed.size(); ++i) {
        const Box b = boxOf(mixed[i], goals[i]);
        onMap &= b.x0 >= 0 && b.z0 >= 0 && b.x1 <= 4096 && b.z1 <= 4096;
    }
    check(onMap && disjoint(mixed, goals), "a click in the map's corner: every spot on the map, none overlapping");
    // Legion's planned movers hold room around the click: flyers keep out of it.
    std::vector<GroupSpotMember> legion;
    for (int i = 0; i < 16; ++i) legion.push_back({px(400 + i % 4 * 40), px(400 + i / 4 * 40), 2, 2, false, true});
    for (int i = 0; i < 3; ++i) legion.push_back({px(2600 + i * 50), px(2600), 2, 2});
    goals = groupSpots(legion, clickX, clickZ, 4096, 4096);
    bool clear = true, legionClick = true;
    int64_t held = 0;
    for (const auto& m : legion) if (m.reserve) held += 3 * m.footX * m.footZ;
    int64_t side = 0;
    while (side * side < held) ++side;
    const Box room{1500 - side * 8, 900 - side * 8, 1500 + side * 8, 900 + side * 8};
    for (size_t i = 0; i < legion.size(); ++i) {
        if (legion[i].place) clear &= !overlap(boxOf(legion[i], goals[i]), room);
        else legionClick &= goals[i][0] == clickX && goals[i][1] == clickZ;
    }
    check(legionClick && clear, "Legion's planned movers keep the click; the others' spots keep out of their room");
}

// --- the sim ------------------------------------------------------------------
UnitType walker(const char* id, float speed, int foot = 2) {
    UnitType t{};
    t.id = t.name = id;
    t.maxVel = Fixed::fromFloat(speed / 30);
    t.accel = t.brake = Fixed::fromInt(10);
    t.turnRate = t.turnInPlaceRate = 10000;
    t.maxHp = 100; t.canMove = true; t.footX = t.footZ = foot; t.sight = 4096; t.buildTime = 1; t.halfCellTicks = 3;
    return t;
}
UnitType flyer(const char* id, float speed) {
    UnitType t{};
    t.id = t.name = id;
    t.canFly = t.canMove = true; t.maxHp = 100; t.footX = t.footZ = 2; t.sight = 4096; t.buildTime = 1;
    t.maxVel = Fixed::fromFloat(speed / 30); t.accel = t.brake = Fixed::fromFloat(0.5f);
    t.turnRate = 1200; t.cruiseAlt = 80;
    return t;
}
struct Sim {
    World w;
    TypeRegistry reg;
    uint32_t serial = 1;
    Sim(PathfindingMode mode, bool threads = false) {
        w.setGameSeed(5); w.setSerialThreads(!threads); w.setPathfindingMode(mode);
        w.setPlayerCount(1);
        w.setTerrain(std::vector<uint8_t>(256 * 256, 100), 256, 256, 20);
        w.setPathService(true);
    }
    int spawn(const UnitType& t, float x, float z) {
        const int id = w.spawn(&t, x, z, 0.0f, 0);
        if (id <= 0) { std::printf("FAIL spawn %s at %.0f,%.0f\n", t.id.c_str(), x, z); ++failures; }
        return id;
    }
    // One click as the client issues it.
    void click(const std::vector<int>& sel, ta::scn::Verb verb, float x, float z, int squad = 0) {
        ta::scn::SelectionOrder o;
        o.verb = verb; o.x = x; o.z = z; o.squad = squad;
        for (const auto& c : ta::scn::hudItems(w, sel, o, 0, serial)) applyCommand(w, reg, c);
    }
    void tick(int n = 1) { for (int i = 0; i < n; ++i) w.tick(1.f / 30); }
};

// Where a unit is bound: Legion's slot for a mover it plans, else the goal of
// its current move-like leg (the client's UnitR::hasSpot rule).
bool destination(const World& w, const Unit& u, std::pair<int32_t, int32_t>& at) {
    Fixed x, z;
    if (w.legionDestination(u.id, x, z)) { at = {x.v, z.v}; return true; }
    for (const auto& o : u.orders)
        if (o.goal) { if (o.targetId) return false; at = {o.x.v, o.z.v}; return true; }
    return false;
}

size_t distinctCount(const std::map<int, std::pair<int32_t, int32_t>>& dest) {
    std::set<std::pair<int32_t, int32_t>> d;
    for (const auto& [id, at] : dest) d.insert(at);
    return d.size();
}

void groupOrder(PathfindingMode mode, ta::scn::Verb verb, const char* verbName) {
    Sim s(mode);
    const auto fast = walker("fast", 70), slow = walker("slow", 35), big = walker("big", 40, 10);
    const auto air = flyer("air", 240);
    std::vector<int> sel;
    for (int i = 0; i < 12; ++i) sel.push_back(s.spawn(i % 2 ? slow : fast, 600 + float(i % 4) * 40, 1000 + float(i / 4) * 40));
    for (int i = 0; i < 4; ++i) sel.push_back(s.spawn(fast, 600 + float(i) * 40, 1700));   // stragglers
    sel.push_back(s.spawn(big, 400, 1300));                                                 // an oversize body
    for (int i = 0; i < 3; ++i) sel.push_back(s.spawn(air, 600 + float(i) * 60, 700));
    s.tick(2);
    s.click(sel, verb, 2400, 1100);
    // Retail: every unit is bound for its own order's point from the click on.
    // Legion hands its surface movers' slots out over several ticks (a fight-move's
    // or patrol's as they come near): until then they hold the click.
    std::map<int, std::pair<int32_t, int32_t>> dest;
    int when = -1;
    for (int t = 0; t < 1500 && when < 0; ++t) {
        s.tick();
        dest.clear();
        for (int id : sel) {
            std::pair<int32_t, int32_t> at;
            if (destination(s.w, *s.w.unit(id), at)) dest[id] = at;
        }
        if (dest.size() == sel.size() && distinctCount(dest) == sel.size()) when = t;
    }
    std::printf("%s %s: %zu units, %zu with a destination, %zu distinct, from tick %d after the click\n",
                modeName(mode), verbName, sel.size(), dest.size(), distinctCount(dest), when);
    check(when >= 0 && (isLegionPathfinding(mode) || when == 0),
          std::string(modeName(mode)) + " " + verbName + ": every unit of the group has its own destination");
}

// Arrival ticks of an Alt+1 formation (or the same units without one).
struct Arrivals { int first = -1, last = -1; std::map<int, int> at; uint64_t hash = 0; };
Arrivals formationRun(PathfindingMode mode, bool formation, bool flyers, bool threads = false) {
    Sim s(mode, threads);
    const auto fast = flyers ? flyer("fast-air", 240) : walker("fast", 70);
    const auto slow = flyers ? flyer("slow-air", 120) : walker("slow", 35);
    std::vector<int> sel;
    for (int i = 0; i < 6; ++i) sel.push_back(s.spawn(i % 2 ? slow : fast, 500 + float(i % 3) * 48, 1000 + float(i / 3) * 48));
    s.tick(2);
    if (formation) s.click(sel, ta::scn::Verb::Squad, 0, 0, -1);
    s.tick(2);
    s.click(sel, ta::scn::Verb::Move, 2900, 1050);
    Arrivals a;
    for (int t = 0; t < 3000 && a.at.size() < sel.size(); ++t) {
        s.tick();
        for (int id : sel)
            if (!a.at.count(id) && s.w.unit(id)->orders.empty()) a.at[id] = t;
    }
    for (const auto& [id, t] : a.at) {
        a.first = a.first < 0 ? t : std::min(a.first, t);
        a.last = std::max(a.last, t);
    }
    if (a.at.size() < sel.size()) a.last = -1;
    a.hash = s.w.stateHash();
    return a;
}

void formation(PathfindingMode mode, bool flyers) {
    const char* kind = flyers ? "flyers" : "walkers";
    const Arrivals loose = formationRun(mode, false, flyers), formed = formationRun(mode, true, flyers);
    std::printf("%s %s: loose arrive %d..%d, Alt+1 formation %d..%d\n", modeName(mode), kind, loose.first, loose.last,
                formed.first, formed.last);
    const std::string tag = std::string(modeName(mode)) + " " + kind;
    check(loose.last > 0 && loose.first * 10 < loose.last * 7,
          tag + ": without a formation the fast units arrive well ahead of the slow ones");
    check(formed.last > 0 && (formed.last - formed.first) * 10 < formed.last,
          tag + ": an Alt+1 formation arrives together (within a tenth of the trip)");
    check(formed.last > 0 && formed.first * 10 > loose.last * 8,
          tag + ": the formation is paced to its slowest member (its first arrival near the slow units')");
    const Arrivals workers = formationRun(mode, true, flyers, true);
    check(workers.hash == formed.hash, tag + ": the formation's run hashes alike serial and threaded");
}

// An idle member far from its resting formation walks back to it.
void straggler(PathfindingMode mode) {
    Sim s(mode);
    const auto t = walker("soldier", 70);
    std::vector<int> sel;
    for (int i = 0; i < 4; ++i) sel.push_back(s.spawn(t, 800 + float(i % 2) * 40, 1000 + float(i / 2) * 40));
    const int far = s.spawn(t, 1800, 1000);
    sel.push_back(far);
    s.tick(2);
    s.click(sel, ta::scn::Verb::Squad, 0, 0, -1);
    s.tick(600);
    const auto& u = *s.w.unit(far);
    std::printf("%s straggler: at %.0f,%.0f\n", modeName(mode), u.x.toFloat(), u.z.toFloat());
    check(u.x.toFloat() < 1500, std::string(modeName(mode)) + ": an idle straggler of an Alt+1 formation rejoins it");
}
}  // namespace

int main(int argc, char** argv) {
    std::vector<PathfindingMode> modes{PathfindingMode::Retail, PathfindingMode::Legion};
    if (argc > 1 && !std::strcmp(argv[1], "retail")) modes = {PathfindingMode::Retail};
    if (argc > 1 && !std::strcmp(argv[1], "legion")) modes = {PathfindingMode::Legion};
    pureSpots();
    for (const auto mode : modes) {
        groupOrder(mode, ta::scn::Verb::Move, "move");
        groupOrder(mode, ta::scn::Verb::Fight, "fight-move");
        groupOrder(mode, ta::scn::Verb::Patrol, "patrol");
        formation(mode, false);
        formation(mode, true);
        straggler(mode);
    }
    std::printf("group spots: %d failures\n", failures);
    return failures ? 1 : 0;
}
