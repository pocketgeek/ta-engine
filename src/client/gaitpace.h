#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tak {

// Legion display gait pacing (deliberate, not retail; display only, never
// hashed). A walker's cycle is a script playing fixed keyframes, so a body
// that Legion holds, shuffles or slows behind another while the mover still
// reports it moving (GET 29 above the gait threshold) walks its legs at full
// pace on the spot. The pace follows the distance the body actually covered
// instead, as a fraction of its terrain-adjusted maximum (the GET 29
// denominator), smoothed over a few ticks so per-tick shuffles do not jitter.
// A body that stops reporting motion snaps back to 1: its cycle then plays
// out its remaining (static-gated) frames at retail pace and restores.
// Full speed reads 1 and leaves the animation exactly as before.
// Per 30 Hz tick: a pace eases down over a few ticks and recovers twice as
// fast, so a body that gets going again does not drag slow legs behind it.
inline constexpr float kGaitSlowing=0.25f,kGaitRecovery=0.5f;
// At or above this fraction of its maximum a body counts as walking at full
// pace: turning while moving and slope stepping shave a few percent off a free
// walker, which must look exactly as before.
inline constexpr double kGaitFullPace=0.9;

// `dx`/`dz` (16.16) moved over `ticks` simulation ticks, `maximum` 16.16 per tick.
inline float legionGaitRate(float previous,int64_t dx,int64_t dz,uint32_t ticks,
                            int64_t maximum,bool moving) {
    if(!moving || maximum<=0) return 1.0f;
    if(!ticks) return previous;
    if(ticks>30) return 1.0f;   // a replay seek or long stall: nothing to judge
    const double speed=std::sqrt(double(dx)*double(dx)+double(dz)*double(dz))/double(ticks);
    const float target=float(std::min(1.0,speed/(double(maximum)*kGaitFullPace)));
    const float keep=std::pow(1.0f-(target<previous ? kGaitSlowing : kGaitRecovery),
                              float(std::min<uint32_t>(ticks,30)));
    return std::clamp(target+(previous-target)*keep,0.0f,1.0f);
}

} // namespace tak
