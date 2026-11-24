// ballistic_line_test: the engine's 2D clear-line test is decided per weapon,
// and a unit that may not leave its post never holds a target it cannot shoot.
//
//   ballistic_line_test <retail-install-dir> [--legion]
//
// Retail admits a shot on range alone (KINGDOMS.icd 52ae90/52fe30, no line
// test anywhere; unverified for TA); the engine keeps a 2D clear-line test on
// the nav grid as a deviation (World::slotClearShot). It once held every
// weapon of a unit to it unless one of them was `lobpreferred`, so a plain
// Ballistic shell refused to fly over a wall its arc clears: a long-range
// artillery piece held an auto target behind its own buildings for minutes
// without a shot. Now a Ballistic weapon fires on range and its 3D arc
// decides, and a weapon whose shot flies the line keeps the test.
//
// Scenes (a flat 128x64 map, a wall of shipped Fortification features --
// TA's Dragon's Teeth: 2x2, blocking, height 55):
//   - a shipped Guardian (armguard, Ballistic Plasma Cannon, range 1250) with
//     an enemy 1040 px off behind the wall: it acquires, fires and its shells
//     land (on the target, over the wall);
//   - a shipped Punisher (corpun, Ballistic, range 1200) the same way at
//     256 px: it fires;
//   - a shipped L.L.T. (armllt, a line-of-sight Light Laser, range 300) with
//     one enemy behind the wall and one in the open shoots the open one only;
//   - the same tower holding an enemy when a row of its owner's Dragon's
//     Teeth (armdrag) goes up between them: it drops it (once, with no
//     re-acquisition) and shoots a second enemy in the open.
// Each shooter has a passive spotter of its own player: towers fire through
// the sweep alone, on enemies their player sees (Guard_NoMove picks no
// target), and "holds" means an order's or a weapon slot's target. Player 0
// starts with ample energy (the L.L.T. pays 10 a shot).
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "tnt/tnt.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace ta;

namespace {
int failures = 0;
sim::PathfindingMode mode = sim::PathfindingMode::Retail;

void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}

struct Scene {
    tnt::Map map;
    sim::World w;
};

// A 128x64 map at height 30 with a column of Fortifications (2x2) at
// `wallX`, rows [z0, z1).
void build(Scene& s, const hpi::Vfs& vfs, const sim::TypeRegistry& reg, int wallX, int z0, int z1) {
    s.map.width = 128;
    s.map.height = 64;
    s.map.seaLevel = 0;
    s.map.heights.assign(size_t(128) * 64, 30);
    s.map.features.assign(size_t(128) * 64, 0xffff);
    s.map.featureNames = {"Fortification"};
    if (wallX >= 0)
        for (int z = z0; z < z1; z += 2) s.map.features[size_t(z) * 128 + size_t(wallX)] = 0;
    s.w.setPathfindingMode(mode);
    s.w.setTerrain(s.map.heights, s.map.width, s.map.height, s.map.seaLevel, &s.map.features);
    sim::registerMapFeatures(s.w, s.map, vfs, &reg);
    s.w.setPathService(true);
    s.w.setStartResources(0, 100000, 100000);
}

// A target that stands where it is put, never fights back and does not die.
int dummy(sim::World& w, const sim::UnitType* t, float x, float z) {
    const int id = w.spawn(t, x, z, {}, 1);
    w.setStance(id, 2);
    w.unit(id)->moveState = 0;
    w.unit(id)->fireState = 0;
    w.unit(id)->hp = sim::Fixed::fromInt(30000);
    return id;
}

// A passive unit of the shooter's player beside `x, z`: retail's towers
// fire only through the sweep (Guard_NoMove picks nothing), which takes
// only an enemy its player sees, and a shooter whose
// sight falls short of its target needs a spotter. It also keeps the
// shooter from being its player's only unit, which the sweep's walk skips.
int spotter(sim::World& w, const sim::TypeRegistry& reg, float x, float z) {
    const int id = w.spawn(reg.find("armpw"), x, z, {}, 0);
    w.setStance(id, 2);
    w.unit(id)->moveState = 0;
    w.unit(id)->fireState = 0;
    w.unit(id)->hp = sim::Fixed::fromInt(30000);
    return id;
}

