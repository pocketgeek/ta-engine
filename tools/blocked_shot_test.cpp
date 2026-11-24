// blocked_shot_test: an attacker whose shots keep stopping short of its target
// on something it cannot destroy walks to a spot with a clear shot.
//
//   blocked_shot_test <retail-install-dir> [--legion]
//
// Retail admits a shot on range alone (52ae90/52fe30) and its projectile
// collision (52a4d0) stops the shot at the first terrain or feature cell it
// meets, so a retail cannon behind a hill crest empties its shells into the
// hill. The engine's 2D sight test sees only cells the nav grid blocks; a
// passable crest, water and tall scenery that does not block movement are
// invisible to it. World::noteShotImpact counts those impacts and
// World::repositionForShot moves the attacker (see sim.cpp).
//
// Shipped TA Hammers (armham, a Ballistic plasma cannon) against a Peewee
// standing behind a passable ridge, in Retail and Legion pathfinding.
// Measured: shots landing short, damage to the target, and where the Hammer
// ends up; plus the limits (Hold Position stays; an open field is
// untouched). A Pyro (corpyro, Flame Thrower) burning a Light Laser Tower
// behind an indestructible urban barrier (Barrier04) counts its flames on
// the barrier as blocked shots. A Brawler gunship behind a lava spire still
// hits: TotalA.exe's hover (AirToGroundHover 0x4139ad) steps 45 degrees
// either side of its bearing to the target on every arrival.
//
// The Kingdoms cases with no TA counterpart are gone: scenery that is tall
// but does not block movement (every TA feature with a height blocks), the
// 8x20 Taros Castle (no TA building is twice as long as it is wide), the
// Creon dragon's Blue Flame, and the Harpy's and Mind Mage's charms (TA has
// no status weapons). So is the failed-search case, which needed a
// nonblocking wall too long to walk round.
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "tnt/tnt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace ta;

namespace {
int failures = 0;
sim::PathfindingMode mode = sim::PathfindingMode::Retail;

void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}

enum class Obstacle { None, Ridge };

struct Outcome {
    int shortShots = 0;      // ground/scenery impacts well short of the target
    int targetHits = 0;      // impacts on the target
    float damage = 0;        // target HP lost
    float moved = 0;         // archer's distance from where it started
    int repositions = 0;     // most walks used at once against the target
    uint64_t legionMoves = 0; // Legion navigator move updates (Legion mode)
};

// A 64x64 map at height 30. Ridge: columns 17..23 climb 12 per cell to a
// 90-high crest and fall again (a 3x3 spread of 24: passable, and clear on
// the nav grid).
Outcome run(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, Obstacle obstacle,
            int moveState, int ticks) {
    tnt::Map map;
    map.width = map.height = 64;
    map.seaLevel = 0;
    map.heights.assign(64 * 64, 30);
    map.features.assign(64 * 64, 0xffff);
    if (obstacle == Obstacle::Ridge) {
        const int profile[] = {42, 54, 66, 78, 90, 78, 66, 54, 42};
        for (int z = 0; z < 64; ++z)
            for (int i = 0; i < 9; ++i) map.heights[size_t(z) * 64 + size_t(16 + i)] = uint8_t(profile[i]);
    }
    sim::World w;
    w.setPathfindingMode(mode);
    w.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    sim::registerMapFeatures(w, map, vfs, &reg);
    w.setPathService(true);
    const sim::UnitType* bow = reg.find("armham");
    const sim::UnitType* sword = reg.find("armpw");
    if (!bow || !sword) { check(false, "shipped armham and armpw load"); return {}; }
    const float sx = 14 * 16 + 8, sz = 32 * 16, tx = 26 * 16 + 8, tz = 32 * 16;
    const int archer = w.spawn(bow, sx, sz, 0, 0);
    const int target = w.spawn(sword, tx, tz, 0, 1);
    // The target stands still and never fights back.
    w.setStance(target, 2);
    w.unit(target)->moveState = 0;
    w.unit(target)->fireState = 0;
    w.unit(archer)->moveState = moveState;
    for (int i = 0; i < 30; ++i) w.tick(1.f / 30);
    const float hp0 = w.unit(target)->hp.toFloat();
    w.attack(archer, target, false);
    Outcome out;
    for (int i = 0; i < ticks; ++i) {
        w.tick(1.f / 30);
        const sim::Unit* a = w.unit(archer);
        const sim::Unit* t = w.unit(target);
        if (!a || !t || !t->alive()) break;
        out.repositions = std::max(out.repositions, int(a->repositions));   // a hit renews the budget
        for (const auto& h : w.hits()) {
            if (h.fromPlayer != 0) continue;
            if (h.victimId == target) { ++out.targetHits; continue; }
            const float dx = h.x - t->x.toFloat(), dz = h.z - t->z.toFloat();
            if (dx * dx + dz * dz > 48.f * 48.f) ++out.shortShots;
        }
    }
    const sim::Unit* a = w.unit(archer);
    const sim::Unit* t = w.unit(target);
    out.damage = hp0 - (t && t->alive() ? t->hp.toFloat() : 0.f);
    if (a) {
        const float dx = a->x.toFloat() - sx, dz = a->z.toFloat() - sz;
        out.moved = std::sqrt(dx * dx + dz * dz);
    }
    out.legionMoves = w.legionStats().moves;
    return out;
}

