#pragma once

#include "sim/retailconstruction.h"
#include "sim/retailflight.h"
#include "sim/retailgroup.h"
#include "sim/retailmission.h"
#include "sim/retailmotion.h"
#include <bit>
#include <cmath>
#include <cstdint>

namespace ta::sim {

// AirToGround (mission registry row 5ebd9b, handler 41bf80): the attack
// mission 4de530 picks for a flyer without hoverattack whose target cannot
// fly. A flying target gets AirToAir (41ca50), hoverattack or not; a
// hoverattack flyer whose target cannot fly gets AirToGroundHover (41d820);
// and a selected weapon with `dropped` AirStrike
// (41b5e0); no shipped weapon sets `dropped`. The flyer does not stop to
// shoot: it flies attack runs over the target and its weapons fire whenever
// the weapon update (52ae90) finds the target in range.
//
//   stage 3: a point controller (4e40e0, flags 0x30) at the target, radius
//            attackrunlength/2, or half the selected weapon's range when the
//            unit has none; it is re-aimed at the target every 16 ticks.
//   stage 5: on arrival, the run: a point beyond the target, along the line
//            from the flyer through it, attackrunlength + 128 + rand(128) px
//            past it (3 x range without one), radius 128 + rand(128). Until
//            the flyer has once been within range/2 of the target it re-aims
//            the run every 15 ticks.
//   stage 6: on arrival, the turn: rand(2) picks 90 degrees left or right of
//            the body's heading, rand(128) + attackrunlength/2 px out (range
//            without one), radius 64 + rand(128).
//   stage 7: on arrival, back to stage 2 and the approach. Off the map it
//            adds one to mission +0x4e (1 once a run has come within
//            range/2, reset to 1 by a wake on the map); once that reaches 3
//            it first flies 800 px back toward the map, to 192 px inside the
//            playable edge.
//
//   stage 4 (41c3eb): with the air-traffic counter (unit +0x12a) at 5 or
//            more, an auto attack (+0x52) first rescans on rand(2) == 0
//            (4d8370, 4d7de0) and ends if it put a new attack ahead; then
//            VTOL_StepOut goes ahead (D = (rand(10) x 3 + footX) x 16), and
//            the run starts again at stage 1 a tick after it ends.
//
// Every draw is 535cc0, the shared game random number generator. The host
// has made the head's checks (a live target that cannot fly, the leash);
// the group check after them (41c19a, retailAttackGroupCheck) is the
// hover's: VTOL_Move_Formation ahead of a member out of its slot.
struct RetailAirRun {
    RetailFlightVector position;   // unit +0x68
    RetailFlightVector target;     // mission +0x22, the target's position at this wake
    uint16_t heading = 0;          // unit +0x7e
    int32_t range = 0;             // the selected slot's WeaponType +0x90
    uint16_t runLength = 0;        // UnitType +0x234, attackrunlength
    bool onMap = true;             // unit +0xa4 is a sector, not the off-map cell (game +0x19f30)
    int32_t width = 0, height = 0; // game +0x19e90 / +0x19e94: the playable map, in pixels
    int8_t traffic = 0;            // unit +0x12a, the air-traffic counter
    int16_t footX = 1;             // unit +0x78
    bool automatic = false;        // mission +0x52, 4d7de0's auto argument
    bool hasTarget = true;         // mission +0x16 is set
};

// 41c5b1..41c5c9 and its relatives: `distance` px from `from` along -heading.
inline RetailFlightVector retailAirRunPoint(RetailFlightVector from, int32_t y, uint16_t heading, uint32_t distance) {
    const int32_t magnitude=std::bit_cast<int32_t>(distance<<16);
    return {satSub32(from.x,retailScaledSine(heading,magnitude)),y,
            satSub32(from.z,retailScaledCosine(heading,magnitude))};
}

// The flyer attacks' group check (AirToGround 41c19a, AirStrike 41b7a4,
// AirToGroundHover 41dafa), after their exits and the leash:
// retailAttackGroupCheck at level 5, leave on rand(2), clear on rand(25).
// Host: outOfSlot() (51d1e0(unit, 1, 5)); nextFlags() (the next mission's
// +0x5a, 0 for none); leave() (50c480); formation() (VTOL_Move_Formation,
// level -4, pushed ahead: false when it could not be built).
template<class Host>
int retailAirGroupCheck(RetailMissionState& m,Host& host) {
    return retailAttackGroupCheck(m.flags,host.outOfSlot(),host.nextFlags(),2,25,
        [&](int n) { return host.random(n); },[&] { host.leave(); },[&] { return host.formation(); });
}

// Host: random(bound); beginFlight() (416c50); slotsAuto() (519b10(3));
// aimSlot() (519b70 and 51a700 on the selected slot); install(goal) (4d4d40,
// which also clears the mission's pending controller events 0x3700);
// retarget() (4d8370, then 4d7de0(unit, found, 0, 0, 1, 0) for a found unit
// that is not the target: true when it put an attack ahead); stepOut(D)
// (VTOL_StepOut built and pushed ahead, 4d7750: false when it could not be);
// the group check's outOfSlot(), nextFlags(), leave() and formation(), as
// AirToGroundHover's.
template<class Host>
int retailAirToGround(RetailMissionState& m, int32_t& count, uint32_t tick, uint32_t events,
                      const RetailAirRun& run, Host& host) {
    if (events&0x2a) return 5;                               // 41c00c -> 41c8f6
    if (events&0x40) { m.sleep(tick,1); return 0; }          // 41c122
    if (const int result=retailAirGroupCheck(m,host);result>=0) return result;   // 41c19a..41c2ba
    auto install=[&](RetailFlightVector point,int radius) {
        host.install(RetailFlightGoal{point,0x30,0,int16_t(radius)});
    };
    switch (m.stage) {
    case 0:                                                  // 41c2cf
        host.beginFlight();
        return 1;
    case 1:                                                  // 41c308
        host.slotsAuto();
        return 1;
    case 2:                                                  // 41c31f
        host.aimSlot();
        return 1;
    case 3:                                                  // 41c37a
        install(run.target,run.runLength ? run.runLength>>1 : run.range/2);
        m.waitMask=0x76a;
        m.sleep(tick,15);
        return 1;
    case 4:                                                  // 41c3eb
        if (events&0x700) return 1;
        if (run.traffic>=5) {                                // 41c401
            if (run.automatic && run.hasTarget && host.random(2)==0 && host.retarget()) return 5;
            const int32_t first=int32_t(host.random(10)),second=int32_t(host.random(10));
            const int32_t distance=(first+second+int32_t(host.random(10))+run.footX)*16;
            if (!host.stepOut(distance)) return 8;
            m.waitMask=0;
            m.stage=1;
            m.sleep(tick,1);
            return 4;
        }
        m.sleep(tick,1);
        m.stage=3;
        return 4;
    case 5: {                                                // 41c531
        const int32_t dx=satSub32(run.position.x,run.target.x);
        const int32_t dz=satSub32(run.position.z,run.target.z);
        const uint16_t heading=uint16_t(retailDirection(Fixed::raw(dx),Fixed::raw(dz)).v);
        const int radius=int(host.random(128))+128;
        const uint32_t distance=run.runLength ? host.random(128)+run.runLength+128u : uint32_t(run.range)*3u;
        install(retailAirRunPoint(run.target,run.target.y,heading,distance),radius);
        m.waitMask=0x76a;
        if (count>0) return 1;
        // x87 at double precision: dx*dx + dz*dz, fsqrt, _ftol.
        const int32_t reach=truncSat32(std::sqrt(double(dx)*dx+double(dz)*dz));
        if (reach>std::bit_cast<int32_t>(uint32_t(run.range/2)<<16)) { m.sleep(tick,15); return 2; }
        count=1;
        return 1;
    }
    case 6: {                                                // 41c6a7
        const uint16_t heading=uint16_t(run.heading+(host.random(2) ? 0x4000 : -0x4000));
        const uint32_t distance=run.runLength ? host.random(128)+(run.runLength>>1) : uint32_t(run.range);
        const auto point=retailAirRunPoint(run.position,run.position.y,heading,distance);
        install(point,int(host.random(128))+64);
        m.waitMask=0x76a;
        return 1;
    }
    case 7: {                                                // 41c7ae
        if (run.onMap) { count=1; m.stage=2; return 2; }
        if (++count<3) { m.stage=2; return 2; }
        RetailFlightVector inside=run.position;
        auto clamp=[](int32_t& v,int32_t size) {
            if (v<0xc00000) v=0xc00000;
            else if (const int32_t edge=std::bit_cast<int32_t>(uint32_t(size+0xff40)<<16);v>edge) v=edge;
        };
        clamp(inside.x,run.width);clamp(inside.z,run.height);
        // 51b340(unit, point): the heading of the unit as seen from the point.
        const uint16_t heading=uint16_t(retailDirection(
            Fixed::raw(satSub32(run.position.x,inside.x)),
            Fixed::raw(satSub32(run.position.z,inside.z))).v);
        install(retailAirRunPoint(run.position,run.position.y,heading,800),128);
        m.waitMask|=0x700;
        return 2;
    }
    default:
        return 7;
    }
}

// The point 800 px from `position` toward 192 px inside the playable edge
// (game +0x19e90/+0x19e94 less 0xc0), the return of every flyer mission's
// off-map branch (4214c8, 41d98b, 41c7e4, ...).
inline RetailFlightVector retailOffMapReturn(RetailFlightVector position,int32_t width,int32_t height) {
    RetailFlightVector inside=position;
    auto clamp=[](int32_t& v,int32_t size) {
        if (v<0xc00000) v=0xc00000;
        else if (const int32_t edge=std::bit_cast<int32_t>(uint32_t(size+0xff40)<<16);v>edge) v=edge;
    };
    clamp(inside.x,width);clamp(inside.z,height);
    // 51b340(unit, point): the heading of the unit as seen from the point.
    const uint16_t heading=uint16_t(retailDirection(
        Fixed::raw(satSub32(position.x,inside.x)),
        Fixed::raw(satSub32(position.z,inside.z))).v);
    return retailAirRunPoint(position,position.y,heading,800);
}

// AirToGroundHover (mission registry row 5ebdb4, handler 41d820): the attack
// mission 4de530 gives a flyer with `hoverattack` (UnitType +0x260 bit
// 0x8000000) whose target cannot fly. It hovers on a ring round the target
// and never fires itself: the weapon update (52ae90) fires the selected slot
// whenever its target is in range.
//
//   head:    events 0x28, or a target gone, dead, dying, carried or off the
//            map, or a selected weapon with `dropped`: the lost-target exit
//            (41e28f, the host's lostTarget()); off the map, 800 px back toward the
//            playable area, radius 128, waiting on the controller (0x700);
//            event 0x40 restarts after a tick; the leash (+0x56, the host's);
//            the group check 51d1e0(unit, 1, 5): out of its formation it may
//            leave on rand(2) == 0 once flagged (0x8000000; return 8, first
//            50c480 when the next mission does not move with the group), and
//            otherwise flags itself and pushes VTOL_Move_Formation (level
//            -4) ahead (return 0); back in, rand(25) == 0 clears the flag.
//   stage 0: 416c50 (Activate, BeginFlight, take-off); mission +0x4e = 0; the
//            target's position into +0x22.
//   stage 1: the target's position again; every slot automatic; farther than
//            D + 160 px from it (whole pixels, x87 and _ftol), a point
//            controller (flags 0x30) on it of radius D + 144, wait mask
//            0x768 and a 15-tick sleep.
//   stage 2: the selected slot manual and aimed at the target (519b70,
//            51a700), its led SweetSpot (51aa50) into +0x22.
//   stage 3: the led SweetSpot again; the ring point (41de5d: the flyer's
//            bearing from it, now and then turned by up to 2048, at the
//            current distance within 16 px of D, else D - 3 + rand(8)); the
//            target controller there (4e4140: fixed point, radius 4, the
//            attack altitude), wait mask 0xe70768 and a sleep of 15 + rand(15).
//   stage 4: with the air-traffic counter (unit +0x12a) at 5 or more: one
//            more step-out (+0x4e); an auto attack (+0x52) rescans on
//            rand(2) == 0 (4d8370, 4d7de0) and ends if it put a new attack
//            ahead; VTOL_StepOut goes ahead (D = (rand(10) x 3 + footX) x 16);
//            past rand(5) + rand(5) + 1 step-outs the attack ends (8),
//            otherwise it starts again at stage 1 a tick after the step-out.
//            Not congested: a shot that hurt nobody but its own side (events
//            0xe00000, 4099e0) on rand(2) == 0 against a target without a
//            mover or standing still (+0x130 & 0xc) goes to stage 5;
//            otherwise back to stage 3 after a tick.
//   stage 5: the target controller at a random point of the ring (rand(8) + D
//            - 3 px from +0x22 at angle rand(0x10000)), wait mask 0x768, then
//            stage 3 on its arrival (0x100) or release.
// D and the altitude are the selected weapon's hoverattackdistance and
// hoverattackaltitude (+0xc2, +0xc4), else the type's (+0x256, +0x258).
// Every draw is 535cc0, the shared game random number generator.
struct RetailAirHover {
    RetailFlightVector position;   // unit +0x68
    int32_t distance=0;            // D
    bool flies=true;               // a mover (unit +8) and canfly (type +0x260 0x800)
    bool target=true;              // mission +0x16 is set
    bool targetStill=false;        // ... to a unit without a mover, or one whose +0x130 & 0xc is 0
    bool automatic=false;          // mission +0x52, 4d7de0's auto argument
    int8_t traffic=0;              // unit +0x12a, the air-traffic counter
    int16_t footX=1;               // unit +0x78
    bool onMap=true;               // unit +0xa4 is a sector, not the off-map cell (game +0x19f30)
    int32_t width=0,height=0;      // game +0x19e90 / +0x19e94: the playable map, in pixels
};

// Host: random(bound); lostTarget() (the exit 41e28f: it may push
// VTOL_SEEKATTACK and answers 5); the group check's (retailAirGroupCheck)
// outOfSlot(), nextFlags(), leave() and formation(); beginFlight() (416c50); slotsAuto() (519b10(3));
// aim() (519b70 and 51a700 on the selected slot, 51a770 without a target);
// targetPoint() (the target's +0x68); lead() (51aa50: the selected slot's led
// SweetSpot); retarget() (4d8370 and 4d7de0(unit, found, 0, 0, 1, 0) for a
// found unit that is not the target: true when it put an attack ahead);
// stepOut(D) (VTOL_StepOut pushed ahead, 4d7750: false when it could not be
// built); approach(goal) (a point controller) and ring(goal) (the target
// controller), both 4d4d40, which clears the mission's pending 0x3700.
// `point` is mission +0x22, `count` mission +0x4e.
template<class Host>
int retailAirToGroundHover(RetailMissionState& m,int32_t& count,RetailFlightVector& point,uint32_t tick,
                           uint32_t events,const RetailAirHover& in,Host& host) {
    if (events&0x28) return host.lostTarget();                           // 41d892 -> 41e28f
    if (!in.onMap) {                                                     // 41d98b
        host.approach(RetailFlightGoal{retailOffMapReturn(in.position,in.width,in.height),0x30,0,128});
        m.waitMask|=0x700;
        return 2;
    }
    if (events&0x40) { m.sleep(tick,1); return 0; }                      // 41da9d
    if (const int result=retailAirGroupCheck(m,host);result>=0) return result;   // 41dafa..41dc1b
    const int32_t d=in.distance;
    auto whole=[&](RetailFlightVector p) {
        const double dx=double(satSub32(p.x,in.position.x));
        const double dz=double(satSub32(p.z,in.position.z));
        // Retail keeps the signed low word (movsx), which turns negative at
        // 32768 px; no retail map has such a distance. Ours: the whole word.
        return int(retailFtol(std::sqrt(dx*dx+dz*dz))>>16);
    };
    switch (m.stage) {
    case 0:                                                              // 41dca6
        if (!in.flies) return 7;
        host.beginFlight();
        count=0;
        if (in.target) point=host.targetPoint();
        return 1;
    case 1:                                                              // 41dd03
        if (in.target) point=host.targetPoint();
        host.slotsAuto();
        if (whole(point)<=d+160) return 1;
        host.approach(RetailFlightGoal{point,0x30,0,int16_t(d+144)});
        m.waitMask=0x768;
        m.sleep(tick,15);
        return 2;
    case 2:                                                              // 41ddcd
        host.aim();
        if (in.target) point=host.lead();
        return 1;
    case 3: {                                                            // 41de3f
        if (in.target) point=host.lead();
        RetailFlightGoal goal=retailHoverAttackGoal(in.position,point,d,[&](int32_t n) { return host.random(n); });
        goal.flags|=0x10;goal.radius=4;   // 4e4540(4)
        host.ring(goal);
        m.waitMask=0xe70768;
        m.sleep(tick,15+host.random(15));
        return 1;
    }
    case 4:                                                              // 41e007
        if (in.traffic>=5) {
            ++count;
            if (in.automatic && host.random(2)==0 && host.retarget()) return 5;
            const int32_t first=int32_t(host.random(10)),second=int32_t(host.random(10));
            const int32_t distance=(first+second+int32_t(host.random(10))+in.footX)*16;
            if (!host.stepOut(distance)) return 8;
            m.waitMask=0;
            const int32_t limit=int32_t(host.random(5));
            if (count>limit+int32_t(host.random(5))+1) return 8;
            m.stage=1;
            m.sleep(tick,1);
            return 4;
        }
        if ((events&0xe00000) && host.random(2)==0 && in.target && in.targetStill) {   // 41e145
            m.sleep(tick,1);
            return 1;
        }
        m.sleep(tick,1);
        m.stage=3;
        return 4;
    case 5: {                                                            // 41e1a2
        const int32_t radius=std::abs(int32_t(host.random(8))+d-3);
        const uint16_t angle=uint16_t(host.random(0x10000));
        const int32_t length=std::bit_cast<int32_t>(uint32_t(radius)<<16);
        RetailFlightGoal goal{{satAdd32(point.x,retailScaledSine(angle,length)),point.y,
                               satAdd32(point.z,retailScaledCosine(angle,length))},
                              0x78,angle,4};
        host.ring(goal);
        m.waitMask=0x768;
        m.stage=3;
        return 4;
    }
    default:
        return 7;
    }
}

// TA: TotalA.exe's AirStrike is handler 0x411f50 (mission table 0x4fcc..,
// status "Airstrike"), the attack every shipped bomber's `dropped` weapon
// gets. It is the Kingdoms mission below with these differences, which
// retailAirStrike follows: no group check and no stage-4 air-traffic
// step-out; the leash is the _hypot of whole pixels; the release ring's
// gravity is the OTA's integer and its speed the mover's scalar speed
// (retailStrikeRelease); zero gravity ends the list at stage 4; stage 6 sends
// a bomber under 3/4 health to a repair pad (not ported). The Kingdoms
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
//            AirToGroundHover's (retailAirGroupCheck).
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
// the group check's outOfSlot(), nextFlags(), leave() and formation().
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
        // (0x4125c2..0x4126b4: under 3/4 health a repair pad within 3840 px
        // takes it to VTOL_LANDING instead; not ported.)
        host.clearSlot();
        host.install(RetailFlightGoal{retailAirRunPoint(in.position,in.position.y,in.heading,1440),0x30,0,128});
        m.waitMask=0x742;
        m.stage=3;
        return 2;
    default:
        return 7;
    }
}

} // namespace ta::sim
