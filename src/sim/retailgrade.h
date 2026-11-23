#pragma once
#include <cstdint>
#include <algorithm>
#include <span>

namespace ta::sim {

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

    // TotalA.exe 0x440af0 (docs/retail-pathfinding.md 1.6), at every search
    // init for the requester's class: one threshold, max(tick,30) - 30. A body
    // that changed cells since then is transparent (7), older bodies and all
    // structures are walls (0): retailCachedBodyGrade with recent == stale.
    // The requester is made transparent for its own search, and the rectangle
    // of every body whose stamp crossed the threshold is fully re-rated
    // (0x440830). `age` is unused by the TA rule and kept for callers.
    template<class Refresh,class Age>
    void prepare(uint32_t tick,const RetailGradeBody& requester,
                 std::span<const RetailGradeBody> bodies,bool /*lastRetry*/,
                 Refresh refresh,Age /*age*/) {
        const uint32_t old=recent;
        recent=stale=std::max(tick,30u)-30;
        requestSlot=requester.slot;
        if (requester.hasMover) refresh(requester);
        if (recent==old) return;
        for (const auto& b:bodies) {
            if (!(b.flags&0x1000000) || !b.hasMover || b.slot==requester.slot) continue;
            if (b.stamp>=old && b.stamp<recent) refresh(b);
        }
    }

    // 0x440be0: after the search, re-block the requester if its stamp is old.
    template<class Refresh>
    void finish(const RetailGradeBody* requester,Refresh refresh) {
        requestSlot=0;
        if (requester && (requester->flags&0x1000000) && requester->hasMover && requester->stamp<recent)
            refresh(*requester);
    }
};

// 413c80 / TotalA.exe: visibility is sampled on the coarser 32px plane, offset
// by one quarter of the footprint. 1 seen, 0 unexplored, -1 outside the plane.
template<class WordAt>
int retailSearchVisible(const RetailGradeContext& c, int mapWidth, int mapHeight,
                        int x,int z,WordAt wordAt) {
    if (uint32_t(x)>=uint32_t(c.width) || uint32_t(z)>=uint32_t(c.height)) return -1;
    const int vx=x/2+c.footX/4, vz=z/2+c.footZ/4;
    if (vx>=mapWidth/2 || vz>=mapHeight/2) return -1;
    return (wordAt(vx,vz) & (uint32_t(1)<<(unsigned(c.player)&31)))!=0 ? 1 : 0;
}

// TotalA.exe search grade (docs/retail-pathfinding.md D7): out of the map or
// the 32px plane, blocked; unexplored, clear at no cost (6); otherwise the
// cached plane's grade. No live query, no gate probe, no start exemption.
template<class Visible,class Cached,class Live,class SpecialOwner>
int retailSearchGrade(const RetailGradeContext& c,int x,int z,Visible visible,
                      Cached cached,Live /*live*/,SpecialOwner /*specialOwner*/) {
    if (uint32_t(x)>=uint32_t(c.width) || uint32_t(z)>=uint32_t(c.height)) return 0;
    const int seen=int(visible(x,z));
    if (seen<0) return 0;
    if (seen==0) return 6;
    return cached(x,z)&7;
}

} // namespace ta::sim