// A Pyro (corpyro, Flame Thrower, range 160) takes an enemy Light Laser
// Tower (held passive) 151 px off on its own, with an indestructible,
// movement-blocking urban barrier (Barrier04: 1x5 cells, height 80) on the
// line of fire. Its flames stop on the barrier, and only their impacts can
// show the shot is blocked.
enum class Wagon { AttackMoveHold, AttackMoveManeuver, Hold };

struct WagonOutcome {
    int stoneFlames = 0;     // flame impacts that are not on the tower
    int towerHits = 0;
    float damage = 0;
    int blockedShots = 0;    // most counted against the tower at once
    float moved = 0;
    bool attackMoveLeft = false;
};

WagonOutcome runWagon(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, Wagon variant, int ticks) {
    tnt::Map map;
    map.width = map.height = 64;
    map.seaLevel = 0;
    map.heights.assign(64 * 64, 62);
    map.features.assign(64 * 64, 0xffff);
    map.featureNames = {"Barrier04"};
    map.features[size_t(22) * 64 + 20] = 0;   // cells 20 x 22..26
    sim::World w;
    w.setPathfindingMode(mode);
    w.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    sim::registerMapFeatures(w, map, vfs, &reg);
    w.setPathService(true);
    const sim::UnitType* wagon = reg.find("corpyro");
    const sim::UnitType* lode = reg.find("armllt");
    if (!wagon || !lode) { check(false, "shipped corpyro and armllt load"); return {}; }
    const float sx = 406.0f, sz = 400.0f;
    const int tower = w.spawn(lode, 256, 384, 0, 1);
    w.blockFoot(*lode, 256, 384, true);
    w.unit(tower)->fireState = 0;
    w.unit(tower)->hp = sim::Fixed::fromInt(30000);
    for (int i = 0; i < 30; ++i) w.tick(1.f / 30);
    const int shooter = w.spawn(wagon, sx, sz, {}, 0);
    sim::Unit* u = w.unit(shooter);
    // The Pyro ships Maneuver (standingmoveorder=1); Hold Position is set.
    u->moveState = variant == Wagon::AttackMoveManeuver ? 1 : 0;
    WagonOutcome out;
    // It takes the tower on its own (fire at will): on Maneuver Standby's
    // scan puts an auto attack in, and an attack-move given then is behind
    // it; on Hold Position Standby's attack is refused
    // (4d7de0) and the sweep holds it in an automatic slot.
    const auto takes = [&](const sim::Unit& a) {
        if (!a.orders.empty() && a.orders.front().targetId == tower) return a.orders.front().autoTarget;
        return a.moveState == 0 && a.orders.empty() &&
               std::find(a.slotTarget.begin(), a.slotTarget.end(), tower) != a.slotTarget.end();
    };
    for (int i = 0; i < 90 && !takes(*u); ++i) {
        w.tick(1.f / 30);
        u = w.unit(shooter);
    }
    if (!takes(*u)) check(false, "the Pyro takes the tower on its own");
    if (variant != Wagon::Hold) w.attackMove(shooter, sx - 500, sz + 480, true);
    const float hp0 = w.unit(tower)->hp.toFloat();
    for (int i = 0; i < ticks; ++i) {
        w.tick(1.f / 30);
        const sim::Unit* a = w.unit(shooter);
        const sim::Unit* t = w.unit(tower);
        if (!a || !a->alive()) break;
        if (a->blockedTarget == tower) out.blockedShots = std::max(out.blockedShots, int(a->blockedShots));
        for (const auto& h : w.hits()) {
            if (h.fromPlayer != 0) continue;
            if (h.victimId == tower) ++out.towerHits;
            else ++out.stoneFlames;
        }
        if (!t || !t->alive()) break;
    }
    const sim::Unit* a = w.unit(shooter);
    const sim::Unit* t = w.unit(tower);
    out.damage = hp0 - (t && t->alive() ? t->hp.toFloat() : 0.f);
    if (a) {
        const float dx = a->x.toFloat() - sx, dz = a->z.toFloat() - sz;
        out.moved = std::sqrt(dx * dx + dz * dz);
        out.attackMoveLeft = !a->orders.empty() && a->orders.back().attackMove;
    }
    return out;
}

