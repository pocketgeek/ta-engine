// shot_range_test: every weapon but melee measures a building centre to
// centre, as retail: none is aimed at a building past its range, and an
// attacker closes to the range itself.
//
//   shot_range_test <retail-install-dir> [--legion]
//
// Retail's unit-target range test is a virtual of the weapon class (vtable
// +0xc). 16 of its 17 classes answer centre to centre with no allowance for
// the target's size (530580, inclusive): WeaponType, every Line-of-Sight
// subclass, Guided, every Remote Effect subclass, Wandering, Dropped (532290,
// a bare call of 530580) and Ballistic (52bc60: its arc, then 530580). Only
// MeleeWeapon (52b980) tests footprint boxes. tools/re/check_inrange_classes.py
// runs all 17 natively. Retail's Attack_Chase (403bb0) approaches a ring of
// the weapon's range round the target's centre.
// The engine padded a structure target's range by its footprint (8 * larger
// side + 24 px). 0.9.12 dropped the pad for a shot that dies at its range (a
// Crusades Death Totem took a lodestone 584 px off and its bolts died ~540
// px out); a Dropped bomb, a Remote or Wandering spell and an instant or
// status Line-of-Sight shot kept it, so each was cast at a building up to
// the pad past its range (Weapon::centreReach now: every class but melee).
//
// Checks (flat 128x128 map, shipped units):
//   - every shipped weapon of either roster, by retail class: no pad but for
//     melee;
//   - Crusades: a zonglyph 584.5 px from an enemy tarlode never targets it
//     and fires nothing; at ~538 px it targets it and its bolts hit;
//   - a unit of each class on Hold Position half the old pad past its range
//     from an enemy building never targets it and does it no damage; 20 px
//     inside its range it does;
//   - a mobile unit of each class ordered to attack an enemy building closes
//     to its plain range (centre to centre) and damages it (a flyer: damages
//     it; a Wandering storm, which drifts: casts);
//   - a Lich (Death Aura, range 60) ordered onto a Barracks damages it only
//     from within 60 px of its centre and keeps the order (Retail
//     pathfinding halts it short: no damage);
//   - an Archer (Ballistic) and a Swordsman (melee) still reach and hit one.
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

using namespace tak;

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
    if (w.melee) return "Melee";
    if (w.kind == K::Dropped) return "DroppedBallistic";
    if (w.ballisticClass()) return "Ballistic";
    if (w.kind == K::Guided) return "Guided";
    if (w.kind == K::Wandering) return "Wandering";
    if (w.kind == K::Remote) {
        switch (w.remote) {
        case sim::Weapon::RemoteKind::Earthquake: return "Earthquake";
        case sim::Weapon::RemoteKind::Hailstorm: return "Hailstorm";
        case sim::Weapon::RemoteKind::MindCtl: return "AreaMindControl";
        case sim::Weapon::RemoteKind::Freeze: return "AreaTurnToFrozen";
        default: return "RemoteEffect";
        }
    }
    if (w.beam) {
        if (w.flameKind >= 0) return "LineOfSightFire";
        if (w.lightning) return "LineOfSightLightning";
        if (w.mindControl) return "LineOfSightMindControl";
        if (w.status == sim::Weapon::Status::Stoned) return "LineOfSightTurnToStone";
        if (w.status == sim::Weapon::Status::Frozen) return "LineOfSightTurnToFrozen";
        return "LineOfSight";
    }
    return "WeaponType";
}

// 0.9.12's rule, kept here to name what this change moves: only a shot that
// dies at its range went without the pad.
bool oldNoPad(const sim::Weapon& w) {
    return !w.melee && w.projVel > 0 && (w.straight || w.lightning || w.flameKind >= 0 ||
                                         w.kind == sim::Weapon::Kind::Guided);
}

