#pragma once
// Legion convoys (docs/legion-pathfinding.md "Convoys"): the orders one click
// gives a selection are one convoy, even when the client's 64-per-tick uplink
// lands them over several ticks.
//
// The client gives every Legion-shared mover (legionSharedClick) the click
// point itself and every other unit the click plus its offset from the
// selection centroid, clamped to +-60 px per axis (a flyer: retail's spread,
// sim/retailflyerspread.h, up to sqrt(3000 n) px -- one sent per unit beyond
// this table's reach opens a convoy of its own; an explicit order joins once,
// at the click, whatever its members' points). The server cannot recover
// the centroid, so a convoy is anchored on its first SHARED point (the click)
// and every order is tested against it per axis, never as a Euclidean
// distance:
//   - a shared order within 1 cell of the anchor;
//   - a non-shared order within 60 px + 1 cell of the anchor;
//   - before any anchor exists, a shared order within 60 px + 1 cell and a
//     non-shared one within 120 px + 1 cell of EVERY point already taken
//     (tested on the convoy's bounding box).
// A convoy is open while now <= last + kGapTicks and now <= first + kSpanTicks.
// An order matching several open convoys of its player and class takes the
// nearest anchor (or first point while unanchored) by Chebyshev distance,
// then the lowest `first`, then the lowest id; otherwise it opens a convoy.
// Matching runs in the server's command-application order.
//
// Lookup is by index, not by a scan of the open convoys (crowdbench and the AI
// give thousands of distinct points in one tick). `fine` holds the anchored
// convoys by anchor cell (16 px): a shared order probes the 3x3 cells round
// its own (reach 1 cell), a non-shared one the 11x11 (reach 76 px, under 5
// cells). `coarse` holds the unanchored convoys by the 256 px tile of their
// first point, probed 3x3 by every order (reach at most 136 px, under one
// tile); a convoy moves from coarse to fine when it gets its anchor. Each
// probe is one row of cells or tiles (an ordered range), and it and every
// candidate tested count in Stats::tests: a shared order costs 6 probes, a
// non-shared one 14, plus the convoys near it.
//
// Explicit orders (Legion 10,000 stage 3). A selection order
// (net::Cmd::Order) knows its exact membership, so it joins the table ONCE, at
// its own click point as a shared order, for all its members (World::
// stampIdentity), instead of once per unit. The table is then the ARRIVAL AREA
// table: several orders of one player and class to one point within the window
// (wall-4x50's four selections on ticks 1-4) still share one area, as the user's
// decision A1 has it, while one order is never split however many units it has.
// Every convoy carries its area key: an area an explicit order opens is keyed
// kExplicitArea | serial (unique, never a tick); one a per-unit command opens
// (the AI, scripts) keeps its first tick, as before.
//
// Hashed state (World::stateHash folds checksum() while the table is not
// empty): the table only. The indexes are derived from it and checked against
// a rebuild by indexesMatch() under TAK_LEGION_VERIFY.
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>

namespace tak::sim {

// The order classes a convoy keeps apart: a fight and a patrol sent to one
// point a moment apart stay separate orders.
enum class ConvoyClass : uint8_t {Move=0,Fight=1,Patrol=2};

// An order leg's identity under Legion (Order::convoyTick / orderSerial): its
// arrival area key and the serial of the explicit order that gave it (0: none).
struct OrderIdentity {
    uint32_t convoyTick=0xffffffffu;
    uint32_t serial=0;
};
// The tag of an explicit order's area key and pinwheel part (serials are at
// most net::kOrderMaxSerial, so tag | serial is never kNone and never a tick).
constexpr uint32_t kExplicitArea=0x80000000u;
constexpr bool explicitArea(uint32_t key) {return key!=0xffffffffu&&(key&kExplicitArea)!=0;}

class ConvoyTable {
public:
    static constexpr uint32_t kNone=0xffffffffu;   // Order::convoyTick unset
    // Up to 8 ticks with no command between two orders of one convoy (a
    // 512-command uplink window with a round trip of 16 ticks resumes 9 ticks
    // after its last full tick), and at most 33 ticks of commands in all.
    static constexpr uint32_t kGapTicks=9,kSpanTicks=32;
    static constexpr int32_t kCell=16<<16;                    // raw Fixed
    static constexpr int32_t kSharedReach=kCell;              // shared vs anchor
    static constexpr int32_t kOffsetReach=(60<<16)+kCell;     // offset vs anchor; shared vs every point
    static constexpr int32_t kSpreadReach=(120<<16)+kCell;    // offset vs every point
    static constexpr int kFineShift=20,kCoarseShift=24;       // 16 px cells, 256 px tiles
    static constexpr int kOffsetCells=5;                      // kOffsetReach in cells, rounded up

