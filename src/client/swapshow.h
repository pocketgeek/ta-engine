#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tak {

// Legion allies swap places (display only, never hashed). The sim trades two
// allied bodies' places in one tick (legion.cpp swapBodies): a jump of one
// footprint, 16-32 px, that the tick interpolation would draw as the two
// bodies darting through each other. The display spreads the jump over
// kSwapShowTicks and sidesteps each body to the right of its jump, so the
// two (jumping opposite ways) pass side by side. Until the swap has played
// out the drawn body trails the sim's by the offset.
inline constexpr float kSwapShowTicks=8,kSwapSidestep=0.35f;
// A swap's jump is one footprint; anything longer (a seek) is not played out.
inline constexpr float kSwapMaxJump=64;

struct SwapShow {
    float x=0,z=0;               // the jump still to show (drawn minus sim position when it began)
    float age=kSwapShowTicks;    // ticks shown so far; kSwapShowTicks = none in progress
    float ox=0,oz=0;             // the offset drawn this tick (added to the sim position)
};

// `prev` is the record drawn last tick, `jumpX`/`jumpZ` this tick's sim step
// over `ticks` ticks, `swapped` whether the body traded places in them
// (World::legionSwapTick).
inline SwapShow swapShowStep(const SwapShow& prev,float jumpX,float jumpZ,uint32_t ticks,bool swapped) {
    SwapShow s=prev;
    const float t=float(std::max<uint32_t>(ticks,1));
    if(swapped&&std::max(std::fabs(jumpX),std::fabs(jumpZ))<=kSwapMaxJump) {
        s.x=prev.ox-jumpX;s.z=prev.oz-jumpZ;s.age=0;   // on from where it was drawn
    }
    s.age=std::min(kSwapShowTicks,s.age+t);
    if(s.age>=kSwapShowTicks) return SwapShow{};
    const float f=s.age/kSwapShowTicks,side=kSwapSidestep*std::sin(f*3.14159265f);
    // The jump runs along (-x,-z); (z,-x) is to its right.
    s.ox=s.x*(1-f)+s.z*side;
    s.oz=s.z*(1-f)-s.x*side;
    return s;
}

} // namespace tak
