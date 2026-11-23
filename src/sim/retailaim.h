#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include "sim/retailweapon.h"
#include "sim/retailmotion.h"
#include "sim/detmath.h"

namespace tak {
// Native weapon speed after fixed-point conversion and substep truncation.
inline int32_t retailAimSpeed(float speed) {
    const int32_t raw=int32_t(double(speed)*2184.5333333333333);
    if(raw<=0)return 0;
    const int steps=std::max(1,int((uint32_t(raw)+0xfffffu)>>20));
    return (raw/steps)*steps;
}

// 51ab9f..51acaa: lead from the shooter center to the target SweetSpot,
// using all three velocity components and truncating before each fixed multiply.
inline std::array<int32_t,3> retailAimLead(const std::array<int32_t,3>& shooter,
        std::array<int32_t,3> target,const std::array<int32_t,3>& velocity,
        int32_t speed,bool noLead) {
    if(noLead || !speed)return target;
    std::array<int32_t,3> delta{};
    for(size_t axis=0;axis<3;++axis)
        delta[axis]=std::bit_cast<int32_t>(uint32_t(shooter[axis])-uint32_t(target[axis]));
    const double distance=std::sqrt((double(delta[2])*delta[2]+double(delta[1])*delta[1])+double(delta[0])*delta[0]);
    const int32_t rawDistance=distance<=INT32_MAX ? int32_t(distance):INT32_MIN;
    const double duration=double(rawDistance)/speed*65536.0;
    const int32_t ticks=duration>=INT32_MIN && duration<=INT32_MAX ? int32_t(duration):0;
    const int32_t lead=std::bit_cast<int32_t>(uint32_t((int64_t(ticks)*0xcccc)>>16));
    for(size_t axis=0;axis<3;++axis)
        target[axis]=std::bit_cast<int32_t>(uint32_t(target[axis])+uint32_t((int64_t(velocity[axis])*lead)>>16));
    return target;
}

// 52bd10: ballistic elevation shared by simulation and display. Inputs use world units/tick,
// with the native float boundary before the double-precision trajectory solve.
// 0x8000 is the native unreachable/vertical-shot sentinel.
inline uint16_t retailBallisticPitch(float dx,float dy,float dz,float speed,
                                    float gravityScale,bool highArc,int32_t gravity=8155) {
    const double horizontalSquared=double(dx)*dx+double(dz)*dz;
    if(horizontalSquared==0)return 0x8000;
    const double horizontal=std::sqrt(horizontalSquared);
    const double ratio=double(speed)*speed/(double(gravity)/65536.0*gravityScale*horizontal);
    const double discriminant=ratio*ratio-2.0*double(dy)*ratio/horizontal-1.0;
    if(discriminant<0)return 0x8000;
    const double root=std::sqrt(discriminant);
    const double first=detmath::atan(ratio+root),second=detmath::atan(ratio-root);
    const double angle=(highArc ? std::max(first,second):std::min(first,second))*10430.378350470464;
    if(!std::isfinite(angle))return 0;
    return uint16_t(int32_t(angle));
}

// 52bc60, BallisticWeapon::inRange: the deltas are target minus shooter
// ORIGIN (+0x68/+0x6c/+0x70, 16.16), the speed the weapon's per-substep
// velocity times its substep count (retailAimSpeed). No arc (52bd10 returns
// 0x8000) is out of range; otherwise the base test 530580 decides: range 0
// is always in range, else the per-axis high dwords of the squared deltas,
// summed, against range squared (inclusive). No footprint enters it.
inline bool retailBallisticInRange(int32_t dx,int32_t dy,int32_t dz,int32_t speed,
                                   float gravityScale,bool highArc,int32_t gravity,int32_t range) {
    if(retailBallisticPitch(float(dx)/65536.f,float(dy)/65536.f,float(dz)/65536.f,
                            float(speed)/65536.f,gravityScale,highArc,gravity)==0x8000)return false;
    if(!range)return true;
    return ((int64_t(dx)*dx)>>32)+((int64_t(dz)*dz)>>32)<=int64_t(int32_t(uint32_t(range)*uint32_t(range)));
}
}

namespace tak::sim {
// 52fe30's facing request (51b340 -> 4dc0b0) as a native heading: 53612a of
// the unit's origin minus the point its slot aims at, wrapping. The handler's
// vtable +0x14 picks the point. Only MeleeWeapon's (531f00) answers true and
// faces the target's origin; every other class -- Line of Sight and its
// subclasses, Ballistic, Dropped, Guided, Remote Effect and its subclasses,
// Wandering -- answers false (42a6b0) and faces 51aa50's led SweetSpot
// (52ff67).
inline uint16_t retailSlotFacing(const std::array<int32_t,3>& origin,
        const std::array<int32_t,3>& targetOrigin,const std::array<int32_t,3>& led,bool melee) {
    const auto& point=melee ? targetOrigin : led;
    return uint16_t(retailDirection(Fixed::raw(std::bit_cast<int32_t>(uint32_t(origin[0])-uint32_t(point[0]))),
                                    Fixed::raw(std::bit_cast<int32_t>(uint32_t(origin[2])-uint32_t(point[2])))).v);
}

// 52feaa..52fef5: only a ground mover that can pivot (turninplacerate, type
// +0x190) or a HOVERATTACK flyer posts the request; 52fff0 checks the same
// units and holds fire until the body is within max(aimtolerance, 512) of it.
inline bool retailPostsFacing(bool mover,bool canFly,bool hoverAttack,uint16_t turnInPlaceRate) {
    return canFly ? hoverAttack : mover && turnInPlaceRate>0;
}
inline bool retailFacingAligned(uint16_t heading,uint16_t request,uint16_t tolerance) {
    return std::abs(int(std::bit_cast<int16_t>(uint16_t(heading-request))))<=std::max(int(tolerance),512);
}
}
