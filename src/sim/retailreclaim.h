#pragma once
#include "sim/retailgoal.h"
#include "sim/retailmission.h"
#include <climits>
#include <cstdint>

namespace tak::sim {
// Retail RECLAIM (406250), checked by emulation (2026-10-24, see
// docs/retail-engine.md "Reclaim"). It pays no mana, and nothing about the
// builder (workertime, builddistance) changes how long it takes.

// 4d4e80, installed at stage 0: the rectangle controller of footprint-origin
// cells whose builder footprint (bw x bh) touches the target's (anchor ax,az,
// footprint fw x fh) at an edge or corner, with no gap and no overlap.
// RetailRectGoal::accepts takes only its perimeter.
inline RetailRectGoal retailReclaimRectangle(int ax,int az,int fw,int fh,int bw,int bh) {
    return {ax-bw,ax+fw,az-bh,az+fh};
}

// Stage 1 (4063a6): the countdown, ftol(type+0x128 + 15.0f). The energy is a
// float in retail too and the add stays in float; ftol truncates.
inline int32_t retailReclaimCountdown(float energy) {
    const float v=energy+15.0f;
    // Out of range (or NaN) is undefined in C++ and differs by CPU (x86
    // gives 0x80000000, ARM saturates): pin it to the x87 integer
    // indefinite ftol returns, so every peer agrees on a modded value.
    if (!(v>=-2147483648.0f && v<2147483648.0f)) return INT32_MIN;
    return int32_t(v);
}

// 494ce0's fade object: alpha 1.0 less 0.03 a tick, so the reclaimed feature
// leaves the plane 34 ticks after the order ends.
inline constexpr uint8_t kReclaimFadeTicks = 34;

// 406529, the work wake of stages 3 and 4: two ticks of countdown, one
// worker particle while more than 15 remain, then sleep two ticks (wait mask
// 2|1: the timer, or the order's cancel). Stage 3 repeats until the count is
// spent, stage 4 runs once more, and the wake after that is stage 5, the
// removal (the caller's: retailReclaimRemoval). Order end = arrival +
// 2*ceil(C/2) + 2 ticks for a countdown C.
struct RetailReclaimWake { bool particle=false; };
inline RetailReclaimWake retailReclaimWork(RetailMissionState& m,int32_t& countdown,uint32_t tick) {
    RetailReclaimWake wake;
    countdown=int32_t(uint32_t(countdown)-2u);   // wraps like retail's 32-bit sub
    wake.particle=countdown>15;
    m.waitMask=2;
    m.sleep(tick,2);
    if (countdown<=0) ++m.stage;
    return wake;
}
inline bool retailReclaimRemoval(const RetailMissionState& m) { return m.stage>=5; }

} // namespace tak::sim
