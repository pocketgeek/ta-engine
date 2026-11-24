#pragma once

#include "sim/retailairattack.h"
#include "sim/retailconstruction.h"
#include "sim/retailflight.h"
#include "sim/retailmission.h"
#include "sim/retailmotion.h"
#include "sim/retailpiecepose.h"
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

namespace ta::sim {

// AirToAir (TotalA.exe air mission table 0x4fca18 row 8, handler 0x412d40,
// flags 0x200): the mission of a flyer attacking a flyer. It flies the
// attacker; slot 0 fires on its own (the weapon update).
//
//   head:    the lost-target events 0x10008 (0x412d4a; the engine's 0x28) end
//            it (5), first appending VTOL_SEEKATTACK when it is the last
//            mission (+0x4a) -- World's seekLostTarget; off the map, 800 px
//            toward the playable map's centre, radius 128 (0x412e54..0x412f44,
//            retailTaAirReturnToMap); the leash (+0x3e, 0x412f47) is the host's.
//   stage 0: (0x413397) no mover or not canfly ends the list (7); every slot
//            manual (0x4898b0(3)), out of a transport, Activate; landed, the
//            take-off (0x43d210(2), a point controller on the spot at half
//            cruisealt); a tick's sleep, the counter (+0x36) 0.
//   stage 1: (0x412fb2) every slot automatic, slot 0 manual and aimed at the
//            target (0x489800(3), 0x4898b0(0), 0x48a060(unit, target, 0)).
//            Woken by the controller (0xe0) with the target ahead (a 20 px
//            dot product, 0x412fda..0x413054): the attack run, a moving
//            controller one second of top speed ahead along the heading,
//            moving at top speed, a sleep of 60 + rand(30), the counter 0
//            (0x41305a..0x413156). Woken by the controller with the target
//            behind, or by the sleep with the counter at 90 or more: VTOL_EVADE
//            ahead of it (0x43acb0), the controller dropped, the counter and
//            wait mask 0, back to stage 0 (0x41332f..0x413394). Otherwise
//            (0x41316d): the counter 0 with the target ahead, else +45; more
//            than 160 px from the target, a moving controller at the target
//            led by 45 ticks of its velocity, moving at that velocity plus
//            half the target's top speed along its heading (0x413237..0x413307);
//            a sleep of 45 waiting on 0x100e8 (0x41330c..0x41331d).
//
// No group check, rescan or air-traffic step-out: TotalA.exe's mission tables
// hold none (docs/ta-port-plan.md). The only draw is the attack run's rand(30)
// (0x4b6c30), the shared game random number generator.

// 41d780 (Kingdoms): whether `a` lies in the half plane ahead of `b` once b's
// facing is turned by octant x 45 degrees. Both directions are 20 px vectors;
// the test multiplies their whole-pixel words in 16 bits.
inline bool retailAirAhead(RetailFlightVector a,RetailFlightVector b,uint16_t heading,int octant) {
    const uint16_t toB=uint16_t(retailDirection(
        Fixed::raw(satSub32(b.x,a.x)),
        Fixed::raw(satSub32(b.z,a.z))).v);
    const uint16_t look=uint16_t(heading+octant*0x2000);
    constexpr int32_t length=20*65536;
    auto whole=[](int32_t value) { return int(int16_t(uint32_t(-value)>>16)); };
    const int dot=whole(retailScaledSine(look,length))*whole(retailScaledSine(toB,length))+
                  whole(retailScaledCosine(look,length))*whole(retailScaledCosine(toB,length));
    return int16_t(dot)>=0;
}

// TotalA.exe AirToAir's facing test (0x412fda..0x413054, again at
// 0x41316d..0x4131e7): the 20 px vector toward the target (0x48a980(unit,
// target), 0x4b70ef / 0x4b7123, negated) against the 20 px heading vector
// (0x4103a0 on unit +0x66); the whole-pixel words multiplied, the sum's low
// 16 bits strictly positive.
inline bool retailTaAirAhead(RetailFlightVector unit,RetailFlightVector target,uint16_t heading) {
    const uint16_t toward=uint16_t(retailDirection(
        Fixed::raw(satSub32(unit.x,target.x)),
        Fixed::raw(satSub32(unit.z,target.z))).v);
    constexpr int32_t length=20*65536;
    auto whole=[](int32_t value) { return int(int16_t(uint32_t(-value)>>16)); };
    const int dot=whole(retailScaledSine(toward,length))*whole(retailScaledSine(heading,length))+
                  whole(retailScaledCosine(toward,length))*whole(retailScaledCosine(heading,length));
    return int16_t(dot)>0;
}

// TotalA.exe's moving controller (vtable 0x4fd3f8, 0x2c bytes, ctor
// 0x44e740: point +0xa, velocity +0x16, flags +8 = 0): a point that moves on
// by its own velocity each time the navigator asks for it (0x44ea60, x and z
// only). Its type (+0x08, 0x44e7a0) answers 3: the navigator flies it at
// cruise height at any distance (0x44ec10, its altitude setter, does
// nothing). Within 48 px (0x44eb60) the navigator posts arrival and, as
// +0x2c (0x44ea50) answers 0, lets it go (0x44f1a0..0x44f1d4). The turning
// flag (+8 bit 0, toward +0x24) is never set in TotalA.exe; the turn below is
// Kingdoms' (4e48d0), kept for `turns`.
struct RetailInterceptGoal {
    RetailFlightVector point;      // +0x0a
    RetailFlightVector velocity;   // +0x16
    uint16_t heading=0;            // +0x24
    bool turns=false;              // +0x08 & 1

