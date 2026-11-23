#pragma once

// The Classic level's squad layer (docs/classic-ai-plan.md §1.1, §1.4): retail's
// AI manager (constructor 0x40fed0) with its 100 squad slots, the quotas and wave
// sizes each base squad rolls as it is built (0x40c560, 0x40c240/0x40c3c0/
// 0x40c490), the dying-unit cleanup (0x40fe80) and the assignment pass (0x40eeb0)
// that the scheduler runs every rand(7)+3 ticks.
//
// Pure functions over the player's own units in unit-slot order, so the same code
// is what ClassicBrain runs and what the emulation oracles check against the
// retail routines: tools/re/check_ai_squad_ctor.py (constructor, base rolls) and
// tools/re/check_ai_assignment.py (cleanup + assignment, with the real set-squad
// 0x50b4f0, nearest base 0x40af70, base centre 0x40c610 and radius 0x40cf30
// running under them). The scheduler and the squad clocks are the shared sim
// templates (retailTickAiSchedule / retailTickAiSquad, checked by
// check_ai_scheduler / check_ai_squads); the nearest base is retailNearestAiBase
// (check_ai_base_selection), the centroid RetailAiCentroid (check_ai_centroid),
// the radius retailAiBaseRadius (check_ai_base_radius).
//
// Nothing here is lockstep state: the brain that owns it runs on the server only.

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sim/retailai.h"

