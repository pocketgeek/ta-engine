#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include "sim/retailpiecepose.h"
#include "sim/retailmotion.h"

namespace tak::sim {

// 511170: discard subpixel fractions, then truncate each signed interpolation
// separately. The last row/column cannot supply a complete terrain quad.
template<class Height>
int retailTerrainHeight(int32_t x,int32_t z,int width,int height,Height sample) {
    const int px=x>>16,pz=z>>16,cx=px>>4,cz=pz>>4;
    if (cx<0 || cz<0 || cx+1>=width || cz+1>=height) return -1;
    const int fx=px&15,fz=pz&15;
    const int a=sample(cx,cz),b=sample(cx+1,cz);
    const int c=sample(cx,cz+1),d=sample(cx+1,cz+1);
    const int top=a+(b-a)*fx/16,bottom=c+(d-c)*fx/16;
    return top+(bottom-top)*fz/16;
}

inline int32_t retailFloatingHeight(uint8_t sea,uint8_t waterline) {
    return (int(sea)-int(waterline))*65536;
}

// 51b220: upright hoverers clamp to sea-waterline; ordinary upright movers
// use the terrain result, including its -1 boundary sentinel.
inline int32_t retailUprightHeight(int terrain,bool hover,uint8_t sea,uint8_t waterline) {
    return (hover?std::max(terrain,int(sea)-int(waterline)):terrain)*65536;
}

using RetailGroundSupport=std::array<std::array<int32_t,2>,4>;

// 51af18: caller supplies the already terrain-adjusted base speed and the
// observed clock for this corner. Keep wall-clock reads out of lockstep code.
inline int retailHoverHeightCorner(int terrain,uint8_t sea,size_t corner,
        uint32_t clock,uint16_t phase,int32_t speed,int32_t effectiveSpeed,uint32_t elapsed) {
    const int32_t half=effectiveSpeed/2;
    const int32_t capped=std::min(speed,half);
    // Native converts to int64, then keeps EAX. Invalid 0/0 converts to the
    // int64 indefinite value, whose low word is zero.
    const int64_t quotient=half?int64_t(capped)*65536/half:0;
    const int32_t ratio=std::bit_cast<int32_t>(uint32_t(quotient));
    int32_t amplitude=2-int32_t((int64_t(ratio)*2)>>16);
    amplitude-=int32_t((std::min(elapsed,60u)*uint32_t(amplitude))/60u);
    const auto angle=uint16_t(((clock&31u)+uint32_t(corner)*8u)*2048u+phase);
    return std::max(terrain,int(sea))+retailScaledCosine(angle,amplitude);
}

// Height portion of 51ad20. Support points use the native model's mirrored X/Z
// coordinates and native heading. Hover bobbing, when active, adjusts each
// sampled corner through the caller; its clock is an explicit external input.
// Missing quads leave height and optional pitch/roll outputs intact.
template<class Height,class Adjust>
int32_t retailSupportedHeight(int32_t x,int32_t z,int32_t oldY,uint16_t heading,
        const RetailGroundSupport& support,Height terrain,Adjust adjust,
        int32_t bankScale=65536,int32_t pitchScale=65536,
        uint16_t* pitch=nullptr,uint16_t* roll=nullptr) {
    std::array<int,4> heights{};
    for (size_t i=0;i<support.size();++i) {
        auto [sx,sz]=support[i];
        retailRotatePair(sx,sz,heading);
        const auto px=std::bit_cast<int32_t>(uint32_t(x)+uint32_t(sx));
        const auto pz=std::bit_cast<int32_t>(uint32_t(z)-uint32_t(sz));
        const int h=terrain(px,pz);
        if (h<0) return oldY;
        heights[i]=adjust(i,h);
    }
    const int front=(heights[0]+heights[1])/2,back=(heights[2]+heights[3])/2;
    auto span=[](int32_t a,int32_t b,int32_t scale) {
        const auto difference=std::bit_cast<int32_t>(uint32_t(a)-uint32_t(b));
        const uint32_t magnitude=difference<0?0u-uint32_t(difference):uint32_t(difference);
        const int pixels=std::bit_cast<int16_t>(uint16_t(magnitude>>16));
        return int32_t(int64_t(pixels)*scale/65536);
    };
    if (pitch) *pitch=uint16_t(retailDirection(Fixed::raw(back-front),
        Fixed::raw(span(support[0][1],support[3][1],pitchScale))).v);
    if (roll) *roll=uint16_t(retailDirection(Fixed::raw(heights[0]-heights[1]),
        Fixed::raw(span(support[0][0],support[1][0],bankScale))).v);
    const int h=(front+back)/2;
    return std::bit_cast<int32_t>((uint32_t(h)<<16)|(uint32_t(oldY)&0xffff));
}

} // namespace tak::sim
