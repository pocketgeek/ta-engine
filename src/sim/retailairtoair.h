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

// AirToAir (mission registry row 5ebd82, handler 41ca50): the attack mission
// 4de530 gives a flyer whose target's type can fly, unless the selected
// weapon has `dropped` (WeaponType +0xc8 bit 0x20, no shipped weapon sets it:
// a `subtype=Dropped` bomb does not count). hoverattack plays no part: a Tarbeak
// or a dragon dogfights a flyer too. The mission flies the attacker and never
// fires: the weapon update (52ae90) fires the selected slot whenever its
// target is in range, a Dropped bomb included (its in-range virtual is the
// base 2D test 530580, noairweapon/toairweapon apart).
//
//   stage 0: 416c50 (Activate, BeginFlight, take-off), sleep 1.
//   stage 1: every slot automatic, the selected one manual and aimed at the
//            target (519b10(3), 519b70, 51a700), sleep 1.
//   stage 2: every 7 + rand(15) ticks: with the selected slot's led
//            SweetSpot P (51aa50) and R = airtoairdistance (the selected
//            weapon's +0xc6, else the type's +0x25a; default
//            sqrt(hoverattackdistance^2 + hoverattackaltitude^2) with a
//            hoverattackdistance, else 100), it either pursues -- a point R
//            px short of P on the line from the flyer -- or, threatened,
//            repositions to a random point R +- R/2 px from P on its own
//            side, or breaks off into VTOL_Evade (41e3d0). Both points are
//            flown with the moving controller below, whose velocity starts as
//            the target's and turns toward the line from the flyer to P.
//
// Every draw is 535cc0, the shared game random number generator. The host has
// made the head's checks (the unit may attack, a live target, the leash at
// mission +0x56). Past the restart (0x40) comes the group check (41ccde,
// retailAirGroupCheck: 51d1e0(unit, 1, 5), VTOL_Move_Formation at level -4
// ahead), as AirToGround's; VTOL_Evade has the same one past its off-map
// return (41e55a), its next mission being the AirToAir behind it.
// Stage 2's air-traffic check (41ce48): with unit +0x12a at 5 or more, an auto
// attack first rescans on rand(2) == 0 (4d8370, 4d7de0) and ends if it put a
// new attack ahead; then VTOL_StepOut goes ahead with D = (rand(15) + the
// target's footX + its own) x 16, and the attack starts again at stage 0 a
// tick after it ends. Every abort returns 5; the VTOL_SEEKATTACK a lost
// target pushes first (41d654) is the host's (retailAirLostTarget,
// retailairseek.h).

// 41d780: whether `a` lies in the half plane ahead of `b` once b's facing is
// turned by octant x 45 degrees. Both directions are 20 px vectors (5360bf /
// 5360f3); the test multiplies their whole-pixel words in 16 bits.
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

// The controller AirToAir and VTOL_Evade fly (vtable 5f29a8, 0x2c bytes,
// built by 4e4560): a point that moves on by its own velocity each time the
// navigator asks for it. Its type (vtable +0x08 answers 3) makes the
// navigator fly it at cruise height at any distance (524b84), and it is never
// let go on arrival (+0x2c answers 1): the navigator posts 0x100 to its
// mission while the flyer is within 48 px.
struct RetailInterceptGoal {
    RetailFlightVector point;      // +0x0a
    RetailFlightVector velocity;   // +0x16, the target mover's velocity (+8) when built
    uint16_t heading=0;            // +0x24, from 4e4c60
    bool turns=false;              // +0x08 & 1, set with the heading

    // 4e48d0: answer the point, then move it on by the velocity (x and z
    // only) and, with a heading, turn the velocity toward that heading by
    // at most turnrate / 8 (536143), dropping its vertical part.
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
    // 4e4be0: within 48 px in 2D (x87 at double precision, times 1/65536).
    bool accepts(RetailFlightVector position) const {
        const double dx=double(satSub32(position.x,point.x));
        const double dz=double(satSub32(position.z,point.z));
        return std::sqrt(dz*dz+dx*dx)<48.0*65536;
    }
    // 4e4b70: the navigator's heading. Inside 48 px the stored heading (the
    // body's own without one); farther out, toward the point.
    uint16_t headingFor(RetailFlightVector position,uint16_t body) const {
        if (accepts(position)) return turns ? heading : body;
        return uint16_t(retailDirection(
            Fixed::raw(satSub32(position.x,point.x)),
            Fixed::raw(satSub32(position.z,point.z))).v);
    }
};