// A Brawler gunship (armbrawl, hoverattack) attacks a Peewee that stands
// just past an indestructible lava spire (LavaSpire006A: 5x4 cells, height
// 142). TotalA.exe's hover holds two thirds of its range out, 45 degrees
// either side of its bearing in turn (0x4139ad), so some rounds get past.
struct HoverOutcome {
    int stoneBolts = 0;      // impacts that are not on the Hunter
    int hunterHits = 0;
    float damage = 0;
    int repositions = 0;
    float moved = 0;
    bool dropped = false;    // the attack order was let go
};

HoverOutcome runHover(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, int ticks,
                      const char* caster = "armbrawl", int targetZ = 432) {
    tnt::Map map;
    map.width = map.height = 64;
    map.seaLevel = 0;
    map.heights.assign(64 * 64, 62);
    map.features.assign(64 * 64, 0xffff);
    map.featureNames = {"LavaSpire006A"};
    map.features[size_t(20) * 64 + 20] = 0;   // anchor: cells 20..24 x 20..23
    sim::World w;
    w.setPathfindingMode(mode);
    w.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    sim::registerMapFeatures(w, map, vfs, &reg);
    w.setPathService(true);
    const sim::UnitType* thirsha = reg.find(caster);
    const sim::UnitType* hunter = reg.find("armpw");
    if (!thirsha || !hunter) { check(false, "shipped armbrawl and armpw load"); return {}; }
    const float sx = 391, sz = 288;
    const int target = w.spawn(hunter, 384, targetZ, 0, 1);
    w.setStance(target, 2);
    w.unit(target)->moveState = 0;
    w.unit(target)->fireState = 0;
    w.unit(target)->hp = sim::Fixed::fromInt(30000);
    const int shooter = w.spawn(thirsha, sx, sz, 0, 0);
    w.unit(shooter)->moveState = 1;
    for (int i = 0; i < 30; ++i) w.tick(1.f / 30);
    const float hp0 = w.unit(target)->hp.toFloat();
    w.attack(shooter, target, false);
    HoverOutcome out;
    for (int i = 0; i < ticks; ++i) {
        w.tick(1.f / 30);
        const sim::Unit* a = w.unit(shooter);
        if (!a || !a->alive()) break;
        out.repositions = std::max(out.repositions, int(a->repositions));
        if (a->orders.empty() || a->orders.front().targetId != target) out.dropped = true;
        for (const auto& h : w.hits()) {
            if (h.fromPlayer != 0) continue;
            if (h.victimId == target) ++out.hunterHits;
            else ++out.stoneBolts;
        }
    }
    const sim::Unit* a = w.unit(shooter);
    const sim::Unit* t = w.unit(target);
    out.damage = hp0 - (t && t->alive() ? t->hp.toFloat() : 0.f);
    if (a) out.moved = std::hypot(a->x.toFloat() - sx, a->z.toFloat() - sz);
    return out;
}