// Every shipped weapon by retail class: no structure pad but for melee.
void survey(const sim::TypeRegistry& reg, const char* roster) {
    struct Row { int weapons = 0, moved = 0, instant = 0; std::set<std::string> units; };
    std::map<std::string, Row> rows;
    int bad = 0;
    std::string badList;
    for (const auto& [id, t] : reg.types()) {
        for (const auto& w : t.weapons) {
            Row& r = rows[retailClass(w)];
            ++r.weapons;
            r.units.insert(id);
            if (!w.melee && !w.ballisticClass() && !oldNoPad(w)) ++r.moved;
            if (!w.melee && w.projVel <= 0) ++r.instant;
            const int32_t want = w.melee ? 8 * 4 + 24 : 0;
            if (w.centreReach() == w.melee || w.structurePad(4) != want) {
                ++bad;
                badList += " " + id + "/" + w.name;
            }
        }
    }
    std::printf("  %s roster, shipped weapons by retail class (moved: padded before, centre to centre now):\n",
                roster);
    for (const auto& [cls, r] : rows)
        std::printf("    %-24s %3d weapons on %3zu types, %3d moved, %3d instant\n", cls.c_str(), r.weapons,
                    r.units.size(), r.moved, r.instant);
    check(bad == 0, std::string(roster) + ": every class but melee measures a building centre to centre", badList);
}

