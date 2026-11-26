// shot_range_test: every TA weapon measures a building centre to centre, as
// retail: none is aimed at a building past its range, and an attacker closes
// to the range itself.
//
//   shot_range_test <retail-install-dir> [--legion]
//
// Retail's unit-target range test is a virtual of the weapon class (vtable
// +0xc). Every TA class answers centre to centre with no allowance for the
// target's size (530580, inclusive): WeaponType, Line-of-Sight, Guided,
// Dropped (532290, a bare call of 530580) and Ballistic (52bc60: its arc,
// then 530580). tools/re/check_inrange_classes.py runs them natively.
// Retail's Attack_Chase (403bb0) approaches a ring of the weapon's range
// round the target's centre.
// The engine padded a structure target's range by its footprint (8 * larger
// side + 24 px). 0.9.12 dropped the pad for a shot that dies at its range; a
// Dropped bomb and an instant Line-of-Sight shot kept it, so each was cast at
// a building up to the pad past its range.
//
// Checks (flat 128x128 map, shipped TA units, an enemy Vehicle Plant):
//   - every shipped weapon, by retail class;
//   - a unit of each class 0.9.12 still padded (TA: Line-of-Sight lasers,
//     Dropped bombs) on Hold Position half the old pad past its range from
//     an enemy building never targets it and does it no damage; 20 px inside
//     its range it does;
//   - a mobile unit of each class ordered to attack an enemy building closes
//     to its plain range (centre to centre) and damages it (a flyer: damages
//     it);
//   - a Hammer (Ballistic) still reaches and hits one.
// The Kingdoms cases with no TA counterpart are gone with their classes
// (Remote Effect, Wandering, the status Line-of-Sight shots, melee).
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "tnt/tnt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>

using namespace ta;

namespace {
int failures = 0;
sim::PathfindingMode mode = sim::PathfindingMode::Retail;

void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}

struct Scene {
    tnt::Map map;
    sim::World w;
};

void build(Scene& s, const hpi::Vfs& vfs, const sim::TypeRegistry& reg) {
    s.map.width = s.map.height = 128;
    s.map.seaLevel = 0;
    s.map.heights.assign(size_t(128) * 128, 62);
    s.map.features.assign(size_t(128) * 128, 0xffff);
    s.w.setPathfindingMode(mode);
    s.w.setTerrain(s.map.heights, s.map.width, s.map.height, s.map.seaLevel, &s.map.features);
    sim::registerMapFeatures(s.w, s.map, vfs, &reg);
    s.w.setPathService(true);
}

// A building that never fights back and does not die. spawn() leaves nav to
// its caller, as a placement does.
int building(sim::World& w, const sim::UnitType* t, float x, float z) {
    const int id = w.spawn(t, x, z, {}, 1);
    w.blockFoot(*t, x, z, true);
    w.setStance(id, 2);
    w.unit(id)->fireState = 0;
    w.unit(id)->hp = sim::Fixed::fromInt(30000);
    return id;
}

bool holds(const sim::Unit* u, int target) {
    if (!u) return false;
    if (!u->orders.empty() && u->orders.front().targetId == target) return true;
    for (int s : u->slotTarget) if (s == target) return true;
    return false;
}

// Retail's weapon class (the handler vtable each shipped weapon gets).
std::string retailClass(const sim::Weapon& w) {
    using K = sim::Weapon::Kind;
    if (w.kind == K::Dropped) return "DroppedBallistic";
    if (w.ballisticClass()) return "Ballistic";
    if (w.kind == K::Guided) return "Guided";
    if (w.beam) return "LineOfSight";
    return "WeaponType";
}

// 0.9.12's rule, kept here to name what this change moves: only a shot that
// dies at its range went without the pad.
bool oldNoPad(const sim::Weapon& w) {
    return w.projVel > 0 && ((w.hasTaFlag(0) && !w.beam) || w.kind == sim::Weapon::Kind::Guided);
}

