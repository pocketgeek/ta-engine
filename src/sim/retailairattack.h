#pragma once

#include "sim/retailconstruction.h"
#include "sim/retailflight.h"
#include "sim/retailmission.h"
#include "sim/retailmotion.h"
#include <bit>
#include <cmath>
#include <cstdint>

namespace ta::sim {

// 41c5b1..41c5c9 and its relatives (TotalA.exe: 0x4b70ef / 0x4b7123 on the
// heading, negated and added, as in AirToGround 0x412ab4): `distance` px from
// `from` along -heading.
inline RetailFlightVector retailAirRunPoint(RetailFlightVector from, int32_t y, uint16_t heading, uint32_t distance) {
    const int32_t magnitude=std::bit_cast<int32_t>(distance<<16);
    return {satSub32(from.x,retailScaledSine(heading,magnitude)),y,
            satSub32(from.z,retailScaledCosine(heading,magnitude))};
}

// TA: TotalA.exe's AirStrike is handler 0x411f50 (mission table 0x4fcc..,
// status "Airstrike"), the attack every shipped bomber's `dropped` weapon
// gets. It is the Kingdoms mission below with these differences, which
// retailAirStrike follows: no group check and no stage-4 air-traffic
// step-out; the leash is the _hypot of whole pixels; the release ring's
// gravity is the OTA's integer and its speed the mover's scalar speed
// (retailStrikeRelease); zero gravity ends the list at stage 4; stage 6
// starts a bomber under 3/4 health again from stage 0, a repair pad's
// VTOL_LANDING ahead of it when it has one (0x4125c2..0x4126b4). The Kingdoms
// notes, for reference:
// AirStrike (registry row 5ebd69, class 2, flags 0x1000600; handler 41b5e0):
// the attack 4de530 picks for a flyer whose selected weapon has the `dropped`
// key (WeaponType +0xc8 bit 0x20) -- mods only, no shipped weapon sets it --
// on a unit (mission +0x16) or a ground point (+0x22). Dive-bombing passes:
//   head:    no weapon (+0x130 0x8000000) ends the list (7). Events 0x2a, a
//            target gone dead or dying, carried (+0x114 bit 4) or off the
//            map (+0x130 & 3 == 0), or a selected weapon without `dropped`
//            go to the lost-target exit (41be1b). A strike without a target
//            whose flags keep the target bit (0x200: the target was lost)
//            pushes VTOL_SEEKATTACK at the flyer's own position when it is
//            the last mission (+0x66 is 0) and ends (5). Event 0x40: a tick's
//            sleep, stage 0. The target's position is the point (+0x22). A
//            leash (+0x56) ends it (5) once the flyer's whole-pixel distance
//            from the origin (+0x2e/+0x30) reaches it. Then the group check,
//            Kingdoms' AirToGroundHover's.
//   stage 0: no mover or not canfly ends the list (7); 416c50 takes off.
//   stage 1: every slot automatic (519b10(3)), the selected one manual
//            (519b70); within 480 px of the point (the 16.16 distance,
//            _ftol'd) a pass first: a point controller 2240 px from the flyer
//            through the point, radius 960, wait 0x742.
//   stage 2: a point half the distance from the flyer, rand(0x4000) - 0x2000
//            round the bearing to the point; radius 480, wait 0x768.
//   stage 3: on to stage 4.
//   stage 4: the air-traffic check (41bac9): at 5 or more on the counter an
//            auto attack (+0x52) on rand(2) == 0 rescans (4d8370, 4d7de0(unit,
//            found, 0, 0, 1, 0) for a found unit not the target: an attack
//            put ahead ends the strike, 5); then VTOL_StepOut, D = (rand(10)
//            x 3 + footX) x 16, the wait cleared, stage 1 a tick after it.
//            Otherwise a controller event (0x700) goes on to stage 5; else
//            the release ring: trunc(sqrt(2 cruisealt / (gravity / 65536)) x
//            30 x the mover's +0x22) + attackrunlength + 1 px round the target
//            (4e3f70, a controller that follows it) or the point, a sleep of
//            a tick and a wait on 0x768.
//   stage 5: the selected slot manual and aimed at the point (51a770); a
//            point controller attackrunlength + 960 px from the flyer through
//            the point, radius 960, wait 0x742.
//   stage 6: the slot's target cleared (51a7f0); a point 1440 px ahead along
//            the flyer's heading, radius 128, wait 0x742, then stage 3.
//   lost (41be1b): the flyer alive and not dying, the strike its last mission
//            and not on Hold Fire, VTOL_SEEKATTACK goes in behind it (4d77b0)
//            at the point, carrying the target when it is alive and not
//            dying; the strike ends (5) either way.
// Every draw is 535cc0.
struct RetailAirStrike {
    RetailFlightVector position;   // unit +0x68
    uint16_t heading=0;            // unit +0x7e
    bool armed=true;               // unit +0x130 0x8000000
    bool dropped=true;             // the selected slot's WeaponType +0xc8 0x20
    bool flies=true;               // a mover (unit +8) and canfly
    bool unitLive=true;            // unit +0x130 0x1000000, not 0x1000
    bool holdFire=false;           // unit +0x130 & 0xc0000 == 0
    bool target=false;             // mission +0x16 is set
    bool targetLive=true;          // ...alive (+0x130 0x1000000) and not dying (0x1000)
    bool targetCarried=false;      // ...carried (+0x114 bit 4)
    bool targetPlaced=true;        // ...on the map (+0x130 & 3 != 0)
    RetailFlightVector targetPoint;// ...its +0x68
    bool last=true;                // mission +0x66 is 0
    bool automatic=false;          // mission +0x52
    int32_t leash=0;               // mission +0x56 (0: none)
    RetailGroundPoint origin;      // mission +0x2e / +0x30
    uint16_t runLength=0;          // UnitType +0x234, attackrunlength
    int16_t cruiseAlt=0;           // UnitType +0x23a
    int32_t gravity=0;             // TA: the OTA gravity integer (map info +0xd3c)
    int16_t speed=0;               // the mover's +0x22, its whole-pixel speed
    int8_t traffic=0;              // unit +0x12a, the air-traffic counter
    int16_t footX=1;               // unit +0x78
};

namespace detail {
// 41b925..41b957: the 16.16 distance from the flyer to the point, through the
// x87 and _ftol.
inline int32_t retailStrikeDistance(RetailFlightVector from,RetailFlightVector to) {
    const double dz=double(satSub32(to.z,from.z));
    const double dx=double(satSub32(to.x,from.x));
    return std::bit_cast<int32_t>(retailFtol(std::sqrt(dx*dx+dz*dz)));
}
// 51b340(unit, point): the unit's heading as seen from the point.
inline uint16_t retailStrikeHeading(RetailFlightVector unit,RetailFlightVector point) {
    return uint16_t(retailDirection(Fixed::raw(satSub32(unit.x,point.x)),
                                    Fixed::raw(satSub32(unit.z,point.z))).v);
}
} // namespace detail

// TotalA.exe 0x4123a8..0x412409: the release ring's radius, the bomb's
// forward carry: the fall time from cruisealt, sqrt(2 cruisealt / gravity) x
// 30 ticks with gravity the OTA's integer (map info +0xd3c, 0x43654d), times
// the mover's whole-pixel speed (mover +0x22: the high word of its 16.16 speed
// +0x20, 0x43cc2d), _ftol'd, plus attackrunlength + 1.
inline int32_t retailStrikeRelease(const RetailAirStrike& in) {
    const double fall=std::sqrt(double(int32_t(in.cruiseAlt))*2.0/double(in.gravity));
    return std::bit_cast<int32_t>(retailFtol(fall*double(30.0f)*double(in.speed))+uint32_t(in.runLength)+1u);
}

// Host: random(bound); beginFlight() (416c50); slotsAuto() (519b10(3));
// slotManual() (519b70 on the selected slot); aimGround(point) (51a770 on the
// selected slot); clearSlot() (51a7f0 on the selected slot); install(point
// goal) (4e40e0 + 4e4540 + 4d4d40, which clears the pending controller events
// 0x3700); follow(radius) (4e3f70 on the target + 4e4540 + 4d4d40);
// retarget() (4d8370, then 4d7de0(unit, found, 0, 0, 1, 0) for a found unit
// that is not the target: true when it put an attack ahead); stepOut(D)
// (VTOL_StepOut built and pushed ahead, 4d7750: false when it could not be);
// seek(target, point) (VTOL_SEEKATTACK inserted behind the strike, 4d77b0);
// the group check's outOfSlot(), nextFlags(), leave() and formation();
// padDamaged() and padDetour() (TotalA.exe's stage-6 pad test and the pad's
// VTOL_LANDING pushed ahead; a host without them has no pads).
// `point` is mission +0x22.
template<class Host>
int retailAirStrike(RetailMissionState& m,RetailFlightVector& point,uint32_t tick,uint32_t events,
                    const RetailAirStrike& in,Host& host) {
    if (!in.armed) return 7;
    // TotalA.exe AirStrike 0x411f50: its head takes the lost-target exit on
    // events 0x1000a (World's 0x2a: the target's death or change of owner).
    const bool lost=(events&0x2a) || (in.target && (!in.targetLive || in.targetCarried || !in.targetPlaced)) ||
                    !in.dropped;
    if (lost) {                                                          // 0x411f6b
        if (in.last && !in.holdFire) host.seek(in.target && in.targetLive,point);
        return 5;
    }
    if (!in.target && (m.flags&0x200u)) {                                // 0x411ff5
        if (in.last) host.seek(false,in.position);
        return 5;
    }
    if (in.target) point=in.targetPoint;                                 // 0x41206e
    if (in.leash) {                                                      // 0x412084: _hypot of whole px
        const double dx=double(int16_t(uint32_t(in.position.x)>>16)-in.origin.x);
        const double dz=double(int16_t(uint32_t(in.position.z)>>16)-in.origin.z);
        if (std::bit_cast<int32_t>(retailFtol(std::sqrt(dz*dz+dx*dx)))>=in.leash) return 5;
    }
    // No air-traffic step-out and no group check: TA's head goes straight on
    // to its stages (0x4120e1).
    switch (m.stage) {
    case 0:                                                              // 0x4120f6
        if (!in.flies) return 7;
        host.beginFlight();
        return 1;
    case 1:                                                              // 0x4121ba
        host.slotsAuto();
        host.slotManual();
        if (detail::retailStrikeDistance(in.position,point)<0x1e00000) {
            host.install(RetailFlightGoal{retailAirRunPoint(in.position,in.position.y,
                detail::retailStrikeHeading(in.position,point),2240),0x30,0,960});
            m.waitMask|=0x742;
        }
        return 1;
    case 2: {                                                            // 0x4122a5
        const int32_t distance=detail::retailStrikeDistance(in.position,point);
        const uint16_t heading=detail::retailStrikeHeading(in.position,point);
        const int32_t half=distance/2;
        const uint16_t angle=uint16_t(host.random(0x4000)+heading-0x2000u);
        RetailFlightGoal goal{{satSub32(in.position.x,retailScaledSine(angle,half)),
                               in.position.y,
                               satSub32(in.position.z,retailScaledCosine(angle,half))},
                              0x30,0,480};
        host.install(goal);
        m.waitMask=0x768;
        return 1;
    }
    case 3:                                                              // 0x41252d
        return 1;
    case 4:                                                              // 0x412394
        if (events&0x700) return 1;
        if (!in.gravity) return 7;                                       // 0x4123d0
        {
            const int32_t radius=retailStrikeRelease(in);
            if (in.target) host.follow(radius);
            else host.install(RetailFlightGoal{point,0x30,0,int16_t(radius)});
        }
        m.sleep(tick,1);
        m.waitMask|=0x768;
        return 2;
    case 5:                                                              // 0x41247f
        host.slotManual();
        host.aimGround(point);
        host.install(RetailFlightGoal{retailAirRunPoint(in.position,in.position.y,
            detail::retailStrikeHeading(in.position,point),uint32_t(in.runLength)+0x3c0),0x30,0,960});
        m.waitMask=0x742;
        return 1;
    case 6:                                                              // 0x41253c
        host.clearSlot();
        host.install(RetailFlightGoal{retailAirRunPoint(in.position,in.position.y,in.heading,1440),0x30,0,128});
        m.waitMask=0x742;
        // 0x4125c2..0x4126b4: under 3/4 health a repair pad within 3840 px
        // takes it to VTOL_LANDING (the wait mask cleared); pad or not, the
        // strike goes back to stage 0.
        if constexpr (requires { host.padDamaged(); host.padDetour(); })
            if (host.padDamaged()) {
                if (host.padDetour()) m.waitMask=0;                      // 0x412697
                return 0;                                                // 0x4126ab / 0x4126c0
            }
        m.stage=3;                                                       // 0x4126cc
        return 2;
    default:
        return 7;
    }
}

} // namespace ta::sim