namespace tak::ai {

// Retail's squad slots (0x40fed0). Slot 0 and 99 hold nothing; dispatch visits
// 0..98. Odd strike slots own the even backup slot after them.
namespace classic_slot {
constexpr int kHome = 1;                     // base 1: the Monarch's, anchored on the start
constexpr int kLastBase = 20;                // bases 1..20
constexpr int kFirstStrike = 21;             // ground strikes 21,23..49 (backups 22..50)
constexpr int kFirstNavalStrike = 51;        // naval strikes 51,53..79 (backups 52..80)
constexpr int kFirstVtol = 81, kLastVtol = 95;
constexpr int kHomeless = 96;                // the plain AISquad: think only reschedules
constexpr int kMassStrike = 97, kMassBackup = 98;  // the campaign mass attack
}

// Strike cohesion (strike p1, squared pixels per member) by strike family.
constexpr int32_t kClassicGroundCohesion = 200000;
constexpr int32_t kClassicNavalCohesion = 500000;
constexpr int32_t kClassicMassCohesion = 1000000;

// One of the player's units as the manager's routines read it. `flags` holds the
// unit +0x130 bits they test; the rest are named fields of the unit and its type.
struct ClassicSquadUnit {
    static constexpr uint32_t kLive = 0x1000000u;       // the slot holds a unit
    static constexpr uint32_t kDying = 0x1000u;         // ... that is dying
    static constexpr uint32_t kSelectable = 0x20u;      // set at creation (0x511990) and by MakeSelectable
    static constexpr uint32_t kStructure = 0x2000000u;  // AI category 1
    static constexpr uint32_t kArmed = 0x8000000u;
    int id = 0;
    uint32_t flags = 0;
    bool commander = false;     // type +0x264 0x40000
    bool sacredSite = false;    // type +0x264 bit 31 (a yardmap 'S' cell)
    bool builder = false;       // type +0x260 0x100 (FBI builder=)
    bool finished = true;       // +0x108 (remaining build fraction) == 0
    bool held = false;          // +0x104 != 0 (U6: never set by the port)
    bool carried = false;       // +0xa8: carried by another unit ...
    bool carrierHolds = false;  // ... whose +0x130 has 0x4000000
    bool mover = false;         // +8: has a navigator
    int32_t menu = 0;           // type +0x12e: build menu entries
    int32_t x = 0, z = 0;       // +0x68 / +0x70, 16.16 pixels
    int16_t footX = 0, footZ = 0;   // +0x78 / +0x7a
    int32_t squad = 0;          // +0xc8: 0 unassigned, -1 dropped while dying, else a slot
    // Read by the base think's member loop (0x40b320):
    uint16_t type = 0;          // the unit's catalogue type (unit +0xb4)
    bool producing = false;     // a BuildingBuild mission is in its list (the factory planner's "busy")
    uint8_t missionClass = 0;   // its first mission's class byte (descriptor +0x10); 0 = no mission
    // Read by the categories (0x40a5f0) and the constructor planner (0x40c890):
    uint32_t typeFlags = 0;     // type +0x260 (0x100 builder, 0x800 canfly, 0x10000 armed, 0x80000 floater)
    uint32_t secondaryFlags = 0;   // type +0x264 (0x40000 commander, bit 31 mana site)
    int16_t minWaterDepth = -10000;   // type +0x194
    bool mission = false;       // +0x60: it has a first mission ...
    uint32_t missionFlags = 0;  // ... whose flags (+0x5a) are these (8 a build, 0x200 a target
                                //     unit, 0x400 a goal point, 0x2000 the default mission) ...
    int missionTarget = 0;      // ... its target unit (+0x16) ...
    int32_t goalX = 0, goalZ = 0;   // ... and goal point (+0x22 / +0x2a, 16.16)
    bool buildingBuild = false; // ... which is BuildingBuild (a factory at work)
    bool buildsManaSite = false;   // ... which builds a mana-site type (0x40c830)
    uint16_t weaponDamage = 0, weaponReload = 0;   // its current weapon (+0xc): +0x88, +0x9c ticks
    // Read by the army squads (ai/classicwaves.h):
    int16_t hp = 0;             // +0x10c
    uint32_t maxDamage = 0;     // type +0x1be
    int16_t sight = 0;          // type +0x226
    int32_t range = 0;          // its current weapon's +0x90
    bool navigator = false;     // +8 -> +4: a ground navigator (the reachability walks need one)
    bool naval = false;         // type +0x260 0x80000 with +0x194 > 0 (the picker's reachability flag)
    int16_t cellX = 0, cellZ = 0;   // +0x74 / +0x76: its footprint's top-left cell
    uint8_t missionKind = 0;    // its first mission's kind byte (+4), in 0x4de530's numbering
    bool live() const { return (flags & kLive) && !(flags & kDying); }
    sim::RetailAiUnitClass unitClass() const {
        sim::RetailAiUnitClass c;
        c.flags = flags; c.typeFlags = typeFlags; c.secondaryFlags = secondaryFlags;
        c.constructor = menu != 0; c.mover = mover; c.mission = mission; c.missionFlags = uint8_t(missionFlags);
        c.capacity = minWaterDepth;
        return c;
    }
};

using ClassicCell = std::pair<int16_t, int16_t>;

struct ClassicSquads {
    sim::RetailAiSchedule schedule;            // countdown (mgr+5) and the squad clocks (+0xc)
    std::array<sim::RetailAiSquad, 100> squads;   // kind, p0..p8, active/dirty, members (unit ids)
    bool firstPassDone = false;                // mgr+0x1a6
    uint32_t massAttack = 0;                   // the campaign's massattack, x30 ticks; 0 = none
    // The anchor tables a base's p0 indexes: p0 = k+1 is start record k (0x62dbdc,
    // one per start position), p0 = -(k+1) sacred-site record k (0x62dbcc). A
    // record is its 16-pixel cell, or none for an empty start.
    std::vector<std::optional<ClassicCell>> starts, sites;
};

// --- the constructor and its rolls --------------------------------------------

// 0x40c240: the ground wave size p5 from the reserve p2 and E = max(10, the
// largest unit count among the active players not allied to this one).
template<class Random>
int32_t classicGroundWave(int32_t reserve, int32_t enemyPopulation, Random random) {
    const int32_t e = enemyPopulation > 10 ? enemyPopulation : 10;
    int32_t p = int32_t(random(reserve < 2 ? 2 : reserve)) + 1;
    if (random(4) != 0) {
        p += int32_t(random(e / 10 < 2 ? 2 : e / 10));
        if (random(3) != 0) p += int32_t(random(e / 5 < 2 ? 2 : e / 5));
    }
    if (random(60) == 0) p = int32_t(uint32_t(int32_t(random(10)) + 5) * uint32_t(p));
    else if (random(6) == 0) p = int32_t(uint32_t(p) * 5u);
    else if (random(2) == 0) p = int32_t(uint32_t(int32_t(random(2)) + 2) * uint32_t(p));
    if (p > 50) p = int32_t(random(10)) + 41;
    if (random(2) == 0) p = int32_t(0u - uint32_t(p));
    return p;
}

// 0x40c3c0 (air: p6 from p3, multipliers 1/100, 1/10, 1/2) and 0x40c490 (naval:
// p7 from p4, 1/100, 1/20, 1/3). Unlike the ground roll there is one added term,
// and it is drawn from the reserve again, not from the enemy's size.
template<class Random>
int32_t classicReserveWave(int32_t reserve, int32_t fiveTimesOdds, int32_t multiplyOdds, Random random) {
    const int32_t r = reserve < 2 ? 2 : reserve;
    int32_t p = int32_t(random(r)) + 1;
    if (random(4) != 0) p += int32_t(random(r));
    if (random(100) == 0) p = int32_t(uint32_t(int32_t(random(10)) + 5) * uint32_t(p));
    else if (random(fiveTimesOdds) == 0) p = int32_t(uint32_t(p) * 5u);
    else if (random(multiplyOdds) == 0) p = int32_t(uint32_t(int32_t(random(2)) + 2) * uint32_t(p));
    if (p > 50) p = int32_t(random(10)) + 41;
    if (random(2) == 0) p = int32_t(0u - uint32_t(p));
    return p;
}
template<class Random> int32_t classicAirWave(int32_t reserve, Random random) {
    return classicReserveWave(reserve, 10, 2, random);
}
template<class Random> int32_t classicNavalWave(int32_t reserve, Random random) {
    return classicReserveWave(reserve, 20, 3, random);
}

// 0x40c220: all three wave sizes, in that order.
template<class Random>
void classicRollWaves(sim::RetailAiSquad& base, int32_t enemyPopulation, Random random) {
    auto& p = base.parameters;
    p[5] = classicGroundWave(p[2], enemyPopulation, random);
    p[6] = classicAirWave(p[3], random);
    p[7] = classicNavalWave(p[4], random);
}

// 0x40c560: a base squad's quotas (p1 builders, p2 ground, p3 air, p4 naval
// reserve), larger for the home base, then the three wave sizes.
template<class Random>
void classicConstructBase(sim::RetailAiSquad& base, int slot, int32_t enemyPopulation, Random random) {
    base.kind = sim::RetailAiSquadKind::Base;
    auto& p = base.parameters;
    if (slot == classic_slot::kHome) {
        p[1] = int32_t(random(2)) + 1; p[2] = int32_t(random(4)) + 2;
        p[3] = int32_t(random(4)); p[4] = int32_t(random(4));
    } else {
        p[1] = int32_t(random(2)) + 1; p[2] = int32_t(random(2)) + 1;
        p[3] = int32_t(random(2)); p[4] = int32_t(random(2));
    }
    classicRollWaves(base, enemyPopulation, random);
}

// 0x40fed0: the manager's squads. Every squad's clock starts at 0 (it thinks on
// the first tick), the assignment countdown at 30. Strike ctor 0x40e9a0: p0 its
// backup, p1 the cohesion, p2 0, p3 6, p4 (target) none, p5 0; backup 0x40ea00:
// p0 its strike. VTOL 0x40eac0 and the homeless squad 0x40a5c0 set nothing.
template<class Random>
void classicConstructSquads(ClassicSquads& s, int32_t enemyPopulation, Random random) {
    using K = sim::RetailAiSquadKind;
    s.schedule = {};
    for (auto& q : s.squads) q = {};
    s.firstPassDone = false;
    auto present = [&](int slot, K kind) { s.squads[size_t(slot)].kind = kind; s.schedule.groups[size_t(slot)].present = true; };
    for (int slot = 1; slot <= classic_slot::kLastBase; ++slot) {
        present(slot, K::Base);
        classicConstructBase(s.squads[size_t(slot)], slot, enemyPopulation, random);
    }
    auto pair = [&](int strike, int32_t cohesion) {
        present(strike, K::Strike); present(strike + 1, K::Backup);
        auto& p = s.squads[size_t(strike)].parameters;
        p[0] = strike + 1; p[1] = cohesion; p[2] = 0; p[3] = 6; p[4] = 0; p[5] = 0;
        s.squads[size_t(strike + 1)].parameters[0] = strike;
    };
    for (int slot = classic_slot::kFirstStrike; slot < classic_slot::kFirstNavalStrike; slot += 2) pair(slot, kClassicGroundCohesion);
    for (int slot = classic_slot::kFirstNavalStrike; slot < classic_slot::kFirstVtol; slot += 2) pair(slot, kClassicNavalCohesion);
    for (int slot = classic_slot::kFirstVtol; slot <= classic_slot::kLastVtol; ++slot) present(slot, K::Vtol);
    present(classic_slot::kHomeless, K::Reserve);
    pair(classic_slot::kMassStrike, kClassicMassCohesion);
}

// --- membership -----------------------------------------------------------------

// 0x50b4f0: move unit i to `slot` (-1 drops it, 0 unassigns). Any slot but -1
// needs a live unit. Leaving a squad swaps the squad's last member into the
// unit's place (0x50b300 -> 0x50b2c0); joining appends.
inline void classicSetSquad(ClassicSquads& s, std::vector<ClassicSquadUnit>& units, size_t i, int32_t slot) {
    auto& u = units[i];
    if (slot != -1 && !u.live()) return;
    if (u.squad == slot) return;
    if (u.squad > 0 && u.squad < 100) {
        auto& m = s.squads[size_t(u.squad)].members;
        for (size_t k = 0; k < m.size(); ++k)
            if (m[k] == u.id) { m[k] = m.back(); m.pop_back(); break; }
    }
    if (slot > 0 && slot < 100) s.squads[size_t(slot)].members.push_back(u.id);
    u.squad = slot;
}

// 0x40c750: a base's anchor cell from its p0, if the record exists.
inline std::optional<ClassicCell> classicAnchor(const ClassicSquads& s, int32_t p0) {
    if (p0 == 0) return std::nullopt;
    const bool site = p0 < 0;
    const int32_t index = int32_t((site ? 0u - uint32_t(p0) : uint32_t(p0)) - 1u);
    const auto& table = site ? s.sites : s.starts;
    if (index < 0 || uint32_t(index) >= table.size()) return std::nullopt;
    return table[size_t(index)];
}

// 0x40c670: anchor a base on start record `index` (p0 = index+1) if it exists.
inline bool classicAnchorOnStart(ClassicSquads& s, int slot, int32_t index) {
    if (index < 0 || uint32_t(index) >= s.starts.size() || !s.starts[size_t(index)]) return false;
    s.squads[size_t(slot)].parameters[0] = index + 1;
    return true;
}

namespace detail {
struct ClassicUnitIndex {
    std::unordered_map<int, size_t> at;
    explicit ClassicUnitIndex(const std::vector<ClassicSquadUnit>& units) {
        at.reserve(units.size());
        for (size_t i = 0; i < units.size(); ++i) at.emplace(units[i].id, i);
    }
    const ClassicSquadUnit* find(const std::vector<ClassicSquadUnit>& units, int id) const {
        const auto it = at.find(id);
        return it == at.end() ? nullptr : &units[it->second];
    }
};
}

// 0x40c610: a base's centre in 16.16 pixels: the anchor cell's centre, else the
// centroid of its live members (0x40aa50, category 0).
inline std::optional<std::pair<int32_t, int32_t>> classicBaseCentre(
        const ClassicSquads& s, const std::vector<ClassicSquadUnit>& units,
        const detail::ClassicUnitIndex& index, int slot) {
    const auto& base = s.squads[size_t(slot)];
    if (const auto cell = classicAnchor(s, base.parameters[0]))
        return std::pair{int32_t(uint32_t(int32_t(cell->first)) << 20) + 0x80000,
                         int32_t(uint32_t(int32_t(cell->second)) << 20) + 0x80000};
    sim::RetailAiCentroid centroid;
    for (int id : base.members)
        if (const auto* u = index.find(units, id); u && sim::retailAiCategory(u->unitClass(), 0)) centroid.add(u->x, u->z);
    return centroid.center();
}

// 0x40cf30: a base's ring radius in pixels, from the footprints of its structures
// (category 1); none without a centre.
inline std::optional<int32_t> classicBaseRadius(const ClassicSquads& s, const std::vector<ClassicSquadUnit>& units,
                                                const detail::ClassicUnitIndex& index, int slot, unsigned ring) {
    if (!classicBaseCentre(s, units, index, slot)) return std::nullopt;
    int32_t footprint = 0;
    for (int id : s.squads[size_t(slot)].members)
        if (const auto* u = index.find(units, id); u && sim::retailAiCategory(u->unitClass(), 1))
            footprint = int32_t(uint32_t(footprint) + uint32_t(int32_t(u->footX)) + uint32_t(int32_t(u->footZ)));
    return sim::retailAiBaseRadius(footprint, ring);
}

// 0x40a880: how many of a base's members are in category `category` (0x40a5f0).
inline int32_t classicBaseCount(const ClassicSquads& s, const std::vector<ClassicSquadUnit>& units,
                                const detail::ClassicUnitIndex& index, int slot, unsigned category) {
    int32_t n = 0;
    for (int id : s.squads[size_t(slot)].members)
        if (const auto* u = index.find(units, id); u && sim::retailAiCategory(u->unitClass(), category)) ++n;
    return n;
}

// 0x40a9b0: a base's firepower in category `category`: over its armed members
// there (+0x130 0x8000000), the sum of their current weapon's damage per reload
// tick (integer division; the damage itself at a reload of 0).
inline int32_t classicBaseFirepower(const ClassicSquads& s, const std::vector<ClassicSquadUnit>& units,
                                    const detail::ClassicUnitIndex& index, int slot, unsigned category) {
    int32_t sum = 0;
    for (int id : s.squads[size_t(slot)].members) {
        const auto* u = index.find(units, id);
        if (!u || !sim::retailAiCategory(u->unitClass(), category) || !(u->flags & ClassicSquadUnit::kArmed)) continue;
        sum = int32_t(uint32_t(sum) + uint32_t(u->weaponReload ? int32_t(u->weaponDamage) / int32_t(u->weaponReload)
                                                                : int32_t(u->weaponDamage)));
    }
    return sum;
}

// 0x40c890's duplicate-factory test: a member of the base of catalogue type
// `type` (unit +0) that has no first mission, or whose first mission is not
// BuildingBuild -- an idle factory of that type.
inline bool classicIdleFactory(const ClassicSquads& s, const std::vector<ClassicSquadUnit>& units,
                               const detail::ClassicUnitIndex& index, int slot, uint16_t type) {
    for (int id : s.squads[size_t(slot)].members)
        if (const auto* u = index.find(units, id); u && u->type == type && (!u->mission || !u->buildingBuild))
            return true;
    return false;
}

// 0x40c830: a member of the base whose first mission is a build (flag 8) of a
// mana-site type (type +0x264 bit 31) -- a builder already on a deposit.
inline bool classicLodestoneBusy(const ClassicSquads& s, const std::vector<ClassicSquadUnit>& units,
                                 const detail::ClassicUnitIndex& index, int slot) {
    for (int id : s.squads[size_t(slot)].members)
        if (const auto* u = index.find(units, id); u && u->mission && (u->missionFlags & 8) && u->buildsManaSite)
            return true;
    return false;
}

// 0x40af70 with no minimum: the occupied base 1..20 whose centre is nearest.
inline unsigned classicNearestBase(const ClassicSquads& s, const std::vector<ClassicSquadUnit>& units,
                                   const detail::ClassicUnitIndex& index, int32_t x, int32_t z) {
    std::array<sim::RetailAiBaseCandidate, 20> bases;
    for (int slot = 1; slot <= classic_slot::kLastBase; ++slot) {
        auto& c = bases[size_t(slot - 1)];
        c.occupied = !s.squads[size_t(slot)].members.empty();
        if (c.occupied) c.center = classicBaseCentre(s, units, index, slot);
    }
    return sim::retailNearestAiBase(bases, x, z);
}

// 0x40af20: the first empty base 2..20, or 0.
inline int classicFirstEmptyBase(const ClassicSquads& s) {
    for (int slot = 2; slot <= classic_slot::kLastBase; ++slot)
        if (s.squads[size_t(slot)].members.empty()) return slot;
    return 0;
}

// 0x40fe80: every dying unit still in a squad leaves it (squad -1).
inline void classicDropDying(ClassicSquads& s, std::vector<ClassicSquadUnit>& units) {
    for (size_t i = 0; i < units.size(); ++i)
        if ((units[i].flags & ClassicSquadUnit::kLive) && (units[i].flags & ClassicSquadUnit::kDying) &&
            units[i].squad != -1)
            classicSetSquad(s, units, i, -1);
}

struct ClassicAssignContext {
    uint32_t tick = 0;          // the game tick (game +0x19f44)
    int32_t scenarioMode = 2;   // [game+0x175dc]+0: 1 = a campaign mission
    int32_t startIndex = -1;    // the player's start record (player +0xec)
};

// 0x40eeb0, the assignment pass. `standing(i)` is retail's standing orders
// 0x5198a0(u, 2) (our Stance 0: move 1, fire 2) and `clear(i)` its mission clear
// 0x4d6a50(u, 0), both for units[i]; retail calls them in this order per unit.
//  - Once (the first call), and only if the tick is <= 30: every commander joins
//    base 1, and outside a campaign mission base 1 is anchored on the start
//    location. A campaign mission instead sorts its preplaced sacred-site
//    buildings, then its unassigned builders, into the nearest base when inside
//    that base's ring-1 radius, else into a new base (the first empty one, else
//    the homeless squad 96); if every base is still empty, every unit goes to 96.
//    The commander gets no standing orders here.
//  - From the massattack tick on, every pass: each armed, mobile, menu-less unit
//    not yet in 97/98 gets standing orders, has its missions cleared and joins
//    98, the mass attack's backup -- unfinished ones included.
//  - Every pass: each live, selectable, finished, free, unassigned unit gets
//    standing orders and joins the nearest occupied base by centre, else base 2.
template<class Standing, class Clear>
void classicAssign(ClassicSquads& s, std::vector<ClassicSquadUnit>& units, const ClassicAssignContext& ctx,
                   Standing standing, Clear clear) {
    const detail::ClassicUnitIndex index(units);
    if (!s.firstPassDone) {
        s.firstPassDone = true;
        if (ctx.tick <= 30) {
            for (size_t i = 0; i < units.size(); ++i) {
                if (!units[i].live() || !units[i].commander) continue;
                classicSetSquad(s, units, i, classic_slot::kHome);
                if (ctx.scenarioMode != 1) classicAnchorOnStart(s, classic_slot::kHome, ctx.startIndex);
            }
            if (ctx.scenarioMode == 1) {
                auto place = [&](size_t i) {
                    const auto& u = units[i];
                    int slot = int(classicNearestBase(s, units, index, u.x, u.z));
                    if (slot > 0) {
                        const auto radius = classicBaseRadius(s, units, index, slot, 1);
                        const auto centre = classicBaseCentre(s, units, index, slot);
                        bool inside = false;
                        if (radius && centre) {
                            const double dz = double(int32_t(uint32_t(centre->second) - uint32_t(u.z)));
                            const double dx = double(int32_t(uint32_t(centre->first) - uint32_t(u.x)));
                            // fsqrt then _ftol (truncate to 64 bits, keep the low 32); the
                            // whole-pixel part is its high word.
                            const int32_t d = int32_t(uint32_t(uint64_t(int64_t(std::sqrt(dz * dz + dx * dx)))));
                            inside = int32_t(int16_t(uint32_t(d) >> 16)) <= *radius;
                        }
                        if (!inside) slot = 0;
                    }
                    if (slot <= 0) slot = classicFirstEmptyBase(s);
                    classicSetSquad(s, units, i, slot > 0 ? slot : classic_slot::kHomeless);
                };
                for (size_t i = 0; i < units.size(); ++i)
                    if (units[i].live() && units[i].sacredSite) place(i);
                for (size_t i = 0; i < units.size(); ++i)
                    if (units[i].live() && units[i].squad <= 0 && units[i].builder) place(i);
                bool empty = true;
                for (int slot = 1; slot <= classic_slot::kLastBase && empty; ++slot)
                    empty = s.squads[size_t(slot)].members.empty();
                if (empty)
                    for (size_t i = 0; i < units.size(); ++i)
                        if (units[i].live()) classicSetSquad(s, units, i, classic_slot::kHomeless);
            }
        }
    }
    if (s.massAttack && ctx.tick >= s.massAttack) {
        for (size_t i = 0; i < units.size(); ++i) {
            const auto& u = units[i];
            if (!u.live() || u.squad == classic_slot::kMassBackup || u.squad == classic_slot::kMassStrike ||
                !(u.flags & ClassicSquadUnit::kArmed) || !u.mover || u.menu > 0) continue;
            standing(i); clear(i);
            classicSetSquad(s, units, i, classic_slot::kMassBackup);
        }
    }
    for (size_t i = 0; i < units.size(); ++i) {
        const auto& u = units[i];
        if (!u.live() || !(u.flags & ClassicSquadUnit::kSelectable) || !u.finished || u.held ||
            (u.carried && !u.carrierHolds) || u.squad != 0) continue;
        standing(i);
        const unsigned slot = classicNearestBase(s, units, index, u.x, u.z);
        classicSetSquad(s, units, i, slot ? int32_t(slot) : 2);
    }
}

// --- the base think ---------------------------------------------------------------

// 0x40b320 after its reschedule and empty-base test (retailTickAiSquad), up to
// the end of its member loop:
//  - a base anchored on a record that no longer exists loses its anchor;
//  - no centre (0x40c610) or ring-0 radius (0x40cf30): nothing more this think;
//  - `threat(centre, radius)` is 0x412000, "a listed enemy inside the ring";
//  - then each member, in member order, that is live, selectable, finished
//    (+0x108 == 0; unordered counts as equal), not held (+0x104) and not carried
//    by a non-holder is planned: a type with a build menu goes to the factory
//    planner 0x40dcb0 if it is a structure, else -- unless it is a non-commander
//    whose first mission has class 1 or 2 -- to the constructor planner 0x40c890
//    if it has a navigator; a type without a menu goes to the combat planner
//    0x40d370 if it has a navigator. Planners get units[i] (and the threat).
// The rest of the think (quota growth, surpluses, transfers, expansion, waves:
// classicBaseLaunch in ai/classicwaves.h) runs when this returns true, as
// retail's does only after a loop it reached.
template<class Threat, class Factory, class Constructor, class Combat>
bool classicBaseMembers(ClassicSquads& s, const std::vector<ClassicSquadUnit>& units, int slot,
                        Threat threat, Factory factory, Constructor constructor, Combat combat) {
    auto& base = s.squads[size_t(slot)];
    if (base.parameters[0] != 0 && !classicAnchor(s, base.parameters[0])) base.parameters[0] = 0;
    const detail::ClassicUnitIndex index(units);
    const auto centre = classicBaseCentre(s, units, index, slot);
    if (!centre) return false;
    const auto radius = classicBaseRadius(s, units, index, slot, 0);
    if (!radius) return false;
    const bool danger = threat(*centre, *radius);
    for (size_t k = 0; k < base.members.size(); ++k) {
        const auto it = index.at.find(base.members[k]);
        if (it == index.at.end()) continue;
        const size_t i = it->second;
        const auto& u = units[i];
        if (!(u.flags & ClassicSquadUnit::kLive) || (u.flags & ClassicSquadUnit::kDying) ||
            !(u.flags & ClassicSquadUnit::kSelectable) || !u.finished || u.held || (u.carried && !u.carrierHolds))
            continue;
        if (u.menu != 0) {
            if (u.flags & ClassicSquadUnit::kStructure) factory(i);
            else if ((u.commander || (u.missionClass != 1 && u.missionClass != 2)) && u.mover) constructor(i, danger);
        } else if (u.mover) {
            combat(i, danger);
        }
    }
    return true;
}

// The campaign keyword: `massattack <n>` in a .ota PlayerN line (sscanf "%i", so
// 0x/0 prefixes count) gives n x 30 ticks (scenario parser 0x4c8541..0x4c8575);
// no keyword, or no number after it, gives 0.
uint32_t classicMassAttackTicks(const std::string& playerLine);

}  // namespace tak::ai
