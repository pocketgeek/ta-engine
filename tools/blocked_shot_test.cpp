// blocked_shot_test: an attacker whose shots keep stopping short of its target
// on something it cannot destroy walks to a spot with a clear shot.
//
//   blocked_shot_test <retail-install-dir> [--legion]
//
// Retail admits a shot on range alone (52ae90/52fe30) and its projectile
// collision (52a4d0) stops the shot at the first terrain or feature cell it
// meets, so a retail archer behind a hill crest empties its quiver into the
// hill. The engine's 2D sight test sees only cells the nav grid blocks; a
// passable crest, water and tall scenery that does not block movement are
// invisible to it. World::noteShotImpact counts those impacts and
// World::repositionForShot moves the attacker (see sim.cpp).
//
// Shipped Aramon Archers (arabow, a ballistic Standard Arrow) against a
// Swordsman standing behind a passable ridge, and behind a row of shipped
// Zhon campfire art (ZonFireD02: indestructible, nonblocking, 'height'
// set), in Retail and Legion pathfinding. Measured: shots landing short,
// damage to the target, and where the archer ends up; plus the limits
// (Hold Position stays; an open field is untouched); the same archer
// against a Taros Castle behind a crest in front of its long face. A hovering Thirsha
// whose Lightning stops on a standing stone moves round its hover ring
// (World::hoverForShot). A Harpy, and a ground Mind Mage, whose charms strike
// a Barracks standing in front of their target give the target up
// (World::statusShotRefused).
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

enum class Obstacle { None, Ridge, Campfires };

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
// the nav grid). Campfires: column 19, rows 26..38, ZonFireD02 (height 50;
// an arrow aimed at a Swordsman's drawn SweetSpot clears the 25-high
// ZonFireD01 since protocol 295).
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
    } else if (obstacle == Obstacle::Campfires) {
        map.featureNames = {"ZonFireD02"};
        for (int z = 20; z <= 44; ++z) map.features[size_t(z) * 64 + 19] = 0;
    }
    sim::World w;
    w.setPathfindingMode(mode);
    w.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    sim::registerMapFeatures(w, map, vfs, &reg);
    w.setPathService(true);
    const sim::UnitType* bow = reg.find("arabow");
    const sim::UnitType* sword = reg.find("arasword");
    if (!bow || !sword) { check(false, "shipped arabow and arasword load"); return {}; }
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

// A failed search writes off the spot, not the target. The archer attacks a
// Swordsman behind a full-height campfire wall too far off to walk round
// (no spot: it fires on), then the Swordsman walks west to stand behind the
// short campfire row: a spot with a clear shot is in reach again, and the
// archer must walk to it. Returns {walks, hits} after the target moved
// (walks -1 if it walked before: the wall was meant to leave no spot).
std::pair<int, int> runSticky(const hpi::Vfs& vfs, const sim::TypeRegistry& reg) {
    tnt::Map map;
    map.width = map.height = 64;
    map.seaLevel = 0;
    map.heights.assign(64 * 64, 30);
    map.features.assign(64 * 64, 0xffff);
    map.featureNames = {"ZonFireD02"};
    for (int z = 0; z < 64; ++z) map.features[size_t(z) * 64 + 34] = 0;
    for (int z = 20; z <= 44; ++z) map.features[size_t(z) * 64 + 19] = 0;
    sim::World w;
    w.setPathfindingMode(mode);
    w.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    sim::registerMapFeatures(w, map, vfs, &reg);
    w.setPathService(true);
    const sim::UnitType* bow = reg.find("arabow");
    const sim::UnitType* sword = reg.find("arasword");
    if (!bow || !sword) return {0, 0};
    const float sz = 32 * 16;
    const int archer = w.spawn(bow, 14 * 16 + 8, sz, 0, 0);
    const int target = w.spawn(sword, 44 * 16 + 8, sz, 0, 1);
    w.setStance(target, 2);
    w.unit(target)->moveState = 0;
    w.unit(target)->fireState = 0;
    w.unit(target)->hp = sim::Fixed::fromInt(30000);
    w.unit(archer)->moveState = 1;
    for (int i = 0; i < 30; ++i) w.tick(1.f / 30);
    w.attack(archer, target, false);
    int walks = 0, hits = 0;
    bool wasMove = false;
    auto step = [&](int n) {
        for (int i = 0; i < n; ++i) {
            w.tick(1.f / 30);
            const sim::Unit* a = w.unit(archer);
            for (const auto& h : w.hits())
                if (h.fromPlayer == 0 && h.victimId == target) ++hits;
            const bool m = a && !a->orders.empty() && a->orders.front().targetId == 0;
            if (m && !wasMove) ++walks;
            wasMove = m;
        }
    };
    step(900);
    const int firstWalks = walks;
    walks = hits = 0;
    w.order(target, 26 * 16 + 8, sz, false);
    step(1800);
    return {firstWalks == 0 ? walks : -1, hits};
}

