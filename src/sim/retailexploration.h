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

// TotalA.exe's True sight (docs/retail-fog.md section 2.3): rays from
// gamedata\los.tdf (loader 0x433130). [TABLEINFO] numtables gives how many of
// [TABLE1].. are read; each table's lines list cells (dx, dz) of one quadrant,
// "count, dx, dz, ...", and 0x4336f0 stores every line four times, rotated a
// quarter turn each: line i of n is ray q*n + i with cells (a, -b), (b, a),
// (-a, b), (-b, -a) for q = 0..3.
using RetailLosRay=std::vector<std::array<int8_t,2>>;
using RetailLosTable=std::vector<RetailLosRay>;
inline std::vector<RetailLosTable>& retailLosTableData() { static std::vector<RetailLosTable> tables; return tables; }
inline void setRetailLosTables(std::vector<RetailLosTable> tables) { retailLosTableData()=std::move(tables); }

struct RetailSightFootprint {
    int16_t x=-1,z=-1;      // the centre cell (projected, 32 px)
    int16_t eyeHeight=0;    // True sight: the eye byte (0..255); Circular: unused (16 bits keeps Unit's layout)
    int16_t distance=0;     // sightdistance, px
    uint8_t sightHeight=0;
    bool active=false;
    bool trueLos=false;     // stamped with True sight's rays, else Circular's mask
    // DOUBLE SIGHT/RADAR (an engine option, not retail): 2 doubles the footprint
    // past retail's caps -- True's rays scaled x2 from the undoubled sight's table,
    // Circular's disc past the 14-cell mask (retailSightMask). 1: retail exactly.
    uint8_t scale=1;
};

// 0x481e09..0x481e52: True sight's table for a sight distance, r = sight/32
// (truncated, at least 0) and at most numtables - 1, element r - 1 (0x433500)
// -- TABLE r. Below 32 px there is no table (retail reads element -1): the
// centre cell alone.
inline const RetailLosTable* retailLosTable(const RetailSightFootprint& sight) {
    const auto& tables=retailLosTableData();
    if (tables.empty()) return nullptr;
    // Doubled sight picks the table of the unit's own sight; its rays are scaled.
    const int sightPx=int(sight.distance)/std::max(1,int(sight.scale));
    const int r=std::min(std::max(0,sightPx/32),int(tables.size())-1);
    return r>=1 ? &tables[size_t(r-1)] : nullptr;
}

// 0x482270: the cells a True footprint counts. The centre always (0x48239c);
// then along each ray, step j = 1, 2, ..., the horizon (h, d) starting at
// (-1, 0): a cell on the plane whose top rises above it, (hi - e)*d > h*j, is
// counted, and then its bottom raises the horizon when (lo - e)*d > h*j
// (0x4823fc..0x48248e). Off-plane cells are skipped. A cell several rays reach
// is counted once per ray.
template<class HeightAt,class Visit>
void retailTrueSightCells(const RetailSightFootprint& sight,int width,int height,HeightAt& heightAt,Visit visit) {
    if (uint32_t(sight.x)>=uint32_t(width) || uint32_t(sight.z)>=uint32_t(height)) return;
    visit(int(sight.x),int(sight.z));
    const RetailLosTable* table=retailLosTable(sight);
    if (!table) return;
    const int e=sight.eyeHeight;
    // DOUBLE SIGHT (scale 2, engine option): every cell of the ray at twice its
    // offset, with the midpoint of each pair of neighbours between them, so the
    // doubled ray is as continuous as the authored one and twice as long.
    thread_local std::vector<std::array<int,2>> steps;
    for (const RetailLosRay& ray:*table) {
        steps.clear();
        if (sight.scale>1) {
            std::array<int,2> prev{0,0};
            for (const auto& c:ray) {
                steps.push_back({prev[0]+c[0],prev[1]+c[1]});   // the midpoint: (2·prev + 2·c) / 2
                steps.push_back({2*c[0],2*c[1]});
                prev={c[0],c[1]};
            }
        } else for (const auto& c:ray) steps.push_back({c[0],c[1]});
        int32_t bestH=-1,bestD=0,j=1;
        for (const auto& step:steps) {
            const int x=sight.x+step[0],z=sight.z+step[1];
            if (uint32_t(x)<uint32_t(width) && uint32_t(z)<uint32_t(height)) {
                const auto h=heightAt(x,z);
                const int32_t hi=int32_t(h[0])-e,lo=int32_t(h[1])-e;
                if (hi*bestD>bestH*j) {
                    visit(x,z);
                    if (lo*bestD>bestH*j) {bestD=j;bestH=lo;}
                }
            }
            ++j;
        }
    }
}

