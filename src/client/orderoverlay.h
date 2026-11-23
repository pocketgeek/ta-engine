#pragma once
// Retail's order-overlay colours and its Shift-only range circles round the
// ORDERING unit (icd 0x4d6530).
//
// Retail's order overlay runs only while Shift is held (0x4fcc46 polls
// GetAsyncKeyState(VK_SHIFT) and calls 0x5210a0), walks the local player's own
// units and hands each to 0x4d6340 with an overlay mask: 0x3f for a selected unit
// (+0x130 & 0x10) and for the unit under the cursor, at most 0x21 for every other
// own unit. 0x4d6340 runs over the unit's mission list, and the FIRST mission whose
// kind descriptor carries flag 0x10 (every kind but park, load, unload, area
// reclaim and facing) draws 0x4d6530 once. So these circles show round selected
// and hovered units only. 0x4d6530 draws, with 0x4d5c40, up to three:
//
//  - mincloakdistance (UnitDef+0x22a, parsed at 0x4c01e9), WHITE (colour -1),
//    only while the unit is cloaked (+0x114 & 4) and the distance is not 0;
//  - for a `kamikaze` type (UnitDef+0x260 bit 0x10000000, 0x4c0856) that has an
//    [EXPLODEAS] weapon (UnitDef+0x1b6, 0x4c16e3), two RED (0xffff0000) circles:
//    a pulse that grows from 8 to half the blast's areaofeffect (weapon+0x8a,
//    0x530f48) over the first 30 ticks of every 60 and holds there for the rest,
//    then a fixed one at kamikazedistance (UnitDef+0x236, 0x4c0876) for a unit
//    with a mover (+8: a mobile unit) or sightdistance (UnitDef+0x226) for one
//    without.
//
// 0x4d5c40 is a ring of line segments, one per 8 world units of circumference:
// n = (int)(r * 2pi * 0.125); point k sits at angle k * (0x10000 / n) (16-bit
// angle units) on the 16.16 sin/cos table. Each point is lifted to the HIGHER of
// the unit's own height and the terrain under the point (0x511170) and drawn at
// screen y = z - height/2; a segment is drawn when either end is on screen. No
// label is passed, so none is drawn.
#include <cstdint>
#include <vector>
#include <cmath>

namespace tak::client {

// 0x4d5c40's segment count: fild r, fmul 2pi, fmul 0.125, _ftol (truncate).
inline int retailCircleSegments(int radius) {
    const double n = double(radius) * 6.28318530717958 * 0.125;
    return n > 0 ? int(n) : 0;
}

// Offset of ring point k from the centre, in world units: angle k*(0x10000/n),
// x += sin*r, z += cos*r (0x5360bf/0x5360f3, negated twice). Retail reads a
// 512-entry table at entry ((angle + 0x20) >> 7) & 0x1ff (cos: angle + 0x4000);
// the same entry is taken here, and float trig stands in for the Q13 values.
struct CircleOffset { float dx, dz; };
inline CircleOffset retailCirclePoint(int radius, int segments, int k) {
    const int step = segments > 0 ? 0x10000 / segments : 0;
    const uint32_t angle = uint32_t(k) * uint32_t(step) & 0xffffu;
    const auto entry = [](uint32_t a) { return double(((a + 0x20u) >> 7) & 0x1ffu) * (6.28318530717958 / 512.0); };
    return {float(std::sin(entry(angle)) * radius), float(std::sin(entry(angle + 0x4000u)) * radius)};
}

// The kamikaze pulse radius at game tick `tick` (0x4d658c..0x4d65c9):
// half = aoe/2; r = (tick % 60) * half * 2 / 60, raised to 8, capped at half.
inline int kamikazePulseRadius(uint32_t tick, int areaOfEffect) {
    const uint32_t half = uint32_t(uint16_t(areaOfEffect)) >> 1;
    uint32_t r = (tick % 60u) * half * 2u / 60u;
    if (r < 8u) r = 8u;
    if (int32_t(r) >= int32_t(half)) r = half;
    return int(r);
}

struct RangeCircle {
    int radius = 0;
    uint32_t argb = 0;   // 0xAARRGGBB, as retail passes it
};

// What 0x4d6530 draws for one unit, in its order. `mobile` is retail's mover
// test (+8, made at birth only for a mobile unit).
struct RangeCircleInput {
    int minCloakDistance = 0;   // UnitDef+0x22a (signed word)
    bool cloaked = false;       // unit+0x114 & 4
    bool kamikaze = false;      // UnitDef+0x260 & 0x10000000
    bool hasExplodeAs = false;  // UnitDef+0x1b6 != 0
    int explodeAoe = 0;         // [EXPLODEAS] areaofeffect (weapon+0x8a)
    int kamikazeDistance = 0;   // UnitDef+0x236 (unsigned word)
    int sightDistance = 0;      // UnitDef+0x226 (signed word)
    bool mobile = false;
};
inline std::vector<RangeCircle> retailRangeCircles(const RangeCircleInput& in, uint32_t tick) {
    std::vector<RangeCircle> out;
    const int16_t cloak = int16_t(in.minCloakDistance);
    if (cloak != 0 && in.cloaked) out.push_back({cloak, 0xffffffffu});
    if (in.kamikaze && in.hasExplodeAs) {
        out.push_back({kamikazePulseRadius(tick, in.explodeAoe), 0xffff0000u});
        out.push_back({in.mobile ? int(uint16_t(in.kamikazeDistance)) : int(int16_t(in.sightDistance)),
                       0xffff0000u});
    }
    return out;
}

// Area-order rectangles (0x4d5180, overlay flag 0x20): white, or yellow
// (0xffffff00) when the ordering unit is selected (+0x130 & 0x10).
constexpr uint32_t retailAreaOrderArgb(bool selected) { return selected ? 0xffffff00u : 0xffffffffu; }

// 0x4d6340 calls 0x4d6530 only off a mission whose kind has flag 0x10. Retail
// always holds at least a standby mission (flag 0x10); the kinds without it are
// park, load, unload, area reclaim (0x22) and facing. Here an empty queue is the
// standby, and `noFlag(i)` says order i is one of the flagless kinds.
template <class NoFlag>
inline bool missionsShowRangeCircles(size_t orders, NoFlag noFlag) {
    if (orders == 0) return true;
    for (size_t i = 0; i < orders; ++i)
        if (!noFlag(i)) return true;
    return false;
}

}  // namespace tak::client
