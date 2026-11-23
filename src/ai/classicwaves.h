#pragma once

// The Classic level's waves (docs/classic-ai-plan.md §1.5 steps 3-7, §1.7):
// what retail's base think 0x40b320 does after its member loop (quota growth,
// the surpluses, the transfer and expansion hooks, the launch into a strike's
// backup, a VTOL squad or a naval strike, the wave re-rolls), the member mover
// 0x40b8e0, and the three army squads' thinks: the strike 0x40e180 (with the
// cohesion pass 0x40abd0 and the retreat-base choice 0x40e120), its backup
// 0x40ea30 and the VTOL squad 0x40eb40, plus the omniscient target picker
// 0x40a200 the strike calls.
//
// Pure templates over the brain's squad view (ai/classicsquads.h) and a host for
// the queries and orders, so the same code is what ClassicBrain runs and what
// the emulation oracles check against the retail routines:
// tools/re/check_ai_target_pick.py (0x40a200), check_ai_strike.py (0x40e180,
// 0x40ea30 and 0x40eb40 with the real cohesion, retreat base, firepower, base
// centres, threat test and set-squad under them) and check_ai_launch.py (the
// whole base think 0x40b320 with its member planners stubbed, so its tail runs
// with the real ready counts, member mover, free-squad searches and wave rolls).
//
// Strikes, backups and VTOL squads only ever order moves and patrols (retail's
// command 2 and 9); the fighting is the sim's standing orders and fire-at-will
// sweep (U13). Server-side only, like the rest of the brain.

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ai/classicdefence.h"
#include "ai/classicsquads.h"

