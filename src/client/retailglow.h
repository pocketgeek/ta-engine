#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace tak {

struct RetailGlowVertex { float x,y; uint32_t color; };
struct RetailGlowEnvelope { int32_t duration,begin,end; };
inline RetailGlowEnvelope retailGlowEnvelope(unsigned kind) {
    switch(kind) {
    case 0:return {15,64,8};
    case 1:return {22,128,16};
    default:return {22,200,32};
    }
}


// Native 492139 uses 53-bit x87 intermediates in this order. Keep stores
// explicit to prevent contraction/reassociation across the truncation boundary.
inline std::array<int32_t,2> retailGlowRadii(int32_t elapsed,int32_t duration,
        int32_t begin,int32_t end) {
    if(duration<=0)return {0,0};
    volatile double fraction=double(elapsed)/double(duration);
    volatile double scaled=fraction*double(int64_t(end)-begin);
    volatile double value=scaled+double(begin);
    const int32_t diameter=int32_t(value);
    return {diameter/2,int32_t(double(diameter)*0.75)/2};
}


// Native 4916d0 hardware glow: center plus a closed, twelve-edge ellipse.
inline std::array<RetailGlowVertex,14> retailGlowMesh(float x,float y,
        float radiusX,float radiusY,uint8_t alpha) {
    std::array<RetailGlowVertex,14> result{};
    result[0]={x,y,(uint32_t(alpha)<<24)|0xffffffu};
    const float a=0.85f,b=0.45f;
    const float ay=std::sqrt(1-a*a),by=std::sqrt(1-b*b);
    const std::array<std::array<float,2>,13> ring={{
        {1,0},{a,ay},{b,by},{0,1},{-b,by},{-a,ay},{-1,0},
        {-a,-ay},{-b,-by},{0,-1},{b,-by},{a,-ay},{1,0}}};
    for(unsigned i=0;i<ring.size();++i)
        result[i+1]={x+ring[i][0]*radiusX,y+ring[i][1]*radiusY,0xffffffu};
    return result;
}

} // namespace tak
