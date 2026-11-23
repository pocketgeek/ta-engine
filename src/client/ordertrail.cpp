#include "client/ordertrail.h"
#include "sim/footprint.h"
#include <algorithm>
#include <limits>
#include <numeric>
#include <set>
#include <unordered_set>

namespace tak {
namespace {
using sim::Fixed;
constexpr int64_t step=16*int64_t(Fixed::kOne);
// Exact squared 16.16 distance, as the sim ranks build clearing and deposits.
uint64_t exact(Fixed x,Fixed z,Fixed qx,Fixed qz) {
    const int64_t dx=int64_t(x.v)-qx.v,dz=int64_t(z.v)-qz.v;
    return uint64_t(dx*dx)+uint64_t(dz*dz);
}
int64_t distance(int64_t x,int64_t z,int64_t qx,int64_t qz) {
    const int64_t dx=x-qx,dz=z-qz;
    // Same high-half scoring and ties as ReclaimArea within the map's normal
    // coordinate range. Keep 64-bit sums in this display-only index.
    const uint64_t ax=uint64_t(dx<0 ? -dx : dx),az=uint64_t(dz<0 ? -dz : dz);
    return int64_t((ax*ax>>32)+(az*az>>32));
}
struct Bounds {
    int64_t x0,z0,x1,z1;
    bool intersects(const Bounds& b) const {
        return x0<=b.x1 && x1>=b.x0 && z0<=b.z1 && z1>=b.z0;
    }
};
Bounds footprint(const OrderTrailTarget& t) {
    return {t.cellX*step,t.cellZ*step,(int64_t(t.cellX)+t.footX)*step-1,
            (int64_t(t.cellZ)+t.footZ)*step-1};
}
// Balanced spatial index over intersecting footprint sample rectangles. A
// nearest lookup removes ONE feature, not one cell, so big footprints cost no
// more than small ones. O(n log n) planning avoids a grid scan per future job
// (and avoids quadratic nearest-feature scans on enormous area clears).
class AreaTargets {
    struct Node {
        Bounds own,all;
        const OrderTrailTarget* target=nullptr;
        int left=-1,right=-1,parent=-1,count=1;
        bool active=true;
    };
    std::vector<Node> nodes_;
    int root_=-1;
    int build(int lo,int hi,int parent,int axis) {
        if(lo>=hi)return -1;
        const int mid=lo+(hi-lo)/2;
        std::nth_element(nodes_.begin()+lo,nodes_.begin()+mid,nodes_.begin()+hi,
            [axis](const Node& a,const Node& b) {
                const auto ac=axis ? a.own.z0+a.own.z1 : a.own.x0+a.own.x1;
                const auto bc=axis ? b.own.z0+b.own.z1 : b.own.x0+b.own.x1;
                return ac!=bc ? ac<bc : a.target->id<b.target->id;
            });
        auto& n=nodes_[size_t(mid)];n.parent=parent;
        n.left=build(lo,mid,mid,!axis);n.right=build(mid+1,hi,mid,!axis);
        n.all=n.own;
        for(int i:{n.left,n.right})if(i>=0) {
            const auto& c=nodes_[size_t(i)];n.count+=c.count;
            n.all.x0=std::min(n.all.x0,c.all.x0);n.all.x1=std::max(n.all.x1,c.all.x1);
            n.all.z0=std::min(n.all.z0,c.all.z0);n.all.z1=std::max(n.all.z1,c.all.z1);
        }
        return mid;
    }
    static int64_t lower(const Bounds& b,int64_t x,int64_t z) {
        return distance(x,z,std::clamp(x,b.x0,b.x1),std::clamp(z,b.z0,b.z1));
    }
    void nearest(int index,int64_t x,int64_t z,int& best,int64_t& score,int64_t& bx,int64_t& bz) const {
        if(index<0)return;
        const auto& n=nodes_[size_t(index)];
        if(!n.count || lower(n.all,x,z)>score)return;
        if(n.active) {
            // Check both neighbours on each axis: quantized squared distance
            // can tie either side of a midpoint. Native ties choose z then x.
            const auto& b=n.own;
            const int64_t qx=b.x0+std::clamp((x-b.x0)/step,int64_t(0),(b.x1-b.x0)/step)*step;
            const int64_t qz=b.z0+std::clamp((z-b.z0)/step,int64_t(0),(b.z1-b.z0)/step)*step;
            for(int64_t sz:{qz,std::min(qz+step,b.z1)})
                for(int64_t sx:{qx,std::min(qx+step,b.x1)}) {
                    const auto d=distance(x,z,sx,sz);
                    if(best<0 || d<score || (d==score && (sz<bz || (sz==bz &&
                        (sx<bx || (sx==bx && n.target->id<nodes_[size_t(best)].target->id)))))) {
                        best=index;score=d;bx=sx;bz=sz;
                    }
                }
        }
        int a=n.left,b=n.right;
        if(a<0 || (b>=0 && lower(nodes_[size_t(b)].all,x,z)<lower(nodes_[size_t(a)].all,x,z)))
            std::swap(a,b);
        nearest(a,x,z,best,score,bx,bz);nearest(b,x,z,best,score,bx,bz);
    }
public:
    AreaTargets(const RenderOrder& area,std::span<const OrderTrailTarget> targets,
                const std::unordered_set<int>& claimed) {
        const Bounds box{area.x.v,area.z.v,area.buildX.v,area.buildZ.v};
        nodes_.reserve(targets.size());
        for(const auto& t:targets) {
            if(!t.id || claimed.contains(t.id))continue;
            auto b=footprint(t);
            if(!b.intersects(box))continue;
            // Snap to the area's own 16px sampling grid, including fractional
            // click coordinates. Only samples inside the footprint qualify.
            b.x0=box.x0+std::max(int64_t(0),(b.x0-box.x0+step-1)/step)*step;
            b.z0=box.z0+std::max(int64_t(0),(b.z0-box.z0+step-1)/step)*step;
            b.x1=box.x0+(std::min(b.x1,box.x1)-box.x0)/step*step;
            b.z1=box.z0+(std::min(b.z1,box.z1)-box.z0)/step*step;
            if(b.x0<=b.x1 && b.z0<=b.z1)nodes_.push_back({b,b,&t});
        }
        root_=build(0,int(nodes_.size()),-1,0);
    }
    const OrderTrailTarget* take(Fixed x,Fixed z) {
        int best=-1;int64_t score=std::numeric_limits<int64_t>::max(),bx=0,bz=0;
        nearest(root_,x.v,z.v,best,score,bx,bz);
        if(best<0)return nullptr;
        auto& n=nodes_[size_t(best)];n.active=false;
        for(int i=best;i>=0;i=nodes_[size_t(i)].parent)--nodes_[size_t(i)].count;
        return n.target;
    }
};
bool upgrade(const OrderTrailSite& site,const sim::UnitType* type,int player) {
    if(!type || type->id.size()!=7)return false;
    const auto faction=type->id.substr(0,3);
    if(faction!="ara" && faction!="tar" && faction!="ver" && faction!="zon" && faction!="cre")return false;
    return site.occupant && type && !site.underConstruction && site.player==player &&
        type->id.size()==7 && type->id.ends_with("mana") &&
        site.occupant->id==type->id.substr(0,3)+"lode" && site.occupant->side==type->side;
}
} // namespace

uint64_t orderTrailSignature(std::span<const RenderOrder> orders) {
    uint64_t h=14695981039346656037ull;
    const auto add=[&](uint64_t v){h^=v;h*=1099511628211ull;};
    for(const auto& o:orders)if(o.goal) {
        for(auto v:{o.x.v,o.z.v,o.clickX.v,o.clickZ.v,o.buildX.v,o.buildZ.v,
                    o.targetId,o.reclaimFeat,o.repairTarget})add(uint32_t(v));
        add(reinterpret_cast<uintptr_t>(o.buildType));add(o.issuedTick);add(o.areaNextSpot);
        add(o.load | o.unload<<1 | o.attackMove<<2 | o.patrol<<3 | o.guard<<4 |
            o.reclaimArea<<5 | o.buildRectangle<<6 | o.manaBuildArea<<7 |
            o.areaExploring<<8 | o.areaApproached<<9 | o.revive<<10);
    }
    return h;
}

void expandOrderTrail(std::span<const RenderOrder> orders,const sim::UnitType& builder,
    int player,Fixed x,Fixed z,std::span<const OrderTrailTarget> targets,
    std::span<const OrderTrailSite> sites,std::span<const uint32_t> areaVisited,
    std::vector<RenderOrder>& output) {
    output.clear();
    bool visitedUsed=false;
    std::unordered_set<int> claimed;
    std::set<std::pair<int32_t,int32_t>> plannedSites;
    const auto append=[&](RenderOrder o){output.push_back(o);x=o.x;z=o.z;};
    const auto reclaim=[&](const OrderTrailTarget& t,uint32_t tick) {
        RenderOrder o;o.goal=true;o.x=t.x;o.z=t.z;o.reclaimFeat=t.id;o.issuedTick=tick;
        o.trailPreview=true;append(o);claimed.insert(t.id);
    };
    for(const auto& o:orders) {
        if(!o.goal)continue;
        if(o.reclaimArea) {
            AreaTargets pending(o,targets,claimed);
            bool approach=!o.areaApproached;
            for(;;) {
                const auto ax=Fixed::raw(std::clamp(x.v,o.x.v,o.buildX.v));
                const auto az=Fixed::raw(std::clamp(z.v,o.z.v,o.buildZ.v));
                if(approach && (ax!=x || az!=z)) {
                    RenderOrder move;move.goal=true;move.x=ax;move.z=az;
                    move.issuedTick=o.issuedTick;move.trailPreview=true;append(move);
                }
                const auto* next=pending.take(x,z);
                if(!next)break;
                reclaim(*next,o.issuedTick);approach=true;
            }
        } else if(o.manaBuildArea && o.buildType) {
            // Same greedy nearest-first choice as World::tickManaBuildArea:
            // from the previous stop, exact squared distance, then spot index.
            // Only the first area job has progress (`areaVisited`); later ones
            // have not started. Reachability ranking and other builders'
            // claims are sim-only, so the prediction can differ there.
            std::vector<size_t> pending;
            for(size_t i=0;i<sites.size();++i) {
                const auto& s=sites[i];
                if(!s.known || s.x<o.x || s.x>o.buildX || s.z<o.z || s.z>o.buildZ ||
                    (s.occupant && !upgrade(s,o.buildType,player)) ||
                    (!visitedUsed && std::binary_search(areaVisited.begin(),areaVisited.end(),uint32_t(i))) ||
                    plannedSites.contains({s.x.v,s.z.v}))continue;
                pending.push_back(i);
            }
            visitedUsed=true;
            while(!pending.empty()) {
                const auto next=std::min_element(pending.begin(),pending.end(),[&](size_t a,size_t b) {
                    const auto da=exact(x,z,sites[a].x,sites[a].z),db=exact(x,z,sites[b].x,sites[b].z);
                    return da!=db ? da<db : a<b;
                });
                const auto& s=sites[*next];pending.erase(next);
                if(!plannedSites.emplace(s.x.v,s.z.v).second)continue;
                const auto bx=sim::footprintWaypoint(sim::footprintCell(s.x,o.buildType->footX),o.buildType->footX);
                const auto bz=sim::footprintWaypoint(sim::footprintCell(s.z,o.buildType->footZ),o.buildType->footZ);
                const int ox=sim::footprintOrigin(bx,o.buildType->footX),oz=sim::footprintOrigin(bz,o.buildType->footZ);
                const Bounds site{ox*step,oz*step,(int64_t(ox)+o.buildType->footX)*step-1,
                    (int64_t(oz)+o.buildType->footZ)*step-1};
                std::vector<const OrderTrailTarget*> clearing;
                for(const auto& t:targets) {
                    if(!t.blocks || claimed.contains(t.id) || !footprint(t).intersects(site))continue;
                    // Ignore the building's '.' yard cells, like placement does.
                    bool hit=o.buildType->yardMap.empty();
                    for(int cz=std::max(oz,t.cellZ);!hit && cz<std::min(oz+o.buildType->footZ,t.cellZ+t.footZ);++cz)
                        for(int cx=std::max(ox,t.cellX);cx<std::min(ox+o.buildType->footX,t.cellX+t.footX);++cx)
                            if(o.buildType->yardMap[size_t(cz-oz)*o.buildType->footX+cx-ox]!='.') {hit=true;break;}
                    if(hit)clearing.push_back(&t);
                }
                // World::queueBuild's clearing order: exact squared distance
                // from where the builder stands, then feature id.
                const auto fromX=x,fromZ=z;
                std::sort(clearing.begin(),clearing.end(),[&](const auto* a,const auto* b) {
                    const auto da=exact(fromX,fromZ,a->x,a->z),db=exact(fromX,fromZ,b->x,b->z);
                    return da!=db ? da<db : a->id<b->id;
                });
                for(const auto* t:clearing)reclaim(*t,o.issuedTick);
                RenderOrder build;build.goal=true;build.buildType=o.buildType;build.buildRectangle=true;
                build.buildX=bx;build.buildZ=bz;build.clickX=bx;build.clickZ=bz;
                build.issuedTick=o.issuedTick;build.trailPreview=true;
                if(builder.canFly) {build.x=bx;build.z=bz;}
                else {
                    const sim::RetailRectGoal rect{ox-builder.footX,ox+o.buildType->footX,
                        oz-builder.footZ,oz+o.buildType->footZ};
                    const auto [cx,cz]=rect.navigationCell(sim::footprintOrigin(x,builder.footX),sim::footprintOrigin(z,builder.footZ));
                    build.x=Fixed::fromInt(cx*16+builder.footX*8);build.z=Fixed::fromInt(cz*16+builder.footZ*8);
                }
                append(build);
            }
        } else {
            append(o);
            if(o.reclaimFeat)claimed.insert(o.reclaimFeat);
            if(o.buildType)plannedSites.emplace(o.buildX.v,o.buildZ.v);
        }
    }
}
} // namespace tak
