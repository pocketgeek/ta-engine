#pragma once

#include "sim/retailairattack.h"
#include "sim/retailairtoair.h"
#include "sim/retailairtraffic.h"
#include "sim/retailflight.h"
#include "sim/retailmission.h"
#include <cstdint>

namespace ta::sim {

// What AirToGround (41bf80), AirToAir (41ca50) and AirToGroundHover (41d820)
// do when they lose their target: AirToGround's head sends events 0x2a and a
// target that is gone, dead, dying (+0x130 0x1000), carried (+0x114 bit 4),
// off the map (+0x130 & 3 == 0) or now able to fly to 41c8f6; AirToAir's sends
// events 0x28 and the same states to 41d654; AirToGroundHover's sends events
// 0x28, the same states (bar flying) and a selected weapon with `dropped` to
// 41e28f. All three then return 5 (the attack ends), first pushing
// VTOL_SEEKATTACK (4d77b0: inserted at the end of the list, so right behind
// the attack) when:
//   41c8f6: the unit is alive (+0x130 0x1000000) and not dying (0x1000);
//           AirToAir's 41d654 and AirToGroundHover's 41e28f do not look;
//   all:    the attack is the last mission (+0x66, the list's next link, is
//           0), and the unit is not on Hold Fire (+0x130 & 0xc0000).
// The seek carries the attack's point (mission +0x22: the target's position
// at AirToGround's last wake, 41c13c; the led SweetSpot at AirToAir's, 51aa50,
// and at AirToGroundHover's stages 2 and 3) and, when the target is still
// alive and not dying, the target (+0x16).
struct RetailAirLoss {
    bool airToAir=false;      // 41d654 or 41e28f rather than 41c8f6
    bool alive=true;          // unit +0x130 0x1000000
    bool dying=false;         // unit +0x130 0x1000
    bool last=true;           // mission +0x66 == 0
    bool holdFire=false;      // unit +0x130 & 0xc0000 == 0
    bool target=false;        // mission +0x16 is set
    bool targetLive=false;    // ...to a unit alive and not dying
};
struct RetailSeekPush {
    bool push=false;          // VTOL_SEEKATTACK goes in behind the attack
    bool target=false;        // ...carrying the attack's target
};
inline RetailSeekPush retailAirLostTarget(const RetailAirLoss& in) {
    if (!in.airToAir && (!in.alive || in.dying)) return {};        // 41c8f6..41c90b
    if (!in.last || in.holdFire) return {};                         // 41c911 / 41d654..41d665
    return {true,in.target && in.targetLive};                       // 41c928 / 41d66b
}

// VTOL_SeekAttack (registry row 5ebe7c, class 19, flags 0x1000600; handler
// 4192f0): a flyer that lost its target circles the target's last point,
// looking for another.
//   head:    no weapon (+0x130 0x8000000) ends the list (7); event 0x200 (the
//            controller failed) ends the mission (5); off the map, 800 px
//            toward the playable area (192 px inside its edge), radius 128,
//            waiting on the controller (as AirToGround's stage 7).
//   stage 0: a seek carrying a target first offers it 4d7de0(unit, target,
//            0, 0, 0, 0): an attack put ahead clears the wait mask and
//            returns 0; otherwise straight on to stage 1. Without a target, a
//            zero point becomes the flyer's own, the orbit angle (mission
//            +0x4e) is rand(0x10000) (+0x52 its low bit, which no stage reads)
//            and 416c50 takes off.
//   stage 1: every slot automatic (519b10(3)); the target scan (4d8370, only
//            on Fire At Will) and 4d7de0(unit, found, 0, 0, 1, 0): an attack
//            put ahead ends the seek (5). Otherwise, woken by its controller
//            (0x100 arrival, 0x400 release), the angle steps back by 0x5555 +
//            rand(0x2000) (120 to 165 degrees); a point controller (flags
//            0x30, radius 128) at the point less (sin, cos)(angle) x (the
//            selected weapon's range + 160), a sleep of 30 + rand(30) ticks,
//            and a wait on the controller (0x700).
//   head, past stage 0 (41943a): with the air-traffic counter (unit +0x12a)
//            at 5 or more, VTOL_StepOut goes ahead (D = (rand(15) + footX) x
//            16) and the seek starts again at stage 0 a tick after it ends.
// Past the air-traffic check, the group check (4194f9..41960f,
// retailAirGroupCheck: 51d1e0(unit, 1, 5), VTOL_Move_Formation at level -4
// ahead), as AirToGround's. Every draw is 535cc0, the shared game random
// number generator.
struct RetailAirSeek {
    RetailFlightVector position;   // unit +0x68
    int32_t range=0;               // the selected slot's WeaponType +0x90 (519ac0)
    bool armed=true;               // unit +0x130 0x8000000
    bool flies=true;               // a mover (unit +8) and canfly (type +0x260 0x800)
    bool target=false;             // mission +0x16 is set
    bool onMap=true;               // unit +0xa4 is a sector, not the off-map cell
    int32_t width=0,height=0;      // game +0x19e90 / +0x19e94
    int8_t traffic=0;              // unit +0x12a, the air-traffic counter
    int16_t footX=1;               // unit +0x78
};

// Host: random(bound); attack() (stage 0's 4d7de0 on the carried target:
// true when it put an attack ahead); beginFlight() (416c50); slotsAuto()
// (519b10(3)); retarget() (4d8370 then 4d7de0(unit, found, 0, 0, 1, 0): true
// when it put an attack ahead); install(point goal) (4d4d40, which also
// clears the mission's pending controller events 0x3700); stepOut(D)
// (VTOL_StepOut pushed ahead, 4d7750: false when it could not be built); the
// group check's outOfSlot(), nextFlags(), leave() and formation(), as
// AirToGround's. `point` is mission +0x22, `angle` the low word of +0x4e.
template<class Host>
int retailVtolSeekAttack(RetailMissionState& m,RetailFlightVector& point,uint16_t& angle,uint32_t tick,
                         uint32_t events,const RetailAirSeek& in,Host& host) {
    if (!in.armed) return 7;                                         // 419301
    if (events&0x200) return 5;                                      // 419310
    if (!in.onMap) return detail::retailAirReturnToMap(m,in.position,in.width,in.height,host);   // 419323
    if (m.stage>0 && in.traffic>=5) {                                // 41943a
        const int32_t distance=(int32_t(host.random(15))+in.footX)*16;
        if (!host.stepOut(distance)) return 8;
        m.sleep(tick,1);
        return 0;
    }
    if (const int result=retailAirGroupCheck(m,host);result>=0) return result;   // 4194f9..41960f
    switch (m.stage) {
    case 0:                                                          // 41972e
        if (!in.flies) return 7;
        if (in.target) {
            if (!host.attack()) return 1;
            m.waitMask=0;
            return 0;
        }
        if (!point.x && !point.z && !point.y) point=in.position;     // 41977a
        angle=uint16_t(host.random(0x10000));                        // 4197a4
        host.beginFlight();
        return 1;
    case 1: {                                                        // 419620
        host.slotsAuto();
        if (host.retarget()) return 5;
        if (events&0x700) angle=uint16_t(angle-0x5555u-host.random(0x2000));   // 419654
        host.install(RetailFlightGoal{retailAirRunPoint(point,point.y,angle,uint32_t(in.range+0xa0)),0x30,0,128});
        m.sleep(tick,host.random(30)+30);                            // 419705
        m.waitMask|=0x700;
        return 2;
    }
    default:
        return 7;
    }
}

// TotalA.exe's off-map return of VTOL_Follow (0x40fc07..0x40fcda) and
// VTOL_SeekGuard (0x41086f..0x41094a): when the flyer's sector (unit +0x82)
// is the off-map cell (game +0x142b7), a point 800 px (0x3200000) from the
// flyer toward the centre of the playable map, (game +0x1422b / 2, game
// +0x1422f / 2) = ((W - 32) / 2, (H - 128) / 2) px (docs/retail-ai.md): the
// flyer less (sin, cos)(0x48a980(unit, centre), 0x4b715a's atan2 of unit -
// centre) x 800 (0x4103a0, added by 0x40f790); a point controller (0x44e2d0)
// of radius 128 (0x44e730(0x80): flag 0x10). `width` / `height` are the
// playable size, W - 32 / H - 128 px. (Kingdoms aimed at the nearest point
// 192 px inside the edge instead.)
inline RetailFlightGoal retailTaAirReturnToCentre(RetailFlightVector position,int32_t width,int32_t height) {
    const int32_t cx=std::bit_cast<int32_t>(uint32_t(width/2)<<16);
    const int32_t cz=std::bit_cast<int32_t>(uint32_t(height/2)<<16);
    const uint16_t heading=uint16_t(retailDirection(Fixed::raw(satSub32(position.x,cx)),
                                                    Fixed::raw(satSub32(position.z,cz))).v);
    return RetailFlightGoal{retailAirRunPoint(position,position.y,heading,800),0x30,0,128};
}

// VTOL_SeekGuard (TotalA.exe air mission row 19 of 0x4fca18, flags 0x600;
// handler 0x410850): a flyer whose guarded unit was lost circles where that
// unit last stood, looking for another to guard. VTOL_Follow appends it
// (0x43ad10, 0x410341..0x410384) when its ward is gone or its events 0x48
// woke it, and the guard is the last mission (+0x4a, the queue's next link,
// is 0); it carries the ward and the point the guard last read (+0x22, the
// ward's position, refreshed each look).
//   head:    event 0x40 (TA's controller failure; ours 0x200) ends it (5,
//            0x41085a); off the map, the return toward the map's centre
//            (retailTaAirReturnToCentre), a wait on the controller (TA 0xe0,
//            ours 0x700) and 2 (0x41086f..0x410956). No air-traffic or group
//            check: TA has neither (no VTOL_StepOut or formation row in
//            0x4fca18). (Kingdoms' 4197d0 had both.)
//   stage 0: no mover or not canfly ends the list (7, 0x410bfb); a zero
//            point becomes the flyer's own (0x410c11); the orbit angle
//            (+0x36) is rand(0x10000) (+0x3a its low bit, which no stage
//            reads); on to stage 1 (0x410c3a).
//   stage 1: (0x410972..0x410a4b, not ported: under 3/4 of maxdamage --
//            unit +0x108 < (type +0x1fa >> 2) x 3 -- a repair pad within
//            0xf00 px (0x40b530), a random one, takes it to VTOL_LANDING
//            pushed ahead and the stage back to 0.) The units 0x47e890 finds
//            within the type's sightdistance (+0x202) of the flyer that
//            0x410c70 keeps (allied to the flyer's owner, not canfly, not
//            itself), in its 128 px sector walk's order. Any found: the
//            controller goes (0x4388d0(0)) and the first is offered a guard
//            (0x43f0e0 action 7, 0x43f4c7: VTOL_FOLLOW for a canguard type,
//            +0x245 0x20); it goes ahead (0x43acb0), the wait mask is cleared
//            and 3 returns (the dispatcher's 30 + rand(15) sleep, 0x43b890:
//            the seek waits behind it) (0x410abf..0x410b2e). Nothing found:
//            woken by its controller (0xe0) the angle steps back 0x4000 +
//            rand(0x2000) (0x410b31); a point controller of radius 128
//            (0x44e730(0x80)) at the point less (sin, cos)(angle) x (slot
//            0's weapon range, unit +0x10 -> +0xdc, + 160) (0x410b4f..
//            0x410bc8), a sleep of 30 (0x439e80(0x1e); Kingdoms 7 + rand(7))
//            and a wait on 0xf8 (ours 0x718) (0x410bcd..0x410bdc); 2.
// Every draw is 0x4b6c30, the shared game random number generator.
struct RetailAirSeekGuard {
    RetailFlightVector position;   // unit +0x6a
    int32_t range=0;               // slot 0's weapon range (unit +0x10 -> +0xdc)
    bool armed=false;              // unit +0x110 0x80000000; unarmed: range 0
    bool flies=true;               // a mover (unit +0) and canfly (type +0x241 0x800)
    bool onMap=true;               // unit +0x82 is a sector, not the off-map cell
    int32_t width=0,height=0;      // game +0x1422b / +0x1422f: W - 32 / H - 128 px
};

// Host: random(bound); found() (0x47e890 with 0x410c70: true when it found
// any); release() (0x4388d0(0): the mission's controller goes, its pending
// controller events 0x3700 cleared); guard() (0x43f0e0 action 7 on the first
// found, then the mission built and pushed ahead: false when there was no
// mission for the pair, see World::tickSeekGuard); install(point goal)
// (0x4388d0, which also clears the pending controller events). `point` is
// mission +0x22, `angle` the low word of +0x36.
template<class Host>
int retailVtolSeekGuard(RetailMissionState& m,RetailFlightVector& point,uint16_t& angle,uint32_t tick,
                        uint32_t events,const RetailAirSeekGuard& in,Host& host) {
    if (events&0x200) return 5;                                      // 0x41085a
    if (!in.onMap) {                                                 // 0x41086f
        host.install(retailTaAirReturnToCentre(in.position,in.width,in.height));
        m.waitMask|=0x700;
        return 2;
    }
    switch (m.stage) {
    case 0:                                                          // 0x410bfb
        if (!in.flies) return 7;
        if (!point.x && !point.z && !point.y) point=in.position;     // 0x410c11
        angle=uint16_t(host.random(0x10000));                        // 0x410c3a
        return 1;
    case 1:                                                          // 0x410972
        if (host.found()) {                                          // 0x410a9a
            host.release();                                          // 0x410ac3
            if (!host.guard()) return 8;                             // 0x410ad9
            m.waitMask=0;                                            // 0x410b16
            return 3;
        }
        if (events&0x700) angle=uint16_t(angle-0x4000u-host.random(0x2000));   // 0x410b31
        host.install(RetailFlightGoal{retailAirRunPoint(point,point.y,angle,
            uint32_t(in.armed ? in.range+0xa0 : 0xa0)),0x30,0,128}); // 0x410b4f..0x410bc8
        m.sleep(tick,30);                                            // 0x410bcd
        m.waitMask|=0x718;                                           // 0x410bd6
        return 2;
    default:
        return 7;
    }
}

} // namespace ta::sim
