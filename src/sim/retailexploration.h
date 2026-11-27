#pragma once

#include "sim/fixed.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>
#include <cstdlib>

namespace ta::sim {

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

// TotalA.exe 0x482c20 (0x4831a6..0x483205): the projected coarse height plane
// True LOS reads (docs/retail-fog.md section 1). Circular sight does not.
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

// TotalA.exe's Circular sight (docs/retail-fog.md section 2.2): a unit stamps
// one of the ten masks of anims\vismasks.gaf sequence `vismask` (loaded at
// 0x429b9e; 11x11 .. 29x29 cells of 32 px, transparent index 9), frame
// clamp(sightdistance/32 - 5, 0, 9), with no terrain test (0x482580 /
// 0x481cc5). The masks are drawn by hand -- the larger ones are not circles of
// any formula -- so match setup loads them (setRetailVisMasks); the fallback,
// for worlds built without game data, is the disc dx^2 + dz^2 <= r^2 + r.
struct RetailVisMask {
    int16_t width=0,height=0,xoff=0,yoff=0;   // xoff/yoff: the centre cell within the mask
    std::vector<uint8_t> on;                  // width*height, 1 = stamped
    bool at(int i,int j) const {
        return uint32_t(i)<uint32_t(width) && uint32_t(j)<uint32_t(height) && on[size_t(j)*width+i];
    }
};
using RetailVisMasks=std::array<RetailVisMask,10>;
inline RetailVisMasks retailDefaultVisMasks() {
    RetailVisMasks masks;
    for (int k=0;k<10;++k) {
        const int r=5+k,n=2*r+1;
        RetailVisMask& m=masks[size_t(k)];
        m.width=m.height=int16_t(n);m.xoff=m.yoff=int16_t(r);m.on.assign(size_t(n)*n,0);
        for (int j=0;j<n;++j) for (int i=0;i<n;++i)
            m.on[size_t(j)*n+i]=(i-r)*(i-r)+(j-r)*(j-r)<=r*r+r;
    }
    return masks;
}
// The process's masks: every World reads the same game data, so every peer
// stamps alike (they are part of the install, like the unit files).
inline RetailVisMasks& retailVisMaskTable() { static RetailVisMasks table=retailDefaultVisMasks(); return table; }
inline void setRetailVisMasks(const RetailVisMasks& masks) { retailVisMaskTable()=masks; }

struct RetailSightFootprint {
    int16_t x=-1,z=-1;      // the centre cell (projected, 32 px)
    int32_t eyeHeight=0;
    int16_t distance=0;     // sightdistance, px
    uint8_t sightHeight=0;
    bool active=false;
};

struct RetailSightVisitAll {
    bool operator()(int, int, bool) const { return true; }
};

// 0x481840..0x481868: the mask frame for a sight distance.
inline const RetailVisMask& retailSightMask(const RetailSightFootprint& sight) {
    return retailVisMaskTable()[size_t(std::clamp(int(sight.distance)/32-5,0,9))];
}
// The farthest the mask reaches from its centre cell, for dirty rectangles.
inline int retailSightRadius(const RetailSightFootprint& sight) {
    const RetailVisMask& m=retailSightMask(sight);
    return std::max({int(m.xoff),int(m.width)-1-m.xoff,int(m.yoff),int(m.height)-1-m.yoff});
}
// Kept for callers of the old cone: Circular sight has no slope.
inline float retailSightSlope(const RetailSightFootprint&) { return 0; }
// Is cell (x, z) under the footprint's mask? (The other arguments are the old
// cone's; Circular reads no terrain.)
template<class HeightAt>
inline bool retailSightAdmits(const RetailSightFootprint& sight,int x,int z,int,float,HeightAt&) {
    const RetailVisMask& m=retailSightMask(sight);
    return m.at(x-sight.x+m.xoff,z-sight.z+m.yoff);
}

// Adds (or removes) a footprint's current-view counts over its mask's cells
// in the map. Exploration is persistent: only the reveal path ORs owner bits.
template<class HeightAt,class Visit,class Needed=RetailSightVisitAll>
void retailSightFootprint(RetailSightFootprint& sight,bool active,bool explore,
        int width,int height,uint8_t player,HeightAt heightAt,Visit visit,Needed needed={}) {
    (void)heightAt;
    if (sight.active==active) return;
    const RetailVisMask& m=retailSightMask(sight);
    const uint16_t mask=explore?uint16_t(uint32_t(1)<<(player&31)):0;
    const int x0=sight.x-m.xoff,z0=sight.z-m.yoff;
    for (int j=std::max(0,-z0);j<m.height && z0+j<height;++j)
        for (int i=std::max(0,-x0);i<m.width && x0+i<width;++i) {
            if (!m.on[size_t(j)*m.width+i]) continue;
            if (!needed(x0+i,z0+j,active)) continue;
            visit(x0+i,z0+j,active?1:-1,mask);
        }
    sight.active=active;
}

// The footprint's centre cell for a unit at (x, y, z): its position projected
// onto the map art, (x >> 21, (z >> 21) - y/64) (0x48186b..0x4818d1).
inline void retailSightCentre(Fixed x,Fixed y,Fixed z,int& cx,int& cz) {
    cx=x.floorInt()>>5;
    cz=(z.floorInt()>>5)-(y.floorInt()>>6);
}

// 0x4827b0 -> 0x48272e..0x482746: a footprint is re-stamped only when its
// centre cell moves (the mask follows the sight distance, whose change resets
// the footprint). A centre off the map stamps nothing.
template<class HeightAt,class Visit,class Needed=RetailSightVisitAll>
void retailUpdateSight(RetailSightFootprint& sight,Fixed x,Fixed y,Fixed z,
        uint8_t sea,bool explore,int width,int height,uint8_t player,
        HeightAt heightAt,Visit visit,Needed needed={}) {
    int cx,cz;retailSightCentre(x,y,z,cx,cz);
    const int eye=std::max(y.floorInt(),int(sea)+1)+sight.sightHeight;
    if (cx==sight.x && cz==sight.z) return;
    retailSightFootprint(sight,false,false,width,height,player,heightAt,visit,needed);
    sight.x=int16_t(cx);sight.z=int16_t(cz);sight.eyeHeight=eye;
    if (uint32_t(cx)<uint32_t(width) && uint32_t(cz)<uint32_t(height))
        retailSightFootprint(sight,true,explore,width,height,player,heightAt,visit,needed);
}

// 0x481930, exploration for callers that keep no current-view counts: the
// same trigger and cells as retailUpdateSight, but exploration is never
// removed, so only the reveal is walked. Its cells are grouped into square
// blocks of (1<<blockShift): covered(bx,bz) may skip a block whose cells
// already hold every bit the reveal would add, and needed(x,z) one cell. The
// reveal only ORs bits, so skips and order cannot change the result.
// blockDone(bx,bz) follows each block that revealed something.
template<class HeightAt,class Covered,class Needed,class Reveal,class BlockDone>
void retailUpdateExploration(RetailSightFootprint& sight,Fixed x,Fixed y,Fixed z,
        uint8_t sea,int width,int height,int blockShift,HeightAt heightAt,
        Covered covered,Needed needed,Reveal reveal,BlockDone blockDone) {
    (void)heightAt;
    int cx,cz;retailSightCentre(x,y,z,cx,cz);
    const int eye=std::max(y.floorInt(),int(sea)+1)+sight.sightHeight;
    if (cx==sight.x && cz==sight.z) return;
    sight.active=false;
    sight.x=int16_t(cx);sight.z=int16_t(cz);sight.eyeHeight=eye;
    if (uint32_t(cx)>=uint32_t(width) || uint32_t(cz)>=uint32_t(height)) return;
    sight.active=true;
    const RetailVisMask& m=retailSightMask(sight);
    const int mx0=cx-m.xoff,mz0=cz-m.yoff;
    const int x0=std::max(0,mx0),x1=std::min(width-1,mx0+m.width-1);
    const int z0=std::max(0,mz0),z1=std::min(height-1,mz0+m.height-1);
    if (x0>x1 || z0>z1) return;
    const int block=1<<blockShift;
    for (int bz=z0>>blockShift;bz<=z1>>blockShift;++bz)
        for (int bx=x0>>blockShift;bx<=x1>>blockShift;++bx) {
            if (covered(bx,bz)) continue;
            const int zl=std::max(z0,bz*block),zh=std::min(z1,bz*block+block-1);
            const int xl=std::max(x0,bx*block),xh=std::min(x1,bx*block+block-1);
            bool changed=false;
            for (int cellZ=zl;cellZ<=zh;++cellZ)
                for (int cellX=xl;cellX<=xh;++cellX) {
                    if (!m.on[size_t(cellZ-mz0)*m.width+(cellX-mx0)]) continue;
                    if (!needed(cellX,cellZ)) continue;
                    reveal(cellX,cellZ);changed=true;
                }
            if (changed) blockDone(bx,bz);
        }
}

} // namespace ta::sim
