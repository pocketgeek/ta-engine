#pragma once
#include <algorithm>
#include <cstdint>

namespace ta::sim {

// TA's group spread (TotalA.exe 0x48cf30, the selection order). The centre is
// the mean of every selected unit but the order's target (0x48cfdf..0x48d049);
// a unit whose mission row carries flag 0x2 at +0x11 (0x48d121..0x48d12a) --
// Move_Ground and VTOL_Move (0x402), Patrol and VTOL_Patrol (0x412),
// RepairPatrol and VTOL_RepairPatrol (0x412): ground and air alike -- takes
// the click plus its offset from that centre, unclamped, when the offset's
// squared length is at most 3000 n px^2 (n the units counted in the centre,
// 0x48d04d..0x48d184; the reach is sqrt(3000 n): 55 px for one, 173 for ten,
// 548 for a hundred). A unit farther out takes the click itself
// (0x48d1d5..0x48d1f4). So a group keeps its shape over the target and only
// stragglers converge on the point. There is no group record after it: each
// unit's mission runs alone (no pacing, no formation, no group check).
//
// The centre is the truncated integer mean of the units' whole-pixel positions
// (the high words of their 16.16 coordinates, idiv); the offset keeps the
// unit's sub-pixel part (position - centre << 16), and its length is tested per
// axis in whole squared pixels ((o * o) >> 32, 0x4e4400 / 0x4e43d0). Everything
// is integer 16.16, so the client (from its frame's floats via
// Fixed::fromFloat) and the sim (an explicit order's OrderShape::Offsets /
// FlyerOffsets) compute the same goals.
//
// Retail pathfinding mode gives every member this spread on a move or patrol;
// Legion keeps its own rule for the surface movers (gameview_clicks.cpp: +-60
// px per axis on a right-click, the shared point for the movers it plans, the
// point itself on an armed move or patrol), a deliberate engine feature.
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

} // namespace ta::sim
