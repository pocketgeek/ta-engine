#pragma once

#include "sim/retailexploration.h"
#include <array>
#include <cmath>
#include <cstdlib>

namespace ta::sim {

// Moves a counted footprint to `to` (the same sight distance, so the same
// mask) on one count plane, visiting only the cells whose coverage changes:
// -1 where only `from` covers, +1 where only `to` does. The plane ends exactly
// as after removing `from` and adding `to` (counts are sums; the visit order
// differs, which a sum cannot see).
template<class HeightAt,class Visit>
void retailSightMove(const RetailSightFootprint& from,const RetailSightFootprint& to,
        int width,int height,HeightAt& heightAt,Visit& visit) {
    const RetailVisMask& m=retailSightMask(from);
    const int fx=from.x-m.xoff,fz=from.z-m.yoff,tx=to.x-m.xoff,tz=to.z-m.yoff;
    const int x0=std::max(0,std::min(fx,tx)),x1=std::min(width-1,std::max(fx,tx)+m.width-1);
    const int z0=std::max(0,std::min(fz,tz)),z1=std::min(height-1,std::max(fz,tz)+m.height-1);
    (void)heightAt;
    for (int z=z0;z<=z1;++z)
        for (int x=x0;x<=x1;++x) {
            const bool was=m.at(x-fx,z-fz),now=m.at(x-tx,z-tz);
            if (was!=now) visit(x,z,now ? 1 : -1,uint16_t(0));
        }
}

// retailUpdateSight for the current-view counts: the same trigger and cached
// footprint, but a counted footprint whose new centre is in the map moves by
// retailSightMove instead of a full removal and re-add.
template<class HeightAt,class Visit>
void retailUpdateSightCounts(RetailSightFootprint& sight,Fixed x,Fixed y,Fixed z,
        uint8_t sea,int width,int height,uint8_t player,HeightAt heightAt,Visit visit) {
    if (sight.trueLos) {   // rays: removed and added whole
        retailUpdateSight(sight,x,y,z,sea,false,width,height,player,heightAt,visit);
        return;
    }
    int cx,cz;retailSightCentre(x,y,z,cx,cz);
    const int eye=std::max(y.floorInt(),int(sea)+1)+sight.sightHeight;
    if (cx==sight.x && cz==sight.z) return;
    if (!sight.active || uint32_t(cx)>=uint32_t(width) || uint32_t(cz)>=uint32_t(height)) {
        retailUpdateSight(sight,x,y,z,sea,false,width,height,player,heightAt,visit);
        return;
    }
    RetailSightFootprint next=sight;
    next.x=int16_t(cx);next.z=int16_t(cz);next.eyeHeight=int16_t(eye);
    retailSightMove(sight,next,width,height,heightAt,visit);
    sight=next;
}

} // namespace ta::sim
