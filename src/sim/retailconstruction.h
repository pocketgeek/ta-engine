#pragma once

#include <cstdint>
#include <optional>
#include "retailmission.h"
#include "retailgoal.h"
#include "footprint.h"
#include "fixed.h"

namespace ta::sim {

// 405756..405809: failed MobileBuild paths may still leave the builder close
// enough to place the site. Each footprint diagonal is truncated separately.
// Retail subtracts in 32 bits and reads the distance as a signed 16-bit word
// of whole pixels (movsx at 4057ec), so 32768 px or more reads as negative and
// "within reach". No retail map has such a distance; a map from 1449 cells a
// side has (its diagonal). Ours: the differences and the distance are taken
// whole, and a far site is far. Identical below 32768 px.
inline bool retailBuildWithinReach(Fixed x,Fixed z,Fixed gx,Fixed gz,
        int16_t fx,int16_t fz,int16_t siteFx,int16_t siteFz,uint16_t reach) {
    const int64_t dx=int64_t(x.v)-int64_t(gx.v);
    const int64_t dz=int64_t(z.v)-int64_t(gz.v);
    const int distance=int(isqrt64(uint64_t(dx*dx)+uint64_t(dz*dz))>>16);
    const auto edge=[](int16_t width,int16_t height) {
        return int(isqrt64(uint64_t(int64_t(width)*width+int64_t(height)*height)*64));
    };
    return distance-edge(fx,fz)-edge(siteFx,siteFz)<=reach;
}

// TotalA.exe HelpBuild (0x403f70) stage 0 (0x403fd9..0x404053), emulated
// (unicorn, 2026-11-26): the approach is the ring controller 0x44d3b0 round
// the site's position (+0x6a), installed through 0x438a00(pos, outer, inner)
// with wait mask 0xe8. inner = ftol(sqrt(fx*fx + 2*fz) * 16.0) / 2 (signed,
// toward zero), fx/fz the SITE type's footprint (+0x14a/+0x14c): the x87
// sequence 0x40401d..0x404023 (fld st1; fmul st,st2; fadd st,st1; fadd
// st,st1) adds footprintz twice where a diagonal would square it, so a 2x2
// site gives 22, 4x4 39, 6x6 55, 5x4 45. outer = builddistance (+0x212,
// unsigned) + inner. The ring's centre is snapped to the HELPER's footprint
// origin (0x44d3c6: the mission's unit +0x7e), and it accepts a footprint
// origin (in/16)^2 <= d^2 <= (out/16)^2 (0x44d7c0). The square root is taken
// whole: floor(16 sqrt(s)) = isqrt(256 s) for an integer s.
inline RetailRingGoal retailHelpBuildRing(Fixed siteX,Fixed siteZ,int16_t siteFx,int16_t siteFz,
                                          int helperFx,int helperFz,uint16_t buildDistance) {
    const int64_t s=int64_t(siteFx)*siteFx+2*int64_t(siteFz);
    const int32_t diagonal=s>0 ? int32_t(isqrt64(uint64_t(s)*256)) : 0;
    const int32_t inner=diagonal/2;
    const int32_t outer=int32_t(buildDistance)+inner;
    return {footprintOrigin(siteX,helperFx),footprintOrigin(siteZ,helperFz),inner,outer,
            retailCircleRadiusSquared(outer)};
}

// COB BUILD_PERCENT_LEFT (TotalA.exe GET 17, 0x480a44): unfinished work maps
// to 1..100 (1 - ftol(remaining * -99)); none left is 0.
inline uint32_t retailConstructionPercent(float remaining) {
    return remaining==0 ? 0u : uint32_t(1-int32_t(double(remaining)*-99.0));
}

// TotalA.exe's GetBuilt (0x402da0) while work remains: stage 0 makes every
// weapon slot automatic (0x4898b0(3)) and sleeps 300 ticks, stage 1 sleeps 30,
// and stage 2 sleeps 30 more after any work event (0x8000) or, on a timer wake
// with none, decays the site once (0x41bcd0(site, 11)) and sleeps 11; every
// stage waits on the work event (mission +6 |= 0x8000). The host owns the
// slots and the decay step (World::decayConstruction).
template<class StopWeapons,class Decay>
int retailGetBuiltWaiting(RetailMissionState& mission,uint32_t tick,uint32_t events,
                          StopWeapons stopWeapons,Decay decay) {
    switch (mission.stage) {
    case 0:
        stopWeapons();mission.sleep(tick,300);mission.waitMask|=0x10000000u;return 1;
    case 1:
        mission.sleep(tick,30);mission.waitMask|=0x10000000u;return 1;
    case 2:
        if (events&0x10000000u) mission.sleep(tick,30);
        else if (events&1) { mission.sleep(tick,11);decay(); }
        mission.waitMask|=0x10000000u;return 2;
    default:return 7;
    }
}

// One dispatch of an unfinished site's GetBuilt when the site's own mission is
// its only one: `worked` is the site's work event at dispatch time (0x41ba9d
// raises it for any work >= 0, paid or not; World keeps it as bit 0x10000000).
// A fresh mission sleeps 300 ticks, a work event wakes it to sleep 30 more,
// and from then on every work event restarts the 30 while a timer wake with
// no work decays the site once and sleeps 11. True when this dispatch decays.
inline bool retailSiteTimer(RetailMissionState& mission,uint32_t tick,bool worked) {
    struct Host {
        RetailMissionState& m;uint32_t tick;bool decay=false;
        bool enabled() const { return true; }
        RetailMissionState* head() { return &m; }
        bool hasNext(const RetailMissionState&) const { return false; }
        void idle() {}
        uint32_t random(int) { return 0; }
        void remove(RetailMissionState&) {}
        void rotate(RetailMissionState&) {}
        void clear() {}
        int handle(RetailMissionState& state,uint32_t events) {
            return retailGetBuiltWaiting(state,tick,events,[]{},[&]{ decay=true; });
        }
    } host{mission,tick};
    uint32_t events=worked ? 0x10000000u : 0u;
    retailDispatchMissions(tick,events,host);
    return host.decay;
}

// 5d3d54 (_ftol): truncate through a 64-bit fistp and keep the low word. Out of
// range stores the integer indefinite, whose low word is zero.
inline uint32_t retailFtol(double x) {
    if (!(x>-9223372036854775808.0 && x<9223372036854775808.0)) return 0;
    return uint32_t(uint64_t(int64_t(x)));
}

// 4bff28..4bff79: the parser reads a nonpositive buildtime as 100 and keeps its
// reciprocal at UnitType+0x216, which every build and repair multiplies by.
inline float retailInverseBuildTime(float buildTime) {
    return float(1.0/double(buildTime>0 ? buildTime : 100.0f));
}

// SELF-REPAIR, 51e1a8 -> 429d90 (unpaid). healtime is the unit's own worker time,
// not seconds per hit point: every eighth tick (the caller's cadence and gates: a
// finished, living unit below full health whose healtime is nonzero) it puts
// healtime * 0.26666664 -- eight thirtieths of a second of that work -- into itself
// as a fraction of its type's build, so it regains maxdamage * healtime / buildtime
// hit points a second. 429d90 turns the fraction into hit points as a 16.16 product
// of maxdamage, adds the unit's carried remainder (+0x10e: bits 8..15 of the last
// sum; bits 0..7 are dropped every call) and returns the whole points it hands to
// 51a140 as damage type 12.
inline uint16_t retailSelfRepairPoints(uint8_t& carry,float healTime,float inverseTime,
                                       uint32_t maxHp) {
    const float work=float(double(healTime)*0.26666664);
    const double progress=double(work)*double(inverseTime);
    const uint32_t sum=(uint32_t(carry)<<8)+retailFtol(double(maxHp)*progress*65536.0);
    carry=uint8_t(sum>>8);
    return uint16_t(sum>>16);
}

// 51a2f8..51a319, damage type 12: the points are added to the signed 16-bit hit
// points as an unsigned word, and anything at or past maxdamage becomes maxdamage.
inline uint16_t retailAddHitPoints(uint16_t hp,uint16_t points,uint32_t maxHp) {
    const uint32_t sum=uint32_t(points)+uint32_t(int32_t(int16_t(hp)));
    return uint16_t(sum>=maxHp ? maxHp : sum);
}

} // namespace ta::sim
