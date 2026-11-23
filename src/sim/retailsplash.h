#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include "sim/fixed.h"

namespace tak::sim {

// Impact dispatch 529c10: a shot that struck a unit with a weapon whose
// areaofeffect (WeaponType +8a, u16) is at most 16 is the direct hit 52a460
// alone -- full damage to that unit, nothing else. Every other impact, a
// bigger blast or any impact with no unit struck, runs the area pass 529dc0.
inline bool retailDirectHitOnly(uint16_t aoe,bool struckUnit) { return struckUnit && aoe<=16; }

// 529dc0's cell window. The radius is aoe>>1. The impact cell comes from the
// whole-pixel words divided by 16 toward zero; the window reaches radius/16+1
// cells either side, clamped to the map, and its far edge is EXCLUSIVE, so it
// stops one cell short on the +x and +z sides.
struct RetailSplashWindow { int x0=0,z0=0,x1=0,z1=0; int32_t radius=0; };
inline RetailSplashWindow retailSplashWindow(int32_t x,int32_t z,uint16_t aoe,int width,int height) {
    auto cell=[](int32_t v) {
        const int pixel=std::bit_cast<int16_t>(uint16_t(uint32_t(v)>>16));
        return (pixel+(pixel<0 ? 15 : 0))>>4;
    };
    RetailSplashWindow window;
    window.radius=int32_t(aoe>>1);
    const int span=window.radius/16+1,cx=cell(x),cz=cell(z);
    window.x0=std::max(cx-span,0);window.z0=std::max(cz-span,0);
    window.x1=std::min(cx+span,width);window.z1=std::min(cz+span,height);
    return window;
}

// 529f97..52a06a: distance from the 3D impact to a unit's bounding box (type
// +13a/+13e/+142 low and +146/+14a/+14e high corner, offsets from the unit's
// position), zero on an axis the impact lies within. 16.16 with wrapping
// adds; x87 sums dz*dz+dy*dy, then dx*dx, at double precision, takes the
// square root, truncates (_ftol 5d3d54) and keeps the whole-pixel word.
inline int32_t retailSplashDistance(const std::array<int32_t,3>& impact,
        const std::array<int32_t,3>& origin,const std::array<int32_t,3>& low,
        const std::array<int32_t,3>& high) {
    std::array<double,3> delta{};
    for(size_t axis=0;axis<3;++axis) {
        // (Saturating, fixed.h: a box corner past the east or south edge of a
        // 2048-cell map stays at the edge, where a wrapped one would sit on
        // the wrong side of every impact.)
        const int32_t a=satAdd32(low[axis],origin[axis]);
        const int32_t b=satAdd32(high[axis],origin[axis]);
        if(impact[axis]<a)delta[axis]=satSub32(a,impact[axis]);
        else if(impact[axis]>b)delta[axis]=satSub32(impact[axis],b);
    }
    const double root=std::sqrt((delta[2]*delta[2]+delta[1]*delta[1])+delta[0]*delta[0]);
    const uint32_t whole=uint32_t(int64_t(root));
    return std::bit_cast<int16_t>(uint16_t(whole>>16));
}

// 52a073..52a0a9: the damage fraction, (d/r-1)^2*(1-edge)+edge at x87
// double precision, stored as a float; exactly 1 at distance 0.
inline float retailSplashFraction(int32_t distance,int32_t radius,float edge) {
    if(distance==0)return 1.0f;
    const double t=double(distance)/double(radius)-1.0;
    return float(t*t*(1.0-double(edge))+double(edge));
}

// The unit side of 529dc0. Cells are visited by row, then column; in each,
// the ground occupant (map word +0) and then the airborne one (word +2).
// A unit is met when it is live (+0x130 bit 0x1000000) and is not the
// shot's owner (+7c). The first 20 units met are remembered and one met
// again is skipped; past 20 nothing more is remembered, so a later unit
// filling several cells of the window is met, and hit, once per cell. A met
// unit within the radius receives `hit(id, fraction)`.
//
// Host: int groundAt(int x,int z), int airAt(int x,int z) -- the word's
// occupant id (0 none, -1 the 0xffff overflow sentinel); bool live(int id);
// RetailSplashBody body(int id).
struct RetailSplashBody { std::array<int32_t,3> origin{},low{},high{}; };
template<class Host,class Hit>
void retailSplashUnits(const std::array<int32_t,3>& impact,uint16_t aoe,float edge,int owner,
        int width,int height,Host& host,Hit hit) {
    const auto window=retailSplashWindow(impact[0],impact[2],aoe,width,height);
    std::array<int,20> met{};
    size_t count=0;
    for(int z=window.z0;z<window.z1;++z)for(int x=window.x0;x<window.x1;++x)
        for(int word=0;word<2;++word) {
            const int id=word ? host.airAt(x,z) : host.groundAt(x,z);
            if(id<=0 || !host.live(id) || id==owner)continue;
            if(std::find(met.begin(),met.begin()+count,id)!=met.begin()+count)continue;
            if(count<met.size())met[count++]=id;
            const auto body=host.body(id);
            const int32_t distance=retailSplashDistance(impact,body.origin,body.low,body.high);
            if(distance<window.radius)hit(id,retailSplashFraction(distance,window.radius,edge));
        }
}
} // namespace tak::sim
