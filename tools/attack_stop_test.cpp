// attack_stop_test: an attack order on a unit aims by the time the walk to
// the target has stopped, and fires from there.
//
//   attack_stop_test <retail-install-dir> [--legion]
//
// The Kingdoms rule this test was written for (Attack_Chase, KINGDOMS.icd
// 403bb0, gave the slot its target only two ticks after the arrival stopped
// the body) is unverified for TA: TotalA.exe's weapon update aims a slot as
// soon as it holds a target (docs/retail-weapon-scripts.md section 2).
//
// Checks (flat map, shipped TA units, a passive Peewee target):
//   - a Hammer (armham, range 320) ordered from 600 px walks in, aims no
//     later than two ticks after it stops, and fires within 5 s of stopping;
//   - an A.K. (corak, a line-of-sight laser) ordered from 600 px does too.
//     TotalA.exe's weapon update starts AimX as soon as the slot holds a
//     target and no aim is outstanding (docs/retail-weapon-scripts.md
//     section 2), so a TA walker may aim while it brakes in reach: the bound
//     is the stop, not the Kingdoms rule that the aim waits for it;
//   - a Hammer ordered in reach while facing away aims on the order's tick
//     (stage 1 runs at its first dispatch) and fires: its turret aims, so
//     the body need not pivot.
// The Kingdoms case this began with (Kirenna, whose walk controller and
// TurnDirection static stalled her aim) has no TA counterpart: TA's
// scripts have no walk controller waiting on the stop, and only a
// non-turret line-of-sight weapon (0x49d9c0) needs the body to face.
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "tnt/tnt.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace ta::sim {
struct RetailReplayProbe {
    static const auto& scriptStatics(const World& world, int id) { return world.unitScripts_.at(id).peek().vm.statics; }
};
}  // namespace ta::sim

using namespace ta;

namespace {
int failures = 0;
sim::PathfindingMode mode = sim::PathfindingMode::Retail;

void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}

constexpr float kTick = 1.f / 30;

struct Scene {
    tnt::Map map;
    sim::World w;
};

void build(Scene& s, const hpi::Vfs& vfs, const sim::TypeRegistry& reg) {
    s.map.width = s.map.height = 96;
    s.map.seaLevel = 0;
    s.map.heights.assign(size_t(96) * 96, 70);
    s.map.features.assign(size_t(96) * 96, 0xffff);
    s.w.setPathfindingMode(mode);
    s.w.setTerrain(s.map.heights, s.map.width, s.map.height, s.map.seaLevel, &s.map.features);
    sim::registerMapFeatures(s.w, s.map, vfs, &reg);
    s.w.setPathService(true);
}

struct Run {
    int order = -1;         // tick the attack was given
    int stop = -1;          // first tick it stood still after walking
    int aim = -1;           // first tick its weapon started aiming
    int turned = -1;        // first tick its heading changed after the order
    int fired = -1;         // first tick it fired
    int turnHeld = 0;       // ticks its script's TurnDirection static was not 0
    int turnValues = 0;     // ... the distinct values it held
    int turnAfter = 0;      // ... that static at the end
    float dist = 0;         // centre distance at the end
};

std::string describe(const Run& r) {
    char b[200];
    std::snprintf(b, sizeof b, "order %d, stop %d, aim %d, turned %d, fired %d, %.1f px off", r.order, r.stop, r.aim,
                  r.turned, r.fired, r.dist);
    return b;
}

// A shooter of player 0 `dist` px from a passive Peewee of player 1 along
// +x, heading `heading` (radians; pi/2 faces the target), is ordered to attack
// it and watched for `ticks`. `fireState` 2 is fire at will; 0 (hold fire)
// keeps it from turning to a target in reach before the order. `turnStatic`
// >= 0: the shooter's script static TurnDirection writes.
Run attack(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, const char* shooterId, float dist, float heading,
           int ticks, int fireState = 2, int turnStatic = -1) {
    Scene s;
    build(s, vfs, reg);
    Run r;
    const sim::UnitType* st = reg.find(shooterId);
    const sim::UnitType* tt = reg.find("armpw");
    if (!st || !tt) { check(false, std::string("shipped ") + shooterId + " and armpw load"); return r; }
    const float sx = 300, sz = 768;
    const int target = s.w.spawn(tt, sx + dist, sz, 0.f, 1);
    s.w.setStance(target, 2);
    s.w.unit(target)->moveState = 0;
    s.w.unit(target)->fireState = 0;
    s.w.unit(target)->hp = sim::Fixed::fromInt(30000);
    const int shooter = s.w.spawn(st, sx, sz, heading, 0);
    s.w.unit(shooter)->fireState = fireState;
    for (int i = 0; i < 30; ++i) s.w.tick(kTick);   // Create's controllers settle
    r.order = int(s.w.tickCount());
    s.w.attack(shooter, target, false);
    const auto start = s.w.unit(shooter)->heading;
    bool walked = false;
    std::set<uint32_t> turns;
    for (int i = 0; i < ticks; ++i) {
        const int now = int(s.w.tickCount());
        s.w.tick(kTick);
        const sim::Unit* u = s.w.unit(shooter);
        const sim::Unit* t = s.w.unit(target);
        if (!u || !t) break;
        if (u->speed.v) walked = true;
        else if (walked && r.stop < 0) r.stop = now;
        const int slot = 0;
        if (r.aim < 0 && (u->weaponAim[size_t(slot)].flags & ta::TaAimState::kRequested)) r.aim = now;
        if (r.turned < 0 && u->heading != start) r.turned = now;
        if (r.fired < 0 && u->firedWeapons) r.fired = now;
        if (turnStatic >= 0) {
            const auto& statics = sim::RetailReplayProbe::scriptStatics(s.w, shooter);
            if (statics[size_t(turnStatic)]) { ++r.turnHeld; turns.insert(statics[size_t(turnStatic)]); }
            r.turnAfter = int(statics[size_t(turnStatic)]);
        }
        r.dist = std::hypot((t->x - u->x).toFloat(), (t->z - u->z).toFloat());
    }
    r.turnValues = int(turns.size());
    return r;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: attack_stop_test <retail-install-dir> [--legion]\n"); return 2; }
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--legion") mode = sim::PathfindingMode::Legion;
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    sim::setupRegistry(reg,vfs);
    std::printf("[%s pathfinding]\n", mode == sim::PathfindingMode::Legion ? "Legion" : "Retail");
    constexpr float kFacing = 1.5707963f;

    // A walker aims by the time it stops, and fires from there.
    for (const char* id : {"armham", "corak"}) {
        const Run r = attack(vfs, reg, id, 600, kFacing, 600);
        std::printf("  %s from 600 px: %s\n", id, describe(r).c_str());
        check(r.stop > r.order && r.aim > r.order && r.aim <= r.stop + 2 && r.fired > r.aim,
              std::string(id) + " aims no later than two ticks after its walk stops, and fires", describe(r));
        check(r.fired <= r.stop + 150 && r.dist <= float(reg.find(id)->weapons[0].range) + 0.5f,
              std::string(id) + " fires within 5 s of stopping, from within its range", describe(r));
    }

    // In reach from the start, facing away: stage 1 runs at once, so it aims
    // on the order's tick, and its turret lets it fire.
    {
        const Run r = attack(vfs, reg, "armham", 200, -kFacing, 300, 0);
        std::printf("  armham in reach, facing away: %s\n", describe(r).c_str());
        check(r.aim == r.order, "ordered in reach it aims at once", describe(r));
        check(r.fired > r.aim, "and it fires", describe(r));
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