// The user's 0.9.1 game: a Creon Fire Wagon (crefire, a flame weapon; the
// FBI's standingunitorder=1 makes it Hold Position) on an attack-move
// auto-acquires an Aramon Lodestone 344 px off, with a Zhon standing stone
// (ZonHenge04: 1x2, indestructible, height 60) on the line of fire. The 2D
// sight walk passes beside the one-cell stone, so only the flames' impacts
// can show the shot is blocked. The geometry is the game's, moved by whole
// cells (224,480).
enum class Wagon { AttackMoveHold, AttackMoveManeuver, Hold };

struct WagonOutcome {
    int stoneFlames = 0;     // flame impacts that are not on the lodestone
    int lodestoneHits = 0;
    float damage = 0;
    int blockedShots = 0;    // most counted against the lodestone at once
    float moved = 0;
    bool attackMoveLeft = false;
};

WagonOutcome runWagon(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, Wagon variant, int ticks) {
    tnt::Map map;
    map.width = map.height = 64;
    map.seaLevel = 0;
    map.heights.assign(64 * 64, 62);
    map.features.assign(64 * 64, 0xffff);
    map.featureNames = {"ZonHenge04"};
    map.features[size_t(23) * 64 + 20] = 0;
    sim::World w;
    w.setPathfindingMode(mode);
    w.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    sim::registerMapFeatures(w, map, vfs, &reg);
    w.setPathService(true);
    const sim::UnitType* wagon = reg.find("crefire");
    const sim::UnitType* lode = reg.find("aralode");
    if (!wagon || !lode) { check(false, "shipped crefire and aralode load"); return {}; }
    const float sx = 591.3f, sz = 456.0f;
    const int lodestone = w.spawn(lode, 256, 384, 0, 1);
    for (int i = 0; i < 30; ++i) w.tick(1.f / 30);
    const int shooter = w.spawn(wagon, sx, sz, {}, 0);
    // A spotter of the wagon's beside the lodestone, as in the game (its sight
    // is 180, the lodestone 344 px off): the sweep and the scans take only
    // what the player sees.
    static sim::UnitType spotter;
    spotter = *wagon; spotter.id = "spotter"; spotter.weapons.clear(); spotter.weapon = {};
    w.spawn(&spotter, 256, 264, {}, 0);
    w.player(0).mana = 30000;
    sim::Unit* u = w.unit(shooter);
    if (variant == Wagon::AttackMoveManeuver) u->moveState = 1;
    WagonOutcome out;
    if (u->moveState != (variant == Wagon::AttackMoveManeuver ? 1 : 0))
        check(false, "crefire ships Hold Position (standingunitorder=1)");
    // It takes the lodestone on its own (fire at will): on Maneuver Standby's
    // scan puts an auto attack in, and the attack-move the AI gave it is then
    // behind it, as in the game; on Hold Position Standby's attack is refused
    // (4d7de0) and the sweep holds it in an automatic slot.
    const auto takes = [&](const sim::Unit& a) {
        if (!a.orders.empty() && a.orders.front().targetId == lodestone) return a.orders.front().autoTarget;
        return a.moveState == 0 && a.orders.empty() &&
               std::find(a.slotTarget.begin(), a.slotTarget.end(), lodestone) != a.slotTarget.end();
    };
    for (int i = 0; i < 90 && !takes(*u); ++i) {
        w.tick(1.f / 30);
        u = w.unit(shooter);
    }
    if (!takes(*u)) check(false, "the wagon takes the lodestone on its own");
    if (variant != Wagon::Hold) w.attackMove(shooter, sx - 500, sz + 480, true);
    const float hp0 = w.unit(lodestone)->hp.toFloat();
    for (int i = 0; i < ticks; ++i) {
        w.tick(1.f / 30);
        const sim::Unit* a = w.unit(shooter);
        const sim::Unit* t = w.unit(lodestone);
        if (!a || !a->alive()) break;
        if (a->blockedTarget == lodestone) out.blockedShots = std::max(out.blockedShots, int(a->blockedShots));
        for (const auto& h : w.hits()) {
            if (h.fromPlayer != 0) continue;
            if (h.victimId == lodestone) ++out.lodestoneHits;
            else ++out.stoneFlames;
        }
        if (!t || !t->alive()) break;
    }
    const sim::Unit* a = w.unit(shooter);
    const sim::Unit* t = w.unit(lodestone);
    out.damage = hp0 - (t && t->alive() ? t->hp.toFloat() : 0.f);
    if (a) {
        const float dx = a->x.toFloat() - sx, dz = a->z.toFloat() - sz;
        out.moved = std::sqrt(dx * dx + dz * dz);
        out.attackMoveLeft = !a->orders.empty() && a->orders.back().attackMove;
    }
    return out;
}

