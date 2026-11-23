#pragma once

#include "sim/fixed.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace tak::sim {

using RetailExplorationHeight = std::array<uint8_t,2>;

// 546f40: bind-pose model top, with a zero floor at each recursive sibling
// list. Unit sight takes the low byte of this fixed-point value's high word.
template<class Object>
int32_t retailModelTop(const Object& object) {
    int32_t top=0;
    for (const auto& vertex:object.verticesRaw)
        top=std::max(top,int32_t(uint32_t(vertex[1])+uint32_t(object.offsetRaw[1])));
    if (!object.children.empty()) {
        int32_t childTop=0;
        for (const auto& child:object.children) childTop=std::max(childTop,retailModelTop(child));
        top=std::max(top,int32_t(uint32_t(childTop)+uint32_t(object.offsetRaw[1])));
    }
    return top;
}

// 50ea58..50ed53: the projected coarse height plane consumed by exploration.
// Input samples are the map record's height byte, on the original 16px grid.
template<class HeightAt>
std::vector<RetailExplorationHeight> retailExplorationHeights(
        int width,int height,uint8_t sea,HeightAt heightAt) {
    const int cw=width/2,ch=height/2;
    std::vector<RetailExplorationHeight> result(size_t(cw)*ch,{0,255});
    auto index=[&](int x,int z) {
        return uint32_t(x)<uint32_t(cw) && uint32_t(z)<uint32_t(ch) ? z*cw+x : -1;
    };
    auto include=[&](int cell,int value) {
        if (cell<0) return;
        auto& pair=result[size_t(cell)];
        pair[0]=uint8_t(std::max(int(pair[0]),value));
        pair[1]=uint8_t(std::min(int(pair[1]),value));
    };
    for (int x=0;x<width;++x) {
        int left=-1,right=-1;
        for (int z=0;z<height;++z) {
            const int h=heightAt(x,z);
            const int projected=z*16-(h>>1),row=projected>>5;
            if (row>=0) {
                const int boundary=(row*32+31)*h/(projected+31);
                include(left,boundary);include(right,boundary);
                left=index((x-1)>>1,row);
                include(left,boundary);
                right=((x-1)>>1)==(x>>1)?-1:index(x>>1,row);
                include(right,boundary);
            }
            include(left,h);include(right,h);
        }
    }
    for (auto& pair:result) {
        const int high=pair[0],low=pair[1];
        pair={uint8_t(std::max(int(sea),(2*high+low)/3)),
              uint8_t(std::max(int(sea),(high+2*low)/3))};
    }
    return result;
}

struct RetailSightFootprint {
    int16_t x=-1,z=-1;
    int32_t eyeHeight=0;
    int16_t distance=0;
    uint8_t sightHeight=0;
    bool active=false;
};

struct RetailSightVisitAll {
    bool operator()(int, int, bool) const { return true; }
};

inline int retailSightRadius(const RetailSightFootprint& sight) {
    return int(sight.distance)*2/32;
}
inline float retailSightSlope(const RetailSightFootprint& sight) {
    return sight.sightHeight ? float(double(sight.distance)/(int(sight.sightHeight)*32)) : 0;
}

// 4c6800's per-cell admission: the 3x3 centre is always seen; beyond it a
// cell must lie in the disc and below the eye's sight cone.
template<class HeightAt>
inline bool retailSightAdmits(const RetailSightFootprint& sight,int x,int z,
        int radiusSquared,float slope,HeightAt& heightAt) {
    const int dx=x-sight.x,dz=z-sight.z,distanceSquared=dx*dx+dz*dz;
    if (distanceSquared<=2) return true;
    if (distanceSquared>radiusSquared) return false;
    const auto heights=heightAt(x,z);
    const int delta=sight.eyeHeight-std::min(heights[0],heights[1]);
    if (delta<0) return false;
    const double reach=double(delta)*slope;
    return !(sight.sightHeight && distanceSquared>reach*reach);
}

