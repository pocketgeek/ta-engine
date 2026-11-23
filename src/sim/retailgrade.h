#pragma once
#include <cstdint>
#include <algorithm>
#include <span>

namespace tak::sim {

// 508bc1..508c6d: passable terrain can still carry the low-priority grade 4.
// Roads suppress the slope penalty, but not the preferred-depth penalty.
constexpr int retailTerrainGrade(int low,int high,int sea,int maxDepth,int minDepth,
        int badMaxDepth,int badMinDepth,int maxSlope,int badSlope,
        int maxWaterSlope,int badWaterSlope,bool road) {
    if (low<sea-maxDepth || high>sea-minDepth) return 0;
    const int slope=high-low;
    const int hard=low<sea ? maxWaterSlope : maxSlope;
    const int soft=low<sea ? badWaterSlope : badSlope;
    if (slope>soft && slope>hard) return 0;
    if (slope>soft && !road) return 4;
    if (low<sea-badMaxDepth || high>sea-badMinDepth) return 4;
    return road ? 7 : 6;
}

// 508cd0: the cached plane includes a one-cell clearance border, graded 4
// when any border strip is below 6. Keep the four strip queries in order:
// their corner ownership is asymmetric and the routine short-circuits.
template<class RateRect>
int retailCachedFootprintGrade(int x,int z,int footX,int footZ,RateRect rate) {
    const int grade=rate(x,z,footX,footZ);
    if (uint32_t(grade)<=4) return grade;
    if (uint32_t(rate(x-1,z-1,footX+1,1))<6) return 4;
    if (uint32_t(rate(x+footX,z-1,1,footZ+1))<6) return 4;
    if (uint32_t(rate(x,z+footZ,footX+1,1))<6) return 4;
    if (uint32_t(rate(x-1,z,1,footZ+1))<6) return 4;
    return grade;
}

// 508b74..508bbe: ordinary allocated bodies in the cached raw rectangle
// query. Seven leaves the accumulated grade unchanged. Special blockers are
// handled by the preceding branch and must not be passed to this helper.
constexpr int retailCachedBodyGrade(bool hasMover,bool requester,uint32_t stamp,
                                    uint32_t recent,uint32_t stale) {
    if (!hasMover) return 0;
    if (requester) return 7;
    if (stamp<stale) return 0;
    return stamp<recent ? 2 : 7;
}

struct RetailGradeContext {
    int width=0, height=0, footX=1, footZ=1;
    int startX=0, startZ=0, player=0, retry=0;
    bool specialBlockerProbe=false;
};

struct RetailGradeBody {
    int slot=0;
    uint32_t flags=0, stamp=0;
    bool hasMover=false;
    int x=0,z=0,footX=1,footZ=1;
};

struct RetailGradePreparation {
    uint32_t recent=0, stale=0;
    int requestSlot=0;

    // 4e1ee0. Bodies are supplied in ascending retail allocation-slot order.
    // stamp is mover+28, not the separate local scan timestamp at mover+30.
    template<class Refresh,class Age>
    void prepare(uint32_t tick,const RetailGradeBody& requester,
                 std::span<const RetailGradeBody> bodies,bool lastRetry,
                 Refresh refresh,Age age) {
        const uint32_t oldRecent=recent, oldStale=stale;
        recent=std::max(tick,10u)-10; stale=std::max(tick,150u)-150;
        requestSlot=requester.slot;
        if (lastRetry || requester.stamp<oldRecent) refresh(requester);
        if (lastRetry) {
            for (const auto& b:bodies)
                if ((b.flags&0x1000000) && b.hasMover) refresh(b);
            return;
        }
        if (recent==oldRecent) return;
        for (const auto& b:bodies) {
            if (!(b.flags&0x1000000) || !b.hasMover || (b.flags&3)!=1) continue;
            if (((b.stamp>=oldRecent && b.stamp<recent) ||
                 (b.stamp>=oldStale && b.stamp<stale)) && b.slot!=requester.slot)
                age(b,b.stamp<stale);
        }
    }