struct RetailSightVisitAll {
    bool operator()(int, int, bool) const { return true; }
};

// 0x481840..0x481868: the Circular mask frame for a sight distance. With DOUBLE
// SIGHT (scale 2, engine option) a sight past the largest mask (14 cells) gets a
// disc of its own radius, sight/32 cells (at most 28), drawn as the fallback
// masks are: dx^2 + dz^2 <= r^2 + r.
inline const RetailVisMask& retailBigDisc(int r) {
    static const auto discs=[] {
        std::array<RetailVisMask,14> d;   // radius 15..28
        for (int k=0;k<14;++k) {
            const int rr=15+k,n=2*rr+1;
            RetailVisMask& m=d[size_t(k)];
            m.width=m.height=int16_t(n);m.xoff=m.yoff=int16_t(rr);m.on.assign(size_t(n)*n,0);
            for (int j=0;j<n;++j) for (int i=0;i<n;++i)
                m.on[size_t(j)*n+i]=(i-rr)*(i-rr)+(j-rr)*(j-rr)<=rr*rr+rr;
        }
        return d;
    }();
    return discs[size_t(std::clamp(r,15,28)-15)];
}
inline const RetailVisMask& retailSightMask(const RetailSightFootprint& sight) {
    if (sight.scale>1 && int(sight.distance)/32>14) return retailBigDisc(int(sight.distance)/32);
    return retailVisMaskTable()[size_t(std::clamp(int(sight.distance)/32-5,0,9))];
}
// The farthest a footprint reaches from its centre cell, for dirty rectangles.
inline int retailSightRadius(const RetailSightFootprint& sight) {
    if (sight.trueLos) {
        int reach=0;
        if (const RetailLosTable* t=retailLosTable(sight))
            for (const auto& ray:*t) for (const auto& c:ray) reach=std::max({reach,std::abs(int(c[0])),std::abs(int(c[1]))});
        return reach*std::max(1,int(sight.scale));
    }
    const RetailVisMask& m=retailSightMask(sight);
    return std::max({int(m.xoff),int(m.width)-1-m.xoff,int(m.yoff),int(m.height)-1-m.yoff});
}
// Kept for callers of the old cone: Circular sight has no slope.
inline float retailSightSlope(const RetailSightFootprint&) { return 0; }
// Is cell (x, z) under the footprint's mask? (The other arguments are the old
// cone's; Circular reads no terrain.)
template<class HeightAt>
inline bool retailSightAdmits(const RetailSightFootprint& sight,int x,int z,int,float,HeightAt& heightAt) {
    if (sight.trueLos) {
        bool hit=false;
        retailTrueSightCells(sight,1<<15,1<<15,heightAt,[&](int cx,int cz){hit=hit || (cx==x && cz==z);});
        return hit;
    }
    const RetailVisMask& m=retailSightMask(sight);
    return m.at(x-sight.x+m.xoff,z-sight.z+m.yoff);
}

