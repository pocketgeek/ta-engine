#include "sim/convoy.h"

#include <algorithm>
#include <limits>

namespace tak::sim {

namespace {
constexpr uint32_t kMax=std::numeric_limits<uint32_t>::max();
bool within(int32_t v,int32_t c,int32_t d) {return int64_t(v)>=int64_t(c)-d&&int64_t(v)<=int64_t(c)+d;}
}

ConvoyTable::FineKey ConvoyTable::fineOf(const Convoy& c) {
    return {c.player,uint8_t(c.cls),c.az>>kFineShift,c.ax>>kFineShift,c.first,c.id};
}
ConvoyTable::CoarseKey ConvoyTable::coarseOf(const Convoy& c) {
    return {c.player,uint8_t(c.cls),c.fz>>kCoarseShift,c.fx>>kCoarseShift,c.first,c.id};
}

bool ConvoyTable::accepts(const Convoy& c,int32_t x,int32_t z,bool shared) {
    if(c.anchored) {
        const int32_t d=shared?kSharedReach:kOffsetReach;
        return within(x,c.ax,d)&&within(z,c.az,d);
    }
    // Within d of every point taken: maxX - d <= x <= minX + d, and for z.
    const int32_t d=shared?kOffsetReach:kSpreadReach;
    return int64_t(c.maxX)-d<=x&&int64_t(x)<=int64_t(c.minX)+d&&
           int64_t(c.maxZ)-d<=z&&int64_t(z)<=int64_t(c.minZ)+d;
}

bool ConvoyTable::nearer(const Convoy& a,const Convoy& b,int32_t x,int32_t z) {
    auto chebyshev=[&](const Convoy& c) {
        const int64_t rx=c.anchored?c.ax:c.fx,rz=c.anchored?c.az:c.fz;
        const int64_t dx=rx>x?rx-x:x-rx,dz=rz>z?rz-z:z-rz;
        return std::max(dx,dz);
    };
    const int64_t da=chebyshev(a),db=chebyshev(b);
    if(da!=db)return da<db;
    if(a.first!=b.first)return a.first<b.first;
    return a.id<b.id;
}

void ConvoyTable::index(const Convoy& c) {
    if(c.anchored)fine_.insert(fineOf(c));
    else coarse_.insert(coarseOf(c));
}
void ConvoyTable::unindex(const Convoy& c) {
    if(c.anchored)fine_.erase(fineOf(c));
    else coarse_.erase(coarseOf(c));
}

void ConvoyTable::prune(uint32_t now) {
    if(prunedAt_==now)return;
    prunedAt_=now;
    for(auto it=table_.begin();it!=table_.end();) {
        if(open(it->second,now)) {++it;continue;}
        unindex(it->second);
        it=table_.erase(it);
    }
}

void ConvoyTable::clear() {
    table_.clear();fine_.clear();coarse_.clear();
    nextId_=1;prunedAt_=kNone;stats_={};
}

const ConvoyTable::Convoy* ConvoyTable::bruteMatch(int player,ConvoyClass cls,int32_t x,int32_t z,bool shared,
                                                   uint32_t now) const {
    const Convoy* best=nullptr;
    for(const auto& [k,c]:table_)
        if(c.player==player&&c.cls==cls&&open(c,now)&&accepts(c,x,z,shared)&&(!best||nearer(c,*best,x,z)))best=&c;
    return best;
}

const ConvoyTable::Convoy& ConvoyTable::join(int player,ConvoyClass cls,int32_t x,int32_t z,bool shared,uint32_t now,
                                              uint32_t key) {
    prune(now);
    const uint8_t k=uint8_t(cls);
    uint64_t tests=0;
    const Convoy* best=nullptr;
    auto consider=[&](uint32_t first,uint32_t id) {
        ++tests;
        const Convoy& c=table_.find(Key{player,k,first,id})->second;
        if(open(c,now)&&accepts(c,x,z,shared)&&(!best||nearer(c,*best,x,z)))best=&c;
    };
    // Anchored convoys: an anchor within the order's reach (1 cell shared,
    // 76 px offset) has its cell within 1 (or 5) cells of the order's.
    const int32_t cx=x>>kFineShift,cz=z>>kFineShift,reach=shared?1:kOffsetCells;
    for(int32_t r=cz-reach;r<=cz+reach;++r) {
        ++tests;
        auto it=fine_.lower_bound(FineKey{player,k,r,cx-reach,0,0});
        const auto end=fine_.upper_bound(FineKey{player,k,r,cx+reach,kMax,kMax});
        for(;it!=end;++it)consider(std::get<4>(*it),std::get<5>(*it));
    }
    // Unanchored convoys: the order must be within at most 136 px of every
    // point, the first included, so the first point's tile is within one.
    const int32_t tx=x>>kCoarseShift,tz=z>>kCoarseShift;
    for(int32_t r=tz-1;r<=tz+1;++r) {
        ++tests;
        auto it=coarse_.lower_bound(CoarseKey{player,k,r,tx-1,0,0});
        const auto end=coarse_.upper_bound(CoarseKey{player,k,r,tx+1,kMax,kMax});
        for(;it!=end;++it)consider(std::get<4>(*it),std::get<5>(*it));
    }
    ++stats_.orders;stats_.tests+=tests;stats_.testsMax=std::max(stats_.testsMax,tests);
    stats_.over16+=tests>16;stats_.over64+=tests>64;
    if(best) {
        Convoy& c=table_.find(keyOf(*best))->second;
        c.last=now;
        c.minX=std::min(c.minX,x);c.maxX=std::max(c.maxX,x);
        c.minZ=std::min(c.minZ,z);c.maxZ=std::max(c.maxZ,z);
        if(shared&&!c.anchored) {
            unindex(c);
            c.anchored=true;c.ax=x;c.az=z;
            index(c);
        }
        return c;
    }
    Convoy c;
    c.id=nextId_++;c.first=c.last=now;c.player=player;c.cls=cls;c.key=key==kNone?now:key;
    c.anchored=shared;c.ax=shared?x:0;c.az=shared?z:0;
    c.fx=x;c.fz=z;c.minX=c.maxX=x;c.minZ=c.maxZ=z;
    const auto& placed=table_.emplace(keyOf(c),c).first->second;
    index(placed);
    return placed;
}

uint64_t ConvoyTable::checksum() const {
    uint64_t h=1469598103934665603ULL;
    auto mix=[&h](uint64_t v) {
        for(int i=0;i<8;++i) {h^=(v>>(i*8))&0xff;h*=1099511628211ULL;}
    };
    mix(table_.size());
    for(const auto& [k,c]:table_) {
        mix(uint32_t(c.player));mix(uint8_t(c.cls));mix(c.first);mix(c.id);mix(c.last);
        mix(c.anchored);
        if(c.anchored) {mix(uint32_t(c.ax));mix(uint32_t(c.az));}
        mix(uint32_t(c.fx));mix(uint32_t(c.fz));
        mix(uint32_t(c.minX));mix(uint32_t(c.maxX));mix(uint32_t(c.minZ));mix(uint32_t(c.maxZ));
        if(c.key!=c.first) {mix(0x41524541ull);mix(c.key);}   // an explicit area's key
    }
    mix(nextId_);
    return h;
}

bool ConvoyTable::indexesMatch(uint32_t now) const {
    std::set<FineKey> fine;
    std::set<CoarseKey> coarse;
    for(const auto& [k,c]:table_) {
        if(k!=keyOf(c)||!open(c,now))return false;
        if(c.anchored)fine.insert(fineOf(c));
        else coarse.insert(coarseOf(c));
    }
    return fine==fine_&&coarse==coarse_;
}

}