// Every shipped weapon by retail class.
void survey(const sim::TypeRegistry& reg, const char* roster) {
    struct Row { int weapons = 0, moved = 0, instant = 0; std::set<std::string> units; };
    std::map<std::string, Row> rows;

    for (const auto& [id, t] : reg.types()) {
        for (const auto& w : t.weapons) {
            Row& r = rows[retailClass(w)];
            ++r.weapons;
            r.units.insert(id);
            if (!w.ballisticClass() && !oldNoPad(w)) ++r.moved;
            if (w.projVel <= 0) ++r.instant;
        }
    }
    std::printf("  %s roster, shipped weapons by retail class (moved: padded before, centre to centre now):\n",
                roster);
    for (const auto& [cls, r] : rows) {
        std::string some;
        for (const auto& u : r.units) some += " " + u;
        std::printf("    %-24s %3d weapons on %3zu types, %3d moved, %3d instant:%s\n", cls.c_str(), r.weapons,
                    r.units.size(), r.moved, r.instant, some.c_str());
    }
    check(!rows.empty(), std::string(roster) + ": the shipped weapons are classed", "");
}

// The weapon an attack approaches on (World::approachSlot): the longest
// when they all fire.
const sim::Weapon* reachWeapon(const sim::UnitType& t, int slot) {
    if (t.weapons.empty()) return nullptr;
    int s = std::max(slot, 0);
    if (t.weapons.size() > 1)
        for (int i = 0; i < int(t.weapons.size()); ++i)
            if (t.weapons[size_t(i)].range > t.weapons[size_t(s)].range) s = i;
    return &t.weapons[size_t(s)];
}

struct Static {
    int held = 0;       // ticks the shooter held the target
    int shots = 0;      // ticks with a shot of its in the air aimed at the target, or a launch
    long damage = 0;    // damage the target took
    int struck = 0;     // ticks a shot struck the target
};


int strikes(const sim::World& w, int target) {
    return std::any_of(w.hits().begin(), w.hits().end(),
                       [&](const auto& h) { return h.victimId == target && h.sound; }) ? 1 : 0;
}

// A shooter (player 0, fire at will; a mobile one on Hold Position) and an
// enemy building at (dx, dz) from it.
Static staticShooter(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, const char* shooterId,
                     const char* targetId, float dx, float dz, int ticks, int slot = -1) {
    Scene s;
    build(s, vfs, reg);
    const sim::UnitType* st = reg.find(shooterId);
    const sim::UnitType* tt = reg.find(targetId);
    if (!st || !tt) { check(false, std::string("shipped ") + shooterId + " and " + targetId + " load"); return {}; }
    // With +shootall off, a human owner's sweep takes only shootme units
    // (docs/retail-ai.md, weapon target selection 0x40b7b0); a TA factory is
    // not one, so the target is a shootme copy: the cases measure range, not
    // the gate.
    static sim::UnitType shootable;
    shootable = *tt; shootable.shootMe = true; tt = &shootable;
    const float sx = 1000, sz = 600;
    const int shooter = s.w.spawn(st, sx, sz, {}, 0);
    if (st->isStructure()) s.w.blockFoot(*st, sx, sz, true);
    else s.w.unit(shooter)->moveState = 0;
    (void)slot;   // TA has no weapon switching: every slot fires
    
    const int target = building(s.w, tt, sx + dx, sz + dz);
    // A spotter of the shooter's beside the building: a shooter whose range
    // outreaches its sight (a Slasher: 230 sight, missiles 600) takes only
    // what its player sees (the sweep and Standby's scan draw on list +5), and
    // the cases measure range, not sight.
    if (const sim::UnitType* sp = reg.find("armpw")) {
        static sim::UnitType spotter;
        spotter = *sp; spotter.id = "spotter"; spotter.weapons.clear(); spotter.weapon = {};
        s.w.spawn(&spotter, sx + dx + 8.f * float(tt->footX) + 64.f, sz + dz, {}, 0);
    }
    Static out;
    for (int i = 0; i < ticks; ++i) {
        
        s.w.player(0).books.stored[sim::econ::E] = 30000;   // a laser tower's energypershot
        s.w.tick(1.f / 30);
        const sim::Unit* u = s.w.unit(shooter);
        if (holds(u, target)) ++out.held;
        bool shot = u && u->justFired;
        for (const auto& p : s.w.projectiles())
            if (p.fromId == shooter && p.targetId == target) { shot = true; break; }
        if (shot) ++out.shots;
        out.struck += strikes(s.w, target);
        if (sim::Unit* t = s.w.unit(target)) {
            out.damage += 30000 - t->hp.floorInt();
            t->hp = sim::Fixed::fromInt(30000);
        }
    }
    return out;
}