// Whether u holds t: its front order's target or a weapon slot's (a tower
// holds a sweep target in its automatic slots, with no order).
bool holds(const sim::Unit& u, int t) {
    if (!u.orders.empty() && u.orders.front().targetId == t) return true;
    for (int s : u.slotTarget) if (s == t) return true;
    return false;
}
int held(const sim::Unit& u) {
    if (!u.orders.empty() && u.orders.front().targetId) return u.orders.front().targetId;
    for (int s : u.slotTarget) if (s) return s;
    return 0;
}

struct Fire {
    int impacts = 0;     // every impact of the shooter's shots
    int onTarget = 0;    // impacts on the target
    int acquired = 0;    // ticks the shooter held the target
};

// A static shooter (player 0, fire at will) and one enemy behind the wall.
Fire staticBehindWall(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, const char* shooterId,
                      int shooterCell, int targetCell, int wallX, int ticks) {
    Scene s;
    build(s, vfs, reg, wallX, 16, 48);
    const sim::UnitType* st = reg.find(shooterId);
    const sim::UnitType* sword = reg.find("corak");
    if (!st || !sword) { check(false, "shipped units load", shooterId); return {}; }
    const int shooter = s.w.spawn(st, float(shooterCell * 16 + 8), 32 * 16, {}, 0);
    const int target = dummy(s.w, sword, float(targetCell * 16 + 8), 32 * 16);
    spotter(s.w, reg, float(targetCell * 16 + 8), 40 * 16);
    Fire f;
    for (int i = 0; i < ticks; ++i) {
        s.w.tick(1.f / 30);
        const sim::Unit* u = s.w.unit(shooter);
        if (u && holds(*u, target)) ++f.acquired;
        for (const auto& h : s.w.hits()) {
            if (h.fromPlayer != 0) continue;
            ++f.impacts;
            if (h.victimId == target) ++f.onTarget;
        }
    }
    return f;
}

std::string describe(const Fire& f) {
    return "impacts=" + std::to_string(f.impacts) + " on target=" + std::to_string(f.onTarget) +
           " held=" + std::to_string(f.acquired / 30) + "s";
}

// An L.L.T. at (10,32). `hidden` stands at (26,32), behind the
// Fortifications at column 20, rows 16..47, or -- `wallGoesUp` -- in the open
// until the tower holds it and a row of the tower owner's shipped Dragon's
// Teeth (armdrag, 2x2) goes up at columns 20-21 between them. `open`
// stands clear of either (4,20), from the start or once the walls are up.
struct TowerOutcome {
    int hitsHidden = 0, hitsOpen = 0;
    int heldHiddenBlind = 0;  // ticks it held `hidden` while `hidden` was behind the wall
    int acquisitions = 0;     // times the tower's front target changed to a unit
};

