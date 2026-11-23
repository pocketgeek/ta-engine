// ballistic_line_test: the engine's 2D clear-line test is decided per weapon,
// and a unit that may not leave its post never holds a target it cannot shoot.
//
//   ballistic_line_test <retail-install-dir> [--legion]
//
// Retail admits a shot on range alone (52ae90/52fe30, no line test anywhere);
// the engine keeps a 2D clear-line test on the nav grid as a deviation
// (World::slotClearShot). Until 0.9.3 every weapon of a unit was held to it
// unless one of them was `lobpreferred`, so a plain Ballistic shell -- the
// Aramon Trebuchet's stone, every arrow in the game -- refused to fly over a
// wall its arc clears: a Trebuchet held an auto target behind its own
// buildings for 36 game-minutes without a shot (classic_ai_duels, seed
// 0x4444). Now a Ballistic weapon fires on range and its 3D arc decides, and
// a weapon whose shot flies the line keeps the test.
//
// Scenes (a flat 128x64 map, a wall of shipped CAVStalag01 stalagmites:
// 1x2, blocking, indestructible, height 60):
//   - a shipped Trebuchet (aratre) with an enemy 1200 px off behind the wall:
//     it acquires, fires and its stones land (on the target, over the wall);
//   - a shipped Aramon arrow tower (araat, two Ballistic slots) the same way at
//     350 px: it fires;
//   - a shipped Veruna tower (vertower, a line-of-sight weapon) with one enemy
//     behind the wall and one in the open shoots the open one only;
//   - the same tower holding an enemy when a row of its owner's walls goes up
//     between them: it drops it (once, with no re-acquisition) and shoots a
//     second enemy in the open.
// Each shooter has a passive spotter of its own player: towers fire through
// the sweep alone, on enemies their player sees (Guard_NoMove picks no
// target, 401a90), and "holds" means an order's or a weapon slot's target.
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "tnt/tnt.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace tak;

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

// A 128x64 map at height 30 with a column of stalagmites at `wallX`, rows
// [z0, z1).
void build(Scene& s, const hpi::Vfs& vfs, const sim::TypeRegistry& reg, int wallX, int z0, int z1) {
    s.map.width = 128;
    s.map.height = 64;
    s.map.seaLevel = 0;
    s.map.heights.assign(size_t(128) * 64, 30);
    s.map.features.assign(size_t(128) * 64, 0xffff);
    s.map.featureNames = {"CAVStalag01"};
    if (wallX >= 0)
        for (int z = z0; z < z1; ++z) s.map.features[size_t(z) * 128 + size_t(wallX)] = 0;
    s.w.setPathfindingMode(mode);
    s.w.setTerrain(s.map.heights, s.map.width, s.map.height, s.map.seaLevel, &s.map.features);
    sim::registerMapFeatures(s.w, s.map, vfs, &reg);
    s.w.setPathService(true);
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
// fire only through the sweep (Guard_NoMove picks nothing, 401a90), which
// takes only an enemy its player sees (40f330, list +5), and a shooter whose
// sight falls short of its target needs a spotter. It also keeps the
// shooter from being its player's only unit, which the sweep's walk skips.
int spotter(sim::World& w, const sim::TypeRegistry& reg, float x, float z) {
    const int id = w.spawn(reg.find("verman"), x, z, {}, 0);
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
    const sim::UnitType* sword = reg.find("arasword");
    if (!st || !sword) { check(false, "shipped units load", shooterId); return {}; }
    const int shooter = s.w.spawn(st, float(shooterCell * 16 + 8), 32 * 16, {}, 0);
    s.w.player(0).mana = 30000;
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

// A Veruna tower at (10,32). `hidden` stands at (26,32), behind the
// stalagmites at column 20, rows 16..47, or -- `wallGoesUp` -- in the open
// until the tower holds it and a row of the tower owner's shipped Aramon
// walls (arawall, 2x2) goes up at columns 20-21 between them. `open`
// stands clear of either (2,4), from the start or once the walls are up.
struct TowerOutcome {
    int hitsHidden = 0, hitsOpen = 0;
    int heldHiddenBlind = 0;  // ticks it held `hidden` while `hidden` was behind the wall
    int acquisitions = 0;     // times the tower's front target changed to a unit
};

TowerOutcome tower(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, bool wallGoesUp, int ticks) {
    Scene s;
    build(s, vfs, reg, wallGoesUp ? -1 : 20, 16, 48);
    const sim::UnitType* vt = reg.find("vertower");
    const sim::UnitType* sword = reg.find("arasword");
    const sim::UnitType* wall = reg.find("arawall");
    if (!vt || !sword || !wall) { check(false, "shipped vertower, arasword and arawall load"); return {}; }
    const int t = s.w.spawn(vt, 10 * 16 + 8, 32 * 16 + 8, {}, 0);
    s.w.player(0).mana = 30000;
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
    open = dummy(s.w, sword, 2 * 16 + 8, 4 * 16 + 8);
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

    // The Trebuchet: WEAPON1 Cannon, Ballistic, range 2700, minrange 500, not
    // lobpreferred. Its target 1200 px off, the stalagmites 400 px in front of it.
    const Fire treb = staticBehindWall(vfs, reg, "aratre", 10, 85, 35, 1800);
    const Fire trebOpen = staticBehindWall(vfs, reg, "aratre", 10, 85, -1, 1800);
    std::printf("  trebuchet, stalagmites between: %s\n", describe(treb).c_str());
    std::printf("  trebuchet, open field:     %s\n", describe(trebOpen).c_str());
    check(treb.acquired > 0 && treb.impacts >= 3,
          "a Trebuchet fires over the stalagmites at a target in range (Ballistic: no 2D line test)");
    check(treb.onTarget >= 1, "its stones clear the 60-high wall and land on the target");
    // An Aramon arrow tower (two Ballistic Arrow slots, range 500).
    const Fire at = staticBehindWall(vfs, reg, "araat", 10, 32, 20, 900);
    std::printf("  arrow tower, stalagmites between: %s\n", describe(at).c_str());
    check(at.acquired > 0 && at.impacts >= 3, "an arrow tower fires on a target behind the stalagmites");

    // A line-of-sight weapon keeps the 2D test.
    const TowerOutcome two = tower(vfs, reg, false, 900);
    std::printf("  veruna tower, one hidden + one open: %s\n", describe(two).c_str());
    check(two.hitsOpen > 0 && two.hitsHidden == 0 && two.heldHiddenBlind == 0,
          "a line-of-sight tower shoots the enemy in the open, never the one behind the stalagmites");
    const TowerOutcome walk = tower(vfs, reg, true, 1800);
    std::printf("  veruna tower, walls go up:  %s\n", describe(walk).c_str());
    check(walk.heldHiddenBlind <= 30, "it drops a target a wall now hides",
          std::to_string(walk.heldHiddenBlind) + " ticks held");
    check(walk.hitsOpen > 0, "and shoots the enemy that appears in the open");
    check(walk.acquisitions <= 2, "with no acquire/drop churn", std::to_string(walk.acquisitions) + " acquisitions");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
