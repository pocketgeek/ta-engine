#pragma once
#include <cstdint>
#include <algorithm>
#include <bit>
#include <utility>
#include "fixed.h"
#include "retailmission.h"
#include "retailflight.h"
#include "retailpiecepose.h"

namespace ta::sim {

// 4dd250: an attached unit's world point is the host's +0x68 position plus the
// 4dd0f0 offset of the attach piece (retailPieceOrigin, which 4dd0f0 also
// returns as zero for a negative or out-of-range piece) -- Kingdoms' layout.
// In TotalA.exe the surface carriers' TransportPickup scripts attach to the
// boom's link and then to piece -1 (ATTACH_UNIT, 0x481340), and VTOL_PICKUP
// attaches at QueryTransport's piece (0x4114a6); the engine still keeps cargo
// on its carrier's own point (the piece offsets are not applied).
inline std::array<int32_t,3> retailAttachPoint(const std::array<int32_t,3>& host,
        std::span<const RetailModelPiece> model,std::span<const cob::RetailPiece> poses,
        int piece,uint16_t heading,uint16_t pitch=0,uint16_t roll=0) {
    if (piece<0) return host;
    const auto offset=retailPieceOrigin(model,poses,piece,heading,pitch,roll);
    std::array<int32_t,3> result{};
    for(size_t axis=0;axis<3;++axis)
        result[axis]=satAdd32(host[axis],offset[axis]);
    return result;
}

// 4dad3e..4dadd2, the attached branch of the per-tick position commit 4dad30:
// the cargo's +0x68 becomes the host attach point, except that a floater
// (type +0x260 bit 0x80000) is held at or above (sea - waterline). The byte
// arithmetic ((waterline*0xffff + sea) << 16) wraps to exactly that height.
// 51b3b0 then stores the point as the cargo's own position.
inline std::array<int32_t,3> retailAttachedPosition(const std::array<int32_t,3>& attach,
        bool floater,uint8_t waterline,uint8_t sea) {
    auto result=attach;
    if (floater) {
        const int32_t surface=std::bit_cast<int32_t>(
            ((uint32_t(waterline)*0xffffu)+uint32_t(sea))<<16);
        if (result[1]<=surface) result[1]=surface;
    }
    return result;
}
// 408992/41a9f8 (pickup), 408df5/41b0cc (unload): compare the sum of the HIGH words of the two fixed-point
// squares. Flooring each term is observable at the edge of the beam radius.
inline bool retailTransportInRange(int32_t dx,int32_t dz,uint16_t range) {
    const uint32_t squared=uint32_t((uint64_t(int64_t(dx)*dx)>>32)+
                                    (uint64_t(int64_t(dz)*dz)>>32));
    return std::bit_cast<int32_t>(squared)<=std::bit_cast<int32_t>(uint32_t(range)*range);
}

// TotalA.exe GROUND_UNLOAD (0x406900) up to the transfer. Stage 1 here is the
// handler's stage 0: one TransportDrop call (0x4069bf), counted at mission
// +0x36. In the boom's reach the script drops the passenger (the transfer,
// next stage). Out of reach the call does nothing: the handler sleeps 15
// (0x4069cf) and waits out BUSY (0x4069de), then -- stage 0 here -- returns
// 9 after the third call (0x406a13) or installs the drop-point circle
// (0x438930) and waits on the navigator only (0x406a65: mask 0xe8, no
// timer), back to the call at the wake (0x406a76 returns 0). A failed route
// wakes it as an arrival does; it is not an abort. The host's circle is
// retailUnloadCircleRadius's.
// Code 9 is the dispatcher's (0x43b8e5..0x43b973): with a next order the
// unload is dropped (8 here), else it calls again in 30 + rand(30) ticks
// with the count kept.
template<class Approach,class Random>
int retailTransportUnloadApproach(RetailMissionState& m,uint32_t& attempts,uint32_t now,
        uint32_t events,bool inRange,bool hasMover,bool hasNext,bool routing,Approach approach,Random random) {
    // The stand-in timer below (not an event) while the route still moves
    // the hull: no navigator failure yet, so no call.
    if(m.stage==1 && (m.flags&0x80000000u) && !(events&~1u) && routing) {
        m.waitMask=0x700u;m.sleep(now,60);return 2;
    }
    m.flags&=~0x80000000u;
    if(m.stage==1) {
        ++attempts;
        if(inRange) { m.sleep(now,1);return 1; }
        m.stage=0;m.sleep(now,15);return 2;
    }
    if(m.stage!=0)return 7;
    if(std::bit_cast<int32_t>(attempts)>=3) {
        if(hasNext)return 8;
        m.flags|=0x800000u;m.stage=1;m.sleep(now,random(30)+30);return 2;
    }
    if(!hasMover)return 8;
    m.stage=1;m.waitMask=0x700u;
    approach();
    // Engine stand-in, not retail: the wait has no timer there, and a route
    // that cannot reach the point ends in the navigator's 0x40 once its
    // 60-tick re-ask comes back empty (docs/retail-pathfinding.md 3.1). The
    // engine's navigator keeps pressing instead (ibid. D16), so a hull that
    // has stopped short is called again after those 60 ticks (flag 0x80000000
    // marks the timer; it is no retail bit).
    m.flags|=0x80000000u;m.sleep(now,60);
    return 2;
}

// 0x406a23..0x406a5e: GROUND_UNLOAD's drop-point circle. A hover carrier
// (flagsA bit 12) circles at ftol(1.5 x its type +0x180, the high word of the
// footprint-z size: footprintz x 16 px); any other carrier goes to the point.
constexpr int32_t retailUnloadCircleRadius(bool canHover,int16_t footZ) {
    return canHover ? int32_t(footZ)*16*3/2 : 0;
}

// The release point of a scripted drop. TransportDrop sets the boom in the
// hull's frame (BoomCalc's XZ_ATAN of the turret-to-point offset) and
// DROP_UNIT releases the passenger where the link piece then is
// (0x4813b0 -> 0x47db70 mode 1 on the link's cell), so a hull that coasts or
// turns during the animation carries the point with it: the offset from the
// carrier at the call (16.16 world units), turned by the hull's heading
// change since (retail BAM). No turn leaves it exact.
inline std::pair<int32_t,int32_t> retailHullPoint(int32_t dx,int32_t dz,uint16_t turn) {
    if (!turn) return {dx,dz};
    return {retailScaledCosine(turn,dx)+retailScaledSine(turn,dz),
            retailScaledCosine(turn,dz)-retailScaledSine(turn,dx)};
}

// GROUND_UNLOAD's transfer (TotalA.exe 0x406900 stages 1-2): the scripted
// drop runs while BUSY holds (0x4069de; the engine's fifteen-tick beam), and
// DROP_UNIT tests the link's cell once, at release (0x4813b0 -> 0x47db70
// mode 1: any other body refuses). A refused drop leaves the passenger
// aboard, so stage 2 (0x4069f0..0x406a13) goes on as after any failed call:
// returned here as stage 0 of retailTransportUnloadApproach. 4 = release.
template<class Placement,class Effects>
int retailSurfaceUnloadTransfer(RetailMissionState& m,int32_t& ticks,uint32_t now,
        bool passengerValid,Placement placement,Effects effects) {
    if (m.stage!=2) return 7;
    if (!passengerValid) { m.sleep(now,1);return 0; }
    if (ticks<15) {
        if (!ticks) effects();
        ++ticks;m.sleep(now,1);return 2;
    }
    ticks=0;
    if (placement()) { m.stage=4;return 4; }
    m.stage=0;m.sleep(now,1);return 2;
}

// Engine event bits for TotalA.exe's mission events: the navigator's arrival,
// failure and release (TA 0x80 / 0x40 / 0x20) are the engine's 0x100 / 0x200
// / 0x400 (Kingdoms' numbering, kept engine-wide); 8 (a referenced unit
// died, 0x406791 / 0x4111c9) and 4 (BUSY changed, 0x438730's wait) are TA's.
constexpr uint32_t kTransportNavEvents=0x700u;

// TotalA.exe GROUND_PICKUP (0x406780), the carrier's only mission: the
// passenger is given none. The referenced passenger gone, or event 8, fails
// it (8, 0x40678b..0x406796). Stage 0 (0x4067b1): canload, then the size test
// (passenger footprint x > transportsize: 8). Stages 1 and 3 (0x40685d ->
// 0x438730): while the carrier's BUSY bit holds, wait on 4 | 8. Stage 2
// (0x40680f): TransportPickup(passenger), counted at mission +0x36, sleep 15.
// Stage 4 (0x40686f): the passenger aboard this carrier ends it (5); after
// three calls 9 (the dispatcher drops it when another order follows, else it
// calls again in 30 + rand(30) ticks, 0x43b8e5); else a radius-0 circle on
// where the passenger stands (0x438930) and a wait on the navigator and 8
// (0xe8). Stage 5 (0x4068b1): the controller goes, back to stage 0.
// The script owns the transfer: call() starts it (the boom reaches or not),
// busy() is its BUSY, aboard() what its ATTACH_UNIT left. The engine's
// navigator never fails a point it cannot reach (docs/retail-pathfinding.md
// D16), and a radius-0 circle on a standing unit is one: the stand-in timer
// (flag 0x80000000, no retail bit) wakes the circle after 60 ticks once the
// hull has stopped, as GROUND_UNLOAD's does (retailTransportUnloadApproach).
template<class Host>
int retailGroundPickup(RetailMissionState& m,uint32_t& calls,uint32_t now,uint32_t events,
        bool passenger,bool routing,Host& host) {
    if(!passenger || (events&8u)) return 8;
    if(m.stage==5 && (m.flags&0x80000000u) && !(events&~1u) && routing) {
        m.waitMask=kTransportNavEvents|8u;m.sleep(now,60);return 2;
    }
    m.flags&=~0x80000000u;
    switch(m.stage) {
    case 0:
        if(!host.canLoad()) return 7;
        return host.fits() ? 1 : 8;
    case 1: case 3:
        if(host.busy()) { m.waitMask=0xcu;return 2; }
        return 1;
    case 2:
        host.call();++calls;m.sleep(now,15);return 1;
    case 4:
        if(host.aboard()) return 5;
        if(std::bit_cast<int32_t>(calls)>=3) return 9;
        host.approach();
        m.waitMask=kTransportNavEvents|8u;
        m.flags|=0x80000000u;m.sleep(now,60);
        return 1;
    case 5:
        host.release();return 0;
    default:return 7;
    }
}

// TotalA.exe VTOL_PICKUP (0x4111b0). No boom: the carrier flies onto the
// passenger and attaches it at the piece QueryTransport names. Every wake
// fails (8) without a passenger, on 8 or the navigator's failure (0x10048;
// its 0x10000 bit is not modelled), when the passenger's top (y + its type
// +0x16e, the model height) is not above the sea (0x4111d5..0x4111f9), or --
// with no message -- while the carrier holds anyone (+0x8a, 0x41121a: one
// passenger per air carrier). Stage 0 (0x411241): canfly, the size test,
// every slot automatic (0x4898b0(3)), off any pad (0x48aac0), Activate
// (0x48b090); a landed carrier takes off to its own point at half its
// cruisealt (0x43d210, 0x44e2d0, 0x44e6c0) and waits on the navigator (0xe0),
// one in the air goes on at once. Stage 1 (0x41132f): a controller on the
// passenger (0x44e190) at cruisealt, radius 0x30 (0x44e730), wait 0x100e8.
// Stage 2 (0x41138a): QueryTransport into +0x36, the same wait. Stage 3
// (0x4113cb): BeginTransport(model height) lowers the link, and a controller
// on the passenger (0x44e250) holds the hull where the piece (0x43def0) meets
// it, wait 0x100ea. Stage 4 (0x411479): a failed approach (0x42) runs
// EndTransport and fails; else ATTACH_UNIT at the piece (0x48aac0), and back
// to its own point at cruisealt (0xe0). Stage 5 (0x4114fe): done.
template<class Host>
int retailVtolPickup(RetailMissionState& m,uint32_t events,bool passenger,bool submerged,
        bool carrying,Host& host) {
    if(!passenger || (events&0x208u) || submerged || carrying) return 8;
    switch(m.stage) {
    case 0:
        if(!host.canFly()) return 7;
        if(!host.fits()) return 8;
        if(host.prepare()) m.waitMask|=kTransportNavEvents;
        return 1;
    case 1:
        host.pursue();m.waitMask=kTransportNavEvents|8u;return 1;
    case 2:
        host.query();m.waitMask=kTransportNavEvents|8u;return 1;
    case 3:
        host.descend();m.waitMask=kTransportNavEvents|0xau;return 1;
    case 4:
        if(events&0x202u) { host.endTransport();return 8; }
        host.attach();host.climb();m.waitMask|=kTransportNavEvents;return 1;
    case 5:return 5;
    default:return 7;
    }
}

// TotalA.exe VTOL_UNLOAD (0x411560): nobody aboard ends it (5). Stage 0
// (0x41159d): canfly; the passenger is the head of the carrier's list (the
// last one aboard, 0x4115c7 -> 0x489690); a point controller at the drop
// point (mission +0x22) at cruisealt, radius 0x140 (0x44e730), wait 0xe8.
// Stage 1 (0x411635): DROP_UNIT's placement test at the point (0x47db70 mode
// 1 on the footprint's corner cell, rounded, 0x41165a..0x411682); refused,
// 9 ("Unable to unload unit"); else down to the point at the passenger's
// model height (type +0x170), wait 0xe8. Stage 2 (0x4116f2): a failed
// approach is 9; the test again (9); EndTransport, the drop (0x48aac0 with no
// carrier) where the link holds it, and back up to cruisealt over the
// carrier's own point (0xe0). Stage 3 (0x4117fe): done.
template<class Host>
int retailVtolUnload(RetailMissionState& m,uint32_t events,bool carrying,Host& host) {
    if(!carrying) return 5;
    switch(m.stage) {
    case 0:
        if(!host.canFly()) return 7;
        host.approach();m.waitMask=kTransportNavEvents|8u;return 1;
    case 1:
        if(!host.placement()) return 9;
        host.descend();m.waitMask=kTransportNavEvents|8u;return 1;
    case 2:
        if(events&0x200u) return 9;
        if(!host.placement()) return 9;
        host.drop();host.climb();m.waitMask=kTransportNavEvents;return 1;
    case 3:return 5;
    default:return 7;
    }
}

// TotalA.exe 0x489a90 (the load test the LOAD order and both pickup missions
// ask, 0x43f70c/0x43fb3d): the passenger's footprint x (type +0x14a) at most the
// carrier's transportsize (+0x22a) and fewer passengers aboard than its
// transportcapacity (+0x22b). TA has no size budget; a nonzero sizeLimit keeps
// the summed one for engine-built carriers.
constexpr bool retailTransportCapacity(uint16_t passengerSize,uint16_t maxPassenger,
        uint32_t passengers,uint16_t countLimit,uint32_t usedSize,uint16_t sizeLimit) {
    return passengerSize<=maxPassenger && passengers<countLimit &&
           (!sizeLimit || usedSize+uint32_t(passengerSize)<=sizeLimit);
}
}
