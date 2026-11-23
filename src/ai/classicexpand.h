#pragma once

// The Classic level's expansion and transfers (docs/classic-ai-plan.md §1.5
// step 5-6, §1.9): what retail's base think 0x40b320 calls after its ready
// counts -- the three transfer-target searches 0x40b050 (a base short of
// builders), 0x40b140 (short of ground units) and 0x40b230 (short of air
// units), the transfer 0x40b9e0 and the expansion 0x40bc20 into a new base
// anchored on a sacred site -- and the zone labels both read: at map load
// 0x4e0a20 labels each start location and sacred site of every navigator grid
// by which of them its reachability walk connects (0x4e1570, static grade).
//
// Pure templates over the brain's squad view (ai/classicsquads.h) and a host,
// so the same code is what ClassicBrain runs and what the emulation oracles
// check against the retail routines: tools/re/check_ai_zone_labels.py
// (0x4e0a20's labelling with the real reachability walk under it),
// check_ai_transfer.py (0x40b050 / 0x40b140 / 0x40b230 and 0x40b9e0 with the
// real ready count, centres, categories and set-squad) and check_ai_expand.py
// (0x40bc20 with the real categories, centres, ring radius, enemy query,
// set-squad and anchor setter). Server-side only, like the rest of the brain.

#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ai/classicdefence.h"
#include "ai/classicplacement.h"
#include "ai/classicsquads.h"
#include "ai/classicwaves.h"