namespace tak::ai {

namespace detail {
// 0x535e90, the octagonal distance: the larger magnitude plus a quarter of the
// smaller (arithmetic shift), magnitudes as two's-complement abs.
inline int32_t classicOctagonal(int32_t dx, int32_t dz) {
    const int32_t sx = dx >> 31, sz = dz >> 31;
    const int32_t ax = std::bit_cast<int32_t>(uint32_t(dx ^ sx) - uint32_t(sx));
    const int32_t az = std::bit_cast<int32_t>(uint32_t(dz ^ sz) - uint32_t(sz));
    return ax > az ? detail::wrapAdd(ax, az >> 2) : detail::wrapAdd(az, ax >> 2);
}
// A 16.16 coordinate's whole-pixel word (unit +0x6a / +0x72), sign-extended.
inline int32_t classicWord(int32_t v) { return int32_t(std::bit_cast<int16_t>(uint16_t(uint32_t(v) >> 16))); }
// (2n)/3 as retail computes it (the doubling wraps, the division truncates).
inline int32_t classicTwoThirds(int32_t n) { return std::bit_cast<int32_t>(uint32_t(n) * 2u) / 3; }
// max(1, weapon range / 16) as the 16-bit cell range of a reachability query.
inline int16_t classicCellRange(int32_t range) {
    const int32_t r = range / 16;
    return r < 1 ? int16_t(1) : int16_t(uint16_t(uint32_t(r)));
}
// hp < maxdamage / divisor, the unsigned compare of the sign-extended hp.
inline bool classicBelow(int16_t hp, uint32_t maxDamage, uint32_t divisor) {
    return uint32_t(int32_t(hp)) < maxDamage / divisor;
}
}  // namespace detail

// 0x40a7b0(cat): the base's members in category `cat` that are live, not dying,
// selectable, finished (+0x108 == 0, unordered counts as equal), not held, not
// carried by a non-holder, with hp >= 3/4 of maxdamage (unsigned, the product
// wrapping at 32 bits).
inline int32_t classicReadyCount(const ClassicSquads& s, const std::vector<ClassicSquadUnit>& units,
                                 const detail::ClassicUnitIndex& index, int slot, unsigned category) {
    int32_t n = 0;
    for (int id : s.squads[size_t(slot)].members) {
        const auto* u = index.find(units, id);
        if (!u || !sim::retailAiCategory(u->unitClass(), category)) continue;
        if (!u->live() || !(u->flags & ClassicSquadUnit::kSelectable) || !u->finished || u->held ||
            (u->carried && !u->carrierHolds))
            continue;
        if (uint32_t(int32_t(u->hp)) < (u->maxDamage * 3u) >> 2) continue;
        ++n;
    }
    return n;
}

// 0x40aa50(cat 0) for an army squad (vtable +8 = 0x410200): the integer-pixel
// mean of its live members, in 16.16; none for a squad without one.
inline std::optional<std::pair<int32_t, int32_t>> classicSquadCentroid(
        const ClassicSquads& s, const std::vector<ClassicSquadUnit>& units,
        const detail::ClassicUnitIndex& index, int slot) {
    sim::RetailAiCentroid centroid;
    for (int id : s.squads[size_t(slot)].members)
        if (const auto* u = index.find(units, id); u && sim::retailAiCategory(u->unitClass(), 0)) centroid.add(u->x, u->z);
    return centroid.center();
}

// 0x40e120 (the strike) and 0x40ee50 (the VTOL squad), the same code: the
// nearest occupied base whose firepower (0x40a9b0, all members) is at least
// max(100, the squad's own), else the nearest occupied base, else base 2.
inline int classicRetreatBase(const ClassicSquads& s, const std::vector<ClassicSquadUnit>& units,
                              const detail::ClassicUnitIndex& index, int slot, std::pair<int32_t, int32_t> at) {
    const int32_t own = classicBaseFirepower(s, units, index, slot, 0);
    const int32_t minimum = own < 100 ? 100 : own;
    std::array<sim::RetailAiBaseCandidate, 20> bases;
    for (int b = 1; b <= classic_slot::kLastBase; ++b) {
        auto& c = bases[size_t(b - 1)];
        c.occupied = !s.squads[size_t(b)].members.empty();
        if (!c.occupied) continue;
        c.eligibleCount = classicBaseFirepower(s, units, index, b, 0);
        c.center = classicBaseCentre(s, units, index, b);
    }
    if (const unsigned b = sim::retailNearestAiBase(bases, at.first, at.second, minimum)) return int(b);
    if (const unsigned b = sim::retailNearestAiBase(bases, at.first, at.second, 0)) return int(b);
    return 2;
}

// 0x40abd0(backup, p1), the strike's cohesion pass. An empty strike first takes
// its backup's first member. Then, over the members' whole-pixel positions (no
// liveness test), while the strike has more than one member the one farthest
// from the mean (squared, 32-bit) goes back to the backup when that square is
// >= n x p1; the running sums then drop the position of the member now in its
// vector slot (set-squad swaps the last member in: a retail slip, kept), and a
// set-squad that moves nothing ends the pass. Last, every backup member with a
// square < n x p1 from the mean joins the strike.
inline void classicStrikeRecruit(ClassicSquads& s, std::vector<ClassicSquadUnit>& units, int slot) {
    const int32_t backup = s.squads[size_t(slot)].parameters[0];
    const int32_t cohesion = s.squads[size_t(slot)].parameters[1];
    if (backup < 1 || backup > classic_slot::kMassBackup || backup == slot) return;
    const detail::ClassicUnitIndex index(units);
    auto& strike = s.squads[size_t(slot)].members;
    auto& reserve = s.squads[size_t(backup)].members;
    auto set = [&](int id, int to) {
        if (const auto it = index.at.find(id); it != index.at.end()) classicSetSquad(s, units, it->second, to);
    };
    auto word = [&](int id, bool z) {
        const auto* u = index.find(units, id);
        return u ? detail::classicWord(z ? u->z : u->x) : 0;
    };
    if (strike.empty()) {
        if (reserve.empty()) return;
        set(reserve.front(), slot);
        if (strike.empty()) return;
    }
    int32_t sumX = 0, sumZ = 0;
    for (int id : strike) { sumX = detail::wrapAdd(sumX, word(id, false)); sumZ = detail::wrapAdd(sumZ, word(id, true)); }
    for (;;) {
        if (strike.empty()) return;
        const int32_t n = int32_t(strike.size());
        const int32_t meanX = sumX / n, meanZ = sumZ / n;
        const int32_t limit = std::bit_cast<int32_t>(uint32_t(n) * uint32_t(cohesion));
        auto square = [&](int id) {
            const uint32_t dx = uint32_t(word(id, false)) - uint32_t(meanX), dz = uint32_t(word(id, true)) - uint32_t(meanZ);
            return std::bit_cast<int32_t>(dz * dz + dx * dx);
        };
        if (n != 1) {
            int32_t farthest = 0;
            size_t far = strike.size();
            for (size_t k = 0; k < strike.size(); ++k)
                if (const int32_t d = square(strike[k]); d > farthest) { farthest = d; far = k; }
            if (farthest >= limit && far < strike.size()) {
                const size_t before = strike.size(), backupBefore = reserve.size();
                const int leaving = strike[far];
                set(leaving, backup);
                const int now = far < strike.size() ? strike[far] : leaving;
                sumX = std::bit_cast<int32_t>(uint32_t(sumX) - uint32_t(word(now, false)));
                sumZ = std::bit_cast<int32_t>(uint32_t(sumZ) - uint32_t(word(now, true)));
                if (strike.size() == before || reserve.size() == backupBefore) return;
                continue;
            }
        }
        std::vector<int> join;
        for (int id : reserve) if (square(id) < limit) join.push_back(id);
        for (int id : join) set(id, slot);
        return;
    }
}

// 0x40b8e0(slot, n, cat, clear): up to n random members of the base in
// category `cat` with hp >= 3/4 of maxdamage move to `slot` (a squad, or 0 to
// leave every squad), each drawn by rand(remaining) and, with `clear`, its
// missions cleared first (h.clear).
template<class Host>
void classicMoveMembers(ClassicSquads& s, std::vector<ClassicSquadUnit>& units, int from, int to, int32_t n,
                        unsigned category, bool clear, Host& h) {
    const detail::ClassicUnitIndex index(units);
    std::vector<size_t> pool;
    for (int id : s.squads[size_t(from)].members) {
        const auto it = index.at.find(id);
        if (it == index.at.end()) continue;
        const auto& u = units[it->second];
        if (uint32_t(int32_t(u.hp)) < (u.maxDamage * 3u) >> 2) continue;
        if (sim::retailAiCategory(u.unitClass(), category)) pool.push_back(it->second);
    }
    while (!pool.empty() && n > 0) {
        const size_t k = size_t(h.random(int(pool.size())));
        const size_t i = pool[k];
        pool.erase(pool.begin() + std::ptrdiff_t(k));
        if (clear) h.clear(i);
        classicSetSquad(s, units, i, to);
        --n;
    }
}

// 0x40deb0 (ground, which also zeroes the strike's state and makes it
// inactive), 0x40df20 (naval) and 0x40eaf0 (VTOL): the first empty squad of
// the family, or 0.
inline int classicFreeStrike(ClassicSquads& s) {
    for (int slot = classic_slot::kFirstStrike; slot < classic_slot::kFirstNavalStrike; slot += 2)
        if (s.squads[size_t(slot)].members.empty()) {
            s.squads[size_t(slot)].parameters[5] = 0;
            s.squads[size_t(slot)].setActive(0);
            return slot;
        }
    return 0;
}
inline int classicFreeNavalStrike(const ClassicSquads& s) {
    for (int slot = classic_slot::kFirstNavalStrike; slot < classic_slot::kFirstVtol; slot += 2)
        if (s.squads[size_t(slot)].members.empty()) return slot;
    return 0;
}
inline int classicFreeVtol(const ClassicSquads& s) {
    for (int slot = classic_slot::kFirstVtol; slot <= classic_slot::kLastVtol; ++slot)
        if (s.squads[size_t(slot)].members.empty()) return slot;
    return 0;
}

// The rest of the base think 0x40b320 after its member loop (0x40b4cf to the
// end). `population` is the owner's unit count (0x410e40: the build manager's
// +0x7d, last tick's recount), `enemyPopulation` what the ground wave roll reads
// as E. The host supplies random(n), clear(i) (0x4d6a50(u, 0)), and the
// routines of ai/classicexpand.h this one calls (the brain runs those; the
// launch oracle answers them from its case): builderBase() (0x40b050),
// groundBase() (0x40b140), airBase() (0x40b230) -- a base to send one unit to,
// or 0 -- transfer(base, n, cat, clear) (0x40b9e0) and expand(slot, escort)
// (0x40bc20, whether a new base was founded).
//  - Quota growth: own population / (20 for base 1, else 30) > p1 grows each of
//    p1..p4 by rand(p/2)+1.
//  - Ready counts (0x40a7b0, hp >= 3/4): free builders (category 3, one kept
//    outside base 1), builders over p1, ground / air / naval combat, less the
//    home reserve p2 / p3 / p4 when that wave size is negative; a wave is ready
//    when its surplus reaches |p5| / |p6| / |p7|.
//  - A builder surplus first tries, on rand(2)==0, a transfer to a base short of
//    builders, and otherwise (when any builder is spare) an expansion into the
//    first empty base, the escort's ground then air surplus following it
//    (re-rolling a ready wave). A ground and then an air surplus each go, on
//    rand(10)==0, to a base short of them (re-rolling that wave).
//  - Launch: with a ground or air wave ready, the first free ground strike (its
//    p2 and p3 zeroed): the ground surplus into its backup (p3 += n, p2 +=
//    rand(2n/3), p5 re-rolled), then any air surplus -- when the air wave is
//    ready, on rand(3)!=0 into the first free VTOL squad, writing p3 = n and p2 =
//    rand(2n/3) onto the ground strike (retail's slip, kept), else into the same
//    backup (p3 += n, p2 += rand(2n/3)); a ready air wave re-rolls p6.
//  - A ready naval wave goes into the first free naval strike's backup (p3 = n,
//    p2 = rand(2n/3)) and re-rolls p7.
template<class Host>
void classicBaseLaunch(ClassicSquads& s, std::vector<ClassicSquadUnit>& units, int slot, int32_t population,
                       int32_t enemyPopulation, Host& h) {
    auto& p = s.squads[size_t(slot)].parameters;
    if (population / (slot == classic_slot::kHome ? 20 : 30) > p[1])
        for (int k = 1; k <= 4; ++k) p[size_t(k)] = detail::wrapAdd(p[size_t(k)], int32_t(h.random(p[size_t(k)] / 2)) + 1);
    const detail::ClassicUnitIndex index(units);
    auto ready = [&](unsigned category) { return classicReadyCount(s, units, index, slot, category); };
    const int32_t freeBuilders = ready(3) - 1 + (slot == classic_slot::kHome ? 1 : 0);
    const int32_t builders = ready(2) - p[1];
    const int32_t ground = ready(4) - (p[5] < 0 ? p[2] : 0);
    const int32_t air = ready(5) - (p[6] < 0 ? p[3] : 0);
    const int32_t naval = ready(6) - (p[7] < 0 ? p[4] : 0);
    auto magnitude = [](int32_t v) { return v < 0 ? std::bit_cast<int32_t>(0u - uint32_t(v)) : v; };
    const bool spare = std::max(builders, freeBuilders) > 0;
    bool groundReady = ground >= magnitude(p[5]);
    const bool navalReady = naval >= magnitude(p[7]);
    bool airReady = air >= magnitude(p[6]);
    auto random = [&](int n) { return h.random(n); };
    auto rerollGround = [&] { p[5] = classicGroundWave(p[2], enemyPopulation, random); };
    auto rerollAir = [&] { p[6] = classicAirWave(p[3], random); };

    bool moved = false;
    if (builders > 0 && h.random(2) == 0)
        if (const int to = h.builderBase()) { h.transfer(to, 1, 2, 0); moved = true; }
    if (!moved && spare) {
        const int empty = classicFirstEmptyBase(s);
        const int32_t escort = ground > 0 ? ground : air > 0 ? air : 0;
        if (empty && h.expand(empty, escort)) {
            if (ground > 0) {
                classicMoveMembers(s, units, slot, empty, ground, 4, true, h);
                if (groundReady) { rerollGround(); groundReady = false; }
            }
            if (air > 0) {
                classicMoveMembers(s, units, slot, empty, air, 5, true, h);
                if (airReady) { rerollAir(); airReady = false; }
            }
        }
    }
    if (ground > 0 && h.random(10) == 0)
        if (const int to = h.groundBase()) { h.transfer(to, 1, 4, 0); groundReady = false; rerollGround(); }
    if (air > 0 && h.random(10) == 0)
        if (const int to = h.airBase()) { h.transfer(to, 1, 5, 0); airReady = false; rerollAir(); }
    if (groundReady || airReady) {
        if (const int strike = classicFreeStrike(s)) {
            auto& q = s.squads[size_t(strike)].parameters;
            q[3] = 0; q[2] = 0;
            if (groundReady) {
                classicMoveMembers(s, units, slot, strike + 1, ground, 4, true, h);
                q[3] = detail::wrapAdd(q[3], ground);
                q[2] = detail::wrapAdd(q[2], int32_t(h.random(detail::classicTwoThirds(ground))));
                rerollGround();
            }
            if (air > 0) {
                if (airReady && h.random(3) != 0) {
                    classicMoveMembers(s, units, slot, classicFreeVtol(s), air, 5, true, h);
                    q[3] = air;
                    q[2] = int32_t(h.random(detail::classicTwoThirds(air)));
                } else {
                    classicMoveMembers(s, units, slot, strike + 1, air, 5, true, h);
                    q[3] = detail::wrapAdd(q[3], air);
                    q[2] = detail::wrapAdd(q[2], int32_t(h.random(detail::classicTwoThirds(air))));
                }
                if (airReady) rerollAir();
            }
        }
    }
    if (navalReady)
        if (const int strike = classicFreeNavalStrike(s)) {
            classicMoveMembers(s, units, slot, strike + 1, naval, 6, true, h);
            auto& q = s.squads[size_t(strike)].parameters;
            q[3] = naval;
            q[2] = int32_t(h.random(detail::classicTwoThirds(naval)));
            p[7] = classicNavalWave(p[4], random);
        }
}

// One enemy player as the target picker scans it (0x40a200's player loop: an
// active record of kind 1-3 with an index, not allied to the AI), its units in
// unit-table order, and its population (+0xe8).
struct ClassicPickPlayer {
    uint16_t population = 0;
    std::vector<ClassicEnemy> units;
};

// 0x40a200(centre, member), the strike's target picker. Nothing for a member
// that is unarmed or has no mover. Otherwise it scores every unit of every
// listed player that is live, on the ground (+0x130 & 3 == 1), ready (no
// 0x2000), not cloaked and that the member's current weapon damages (h.damage,
// 0x531e50 > 0): INT_MAX / ((rand(d1/8)+rand(d1/8))/4+1) / ((rand(d2/2)+
// rand(d2/2))/4+1), d1 and d2 the octagonal whole-pixel distances from the
// centre and from the member; then / (rand(10)+rand(10)+1) unless the AI sees it
// (h.visible, 0x4f7210), / (rand(5)+rand(5)+1) unless it is a mana site, /
// (rand(3)+1) if unfinished, / (rand(3)+1) if unarmed, / (rand(q)+1) with q =
// min(20, unit cap / max(20, its owner's population)). A strictly higher score
// is checked last for reachability (h.reachable(member, target, cell range,
// naval), 0x4e1570) when the member has a ground navigator, and wins if it
// passes. Returns the winner's id, or 0.
template<class Host>
int classicPickTarget(Host& h, const ClassicSquadUnit& member, std::pair<int32_t, int32_t> centre) {
    if (!(member.flags & ClassicSquadUnit::kArmed) || !member.mover) return 0;
    int best = 0;
    int32_t bestScore = 0;
    const int32_t cx = detail::classicWord(centre.first), cz = detail::classicWord(centre.second);
    const int32_t mx = detail::classicWord(member.x), mz = detail::classicWord(member.z);
    const int32_t cap = int32_t(uint16_t(h.unitCap()));
    for (const ClassicPickPlayer& player : h.pickPlayers()) {
        const int32_t pop = player.population < 20 ? 20 : int32_t(player.population);
        int32_t q = cap / pop;
        if (q > 20) q = 20;
        for (const ClassicEnemy& t : player.units) {
            if (!t.live() || (t.flags & 3u) != 1u || (t.flags & ClassicEnemy::kUnready) || t.cloaked) continue;
            if (h.damage(member, t) <= 0) continue;
            const int32_t tx = detail::classicWord(t.x), tz = detail::classicWord(t.z);
            const int32_t d1 = detail::classicOctagonal(cx - tx, cz - tz);
            const int32_t d2 = detail::classicOctagonal(mx - tx, mz - tz);
            const int32_t a = d1 / 8;
            const int32_t a1 = int32_t(h.random(a));
            const int32_t a2 = int32_t(h.random(a));
            int32_t score = 0x7fffffff / ((detail::wrapAdd(a1, a2) / 4) + 1);
            const int32_t b = d2 / 2;
            const int32_t b1 = int32_t(h.random(b));
            const int32_t b2 = int32_t(h.random(b));
            score /= (detail::wrapAdd(b1, b2) / 4) + 1;
            if (!h.visible(t)) {
                const int32_t r1 = int32_t(h.random(10));
                score /= r1 + int32_t(h.random(10)) + 1;
            }
            if (!t.manaSite) {
                const int32_t r1 = int32_t(h.random(5));
                score /= r1 + int32_t(h.random(5)) + 1;
            }
            if (t.unfinished) score /= int32_t(h.random(3)) + 1;
            if (!(t.flags & ClassicSquadUnit::kArmed)) score /= int32_t(h.random(3)) + 1;
            score /= int32_t(h.random(q)) + 1;
            if (score <= bestScore) continue;
            if (member.navigator && !h.reachable(member, t, detail::classicCellRange(member.range), member.naval)) continue;
            best = t.id;
            bestScore = score;
        }
    }
    return best;
}

// The move a squad gives (retail's command 2) and a VTOL squad's patrol (9).
constexpr int kClassicSquadMove = 2, kClassicSquadPatrol = 9;

// What the army squads' hosts answer and do, by member index i into `units`:
//   random(n)                     the game RNG
//   pick(centre, i)               0x40a200 with members[i] (the brain runs
//                                 classicPickTarget): a target id or 0
//   target(id)                    0x40ddd0: the unit p4 names if it is in the
//                                 table and live (+0x130 0x1000000), else null
//   threat(centre, r)             0x412000 over the visible-enemy list
//   reachable(member, target, range, naval)   0x4e1570 from the member's cell
//                                 and footprint to range x range cells at the
//                                 target's cell, static grade
//   resolve(command, i, point)    0x4de530: the mission kind, 0 for none
//   order(kind, queue, i, point, a6, a7)      0x4d78a0
//   clear(i)                      0x4d6a50(u, 0)
//   width(), height()             the map in pixels (the VTOL patrol)

// Retail's order to a withdrawn member (the strike's and the VTOL squad's):
// it joins `base`, its missions are cleared, and with a base centre and ring-0
// radius it moves to the centre, ending within that radius (a6).
template<class Host>
void classicSendHome(ClassicSquads& s, std::vector<ClassicSquadUnit>& units, size_t i, int base, Host& h) {
    classicSetSquad(s, units, i, base);
    h.clear(i);
    const detail::ClassicUnitIndex index(units);
    const auto centre = classicBaseCentre(s, units, index, base);
    if (!centre) return;
    const auto radius = classicBaseRadius(s, units, index, base, 0);
    if (!radius) return;
    const ClassicPoint at{centre->first, 0, centre->second};
    if (const uint8_t kind = h.resolve(kClassicSquadMove, i, at)) h.order(kind, false, i, at, *radius, 0);
}

// 0x40e180 after its reschedule, cohesion pass and empty test (the shared
// sim::retailTickAiSquad with classicStrikeRecruit): the strike's think for
// slot `slot`. `clock` is its scheduler clock (a strike without a target sleeps
// rand(900)+150 ticks).
//  - No centroid: nothing. The retreat base is chosen from the centroid.
//  - Dissolve when the strike and its backup together have <= p2 members:
//    each, strike first, has its missions cleared and joins the retreat base;
//    p5 = 0, inactive. (A member set-squad cannot move -- a dying one -- would
//    make retail's loop spin; the port stops there.)
//  - The target p4 is kept while p5 != 0, it is live and not dying, and
//    rand(250) != 0; otherwise a new one is picked for a random member. A new
//    live target unlike the old one is "changed"; none sleeps the strike.
//  - Members that are unarmed or have no mover, below maxdamage/(rand(3)+3),
//    or (with a navigator, when changed or on rand(20)==0) that cannot reach the
//    target within their weapon's cell range, go home (classicSendHome).
//  - Contact when a member is on an attack mission (class 1 or 2) or the
//    visible-enemy list has a unit within 3/2 of R = max(10, the armed members'
//    largest max(sight, range)) of the centroid: p5 = 1, and a move to the
//    target goes to every member when changed, with no mission or one flagged
//    0x10000, and on rand(10)==0 to one already on that move kind; others keep
//    fighting. Otherwise the march, p5 = 2: unless changed, "close" when a
//    member on the move kind has its goal within 50 px (squared < 2500) of the
//    target; then a move to the target goes to every member when changed, when
//    none is on the move kind yet, or without a mission; a 0x10000 one gets it
//    on rand(50)==0; one on another kind is left; one on the move kind gets it
//    always, or on rand(10)==0 when close. Moves replace the missions (queue
//    0, a6 0, a7 -1). Last, active = (p5 == 2).
template<class Host>
void classicStrikeThink(ClassicSquads& s, std::vector<ClassicSquadUnit>& units, int slot,
                        sim::RetailAiGroupClock& clock, uint32_t tick, Host& h) {
    auto& squad = s.squads[size_t(slot)];
    auto& p = squad.parameters;
    const int backup = p[0];
    auto finish = [&] { squad.setActive(p[5] == 2 ? 1 : 0); };
    const detail::ClassicUnitIndex index(units);
    const auto centroid = classicSquadCentroid(s, units, index, slot);
    if (!centroid) return;
    const int home = classicRetreatBase(s, units, index, slot, *centroid);
    const bool backupValid = backup >= 1 && backup <= classic_slot::kMassBackup;
    const int32_t together = int32_t(squad.members.size() + (backupValid ? s.squads[size_t(backup)].members.size() : 0));
    if (together <= p[2]) {
        auto drain = [&](std::vector<int>& members) {
            while (!members.empty()) {
                const size_t before = members.size();
                const auto it = index.at.find(members.front());
                if (it == index.at.end()) return;
                h.clear(it->second);
                classicSetSquad(s, units, it->second, home);
                if (members.size() == before) return;
            }
        };
        drain(squad.members);
        if (backupValid) drain(s.squads[size_t(backup)].members);
        p[5] = 0;
        squad.setActive(0);
        return;
    }
    // p4 holds the target. Retail stores a 16-bit unit slot there; World's unit ids
    // are never reused and pass 65,535 in a long game, so the port keeps the whole
    // id (masked, a later unit's id read as an older unit's: a dead one, or the wrong one).
    auto targetId = [&] { return int(p[4]); };
    bool repick = true;
    if (p[5] != 0)
        if (const ClassicEnemy* t = h.target(targetId()); t && (t->flags & ClassicEnemy::kLive) && !(t->flags & ClassicEnemy::kDying))
            repick = h.random(250) == 0;
    bool changed = false;
    if (repick) {
        const ClassicEnemy* before = h.target(targetId());
        const int old = before ? before->id : 0;
        const size_t k = size_t(h.random(int(squad.members.size())));
        const auto it = index.at.find(squad.members[k]);
        const int picked = it == index.at.end() ? 0 : h.pick(ClassicPoint{centroid->first, 0, centroid->second}, it->second);
        // 0x40de20: a live pick, else none.
        const ClassicEnemy* now = picked ? h.target(picked) : nullptr;
        p[4] = now ? int32_t(now->id) : 0;
        now = h.target(targetId());
        if (now && now->id != old) changed = true;
        else if (!now) clock.deadline = tick + uint32_t(h.random(900)) + 150u;
    }
    const ClassicEnemy* target = h.target(targetId());
    if (!target) { p[5] = 0; finish(); return; }

    std::vector<size_t> leaving;
    for (int id : squad.members) {
        const auto it = index.at.find(id);
        if (it == index.at.end()) continue;
        const size_t i = it->second;
        const auto& u = units[i];
        if (!(u.flags & ClassicSquadUnit::kArmed) || !u.mover) { leaving.push_back(i); continue; }
        const uint32_t divisor = uint32_t(h.random(3)) + 3u;
        if (detail::classicBelow(u.hp, u.maxDamage, divisor)) { leaving.push_back(i); continue; }
        if (!u.navigator) continue;
        if (!changed && h.random(20) != 0) continue;
        if (!h.reachable(u, *target, detail::classicCellRange(u.range), false)) leaving.push_back(i);
    }
    for (const size_t i : leaving) classicSendHome(s, units, i, home, h);
    if (squad.members.empty()) { p[5] = 0; squad.setActive(0); return; }

    target = h.target(targetId());
    const ClassicPoint at{target->x, target->y, target->z};
    int32_t reach = 10;
    bool contact = false;
    for (int id : squad.members) {
        const auto* u = index.find(units, id);
        if (!u) continue;
        if (u->flags & ClassicSquadUnit::kArmed) {
            const int32_t sight = int32_t(u->sight);
            const int32_t m = sight > u->range ? sight : u->range;
            if (m > reach) reach = m;
        }
        if (u->mission && (u->missionClass == 1 || u->missionClass == 2)) { contact = true; break; }
    }
    if (!contact) contact = h.threat(*centroid, std::bit_cast<int32_t>(uint32_t(reach) * 3u) / 2);
    auto order = [&](uint8_t kind, size_t i) { h.order(kind, false, i, at, 0, -1); };
    // The members are walked by position; an order never changes membership.
    const std::vector<int> members = squad.members;
    if (contact) {
        p[5] = 1;
        for (int id : members) {
            const size_t i = index.at.at(id);
            const uint8_t kind = h.resolve(kClassicSquadMove, i, at);
            if (!kind) continue;
            const auto& u = units[i];
            if (!changed && u.mission && !(u.missionFlags & 0x10000u)) {
                if (u.missionKind != kind) continue;
                if (h.random(10) != 0) continue;
            }
            order(kind, i);
        }
        finish();
        return;
    }
    p[5] = 2;
    bool anyOnMove = false, close = false;
    if (!changed)
        for (int id : members) {
            const size_t i = index.at.at(id);
            const uint8_t kind = h.resolve(kClassicSquadMove, i, at);
            const auto& u = units[i];
            if (!kind || !u.mission || u.missionKind != kind) continue;
            anyOnMove = true;
            if ((u.missionFlags & 0x400u) && detail::classicHighSquares(u.goalX, u.goalZ, at[0], at[2]) < 2500) {
                close = true;
                break;
            }
        }
    for (int id : members) {
        const size_t i = index.at.at(id);
        const uint8_t kind = h.resolve(kClassicSquadMove, i, at);
        if (!kind) continue;
        const auto& u = units[i];
        if (!changed && anyOnMove && u.mission) {
            const bool forced = (u.missionFlags & 0x10000u) && h.random(50) == 0;
            if (!forced) {
                if (u.missionKind != kind) continue;
                if (close && h.random(10) != 0) continue;
            }
        }
        order(kind, i);
    }
    finish();
}

// 0x40ea30 after its reschedule (+150): with members and a strike (p0) that has
// members and a centroid, every live, not dying unit of the player in this
// squad, in unit order (0x50b5d0), gets a move to the strike's centroid
// (queue 0, a6 0, a7 -1).
template<class Host>
void classicBackupThink(ClassicSquads& s, std::vector<ClassicSquadUnit>& units, int slot, Host& h) {
    const int strike = s.squads[size_t(slot)].parameters[0];
    if (s.squads[size_t(slot)].members.empty()) return;
    if (strike < 1 || strike > classic_slot::kMassBackup || s.squads[size_t(strike)].members.empty()) return;
    const detail::ClassicUnitIndex index(units);
    const auto centroid = classicSquadCentroid(s, units, index, strike);
    if (!centroid) return;
    const ClassicPoint at{centroid->first, 0, centroid->second};
    for (size_t i = 0; i < units.size(); ++i) {
        if (!units[i].live() || units[i].squad != slot) continue;
        if (const uint8_t kind = h.resolve(kClassicSquadMove, i, at)) h.order(kind, false, i, at, 0, -1);
    }
}

// 0x40eb40 after its reschedule and empty test: a VTOL squad's think. No
// centroid: nothing. Members below maxdamage/(rand(3)+3) go home to the
// retreat base (0x40ee50); the others make the squad busy when their first
// mission is not the default one (no 0x2000). Then empty -> inactive, else
// active; busy keeps its task on rand(30)!=0. Otherwise every live unit of the
// squad, in unit order, patrols (queue 0) to one random point: on rand(2)==0
// anywhere on the map, else on rand(2)==0 a random x on the top (rand(2)!=0)
// or bottom edge, else a random z on the left (rand(2)!=0) or right edge (the
// far edges are the last whole pixel).
template<class Host>
void classicVtolThink(ClassicSquads& s, std::vector<ClassicSquadUnit>& units, int slot, Host& h) {
    auto& squad = s.squads[size_t(slot)];
    const detail::ClassicUnitIndex index(units);
    const auto centroid = classicSquadCentroid(s, units, index, slot);
    if (!centroid) return;
    const int home = classicRetreatBase(s, units, index, slot, *centroid);
    bool busy = false;
    std::vector<size_t> leaving;
    for (int id : squad.members) {
        const auto it = index.at.find(id);
        if (it == index.at.end()) continue;
        const auto& u = units[it->second];
        const uint32_t divisor = uint32_t(h.random(3)) + 3u;
        if (detail::classicBelow(u.hp, u.maxDamage, divisor)) leaving.push_back(it->second);
        else if (u.mission && !(u.missionFlags & 0x2000u)) busy = true;
    }
    for (const size_t i : leaving) classicSendHome(s, units, i, home, h);
    if (squad.members.empty()) { squad.setActive(0); return; }
    squad.setActive(1);
    if (busy && h.random(30) != 0) return;
    const int32_t w = h.width(), hgt = h.height();
    auto pixels = [](int32_t v) { return int32_t(uint32_t(v) << 16); };
    int32_t x = 0, z = 0;
    if (h.random(2) == 0) {
        x = pixels(int32_t(h.random(w)));
        z = pixels(int32_t(h.random(hgt)));
    } else if (h.random(2) == 0) {
        x = pixels(int32_t(h.random(w)));
        z = h.random(2) != 0 ? 0 : pixels(hgt + 0xffff);
    } else {
        x = h.random(2) != 0 ? 0 : pixels(w + 0xffff);
        z = pixels(int32_t(h.random(hgt)));
    }
    const ClassicPoint at{x, 0, z};
    for (size_t i = 0; i < units.size(); ++i) {
        if (!units[i].live() || units[i].squad != slot) continue;
        if (const uint8_t kind = h.resolve(kClassicSquadPatrol, i, at)) h.order(kind, false, i, at, 0, 0);
    }
}

}  // namespace tak::ai
