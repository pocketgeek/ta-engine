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
#include <cmath>
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

// 0x49e0ea..0x49e11b (unit creation, 0x49e070): a slot's barrel word S+0x10,
// _ftol((QueryX.z - AimFromX.z) * 1.25) of the two points the new unit's pose
// gives (heading 0x8000 - buildangle/2 + rand(buildangle), 0x485bd6).
inline int32_t taBallisticBarrel(int32_t queryZ,int32_t aimFromZ) {
    return int32_t(double(std::bit_cast<int32_t>(uint32_t(queryZ)-uint32_t(aimFromZ)))*1.25);
}

// 0x49ce4f..0x49cec3: a ballistic shell leaves along the slot's stored
// heading and pitch (S+0x16 made absolute and spread by the fire routine,
// S+0x18 spread), the horizontal part v cos(pitch) along (-sin h, -cos h),
// and its vertical speed v sin(pitch) less gravity times the barrel word over
// the velocity (unsigned division, 0x49ce6a; imul, 0x49ce79).
inline std::array<int32_t,3> taBallisticVelocity(uint16_t heading,uint16_t pitch,int32_t velocity,
                                                 int32_t barrel,int32_t gravity) {
    auto v=taShotVelocity(heading,pitch,velocity);
    const uint32_t ticks=velocity ? uint32_t(barrel)/uint32_t(velocity) : 0u;
    v[1]=std::bit_cast<int32_t>(uint32_t(v[1])-ticks*uint32_t(gravity));
    return v;
}

// 0x49cecc..0x49cf42: a burnblow shell expires when it has flown the
// horizontal distance from the muzzle to the target point at its horizontal
// speed (_hypot, _ftol, idiv); any other at weapontimer. The expiry is only
// read when weapontimer is set (0x49bc19).
inline uint32_t taBallisticLifetime(const std::array<int32_t,3>& muzzle,const std::array<int32_t,3>& target,
                                    uint16_t pitch,int32_t velocity,bool burnBlow,uint16_t weaponTimer) {
    if(!burnBlow)return weaponTimer;
    const int32_t horizontal=retailScaledCosine(pitch,velocity);
    const double dz=double(std::bit_cast<int32_t>(uint32_t(muzzle[2])-uint32_t(target[2])));
    const double dx=double(std::bit_cast<int32_t>(uint32_t(muzzle[0])-uint32_t(target[0])));
    const double distance=std::sqrt(dx*dx+dz*dz);
    const int32_t whole=distance<2147483648.0 ? int32_t(distance) : INT32_MIN;
    if(!horizontal)return 0;
    return uint32_t(whole/horizontal);
}

// 0x49bce3..0x49bd41 (ballistic and dropped shots): the position moves by the
// velocity, then by the wind vector G+0x37ecc (-2 r sin, 0, -2 r cos of the
// wind's speed and direction, 0x490cf7..0x490d35), then gravity comes off the
// vertical speed. Once a tick, no sub-steps.
inline void taBallisticStep(std::array<int32_t,3>& position,std::array<int32_t,3>& velocity,
                            const std::array<int32_t,3>& wind,int32_t gravity) {
    for(size_t axis=0;axis<3;++axis)
        position[axis]=std::bit_cast<int32_t>(uint32_t(position[axis])+uint32_t(velocity[axis]));
    for(size_t axis=0;axis<3;++axis)
        position[axis]=std::bit_cast<int32_t>(uint32_t(position[axis])+uint32_t(wind[axis]));
    velocity[1]=std::bit_cast<int32_t>(uint32_t(velocity[1])-uint32_t(gravity));
}

// 0x49bb60..0x49bc07, a line-of-sight shot's tick while its expiry (+0x46)
// is ahead (0x49bb7c; at or past it the shot retires without a burst,
// 0x49bcd0): the head (+0x04) moves by the velocity, straight, no gravity.
// A beamweapon also has a tail (+0x10, the muzzle at launch, 0x49c761):
// once +0x69 bit 0 is set it moves by the same velocity (0x49bbbb); until
// then the bit is set on the first tick whose counter is past the launch
// tick (+0x42) plus `duration` (0x49bbe1..0x49bc03), so the tail starts a
// tick later. The two ends draw the beam (0x49bf8d); only the head is ever
// tested for a hit (0x49bd86 -> 0x49b090).
struct TaLineOfSightShot {
    std::array<int32_t,3> head{},tail{},velocity{};
    uint32_t launch=0;      // +0x42
    bool tailMoving=false;  // +0x69 bit 0
};

inline void taLineOfSightStep(TaLineOfSightShot& shot,bool beam,uint16_t duration,uint32_t now) {
    for(size_t axis=0;axis<3;++axis)
        shot.head[axis]=std::bit_cast<int32_t>(uint32_t(shot.head[axis])+uint32_t(shot.velocity[axis]));
    if(!beam)return;
    if(shot.tailMoving) {
        for(size_t axis=0;axis<3;++axis)
            shot.tail[axis]=std::bit_cast<int32_t>(uint32_t(shot.tail[axis])+uint32_t(shot.velocity[axis]));
    } else if(now>uint32_t(shot.launch+uint32_t(duration))) shot.tailMoving=true;
}

// 0x490d05 / 0x490d31: the wind vector from the wind's r sin and r cos.
inline std::array<int32_t,3> taWindVector(int32_t sine,int32_t cosine) {
    return {std::bit_cast<int32_t>((0u-uint32_t(sine))<<1),0,std::bit_cast<int32_t>((0u-uint32_t(cosine))<<1)};
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