struct Approach {
    float dist = 0;     // centre distance where it stood at the end
    float drift = 0;    // how far it moved over the last 10 s
    long damage = 0;    // damage the target took
    int shots = 0;      // ticks it launched a shot or spell
    int range = 0;      // the weapon it approached on
    int struck = 0;     // ticks a shot struck the target
    bool held = false;  // still on the attack at the end
    bool ok = false;
};

// A mobile shooter 1100 px from an enemy building, ordered to attack it.
Approach attackBuilding(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, const std::string& shooterId,
                        const char* targetId, int ticks, int slot = -1) {
    Scene s;
    build(s, vfs, reg);
    const sim::UnitType* st = reg.find(shooterId);
    const sim::UnitType* tt = reg.find(targetId);
    if (!st || !tt) { check(false, "shipped " + shooterId + " and " + targetId + " load"); return {}; }
    const float tx = 1600, tz = 1024;
    const int shooter = s.w.spawn(st, tx - 1100, tz, {}, 0);
    (void)slot;   // TA has no weapon switching: every slot fires
    
    const int target = building(s.w, tt, tx, tz);
    for (int i = 0; i < 30; ++i) s.w.tick(1.f / 30);
    s.w.attack(shooter, target, false);
    Approach out;
    float lateX = 0, lateZ = 0;
    for (int i = 0; i < ticks; ++i) {
        
        s.w.tick(1.f / 30);
        if (sim::Unit* t = s.w.unit(target)) {
            out.damage += 30000 - t->hp.floorInt();
            t->hp = sim::Fixed::fromInt(30000);
        }
        if (const sim::Unit* u = s.w.unit(shooter); u && u->justFired) ++out.shots;
        out.struck += strikes(s.w, target);
        if (const sim::Unit* u = s.w.unit(shooter); u && i == ticks - 300) {
            lateX = u->x.toFloat();
            lateZ = u->z.toFloat();
        }
    }
    const sim::Unit* u = s.w.unit(shooter);
    const sim::Unit* t = s.w.unit(target);
    if (!u || !t) return out;
    out.dist = std::hypot((t->x - u->x).toFloat(), (t->z - u->z).toFloat());
    out.drift = std::hypot(u->x.toFloat() - lateX, u->z.toFloat() - lateZ);
    const sim::Weapon* w = reachWeapon(*u->type, slot);
    out.range = w ? w->range : 0;
    out.held = holds(u, target);
    out.ok = true;
    return out;
}

std::string describe(const Approach& a) {
    char b[200];
    std::snprintf(b, sizeof b,
                  "stood %.1f px off (range %d), %d shots, %d strikes, %ld damage, moved %.0f px in the last 10 s%s",
                  a.dist, a.range, a.shots, a.struck, a.damage, a.drift, a.held ? "" : ", order gone");
    return b;
}

std::string describe(const Static& s) {
    char b[140];
    std::snprintf(b, sizeof b, "held %d ticks, %d shot ticks, %d strikes, %ld damage", s.held, s.shots, s.struck,
                  s.damage);
    return b;
}

