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
// returns as zero for a negative or out-of-range piece). Both transport pickup
// handlers attach with piece -1 (408d00 ground, 41add7 air, through 51b4f0 to
// 51b5a0, which stores it at +0x112), and no shipped unit script issues
// ATTACH_UNIT, so transport cargo sits exactly on its carrier.
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
// 4034ec..4035e7: Move_Seek_Pickup after attachment, eligibility and
// carrier mission-chain checks. The host copies the carrier position and owns
// controller installation/removal; the dispatcher interprets the return code.
template<class Approach,class Detach>
int retailPassengerPickup(RetailMissionState& m,uint32_t& retries,uint32_t now,
        uint32_t events,bool surface,Approach approach,Detach detach) {
    switch(m.stage) {
    case 0:
        if(surface) { approach();m.waitMask=0x700; }
        else detach();
        m.waitMask|=0x88;m.sleep(now,30);return 1;
    case 1:
        if(events&0x80) { detach();retries=0;return 1; }
        if(events&0x700) { detach();m.waitMask=0x88;m.sleep(now,30);return 2; }
        m.stage=0;m.sleep(now,5);return 4;
    case 2:
        ++retries;
        if(std::bit_cast<int32_t>(retries)>5)return 8;
        m.sleep(now,30);return 2;
    default:return 7;
    }
}

// 408992/41a9f8 (pickup), 408df5/41b0cc (unload): compare the sum of the HIGH words of the two fixed-point
// squares. Flooring each term is observable at the edge of the beam radius.
inline bool retailTransportInRange(int32_t dx,int32_t dz,uint16_t range) {
    const uint32_t squared=uint32_t((uint64_t(int64_t(dx)*dx)>>32)+
                                    (uint64_t(int64_t(dz)*dz)>>32));
    return std::bit_cast<int32_t>(squared)<=std::bit_cast<int32_t>(uint32_t(range)*range);
}

// 41aa4b..41aab7 / 41b12f..41b19e: once in pickup/unload transfer range, an air carrier flies away along
// its current heading while unloading. This is a controller goal, not a
// different passenger landing position.
inline RetailFlightGoal retailUnloadStepOut(RetailFlightVector position,uint16_t heading,uint16_t range) {
    const int32_t distance=std::bit_cast<int32_t>((uint32_t(range)+400u)*131072u);
    position.x=satSub32(position.x,retailScaledSine(heading,distance));
    position.z=satSub32(position.z,retailScaledCosine(heading,distance));
    return {position,0x30,0,16};
}

// 4e3f70/4e4540/4e41d0: pickup's persistent pursuit follows the
// non-flying passenger, including its altitude, with a strict horizontal radius.
inline RetailFlightGoal retailPickupPursuitGoal(RetailFlightVector target,uint16_t range) {
    target.y=std::min(target.y,511*65536);
    return {target,0x11,0,int16_t(uint16_t(range-1))};
}

// 408bdd..408c21 / 41ac91..41acf3: approach polls while the navigator
// continues independently. Controller arrival/failure can wake the mission early.
template<class Random>
void retailPickupApproachWait(RetailMissionState& m,uint32_t now,bool flying,Random random) {
    m.waitMask=flying ? 0x728u : 0x708u;
    m.sleep(now,flying ? random(6)+6u : 15u);
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

// 4089fa..408a19: a surface pickup outside transfer range aborts when
// its controller failed (or there is no mover) and the passenger is stationary.
// A moving passenger may still close the gap. VTOL_PICKUP has no such gate.
constexpr bool retailPickupApproachAborted(bool flying,bool hasMover,uint32_t events,int32_t passengerSpeed) {
    return !flying && (!hasMover || (events&0x200u)) && passengerSpeed==0;
}

// GROUND_PICKUP / VTOL_PICKUP transfer stage (408c3b / 41ad12).
// A moving passenger restarts the beam delay, with a bounded six-tick retry.
template<class Effects>
int retailPickupTransfer(RetailMissionState& m,int32_t& ticks,uint32_t& attempts,
        uint32_t now,bool flying,int32_t passengerSpeed,Effects effects) {
    if(std::bit_cast<int32_t>(attempts)>=(flying ? 4:10))return 8;
    if(passengerSpeed>0) {
        ++attempts;ticks=0;m.sleep(now,6);return 2;
    }
    if(ticks>=15)return 1;
    if(!ticks)effects();
    ++ticks;m.sleep(now,1);return 2;
}

// Shared transfer/retry stages of Unload (408ee1/408fe6) and VTOL_Unload
// (41b22b/41b362). Approach and attachment remain host operations.
template<class Placement,class Effects,class Blocked>
int retailUnloadTransfer(RetailMissionState& m,int32_t& ticks,uint32_t attempts,
        uint32_t now,bool passengerValid,Placement placement,Effects effects,Blocked blocked) {
    if (m.stage==2) {
        if (!passengerValid) { m.sleep(now,1);return 0; }
        if (placement(false)) {
            if (ticks>=15) { m.stage=4;return 4; }
            if (!ticks) effects();
            ++ticks;m.sleep(now,1);return 2;
        }
        if (placement(true)) { m.sleep(now,1);return 1; }
        blocked();return 8;
    }
    if (m.stage==3) {
        if (std::bit_cast<int32_t>(attempts)>=6) return 9;
        ticks=0;m.sleep(now,10);m.stage=1;return 4;
    }
    return 7;
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

// 4090b7/41b42c: after unloading, PARK disperses the passenger so
// the next one can use the same drop point. Two random(3) draws precede
// the footprint allowance for up to six passengers still aboard.
inline uint32_t retailUnloadParkPadding(uint32_t first,uint32_t second,
        uint32_t remaining,int16_t nextFootX,int16_t nextFootZ) {
    const uint64_t squared=uint64_t(int64_t(nextFootX)*nextFootX+
                                    int64_t(nextFootZ)*nextFootZ);
    return (first+second+1)*16+std::min(remaining,6u)*uint32_t(isqrt64(squared*256)/2);
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