// The user's 0.9.10 game (~4:28): a Thirsha (zonhunt) hover-attacks a Zhon
// Hunter that stands just past a Zhon standing stone (ZonHenge11: 5x4,
// indestructible, height 94). From its hover point 150 px north of the
// Hunter at attack altitude every Lightning bolt stops on the stone's top
// (52a4d0), and retail's hover mission (41de5d) keeps that bearing at every
// wake: in retail it fires into the stone for as long as the order lasts.
// The engine moves it round the ring to a clear bearing. The geometry is the
// game's, moved by whole cells (-2096,-768).
struct HoverOutcome {
    int stoneBolts = 0;      // impacts that are not on the Hunter
    int hunterHits = 0;
    float damage = 0;
    int repositions = 0;
    float moved = 0;
    bool dropped = false;    // the attack order was let go
};

// `caster` "credrag": the Creon dragon owns Freezing Breath beside its Blue
// Flame, and a hovering status caster whose charms strike a body they cannot
// take drops its target (runHarpy). Its Blue Flame is no status shot: stopped
// by the stone it hops round its ring like the Thirsha. Its stream clears the
// stone's top onto a Hunter at the game's spot, so for it the Hunter stands
// 22 px nearer the stone, where every flame from the first hover point stops.
HoverOutcome runHover(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, int ticks,
                      const char* caster = "zonhunt", int targetZ = 432) {
    tnt::Map map;
    map.width = map.height = 64;
    map.seaLevel = 0;
    map.heights.assign(64 * 64, 62);
    map.features.assign(64 * 64, 0xffff);
    map.featureNames = {"ZonHenge11"};
    map.features[size_t(20) * 64 + 20] = 0;   // anchor: cells 20..24 x 20..23
    sim::World w;
    w.setPathfindingMode(mode);
    w.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    sim::registerMapFeatures(w, map, vfs, &reg);
    w.setPathService(true);
    const sim::UnitType* thirsha = reg.find(caster);
    const sim::UnitType* hunter = reg.find("zonter");
    if (!thirsha || !hunter) { check(false, "shipped hover casters and zonter load"); return {}; }
    const float sx = 391, sz = 288;
    const int target = w.spawn(hunter, 384, targetZ, 0, 1);
    w.setStance(target, 2);
    w.unit(target)->moveState = 0;
    w.unit(target)->fireState = 0;
    w.unit(target)->hp = sim::Fixed::fromInt(30000);
    const int shooter = w.spawn(thirsha, sx, sz, 0, 0);
    // The Thirsha ships Hold Position, on which its standby takes no attack
    // (hold_flyer_test). On Maneuver it is already hovering in on the Hunter
    // when the order comes, the case this replays.
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

// The 1.0.6 replay (Zhon against Aramon): eleven Harpy charms aimed at an
// Aramon builder all struck an Aramon Barracks (arakeep) standing in the line
// of fire. The charm is a straight Line of Sight status shot that takes the
// first body it strikes (52d8d0); a Barracks it cannot take stops it as
// surely as a wall, and the Harpy (hoverattack) kept firing from its hover
// point. Engine deviation (owner's call 2026-11-15): a status shot that
// strikes another body it cannot take counts as blocked
// (World::noteShotImpact). The Harpy hovers north of the builder with the
// Barracks between them; with an enemy Swordsman in the line instead, the
// charm takes the Swordsman, as before.
enum class Blocker { Barracks, Swordsman };
struct HarpyOutcome {
    int blockerStrikes = 0;  // charms that struck the body in the line
    int builderStrikes = 0;  // charms that struck the builder
    int maxRun = 0;          // most blocker strikes in a row from one spot
    int repositions = 0;
    int blockedMax = 0;      // most blocked shots counted at once
    bool builderTaken = false, blockerTaken = false;
    bool dropped = false;    // the Harpy let the builder go
};

HarpyOutcome runHarpy(const sim::TypeRegistry& reg, Blocker blocker, bool attackMove, int ticks,
                      const char* caster = "zonharp") {
    sim::World w;
    w.setPathfindingMode(mode);
    w.setTerrain(std::vector<uint8_t>(64 * 64, 40), 64, 64, 0);
    w.buildNavClasses(reg);
    w.setPathService(true);
    const sim::UnitType* harpy = reg.find(caster);
    const sim::UnitType* builder = reg.find("arabuild");
    const sim::UnitType* body = reg.find(blocker == Blocker::Barracks ? "arakeep" : "arasword");
    if (!harpy || !builder || !body) { check(false, "shipped casters, arabuild, arakeep and arasword load"); return {}; }
    const int target = w.spawn(builder, 512, 640, 0, 1);
    const int wall = w.spawn(body, 512, blocker == Blocker::Barracks ? 560 : 600, 0, 1);
    for (const int id : {target, wall}) {
        w.setStance(id, 2);
        w.unit(id)->moveState = 0;
        w.unit(id)->fireState = 0;
    }
    const int shooter = w.spawn(harpy, 512, 360, 0, 0);
    w.unit(shooter)->moveState = 1;
    w.unit(shooter)->fireState = 2;
    for (int i = 0; i < 30; ++i) w.tick(1.f / 30);
    if (attackMove) w.attackMove(shooter, 512, 900, false);
    else w.attack(shooter, target, false);
    HarpyOutcome out;
    int run = 0, lastRepositions = 0;
    for (int i = 0; i < ticks; ++i) {
        if (sim::Unit* a = w.unit(shooter)) a->mana = a->type->maxMana;   // a charm every reload
        w.tick(1.f / 30);
        const sim::Unit* a = w.unit(shooter);
        if (!a || !a->alive()) break;
        if (a->repositions != lastRepositions) { run = 0; lastRepositions = a->repositions; }
        out.repositions = std::max(out.repositions, int(a->repositions));
        if (a->blockedTarget == target) out.blockedMax = std::max(out.blockedMax, int(a->blockedShots));
        for (const auto& h : w.hits()) {
            if (h.fromPlayer != 0 || h.weapon != &harpy->weapons.front()) continue;
            if (h.victimId == wall) { ++out.blockerStrikes; out.maxRun = std::max(out.maxRun, ++run); }
            else if (h.victimId == target) { ++out.builderStrikes; run = 0; }
        }
        const sim::Unit* t = w.unit(target);
        if (t && t->player == 0) { out.builderTaken = true; break; }
        if (!out.dropped && i > 60) {
            const bool holds = std::any_of(a->orders.begin(), a->orders.end(),
                                           [&](const sim::Order& o) { return o.targetId == target; });
            out.dropped = !holds && a->scriptAimTarget != target;
        }
    }
    const sim::Unit* b = w.unit(wall);
    out.blockerTaken = b && b->player == 0;
    return out;
}

std::string describe(const HarpyOutcome& o) {
    char buf[200];
    std::snprintf(buf, sizeof buf, "blocker strikes=%d (most in a row %d) builder strikes=%d blocked=%d "
                  "repositions=%d builder taken=%d blocker taken=%d dropped=%d",
                  o.blockerStrikes, o.maxRun, o.builderStrikes, o.blockedMax, o.repositions,
                  int(o.builderTaken), int(o.blockerTaken), int(o.dropped));
    return buf;
}

// A long building behind a crest. Retail's collision reaches a unit through
// the cells it owns (tools/re/probe_long_footprint_blocked.py), so a shot
// that lands on a ridge in front of a building's long face is short of it,
// however long the building is the other way. The Taros Castle (tarcastl,
// 8x20 cells) stands with its long face to the archer, and a short passable
// ridge (columns 32..38, rows 30..33, a 90-high crest at x 568) lies 16..128
// px in front of that face, the crest 136 px from the castle's centre: inside
// a circle of its 160 px long half-extent, outside its footprint. Returns
// the same Outcome as run().
Outcome runCastle(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, int moveState, int ticks) {
    tnt::Map map;
    map.width = map.height = 64;
    map.seaLevel = 0;
    map.heights.assign(64 * 64, 30);
    map.features.assign(64 * 64, 0xffff);
    const int profile[] = {45, 60, 75, 90, 75, 60, 45};
    for (int z = 30; z <= 33; ++z)
        for (int i = 0; i < 7; ++i) map.heights[size_t(z) * 64 + size_t(32 + i)] = uint8_t(profile[i]);
    sim::World w;
    w.setPathfindingMode(mode);
    w.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    sim::registerMapFeatures(w, map, vfs, &reg);
    w.setPathService(true);
    const sim::UnitType* bow = reg.find("arabow");
    const sim::UnitType* castle = reg.find("tarcastl");
    if (!bow || !castle) { check(false, "shipped arabow and tarcastl load"); return {}; }
    if (castle->footX != 8 || castle->footZ != 20) check(false, "tarcastl is 8x20");
    // Footprint columns 40..47, rows 22..41: centre (704,512), long face x=640.
    const float sx = 23 * 16 + 8, sz = 32 * 16, tx = 704, tz = 512;
    const int archer = w.spawn(bow, sx, sz, 0, 0);
    const int target = w.spawn(castle, tx, tz, 0, 1);
    w.setStance(target, 2);
    w.unit(target)->fireState = 0;
    w.unit(target)->hp = sim::Fixed::fromInt(30000);
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
        out.repositions = std::max(out.repositions, int(a->repositions));
        for (const auto& h : w.hits()) {
            if (h.fromPlayer != 0) continue;
            if (h.victimId == target) { ++out.targetHits; continue; }
            if (h.x < 640) ++out.shortShots;   // short of the long face
        }
    }
    const sim::Unit* a = w.unit(archer);
    const sim::Unit* t = w.unit(target);
    out.damage = hp0 - (t && t->alive() ? t->hp.toFloat() : 0.f);
    if (a) out.moved = std::hypot(a->x.toFloat() - sx, a->z.toFloat() - sz);
    out.legionMoves = w.legionStats().moves;
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
                  o.stoneFlames, o.lodestoneHits, o.damage, o.blockedShots, o.moved);
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
// the open whatever it shot at before. An idle Hold Position archer
// auto-acquires a Swordsman behind the ridge and lands its arrows short until
// the target is written off from this spot. The Swordsman walks off out of
// range (the archer lets it go) and then back over the ridge into the open
// beside the archer, which must acquire and engage it unordered. Returns
// {short shots before, hits after} (-1 short shots if the target was not
// written off, or was hit, before it moved; -2 if the archer kept it).
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
    const sim::UnitType* bow = reg.find("arabow");
    const sim::UnitType* sword = reg.find("arasword");
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
    // Out of range to the east: the Hold Position archer drops its auto
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
    const int kTicks = 900;   // 30 s: about eleven Standard Arrows

    const Outcome open = run(vfs, reg, Obstacle::None, 1, kTicks);
    std::printf("  open field:  %s\n", describe(open).c_str());
    check(open.targetHits > 0 && open.repositions == 0 && open.moved < 8,
          "an open shot connects from where the archer stands, no reposition");

    for (const Obstacle obstacle : {Obstacle::Ridge, Obstacle::Campfires}) {
        const char* name = obstacle == Obstacle::Ridge ? "ridge" : "campfires";
        const Outcome held = run(vfs, reg, obstacle, 0, kTicks);
        const Outcome free = run(vfs, reg, obstacle, 1, kTicks);
        std::printf("  %-10s hold position: %s\n", name, describe(held).c_str());
        std::printf("  %-10s maneuver:      %s\n", name, describe(free).c_str());
        // An ordered attack walks to a clear shot on Hold Position (Defensive) too.
        check(held.shortShots >= 3 && held.repositions >= 1 && held.damage > 0,
              obstacle == Obstacle::Ridge ? "ridge: the shot is blocked, and an ordered Hold Position archer walks to a clear shot"
                                          : "campfires: the shot is blocked, and an ordered Hold Position archer walks to a clear shot");
        check(free.repositions >= 1 && free.damage > 0 && free.shortShots <= 4,
              obstacle == Obstacle::Ridge ? "ridge: the archer walks to a clear shot and hurts the target"
                                          : "campfires: the archer walks to a clear shot and hurts the target");
        if (mode == sim::PathfindingMode::Legion)
            check(free.legionMoves > 0 && held.legionMoves > 0, "the walks are Legion's",
                  std::to_string(free.legionMoves) + "/" + std::to_string(held.legionMoves));
    }
    // A long building: the crest in front of its long face blocks the shot.
    {
        const Outcome held = runCastle(vfs, reg, 0, kTicks);
        const Outcome free = runCastle(vfs, reg, 1, kTicks);
        std::printf("  castle     hold position: %s\n", describe(held).c_str());
        std::printf("  castle     maneuver:      %s\n", describe(free).c_str());
        check(held.shortShots >= 3 && held.repositions >= 1 && held.damage > 0,
              "castle: a crest in front of its long face blocks the shot, and an ordered Hold Position archer walks to a clear shot");
        check(free.repositions >= 1 && free.damage > 0 && free.shortShots <= 4,
              "castle: the archer counts those shots blocked, walks to a clear shot and hurts the castle");
        if (mode == sim::PathfindingMode::Legion)
            check(free.legionMoves > 0 && held.legionMoves > 0, "the walks are Legion's",
                  std::to_string(free.legionMoves) + "/" + std::to_string(held.legionMoves));
    }
    // Fire Wagon flames against a lodestone behind a standing stone.
    const WagonOutcome hold = runWagon(vfs, reg, Wagon::Hold, kTicks);
    const WagonOutcome onMove = runWagon(vfs, reg, Wagon::AttackMoveHold, 1800);
    const WagonOutcome maneuver = runWagon(vfs, reg, Wagon::AttackMoveManeuver, 1800);
    std::printf("  fire wagon hold, idle:        %s\n", describe(hold).c_str());
    std::printf("  fire wagon hold, attack-move: %s\n", describe(onMove).c_str());
    std::printf("  fire wagon maneuver, a-move:  %s\n", describe(maneuver).c_str());
    check(hold.stoneFlames >= 3 && hold.blockedShots >= 3,
          "flame impacts on the stone count as blocked shots against the lodestone");
    check(hold.damage == 0 && hold.moved < 8, "Hold Position with nothing queued stays and fires on");
    check(onMove.damage > 0 && onMove.stoneFlames <= 6,
          "Hold Position on an attack-move leaves the blocked spot and burns the lodestone");
    check(maneuver.damage > 0 && maneuver.stoneFlames <= 6,
          "Maneuver on an attack-move walks to a clear shot and burns the lodestone");
    // A Thirsha hovering behind a standing stone moves round its ring.
    const HoverOutcome hover = runHover(vfs, reg, 1800);
    std::printf("  thirsha hover, stone:        %s\n", describe(hover).c_str());
    // On its shipped Hold Position the idle Thirsha no longer takes off for
    // the Hunter before the order (VTOL_Standby's 4d7de0 refuses), so it
    // starts its hover from the ground: its first bolt still stops on the
    // stone, and from where it then hovers the rest clear it (no reposition
    // is needed; before, it came in already airborne on a non-retail auto
    // attack and needed one).
    check(hover.stoneBolts >= 1 && hover.stoneBolts <= 6,
          "the Thirsha's first bolts stop on the standing stone, then it stops firing into it");
    check(hover.hunterHits > 0 && hover.damage > 0,
          "the Thirsha reaches a clear bearing and hits the Hunter");
    // The Creon dragon on its Blue Flame: a damaging stream, not a status
    // shot, so stopped by the stone it keeps the Thirsha's ring hop instead
    // of the status caster's write-off.
    const HoverOutcome dragon = runHover(vfs, reg, 1800, "credrag", 410);
    std::printf("  dragon blue flame, stone:    %s dropped=%d\n", describe(dragon).c_str(), int(dragon.dropped));
    const sim::UnitType* credrag = reg.find("credrag");
    check(credrag && !credrag->weapons.empty() && !credrag->weapons.front().mindControl &&
              credrag->weapons.front().status == sim::Weapon::Status::None,
          "the Creon dragon's selected weapon at spawn is its Blue Flame");
    check(!dragon.dropped && dragon.repositions >= 1 && dragon.hunterHits > 0 && dragon.damage > 0,
          "a hovering Creon dragon whose Blue Flame stops on the stone hops round its ring and burns the Hunter");
    for (const bool attackMove : {false, true}) {
        const HarpyOutcome keep = runHarpy(reg, Blocker::Barracks, attackMove, 3600);
        const HarpyOutcome sword = runHarpy(reg, Blocker::Swordsman, attackMove, 1800);
        std::printf("  harpy %s, barracks:  %s\n", attackMove ? "attack-move" : "attack     ", describe(keep).c_str());
        std::printf("  harpy %s, swordsman: %s\n", attackMove ? "attack-move" : "attack     ", describe(sword).c_str());
        const HarpyOutcome mage = runHarpy(reg, Blocker::Barracks, attackMove, 3600, "tarmind");
        std::printf("  mind mage %s, barracks: %s\n", attackMove ? "attack-move" : "attack     ", describe(mage).c_str());
        check(keep.blockedMax >= 3 && keep.maxRun <= 3 && keep.blockerStrikes <= 6 && (keep.dropped || keep.builderTaken),
              attackMove ? "a Harpy on an attack-move stops charming the Barracks in the line within three charms and lets the builder go"
                         : "an ordered Harpy stops charming the Barracks in the line within three charms and lets the builder go");
        check(sword.blockedMax == 0 && sword.blockerTaken && sword.builderTaken,
              "a charm that strikes an enemy it can take takes it and counts no blocked shot");
        // A ground caster walks to another spot (repositionForShot: the
        // chord test sees terrain only) and gives the target up when its
        // walks are spent, three charms a spot.
        // Without the rule it charmed the Barracks 46 times in either case.
        // The attack-move runs straight down the line of fire, so the Mage
        // also fires from each new spot it walks to (18 strikes, 27 on
        // Legion's paths) before it lets the builder go.
        check(mage.dropped && mage.blockerStrikes <= (attackMove ? 32 : 12),
              "a ground Mind Mage behind the Barracks gives the builder up after its walks");
    }
    const auto [walks, hits] = runSticky(vfs, reg);
    check(walks >= 1 && hits > 0, "a failed search does not give up on the target once a clear spot is in reach",
          "walks=" + std::to_string(walks) + " hits=" + std::to_string(hits));
    const auto [blockedBefore, hitsAfter] = runWalkOut(vfs, reg);
    std::printf("  walk-out:   %d short before, %d hits after it walked into the open\n", blockedBefore, hitsAfter);
    check(blockedBefore >= 3 && hitsAfter > 0,
          "an idle Hold Position archer engages an enemy it found blocked once it walks into the open",
          "short=" + std::to_string(blockedBefore) + " hits=" + std::to_string(hitsAfter));
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