// One shooter per retail class whose weapon was padded before (0.9.12 kept
// the pad for it). `slot` selects the weapon of a switcher; -1 keeps the
// first (the longest decides for a unit whose weapons all fire).
struct ClassCase { const char* unit; int slot; const char* what; };
const ClassCase kClassCases[] = {
    {"armthund", -1, "a Dropped bomber (Arm Thunder, Bombs)"},
    {"corak", -1, "a Line-of-Sight laser kbot (Core A.K.)"},
    {"armllt", -1, "a Line-of-Sight laser tower (Arm L.L.T.)"},
};
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: shot_range_test <retail-install-dir> [--legion]\n"); return 2; }
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--legion") mode = sim::PathfindingMode::Legion;
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry standard;
    sim::setupRegistry(standard, vfs);
    std::printf("[%s pathfinding]\n", mode == sim::PathfindingMode::Legion ? "Legion" : "Retail");

    survey(standard, "standard");

    // The target: an Arm Vehicle Plant (8x6: the old pad was 88 px, its half
    // widths 64 and 48).
    const char* target = "armvp";
    const sim::UnitType* keep = standard.find(target);
    if (!keep) { check(false, std::string("shipped ") + target + " loads"); return 1; }
    const int oldPad = 8 * std::max(keep->footX, keep->footZ) + 24;

    // Each class on Hold Position (fire at will): half the old pad past its
    // range it never targets the building; 20 px inside the range it does.
    for (const auto& c : kClassCases) {
        const sim::UnitType* t = standard.find(c.unit);
        if (t && t->canFly) continue;   // a flyer does not hold a post
        const sim::Weapon* w = t ? reachWeapon(*t, c.slot) : nullptr;
        if (!w || w->ballisticClass() || oldNoPad(*w)) {
            check(false, std::string(c.unit) + ": a weapon 0.9.12 still padded", "fixture");
            continue;
        }
        const Static band = staticShooter(vfs, standard, c.unit, target, 0, float(w->range) + oldPad * 0.5f, 900,
                                          c.slot);
        std::printf("  %s (%s, %s, range %d) on hold %d px off: %s\n", c.unit, w->name.c_str(),
                    retailClass(*w).c_str(), w->range, w->range + oldPad / 2, describe(band).c_str());
        check(band.held == 0 && band.damage == 0 && band.struck == 0,
              std::string(c.what) + " on hold never targets a building past its range", describe(band));
        const Static in = staticShooter(vfs, standard, c.unit, target, 0, float(w->range) - 20, 900, c.slot);
        std::printf("  %s on hold %d px off: %s\n", c.unit, w->range - 20, describe(in).c_str());
        check(in.held > 0 && in.damage > 0, std::string(c.what) + " on hold hits one inside its range",
              describe(in));
    }

    // Mobile units of each class attack an enemy building. Each must stand
    // within its plain range and damage it; before, a shot that dies at its
    // range parked up to the pad beyond and missed, and the other classes
    // parked there and hit from past their range.
    {
        struct Case { const char* unit; int slot; const char* what; };
        std::vector<Case> cases = {
            {"armpw", -1, "a plain WeaponType shooter (Arm Peewee, E.M.G.)"},
            {"cormist", -1, "a Guided missile launcher (Core Slasher)"},
            {"armbrawl", -1, "a hovering gunship (Arm Brawler)"},
        };
        for (const auto& c : kClassCases)
            if (const sim::UnitType* t = standard.find(c.unit); !t || !t->isStructure())
                cases.push_back({c.unit, c.slot, c.what});
        for (const auto& c : cases) {
            const sim::UnitType* t = standard.find(c.unit);
            if (!t) { check(false, std::string("shipped ") + c.unit + " loads"); continue; }
            const sim::Weapon* w = reachWeapon(*t, c.slot);
            if (!w || w->ballisticClass()) {
                check(false, std::string(c.unit) + "'s reach weapon is ranged and not Ballistic", "fixture");
                continue;
            }
            const Approach a = attackBuilding(vfs, standard, c.unit, target, 1800, c.slot);
            std::printf("  %s (%s) vs %s: %s\n", c.unit, retailClass(*w).c_str(), target, describe(a).c_str());
            // A flyer passes over on its runs; only a ground unit stands.
            check(a.ok && (t->canFly || a.dist <= float(a.range) + 0.5f) && a.damage > 0,
                  std::string(c.what) + " ordered to attack a building " +
                      (t->canFly ? "damages it" : "closes to its plain range and damages it"),
                  describe(a));
        }
    }

    // A Ballistic Hammer keeps its reach.
    {
        const Approach cannon = attackBuilding(vfs, standard, "armham", target, 1800);
        std::printf("  armham vs %s: %s\n", target, describe(cannon).c_str());
        check(cannon.ok && cannon.damage > 0, "a Ballistic Hammer still reaches and hits a building", describe(cannon));
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
