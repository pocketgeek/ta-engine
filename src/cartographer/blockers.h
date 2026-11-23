#pragma once

// The BLOCKED tool's edits of the TNT feature plane (no SDL; unit-tested).
//
// 0xFFFC is the game's hard blocker (tnt.h): impassable for every unit that does
// not fly, in both pathfinding modes, and unbuildable. Retail Cartographer had no
// tool for it -- blockers only came with stamped sections -- so painting them is
// our own addition. Paint turns only EMPTY cells (0xFFFF) into blockers and erase
// turns only blockers back into empty ground: named features, roads (0xFFFB) and
// every other special value are never touched.

#include "tnt/tnt.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace cart {

constexpr uint16_t kCellEmpty=0xFFFF,kCellRoad=0xFFFB,kCellBlocked=0xFFFC;
enum class BlockOp {Paint,Erase};
// Square brushes, in 16px cells.
inline constexpr std::array<int,5> kBlockedBrushSizes{1,2,4,8,16};

struct CellRect {int x0=0,z0=0,x1=0,z1=0;};   // inclusive cell corners

// Paint or erase every cell of the inclusive rectangle (corners in any order),
// clipped to the map. Returns the number of cells changed.
inline int applyBlockers(tak::tnt::Map& map,int x0,int z0,int x1,int z1,BlockOp op) {
    if(map.width<=0 || map.height<=0 || map.features.size()!=size_t(map.width)*size_t(map.height))return 0;
    const int lx=std::max(0,std::min(x0,x1)),hx=std::min(map.width-1,std::max(x0,x1));
    const int lz=std::max(0,std::min(z0,z1)),hz=std::min(map.height-1,std::max(z0,z1));
    const uint16_t from=op==BlockOp::Paint?kCellEmpty:kCellBlocked,to=op==BlockOp::Paint?kCellBlocked:kCellEmpty;
    int changed=0;
    for(int z=lz;z<=hz;++z)for(int x=lx;x<=hx;++x) {
        uint16_t& v=map.features[size_t(z)*map.width+x];
        if(v==from) {v=to;++changed;}
    }
    return changed;
}

// The cells a brush of `size` covers when centred on cell (cx,cz): an even size
// puts the extra row and column after the centre.
inline CellRect brushCells(int cx,int cz,int size) {
    size=std::max(1,size);
    return {cx-(size-1)/2,cz-(size-1)/2,cx+size/2,cz+size/2};
}

// The brush stamped at every cell of the line a->b, so a fast drag leaves no gaps.
// The line steps along x or z, never both at once: it is 4-connected, so a wall
// drawn with the smallest brush has no diagonal chinks.
inline int applyBlockerLine(tak::tnt::Map& map,int ax,int az,int bx,int bz,int size,BlockOp op) {
    const int nx=std::abs(bx-ax),nz=std::abs(bz-az),sx=bx<ax?-1:1,sz=bz<az?-1:1;
    auto stamp=[&](int x,int z) {const auto b=brushCells(x,z,size);return applyBlockers(map,b.x0,b.z0,b.x1,b.z1,op);};
    int changed=stamp(ax,az);
    for(int ix=0,iz=0;ix<nx || iz<nz;) {
        if((1+2*ix)*nz<(1+2*iz)*nx) {ax+=sx;++ix;} else {az+=sz;++iz;}   // the axis whose next cell boundary is nearer
        changed+=stamp(ax,az);
    }
    return changed;
}

inline int countBlocked(const tak::tnt::Map& map) {
    return int(std::count(map.features.begin(),map.features.end(),kCellBlocked));
}

} // namespace cart
