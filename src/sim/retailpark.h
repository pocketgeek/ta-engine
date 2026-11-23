#pragma once
#include "fixed.h"
#include "retailmission.h"
#include "retailgoal.h"
#include <optional>
#include <utility>
#include <bit>

namespace tak::sim {

struct RetailParkState {
    int target=0;
    uint32_t padding=0,attempts=0;
    int32_t permanent=1;
    std::optional<RetailRingGoal> ring;
    // Ours, Legion only (World::legionParkSpot): a factory output's PARK
    // spreads over the ring band instead of steering along its bearing.
    // `door` is the factory's exit bearing (centre to pad, retail BAM), and
    // `spot` the footprint origin this unit has claimed while it is on the
    // leg. Hashed when `spread` is set; Retail never sets it.
    bool spread=false;
    uint16_t door=0;
    std::optional<std::pair<int,int>> spot;
};

// PARK's radius, both branches of 407ca0: footX*16 + padding, plus half the
// target's diagonal while it has a live target. The flight branch computes the
// diagonal as trunc(sqrt(fx^2+fz^2)*16)/2 (the constant at 0x5ebaa8 is -16.0,
// subtracted), which is this integer square root of 256*(fx^2+fz^2).
inline uint32_t retailParkRadius(int16_t footX,uint32_t padding,bool targetPresent,
                                 int16_t targetFootX,int16_t targetFootZ) {
    uint32_t radius=uint32_t(int32_t(footX)*16)+padding;
    if (targetPresent) {
        const uint64_t squared=uint64_t(int64_t(targetFootX)*targetFootX+
                                         int64_t(targetFootZ)*targetFootZ);
        radius+=uint32_t(isqrt64(squared*256)/2);
    }
    return radius;
}

// Flight branch of 407ca0's stage 0 (canfly, test at 0x407e51; 0x407e5a..
// 0x407f62): the unit flies to ONE point on the circle of retailParkRadius
// around the target -- or around itself when the target is gone or no longer
// live (0x407f0a), without the diagonal term -- at angle rand(65536), drawn by
// the stage itself. The point is centre - (sin a, cos a) * r through the
// 0x5360bf / 0x5360f3 tables, pushed as a single VTOL_MOVE child (0x4d4ef0);
// stage 1 retires on its arrival. This is the offset from the centre, Q16.
inline std::pair<int32_t,int32_t> retailFlightParkOffset(uint16_t angle,uint32_t radius) {
    const int32_t r=std::bit_cast<int32_t>(radius<<16);
    return {-retailScaledSine(angle,r),-retailScaledCosine(angle,r)};
}

// Ground branch of 407ca0. The host resolves target lifetime and whether a
// following order can run, and installs the requested annular navigator goal.
template<class Random,class ClearTarget,class ResetGoal>
int retailGroundPark(RetailMissionState& mission,uint32_t events,
        bool hasMover,bool targetPresent,bool targetValid,bool nextReady,
        int16_t footX,int16_t targetFootX,int16_t targetFootZ,
        uint32_t padding,int32_t permanent,uint32_t& attempts,
        Random random,ClearTarget clearTarget,ResetGoal resetGoal) {
    if ((events&8) || (targetPresent && !targetValid)) {
        clearTarget();targetPresent=false;
    }
    if (mission.stage==0) {
        if (!hasMover) return 7;
        attempts=0;return 1;
    }
    if (mission.stage!=1) return 7;
    if ((events&0x100) || nextReady) return 5;
    if (!permanent) {
        const uint32_t first=random(4),second=random(4),third=random(4);
        if (std::bit_cast<int32_t>(attempts)>int32_t(first+second+third)) return 8;
    }
    ++attempts;
    const uint32_t radius=retailParkRadius(footX,padding,targetPresent,targetFootX,targetFootZ);
    resetGoal(targetPresent,std::bit_cast<int32_t>(radius*3),std::bit_cast<int32_t>(radius));
    mission.waitMask=0x2708;return 2;
}

} // namespace tak::sim
