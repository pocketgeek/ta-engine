#pragma once
#include "sim/retailmotion.h"
#include "sim/retailpiecepose.h"
#include <array>
#include <cmath>
#include <optional>

namespace tak {
struct RetailEffectVertex { float x=0,y=0,u=0,v=0; };

// 52ce47..52d171: named lightning's screen-space textured quad. Inputs have
// already undergone signed whole-word projection and camera subtraction.
inline std::optional<std::array<RetailEffectVertex,4>> retailLightningQuad(
        std::array<int32_t,2> start,std::array<int32_t,2> end,int viewportWidth,int viewportHeight,
        int effectWidth,int effectHeight,int textureWidth,int textureHeight) {
    if(start[0]>end[0])std::swap(start,end);
    if(start[0]<-500) {
        if(end[0]==start[0])return {};
        const int ratio=(-500-start[0])/(end[0]-start[0]);
        start[1]+=ratio*(end[1]-start[1]);start[0]=-500;
    }
    if(end[0]>viewportWidth+500) {
        if(end[0]==start[0])return {};
        const int ratio=(end[0]-viewportWidth-500)/(end[0]-start[0]);
        end[1]-=ratio*(end[1]-start[1]);end[0]=viewportWidth+500;
    }
    if(start[1]>end[1])std::swap(start,end);
    if(start[1]<-500) {
        if(end[1]==start[1])return {};
        const int ratio=(-500-start[1])/(end[1]-start[1]);
        start[0]+=ratio*(end[0]-start[0]);start[1]=-500;
    }
    if(end[1]>viewportHeight+500) {
        if(end[1]==start[1])return {};
        const int ratio=(end[1]-viewportHeight-500)/(end[1]-start[1]);
        end[0]-=ratio*(end[0]-start[0]);end[1]=viewportHeight+500;
    }
    const int32_t dx=end[0]-start[0],dy=end[1]-start[1];
    const auto direction=sim::retailDirection(sim::Fixed::raw(dy),sim::Fixed::raw(dx));
    const int32_t square=std::bit_cast<int32_t>(uint32_t(dx)*uint32_t(dx)+uint32_t(dy)*uint32_t(dy));
    // Native's signed squared-distance overflow can feed NaN to its 64-bit
    // float-to-integer helper; that helper's low 32-bit result is then zero.
    const int32_t length=square<0 ? 0 : int32_t(std::sqrt(double(square)));
    const int half=effectHeight/2;
    std::array<std::array<int32_t,2>,4> corners{{{0,-half},{length,-half},{length,half},{0,half}}};
    const float u=float(effectWidth)/float(textureWidth),v=float(effectHeight)/float(textureHeight);
    const std::array<std::array<float,2>,4> uv{{{0,v},{u,v},{u,0},{0,0}}};
    std::array<RetailEffectVertex,4> result;
    for(size_t i=0;i<4;++i) {
        auto point=corners[i];sim::retailRotatePair(point[0],point[1],uint16_t(direction.v));
        result[i]={float(point[0]+start[0]),float(point[1]+start[1]),uv[i][0],uv[i][1]};
    }
    return result;
}
} // namespace tak