namespace tak::ai {

// One navigator grid's zone labels (grid +0x1c start records, +0x2c sacred-site
// records): -1 for an empty record, else the number of the zone it was put in.
struct ClassicZoneLabels {
    std::vector<int32_t> starts, sites;
};

// 0x4e0a20's labelling (to 0x4e0fbf). Every record gets 0 (-1 when empty), then
// while one is left at 0 -- the start records first, then the sacred sites, in
// table order -- it seeds a new zone, and every record still at 0 that the
// reachability walk connects to it joins that zone: the starts, then the sites.
// A start counts as its cell less 6, 13 x 13 cells; a site as its cell less 6
// and its feature's footprint plus 12 (`footprint(cell)`: the sacred feature's
// footX / footZ at that cell, else 1 x 1). `reach(sx, sz, sw, sh, tx, tz, tw,
// th)` is 0x4e1570(source, target, mode 1, unbounded) over the grid. The walk is
// not symmetric, so who seeds matters; the labels follow retail's order.
template<class Footprint, class Reach>
ClassicZoneLabels classicZoneLabels(const std::vector<std::optional<ClassicCell>>& starts,
                                    const std::vector<std::optional<ClassicCell>>& sites,
                                    Footprint footprint, Reach reach) {
    ClassicZoneLabels z;
    z.starts.resize(starts.size());
    z.sites.resize(sites.size());
    for (size_t k = 0; k < starts.size(); ++k) z.starts[k] = starts[k] ? 0 : -1;
    for (size_t k = 0; k < sites.size(); ++k) z.sites[k] = sites[k] ? 0 : -1;
    struct Rect { int16_t x, z, w, h; };
    auto words = [](int v) { return int16_t(uint16_t(uint32_t(v))); };
    auto startRect = [&](const ClassicCell& c) {
        return Rect{words(c.first - 6), words(c.second - 6), 13, 13};
    };
    auto siteRect = [&](const ClassicCell& c) {
        const std::pair<int16_t, int16_t> f = footprint(c);
        return Rect{words(c.first - 6), words(c.second - 6), words(f.first + 12), words(f.second + 12)};
    };
    int32_t zone = 0;
    for (;;) {
        std::optional<Rect> seed;
        for (size_t k = 0; k < starts.size() && !seed; ++k)
            if (starts[k] && z.starts[k] == 0) { z.starts[k] = ++zone; seed = startRect(*starts[k]); }
        for (size_t k = 0; k < sites.size() && !seed; ++k)
            if (sites[k] && z.sites[k] == 0) { z.sites[k] = ++zone; seed = siteRect(*sites[k]); }
        if (!seed) return z;
        auto joins = [&](const Rect& t) { return reach(seed->x, seed->z, seed->w, seed->h, t.x, t.z, t.w, t.h); };
        for (size_t k = 0; k < starts.size(); ++k)
            if (starts[k] && z.starts[k] == 0 && joins(startRect(*starts[k]))) z.starts[k] = zone;
        for (size_t k = 0; k < sites.size(); ++k)
            if (sites[k] && z.sites[k] == 0 && joins(siteRect(*sites[k]))) z.sites[k] = zone;
    }
}

// The sacred feature a map cell holds, as 0x4e0a20 resolves it for a site's
// footprint (the placers' reading, classicSacredCell, giving the index): the
// cell's own type if it is below the type count and sacred, or a footprint
// tail's anchor type (no count check on that path) if sacred; else none.
template<class CellAt, class Sacred>
std::optional<uint16_t> classicSacredFeature(int x, int z, uint32_t featureTypes, CellAt cellAt, Sacred sacred) {
    const ClassicFeatureCell c = cellAt(x, z);
    if (c.feature < 0xfffa) {
        if (c.feature < featureTypes && sacred(c.feature)) return c.feature;
        return std::nullopt;
    }
    if (c.feature != 0xfffe) return std::nullopt;
    const ClassicFeatureCell anchor = cellAt(x - c.backX, z - c.backZ);
    if (anchor.feature < 0xfffa && sacred(anchor.feature)) return anchor.feature;
    return std::nullopt;
}

namespace detail {
// A base anchor's zone label on a grid: p0 = k+1 is start record k, p0 = -(k+1)
// sacred-site record k. Retail indexes the vector unchecked; a record past its
// end (a stale anchor) reads here as a label no record has.
inline int32_t classicAnchorLabel(const ClassicZoneLabels& z, int32_t p0) {
    const bool site = p0 < 0;
    const int64_t k = site ? -int64_t(p0) - 1 : int64_t(p0) - 1;
    const auto& v = site ? z.sites : z.starts;
    return k >= 0 && k < int64_t(v.size()) ? v[size_t(k)] : int32_t(0x80000000u);
}
// A navigator's labels match the tables only when it has one per record.
inline bool classicLabelsFit(const ClassicSquads& s, const ClassicZoneLabels& z) {
    return z.sites.size() == s.sites.size() && z.starts.size() == s.starts.size();
}
// trunc(sqrt(dx^2 + dz^2)) of whole-pixel word differences, as _ftol leaves it.
inline int32_t classicWholeDistance(int32_t ax, int32_t az, int32_t bx, int32_t bz) {
    const double dx = double(classicWord(ax) - classicWord(bx));
    const double dz = double(classicWord(az) - classicWord(bz));
    return int32_t(uint32_t(uint64_t(int64_t(std::sqrt(dx * dx + dz * dz)))));
}
}  // namespace detail

// 0x40b050 (category 2 against p1), 0x40b140 (4 against p2) and 0x40b230 (5
// against p3): of the other occupied bases 1..20 whose ready count (0x40a7b0)
// in `category` is below their own quota p[quota] and that have a centre, the
// one whose centre is nearest this base's (the high dwords of the 64-bit
// squares of the 16.16 differences, summed; the first on ties), or 0. Nothing
// without a centre of its own.
inline int classicShortBase(const ClassicSquads& s, const std::vector<ClassicSquadUnit>& units, int slot,
                            unsigned category, int quota) {
    const detail::ClassicUnitIndex index(units);
    const auto own = classicBaseCentre(s, units, index, slot);
    if (!own) return 0;
    int best = 0;
    int32_t bestSquares = 0;
    for (int b = 1; b <= classic_slot::kLastBase; ++b) {
        if (b == slot || s.squads[size_t(b)].members.empty()) continue;
        if (classicReadyCount(s, units, index, b, category) >= s.squads[size_t(b)].parameters[size_t(quota)]) continue;
        const auto centre = classicBaseCentre(s, units, index, b);
        if (!centre) continue;
        const int32_t squares = detail::classicHighSquares(centre->first, centre->second, own->first, own->second);
        if (best == 0 || squares < bestSquares) { best = b; bestSquares = squares; }
    }
    return best;
}

// What the transfer's and the expansion's hosts answer, by unit index i:
//   random(n)          the game RNG
//   clear(i)           0x4d6a50(u, 0)
//   labels(i)          the zone labels of the unit's navigator grid (unit +8 ->
//                      +4), or null for a unit without one (a flyer)
//   enemies(point, r)  how many units 0x411ae0 lists within r pixels of the
//                      point (the expansion's escort test)

// 0x40b9e0(to, n, category, clear): with both this base and `to` anchored, up
// to n members of this base in `category` with hp >= 3/4 of maxdamage, each
// drawn by rand(remaining), go to `to` (missions cleared first with `clear`) --
// a flyer always, a unit with a navigator only when its grid gives both anchors
// the same zone. A drawn unit whose zones differ still uses up one of the n; a
// grid without a label per record ends the transfer.
template<class Host>
void classicTransfer(ClassicSquads& s, std::vector<ClassicSquadUnit>& units, int slot, int to, int32_t n,
                     unsigned category, bool clear, Host& h) {
    if (s.squads[size_t(slot)].parameters[0] == 0) return;
    const int32_t target = s.squads[size_t(to)].parameters[0];
    if (target == 0) return;
    const detail::ClassicUnitIndex index(units);
    std::vector<size_t> pool;
    for (int id : s.squads[size_t(slot)].members) {
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
        if (const ClassicZoneLabels* z = h.labels(i)) {
            if (!detail::classicLabelsFit(s, *z)) return;
            if (detail::classicAnchorLabel(*z, target) !=
                detail::classicAnchorLabel(*z, s.squads[size_t(slot)].parameters[0])) {
                --n;
                continue;
            }
        }
        if (clear) h.clear(i);
        classicSetSquad(s, units, i, to);
        --n;
    }
}

// 0x40bc20(newSlot, escort), the expansion. A random member of this base that is
// live, not dying, selectable, finished, not held or carried by a non-holder,
// with hp >= 3/4 of maxdamage and a free builder (category 3) -- or, with none,
// a builder (category 2) -- is the founder. Its navigator grid gives the zone of
// this base's anchor (a flyer: any zone; an unanchored base, or a grid without a
// label per record: no expansion). Of the sacred-site records still holding a
// site and in that zone, the nearest to this base's centre (whole pixels, the
// last on ties) that no occupied base is anchored on and that lies outside every
// occupied base's ring-2 radius is chosen. It is refused when three times the
// enemies 0x411ae0 lists within 50 px of the site's cell centre exceed the
// escort. Otherwise the founder joins `newSlot` (no order: its own next think
// builds around the site) and the new base is anchored on the site.
template<class Host>
bool classicExpand(ClassicSquads& s, std::vector<ClassicSquadUnit>& units, int slot, int newSlot, int32_t escort,
                   Host& h) {
    const detail::ClassicUnitIndex index(units);
    std::vector<size_t> pool;
    auto gather = [&](unsigned category) {
        for (int id : s.squads[size_t(slot)].members) {
            const auto it = index.at.find(id);
            if (it == index.at.end()) continue;
            const auto& u = units[it->second];
            if (!(u.flags & ClassicSquadUnit::kLive) || (u.flags & ClassicSquadUnit::kDying) ||
                !(u.flags & ClassicSquadUnit::kSelectable) || !u.finished || u.held || (u.carried && !u.carrierHolds))
                continue;
            if (uint32_t(int32_t(u.hp)) < (u.maxDamage * 3u) >> 2) continue;
            if (sim::retailAiCategory(u.unitClass(), category)) pool.push_back(it->second);
        }
    };
    gather(3);
    if (pool.empty()) gather(2);
    if (pool.empty()) return false;
    const size_t founder = pool[size_t(h.random(int(pool.size())))];
    constexpr int32_t kAnyZone = -2;
    int32_t zone = kAnyZone;
    const ClassicZoneLabels* labels = h.labels(founder);
    if (labels) {
        const int32_t anchor = s.squads[size_t(slot)].parameters[0];
        if (anchor == 0 || !detail::classicLabelsFit(s, *labels)) return false;
        zone = detail::classicAnchorLabel(*labels, anchor);
    }
    const auto centre = classicBaseCentre(s, units, index, slot);
    if (!centre) return false;
    int best = -1;
    int32_t bestDistance = 0;
    std::pair<int32_t, int32_t> at{};
    for (size_t k = 0; k < s.sites.size(); ++k) {
        if (!s.sites[k]) continue;
        if (zone != kAnyZone && zone != labels->sites[k]) continue;
        const int32_t sx = int32_t(uint32_t(int32_t(s.sites[k]->first)) << 20) + 0x80000;
        const int32_t sz = int32_t(uint32_t(int32_t(s.sites[k]->second)) << 20) + 0x80000;
        const int32_t distance = detail::classicWholeDistance(centre->first, centre->second, sx, sz);
        if (best >= 0 && distance > bestDistance) continue;
        const int32_t anchor = -int32_t(k) - 1;
        bool free = true;
        for (int b = 1; b <= classic_slot::kLastBase && free; ++b) {
            if (s.squads[size_t(b)].members.empty()) continue;
            if (s.squads[size_t(b)].parameters[0] == anchor) { free = false; break; }
            const auto other = classicBaseCentre(s, units, index, b);
            if (!other) continue;
            const auto radius = classicBaseRadius(s, units, index, b, 2);
            if (!radius) continue;
            if (detail::classicWholeDistance(other->first, other->second, sx, sz) <= *radius) free = false;
        }
        if (!free) continue;
        best = int(k);
        bestDistance = distance;
        at = {sx, sz};
    }
    if (best < 0) return false;
    // 0x40cfa0(1): five times ring 1's scale, 50 px.
    const uint32_t near = uint32_t(h.enemies(at, 50));
    if (near * 3u > uint32_t(escort)) return false;
    classicSetSquad(s, units, founder, newSlot);
    // 0x40c6e0: the anchor, when the record is there.
    if (size_t(best) < s.sites.size() && s.sites[size_t(best)]) s.squads[size_t(newSlot)].parameters[0] = -best - 1;
    return true;
}

}  // namespace tak::ai
