#pragma once

// TotalA.exe's own projectile launch and per-tick motion for the
// line-of-sight, self-propelled, vertical-launch and dropped classes
// (docs/retail-data.md 3.3, docs/retail-weapon-scripts.md 4). Projectile
// record: +0x04 position, +0x1c velocity, +0x28 target point, +0x36 heading,
// +0x38 pitch, +0x3a speed, +0x3e launch distance, +0x46 expiry tick,
// +0x69 bits 4..5 two-phase stage. All 16.16 per tick; angles BAM with the
// mover's convention (heading h travels along (-sin h, -cos h)).

#include "sim/retailanimationqueries.h"
#include "sim/retailmotion.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>

namespace ta::sim {

// 0x49ca37..0x49caa3: heading atan2(d.x, d.z) and pitch
// atan2(-(d.y>>16), |d.xz|>>16) of d = from - to (positive pitch: the
// point is above). The same arithmetic as retailDirectAim with no body.
inline std::array<uint16_t,2> taShotAngles(const std::array<int32_t,3>& from,
                                           const std::array<int32_t,3>& to) {
    std::array<int32_t,3> d{};
    for(size_t axis=0;axis<3;++axis)
        d[axis]=std::bit_cast<int32_t>(uint32_t(from[axis])-uint32_t(to[axis]));
    return retailDirectAim(d[0],d[1],d[2],0);
}

// 0x49caa7..0x49cad3 (and 0x49cca5 for vlaunch): startvelocity when set,
// else weaponvelocity for a weapon with no weaponacceleration, else 0.
inline int32_t taShotStartSpeed(int32_t startVelocity,int32_t acceleration,int32_t velocity) {
    if(startVelocity)return startVelocity;
    return acceleration ? 0 : velocity;
}

// 0x49cad3..0x49cb14 / 0x49ba74..0x49babe: vy = speed sin(pitch); the
// horizontal part speed cos(pitch) along the heading.
inline std::array<int32_t,3> taShotVelocity(uint16_t heading,uint16_t pitch,int32_t speed) {
    const int32_t horizontal=retailScaledCosine(pitch,speed);
    return {-retailScaledSine(heading,horizontal),retailScaledSine(pitch,speed),
            -retailScaledCosine(heading,horizontal)};
}

// 0x49cb17..0x49cb65: the expiry, (range << 16) / weaponvelocity ticks
// from now, or weapontimer for a weapon with no velocity or with
// noautorange.
inline uint32_t taShotLifetime(int32_t range,int32_t velocity,bool noAutoRange,uint16_t weaponTimer) {
    if(velocity && !noAutoRange)return (uint32_t(range)<<16)/uint32_t(velocity);
    return weaponTimer;
}

// 0x49b520: turn heading and pitch toward `target` by at most `turnRate`
// each (an angle within the rate is taken outright). With burnblow, a
// heading or pitch error over 0x6978 detonates the shot instead (false).
inline bool taShotSteer(uint16_t& heading,uint16_t& pitch,const std::array<int32_t,3>& position,
                        const std::array<int32_t,3>& target,uint16_t turnRate,bool burnBlow) {
    const auto want=taShotAngles(position,target);
    uint16_t* const angles[2]={&heading,&pitch};
    for(size_t i=0;i<2;++i) {
        uint16_t& now=*angles[i];
        const int16_t diff=std::bit_cast<int16_t>(uint16_t(want[i]-now));
        const int16_t size=int16_t(std::abs(int32_t(diff)));   // -32768 stays negative (cmp ax)
        if(size>0x6978 && burnBlow)return false;
        if(int32_t(size)<int32_t(turnRate))now=want[i];
        else now=uint16_t(diff<0 ? now-turnRate : now+turnRate);
    }
    return true;
}

} // namespace ta::sim