TowerOutcome tower(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, bool wallGoesUp, int ticks) {
    Scene s;
    build(s, vfs, reg, wallGoesUp ? -1 : 20, 16, 48);
    const sim::UnitType* vt = reg.find("armllt");
    const sim::UnitType* sword = reg.find("corak");
    const sim::UnitType* wall = reg.find("armdrag");
    if (!vt || !sword || !wall) { check(false, "shipped armllt, corak and armdrag load"); return {}; }
    const int t = s.w.spawn(vt, 10 * 16 + 8, 32 * 16 + 8, {}, 0);
    spotter(s.w, reg, 4 * 16 + 8, 32 * 16 + 8);
    TowerOutcome out;
    const int hidden = dummy(s.w, sword, 26 * 16 + 8, 32 * 16 + 8);
    int open = 0;
    if (wallGoesUp) {
        for (int i = 0; i < 90; ++i) s.w.tick(1.f / 30);
        const sim::Unit* u = s.w.unit(t);
        check(u && holds(*u, hidden),
              "the tower acquires the enemy while it stands in the open");
        for (int z = 16; z < 48; z += 2) {   // spawn() leaves nav to its caller, as a placement does
            s.w.spawn(wall, 21 * 16, float(z * 16 + 16), {}, 0);
            s.w.blockFoot(*wall, 21 * 16, float(z * 16 + 16), true);
        }
    }
    open = dummy(s.w, sword, 4 * 16 + 8, 20 * 16 + 8);
    int last = -1;
    for (int i = 0; i < ticks; ++i) {
        s.w.tick(1.f / 30);
        const sim::Unit* u = s.w.unit(t);
        const int front = u ? held(*u) : 0;
        if (front != last && front) ++out.acquisitions;
        last = front;
        if (front == hidden) ++out.heldHiddenBlind;
        for (const auto& hit : s.w.hits()) {
            if (hit.fromPlayer != 0) continue;
            if (hit.victimId == hidden) ++out.hitsHidden;
            if (hit.victimId == open) ++out.hitsOpen;
        }
    }
    return out;
}

std::string describe(const TowerOutcome& o) {
    return "hits hidden=" + std::to_string(o.hitsHidden) + " open=" + std::to_string(o.hitsOpen) +
           " held hidden blind=" + std::to_string(o.heldHiddenBlind / 30) + "s acquisitions=" +
           std::to_string(o.acquisitions);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: ballistic_line_test <retail-install-dir> [--legion]\n"); return 2; }
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--legion") mode = sim::PathfindingMode::Legion;
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
    reg.loadDir(vfs, "units/");
    std::printf("[%s pathfinding]\n", mode == sim::PathfindingMode::Legion ? "Legion" : "Retail");

    // The Guardian: Ballistic Plasma Cannon, range 1250, not lobpreferred.
    // Its target 1040 px off (past ~1090 px its 350 px/s shell has no arc on
    // flat ground, short of the 1250 range), the Fortifications 400 px in front.
    const Fire treb = staticBehindWall(vfs, reg, "armguard", 10, 75, 35, 1800);
    const Fire trebOpen = staticBehindWall(vfs, reg, "armguard", 10, 75, -1, 1800);
    std::printf("  guardian, fortifications between: %s\n", describe(treb).c_str());
    std::printf("  guardian, open field:     %s\n", describe(trebOpen).c_str());
    check(treb.acquired > 0 && treb.impacts >= 3,
          "a Guardian fires over the Fortifications at a target in range (Ballistic: no 2D line test)");
    check(treb.onTarget >= 1, "its shells clear the 55-high wall and land on the target");
    // A Punisher (Ballistic, range 1200) close behind the wall.
    const Fire at = staticBehindWall(vfs, reg, "corpun", 10, 26, 20, 900);
    std::printf("  punisher, fortifications between: %s\n", describe(at).c_str());
    check(at.acquired > 0 && at.impacts >= 3, "a Punisher fires on a target behind the Fortifications");

    // A line-of-sight weapon keeps the 2D test.
    const TowerOutcome two = tower(vfs, reg, false, 900);
    std::printf("  L.L.T., one hidden + one open: %s\n", describe(two).c_str());
    check(two.hitsOpen > 0 && two.hitsHidden == 0 && two.heldHiddenBlind == 0,
          "a line-of-sight tower shoots the enemy in the open, never the one behind the Fortifications");
    const TowerOutcome walk = tower(vfs, reg, true, 1800);
    std::printf("  L.L.T., walls go up:  %s\n", describe(walk).c_str());
    check(walk.heldHiddenBlind <= 30, "it drops a target a wall now hides",
          std::to_string(walk.heldHiddenBlind) + " ticks held");
    check(walk.hitsOpen > 0, "and shoots the enemy that appears in the open");
    check(walk.acquisitions <= 2, "with no acquire/drop churn", std::to_string(walk.acquisitions) + " acquisitions");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