// What AirToAir reads at one wake.
struct RetailAirDuel {
    RetailFlightVector position;        // unit +0x68
    uint16_t heading=0;                 // unit +0x7e
    RetailFlightVector target;          // the target's +0x68
    uint16_t targetHeading=0;           // the target's +0x7e
    RetailFlightVector targetVelocity;  // the target mover's +8
    int32_t speed=0;                    // mover +0x20
    int32_t maxSpeed=0;                 // unit +0x12b (421ae0)
    uint16_t distance=0;                // airtoairdistance
    bool automatic=false;               // mission +0x52: an auto attack, which may retarget
    bool flies=true;                    // a mover and canfly (type +0x260 bit 0x800)
    bool onMap=true;                    // unit +0xa4 is a sector, not the off-map cell
    int32_t width=0,height=0;           // game +0x19e90 / +0x19e94
    int8_t traffic=0;                   // unit +0x12a, the air-traffic counter
    int16_t footX=1,targetFootX=1;      // unit +0x78, the target's +0x78
};

namespace detail {
// 41cb80..41cc6d, 41e45a..41e54c: off the map, 800 px toward the playable
// area (192 px inside its edge), radius 128, waiting on the controller.
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

// Host: random(bound); beginFlight() (416c50); aim() (stage 1's slot calls);
// lead() (51aa50: the selected slot's led SweetSpot, mission +0x22);
// retarget() (4d8370 then 4d7de0(unit, found, 0, 0, 1, 0): true when it put a
// new attack ahead); install(point goal) and install(intercept goal) (4d4d40,
// which also clears the mission's pending controller events 0x3700);
// release() (4d4d40 with no controller); evade() (pushes VTOL_Evade on the
// target, true when inserted); stepOut(D) (VTOL_StepOut pushed ahead, 4d7750:
// false when it could not be built); the group check's outOfSlot(),
// nextFlags(), leave() and formation(), as AirToGround's. `count` is mission
// +0x4e.
template<class Host>
int retailAirToAir(RetailMissionState& m,int32_t& count,uint32_t tick,uint32_t events,
                   const RetailAirDuel& duel,Host& host) {
    if (events&0x28) return 5;                                       // 41ca7e -> 41d654
    if (!duel.onMap) return detail::retailAirReturnToMap(m,duel.position,duel.width,duel.height,host);
    if (events&0x40) { m.sleep(tick,1); return 0; }                  // 41ccc4
    if (const int result=retailAirGroupCheck(m,host);result>=0) return result;   // 41ccde..41cdfe
    const uint32_t wake=host.random(15)+7;                           // 41cdfe
    switch (m.stage) {
    case 0:                                                          // 41d60b
        if (!duel.flies) return 7;
        host.beginFlight();
        m.sleep(tick,1);
        count=0;
        return 1;
    case 1:                                                          // 41d5ae
        host.aim();
        m.sleep(tick,1);
        return 1;
    case 2: break;                                                   // 41ce31
    default: return 7;
    }
    const RetailFlightVector lead=host.lead();                       // 41ce43
    if (duel.traffic>=5) {                                           // 41ce48
        if (duel.automatic && host.random(2)==0 && host.retarget()) return 5;
        const int32_t distance=(int32_t(host.random(15))+duel.targetFootX+duel.footX)*16;
        if (!host.stepOut(distance)) return 8;
        m.sleep(tick,1);
        return 0;
    }
    const RetailFlightVector& me=duel.position;
    const RetailFlightVector& them=duel.target;
    // 41cf43..41cffe: who is in front of whom.
    const bool ahead=retailAirAhead(them,me,duel.heading,0);
    const bool aheadLeft=retailAirAhead(them,me,duel.heading,2);
    const bool aheadRight=retailAirAhead(them,me,duel.heading,6);
    const bool sights=ahead && retailAirAhead(them,me,duel.heading,7) && retailAirAhead(them,me,duel.heading,1);
    const bool seen=retailAirAhead(me,them,duel.targetHeading,0);
    const bool seenLeft=retailAirAhead(me,them,duel.targetHeading,2);
    const bool seenRight=retailAirAhead(me,them,duel.targetHeading,6);
    const bool sighted=seen && retailAirAhead(me,them,duel.targetHeading,7) && retailAirAhead(me,them,duel.targetHeading,1);
    const bool tailed=!ahead && seen, hunted=!ahead && sighted, apart=!ahead && !seen;
    // 41d042..41d0f8: the whole-pixel distance to P (_ftol's low word) against R.
    const double dx=double(satSub32(me.x,lead.x));
    const double dz=double(satSub32(me.z,lead.z));
    // (Retail's signed word turns negative at 32768 px, farther than any retail
    // map is wide. Ours: the whole word.)
    const int span=int(retailFtol(std::sqrt(dz*dz+dx*dx))>>16);
    const int reach=duel.distance;
    const bool inHalf=span<reach/2, inQuarter=span<reach/4, beyond=span>3*reach/2;
    if (span>5*reach && duel.automatic && host.random(2)==0 && host.retarget()) return 5;   // 41d0fa
    // 41d132..41d176
    bool threatened;
    if (tailed) threatened=true;
    else if (sights) threatened=sighted;
    else threatened=inQuarter || (seen && inHalf) ||
                    (!beyond && ((aheadRight && seenRight) || (aheadLeft && seenLeft)));
    bool reposition=false;
    if (!threatened) {
        if (apart || ahead) count=0;                                 // 41d178
    } else {
        // 41d19f: a flyer in the target's sights, on its tail, or now and then
        // any other: count the wait and maybe break off.
        if ((hunted && host.random(2)==0) || (tailed && host.random(3)==0) || host.random(30)==0) {
            count+=int32_t(wake);                                    // 41d1d2
            const uint32_t first=host.random(60);
            const uint32_t limit=first+host.random(60);
            if (count>=int32_t(limit)) {
                host.release();
                if (!host.evade()) return 8;
                count=0;
                m.waitMask=0;
                return 0;
            }
        } else {
            if (!(sights && sighted) || host.random(5)==0) count+=int32_t(wake);   // 41d27a
            const uint32_t first=host.random(60);
            const uint32_t limit=first+host.random(60);
            reposition=count>=int32_t(limit);                         // 41d2c8
        }
    }
    // 41d2c8 / 41d435: P, 32 px further along the flyer's heading when it is
    // above half its top speed.
    RetailFlightVector point=lead;
    if (int32_t(int64_t(duel.speed)*2)>duel.maxSpeed) {
        point.x=satSub32(point.x,retailScaledSine(duel.heading,32*65536));
        point.z=satSub32(point.z,retailScaledCosine(duel.heading,32*65536));
    }
    uint32_t length=uint32_t(reach);
    if (reposition) {
        const uint32_t closer=host.random(reach/2);
        length=uint32_t(reach)-closer+host.random(reach/2);
    }
    // 51b340: the heading of P as seen from the flyer; the point is then
    // `length` px back toward the flyer, within 90 degrees either side of
    // that line when repositioning.
    uint16_t angle=uint16_t(retailDirection(
        Fixed::raw(satSub32(point.x,me.x)),
        Fixed::raw(satSub32(point.z,me.z))).v);
    if (reposition) angle=uint16_t(angle+host.random(0x8000)-0x4000);
    const int32_t magnitude=std::bit_cast<int32_t>(length<<16);
    point.x=satSub32(point.x,retailScaledSine(angle,magnitude));
    point.z=satSub32(point.z,retailScaledCosine(angle,magnitude));
    // 41d570: the controller turns toward the line from the flyer to P.
    RetailInterceptGoal goal{point,duel.targetVelocity,uint16_t(retailDirection(
        Fixed::raw(satSub32(me.x,lead.x)),
        Fixed::raw(satSub32(me.z,lead.z))).v),true};
    host.install(goal);
    m.sleep(tick,wake);
    m.waitMask|=0x68;
    return 2;
}

// VTOL_Evade (registry row 5ebe63, handler 41e3d0): pushed ahead of AirToAir
// when it breaks off, on the same target, with mission +0x4e (the maneuver)
// and +0x52 (its remaining ticks) zero. Stage 0 picks a maneuver, more often
// a hard turn on the spot (4..6) when the flyer is no faster or nimbler than
// its target, and 33..88 ticks less 15..43 when the flyer is twice as fast:
//   1..3: fly a straight line within 45 degrees of the heading (1), or 45 to
//         165 degrees to one side (2) or the other (3): a point controller
//         far out along it, radius 16.
//   4..6: the same three arcs as a heading for the moving controller held 32
//         px ahead with no velocity: brake and turn to it.
// Stage 1 repeats them, 5..33 ticks at a time (5..19 for 4..6), until the
// ticks run out (then rand(2) restarts or ends it) or the target is far
// enough: the sum of three rand(distance) draws above 3 x airtoairdistance.
struct RetailAirEvade {
    RetailMissionState mission;
    int32_t maneuver=0;            // mission +0x4e
    int32_t budget=0;              // mission +0x52
};

// What VTOL_Evade reads.
struct RetailAirEscape {
    RetailFlightVector position;   // unit +0x68
    uint16_t heading=0;            // unit +0x7e
    RetailFlightVector target;     // the target's +0x68
    int32_t maxSpeed=0;            // unit +0x12b (421ae0)
    int32_t typeSpeed=0;           // UnitType +0x162, maxvelocity
    int32_t targetTypeSpeed=0;     // the target's
    uint16_t turnRate=0;           // UnitType +0x18e
    uint16_t targetTurnRate=0;     // the target's
    uint16_t distance=0;           // airtoairdistance
    bool flies=true;               // a mover and canfly
    bool onMap=true;
    int32_t width=0,height=0;
};

// World's AirToAir state on an attack order, beside the order's `mission`
// (AirToAir's own) and `flightGoal` (a point controller: off the map, or one
// of VTOL_Evade's lines).
struct RetailAirToAirState {
    int32_t count=0;                              // AirToAir's mission +0x4e
    std::optional<RetailInterceptGoal> intercept; // the moving controller installed
    std::optional<RetailAirEvade> evade;          // VTOL_Evade, pushed ahead of AirToAir
    bool evadeController=false;                   // the controller is the evade's
};

// Host: random(bound); install(point goal) and install(intercept goal); the
// group check's outOfSlot(), nextFlags() (the AirToAir behind it), leave()
// and formation(). A flyer's mover never carries the road or water bits (mover +0x36 0x800 /
// 0x1000) that would scale the speeds and turn rates read here.
template<class Host>
int retailAirEvade(RetailAirEvade& e,uint32_t tick,uint32_t events,const RetailAirEscape& in,Host& host) {
    auto& m=e.mission;
    if (events&0x28) return 5;                                       // 41e404
    if (!in.onMap) return detail::retailAirReturnToMap(m,in.position,in.width,in.height,host);
    if (const int result=retailAirGroupCheck(m,host);result>=0) return result;   // 41e55a..41e67a
    auto any=[&] { return int32_t(host.random(6))+1; };
    auto straight=[&] { return int32_t(host.random(3))+1; };
    switch (m.stage) {
    case 0: {                                                        // 41eb12
        if (!in.flies) return 7;
        if (e.maneuver>=4) {                                         // 41ed7f
            if (host.random(4)!=0) e.maneuver=straight();
            else if (e.maneuver==4) e.maneuver=any();
        } else if (int32_t((int64_t(in.typeSpeed)*0xcccc)>>16)>in.targetTypeSpeed) {
            e.maneuver=host.random(5)==0 ? any() : straight();       // 41ec2d
        } else if (int32_t((int64_t(in.turnRate)*0xcccc)>>16)>int32_t(in.targetTurnRate)) {
            e.maneuver=any();                                        // 41ed56
            if (e.maneuver%3==1) e.maneuver=any();
        } else {
            e.maneuver=host.random(2)==0 ? any() : straight();       // 41ed70
        }
        int32_t budget=int32_t(host.random(30));                     // 41ed9d
        budget+=int32_t(host.random(30));
        budget+=int32_t(host.random(15));
        budget+=int32_t(host.random(15))+5;
        if (int32_t((int64_t(in.typeSpeed)*0x8000)>>16)>in.targetTypeSpeed) {
            int32_t less=int32_t(host.random(30));
            less+=int32_t(host.random(15));
            budget-=less;
        }
        if (budget<10) budget=int32_t(host.random(10))+3;
        e.budget=budget;
        return 1;
    }
    case 1: break;                                                   // 41e699
    default: return 7;
    }
    const double dz=double(satSub32(in.position.z,in.target.z));
    const double dx=double(satSub32(in.position.x,in.target.x));
    const int span=int(retailFtol(std::sqrt(dx*dx+dz*dz))>>16);   // the whole word, as AirToAir's
    int32_t gap=int32_t(host.random(span));
    gap+=int32_t(host.random(span));
    gap+=int32_t(host.random(span));
    if (gap>3*int32_t(in.distance)) return 5;
    if (e.budget<=0) return host.random(2)!=0 ? 0 : 5;              // 41e740
    int32_t sleep=e.budget;
    if (e.maneuver<4) {
        const uint32_t first=host.random(15);
        if (e.budget>=int32_t(first+host.random(15)+5)) {
            const uint32_t again=host.random(15);
            sleep=int32_t(again+host.random(15)+5);
        }
        // 41e7a8: budget x 100 px per unit of top speed, wrapping as retail's
        // 32-bit product does; only its direction matters.
        const int32_t scaled=std::bit_cast<int32_t>(uint32_t(e.budget*25)<<18);
        const int32_t length=int32_t(uint32_t(uint64_t((int64_t(scaled)*in.maxSpeed)>>16)));
        uint16_t angle;
        if (e.maneuver==1) angle=uint16_t(host.random(0x4000)+in.heading-0x2000);
        else if (e.maneuver==2) angle=uint16_t(in.heading-host.random(0x5555)-0x2000);
        else angle=uint16_t(host.random(0x5555)+in.heading+0x2000);
        // The one point here that is a direction and not a place: the line ends
        // tens of thousands of pixels out, past the end of the range on any map,
        // so it keeps retail's wrapped subtraction, and the navigator subtracts
        // it the same way (RetailFlightNavigation::wraps): the wrapped point
        // less the position is the line again.
        const RetailFlightVector point{
            std::bit_cast<int32_t>(uint32_t(in.position.x)-uint32_t(retailScaledSine(angle,length))),in.position.y,
            std::bit_cast<int32_t>(uint32_t(in.position.z)-uint32_t(retailScaledCosine(angle,length)))};
        host.install(RetailFlightGoal{point,0x30,0,16});
    } else {
        if (e.budget>=int32_t(host.random(15)+5)) sleep=int32_t(host.random(15)+5);   // 41e9eb
        uint16_t angle;
        if (e.maneuver==4) angle=uint16_t(host.random(0x4000)+in.heading-0x2000);
        else if (e.maneuver==5) angle=uint16_t(in.heading-host.random(0x5555)-0x2000);
        else angle=uint16_t(host.random(0x5555)+in.heading+0x2000);
        const RetailFlightVector point{retailAirRunPoint(in.position,in.position.y,in.heading,32)};
        host.install(RetailInterceptGoal{point,{},angle,true});
    }
    e.budget-=sleep;                                                 // 41eaea
    m.waitMask=0x28;
    m.sleep(tick,uint32_t(sleep));
    return 2;
}

} // namespace ta::sim
