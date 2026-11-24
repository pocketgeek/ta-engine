#pragma once

#include "sim/retailmission.h"
#include <cstdint>

namespace ta::sim {

// TotalA.exe's repair pads. An air base (flagsA bit 9 isairbase, with bit 6
// builder: armasp / corasp, and the carriers armcarry / corcarry) takes
// aircraft onto the pieces its script's QueryLandingPad names and mends them
// there with its own WorkerTime.
//
// The detour (VTOL_Patrol 0x410f3e, VTOL_RepairPatrol 0x415397,
// VTOL_SeekAttack 0x410518, VTOL_SeekGuard 0x410972, AirStrike 0x4125c2; also
// AirToGround 0x412b3f and AirToGroundHover 0x413a8a, whose TA stages are not
// ported): below 3/4 of its maxdamage (unit +0x108 < (type +0x1fa >> 2) x 3)
// the flyer asks 0x40b530(owner, its position, 0xf00, list) for its owner's
// air bases -- builder and isairbase, activated (+0x10e bit 0), within 3840 px
// ((dx*dx >> 32) + (dz*dz >> 32), 16.16 deltas) -- and, when there is any,
// drops its controller (0x4388d0(0)), puts "VTOL_LANDING" on a random one
// (0x4b6c30(count)) at the head of its queue (0x43acb0), clears its wait mask
// and returns 0: the mission it left starts again from stage 0 once the
// landing is done. (AirStrike returns 0 under 3/4 health with or without a
// pad.)
inline bool retailPadDetourDamaged(int32_t hp,int32_t maxDamage) {
    return hp<int32_t((uint32_t(maxDamage)>>2)*3u);
}
inline bool retailPadInReach(int32_t dx,int32_t dz,int32_t radius) {
    const int64_t x=(int64_t(dx)*dx)>>32,z=(int64_t(dz)*dz)>>32;
    return x+z<=int64_t(radius)*radius;
}

// VTOL_Landing (TotalA.exe 0x4118e0; air table 0x4fca18 row 2, "Landing"),
// the pad as its target (mission +0x16). Events are the engine's: arrival
// 0x100 (TA 0x20), failure 0x200 (TA 0x40), release 0x400 (TA 0x80), the
// target's death 8; TA's 0xe0 / 0xe8 masks are 0x700 / 0x708.
//   head (0x4118fb): no pad ends it (8, "Landing aborted").
//   stage 0 (0x411934): no mover or not canfly ends the list (7); "Landing"
//            as its status; every slot manual (0x4898b0(3)), off any carrier
//            (0x48aac0(unit, 0, -1, 2)), Activate (0x48b090(1, 1)); a landed
//            flyer takes off (0x43d210(2)) to a point controller over its
//            own position at cruisealt / 2 and the events wait on it (0xe0);
//            the circle angle (+0x36) is rand(0x10000). 1.
//   stage 1 (0x411a03): a free pad piece (QueryLandingPad, then 0x47e570 on
//            each of its four answers; asked a second time when the first
//            names none, 0x411a77) goes on to stage 2 (2). None: a point
//            controller on the pad less (sin, cos)(angle) x the flyer's slot
//            0 weapon range (unit +0x10 -> +0xdc), radius 0x80; the angle
//            steps on 0x4000; wait 0xe8, stage 1 (2): the flyer circles the
//            pad a quarter turn at a time until a piece comes free.
//   stage 2 (0x411b8f): a controller on the pad (0x44e250(pad, -1)), radius
//            0xa0, wait 0xe8. 1.
//   stage 3 (0x411bc3): a free piece into +0x36; none is a failure (0,
//            "Landing failed": the landing starts again). A controller on
//            that piece (0x44e250(pad, piece)), radius 0x30, wait 0xe8. 1.
//   stage 4 (0x411c9d): 1, straight on.
//   stage 5 (0x411c96): an arrival (0x20) goes on to stage 6 (1). Else the
//            piece, or another free one (none: 0, "all pads are occupied"),
//            a controller on it at its height plus a passenger's (+0x8a ->
//            type +0x170; 0 when it carries none: 0x44e6c0), the flyer's
//            EndTransport call-in, a sleep of 15 and the events (0xe8);
//            stage 5 (2).
//   stage 6 (0x411dd2): a failed controller (0x40) fails the list (8); the
//            piece taken meanwhile is a failure (0, "no pads available");
//            else the flyer (or the passenger it carries, which it puts down
//            there) is attached to the pad at the piece (0x48aac0(unit, pad,
//            piece, 0): mover mode 0), and a whole flyer -- or one whose pad
//            is not a finished builder air base -- is done (5); a damaged one
//            pushes SELFREPAIR on the pad (0x411ea7..0x411ee1) and is done.
// Host: padLive(); canFly(); prepare() (stage 0's slots, carrier, Activate
// and take-off: true when it installed the take-off controller); random(n);
// freePiece(queries) (QueryLandingPad asked up to `queries` times until one
// names a free piece, 0x47e570: -1 none); pieceFree(piece)
// (0x47e570); circle(angle) (stage 1's controller); approach() (stage 2's);
// hold(piece) (stage 3's); descend(piece) (stage 5's, with EndTransport);
// attach(piece) (stage 6's attach and SelfRepair push). `angle` and `piece`
// are both mission +0x36.
template<class Host>
int retailVtolLanding(RetailMissionState& m,uint16_t& angle,int32_t& piece,uint32_t tick,uint32_t events,
                      Host& host) {
    if (!host.padLive()) return 8;                                   // 0x4118fb
    switch (m.stage) {
    case 0:                                                          // 0x411934
        if (!host.canFly()) return 7;
        if (host.prepare()) m.waitMask|=0x700;                       // 0x4119df
        angle=uint16_t(host.random(0x10000));                        // 0x4119e7
        return 1;
    case 1:                                                          // 0x411a03
        if (host.freePiece(2)>=0) { m.stage=2; return 2; }           // 0x411a3a, 0x411aab
        host.circle(angle);                                          // 0x411af1..0x411b70
        angle=uint16_t(angle+0x4000u);                               // 0x411b21
        m.waitMask=0x708;
        m.stage=1;
        return 2;
    case 2:                                                          // 0x411b8f
        host.approach();
        m.waitMask=0x708;
        return 1;
    case 3:                                                          // 0x411bc3
        piece=host.freePiece(1);
        if (piece<0) return 0;                                       // 0x411c2e
        host.hold(piece);
        m.waitMask=0x708;
        return 1;
    case 4:                                                          // 0x411c9d
        return 1;
    case 5:                                                          // 0x411c96
        if (events&0x100) return 1;
        if (piece<0 || !host.pieceFree(piece)) {                     // 0x411cac
            piece=host.freePiece(1);
            if (piece<0) return 0;                                   // 0x411d2a
        }
        host.descend(piece);                                         // 0x411d47..0x411da9
        m.sleep(tick,15);                                            // 0x411dae
        m.waitMask|=0x708;
        m.stage=5;
        return 2;
    case 6:                                                          // 0x411dd2
        if (events&0x200) return 8;
        if (!host.pieceFree(piece)) return 0;                        // 0x411df9
        host.attach(piece);                                          // 0x411e12..0x411ee6
        return 5;
    default:
        return 7;
    }
}

// SelfRepair (TotalA.exe 0x402430; mission table 0x4fc490 row 20,
// "Repairing"), which VTOL_Landing pushes as "SELFREPAIR" on the pad it
// landed on (mission +0x16).
//   head (0x40243e): no pad ends it (8, "Repair aborted.").
//   stage 0 (0x4025d7): a pad that is not a builder (type +0x241 bit 6)
//            ends the list (7); one unfinished (+0x104 != 0.0) or a flyer not
//            activated (unit +0x10e bit 0) fails it (8); every slot manual
//            (0x4898b0(3)). 1.
//   stage 1 (0x4024a5): whole (+0x108 >= type +0x1fa) goes on to stage 2
//            (1). Else the flyer may not cloak for 150 ticks (+0xb0), one
//            repair step of the pad on it (0x41bd10(pad, flyer, WorkerTime /
//            30)) and, when it took, the pad's nano spray at it (0x43e400,
//            0x4720d0); a sleep of a tick, waiting on the target's death too
//            (8). 2.
//   stage 2 (0x402485): "Unit repaired", done (5).
// Host: padLive(); padBuilder(); padFinished(); activated(); slotsManual();
// whole(); repair().
template<class Host>
int retailPadRepair(RetailMissionState& m,uint32_t tick,Host& host) {
    if (!host.padLive()) return 8;                                   // 0x40243e
    switch (m.stage) {
    case 0:                                                          // 0x4025d7
        if (!host.padBuilder()) return 7;
        if (!host.padFinished() || !host.activated()) return 8;      // 0x4025f5..0x402613
        host.slotsManual();                                          // 0x402615
        return 1;
    case 1:                                                          // 0x4024a5
        if (host.whole()) return 1;
        host.repair();                                               // 0x4024cd..0x4025ae
        m.sleep(tick,1);
        m.waitMask|=8;
        return 2;
    case 2:                                                          // 0x402485
        return 5;
    default:
        return 7;
    }
}

} // namespace ta::sim