std::string describe(const HoverOutcome& o) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "bolts off target=%d hits=%d damage=%.0f repositions=%d moved=%.0fpx",
                  o.stoneBolts, o.hunterHits, o.damage, o.repositions, o.moved);
    return buf;
}

std::string describe(const WagonOutcome& o) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "flames off target=%d hits=%d damage=%.0f blocked=%d moved=%.0fpx",
                  o.stoneFlames, o.towerHits, o.damage, o.blockedShots, o.moved);
    return buf;
}

std::string describe(const Outcome& o) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "short=%d hits=%d damage=%.0f moved=%.0fpx repositions=%d",
                  o.shortShots, o.targetHits, o.damage, o.moved, o.repositions);
    return buf;
}
// A write-off is of the line between two spots, not of the target. Retail
// keeps no memory of a blocked shot (52ae90 admits a shot on range alone and
// nothing feeds 52a4d0's impact back), so a retail unit shoots at an enemy in
// the open whatever it shot at before. An idle Hold Position Hammer
// auto-acquires a Peewee behind the ridge and lands its shells short until
// the target is written off from this spot. The Peewee walks off out of
// range (the Hammer lets it go) and then back over the ridge into the open
// beside the Hammer, which must acquire and engage it unordered. Returns
// {short shots before, hits after} (-1 short shots if the target was not
// written off, or was hit, before it moved; -2 if the Hammer kept it).
std::pair<int, int> runWalkOut(const hpi::Vfs& vfs, const sim::TypeRegistry& reg) {
    tnt::Map map;
    map.width = map.height = 64;
    map.seaLevel = 0;
    map.heights.assign(64 * 64, 30);
    map.features.assign(64 * 64, 0xffff);
    const int profile[] = {42, 54, 66, 78, 90, 78, 66, 54, 42};
    for (int z = 0; z < 64; ++z)
        for (int i = 0; i < 9; ++i) map.heights[size_t(z) * 64 + size_t(16 + i)] = uint8_t(profile[i]);
    sim::World w;
    w.setPathfindingMode(mode);
    w.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    sim::registerMapFeatures(w, map, vfs, &reg);
    w.setPathService(true);
    const sim::UnitType* bow = reg.find("armham");
    const sim::UnitType* sword = reg.find("armpw");
    if (!bow || !sword) return {0, 0};
    const float sz = 32 * 16;
    const int archer = w.spawn(bow, 14 * 16 + 8, sz, 0, 0);
    const int target = w.spawn(sword, 26 * 16 + 8, sz, 0, 1);
    w.setStance(target, 2);
    w.unit(target)->moveState = 0;
    w.unit(target)->fireState = 0;
    w.unit(target)->hp = sim::Fixed::fromInt(30000);
    w.unit(archer)->moveState = 0;
    int shortShots = 0, hits = 0;
    auto step = [&](int n) {
        for (int i = 0; i < n; ++i) {
            w.tick(1.f / 30);
            const sim::Unit* t = w.unit(target);
            if (!t || !t->alive()) return;
            for (const auto& h : w.hits()) {
                if (h.fromPlayer != 0) continue;
                if (h.victimId == target) { ++hits; continue; }
                const float dx = h.x - t->x.toFloat(), dz = h.z - t->z.toFloat();
                if (dx * dx + dz * dz > 48.f * 48.f) ++shortShots;
            }
        }
    };
    step(900);
    const sim::Unit* a = w.unit(archer);
    if (!a || a->blockedTarget != target || a->blockedShots < 3 || hits != 0) return {-1, hits};
    const int before = shortShots;
    // Out of range to the east: the Hold Position Hammer drops its auto
    // target (tickCombat's !mayChase && !inReach)...
    w.order(target, 60 * 16 + 8, sz, false);
    step(900);
    a = w.unit(archer);
    if (!a || !a->orders.empty()) return {-2, 0};
    // ...then back over the ridge into the open beside it.
    hits = 0;
    w.order(target, 9 * 16 + 8, sz + 160, false);
    step(1200);
    return {before, hits};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: blocked_shot_test <retail-install-dir> [--legion]\n"); return 2; }
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--legion") mode = sim::PathfindingMode::Legion;
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
    reg.loadDir(vfs, "units/");
    std::printf("[%s pathfinding]\n", mode == sim::PathfindingMode::Legion ? "Legion" : "Retail");
    const int kTicks = 900;   // 30 s: about fifteen Hammer shells

    const Outcome open = run(vfs, reg, Obstacle::None, 1, kTicks);
    std::printf("  open field:  %s\n", describe(open).c_str());
    check(open.targetHits > 0 && open.repositions == 0 && open.moved < 8,
          "an open shot connects from where the Hammer stands, no reposition");

    {
        const Outcome held = run(vfs, reg, Obstacle::Ridge, 0, kTicks);
        const Outcome free = run(vfs, reg, Obstacle::Ridge, 1, kTicks);
        std::printf("  ridge      hold position: %s\n", describe(held).c_str());
        std::printf("  ridge      maneuver:      %s\n", describe(free).c_str());
        // An ordered attack walks to a clear shot on Hold Position (Defensive) too.
        check(held.shortShots >= 3 && held.repositions >= 1 && held.damage > 0,
              "ridge: the shot is blocked, and an ordered Hold Position Hammer walks to a clear shot");
        // TotalA.exe's shells (0x49cde0 on AimX's angles, 0x49bce3) take a second
        // walk here: up to two blocked spots' worth of short shots.
        check(free.repositions >= 1 && free.damage > 0 && free.shortShots <= 2 * 4,
              "ridge: the Hammer walks to a clear shot and hurts the target");
        if (mode == sim::PathfindingMode::Legion)
            check(free.legionMoves > 0 && held.legionMoves > 0, "the walks are Legion's",
                  std::to_string(free.legionMoves) + "/" + std::to_string(held.legionMoves));
    }
    // Pyro flames against a tower behind a barrier.
    const WagonOutcome hold = runWagon(vfs, reg, Wagon::Hold, kTicks);
    const WagonOutcome onMove = runWagon(vfs, reg, Wagon::AttackMoveHold, 1800);
    const WagonOutcome maneuver = runWagon(vfs, reg, Wagon::AttackMoveManeuver, 1800);
    std::printf("  pyro hold, idle:        %s\n", describe(hold).c_str());
    std::printf("  pyro hold, attack-move: %s\n", describe(onMove).c_str());
    std::printf("  pyro maneuver, a-move:  %s\n", describe(maneuver).c_str());
    check(hold.stoneFlames >= 3 && hold.blockedShots >= 3,
          "flame impacts on the barrier count as blocked shots against the tower");
    check(hold.damage == 0 && hold.moved < 8, "Hold Position with nothing queued stays and fires on");
    // A Flame Thrower shot is a stream of particles, each its own impact, so
    // the bound is in write-offs: the idle case's count is one (17). On an
    // attack-move it may write off one more spot on its way down the line of
    // fire (Legion's path does), so at most two.
    const int writeOff = std::max(hold.stoneFlames, 1);
    check(onMove.damage > 0 && onMove.stoneFlames <= 2 * writeOff,
          "Hold Position on an attack-move leaves the blocked spot and burns the tower");
    check(maneuver.damage > 0 && maneuver.stoneFlames <= 2 * writeOff,
          "Maneuver on an attack-move walks to a clear shot and burns the tower");
    // A Brawler hovering behind a lava spire.
    const HoverOutcome hover = runHover(vfs, reg, 1800);
    std::printf("  brawler hover, spire:  %s\n", describe(hover).c_str());
    check(hover.hunterHits > 0 && hover.damage > 0,
          "the Brawler reaches a clear bearing and hits the Peewee");
    const auto [blockedBefore, hitsAfter] = runWalkOut(vfs, reg);
    std::printf("  walk-out:   %d short before, %d hits after it walked into the open\n", blockedBefore, hitsAfter);
    check(blockedBefore >= 3 && hitsAfter > 0,
          "an idle Hold Position Hammer engages an enemy it found blocked once it walks into the open",
          "short=" + std::to_string(blockedBefore) + " hits=" + std::to_string(hitsAfter));
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