// Adds (or removes) a footprint's current-view counts over its mask's cells
// in the map. Exploration is persistent: only the reveal path ORs owner bits.
template<class HeightAt,class Visit,class Needed=RetailSightVisitAll>
void retailSightFootprint(RetailSightFootprint& sight,bool active,bool explore,
        int width,int height,uint8_t player,HeightAt heightAt,Visit visit,Needed needed={}) {
    if (sight.active==active) return;
    const uint16_t mask=explore?uint16_t(uint32_t(1)<<(player&31)):0;
    if (sight.trueLos) {
        retailTrueSightCells(sight,width,height,heightAt,[&](int x,int z) {
            if (needed(x,z,active)) visit(x,z,active?1:-1,mask);
        });
        sight.active=active;
        return;
    }
    const RetailVisMask& m=retailSightMask(sight);
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
// True sight's eye and centre (0x4817f8, 0x4825d4..0x48261c): the eye byte is
// the height, at least sea level + 1, plus the model height, held to 0..255;
// the centre is (x >> 5, (z - eye/2) >> 5) in pixels.
inline void retailTrueSightEye(Fixed x,Fixed y,Fixed z,uint8_t sea,uint8_t sightHeight,int& cx,int& cz,int& eye) {
    eye=std::clamp(std::max(y.floorInt(),int(sea)+1)+int(sightHeight),0,255);
    cx=x.floorInt()>>5;
    cz=(z.floorInt()-eye/2)>>5;
}

// 0x4827b0 -> 0x48272e..0x482746: a footprint is re-stamped only when its
// centre cell moves (the mask follows the sight distance, whose change resets
// the footprint). A centre off the map stamps nothing.
template<class HeightAt,class Visit,class Needed=RetailSightVisitAll>
void retailUpdateSight(RetailSightFootprint& sight,Fixed x,Fixed y,Fixed z,
        uint8_t sea,bool explore,int width,int height,uint8_t player,
        HeightAt heightAt,Visit visit,Needed needed={}) {
    int cx,cz,eye;
    if (sight.trueLos) {
        // 0x48261f..0x48262e: the same cell and the eye within 5 keep the stamp.
        retailTrueSightEye(x,y,z,sea,sight.sightHeight,cx,cz,eye);
        if (cx==sight.x && cz==sight.z && std::abs(sight.eyeHeight-eye)<=5) return;
    } else {
        retailSightCentre(x,y,z,cx,cz);
        eye=std::max(y.floorInt(),int(sea)+1)+sight.sightHeight;
        if (cx==sight.x && cz==sight.z) return;
    }
    retailSightFootprint(sight,false,false,width,height,player,heightAt,visit,needed);
    sight.x=int16_t(cx);sight.z=int16_t(cz);sight.eyeHeight=int16_t(eye);
    if (uint32_t(cx)<uint32_t(width) && uint32_t(cz)<uint32_t(height))
        retailSightFootprint(sight,true,explore,width,height,player,heightAt,visit,needed);
    else if (sight.trueLos) sight.eyeHeight=0;   // 0x482600: off the plane, the eye byte is cleared
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
    int cx,cz,eye;
    if (sight.trueLos) {
        retailTrueSightEye(x,y,z,sea,sight.sightHeight,cx,cz,eye);
        if (cx==sight.x && cz==sight.z && std::abs(sight.eyeHeight-eye)<=5) return;
    } else {
        retailSightCentre(x,y,z,cx,cz);
        eye=std::max(y.floorInt(),int(sea)+1)+sight.sightHeight;
        if (cx==sight.x && cz==sight.z) return;
    }
    sight.active=false;
    sight.x=int16_t(cx);sight.z=int16_t(cz);sight.eyeHeight=int16_t(eye);
    if (uint32_t(cx)>=uint32_t(width) || uint32_t(cz)>=uint32_t(height)) return;
    sight.active=true;
    if (sight.trueLos) {
        // 0x481930 walks the same rays: hills hide what lies behind them from
        // the map too. Every block a reveal touched is reported once.
        std::vector<std::pair<int,int>> touched;
        retailTrueSightCells(sight,width,height,heightAt,[&](int x,int z) {
            if (covered(x>>blockShift,z>>blockShift) || !needed(x,z)) return;
            reveal(x,z);
            const std::pair<int,int> b{x>>blockShift,z>>blockShift};
            if (std::find(touched.begin(),touched.end(),b)==touched.end()) touched.push_back(b);
        });
        for (const auto& [bx,bz]:touched) blockDone(bx,bz);
        return;
    }
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
