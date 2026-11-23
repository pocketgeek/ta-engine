#pragma once
#include "client/renderframe.h"
#include <span>

namespace tak {
// Render-owned, visible work targets. Bounds are footprint origins in 16px
// cells, not sprite bounds; a box can intersect a tree without its centre.
struct OrderTrailTarget {
    int id=0;
    sim::Fixed x,z;
    int cellX=0,cellZ=0,footX=1,footZ=1;
    bool blocks=true;
};
struct OrderTrailSite {
    sim::Fixed x,z;
    bool known=true;
    const sim::UnitType* occupant=nullptr;
    int player=-1;
    bool underConstruction=false;
};
uint64_t orderTrailSignature(std::span<const RenderOrder> orders);
// Expand persistent area jobs into their remaining known stops. This is a
// display prediction: never submits orders or touches the simulation. The sim
// can choose differently when targets move, vanish or become visible.
// `areaVisited` is the first lodestone area's tried deposits (sorted indices
// into `sites`, as sim::ManaBuildArea::visited).
void expandOrderTrail(std::span<const RenderOrder> orders,const sim::UnitType& builder,
    int player,sim::Fixed x,sim::Fixed z,std::span<const OrderTrailTarget> targets,
    std::span<const OrderTrailSite> sites,std::span<const uint32_t> areaVisited,
    std::vector<RenderOrder>& output);
} // namespace tak
