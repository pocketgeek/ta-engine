#pragma once

#include "sim/retailairattack.h"
#include "sim/retailairtoair.h"
#include "sim/retailflight.h"
#include "sim/retailmission.h"
#include "sim/retailmotion.h"
#include <bit>
#include <cmath>
#include <cstdint>

namespace ta::sim {

// TotalA.exe's attacks on a target that cannot fly: AirToGround (air mission
// table 0x4fca18 row 9, handler 0x412710) for a flyer without hoverattack,
// AirToGroundHover (row 10, handler 0x413470) for one with it. Neither has a
// group check, a formation or an air-traffic step-out (the mission tables
// hold none, docs/ta-port-plan.md), and neither fires itself: slot 0 is
// given the target and the weapon update fires it whenever it is in range.
// Both read slot 0's weapon range (unit +0x10 -> WeaponType +0xdc), not a
// selected slot's. Events are the engine's: TA's lost-target bits 0x10008 are
// 0x28 here (0x1000a, with 0x2, 0x2a), its controller bits 0xe0 are 0x700, so
// the wait masks 0x100e8 / 0x100ea are 0x728 / 0x72a.
//
// The head of both:
//   events 0x2a (AirToGround, 0x41272c) or 0x28 (AirToGroundHover, 0x413477):
//            the attack ends (5), first appending VTOL_SEEKATTACK (0x43ad10)
//            with its target (+0x16) and point (+0x22) when it is the last
//            mission (+0x4a, the next link, is 0) and not on Hold Fire (unit
//            +0x110 & 0x300000).
//   no target and mission flag 0x200 (0x41279a, 0x413505): the attack ends
//            (5), first appending VTOL_SEEKATTACK at the flyer's own position
//            when it is the last mission (no fire-state test).
//   the leash (+0x3e, 0x412849, 0x413670: _hypot of whole pixels from +0x2e /
//            +0x30) is the host's.
struct RetailTaAirAttack {
    RetailFlightVector position;    // unit +0x6a
    uint16_t heading=0;             // unit +0x66
    int32_t range=0;                // slot 0's WeaponType +0xdc
    bool flies=true;                // a mover (unit +0) and canfly (type +0x241 0x800)
    bool target=false;              // mission +0x16 is set
    RetailFlightVector targetPoint; // ...its +0x6a
    bool last=true;                 // mission +0x4a is 0
    bool holdFire=false;            // unit +0x110 & 0x300000 == 0
    bool onMap=true;                // unit +0x82 is not the off-map sector (game +0x142b7)
    int32_t width=0,height=0;       // game +0x1422b / +0x1422f: the playable map, in pixels
};

namespace detail {
template<class Host>
bool retailTaAirAttackHead(RetailMissionState& m,uint32_t events,uint32_t lost,const RetailTaAirAttack& in,
                           RetailFlightVector point,Host& host) {
    if (events&lost) {
        if (in.last && !in.holdFire) host.seek(in.target,point);
        return true;
    }
    if (!in.target && (m.flags&0x200u)) {
        if (in.last) host.seek(false,in.position);
        return true;
    }
    return false;
}

// Stage 1 of both (AirToGround 0x412977..0x412a9e, AirToGroundHover
// 0x4137a7..0x41388a): a point controller (radius 0x80, 0x44e730) half the
// way to `to` (the 16.16 distance through _hypot and _ftol, halved toward
// zero), rand(0x4000) - 0x2000 round the bearing 0x48a980(unit, to).
template<class Host>
RetailFlightGoal retailTaAirApproach(RetailFlightVector position,RetailFlightVector to,Host& host) {
    const double dz=double(satSub32(to.z,position.z));
    const double dx=double(satSub32(to.x,position.x));
    const int32_t distance=std::bit_cast<int32_t>(retailFtol(std::sqrt(dx*dx+dz*dz)));
    const uint16_t bearing=uint16_t(retailDirection(Fixed::raw(satSub32(position.x,to.x)),
                                                    Fixed::raw(satSub32(position.z,to.z))).v);
    const int32_t half=distance/2;
    const uint16_t angle=uint16_t(host.random(0x4000)+bearing-0x2000u);
    return {{satSub32(position.x,retailScaledSine(angle,half)),position.y,
             satSub32(position.z,retailScaledCosine(angle,half))},0x30,0,0x80};
}
} // namespace detail

// AirToGround (0x412710): attack runs through the target.
//   head:    as above; then the target's position into +0x22 (0x412812) and,
//            off the map, a sleep of 30 and stage 2 (0x412828..0x412845).
//   stage 0: (0x4128b5) no mover or not canfly ends the list (7); every slot
//            manual (0x4898b0(3)), off any carrier (0x48aac0(unit, 0, -1,
//            2)), Activate (0x48b090(1, 1)) and, landed, the take-off
//            (0x43d210(2), the climb controller at cruisealt / 2, waiting on
//            it, 0xe0). 1.
//   stage 1: (0x412977) every slot automatic (0x489800(3)); the approach
//            (detail::retailTaAirApproach) to the point; wait 0x100e8. 1.
//   stage 2: (0x412a4b) slot 0 manual (0x4898b0(0)) and given the target
//            (0x48a060(unit, target, 0)) or the point (0x48a0a0); a point
//            controller on the point, radius the range; wait 0x100e8. 1.
//   stage 3: (0x412ab4) the run: a point 3 x range past the point on the line
//            from the flyer (0x4b715a's atan2 of unit - point), radius 0x80 +
//            rand(0x80); wait 0x100ea. 1.
//   stage 4: (0x412b3f) under 3/4 health a repair pad (0x40b530) takes it to
//            VTOL_LANDING ahead, the controller let go, the wait mask cleared,
//            0 (retailairpad.h). Otherwise the turn: rand(2) picks 90 degrees
//            right (+0x4000) or left of the heading, range px out, radius
//            0x80; wait 0x100ea. 1.
//   stage 5: (0x412cf9) back to stage 2 (2).
// Host: random(bound); seek(target, point) (VTOL_SEEKATTACK appended);
// beginFlight() (stage 0's calls: true when it took off); slotsAuto()
// (0x489800(3)); aim() (stage 2's slot calls); install(goal) (0x4388d0, which
// also clears the pending controller events); padDamaged() and padDetour()
// (the pad test and the pad's VTOL_LANDING pushed ahead). `point` is mission
// +0x22.
template<class Host>
int retailTaAirToGround(RetailMissionState& m,RetailFlightVector& point,uint32_t tick,uint32_t events,
                        const RetailTaAirAttack& in,Host& host) {
    if (detail::retailTaAirAttackHead(m,events,0x2a,in,point,host)) return 5;      // 0x41272c..0x41280b
    if (in.target) point=in.targetPoint;                                             // 0x412812
    if (!in.onMap) { m.sleep(tick,30); m.stage=2; }                                  // 0x412828..0x412845
    switch (m.stage) {
    case 0:                                                                          // 0x4128b5
        if (!in.flies) return 7;
        if (host.beginFlight()) m.waitMask|=0x700;                                   // 0x412960
        return 1;
    case 1:                                                                          // 0x412977
        host.slotsAuto();
        host.install(detail::retailTaAirApproach(in.position,point,host));
        m.waitMask=0x728;                                                            // 0x412a9e
        return 1;
    case 2:                                                                          // 0x412a4b
        host.aim();
        host.install(RetailFlightGoal{point,0x30,0,int16_t(in.range)});
        m.waitMask=0x728;
        return 1;
    case 3: {                                                                        // 0x412ab4
        const uint16_t heading=uint16_t(retailDirection(Fixed::raw(satSub32(in.position.x,point.x)),
                                                        Fixed::raw(satSub32(in.position.z,point.z))).v);
        const RetailFlightVector past=retailAirRunPoint(point,point.y,heading,uint32_t(in.range)*3u);
        host.install(RetailFlightGoal{past,0x30,0,int16_t(host.random(0x80)+0x80)});
        m.waitMask=0x72a;                                                            // 0x412ce3
        return 1;
    }
    case 4: {                                                                        // 0x412b3f
        if (host.padDamaged() && host.padDetour()) { m.waitMask=0; return 0; }      // 0x412bba..0x412c49
        const uint16_t heading=uint16_t(in.heading+(host.random(2) ? 0x4000 : -0x4000));   // 0x412c55
        host.install(RetailFlightGoal{retailAirRunPoint(in.position,in.position.y,heading,uint32_t(in.range)),
                                      0x30,0,0x80});
        m.waitMask=0x72a;
        return 1;
    }
    case 5:                                                                          // 0x412cf9
        m.stage=2;
        return 2;
    default:
        return 7;
    }
}

// AirToGroundHover (0x413470): the flyer hovers on the near side of its
// target, two thirds of its range out, stepping 45 degrees either side of the
// bearing to it in turn.
//   head:    as above; off the map, 800 px toward the playable map's centre,
//            radius 128, waiting on it (0x41357d..0x413666,
//            retailTaAirReturnToMap).
//   stage 0: (0x4136e5) as AirToGround's.
//   stage 1: (0x4137a7) every slot automatic; the approach to the target's
//            position; wait 0x100e8. 1.
//   stage 2: (0x4138a0) slot 0 manual and given the target; a point
//            controller on the target's position, radius the range; wait
//            0x100e8; the side (+0x36) and the miss count (+0x3a) 0. 1.
//   stage 3: (0x413902) slot 0 out of reach of the target (0x49abb0(unit,
//            target, 0)) adds a miss. At two misses (0x41391e..0x41399e) the
//            count goes back to 0 and a point controller rand(0x10000) round
//            the target at the range, radius 0x80, is built but never
//            installed (no 0x4388d0: it leaks), so the flyer keeps the one it
//            has; wait 0x110e8 (TA's 0x1000 is not posted here), 2.
//            Otherwise (0x4139ad..0x413a83) the hold: the target controller
//            (0x44e330: a fixed point, the flyer headed at the target) at the
//            target plus (sin, cos)(bearing 0x48a980(unit, target) + 0x2000
//            when +0x36 is set, else - 0x2000) x 2 range / 3, +0x36 flipped,
//            radius 0x10 (0x44e730), at cruisealt (0x44e6c0); wait 0x100e8.
//            Then under 3/4 health a repair pad takes it to VTOL_LANDING ahead
//            (0x413a8a..0x413b7c, 0, as AirToGround's stage 4); else 2.
// Host: random(bound); seek(target, point); beginFlight(); slotsAuto(); aim()
// (0x4898b0(0), 0x48a060); install(goal) (a point controller); hold(goal)
// (the target controller); inReach() (0x49abb0); padDamaged() and
// padDetour(). `point` is mission +0x22 (read by the seek only), `side`
// +0x36, `misses` +0x3a.
template<class Host>
int retailTaAirToGroundHover(RetailMissionState& m,RetailFlightVector point,int32_t& side,int32_t& misses,
                             uint32_t tick,uint32_t events,const RetailTaAirAttack& in,Host& host) {
    (void)tick;
    if (detail::retailTaAirAttackHead(m,events,0x28,in,point,host)) return 5;      // 0x413477..0x41357a
    if (!in.onMap) return retailTaAirReturnToMap(m,in.position,in.width,in.height,host);   // 0x41357d
    switch (m.stage) {
    case 0:                                                                          // 0x4136e5
        if (!in.flies) return 7;
        if (host.beginFlight()) m.waitMask|=0x700;                                   // 0x413790
        return 1;
    case 1:                                                                          // 0x4137a7
        host.slotsAuto();
        host.install(detail::retailTaAirApproach(in.position,in.targetPoint,host));
        m.waitMask=0x728;                                                            // 0x41388a
        return 1;
    case 2:                                                                          // 0x4138a0
        host.aim();
        host.install(RetailFlightGoal{in.targetPoint,0x30,0,int16_t(in.range)});
        m.waitMask=0x728;
        side=0;misses=0;                                                             // 0x4138ed
        return 1;
    case 3: {                                                                        // 0x413902
        if (!host.inReach()) ++misses;
        if (misses>=2) {                                                             // 0x413914
            misses=0;
            host.random(0x10000);                                                    // 0x413929: the leaked controller's angle
            m.waitMask=0x728;                                                        // 0x413996
            return 2;
        }
        const uint16_t bearing=uint16_t(retailDirection(Fixed::raw(satSub32(in.position.x,in.targetPoint.x)),
                                                        Fixed::raw(satSub32(in.position.z,in.targetPoint.z))).v);
        uint16_t angle;
        if (side) { angle=uint16_t(bearing+0x2000);side=0; }                         // 0x4139c2
        else { angle=uint16_t(bearing-0x2000);side=1; }                              // 0x4139d0
        const int32_t length=std::bit_cast<int32_t>(uint32_t(in.range*2/3)<<16);     // 0x4139e0..0x4139f7
        host.hold(RetailFlightGoal{{satAdd32(in.targetPoint.x,retailScaledSine(angle,length)),in.targetPoint.y,
                                    satAdd32(in.targetPoint.z,retailScaledCosine(angle,length))},0x59,0,0x10});
        m.waitMask=0x728;                                                            // 0x413a83
        if (host.padDamaged() && host.padDetour()) { m.waitMask=0; return 0; }      // 0x413a8a..0x413b7c
        return 2;
    }
    default:
        return 7;
    }
}

} // namespace ta::sim
