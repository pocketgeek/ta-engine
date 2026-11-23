#pragma once
#include <cstdint>
#include <optional>
#include <utility>

namespace tak::sim {
// ReclaimArea selector (509cc0): sample the rectangle in 16-world-unit rows,
// retain the first sample on ties, and compare the sum of the HIGH halves of
// the two squared signed 16.16 differences. This is not float distance.
// RECLAIMAREA (406670) stages: 0 records the builder's position as the last
// target (+0x22), 1 approaches the clamped point, 2 picks the next target.
struct RetailReclaimArea {
    int32_t minX=0,minZ=0,maxX=0,maxZ=0;
    uint8_t stage=0;
    int32_t lastX=0,lastZ=0;   // mission +0x22, 16.16
};
template<class Eligible>
std::optional<std::pair<int32_t,int32_t>> retailReclaimAreaTarget(
        const RetailReclaimArea& area,int32_t builderX,int32_t builderZ,Eligible eligible) {
    std::optional<std::pair<int32_t,int32_t>> result;
    int32_t best=0;
    for (int64_t z=area.minZ;z<=area.maxZ;z+=16*65536)
        for (int64_t x=area.minX;x<=area.maxX;x+=16*65536) {
            if (!eligible(int32_t(x),int32_t(z))) continue;
            const int64_t dx=int32_t(uint32_t(builderX)-uint32_t(x));
            const int64_t dz=int32_t(uint32_t(builderZ)-uint32_t(z));
            const int32_t distance=int32_t(uint32_t((dx*dx)>>32)+uint32_t((dz*dz)>>32));
            if (!result || distance<best) {result={{int32_t(x),int32_t(z)}};best=distance;}
        }
    return result;
}

// 509d9f..509e6d: whether a sampled cell counts as seen. With LosOn the
// owner's current view decides, without it the Mapping (explored) plane.
// Unseen, a cell still passes when its +7 owner byte is 0xa (a world
// feature, not a corpse) and the cell is explored.
inline bool retailReclaimAreaSeen(bool losOn,bool currentView,bool explored,uint8_t cellOwner) {
    if (losOn ? currentView : explored) return true;
    return cellOwner==0xa && explored;
}

// 4066a3..406792, RECLAIMAREA stage 2 after an approach (arrived or failed):
// no target ends the order (5); otherwise the pick snaps down to its 16 px
// cell, and a pick in the same 50e660 cell as the last target retires the
// order (8) -- the child for that cell just ended without clearing it, or
// (first pick) it is the builder's own cell. 50e660 is NULL off the map, so
// two off-map points compare equal. Otherwise the snapped pick becomes the
// last target and a child RECLAIM goes to it.
enum class RetailReclaimAreaStep : uint8_t { Done, Retire, Child };
struct RetailReclaimAreaPick {
    RetailReclaimAreaStep step=RetailReclaimAreaStep::Done;
    int32_t x=0,z=0;   // the snapped pick (Child)
};
template<class Eligible>
RetailReclaimAreaPick retailReclaimAreaPick(RetailReclaimArea& area,int32_t builderX,int32_t builderZ,
                                            int width,int height,Eligible eligible) {
    const auto point=retailReclaimAreaTarget(area,builderX,builderZ,eligible);
    if (!point) return {};
    const int32_t x=int32_t(uint32_t(point->first)&~0xFFFFFu),z=int32_t(uint32_t(point->second)&~0xFFFFFu);
    const auto cell=[&](int32_t px,int32_t pz)->std::optional<std::pair<int,int>> {
        const int cx=px>>20,cz=pz>>20;
        if (cx<0 || cz<0 || cx>=width || cz>=height) return std::nullopt;
        return std::pair{cx,cz};
    };
    if (cell(area.lastX,area.lastZ)==cell(x,z)) return {RetailReclaimAreaStep::Retire,x,z};
    area.lastX=x;area.lastZ=z;
    return {RetailReclaimAreaStep::Child,x,z};
}
} // namespace tak::sim