    RetailFlightVector target(uint16_t turnRate) {
        const RetailFlightVector answer=point;
        point.x=satAdd32(point.x,velocity.x);
        point.z=satAdd32(point.z,velocity.z);
        const uint16_t moving=uint16_t(retailDirection(
            Fixed::raw(std::bit_cast<int32_t>(0u-uint32_t(velocity.x))),
            Fixed::raw(std::bit_cast<int32_t>(0u-uint32_t(velocity.z)))).v);
        if (turns && moving!=heading) {
            const int difference=int16_t(uint16_t(heading-moving));
            const int limit=int(turnRate>>3);
            const int step=difference>=limit ? limit : difference>-limit ? difference : -limit;
            retailRotatePair(velocity.x,velocity.z,uint16_t(-step));
            velocity.y=0;
        }
        return answer;
    }
    // 0x44eb60: within 48 px in 2D (x87 at double precision, times 1/65536).
    bool accepts(RetailFlightVector position) const {
        const double dx=double(satSub32(position.x,point.x));
        const double dz=double(satSub32(position.z,point.z));
        return std::sqrt(dz*dz+dx*dx)<48.0*65536;
    }
    // 0x44eb40: the navigator's heading, toward the point (0x48a980). A
    // turning controller (Kingdoms 4e4b70) keeps its stored heading inside
    // 48 px.
    uint16_t headingFor(RetailFlightVector position,uint16_t) const {
        if (turns && accepts(position)) return heading;
        return uint16_t(retailDirection(
            Fixed::raw(satSub32(position.x,point.x)),
            Fixed::raw(satSub32(position.z,point.z))).v);
    }
};

// What AirToAir reads at one wake.
struct RetailAirDuel {
    RetailFlightVector position;        // unit +0x6a
    uint16_t heading=0;                 // unit +0x66
    RetailFlightVector target;          // the target's +0x6a
    uint16_t targetHeading=0;           // the target's +0x66
    RetailFlightVector targetVelocity;  // the target mover's +8
    int32_t maxSpeed=0;                 // type +0x192, maxvelocity
    int32_t targetMaxSpeed=0;           // the target type's
    bool flies=true;                    // a mover (unit +0) and canfly (type +0x241 bit 0x800)
    bool onMap=true;                    // unit +0x82 is not the off-map sector (game +0x142b7)
    int32_t width=0,height=0;           // game +0x1422b / +0x1422f: the playable map, in pixels
};

namespace detail {
// Kingdoms' off-map return (41cb80..41cc6d): 800 px toward a point 192 px
// inside the playable area's edge, radius 128, waiting on the controller.
// Still VTOL_SeekGuard's (retailairseek.h).
template<class Host>
int retailAirReturnToMap(RetailMissionState& m,RetailFlightVector position,int32_t width,int32_t height,Host& host) {
    RetailFlightVector inside=position;
    auto clamp=[](int32_t& v,int32_t size) {
        if (v<0xc00000) v=0xc00000;
        else if (const int32_t edge=std::bit_cast<int32_t>(uint32_t(size+0xff40)<<16);v>edge) v=edge;
    };
    clamp(inside.x,width);clamp(inside.z,height);
    const uint16_t heading=uint16_t(retailDirection(
        Fixed::raw(satSub32(position.x,inside.x)),
        Fixed::raw(satSub32(position.z,inside.z))).v);
    host.install(RetailFlightGoal{retailAirRunPoint(position,position.y,heading,800),0x30,0,128});
    m.waitMask|=0x700;
    return 2;
}
}

// TotalA.exe's off-map return (AirToAir 0x412e54..0x412f44, VTOL_SeekAttack
// 0x4103ff..0x4104ec): 800 px from the flyer toward the playable map's centre
// (0x48a980, 0x4b70ef / 0x4b7123, 0x40f790), a point controller of radius
// 128 (0x44e730(0x80)), waiting on it (0xe0).
template<class Host>
int retailTaAirReturnToMap(RetailMissionState& m,RetailFlightVector position,int32_t width,int32_t height,Host& host) {
    const int32_t cx=std::bit_cast<int32_t>(uint32_t(width/2)<<16);
    const int32_t cz=std::bit_cast<int32_t>(uint32_t(height/2)<<16);
    const uint16_t heading=uint16_t(retailDirection(
        Fixed::raw(satSub32(position.x,cx)),
        Fixed::raw(satSub32(position.z,cz))).v);
    host.install(RetailFlightGoal{retailAirRunPoint(position,position.y,heading,800),0x30,0,128});
    m.waitMask|=0x700;
    return 2;
}

// Host: random(bound); beginFlight() (stage 0's slots, Activate and take-off);
// aim() (stage 1's slot calls); install(intercept goal) (0x4388d0, which also
// clears the mission's pending controller events); install(point goal) (the
// off-map return); release() (0x4388d0(0)); evade() (VTOL_EVADE pushed ahead
// on the target, 0x43acb0). `count` is mission +0x36.
template<class Host>
int retailAirToAir(RetailMissionState& m,int32_t& count,uint32_t tick,uint32_t events,
                   const RetailAirDuel& duel,Host& host) {
    if (events&0x28) return 5;                                       // 0x412d4a
    if (!duel.onMap) return retailTaAirReturnToMap(m,duel.position,duel.width,duel.height,host);
    switch (m.stage) {
    case 0:                                                          // 0x413397
        if (!duel.flies) return 7;
        host.beginFlight();
        m.sleep(tick,1);                                             // 0x413444
        count=0;
        return 1;
    case 1: break;                                                   // 0x412fb2
    default: return 7;
    }
    host.aim();
    const bool ahead=retailTaAirAhead(duel.position,duel.target,duel.heading);
    const bool controller=(events&0x700)!=0;                         // 0x412fce: TA 0xe0
    if (controller && ahead) {
        // 0x41305a..0x413156: one second (x 30) of top speed ahead, moving at
        // top speed along the heading.
        const int32_t run=std::bit_cast<int32_t>(uint32_t(duel.maxSpeed)*30u);
        const RetailFlightVector point{
            satSub32(duel.position.x,retailScaledSine(duel.heading,run)),duel.position.y,
            satSub32(duel.position.z,retailScaledCosine(duel.heading,run))};
        const RetailFlightVector velocity{-retailScaledSine(duel.heading,duel.maxSpeed),0,
                                          -retailScaledCosine(duel.heading,duel.maxSpeed)};
        host.install(RetailInterceptGoal{point,velocity,0,false});
        m.sleep(tick,host.random(30)+60);                            // 0x413131
        count=0;
        return 2;
    }
    if (!controller && count<90) {                                   // 0x413163
        if (ahead) count=0; else count+=45;                          // 0x4131e4..0x4131f2
        // 0x4131f6..0x41323e: the whole-pixel word of the distance to the target.
        const double dz=double(satSub32(duel.position.z,duel.target.z));
        const double dx=double(satSub32(duel.position.x,duel.target.x));
        const int span=int(int16_t(retailFtol(std::sqrt(dz*dz+dx*dx))>>16));
        if (span>160) {
            // 0x413244..0x413307: the target 45 ticks on by its velocity,
            // moving at that velocity plus half its top speed along its heading.
            const RetailFlightVector& v=duel.targetVelocity;
            const RetailFlightVector point{
                std::bit_cast<int32_t>(uint32_t(duel.target.x)+uint32_t(v.x)*45u),duel.target.y,
                std::bit_cast<int32_t>(uint32_t(duel.target.z)+uint32_t(v.z)*45u)};
            const int32_t half=duel.targetMaxSpeed/2;
            const RetailFlightVector velocity{
                std::bit_cast<int32_t>(uint32_t(v.x)-uint32_t(retailScaledSine(duel.targetHeading,half))),v.y,
                std::bit_cast<int32_t>(uint32_t(v.z)-uint32_t(retailScaledCosine(duel.targetHeading,half)))};
            host.install(RetailInterceptGoal{point,velocity,0,false});
        }
        m.sleep(tick,45);                                            // 0x41330c
        m.waitMask|=0x28|0x700;                                      // 0x100e8
        return 2;
    }
    // 0x41332f..0x413394: break off.
    host.release();
    host.evade();
    count=0;
    m.waitMask=0;
    return 0;
}

// VTOL_Evade (TotalA.exe air mission table row 17, handler 0x413bc0): pushed
// ahead of AirToAir when it breaks off, on the same target. D is slot 0's
// weapon range (unit +0x10 -> +0xdc).
//   head:    no target (+0x16) or the lost-target events 0x10008 end it (5).
//   stage 0: (0x413c9a) no mover or not canfly ends the list (7); a side
//            (+0x36) of rand(2); a point controller D px out at 90 degrees
//            from the heading (unit +0x66 - 0x4000 for side 1, + 0x4000 for
//            side 0), radius 128, waiting on 0x100e8 (no sleep).
//   stage 1: (0x413c18) the same on the same side, 2 x D px out.
//   stage 2: it ends (5).
struct RetailAirEvade {
    RetailMissionState mission;
    int32_t side=0;                // mission +0x36
};

// What VTOL_Evade reads.
struct RetailAirEscape {
    RetailFlightVector position;   // unit +0x6a
    uint16_t heading=0;            // unit +0x66
    int32_t distance=0;            // slot 0's range, WeaponType +0xdc
    bool flies=true;               // a mover and canfly
    bool target=true;              // mission +0x16 is set
};

// World's AirToAir state on an attack order, beside the order's `mission`
// (AirToAir's own) and `flightGoal` (a point controller: off the map, or one
// of VTOL_Evade's).
struct RetailAirToAirState {
    int32_t count=0;                              // AirToAir's mission +0x36
    std::optional<RetailInterceptGoal> intercept; // the moving controller installed
    std::optional<RetailAirEvade> evade;          // VTOL_Evade, pushed ahead of AirToAir
    bool evadeController=false;                   // the controller is the evade's
};

// Host: random(bound); install(point goal).
template<class Host>
int retailAirEvade(RetailAirEvade& e,uint32_t,uint32_t events,const RetailAirEscape& in,Host& host) {
    auto& m=e.mission;
    if (!in.target || (events&0x28)) return 5;                       // 0x413bd8..0x413beb
    uint32_t distance=uint32_t(in.distance);
    switch (m.stage) {
    case 0:                                                          // 0x413c9a
        if (!in.flies) return 7;
        e.side=int32_t(host.random(2));
        break;
    case 1:                                                          // 0x413c18
        distance*=2;
        break;
    case 2: return 5;                                                // 0x413c03
    default: return 7;
    }
    const uint16_t angle=uint16_t(in.heading+(e.side ? -0x4000 : 0x4000));
    host.install(RetailFlightGoal{retailAirRunPoint(in.position,in.position.y,angle,distance),0x30,0,128});
    m.waitMask=0x28|0x700;                                           // 0x413d54: 0x100e8
    return 1;
}

} // namespace ta::sim
