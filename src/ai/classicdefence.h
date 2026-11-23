#pragma once

// The Classic level's base defence (docs/classic-ai-plan.md §1.8): retail's
// threat test 0x412000, the enemy query 0x411ae0 under it, the combat-member
// planner 0x40d370 (the ring by role, the wounded radius, the threat candidates,
// the defence and patrol points and the orders), and the Monarch's retreat
// 0x40d010.
//
// Pure templates over the brain's own views of the units and a host for the
// queries and orders, so the same code is what ClassicBrain runs and what the
// emulation oracles check against the retail routines:
// tools/re/check_ai_threat.py (0x412000 and 0x411ae0), check_ai_defence.py
// (0x40d370 with the real base centre, radius, enemy query, trig and the
// weapon-class answer under it) and check_ai_retreat.py (0x40d010). The patrol
// points of a member with no threat candidates are the shared
// sim::retailAiPatrolPoints (check_ai_patrol_points).
//
// Server-side only, like the rest of the brain.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "ai/classicplacement.h"   // classicFootprintCell: the placement test's cell
#include "sim/retailaipatrol.h"
#include "sim/retailmotion.h"

namespace tak::ai {

// One unit of the player's visible-enemy list (build manager +5, rebuilt by
// 0x411700 every >= 7 ticks), as the AI reads it at query time.
struct ClassicEnemy {
    static constexpr uint32_t kLive = 0x1000000u;     // unit +0x130: the slot holds a unit ...
    static constexpr uint32_t kDying = 0x1000u;       // ... that is dying
    static constexpr uint32_t kUnready = 0x2000u;     // a scenario placement not yet selectable (U16)
    int id = 0;
    uint32_t flags = 0;                 // unit +0x130 (its low two bits: 1 = on the ground)
    int32_t x = 0, y = 0, z = 0;        // +0x68 / +0x6c / +0x70, 16.16 pixels
    bool cloaked = false;               // +0x114 bit 4
    // Read by the target picker (ai/classicwaves.h):
    int16_t cellX = 0, cellZ = 0;       // +0x74 / +0x76: its footprint's top-left cell
    bool unfinished = false;            // +0x108 != 0 (an unordered compare counts as 0)
    bool manaSite = false;              // type +0x264 bit 31
    bool live() const { return (flags & kLive) && !(flags & kDying); }
};

using ClassicPoint = std::array<int32_t, 3>;   // x, y, z in 16.16 pixels

namespace detail {
// The squared distance retail compares with r*r: the high dwords of the two
// 64-bit squares of the 16.16 differences (whole square pixels), added as
// 32-bit ints, so far-off pairs can wrap negative as retail's do.
inline int32_t classicHighSquares(int32_t ax, int32_t az, int32_t bx, int32_t bz) {
    const int64_t dz = std::bit_cast<int32_t>(uint32_t(az) - uint32_t(bz));
    const int64_t dx = std::bit_cast<int32_t>(uint32_t(ax) - uint32_t(bx));
    return std::bit_cast<int32_t>(uint32_t(uint64_t(dz * dz) >> 32) + uint32_t(uint64_t(dx * dx) >> 32));
}
// fsqrt of the squares of 32-bit differences, then _ftol: its low dword's whole
// pixels (bits 16..31) as a signed word.
inline int16_t classicPixels(double squares) {
    const int64_t d = int64_t(std::sqrt(squares));
    return std::bit_cast<int16_t>(uint16_t(uint32_t(uint64_t(d)) >> 16));
}
inline int16_t classicPlanarPixels(int32_t ax, int32_t az, int32_t bx, int32_t bz) {
    const double dz = double(std::bit_cast<int32_t>(uint32_t(az) - uint32_t(bz)));
    const double dx = double(std::bit_cast<int32_t>(uint32_t(ax) - uint32_t(bx)));
    return classicPixels(dx * dx + dz * dz);
}
// -sin / -cos of a BAM angle at a 16.16 distance (0x5360bf / 0x5360f3), negated.
inline std::pair<int32_t, int32_t> classicOffset(uint16_t angle, int32_t distance) {
    return {std::bit_cast<int32_t>(0u - uint32_t(sim::retailScaledSine(angle, distance))),
            std::bit_cast<int32_t>(0u - uint32_t(sim::retailScaledCosine(angle, distance)))};
}
inline int32_t wrapAdd(int32_t a, int32_t b) { return std::bit_cast<int32_t>(uint32_t(a) + uint32_t(b)); }
}  // namespace detail

// 0x412000(side, centre, radius): a unit of the visible-enemy list, live and not
// dying, within `radius` pixels of the centre.
template<class List>
bool classicThreat(const List& visible, std::pair<int32_t, int32_t> centre, int32_t radius) {
    const int32_t r2 = std::bit_cast<int32_t>(uint32_t(radius) * uint32_t(radius));
    for (const ClassicEnemy& e : visible)
        if (e.live() && detail::classicHighSquares(centre.first, centre.second, e.x, e.z) <= r2) return true;
    return false;
}

// 0x411ae0(side, centre, radius, -, out): the live, not dying units of the
// visible-enemy list within `radius` of the centre, in list order. When none is
// and the player owns a targeting-upgrade unit (build manager +0x79), the same
// test runs over list +0x25 (visible enemies with +0x130 0x100). No shipped FBI
// has istargetingupgrade, so the brain never takes that branch.
template<class List>
std::vector<ClassicEnemy> classicEnemiesNear(const List& visible, std::pair<int32_t, int32_t> centre, int32_t radius,
                                             bool upgrade = false, const List* upgraded = nullptr) {
    const int32_t r2 = std::bit_cast<int32_t>(uint32_t(radius) * uint32_t(radius));
    std::vector<ClassicEnemy> out;
    for (const ClassicEnemy& e : visible)
        if (e.live() && detail::classicHighSquares(centre.first, centre.second, e.x, e.z) <= r2) out.push_back(e);
    if (upgrade && upgraded && out.empty())
        for (const ClassicEnemy& e : *upgraded)
            if (e.live() && detail::classicHighSquares(centre.first, centre.second, e.x, e.z) <= r2) out.push_back(e);
    return out;
}

// What 0x40d370 and 0x40d010 read of the unit they plan.
struct ClassicCombatUnit {
    bool armed = false;          // +0x130 0x8000000
    bool commander = false;      // type +0x264 0x40000
    bool builder = false;        // type +0x260 0x100
    bool canFly = false;         // type +0x260 0x800
    int16_t hp = 0;              // +0x10c
    uint32_t maxDamage = 0;      // type +0x1be
    ClassicPoint at{};           // +0x68..+0x70
    int16_t sight = 0;           // type +0x226
    int32_t range = 0;           // its current weapon (+0xc) +0x90
    bool melee = false;          // that weapon's class answers vtable +0x14 (MeleeWeapon only)
    // Its first mission (+0x60): the flags it copied from its descriptor (+0x5a:
    // 0x200 has a target unit, 0x400 a goal point, 0x2000 the type's default
    // mission or one pushed onto it), its target (+0x16) and goal (+0x22/+0x2a).
    bool mission = false;
    uint32_t missionFlags = 0;
    int missionTarget = 0;
    int32_t goalX = 0, goalZ = 0;
};

// Retail's order commands as 0x4de530 takes them.
constexpr int kClassicMove = 2, kClassicPatrol = 9;

// 0x40d370(unit, threat), the combat-member planner, for a ready base member
// (a combat unit, a mobile builder after the constructor planner, the Monarch
// when it defends). The host supplies:
//   centre()                 the base centre (vtable +8, else 0x412c10), if any
//   radius(ring)             0x40cf30: the base's ring radius, if it has a centre
//   enemies(centre, r)       0x411ae0 around the centre
//   damage(e)                0x531e50: its current weapon's damage against e
//   random(n)                the game RNG
//   placeable(x, z)          0x507d10 for its own type at the footprint cell there
//                            (never asked for a flyer)
//   width(), height()        the map in pixels (game +0x19e88 / +0x19e8c)
//   resolve(command, point)  0x4de530: the mission kind for a move (2) or patrol
//                            (9) to the point, 0 for none
//   order(kind, queue, point, a6, a7)   0x4d78a0: queue 0 clears its missions
//                            first; a6 / a7 are mission +0x4e / +0x52
// Without a threat (or unarmed) a member on a mission other than its default one
// re-plans only on rand(50) == 0. The ring is 0 for an armed member under
// threat, else 1; for the Monarch 1 (2 if it cannot fly) under threat, else 3;
// for a builder 2; for a flyer 0. The radius falls back to 320, and under 1/4 HP
// it is max(r/5, 160). Under threat an armed member lists the enemies within
// max(sight, range) + r of the centre that it can hit (live, ready, not cloaked,
// damage > 0, a melee weapon only on the ground) and, unless idle, keeps what it
// is doing (49/50) when its mission targets one of them, its goal point or it
// itself is within max(sight, range) of one. Then two points: the first, with
// candidates, from 9 tries at rand(range) and 10 at rand(max(sight, range))
// around a random candidate (kept when within max(sight, range) + r of the
// centre), then centre tries up to 20 (30 when idle) in all; the second, and
// the first without candidates, from 10 tries at rand(r/2) + rand(r) around the
// centre (retailAiPatrolPoints); each on the map, placeable (flyers skip it),
// the second >= 100 px from the first. Orders: a move to the centre (a6 = r)
// replacing its missions, a queued move to the first point (a7 = -1) -- a flyer
// already within r of the centre gets the move to the first point alone,
// replacing -- and a queued patrol to the second.
template<class Host>
void classicPlanCombat(Host& h, const ClassicCombatUnit& u, bool threat) {
    const bool idle = !u.mission || (u.missionFlags & 0x2000u);
    const bool armedThreat = threat && u.armed;
    if (!armedThreat && !idle && h.random(50) != 0) return;
    const std::optional<ClassicPoint> found = h.centre();
    if (!found) return;
    ClassicPoint centre = *found;
    unsigned ring = armedThreat ? 0u : 1u;
    if (u.commander) ring = threat ? (u.canFly ? 1u : 2u) : 3u;
    else if (u.builder) ring = 2u;
    else if (u.canFly) ring = 0u;
    int32_t r = h.radius(ring).value_or(320);
    if (uint32_t(int32_t(u.hp)) < (u.maxDamage >> 2)) {
        r /= 5;
        if (r < 160) r = 160;
    }
    centre[1] = u.at[1];
    const int32_t reach = int32_t(u.sight) > u.range ? int32_t(u.sight) : u.range;
    std::vector<ClassicEnemy> candidates;
    if (armedThreat) {
        for (const ClassicEnemy& e : h.enemies(std::pair{centre[0], centre[2]}, detail::wrapAdd(reach, r))) {
            if (!e.live() || (e.flags & ClassicEnemy::kUnready) || e.cloaked) continue;
            if (u.melee && (e.flags & 3u) != 1u) continue;
            if (h.damage(e) > 0) candidates.push_back(e);
        }
        if (candidates.empty() && !idle && h.random(50) != 0) return;
        if (!idle)
            for (const ClassicEnemy& e : candidates) {
                bool near = false;
                if ((u.missionFlags & 0x200u) && u.missionTarget == e.id) near = true;
                else if ((u.missionFlags & 0x400u) && detail::classicPlanarPixels(u.goalX, u.goalZ, e.x, e.z) <= reach)
                    near = true;
                else if (detail::classicPlanarPixels(u.at[0], u.at[2], e.x, e.z) <= reach) near = true;
                if (!near) continue;
                if (h.random(50) != 0) return;
                break;
            }
    }

    std::array<ClassicPoint, 2> points{};
    if (candidates.empty()) {
        const auto found2 = sim::retailAiPatrolPoints(centre[0], centre[2], int(r), h.width(), h.height(),
            [&](int n) { return h.random(n); },
            [&](int32_t x, int32_t z) { return u.canFly || h.placeable(x, z); });
        if (!found2) return;
        for (size_t k = 0; k < 2; ++k) points[k] = {(*found2)[k].first, u.at[1], (*found2)[k].second};
    } else {
        for (unsigned pi = 0; pi < 2; ++pi) {
            const int tries = pi == 0 ? (idle ? 30 : 20) : 10;
            bool accepted = false;
            for (int attempt = 1; attempt <= tries && !accepted; ++attempt) {
                ClassicPoint p{};
                if (pi == 0 && attempt < 20) {
                    const int32_t d = int32_t(uint32_t(h.random(attempt < 10 ? u.range : reach)) << 16);
                    const auto [ox, oz] = detail::classicOffset(uint16_t(h.random(65536)), d);
                    const ClassicEnemy& e = candidates[size_t(h.random(int(candidates.size())))];
                    p = {detail::wrapAdd(e.x, ox), e.y, detail::wrapAdd(e.z, oz)};
                    const double dx = double(std::bit_cast<int32_t>(uint32_t(p[0]) - uint32_t(centre[0])));
                    const double dy = double(std::bit_cast<int32_t>(uint32_t(p[1]) - uint32_t(centre[1])));
                    const double dz = double(std::bit_cast<int32_t>(uint32_t(p[2]) - uint32_t(centre[2])));
                    if (int32_t(detail::classicPixels(dz * dz + dy * dy + dx * dx)) > detail::wrapAdd(reach, r)) continue;
                } else {
                    const uint32_t inner = uint32_t(h.random(r / 2));
                    const int32_t d = int32_t((inner + uint32_t(h.random(r))) << 16);
                    const auto [ox, oz] = detail::classicOffset(uint16_t(h.random(65536)), d);
                    p = {detail::wrapAdd(centre[0], ox), centre[1], detail::wrapAdd(centre[2], oz)};
                }
                if (p[0] < 0 || p[0] >= int32_t(uint32_t(h.width()) << 16) ||
                    p[2] < 0 || p[2] >= int32_t(uint32_t(h.height()) << 16)) continue;
                if (!u.canFly && !h.placeable(p[0], p[2])) continue;
                if (pi == 1 && detail::classicHighSquares(points[0][0], points[0][2], p[0], p[2]) < 10000) continue;
                points[pi] = p;
                accepted = true;
            }
            if (!accepted) return;
        }
    }

    const uint8_t toCentre = h.resolve(kClassicMove, centre);
    const uint8_t toFirst = h.resolve(kClassicMove, points[0]);
    if (!toCentre || !toFirst) return;
    if (u.canFly && detail::classicPlanarPixels(u.at[0], u.at[2], centre[0], centre[2]) <= r) {
        h.order(toFirst, false, points[0], 0, -1);
    } else {
        h.order(toCentre, false, centre, r, 0);
        h.order(toFirst, true, points[0], 0, -1);
    }
    if (const uint8_t patrol = h.resolve(kClassicPatrol, points[1])) h.order(patrol, true, points[1], 0, 0);
}

// 0x40d010(unit), the Monarch's retreat, with the same host. Twenty points at
// rand(R0) pixels and a random angle around the base centre (R0 the ring-0
// radius, else 320), each kept if placeable (a flyer keeps all; there is no map
// bounds test of its own); none kept -> nothing. With no enemy within R0 of the
// centre the first kept point wins, else the kept point whose nearest listed
// enemy is farthest (strictly more than the best so far, which starts at 0, so
// with every kept point on top of an enemy point 0 wins even unkept). It moves
// there replacing its missions (a6 = 16); a flyer then queues a patrol leg
// (rand(5) + 4) x 16 pixels away.
template<class Host>
void classicRetreat(Host& h, const ClassicCombatUnit& u) {
    const std::optional<ClassicPoint> found = h.centre();
    if (!found) return;
    ClassicPoint centre = *found;
    const int32_t r = h.radius(0u).value_or(320);
    centre[1] = u.at[1];
    std::array<ClassicPoint, 20> points{};
    std::array<bool, 20> valid{};
    bool any = false;
    for (size_t i = 0; i < points.size(); ++i) {
        const int32_t d = int32_t(uint32_t(h.random(r)) << 16);
        const auto [ox, oz] = detail::classicOffset(uint16_t(h.random(65536)), d);
        points[i] = {detail::wrapAdd(centre[0], ox), u.at[1], detail::wrapAdd(centre[2], oz)};
        valid[i] = u.canFly || h.placeable(points[i][0], points[i][2]);
        any = any || valid[i];
    }
    if (!any) return;
    const std::vector<ClassicEnemy> near = h.enemies(std::pair{centre[0], centre[2]}, r);
    size_t best = 0;
    if (near.empty()) {
        while (!valid[best]) ++best;
    } else {
        int32_t bestScore = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            if (!valid[i]) continue;
            int32_t nearest = 0x7fffffff;
            for (const ClassicEnemy& e : near)
                nearest = std::min(nearest, detail::classicHighSquares(points[i][0], points[i][2], e.x, e.z));
            if (nearest > bestScore) { bestScore = nearest; best = i; }
        }
    }
    const ClassicPoint& to = points[best];
    const uint8_t move = h.resolve(kClassicMove, to);
    if (!move) return;
    h.order(move, false, to, 16, 0);
    if (!u.canFly) return;
    const int32_t d = int32_t(uint32_t(h.random(5) + 4) << 20);
    const auto [ox, oz] = detail::classicOffset(uint16_t(h.random(65536)), d);
    const ClassicPoint leg{detail::wrapAdd(to[0], ox), to[1], detail::wrapAdd(to[2], oz)};
    if (const uint8_t patrol = h.resolve(kClassicPatrol, leg)) h.order(patrol, true, leg, 0, 0);
}

}  // namespace tak::ai