// 4c6800: changing a sight footprint adds/removes current-view reference
// counts. Exploration is persistent: only the reveal path ORs owner bits.
template<class HeightAt,class Visit,class Needed=RetailSightVisitAll>
void retailSightFootprint(RetailSightFootprint& sight,bool active,bool explore,
        int width,int height,uint8_t player,HeightAt heightAt,Visit visit,Needed needed={}) {
    if (sight.active==active) return;
    const int radius=retailSightRadius(sight);
    const int radiusSquared=radius*radius;
    const float slope=retailSightSlope(sight);
    const uint16_t mask=explore?uint16_t(uint32_t(1)<<(player&31)):0;
    for (int z=std::max(0,int(sight.z)-radius);z<=std::min(height-1,int(sight.z)+radius);++z)
        for (int x=std::max(0,int(sight.x)-radius);x<=std::min(width-1,int(sight.x)+radius);++x) {
            if (!needed(x,z,active)) continue;
            if (!retailSightAdmits(sight,x,z,radiusSquared,slope,heightAt)) continue;
            visit(x,z,active?1:-1,mask);
        }
    sight.active=active;
}

// 4c6c00 / 4c6a70: update the cached footprint only after changing cells or
// moving its eye height by more than five. Current sight and exploration
// use the same admitted cells, but exploration is never removed.
template<class HeightAt,class Visit,class Needed=RetailSightVisitAll>
void retailUpdateSight(RetailSightFootprint& sight,Fixed x,Fixed y,Fixed z,
        uint8_t sea,bool explore,int width,int height,uint8_t player,
        HeightAt heightAt,Visit visit,Needed needed={}) {
    const int cx=x.floorInt()/32,cz=z.floorInt()/32;
    const int eye=std::max(y.floorInt(),int(sea)+1)+sight.sightHeight;
    if (cx==sight.x && cz==sight.z && std::abs(sight.eyeHeight-eye)<=5) return;
    retailSightFootprint(sight,false,false,width,height,player,heightAt,visit,needed);
    sight.x=int16_t(cx);sight.z=int16_t(cz);sight.eyeHeight=eye;
    if (uint32_t(cx)<uint32_t(width) && uint32_t(cz)<uint32_t(height))
        retailSightFootprint(sight,true,explore,width,height,player,heightAt,visit,needed);
}

// The exploration-only form of 4c6c00 for callers that keep no current-view
// counts. Dropping a footprint only decrements those counts in 4c6800 and
// exploration is never removed, so the removal traversal is skipped. Revealing
// visits the same admitted cells, grouped into square blocks of
// (1<<blockShift) cells: covered(bx,bz) may skip a whole block whose cells
// already hold every bit the reveal would add, and needed(x,z) one cell. The
// reveal only ORs bits, so the skipped visits and the visit order cannot
// change the result. blockDone(bx,bz) follows each block that was scanned.
template<class HeightAt,class Covered,class Needed,class Reveal,class BlockDone>
void retailUpdateExploration(RetailSightFootprint& sight,Fixed x,Fixed y,Fixed z,
        uint8_t sea,int width,int height,int blockShift,HeightAt heightAt,
        Covered covered,Needed needed,Reveal reveal,BlockDone blockDone) {
    const int cx=x.floorInt()/32,cz=z.floorInt()/32;
    const int eye=std::max(y.floorInt(),int(sea)+1)+sight.sightHeight;
    if (cx==sight.x && cz==sight.z && std::abs(sight.eyeHeight-eye)<=5) return;
    sight.active=false;
    sight.x=int16_t(cx);sight.z=int16_t(cz);sight.eyeHeight=eye;
    if (uint32_t(cx)>=uint32_t(width) || uint32_t(cz)>=uint32_t(height)) return;
    sight.active=true;
    const int radius=retailSightRadius(sight);
    const int radiusSquared=radius*radius;
    const float slope=retailSightSlope(sight);
    const int x0=std::max(0,cx-radius),x1=std::min(width-1,cx+radius);
    const int z0=std::max(0,cz-radius),z1=std::min(height-1,cz+radius);
    const int block=1<<blockShift;
    for (int bz=z0>>blockShift;bz<=z1>>blockShift;++bz)
        for (int bx=x0>>blockShift;bx<=x1>>blockShift;++bx) {
            if (covered(bx,bz)) continue;
            const int zl=std::max(z0,bz*block),zh=std::min(z1,bz*block+block-1);
            const int xl=std::max(x0,bx*block),xh=std::min(x1,bx*block+block-1);
            bool changed=false;
            for (int cellZ=zl;cellZ<=zh;++cellZ)
                for (int cellX=xl;cellX<=xh;++cellX) {
                    if (!needed(cellX,cellZ)) continue;
                    if (!retailSightAdmits(sight,cellX,cellZ,radiusSquared,slope,heightAt)) continue;
                    reveal(cellX,cellZ);changed=true;
                }
            if (changed) blockDone(bx,bz);
        }
}

} // namespace tak::sim