    // 4e2060: restore the requesting body's occupancy after the search.
    template<class Age>
    void finish(const RetailGradeBody* requester,Age age) {
        requestSlot=0;
        if (requester && (requester->flags&0x1000000) && requester->stamp<recent)
            age(*requester,requester->stamp<stale);
    }
};

// 4e0300: age an occupied rectangle in a cached grade plane. The border
// becomes at most 4; the interior becomes at most 2, or 0 for stale bodies.
// Bounds for the interior are clipped independently of the border. Callers
// retain the fourth (tag) bit when writing the three-bit grade.
template<class Read,class Write>
void retailAgeGradeRect(const RetailGradeContext& c,int x,int z,int footX,int footZ,
                        bool stale,Read read,Write write) {
    const int left=x-c.footX, top=z-c.footZ, right=x+footX+1, bottom=z+footZ+1;
    if (left>=right || top>=bottom) return;
    const int x0=std::max(0,left), z0=std::max(0,top);
    const auto upper=[](int value,int limit) {
        return uint32_t(value)>uint32_t(limit) ? limit : value;
    };
    const int x1=upper(right,c.width), z1=upper(bottom,c.height);
    const auto lowerGrade=[&](int qx,int qz,int cap) {
        if ((read(qx,qz)&7)>cap) write(qx,qz,cap);
    };
    for (int qx=x0;qx<x1;++qx) {
        lowerGrade(qx,z0,4); lowerGrade(qx,z1-1,4);
    }
    for (int qz=z0+1;qz<z1-1;++qz) {
        lowerGrade(x0,qz,4); lowerGrade(x1-1,qz,4);
    }
    const int ix0=std::max(0,left+1), iz0=std::max(0,top+1);
    const int ix1=upper(right-1,c.width), iz1=upper(bottom-1,c.height);
    for (int qz=iz0;qz<iz1;++qz)
        for (int qx=ix0;qx<ix1;++qx) lowerGrade(qx,qz,stale ? 0 : 2);
}

// 413c80: visibility is sampled on the coarser 32px plane, offset by
// one quarter of the footprint. It is not the visibility of the origin cell.
template<class WordAt>
bool retailSearchVisible(const RetailGradeContext& c, int mapWidth, int mapHeight,
                         int x,int z,WordAt wordAt) {
    if (uint32_t(x)>=uint32_t(c.width) || uint32_t(z)>=uint32_t(c.height)) return false;
    const int vx=x/2+c.footX/4, vz=z/2+c.footZ/4;
    if (vx>=mapWidth/2 || vz>=mapHeight/2) return false;
    return (wordAt(vx,vz) & (uint32_t(1)<<(unsigned(c.player)&31)))!=0;
}

// 4139d0. The third direction argument to the retail routine is unused.
// specialOwner returns -1 unless a map record names an allocated unit with
// type flag 264:40000000; otherwise it returns that unit's owner byte.
template<class Visible,class Cached,class Live,class SpecialOwner>
int retailSearchGrade(const RetailGradeContext& c,int x,int z,Visible visible,
                      Cached cached,Live live,SpecialOwner specialOwner) {
    if (uint32_t(x)>=uint32_t(c.width) || uint32_t(z)>=uint32_t(c.height)) return 0;
    const auto nearStart=[](int value,int start,int foot,int margin) {
        return value>=start-margin && value<=start+foot+margin-1;
    };
    if (!visible(x,z) && !nearStart(x,c.startX,c.footX,1) && !nearStart(z,c.startZ,c.footZ,1))
        return 5;
    const int grade=cached(x,z)&7;
    if (grade==2 && c.retry<=1 &&
        !nearStart(x,c.startX,c.footX,9) && !nearStart(z,c.startZ,c.footZ,9))
        return live(x,z);
    if (grade==3 && c.retry<=1 && c.specialBlockerProbe && c.footX==c.footZ) {
        for (int i=0; i<c.footX; ++i) {
            int owner=specialOwner(x+i,z+i);
            if (owner>=0) return owner==c.player ? 4 : 3;
            if (i) {
                owner=specialOwner(x+i,z);
                if (owner>=0) return owner==c.player ? 4 : 3;
                owner=specialOwner(x,z+i);
                if (owner>=0) return owner==c.player ? 4 : 3;
            }
        }
    }
    return grade;
}

} // namespace tak::sim