// The weapon an attack approaches on (World::approachSlot): the selected one
// for a switcher, the longest when they all fire.
const sim::Weapon* reachWeapon(const sim::UnitType& t, int slot) {
    if (t.weapons.empty()) return nullptr;
    int s = std::max(slot, 0);
    if (!t.weaponSwitching && t.weapons.size() > 1)
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

// A stone or freeze shot never damages a building: retail's callbacks
// (52dca0 / 52df00) refuse a body without a mover and replace the damage.
// Its reach shows in the shots that strike the building instead.
bool statusShot(const sim::Weapon& w) { return w.losStatus != sim::Weapon::LosStatus::None; }

int strikes(const sim::World& w, int target) {
    return std::any_of(w.hits().begin(), w.hits().end(),
                       [&](const auto& h) { return h.victimId == target && h.sound; }) ? 1 : 0;
}

// A shooter (player 0, fire at will; a mobile one on Hold Position) and an
// enemy building at (dx, dz) from it. `slot` >= 0 selects a weapon.
Static staticShooter(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, const char* shooterId,
                     const char* targetId, float dx, float dz, int ticks, int slot = -1) {
    Scene s;
    build(s, vfs, reg);
    const sim::UnitType* st = reg.find(shooterId);
    const sim::UnitType* tt = reg.find(targetId);
    if (!st || !tt) { check(false, std::string("shipped ") + shooterId + " and " + targetId + " load"); return {}; }
    const float sx = 1000, sz = 600;
    const int shooter = s.w.spawn(st, sx, sz, {}, 0);
    if (st->isStructure()) s.w.blockFoot(*st, sx, sz, true);
    else s.w.unit(shooter)->moveState = 0;
    if (slot >= 0) s.w.setWeapon(shooter, slot);
    s.w.player(0).mana = 30000;
    s.w.unit(shooter)->mana = st->maxMana;   // a caster's pool starts empty (511990): give it a full one
    const int target = building(s.w, tt, sx + dx, sz + dz);
    // A spotter of the shooter's beside the building: a shooter whose range
    // outreaches its sight (a Fire Mage: 200 sight, Fire Storm 447) takes only
    // what its player sees (the sweep and Standby's scan draw on list +5), and
    // the cases measure range, not sight.
    if (const sim::UnitType* sp = reg.find("araarch")) {
        static sim::UnitType spotter;
        spotter = *sp; spotter.id = "spotter"; spotter.weapons.clear(); spotter.weapon = {};
        s.w.spawn(&spotter, sx + dx + 8.f * float(tt->footX) + 64.f, sz + dz, {}, 0);
    }
    Static out;
    for (int i = 0; i < ticks; ++i) {
        s.w.player(0).mana = 30000;
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
    if (slot >= 0) s.w.setWeapon(shooter, slot);
    s.w.player(0).mana = 30000;
    s.w.unit(shooter)->mana = st->maxMana;   // a caster's pool starts empty (511990): give it a full one
    const int target = building(s.w, tt, tx, tz);
    for (int i = 0; i < 30; ++i) s.w.tick(1.f / 30);
    s.w.attack(shooter, target, false);
    Approach out;
    float lateX = 0, lateZ = 0;
    for (int i = 0; i < ticks; ++i) {
        s.w.player(0).mana = 30000;
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
    {"tarbeak", -1, "a Dropped bomber (Taros Iron Beak, Egg Bomb)"},
    {"tarnecro", 2, "a Remote Effect caster (Taros Lokken, Fire Wave)"},
    {"tarmage", 2, "a Hailstorm caster (Taros Fire Mage, Fire Storm)"},
    {"arapries", 0, "an Earthquake caster (Aramon Acolyte, Earthquake)"},
    {"tarwitch", 0, "a Wandering caster (Taros Weather Witch, Tornado)"},
    {"cremech", 1, "a freezing Line-of-Sight shot (Creon Mechanic, Freeze Beam)"},
    {"zonbasil", -1, "a petrifying Line-of-Sight shot (Zhon Basilisk, Turn To Stone)"},
};
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: shot_range_test <retail-install-dir> [--legion]\n"); return 2; }
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--legion") mode = sim::PathfindingMode::Legion;
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry standard, crusades;
    sim::setupRegistry(standard, vfs, false);
    sim::setupRegistry(crusades, vfs, true);
    std::printf("[%s pathfinding]\n", mode == sim::PathfindingMode::Legion ? "Legion" : "Retail");

    survey(standard, "standard");
    survey(crusades, "Crusades");

    // The 0.9.12 replay's geometry: totem at (2600, 2952), lodestone at
    // (2304, 3456), 584.49 px apart. Retail's 530580 at range 550: out.
    {
        const Static far = staticShooter(vfs, crusades, "zonglyph", "tarlode", -296, 504, 900);
        std::printf("  Death Totem, lodestone 584.5 px off: %s\n", describe(far).c_str());
        check(far.held == 0 && far.shots == 0,
              "a Crusades Death Totem (range 550) never targets a lodestone 584.5 px off");
        const Static near = staticShooter(vfs, crusades, "zonglyph", "tarlode", -272, 464, 900);
        std::printf("  Death Totem, lodestone 537.8 px off: %s\n", describe(near).c_str());
        check(near.held > 0 && near.damage > 0, "at 537.8 px it targets the lodestone and its bolts hit");
    }

    // The target: an Aramon Barracks (7x12: the old pad was 120 px, its half
    // width 56).
    const char* target = "arakeep";
    const sim::UnitType* keep = standard.find(target);
    if (!keep) { check(false, std::string("shipped ") + target + " loads"); return 1; }
    const int oldPad = 8 * std::max(keep->footX, keep->footZ) + 24;

    // Each class on Hold Position (fire at will): half the old pad past its
    // range it never targets the building; 20 px inside the range it does.
    for (const auto& c : kClassCases) {
        const sim::UnitType* t = standard.find(c.unit);
        if (t && t->canFly) continue;   // a flyer does not hold a post
        const sim::Weapon* w = t ? reachWeapon(*t, c.slot) : nullptr;
        if (!w || w->melee || w->ballisticClass() || oldNoPad(*w)) {
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
        // A stone or freeze class never takes the Keep on its own: it is
        // cantbestoned and cantbefrozen, and the picker (412620, the sweep's
        // and the scan's) skips a type immune to the slot's status. Its reach
        // shows in the explicit attacks below.
        if (statusShot(*w)) {
            check(in.held == 0 && in.struck == 0,
                  std::string(c.what) + " on hold never takes a building immune to its status", describe(in));
            continue;
        }
        check(in.held > 0 && (statusShot(*w) ? in.struck > 0 && in.damage == 0 : in.damage > 0),
              std::string(c.what) + (statusShot(*w) ? " on hold strikes one inside its range, harmlessly"
                                                    : " on hold hits one inside its range"),
              describe(in));
    }

    // Mobile units of each class attack an enemy building. Each must stand
    // within its plain range and damage it; before, a shot that dies at its
    // range parked up to the pad beyond and missed, and the other classes
    // parked there and hit from past their range.
    {
        struct Case { const char* unit; int slot; const char* what; };
        std::vector<Case> cases = {
            {"verarch", -1, "a line-of-sight shooter (Veruna Crossbowman)"},
            {"zonsham", -1, "a lightning caster (Zhon Shaman)"},
            {"tarfire", -1, "a Guided caster (Taros Fire Demon)"},
            {"crefire", -1, "a flame thrower (Creon Fire Wagon)"},
            {"zondrake", -1, "a flying flame breather (Zhon Drake)"},
        };
        for (const auto& c : kClassCases) cases.push_back({c.unit, c.slot, c.what});
        for (const auto& c : cases) {
            const sim::UnitType* t = standard.find(c.unit);
            if (!t) { check(false, std::string("shipped ") + c.unit + " loads"); continue; }
            const sim::Weapon* w = reachWeapon(*t, c.slot);
            if (!w || w->melee || w->ballisticClass()) {
                check(false, std::string(c.unit) + "'s reach weapon is ranged and not Ballistic", "fixture");
                continue;
            }
            const Approach a = attackBuilding(vfs, standard, c.unit, target, 1800, c.slot);
            std::printf("  %s (%s) vs %s: %s\n", c.unit, retailClass(*w).c_str(), target, describe(a).c_str());
            // A flyer passes over on its runs; only a ground unit stands. A
            // Wandering storm is born in front of its caster and drifts at the
            // target, weaving: whether one crosses the building is chance, so
            // the caster need only cast.
            const bool storm = w->kind == sim::Weapon::Kind::Wandering;
            const bool status = statusShot(*w);
            // A stone or freeze class refuses the order: its callback refuses
            // a building, so retail spent shots on it for good (engine
            // deviation, World::statusCanTake).
            if (status) {
                check(!a.held && a.shots == 0 && a.damage == 0,
                      std::string(c.what) + " ordered to attack a building refuses the order", describe(a));
                continue;
            }
            check(a.ok && (t->canFly || a.dist <= float(a.range) + 0.5f) &&
                      (storm ? a.shots > 0 : a.damage > 0),
                  std::string(c.what) + " ordered to attack a building " +
                      (t->canFly ? "damages it" : storm ? "closes to its plain range and casts"
                                            : "closes to its plain range and damages it"),
                  describe(a));
        }
    }

    // A footprint the range barely reaches past: a Lich (Death Aura, range
    // 60) ordered onto an Aramon Barracks, whose nearest face is 56 px from
    // its centre. Retail's Attack_Chase heads for a 60 px ring round the
    // centre and casts only once its range test passes, so the Lich hits the
    // building exactly when it stands within 60 px of the centre. Retail
    // pathfinding halts it short of that (no damage, the order stays);
    // Legion's lets its body press closer to the footprint.
    {
        const sim::UnitType* lich = standard.find("tarlich");
        const sim::Weapon* w = lich ? reachWeapon(*lich, -1) : nullptr;
        if (!w || w->melee || w->range >= 8 * std::min(keep->footX, keep->footZ) + 8 * lich->footX) {
            check(false, "tarlich's reach is shorter than a Barracks' nearest face plus its own half", "fixture");
        } else {
            const Approach a = attackBuilding(vfs, standard, "tarlich", target, 1800);
            std::printf("  tarlich (%s, range %d) vs %s: %s\n", w->name.c_str(), w->range, target,
                        describe(a).c_str());
            const bool within = a.dist <= float(a.range) + 0.5f;
            check(a.ok && a.held && (a.damage > 0) == within && (mode == sim::PathfindingMode::Legion || !within),
                  "a Lich ordered onto a building damages it only from within its range", describe(a));
        }
    }

    // A Ballistic archer and a melee swordsman keep their reach.
    {
        const Approach bow = attackBuilding(vfs, standard, "araarch", target, 1800);
        std::printf("  araarch vs %s: %s\n", target, describe(bow).c_str());
        check(bow.ok && bow.damage > 0, "a Ballistic archer still reaches and hits a building", describe(bow));
        const Approach sword = attackBuilding(vfs, standard, "arasword", target, 1800);
        std::printf("  arasword vs %s: %s\n", target, describe(sword).c_str());
        check(sword.ok && sword.damage > 0, "a melee swordsman still reaches and hits a building", describe(sword));
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
