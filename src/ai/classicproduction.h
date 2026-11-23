#pragma once

// The Classic level's production choice (docs/classic-ai-plan.md §1.3, §1.6):
// retail's weighted menu draw 0x412d00 over a builder type's canbuild menu, and
// the factory planner 0x40dcb0 that queues its pick.
//
// Both are thin layers over the shared sim templates, which are what the
// emulation oracles check: sim::retailAiBuildWeight (0x412c90, check_ai_build_weight),
// sim::retailAiChooseBuild (0x412d00, check_ai_build_choice) and
// sim::retailAiPlanFactory (0x40dcb0, check_ai_factory_plan). The layer itself --
// the menu walk with the brain's own tables, the allocation the planner gates on,
// and the base think's dispatch to it -- is checked as one by
// tools/re/check_ai_factory_think.py (classic_ai_test --factory-think), with
// retail's own 0x40b320, 0x40dcb0, 0x412d00, 0x412c90, 0x409890 and 0x410de0
// running and the allocation written by retail's economy step 0x51d837.
//
// Server-side only, like the rest of the brain.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "sim/retailai.h"

namespace tak::ai {

// 0x412d00(player, unit, special): one pass over the builder type's menu (retail
// type +0x132, in canbuild [Menu] Priority order). `weight(t)` is 0x412c90 for
// catalogue type t (0 at its limit, else pref * priority / 100), `lodestone(t)`
// its draw class (yardmap[0] & 0x80) and `sameSide(t)` whether its side string
// equals the builder's (retail's strcmp of type +0x8a). Each kept entry -- weight
// > 0, of the requested class -- costs one rand(total) (none for the first, as
// rand(n < 2) does not draw); a winner from another side gives 0.
template<class Weight, class Lodestone, class SameSide, class Random>
uint16_t classicMenuDraw(const std::vector<uint16_t>& menu, bool special, Weight weight, Lodestone lodestone,
                         SameSide sameSide, Random random) {
    std::vector<sim::RetailAiBuildChoice> choices;
    choices.reserve(menu.size());
    for (uint16_t t : menu) choices.push_back({t, int32_t(weight(t)), bool(lodestone(t)), bool(sameSide(t))});
    return sim::retailAiChooseBuild(choices, special, random);
}

// 0x40dcb0, the factory planner, for a ready factory member of a base: nothing
// below allocation 0.7/3 or while the factory has a BuildingBuild mission (one
// unit at a time); else the ordinary draw over its `menu` (special 0, the
// callables as for classicMenuDraw), and under allocation 0.7 an item costing
// over 500 is deferred when rand(int((cost-500)/300+2)) != 0 and the owner's unit
// count is under rand(30)+30. Otherwise `queue(t)` is retail's
// 0x429810(type, factory, 1): one BuildingBuild of one unit.
//
// `allocation` is the owner's construction allocation (resource object +8, which
// the economy step 0x51d837 sets to min(1, stored / this tick's demand); not the
// store fill). `population` is the owner's unit count (player +0xe8).
template<class Weight, class Lodestone, class SameSide, class Cost, class Random, class Queue>
bool classicPlanFactory(float allocation, bool producing, uint16_t population, const std::vector<uint16_t>& menu,
                        Weight weight, Lodestone lodestone, SameSide sameSide, Cost cost, Random random,
                        Queue queue) {
    return sim::retailAiPlanFactory(allocation, producing, population,
        [&] { return classicMenuDraw(menu, false, weight, lodestone, sameSide, random); }, cost, random, queue);
}

// --- the constructor planner -----------------------------------------------------

// What 0x40c890 reads of the mobile builder it plans.
struct ClassicConstructorUnit {
    bool commander = false;      // type +0x264 0x40000
    int16_t hp = 0;              // unit +0x10c
    int32_t maxDamage = 0;       // type +0x1be (a dword)
    float mana = 0, maxMana = 0; // unit +0xd8 / +0xdc
    // Its current weapon (unit +0x44), and that weapon's mana per shot over the
    // unit's cost multiplier (0x531ea0).
    bool weapon = false;
    double shotCost = 0;
    // Its first mission (unit +0x60): whether there is one, its class byte
    // (descriptor +0x10: 1 and 2 are the attack kinds) and its flag 8 (a build:
    // MobileBuild, HelpBuild or BuildingBuild).
    bool mission = false;
    uint8_t missionClass = 0;
    bool building = false;
    bool navigator = true;       // a ground navigator (unit +8 -> +4): reachability applies
};

// What it reads of a type it may build (catalogue facts).
struct ClassicConstructorType {
    uint8_t bmcode = 0;          // type +0x24a
    int32_t menu = 0;            // type +0x12e: build menu entries
    bool armed = false;          // type +0x260 0x10000
};

// 0x531ea0: a weapon's mana per shot (weapon +0xd4) over the unit's cost
// multiplier (0x5193c0: 1 for a type without veterancy, else the unit's own).
inline double classicShotCost(float manaPerShot, float multiplier) {
    return double(manaPerShot) / double(multiplier);
}

// The rectangle 0x40c890 hands 0x4e1570 for a site (a footprint centre, 16.16):
// the footprint's top-left cell less builddistance/16 cells (type +0x230, an
// unsigned word), and its size plus (2 * builddistance)/16 cells -- each a word.
struct ClassicReachRect { int16_t x = 0, z = 0, w = 0, h = 0; };
inline ClassicReachRect classicReachRect(std::pair<int32_t, int32_t> site, int16_t footX, int16_t footZ,
                                         uint16_t buildDistance) {
    auto cell = [](int32_t p, int16_t foot) {
        return int16_t(int32_t(uint32_t(p) - (uint32_t(int32_t(foot)) << 19) + 0x80000u) >> 20);
    };
    const int16_t grow = int16_t(buildDistance >> 4), span = int16_t((2 * int32_t(buildDistance)) >> 4);
    return {int16_t(cell(site.first, footX) - grow), int16_t(cell(site.second, footZ) - grow),
            int16_t(footX + span), int16_t(footZ + span)};
}

// The static bit of a navigator grid (cell nibble bit 8), which 0x4e1570's mode 1
// (the static grade) tests. The grid build 0x4dfee0 sets it once, at map load:
// it rates every cell (0x508660), takes the minimum over the class footprint
// [x, x + footX) x [z, z + footZ), and marks the cell where that is 0 (0x4e017c ->
// 0x4dfe90, its only caller) -- so where the footprint holds a cell rated 0, or
// runs past the map's right or bottom edge. `zero(x, z)` is whether a cell's own
// rating is 0. Returns one byte per cell (row-major), 1 where the bit is set.
template<class Zero>
std::vector<uint8_t> classicStaticPlane(int width, int height, int footX, int footZ, Zero zero) {
    const size_t w = size_t(std::max(width, 0)), h = size_t(std::max(height, 0));
    std::vector<uint8_t> rated(w * h), plane(w * h, 1);
    for (size_t z = 0; z < h; ++z)
        for (size_t x = 0; x < w; ++x) rated[z * w + x] = zero(int(x), int(z)) ? 1 : 0;
    for (int z = 0; z + footZ <= height; ++z)
        for (int x = 0; x + footX <= width; ++x) {
            uint8_t blocked = 0;
            for (int dz = 0; dz < footZ && !blocked; ++dz)
                for (int dx = 0; dx < footX && !blocked; ++dx) blocked = rated[size_t(z + dz) * w + size_t(x + dx)];
            plane[size_t(z) * w + size_t(x)] = blocked;
        }
    return plane;
}

// 0x40c890, the constructor planner, for a ready mobile builder of a base, with
// the base think's threat answer. The host supplies retail's queries and orders:
//   allocation()            the owner's construction allocation (resource +8)
//   firepower()             0x40a9b0(4): the base's ground-combat firepower
//   builders()              0x40a880(2): the base's constructors, the Monarch not counted
//   desiredBuilders()       the base's p1
//   tick(), alarm()         the game tick and the Monarch alarm deadline (manager +0xd)
//   draw(special)           0x412d00(player, unit, special): the menu draw
//   type(t)                 ClassicConstructorType of catalogue type t
//   idleFactory(t)          the duplicate-factory test: a base member of type t whose
//                           first mission is not BuildingBuild (or that has none)
//   typeCount(t)            0x410e60: the owner's units of type t (unfinished included)
//   radius(ring)            0x40cf30: the base's ring radius, if it has a centre
//   site(t, radius)         0x412f80 around the base centre: a footprint centre (16.16)
//   reachable(t, site)      0x4e1570 from the builder to the site within builddistance
//   lodestoneBusy()         0x40c830: a base member's first mission builds a mana site
//   build(t, site)          the build order (kind 14 through 0x4de530 / 0x4d78a0)
//   cancel()                0x4d6a50(unit, 0): its missions cleared
//   retreat()               0x40d010, the Monarch's retreat
//   combat()                0x40d370(unit, threat), the base defence planner
// The order of the host calls is retail's; draws happen only in draw() and site().
//
//  - The Monarch first: it flees below maxdamage * k / (4 * m) HP -- k = 2 when its
//    mana is at most half, m = 2 when its weapon can fire now (a per-shot cost under
//    0.01 or within its mana) -- but only if the base has ground firepower. Then, with
//    a threat, it cancels a build and retreats; without one it goes straight to the
//    builder gate. Otherwise a first mission of class 1 or 2 leaves it alone, and with
//    a threat and no ground firepower it cancels a build and defends. The builder
//    gate: with as many base constructors as p1 it defends; before the alarm deadline
//    it retreats if it was low, else defends. Past both it builds like any builder.
//  - A builder already on a build mission is left alone.
//  - The ordinary pick (not under allocation 0.7): draw(0); a factory type (bmcode 0
//    with a menu) is dropped while an idle one of that type is in the base; a tower
//    (bmcode 0, armed, no menu) is dropped for the Monarch at 4 of that type; its
//    site is placed on ring 2 for the Monarch's first of a type and ring 3 for later
//    ones, ring 1 for another builder's tower and ring 2 otherwise; it counts if the
//    site is reachable (or the builder has no ground navigator).
//  - The lodestone pick, drawn on every think: draw(1), unless a base member is
//    already building a mana site; ring 2 for the Monarch, 1 for others; if it is
//    placed and reachable it replaces the ordinary pick.
//  - The pick, if any, is ordered built; then the builder always goes to combat().
template<class Host>
void classicPlanConstructor(Host& h, const ClassicConstructorUnit& u, bool threat) {
    const bool lowFunds = !(double(h.allocation()) >= 0.7);   // fcomp: below or unordered
    if (u.commander) {
        bool castable = false;
        if (u.weapon) {
            const double c = u.shotCost;
            castable = !(c >= 0.01) || !(c > double(u.mana));
        }
        const double ratio = double(u.mana) / double(u.maxMana);
        const uint32_t k = ratio > 0.5 ? 1u : 2u;
        const uint32_t threshold = uint32_t(u.maxDamage) * k / ((castable ? 2u : 1u) << 2);
        bool low = false;
        if (uint32_t(int32_t(u.hp)) < threshold && h.firepower() > 0) {
            low = true;
            if (threat) {
                if (u.building) h.cancel();
                h.retreat();
                return;
            }
        } else {
            if (u.mission && (u.missionClass == 1 || u.missionClass == 2)) return;
            if (threat && h.firepower() == 0) {
                if (u.building) h.cancel();
                h.combat();
                return;
            }
        }
        if (h.builders() >= h.desiredBuilders()) { h.combat(); return; }
        if (h.tick() < h.alarm()) {
            if (low) h.retreat(); else h.combat();
            return;
        }
    }
    if (u.building) return;
    bool ok = false;
    uint16_t choice = 0;
    std::optional<std::pair<int32_t, int32_t>> site;
    if (!lowFunds) {
        choice = h.draw(false);
        if (choice) [&] {
            const ClassicConstructorType t = h.type(choice);
            if (t.bmcode == 0 && t.menu > 0 && h.idleFactory(choice)) { choice = 0; return; }
            const bool tower = t.bmcode == 0 && t.armed && t.menu == 0;
            if (tower && u.commander && h.typeCount(choice) >= 4) return;
            const unsigned ring = u.commander ? (h.typeCount(choice) != 0 ? 3u : 2u) : (tower ? 1u : 2u);
            const std::optional<int32_t> r = h.radius(ring);
            if (!r) return;
            site = h.site(choice, *r);
            if (!site) return;
            ok = !u.navigator || h.reachable(choice, *site);
        }();
    }
    const uint16_t lode = h.draw(true);
    if (lode && !h.lodestoneBusy()) {
        if (const std::optional<int32_t> r = h.radius(u.commander ? 2u : 1u))
            if (const auto at = h.site(lode, *r))
                if (!u.navigator || h.reachable(lode, *at)) { choice = lode; site = at; ok = true; }
    }
    if (ok) h.build(choice, *site);
    h.combat();
}

}  // namespace tak::ai
