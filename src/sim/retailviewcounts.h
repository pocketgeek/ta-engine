#pragma once

#include "sim/retailexploration.h"
#include <array>
#include <cmath>
#include <cstdlib>

namespace tak::sim {

// floor(sqrt(v)) for v >= 0, exact whatever the sqrt rounding.
inline int retailIsqrt(int v) {
    int s=int(std::sqrt(double(v)));
    while (s>0 && s*s>v) --s;
    while ((s+1)*(s+1)<=v) ++s;
    return s;
}

// The cells of a row that retailSightFootprint's traversal can admit: its box
// clipped to the map, less the cells beyond the disc, which retailSightAdmits
// rejects before it reads terrain (the 3x3 centre lies inside the disc once
// the radius is 2; below that the box is all centre). lo > hi: none. The
// disc's half-width is corrected from the last row asked for; over rows taken
// in order it only grows to the centre row and shrinks after it, so a whole
// footprint costs one square root (its first row) and at most twice its
// radius in steps.
class RetailSightRows {
public:
    RetailSightRows(const RetailSightFootprint& sight,int radius,int width)
        : x_(sight.x),z_(sight.z),radius_(radius),width_(width) {}
    void span(int z,int& lo,int& hi) {
        lo=1;hi=0;
        const int dz=z-z_;
        if (radius_<0 || dz<-radius_ || dz>radius_) return;
        int half=radius_;
        if (radius_>=2) {
            const int room=radius_*radius_-dz*dz;
            if (half_<0) half_=retailIsqrt(room);
            while (half_*half_>room) --half_;
            while ((half_+1)*(half_+1)<=room) ++half_;
            half=half_;
        }
        lo=std::max(0,x_-half);hi=std::min(width_-1,x_+half);
    }
private:
    int x_,z_,radius_,width_,half_=-1;
};

// retailSightAdmits for the cells of a row span, by the lowest of a cell's two
// terrain heights: a cell is admitted when its squared distance is at most
// limit[height]. The centre's 3x3 (squared distance 2 or less) always is; else
// the eye must not be below the cell, and the cell must lie within the cone's
// reach -- the same double products and comparison, so for a whole squared
// distance d, d > reach*reach exactly when d > floor(reach*reach). The limit
// only falls as the terrain rises.
using RetailSightLimits=std::array<int32_t,256>;
inline void retailSightLimits(const RetailSightFootprint& sight,int radiusSquared,float slope,
        RetailSightLimits& limit) {
    for (int m=255;m>=0;--m) {
        const int delta=sight.eyeHeight-m;
        int t=radiusSquared;
        if (delta<0) t=-1;
        else if (sight.sightHeight) {
            const double reach=double(delta)*slope;
            const double reachSquared=reach*reach;
            if (reachSquared<double(radiusSquared)) t=int(reachSquared);
            else {   // the cone covers the disc here and on all lower ground
                for (int lower=m;lower>=0;--lower) limit[size_t(lower)]=std::max(2,radiusSquared);
                return;
            }
        }
        limit[size_t(m)]=std::max(2,t);
    }
}

// Moves a counted footprint to `to` (the same distance and sight height) on
// one count plane, visiting only the cells whose admission changes: -1 where
// only `from` admits, +1 where only `to` does; cells both admit keep their
// count. The plane ends exactly as after removing `from` and adding `to`
// (counts are sums; the visit order differs, which a sum cannot see).
template<class HeightAt,class Visit>
void retailSightMove(const RetailSightFootprint& from,const RetailSightFootprint& to,
        int width,int height,HeightAt& heightAt,Visit& visit) {
    const int radius=retailSightRadius(from);
    if (radius<0) return;
    const int radiusSquared=radius*radius;
    const float slope=retailSightSlope(from);
    RetailSightLimits fromLimit,toLimitOwn;
    retailSightLimits(from,radiusSquared,slope,fromLimit);
    const bool sameEye=from.eyeHeight==to.eyeHeight;
    if (!sameEye) retailSightLimits(to,radiusSquared,slope,toLimitOwn);
    const RetailSightLimits& toLimit=sameEye ? fromLimit : toLimitOwn;
    auto lowest=[&](int x,int z) {
        const auto heights=heightAt(x,z);
        return size_t(std::min(heights[0],heights[1]));
    };
    RetailSightRows fromRows(from,radius,width),toRows(to,radius,width);
    const int z0=std::max(0,std::min(int(from.z),int(to.z))-radius);
    const int z1=std::min(height-1,std::max(int(from.z),int(to.z))+radius);
    for (int z=z0;z<=z1;++z) {
        int fromLo,fromHi,toLo,toHi;
        fromRows.span(z,fromLo,fromHi);
        toRows.span(z,toLo,toHi);
        // Distances only inside a footprint's own span (far from its centre
        // they could overflow).
        const int fromDz=z-int(from.z),toDz=z-int(to.z);
        const int fromDz2=fromDz*fromDz,toDz2=toDz*toDz;
        auto fromAdmits=[&](int x,size_t m) {
            const int dx=x-int(from.x);
            return dx*dx+fromDz2<=fromLimit[m];
        };
        auto toAdmits=[&](int x,size_t m) {
            const int dx=x-int(to.x);
            return dx*dx+toDz2<=toLimit[m];
        };
        // Cells only `from` spans lose its count where it admits them, cells
        // only `to` spans gain where it does, cells both span change where the
        // two disagree.
        auto fromOnly=[&](int lo,int hi) {
            for (int x=lo;x<=hi;++x) if (fromAdmits(x,lowest(x,z))) visit(x,z,-1,uint16_t(0));
        };
        auto toOnly=[&](int lo,int hi) {
            for (int x=lo;x<=hi;++x) if (toAdmits(x,lowest(x,z))) visit(x,z,1,uint16_t(0));
        };
        const int bothLo=std::max(fromLo,toLo),bothHi=std::min(fromHi,toHi);
        if (bothLo>bothHi) {   // disjoint spans, or one empty
            fromOnly(fromLo,fromHi);toOnly(toLo,toHi);
            continue;
        }
        fromOnly(fromLo,bothLo-1);toOnly(toLo,bothLo-1);   // one of the two is empty
        for (int x=bothLo;x<=bothHi;++x) {
            const size_t m=lowest(x,z);
            const bool was=fromAdmits(x,m),now=toAdmits(x,m);
            if (was!=now) visit(x,z,now ? 1 : -1,uint16_t(0));
        }
        fromOnly(bothHi+1,fromHi);toOnly(bothHi+1,toHi);    // likewise
    }
}

// 4c6c00 / 4c6a70 for the current-view counts: retailUpdateSight with
// exploration off -- the same trigger, cached footprint and counts after it --
// but a counted footprint whose new centre is in the map moves by
// retailSightMove instead of a full removal and re-add.
template<class HeightAt,class Visit>
void retailUpdateSightCounts(RetailSightFootprint& sight,Fixed x,Fixed y,Fixed z,
        uint8_t sea,int width,int height,uint8_t player,HeightAt heightAt,Visit visit) {
    const int cx=x.floorInt()/32,cz=z.floorInt()/32;
    const int eye=std::max(y.floorInt(),int(sea)+1)+sight.sightHeight;
    if (cx==sight.x && cz==sight.z && std::abs(sight.eyeHeight-eye)<=5) return;
    if (!sight.active || uint32_t(cx)>=uint32_t(width) || uint32_t(cz)>=uint32_t(height)) {
        retailUpdateSight(sight,x,y,z,sea,false,width,height,player,heightAt,visit);
        return;
    }
    RetailSightFootprint next=sight;
    next.x=int16_t(cx);next.z=int16_t(cz);next.eyeHeight=eye;
    retailSightMove(sight,next,width,height,heightAt,visit);
    sight=next;
}

} // namespace tak::sim
