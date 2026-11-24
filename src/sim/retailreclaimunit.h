#pragma once

// TotalA.exe's RECLAIMUNIT arithmetic: ReclaimUnit (0x404730, ground table
// row 20 at 0x4fc8dc) and VTOL_ReclaimUnit (0x414a80, air table row 16 at
// 0x4fcba8). The mission itself is World::tickReclaimUnit.

#include "sim/retailconstruction.h"   // retailFtol

#include <algorithm>
#include <cstdint>

namespace ta::sim {

// 0x438650(unit, target, 15): the damage one reclaim record deals, stored in
// mission +0x36 when the approach is installed (0x40483d, 0x414c86):
//   max(1, ftol(WT x ((kills + 5) / 5) x maxdamage x period
//                / (max(buildcostmetal, 10) x 300)))
// WT is the reclaimer type's workertime (+0x1fe, u16), kills its own count
// (+0xb8, u16), maxdamage and buildcostmetal the TARGET type's (+0x1fa,
// +0x18a). The integer product is a 32-bit imul loaded as an unsigned qword
// (0x4386c8..0x4386cc); buildcostmetal above 10 is kept, else 10 (0x43865f).
inline int32_t retailReclaimUnitRate(uint16_t workerTime, uint16_t kills, int32_t maxDamage,
                                     float buildCostMetal, int32_t period = 15) {
    const uint32_t veteran = uint32_t((int32_t(kills) + 5) / 5);
    const uint32_t work = uint32_t(workerTime) * veteran * uint32_t(maxDamage) * uint32_t(period);
    const float cost = buildCostMetal > 10.0f ? buildCostMetal : 10.0f;
    const int32_t rate = int32_t(retailFtol(double(work) / (double(cost) * 300.0)));
    return rate > 1 ? rate : 1;
}

// The target type's +0x184: the high word of +0x182, (sx + sz) / 3 with the
// footprint sizes in 16.16 (fx x 16 px each; docs/retail-data.md).
inline int32_t retailReclaimUnitRadius(int footX, int footZ) {
    const int32_t sum = int32_t(uint32_t(footX) * 16u * 65536u + uint32_t(footZ) * 16u * 65536u);
    return int16_t(uint32_t(sum / 3) >> 16);
}

// The work stage's reach (ground 0x4048d2..0x404955, VTOL 0x414af0..0x414b61):
// each 16.16 delta squared as a 64-bit product, its high dword kept (whole
// square pixels), the two summed in 32 bits, against reach squared. The
// ground reach is builddistance + retailReclaimUnitRadius of the target; the
// flyer's builddistance alone.
inline bool retailReclaimUnitInReach(int32_t dx, int32_t dz, int32_t reach) {
    const uint32_t sx = uint32_t(uint64_t(int64_t(dx) * dx) >> 32);
    const uint32_t sz = uint32_t(uint64_t(int64_t(dz) * dz) >> 32);
    return int32_t(sx + sz) <= int32_t(uint32_t(reach) * uint32_t(reach));
}

// 0x486c9e..0x486d1e: what a unit dying of a kind-5 record pays the unit that
// hit it last (+0xf0), into that unit's metal production (+0xd4):
// (1.0 - remaining) x buildcostmetal, remaining the dead unit's build
// fraction left (+0x104, 0 when finished). A computer player's (+0x73 2)
// takes x 0.5 on difficulty 0 and x 0.7 on 1 (game +0x37eee; the double
// constants -0.5 / -0.7 subtracted), as World's books.mult carries them.
inline float retailReclaimCredit(float production, float remaining, float buildCostMetal, float mult) {
    const double value = (1.0 - double(remaining)) * double(buildCostMetal);
    if (mult == 1.0f) return float(double(production) + value);
    const double m = mult == 0.5f ? 0.5 : mult == 0.7f ? 0.7 : double(mult);
    return float(double(production) - value * -m);
}

} // namespace ta::sim
