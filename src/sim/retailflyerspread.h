#pragma once
#include <algorithm>
#include <cstdint>

namespace tak::sim {

// Retail's group spread for flyers (the order code 0x5214b0). A move or patrol
// whose mission descriptor carries flag 0x2 at +0x11 -- VTOL_Move,
// VTOL_Move_Formation(Standby) and VTOL_Patrol; never Move_Ground or ground
// Patrol -- gives each selected flyer the click plus its offset from the
// selection's centre, unclamped, when the offset's squared length is at most
// 3000 n px^2 (n = the units counted in the centre, so the reach is
// sqrt(3000 n): 55 px for one, 173 for ten, 548 for a hundred). A flyer
// farther out takes the click itself. So a big air group keeps its shape over
// the target and only stragglers converge on the point.
//
// The centre is the truncated integer mean of the units' whole-pixel positions
// (the high words of their 16.16 coordinates, idiv); the offset keeps the
// unit's sub-pixel part (position - centre << 16), and its length is tested per
// axis in whole squared pixels ((o * o) >> 32). Everything is integer 16.16, so
// the client (from its frame's floats via Fixed::fromFloat) and the sim (an
// explicit order's OrderShape::Offsets / FlyerOffsets) compute the same goals.
//
// Ground units keep OUR rule (gameview_hud.cpp: +-60 px per axis on a
// right-click, the point itself on an armed move or patrol; Legion shares the
// point for the surface movers it plans): retail sends every ground unit to the
// click, a deliberate difference.
struct FlyerGroupCentre {
    int64_t sx = 0, sz = 0;
    int64_t n = 0;
    void add(int32_t xRaw, int32_t zRaw) { sx += xRaw >> 16; sz += zRaw >> 16; ++n; }
    // The raw 16.16 offset a flyer at (xRaw, zRaw) keeps from the click, or
    // false when it is out of reach (or there is no centre): it takes the click.
    bool offset(int32_t xRaw, int32_t zRaw, int64_t& ox, int64_t& oz) const {
        ox = oz = 0;
        if (n <= 0) return false;
        const int64_t dx = int64_t(xRaw) - (sx / n) * 65536, dz = int64_t(zRaw) - (sz / n) * 65536;
        if (((dx * dx) >> 32) + ((dz * dz) >> 32) > 3000 * n) return false;
        ox = dx;
        oz = dz;
        return true;
    }
    // The goal (raw 16.16) of a flyer at (xRaw, zRaw) for a click at (clickX, clickZ).
    void goal(int32_t xRaw, int32_t zRaw, int32_t clickX, int32_t clickZ, int32_t& gx, int32_t& gz) const {
        int64_t ox = 0, oz = 0;
        offset(xRaw, zRaw, ox, oz);   // out of reach: 0, 0 -- the click itself
        gx = int32_t(std::clamp<int64_t>(clickX + ox, INT32_MIN, INT32_MAX));
        gz = int32_t(std::clamp<int64_t>(clickZ + oz, INT32_MIN, INT32_MAX));
    }
};

} // namespace tak::sim