    struct Convoy {
        uint32_t id=0,first=0,last=0;
        uint32_t key=0;             // the area key (see above): `first`, or kExplicitArea | serial
        int player=0;
        ConvoyClass cls=ConvoyClass::Move;
        bool anchored=false;
        int32_t ax=0,az=0;          // anchor (raw Fixed), valid when anchored
        int32_t fx=0,fz=0;          // first point
        int32_t minX=0,maxX=0,minZ=0,maxZ=0;   // every point taken
    };
    // Observation only, never hashed: per joined order, the index probes plus
    // candidates tested.
    struct Stats {
        uint64_t orders=0,tests=0,testsMax=0,over16=0,over64=0;
    };

    // Join the matching open convoy or open one; returns it. `key`, when not
    // kNone, is the area key a convoy this join opens gets (an explicit order's).
    const Convoy& join(int player,ConvoyClass cls,int32_t x,int32_t z,bool shared,uint32_t now,uint32_t key=kNone);
    // The convoy join() would pick, by a scan of the whole table (nullptr: it
    // would open one). The reference the indexes are checked against.
    const Convoy* bruteMatch(int player,ConvoyClass cls,int32_t x,int32_t z,bool shared,uint32_t now) const;
    // Erase every convoy no order at `now` can join. Idempotent per tick.
    void prune(uint32_t now);
    void clear();
    // Can an order still join an area of `player` keyed `key` (any class)?
    // Legion's deferred slot assignment (A2) waits while it can. A tick key is
    // looked up by its first tick, as before; an explicit key (few areas are
    // open at once) by a scan of the player's convoys.
    bool joinable(int player,uint32_t key,uint32_t now) const {
        if(explicitArea(key)) {
            for(auto it=table_.lower_bound({player,0,0,0});it!=table_.end()&&std::get<0>(it->first)==player;++it)
                if(it->second.key==key&&open(it->second,now))return true;
            return false;
        }
        for(uint8_t cls=0;cls<3;++cls)
            for(auto it=table_.lower_bound({player,cls,key,0});it!=table_.end()&&
                std::get<0>(it->first)==player&&std::get<1>(it->first)==cls&&std::get<2>(it->first)==key;++it)
                if(open(it->second,now)&&it->second.key==key)return true;
        return false;
    }
    bool empty() const {return table_.empty();}
    size_t size() const {return table_.size();}
    uint64_t checksum() const;
    // Both indexes equal a rebuild from the table, and every convoy is open at `now`.
    bool indexesMatch(uint32_t now) const;
    const Stats& stats() const {return stats_;}
    // Test hook: every open convoy, in hash order (player, class, first, id).
    template<class F> void forEach(F&& f) const {for(const auto& [k,c]:table_)f(c);}

private:
    using Key=std::tuple<int,uint8_t,uint32_t,uint32_t>;                       // player, class, first, id
    using FineKey=std::tuple<int,uint8_t,int32_t,int32_t,uint32_t,uint32_t>;   // player, class, cz, cx, first, id
    using CoarseKey=std::tuple<int,uint8_t,int32_t,int32_t,uint32_t,uint32_t>; // player, class, tz, tx, first, id
    static Key keyOf(const Convoy& c) {return {c.player,uint8_t(c.cls),c.first,c.id};}
    static FineKey fineOf(const Convoy& c);
    static CoarseKey coarseOf(const Convoy& c);
    static bool open(const Convoy& c,uint32_t now) {return now<=c.last+kGapTicks&&now<=c.first+kSpanTicks;}
    static bool accepts(const Convoy& c,int32_t x,int32_t z,bool shared);
    // Is a better than b for an order at (x, z)?
    static bool nearer(const Convoy& a,const Convoy& b,int32_t x,int32_t z);
    void index(const Convoy& c);
    void unindex(const Convoy& c);

    std::map<Key,Convoy> table_;
    std::set<FineKey> fine_;
    std::set<CoarseKey> coarse_;
    uint32_t nextId_=1;
    uint32_t prunedAt_=kNone;   // a cache of the last prune(now); never hashed
    Stats stats_;
};

}
