#pragma once
#include "client/renderframe.h"
#include <span>

namespace ta {
// Render-owned, visible work targets. Bounds are footprint origins in 16px
// cells, not sprite bounds; a box can intersect a tree without its centre.
struct OrderTrailTarget {
    int id=0;
    sim::Fixed x,z;
    int cellX=0,cellZ=0,footX=1,footZ=1;
    bool blocks=true;
};
uint64_t orderTrailSignature(std::span<const RenderOrder> orders);
// Expand persistent area jobs (the area reclaim) into their remaining known
// stops. This is a display prediction: never submits orders or touches the
// simulation. The sim can choose differently when targets move, vanish or
// become visible.
void expandOrderTrail(std::span<const RenderOrder> orders,const sim::UnitType& builder,
    int player,sim::Fixed x,sim::Fixed z,std::span<const OrderTrailTarget> targets,
    std::vector<RenderOrder>& output);
} // namespace ta
