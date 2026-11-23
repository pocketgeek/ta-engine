#pragma once

// Parameterized, asset-free comparisons. Observation, traces, and scenario
// mutations are outside the timed World::tick. Include in crowdbench.cpp only.
#include "sim/sim.h"
#include "crowdbench_telemetry.h"
#include "crowdbench_acceptance.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif
#if defined(__unix__)
#include <sys/resource.h>
#endif

namespace crowdbench_allocation {
inline std::atomic<bool> enabled{false};
inline std::atomic<uint64_t> calls{0}, bytes{0};
inline void count(size_t n) {
    if (enabled.load(std::memory_order_relaxed)) {
        calls.fetch_add(1,std::memory_order_relaxed);
        bytes.fetch_add(n,std::memory_order_relaxed);
    }
}
inline void* allocate(size_t n) {
    if (void* p=std::malloc(std::max(size_t(1),n))) {count(n);return p;}
    throw std::bad_alloc();
}
inline void* aligned(size_t n,size_t alignment) {
    if (n>std::numeric_limits<size_t>::max()-alignment) throw std::bad_alloc();
#if defined(_WIN32)
    void* p=_aligned_malloc(std::max(size_t(1),n),alignment);
#else
    const size_t rounded=(std::max(size_t(1),n)+alignment-1)/alignment*alignment;
    void* p=std::aligned_alloc(alignment,rounded);
#endif
    if(p) {count(n);return p;}
    throw std::bad_alloc();
}
inline void alignedFree(void* p) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
}
void* operator new(size_t n) {return crowdbench_allocation::allocate(n);}
void* operator new[](size_t n) {return crowdbench_allocation::allocate(n);}
// The nothrow forms too (std::inplace_merge's buffer uses them): left to the
// library they would come from malloc's arena paired with our std::free --
// harmless, but an alloc-dealloc mismatch under AddressSanitizer.
void* operator new(size_t n,const std::nothrow_t&) noexcept {try {return crowdbench_allocation::allocate(n);} catch (...) {return nullptr;}}
void* operator new[](size_t n,const std::nothrow_t&) noexcept {try {return crowdbench_allocation::allocate(n);} catch (...) {return nullptr;}}
void operator delete(void* p,const std::nothrow_t&) noexcept {std::free(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept {std::free(p);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
void operator delete[](void* p,size_t) noexcept {std::free(p);}
void* operator new(size_t n,std::align_val_t a) {return crowdbench_allocation::aligned(n,size_t(a));}
void* operator new[](size_t n,std::align_val_t a) {return crowdbench_allocation::aligned(n,size_t(a));}
void operator delete(void* p,std::align_val_t) noexcept {crowdbench_allocation::alignedFree(p);}
void operator delete[](void* p,std::align_val_t) noexcept {crowdbench_allocation::alignedFree(p);}
void operator delete(void* p,size_t,std::align_val_t) noexcept {crowdbench_allocation::alignedFree(p);}
void operator delete[](void* p,size_t,std::align_val_t) noexcept {crowdbench_allocation::alignedFree(p);}

namespace tak::sim {
// Existing diagnostic friendship: observe pending ownership without changing it.
struct RetailReplayProbe {
    static bool pending(const World& world,int id) {return world.pathPending(id);}
    static void telemetry(World& world,NavigationTelemetry* observer) {
        world.paths_.setTelemetry(observer);
    }
    // churn: remove the scenario's structure at once (no corpse, no death
    // animation), through the same path a CRT defeat uses.
    static void removeUnits(World& world,int player) {world.removeScenarioUnits(player);}
};
}
namespace crowdbench_matrix {
using namespace tak::sim;
using Clock=std::chrono::steady_clock;
constexpr int restTicks=30;
struct Options {
    std::string mode="retail",scenario="open",trace;
    int units=200,players=1,movingPercent=100,ticks=3600;
    // --turn-rate: the movers' turnRate and turnInPlaceRate (BAM/tick).
    // -1 keeps each type's built-in rate (2500, see mover()).
    int turnRate=-1;
    uint32_t seed=0;
    bool workers=false,allocations=false,profile=false,latency=false;
};
inline int integer(const char* value) {
    size_t consumed=0;const int result=std::stoi(value,&consumed);
    if(consumed!=std::string_view(value).size())throw std::runtime_error("invalid integer");
    return result;
}
inline Options parse(int argc,char** argv) {
    Options o;
    for(int i=1;i<argc;++i) {
        const std::string_view key=argv[i];
        if(key=="--workers") {o.workers=true;continue;}
        if(key=="--allocations") {o.allocations=true;continue;}
        if(key=="--profile") {o.profile=true;continue;}
        if(key=="--latency") {o.latency=true;continue;}
        if(i+1==argc)throw std::runtime_error("option needs a value");
        const char* value=argv[++i];
        if(key=="--mode")o.mode=value;
        else if(key=="--scenario")o.scenario=value;
        else if(key=="--units")o.units=integer(value);
        else if(key=="--players")o.players=integer(value);
        else if(key=="--moving-percent")o.movingPercent=integer(value);
        else if(key=="--ticks")o.ticks=integer(value);
        else if(key=="--trace")o.trace=value;
        else if(key=="--turn-rate") {
            o.turnRate=integer(value);
            if(o.turnRate<1||o.turnRate>32768)throw std::runtime_error("turn-rate must be 1..32768 BAM/tick");
        }
        else if(key=="--seed") {
            size_t count=0;const std::string seed=value;
            const auto parsed=std::stoull(seed,&count);
            if(seed.empty()||seed.front()=='-'||count!=seed.size()||parsed>UINT32_MAX)
                throw std::runtime_error("seed must be an unsigned 32-bit integer");
            o.seed=uint32_t(parsed);
        }
        else throw std::runtime_error("unknown option: "+std::string(key));
    }
    if(o.mode!="retail"&&o.mode!="legion")
        throw std::runtime_error("mode must be retail or legion");
    constexpr std::array names{"open","doors","bridges","maze","opposingcolumns","sharedgoal",
        "mixedfootprints","exploration","dynamicobstacle","rapidreplacement","unreachable","recovery","recovery-passive",
        "jagged","trapped","crowdtrap","singleunit","groupdetour","churn","opposingdoors","opposingbridges","crossingcolumns"};
    if(std::find(names.begin(),names.end(),o.scenario)==names.end())throw std::runtime_error("unknown scenario");
    if(o.units<1||o.units>2000||o.players<1||o.players>8||o.movingPercent<0||o.movingPercent>100||o.ticks<1)
        throw std::runtime_error("units: 1..2000 per player; players: 1..8; moving-percent: 0..100; ticks: positive");
    return o;
}
inline UnitType mover(int footprint,bool exploring,int turnRate=-1) {
    UnitType type{};type.id=type.name="benchmark-foot-"+std::to_string(footprint);
    type.canMove=true;type.maxHp=100;type.footX=type.footZ=footprint;
    type.sight=exploring?192:4096;type.maxVel=Fixed::raw(117964);
    type.accel=type.brake=Fixed::fromInt(10);type.turnRate=type.turnInPlaceRate=turnRate>0?turnRate:2500;
    type.halfCellTicks=3;type.buildTime=1;return type;
}
// churn's structure: a 4x4 building (maxVel 0, so isStructure()) that the
// scenario places and removes, as AI construction and losses do.
inline UnitType churnStructure() {
    UnitType type{};type.id=type.name="benchmark-structure";
    type.canMove=false;type.maxHp=100;type.footX=type.footZ=4;type.sight=64;type.buildTime=1;return type;
}
struct Member {
    int id=0,player=0,epoch=0,commandTick=0,firstMove=-1,firstAdmission=-1,firstRoute=-1,firstPendingClear=-1;
    int rest=0,arrivedTick=-1,retiredTick=-1,crossedTick=-1,stamp=-1;
    int approachTick=-1;   // first tick Legion held this unit at its approach point (state 5)
    int stillSince=0;      // last tick this member moved or had no orders (age classes)
    int initialX=0,initialZ=0,previousX=0,previousZ=0;
    float gx=0,gz=0,alternateX=0,alternateZ=0,radius=32;
    double path=0,straight=0;uint64_t stalled=0;
    bool moving=false,reverse=false,near=false,legal=true,pending=false;
    bool vertical=false;float crossLine=0;   // crossingcolumns' second stream: crosses the first stream's band, north past crossLine (px)
    uint64_t traceControl=~uint64_t(0);
};
struct Rect {int x,z,w,h;};
inline int round64(int n) {return (n+63)/64*64;}
inline double millis(Clock::time_point begin,Clock::time_point end) {
    return std::chrono::duration<double,std::milli>(end-begin).count();
}
inline double percentile(std::vector<double> values,double fraction) {
    if(values.empty())return -1;
    std::sort(values.begin(),values.end());
    return values[size_t(std::ceil(fraction*values.size()))-1];
}
inline int milestone(std::vector<int> times,int count,double fraction) {
    if(!count)return 0;
    std::sort(times.begin(),times.end());
    const size_t index=size_t(std::ceil(fraction*count))-1;
    return index<times.size()?times[index]:-1;
}
inline uint64_t mix(uint64_t h,uint64_t v) {return (h^v)*1099511628211ull;}
inline uint64_t control(const Unit& u) {
    uint64_t h=mix(1469598103934665603ull,u.orders.size());h=mix(h,uint32_t(u.routeStamp));
    for(const auto& order:u.orders) {
        h=mix(h,uint32_t(order.x.v));h=mix(h,uint32_t(order.z.v));h=mix(h,order.goal);
        h=mix(h,order.controller);h=mix(h,order.hasSegment);
        h=mix(h,order.navigationConsumed);h=mix(h,order.navigationExhausted);
        h=mix(h,uint32_t(order.segmentX.v));h=mix(h,uint32_t(order.segmentZ.v));
    }
    return h;
}
inline void known(World& world) {
    world.updateNavigationExploration();
    auto& cells=world.editNavigationExploration();
    std::fill(cells.begin(),cells.end(),0xffff);
}
template<class W> void profiling(W& world,bool enabled) {
    if constexpr(requires {world.setPathProfiling(enabled);world.resetPathDiagnostics();}) {
        world.setPathProfiling(enabled);world.resetPathDiagnostics();
    } else if(enabled)throw std::runtime_error("path profiling is unavailable in this build");
}
// Build identity carried in the result itself, so a debug or unoptimized
// executable cannot silently stand in for a Release benchmark binary.
inline void printBuild() {
#if defined(__clang__)
    const char* compiler="clang " __clang_version__;
#elif defined(__GNUC__)
    const char* compiler="gcc " __VERSION__;
#elif defined(_MSC_VER)
    const char* compiler="msvc";
#else
    const char* compiler="unknown";
#endif
#if defined(NDEBUG)
    const bool ndebug=true;
#else
    const bool ndebug=false;
#endif
#if defined(__OPTIMIZE__)
    const bool optimized=true;
#else
    const bool optimized=false;
#endif
    std::printf("\"build_compiler\":\"%s\",\"build_ndebug\":%s,\"build_optimized\":%s,",compiler,
        ndebug?"true":"false",optimized?"true":"false");
}
template<class W> void printDiagnostics(const W& world,bool enabled) {
    auto metric=[](const char* name,uint64_t value) {
        std::printf("\"%s\":%llu,",name,(unsigned long long)value);
    };
    if constexpr(requires {world.pathDiagnostics();}) {
        const auto d=world.pathDiagnostics();
        std::printf("\"path_profiling\":%s,\"search_initializations\":%llu,\"search_execution_slices\":%llu,\"search_reset_cells\":%llu,\"search_initialization_ns\":%llu,\"search_preparation_ns\":%llu,\"search_execution_ns\":%llu,\"search_scratch_bytes\":%zu,\"body_snapshot_rebuilds\":%llu,\"body_snapshot_bytes\":%zu,\"grade_plane_bytes\":%zu,",
            enabled?"true":"false",(unsigned long long)d.initializations,(unsigned long long)d.executionSlices,
            (unsigned long long)d.resetCells,(unsigned long long)d.initializationNanoseconds,
            (unsigned long long)d.preparationNanoseconds,(unsigned long long)d.executionNanoseconds,d.scratchBytes,
            (unsigned long long)d.bodySnapshotRebuilds,d.bodySnapshotBytes,d.gradePlaneBytes);
    } else std::printf("\"path_profiling\":false,\"search_scratch_bytes\":null,");
    // Every LegionNavigator::Stats counter, run totals, as legion_<name>
    // (forEachStat's names, so a counter added to Stats is exported here
    // without a second list to keep in step).
    if constexpr(requires {world.legionStats();}) {
        tak::sim::LegionNavigator::forEachStat(world.legionStats(),[&](const char* name,uint64_t value) {
            metric(("legion_"+std::string(name)).c_str(),value);
        });
    }
    // World's convoy lookups (sim/convoy.h; outside Stats, exported beside it):
    // orders joined, index probes plus candidates tested, the most for one
    // order, and orders over 16 (the p99 bound) and over 64 (the max bound).
    if constexpr(requires {world.convoyStats();}) {
        const auto c=world.convoyStats();
        metric("legion_convoy_orders",c.orders);metric("legion_convoy_tests",c.tests);
        metric("legion_convoy_tests_max",c.testsMax);
        metric("legion_convoy_tests_over16",c.over16);metric("legion_convoy_tests_over64",c.over64);
    }
}
// MV-06's route-follower split needs the member's committed detour, read through
// LegionNavigator::routeLength(id) (cells left in its local detour route; read-only,
// unhashed). A build whose navigator lacks the hook prints route_crawl_samples as null.
template<class N> constexpr bool hasRouteHook() {return requires(const N& n) {n.routeLength(0);};}
template<class N> int routeLengthOf(const N* nav,int id) {
    if constexpr(hasRouteHook<N>()) {return nav?nav->routeLength(id):0;}
    else {(void)nav;(void)id;return 0;}
}
inline void barriers(World& world,int width,int height,const std::vector<Rect>& previous,
                     const std::vector<Rect>& next) {
    for(const auto& r:previous)world.blockCells(r.x,r.z,r.w,r.h,false);
    std::vector<uint16_t> cells(size_t(width)*height,0xffff);
    for(const auto& r:next) {
        world.blockCells(r.x,r.z,r.w,r.h,true);
        for(int z=r.z;z<r.z+r.h;++z)for(int x=r.x;x<r.x+r.w;++x)
            cells[size_t(z)*width+x]=0;
    }
    world.setMapPlacementFeatures(cells,{{"benchmark-wall",1,1,true,true,false,0}});
}
// Acceptance scenarios (jagged trapped crowdtrap singleunit groupdetour). The
// geometry depends only on scenario, units and players, never on the mode.
struct Spawn {float x=0,z=0,gx=0,gz=0,radius=32;int foot=2,player=0;bool moving=true;};
struct Layout {
    int width=0,height=0;std::vector<Rect> walls;std::vector<Spawn> spawns;
    Rect gate{0,0,0,0};int closeTick=-1,openTick=-1;
    std::vector<Rect> blocks; // groupdetour obstacle per player (route-side observation)
};
inline bool acceptanceScenario(const std::string& name) {
    return name=="jagged"||name=="trapped"||name=="crowdtrap"||name=="singleunit"||name=="groupdetour";
}
inline Layout acceptanceLayout(const Options& o) {
    Layout l;const int movingPerPlayer=o.units*o.movingPercent/100;
    auto cell=[&](int x,int z) {l.walls.push_back({x,z,1,1});};
    if(o.scenario=="singleunit") {
        // Every unit alone in its own walled tile with a concave cup facing it:
        // even tiles start in front of the cup, odd tiles start inside it.
        //
        // World coordinates are 16.16 Fixed, so every position must stay
        // below 32768 px (2048 cells). The square 56x40 tiling does up to
        // 1296 units (36 per row); beyond that its far tiles overflowed (a
        // spawn there failed "invalid initial footprint", a goal there
        // wrapped). Larger populations use a compact 50x36 tile with the same
        // cup, packed 40 per row: 2000 units fit in 2000x1800 cells.
        constexpr int kMaxCells=2048;const int total=o.units*o.players;
        int TW=56,TH=40,perRow=int(std::ceil(std::sqrt(double(total))));
        if(perRow*TW>kMaxCells) {TW=50;TH=36;perRow=(kMaxCells-1)/TW;}
        const int tileRows=(total+perRow-1)/perRow;
        if(tileRows*TH>=kMaxCells)throw std::runtime_error("singleunit: population does not fit the 2048-cell map limit");
        l.width=round64(perRow*TW);l.height=round64(tileRows*TH);
        for(int i=0;i<total;++i) {
            const int tx=i%perRow*TW,tz=i/perRow*TH,zc=tz+TH/2;
            l.walls.push_back({tx,tz,TW,1});l.walls.push_back({tx,tz+TH-1,TW,1});
            l.walls.push_back({tx,tz,1,TH});l.walls.push_back({tx+TW-1,tz,1,TH});
            l.walls.push_back({tx+32,zc-10,2,22});
            l.walls.push_back({tx+16,zc-10,18,2});l.walls.push_back({tx+16,zc+10,18,2});
            Spawn s;s.player=i/o.units;s.moving=i%o.units<movingPerPlayer;
            s.x=float((tx+(i%2?24:8))*16);s.z=float(zc*16);s.gx=float((tx+TW-8)*16);s.gz=float(zc*16);
            l.spawns.push_back(s);
        }
        return l;
    }
    const bool jagged=o.scenario=="jagged",crowdtrap=o.scenario=="crowdtrap";
    const int stride=jagged?5:3,rows=int(std::ceil(std::sqrt(double(o.units))));
    const int columns=(o.units+rows-1)/rows;
    const int laneHeight=std::max(rows*stride+(crowdtrap?24:16),o.scenario=="groupdetour"?64:48);
    l.width=round64(2*columns*stride+320);l.height=round64(o.players*laneHeight+64);
    const int middle=l.width/2,left=32,right=l.width-32-columns*stride;
    for(int p=0;p<=o.players;++p)l.walls.push_back({0,24+p*laneHeight,l.width,2});
    for(int p=0;p<o.players;++p) {
        const int low=26+p*laneHeight,high=24+(p+1)*laneHeight,span=high-low,zc=(low+high)/2;
        const int baseZ=36+p*laneHeight,divider=baseZ+(rows/2)*stride+1;
        if(jagged) {
            // Funnel of 2-cell stair steps, a corridor whose faces carry a
            // triangle staircase, 1-cell teeth and 1-cell notches, then a
            // staggered field of small plus/L/staircase rocks.
            const int hw=std::max(11,span/4),c0=middle-24,c1=middle+24;
            const int depth=zc-hw-low,funnel=(depth+1)/2;
            auto jag=[](int x) {const int t=x%8<4?x%8:8-x%8;return t+(x%13==0?3:0)-(x%17==5?3:0);};
            for(int x=c0;x<c1;++x) {
                const int top=zc-hw+jag(x),bottom=zc+hw-jag(x+3);
                l.walls.push_back({x,low,1,top-low});l.walls.push_back({x,bottom,1,high-bottom});
            }
            for(int i=0;i<funnel;++i) {
                const int h=std::min(depth,2*(i+1)+(i%2)),g=std::min(depth,2*(funnel-i)+(i%2));
                l.walls.push_back({c0-funnel+i,low,1,h});l.walls.push_back({c0-funnel+i,high-h,1,h});
                l.walls.push_back({c1+i,low,1,g});l.walls.push_back({c1+i,high-g,1,g});
            }
            const int r0=c1+funnel+6;
            for(int gx=0;gx<=30;gx+=10)for(int gz=3+(gx/10%2)*5;gz+3<=span-3;gz+=10) {
                const int x=r0+gx,z=low+gz;
                switch((gx+gz)%3) {
                case 0:cell(x+1,z);cell(x,z+1);cell(x+1,z+1);cell(x+2,z+1);cell(x+1,z+2);break;
                case 1:cell(x,z);cell(x,z+1);cell(x,z+2);cell(x+1,z+2);cell(x+2,z+2);break;
                default:cell(x,z);cell(x+1,z);cell(x+1,z+1);cell(x+2,z+1);cell(x+2,z+2);break;
                }
            }
        }
        if(crowdtrap) {
            // Two sub-lanes, each with a 6-cell door; the lower door is gated.
            l.walls.push_back({0,divider,l.width,2});
            const int upper=(low+divider)/2-3,lower=(divider+2+high)/2-3;
            l.walls.push_back({middle-2,low,4,upper-low});
            l.walls.push_back({middle-2,upper+6,4,divider-upper-6});
            l.walls.push_back({middle-2,divider+2,4,lower-divider-2});
            l.walls.push_back({middle-2,lower+6,4,high-lower-6});
            if(!p) {l.gate={middle-2,lower,4,6};l.closeTick=std::min(30,std::max(1,o.ticks/3));l.openTick=std::max(2,o.ticks/2);}
        }
        if(o.scenario=="groupdetour") {
            const Rect block{middle-24,low+std::max(6,span*30/100),48,0};
            Rect b=block;b.h=high-std::max(6,span*15/100)-b.z;
            l.walls.push_back(b);l.blocks.push_back(b);
        }
        int pockets=0;
        for(int k=0;k<o.units;++k) {
            const int col=k/rows,row=k%rows,slotX=(columns-1-col)*stride;
            const int z=baseZ+row*stride+(crowdtrap&&row>=rows/2?4:0);
            Spawn s;s.player=p;s.moving=k<movingPerPlayer;s.foot=jagged?1+k%4:2;
            s.x=float((left+slotX)*16);s.z=float(z*16);s.gx=float((right+slotX)*16);s.gz=float(z*16);
            if(o.scenario=="trapped"&&k%8==7) {
                // Alternately sealed, or leaking through a gap one cell narrower
                // than the footprint. Pitch 12 leaves >=4-cell lanes between pockets.
                const int j=pockets++,f=2+(j%3),perColumn=std::max(1,(span-4)/12);
                const int px=left+columns*stride+12+(j/perColumn)*12,pz=low+2+(j%perColumn)*12,size=f+4;
                if(px+size>=right-8)throw std::runtime_error("trapped scenario: pockets do not fit");
                l.walls.push_back({px,pz,size,1});l.walls.push_back({px,pz+size-1,size,1});
                l.walls.push_back({px,pz,1,size});
                if(j%2==0)l.walls.push_back({px+size-1,pz,1,size});
                else {
                    const int gap=f-1,gz=pz+1+(f+2-gap)/2;
                    l.walls.push_back({px+size-1,pz,1,gz-pz});
                    l.walls.push_back({px+size-1,gz+gap,1,pz+size-gz-gap});
                }
                s.foot=f;s.x=float((px+1)*16+(f+2)*8);s.z=float((pz+1)*16+(f+2)*8);
            }
            l.spawns.push_back(s);
        }
    }
    return l;
}
inline int run(const Options& o) {
    const auto setupStart=Clock::now();
    const bool mixed=o.scenario=="mixedfootprints",shared=mixed||o.scenario=="sharedgoal";
    // W7 step 0 (PLAN 4 W7): the multi-command opposing scenarios. opposingdoors / opposingbridges are doors /
    // bridges with half of each lane's members sent the other way, and crossingcolumns sends a second block north
    // across the first block's band. In all three the second stream is a separate command, kSecondStream ticks
    // after the first, so the awareness code (which never plans round a member of its own command) sees two
    // groups. opposingcolumns stays one command.
    const bool doorsLike=o.scenario=="doors"||o.scenario=="opposingdoors",bridgesLike=o.scenario=="bridges"||o.scenario=="opposingbridges";
    const bool opposing=o.scenario=="opposingcolumns"||o.scenario=="opposingdoors"||o.scenario=="opposingbridges",exploring=o.scenario=="exploration";
    const bool crossing=o.scenario=="crossingcolumns",multiCommand=crossing||o.scenario=="opposingdoors"||o.scenario=="opposingbridges";
    constexpr int kSecondStream=100;
    if(crossing&&o.players!=1)throw std::runtime_error("crossingcolumns: players must be 1");
    const bool churn=o.scenario=="churn";
    const int stride=mixed?5:3,rows=int(std::ceil(std::sqrt(double(o.units))));
    const int columns=(o.units+rows-1)/rows,laneHeight=rows*stride+16;
    // crossingcolumns: the second block starts south of the first block's band and ends north of it, so the band
    // sits below a pad as deep as the block.
    const int topPad=crossing?columns*stride+16:0;
    const bool accept=acceptanceScenario(o.scenario);const Layout acc=accept?acceptanceLayout(o):Layout{};
    const int width=accept?acc.width:round64(2*columns*stride+320),
        height=accept?acc.height:round64(crossing?topPad+laneHeight+columns*stride+16+64:o.players*laneHeight+64);
    const int middle=width/2,left=32,right=width-32-columns*stride;
    const int movingPerPlayer=o.units*o.movingPercent/100,totalMoving=movingPerPlayer*o.players;
    LatencyObserver latency;
    World world;world.setGameSeed(o.seed);world.setSerialThreads(!o.workers);world.setPathService(true);
    // Numeric identities (Retail 0, Legion 4) so the frozen baseline, which
    // predates the Legion enumerator, builds this harness unchanged.
    world.setPathfindingMode(PathfindingMode(o.mode=="legion"?4:0));
    // churn's structures belong to one extra allied player, so removing that
    // player's units removes exactly the standing structure.
    const int structurePlayer=o.players;
    world.setPlayerCount(o.players+(churn?1:0));for(int p=0;p<o.players+(churn?1:0);++p)world.setTeam(p,0);
    world.setTerrain(std::vector<uint8_t>(size_t(width)*height,100),width,height,64);
    std::vector<Rect> walls;if(accept)walls=acc.walls;
    const bool lanes=!accept&&(doorsLike||bridgesLike||o.scenario=="maze"||exploring);
    if(lanes)for(int p=0;p<=o.players;++p)walls.push_back({0,24+p*laneHeight,width,2});
    if(!accept)for(int p=0;p<o.players;++p) {
        const int low=26+p*laneHeight,high=24+(p+1)*laneHeight,center=(low+high)/2;
        if(doorsLike||bridgesLike) {
            const int thickness=doorsLike?4:64;
            walls.push_back({middle-thickness/2,low,thickness,center-3-low});
            walls.push_back({middle-thickness/2,center+3,thickness,high-center-3});
        }
        if(o.scenario=="maze"||exploring)for(int b=0;b<4;++b) {
            const int x=left+columns*stride+24+b*32;
            walls.push_back({x,low+(b%2?12:0),4,high-low-12});
        }
    }
    if(o.scenario=="unreachable"||o.scenario=="recovery"||o.scenario=="recovery-passive")walls.push_back({middle-2,0,4,height});
    barriers(world,width,height,{},walls);
    std::array<UnitType,3> types{mover(2,exploring,o.turnRate),mover(3,exploring,o.turnRate),mover(4,exploring,o.turnRate)};
    std::vector<Member> members;members.reserve(size_t(o.units)*o.players);
    std::array<UnitType,4> footTypes{mover(1,false,o.turnRate),mover(2,false,o.turnRate),mover(3,false,o.turnRate),mover(4,false,o.turnRate)};
    if(accept)for(const auto& s:acc.spawns) {
        Member member;member.player=s.player;member.moving=s.moving;member.gx=s.gx;member.gz=s.gz;
        member.alternateX=s.x;member.alternateZ=s.z;member.radius=s.radius;
        member.id=world.spawn(&footTypes[size_t(s.foot-1)],s.x,s.z,std::nullopt,s.player);
        if(member.id<=0)throw std::runtime_error("failed to spawn requested population");
        const auto& u=*world.unit(member.id);member.initialX=member.previousX=u.x.v;
        member.initialZ=member.previousZ=u.z.v;members.push_back(member);
    }
    if(!accept)for(int p=0;p<o.players;++p)for(int k=0;k<o.units;++k) {
        const int slot=opposing?k/2:k;
        const int col=slot/rows,row=slot%rows,baseZ=36+p*laneHeight+topPad;
        // Movers occupy the front columns; inactive bodies do not form an
        // artificial wall between the commanded crowd and its destination.
        const int slotX=(columns-1-col)*stride,slotZ=row*stride;
        Member member;member.player=p;member.moving=k<movingPerPlayer;
        member.reverse=opposing&&(k%2==1);
        int x=(member.reverse?right:left)+slotX,z=baseZ+slotZ;
        member.gx=float(((member.reverse?left:right)+slotX)*16);member.gz=float(z*16);
        member.alternateX=float((left+slotX)*16);member.alternateZ=float(z*16);
        if(crossing&&k%2==1) {
            // The second block: `rows` bodies across, centred on the band's middle; its front column
            // (col 0) starts nearest the band and walks north to the same offset above it.
            member.vertical=true;
            x=middle-rows*stride/2+row*stride;
            z=baseZ+rows*stride+16+col*stride;
            member.gx=float(x*16);member.gz=float((baseZ-16-(columns-col)*stride)*16);
            member.alternateX=float(x*16);member.alternateZ=float(z*16);
            member.crossLine=float(baseZ*16-32);
        }
        if(shared) {
            member.gx=float((right+columns*stride/2)*16);
            member.gz=float((baseZ+rows*stride/2)*16);
            // Fixed destination geometry, authored before running any mode.
            // Each settled footprint gets its area plus one cell of clearance.
            const int body=mixed?64:32;
            member.radius=float(std::ceil(std::sqrt(double(movingPerPlayer)*body*body/3.141592653589793))+2*stride*16);
        }
        const UnitType& type=types[size_t(mixed?k%3:0)];
        member.id=world.spawn(&type,float(x*16),float(z*16),std::nullopt,p);
        if(member.id<=0)throw std::runtime_error("failed to spawn requested population");
        const auto& u=*world.unit(member.id);member.initialX=member.previousX=u.x.v;
        member.initialZ=member.previousZ=u.z.v;members.push_back(member);
    }
    // Build spatial indices once before commands; that setup tick is untimed.
    world.tick(1.f/30);if(!exploring)known(world);
    for(const auto& m:members) {
        const auto& u=*world.unit(m.id);
        if(!world.mobilePlacement(u,footprintOrigin(u.x,u.type->footX),footprintOrigin(u.z,u.type->footZ),false))
            throw std::runtime_error("invalid initial footprint for unit "+std::to_string(m.id));
    }
    auto issue=[&](Member& m,int tick) {
        world.order(m.id,m.gx,m.gz,false);const auto& u=*world.unit(m.id);
        m.commandTick=tick;++m.epoch;m.firstMove=m.firstAdmission=m.firstRoute=-1;
        m.firstPendingClear=-1;m.pending=RetailReplayProbe::pending(world,m.id);
        m.rest=0;m.arrivedTick=m.retiredTick=m.approachTick=-1;m.stamp=u.routeStamp;
        m.initialX=u.x.v;m.initialZ=u.z.v;
        if(m.epoch==1)m.straight=std::hypot(double(u.x.toFloat()-m.gx),double(u.z.toFloat()-m.gz));
    };
    auto command=[&](int tick,bool alternate) {
        for(auto& m:members)if(m.moving) {
            if(alternate) {std::swap(m.gx,m.alternateX);std::swap(m.gz,m.alternateZ);}
            issue(m,tick);
        }
    };
    // The streams of a multi-command scenario: the first (even members) now, the second (odd) at kSecondStream.
    auto commandStream=[&](int tick,bool second) {
        for(size_t i=0;i<members.size();++i)if(members[i].moving&&((i%size_t(o.units))%2==1)==second)issue(members[i],tick);
    };
    if(o.latency)RetailReplayProbe::telemetry(world,&latency);
    // The observer's own map/vector growth is harness work, not simulation
    // allocation. Pausing the global switch is only exact without workers.
    if(o.allocations&&!o.workers)latency.quiet=&crowdbench_allocation::enabled;
    if(multiCommand)commandStream(0,false);else command(0,false);
    // Acceptance observation (all modes, all scenarios): see crowdbench_acceptance.h.
    namespace ca=crowdbench_acceptance;
    ca::Observer observer;ca::Totals totals;std::vector<ca::Track> tracks(members.size());
    for(auto& t:tracks)t.init();
    std::vector<int> foots;
    for(const auto& m:members) {const int f=world.unit(m.id)->type->footX;
        if(std::find(foots.begin(),foots.end(),f)==foots.end())foots.push_back(f);}
    std::sort(foots.begin(),foots.end());
    std::vector<Rect> observedWalls=walls;
    std::map<std::tuple<int,int,int,int>,std::vector<int32_t>> goalLabels;
    auto rebuildObserver=[&] {
        std::vector<ca::Wall> w;for(const auto& r:walls)w.push_back({r.x,r.z,r.w,r.h});
        observer.reset(width,height,w,foots);goalLabels.clear();observedWalls=walls;
    };
    rebuildObserver();
    auto originOf=[](const Unit& u) {
        return std::pair{footprintOrigin(u.x,u.type->footX),footprintOrigin(u.z,u.type->footZ)};
    };
    // Static path to the goal disc for this footprint (component membership).
    auto reachable=[&](const Member& m,const Unit& u) {
        const int f=u.type->footX;const auto [ox,oz]=originOf(u);
        const auto& p=observer.prints.at(f);
        if(!observer.legalAt(p,ox,oz))return true; // not judged: placement keeps origins legal
        const auto key=std::make_tuple(f,int(m.gx),int(m.gz),int(m.radius));
        auto it=goalLabels.find(key);
        if(it==goalLabels.end()) {
            std::vector<int32_t> labels;
            observer.goalOrigins(p,int(m.gx),int(m.gz),int(m.radius),[&](int c){labels.push_back(p.label[c]);});
            std::sort(labels.begin(),labels.end());labels.erase(std::unique(labels.begin(),labels.end()),labels.end());
            it=goalLabels.emplace(key,std::move(labels)).first;
        }
        return std::binary_search(it->second.begin(),it->second.end(),p.label[size_t(oz)*width+ox]);
    };
    const bool computeOptimal=o.scenario=="singleunit"||totalMoving<=256;
    if(computeOptimal)for(size_t i=0;i<members.size();++i)if(members[i].moving) {
        const auto& u=*world.unit(members[i].id);const auto [ox,oz]=originOf(u);
        tracks[i].optimal=observer.optimalPx(u.type->footX,ox,oz,int(members[i].gx),int(members[i].gz),int(members[i].radius));
    }
    std::vector<size_t> stamped;
    std::vector<uint32_t> occMember(size_t(width)*height,0); // cell -> member index + 1 (0 = none)
    std::vector<ca::Step> steps(members.size());
    auto observeAcceptance=[&](int tick) {
        for(size_t c:stamped)observer.occupancy[c]=0,occMember[c]=0;
        stamped.clear();
        for(size_t i=0;i<members.size();++i) {
            const auto& m=members[i];
            const auto& u=*world.unit(m.id);if(!u.alive())continue;
            const auto [ox,oz]=originOf(u);
            for(int z=oz;z<oz+u.type->footZ;++z)for(int x=ox;x<ox+u.type->footX;++x)
                if(x>=0&&z>=0&&x<width&&z<height) {
                    const size_t c=size_t(z)*width+x;observer.occupancy[c]=m.id;occMember[c]=uint32_t(i+1);stamped.push_back(c);
                }
        }
        // Advance every window first, so a leader's progress over this same
        // window is known when a follower behind it is classified.
        for(size_t i=0;i<members.size();++i) {
            const auto& u=*world.unit(members[i].id);
            steps[i]=ca::advance(tracks[i],u.x.v,u.z.v,u.heading.v);
        }
        // Queued behind a moving leader (user decision, 2026-10-06; see
        // crowdbench_acceptance.h): a same-player mobile member whose centre
        // is within two body widths and within 60 degrees of this body's goal
        // or static travel direction, and which itself made progress over the
        // same window.
        auto queuedBehindMover=[&](size_t i,const Unit& u,int ox,int oz,int tx,int tz) {
            const auto& m=members[i];const int f=u.type->footX;
            const double ux=u.x.v/65536.0,uz=u.z.v/65536.0;
            const double gx=m.gx-ux,gz=m.gz-uz,gl=std::sqrt(gx*gx+gz*gz),tl=std::sqrt(double(tx*tx+tz*tz));
            for(int z=oz-2*f;z<oz+3*f;++z)for(int x=ox-2*f;x<ox+3*f;++x) {
                if(x<0||z<0||x>=width||z>=height)continue;
                const uint32_t k=occMember[size_t(z)*width+x];
                if(!k||k-1==i)continue;
                const size_t j=k-1;const auto& lm=members[j];const auto& l=*world.unit(lm.id);
                if(lm.player!=m.player||l.type->maxVel<=Fixed()||tracks[j].filled<=ca::window||steps[j].stationary)continue;
                const double vx=l.x.v/65536.0-ux,vz=l.z.v/65536.0-uz,vl=std::sqrt(vx*vx+vz*vz);
                if(vl<=0||vl>(f+l.type->footX)*16.0)continue;
                const bool goalAhead=gl>0&&vx*gx+vz*gz>=0.5*vl*gl;
                const bool travelAhead=tl>0&&vx*tx+vz*tz>=0.5*vl*tl;
                if(goalAhead||travelAhead)return true;
            }
            return false;
        };
        for(size_t i=0;i<members.size();++i) {
            const auto& m=members[i];auto& t=tracks[i];const auto& u=*world.unit(m.id);
            const auto& step=steps[i];
            if(step.stationary) {totals.headingStationaryBam+=step.turnAbs;totals.travelReversals+=step.reversed;}
            if(step.spinning) {++t.spinTicks;++totals.spinTicks;}
            if(!m.moving)continue;
            const auto [ox,oz]=originOf(u);const int f=u.type->footX;
            int cls;
            if(m.near)cls=ca::AtGoal;
            else if(!reachable(m,u))cls=ca::Trapped;
            else if(tick-m.commandTick<ca::window)cls=ca::Warmup;
            // The no-progress test looks back one window. Right after a unit
            // stops being trapped (a gate opens, a wall is removed) that window
            // still spans the trapped period, so a unit already moving at full
            // speed would read as stationary. Judge it only after one full
            // window of being reachable, as after a new command. (User
            // decision, 2026-10-06; applies identically to every mode.)
            else if(t.lastTrapped>=0&&tick-t.lastTrapped<ca::window)cls=ca::Warmup;
            else if(!step.stationary)cls=ca::Progressing;
            else if(const auto* field=observer.direction(f,int(m.gx),int(m.gz),int(m.radius));!field||field->at(ox,oz)==ca::far)cls=ca::Unclassified;
            else {
                // Follow the static distance field for foot+2 steps; any other
                // body on those footprint cells means the crowd holds the unit.
                const auto& p=observer.prints.at(f);
                int cx=ox,cz=oz;bool crowd=false;int tx=0,tz=0;
                for(int s=0;s<f+2&&!crowd;++s) {
                    int best=-1;uint16_t bestD=field->at(cx,cz);
                    observer.forNeighbours(p,cx,cz,[&](int n,int){const uint16_t d=field->at(n%width,n/width);if(d<bestD){bestD=d;best=n;}});
                    if(best<0)break;
                    cx=best%width;cz=best/width;
                    if(s==0)tx=cx-ox,tz=cz-oz;
                    for(int z=cz;z<cz+f;++z)for(int x=cx;x<cx+f;++x) {
                        const int32_t id=observer.occupancy[size_t(z)*width+x];crowd|=id&&id!=m.id;
                    }
                }
                bool terrain=false;
                for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx)terrain|=!observer.legalAt(p,ox+dx,oz+dz);
                // IN-12: the raw terrain-contact signal before the narrowing
                // below, kept beside class_terrain_stuck (observation only).
                if(terrain) {++totals.rawTerrainTicks;t.everRawTerrain=true;}
                if(terrain&&crowd)++totals.crowdAndTerrainTicks;
                if(!crowd&&terrain&&queuedBehindMover(i,u,ox,oz,tx,tz)) {crowd=true;++totals.narrowedTicks;t.everNarrowed=true;}
                cls=crowd?ca::CrowdHeld:terrain?ca::TerrainStuck:ca::OpenIdle;
            }
            ++totals.classTicks[cls];t.finalClass=cls;
            t.everTerrainStuck|=cls==ca::TerrainStuck;t.everCrowdHeld|=cls==ca::CrowdHeld;
            if(cls==ca::CrowdHeld&&step.spinning)++totals.spinCrowdHeld;
            if(cls==ca::Trapped) {
                t.lastTrapped=tick;
                if(t.trappedSince<0)t.trappedSince=tick;
                t.everTrapped=true;
                if(step.spinning)++totals.spinTrapped;
                if(step.moved||step.turned) {
                    t.settleMax=std::max(t.settleMax,tick-t.trappedSince);
                    if(tick-t.trappedSince>=ca::trappedGrace) {++totals.trappedMoving;t.movingAfterGrace=true;}
                }
            } else t.trappedSince=-1;
            if(m.near&&t.pathAtGoal<0)t.pathAtGoal=m.path;
            if(t.side<0&&size_t(m.player)<acc.blocks.size()) {
                const auto& b=acc.blocks[size_t(m.player)];
                if(u.x.v>=int32_t(b.x*16)<<16&&u.x.v<int32_t((b.x+b.w)*16)<<16)
                    t.side=int64_t(u.z.v)*2<(int64_t(b.z*2+b.h)*16<<16)?0:1;
            }
        }
        if(tick%30==0)for(int p=0;p<o.players;++p) {
            std::vector<int32_t> xs;
            for(const auto& m:members)if(m.moving&&m.player==p&&!m.near)xs.push_back(world.unit(m.id)->x.v);
            if(xs.size()<2)continue;
            std::sort(xs.begin(),xs.end());
            const double spread=double(xs[(xs.size()-1)*9/10]-xs[(xs.size()-1)/10])/65536;
            totals.spreadSum+=spread;totals.spreadMax=std::max(totals.spreadMax,spread);++totals.spreadSamples;
        }
    };
    profiling(world,o.profile);
    std::ofstream trace,unitTrace,wallTrace;
    // Plotting sidecar: map extent, then every published barrier set in cells.
    auto traceWalls=[&](int tick) {
        if(!wallTrace.is_open())return;
        wallTrace<<tick<<",set,0,0,0,0\n";
        for(const auto& r:walls)wallTrace<<tick<<",wall,"<<r.x<<','<<r.z<<','<<r.w<<','<<r.h<<'\n';
    };
    if(!o.trace.empty()) {
        trace.open(o.trace);unitTrace.open(o.trace+".units.csv");wallTrace.open(o.trace+".walls.csv");
        if(!trace||!unitTrace||!wallTrace)throw std::runtime_error("cannot create trace output");
        wallTrace<<"tick,kind,x,z,w,h\n0,map,0,0,"<<width<<','<<height<<'\n';
        traceWalls(0);
        trace<<"tick,world_tick,hash,retail_requests,retail_completions,retail_failures,retail_work,retail_pending\n";
        unitTrace<<"tick,id,epoch,x_raw,z_raw,speed_raw,orders,route_stamp,control_hash,front_x_raw,front_z_raw,controller,segment,exhausted,consumed,goal_x,goal_z,goal_radius,foot_x,foot_z,pending,heading_bam,turn_request_bam,blocked,route_failed,mission_events,leg_controller,leg_x_raw,leg_z_raw,leg_exhausted,leg_consumed,mission_stage,mission_pending,mission_wait_mask,mission_radius,mission_target_x_raw,mission_target_z_raw,next_x_raw,next_z_raw\n";
    }
    const double setupMs=millis(setupStart,Clock::now());
    std::vector<double> times;times.reserve(size_t(o.ticks));
    uint64_t illegalSamples=0,peakPending=0;
    double eventMs=0;int allAt=-1,firstCross=-1,lastCross=-1;
    auto observeTrace=[&](int tick) {
        if(!trace.is_open())return;
        const auto& retail=world.pathStats();
        trace<<tick<<','<<world.tickCount()<<','<<std::hex<<world.stateHash()<<std::dec<<','
            <<retail.requests()<<','<<retail.completions()<<','<<retail.failures()<<','<<retail.workSpent()<<','
            <<retail.pendingCount()<<'\n';
        for(auto& m:members) {
            const auto& u=*world.unit(m.id);const uint64_t hash=mix(control(u),m.pending);
            if(hash==m.traceControl&&tick%30!=0)continue;
            m.traceControl=hash;unitTrace<<tick<<','<<m.id<<','<<m.epoch<<','<<u.x.v<<','<<u.z.v<<','<<u.speed.v
                <<','<<u.orders.size()<<','<<u.routeStamp<<','<<std::hex<<hash<<std::dec;
            if(u.orders.empty())unitTrace<<",0,0,0,0,0,0";
            else {const auto& q=u.orders.front();unitTrace<<','<<q.x.v<<','<<q.z.v<<','<<q.controller<<','
                <<q.hasSegment<<','<<q.navigationExhausted<<','<<q.navigationConsumed;}
            unitTrace<<','<<m.gx<<','<<m.gz<<','<<m.radius<<','<<u.type->footX<<','<<u.type->footZ<<','<<m.pending
                <<','<<u.heading.v<<','<<u.turnReqBam<<','<<u.bodyBlockStreak<<','<<u.routeFailed<<','<<u.missionEvents;
            if(u.orders.empty())unitTrace<<",0,0,0,0,0,0,0,0,0,0,0";
            else {const auto& q=u.orders[World::currentLeg(u.orders)];
                unitTrace<<','<<q.controller<<','<<q.x.v<<','<<q.z.v<<','<<q.navigationExhausted<<','<<q.navigationConsumed
                    <<','<<int(q.mission.stage)<<','<<q.mission.pending<<','<<q.mission.waitMask<<','<<q.missionRadius
                    <<','<<(q.missionTarget?q.missionTarget->first.v:0)<<','<<(q.missionTarget?q.missionTarget->second.v:0);}
            if(u.orders.size()>1)unitTrace<<','<<u.orders[1].x.v<<','<<u.orders[1].z.v;
            else unitTrace<<",0,0";
            unitTrace<<'\n';
        }
    };
    observeTrace(0);
    // churn (T7 I3): staggered re-orders keep hundreds of Legion groups live,
    // and a structure is placed or removed every kChurnStructureEvery ticks.
    // Every choice is a pure function of the seed, the member index, its order
    // count and the deterministic unit positions, so the run is reproducible.
    constexpr int kChurnEvery=4,kChurnSlices=30,kChurnStructureEvery=20;
    const UnitType structureType=churnStructure();
    int structureId=0;uint64_t churnReorders=0,churnPlaced=0,churnRemoved=0,churnSkipped=0;
    auto churnGoal=[&](size_t index,Member& m) {
        uint64_t h=mix(mix(mix(1469598103934665603ull,o.seed),index),uint64_t(m.epoch));
        h^=h>>29;h*=0xbf58476d1ce4e5b9ull;h^=h>>32;
        // Goals sit within 3 cells of anchors 32 cells apart: Legion groups
        // one tick's orders whose goals lie within 16 cells of each other,
        // so every anchor picked in a burst is a group of its own.
        const int spanX=right+columns*stride-left,spanZ=rows*stride,baseZ=36+m.player*laneHeight;
        const int nx=std::max(1,spanX/32),nz=std::max(1,spanZ/32),a=int(h%uint64_t(nx*nz));
        const int ax=left+(a%nx)*32+std::min(16,spanX/2),az=baseZ+(a/nx)*32+std::min(16,spanZ/2);
        const int x=std::clamp(ax+int((h>>32)%7)-3,left,left+spanX-1),z=std::clamp(az+int((h>>40)%7)-3,baseZ,baseZ+spanZ-1);
        m.gx=float(x*16);m.gz=float(z*16);
    };
    auto churnStructure=[&](int tick) {
        if(structureId) {RetailReplayProbe::removeUnits(world,structurePlayer);structureId=0;++churnRemoved;return;}
        // The first candidate site whose 4x4 footprint plus a one-cell margin
        // touches no body; candidates rotate through every player's lane.
        const int f=structureType.footX;
        for(int attempt=0;attempt<25;++attempt) {
            const int k=int(churnPlaced+churnSkipped)+attempt,p=k%o.players;
            const int cx=middle-f/2+((k*7)%5-2)*8,cz=36+p*laneHeight+rows*stride/2-f/2+((k*3)%5-2)*std::max(1,rows*stride/6);
            bool clear=cx>0&&cz>26+p*laneHeight&&cx+f<width&&cz+f<24+(p+1)*laneHeight;
            for(size_t i=0;clear&&i<members.size();++i) {
                const auto& u=*world.unit(members[i].id);if(!u.alive())continue;
                const int ox=footprintOrigin(u.x,u.type->footX),oz=footprintOrigin(u.z,u.type->footZ);
                clear=ox+u.type->footX<cx-1||ox>cx+f||oz+u.type->footZ<cz-1||oz>cz+f;
            }
            if(!clear)continue;
            structureId=world.spawn(&structureType,float(cx*16+f*8),float(cz*16+f*8),std::nullopt,structurePlayer);
            world.blockFoot(structureType,float(cx*16+f*8),float(cz*16+f*8),true);
            ++churnPlaced;return;
        }
        ++churnSkipped;(void)tick;
    };
    // Observation-only per-run counters for the keys added 2026-10-08
    // (Retail age classes, MV-06 crawl samples, live Legion groups).
    // Fetched after every tick: the World creates its navigator lazily.
    const bool legionMode=o.mode=="legion";LegionNavigator* legionNav=nullptr;
    auto legionState=[&](int id) {return legionNav?legionNav->unitState(id):0;};
    uint64_t ageTicks[4]{};   // waiting held/no-progress, parked held/no-progress
    uint64_t movingSamples=0,cap8Samples=0,routeCrawlSamples=0;
    constexpr bool routeHook=hasRouteHook<LegionNavigator>();
    uint64_t liveGroupSamples=0,liveGroupSum=0,liveGroupPeak=0,liveGroupMin=~0ull;
    constexpr int kLiveWarmup=300;
    for(int tick=1;tick<=o.ticks;++tick) {
        latency.tick=uint64_t(tick);
        const auto eventStart=Clock::now();
        if(o.scenario=="rapidreplacement"&&tick%120==0&&tick<=o.ticks/2)command(tick,true);
        if(multiCommand&&tick==kSecondStream)commandStream(tick,true);
        if(churn&&tick%kChurnEvery==0) {
            const size_t slice=size_t(tick/kChurnEvery%kChurnSlices);
            for(size_t i=slice;i<members.size();i+=kChurnSlices)if(members[i].moving) {
                churnGoal(i,members[i]);issue(members[i],tick);++churnReorders;
            }
        }
        if(churn&&tick%kChurnStructureEvery==0)churnStructure(tick);
        if((o.scenario=="recovery"||o.scenario=="recovery-passive")&&tick==std::max(1,o.ticks/3)) {
            barriers(world,width,height,walls,{});walls.clear();traceWalls(tick);
            if(o.scenario=="recovery")command(tick,false);
        }
        if(o.scenario=="dynamicobstacle"&&(tick==std::min(30,std::max(1,o.ticks/3))||tick==std::max(2,o.ticks*2/3))) {
            std::vector<Rect> next;
            if(tick<o.ticks*2/3)for(int p=0;p<o.players;++p) {
                // Publish within 30 ticks, before even the fastest mover can
                // reach the wall. The geometry and event times are independent
                // of the selected navigator or its intermediate positions.
                const int z=36+p*laneHeight+rows*stride/3;
                next.push_back({middle-2,z,4,std::max(1,rows*stride/3)});
            }
            barriers(world,width,height,walls,next);walls=std::move(next);traceWalls(tick);
        }
        if(accept&&acc.closeTick>0&&(tick==acc.closeTick||tick==acc.openTick)) {
            std::vector<Rect> next=acc.walls;if(tick==acc.closeTick)next.push_back(acc.gate);
            barriers(world,width,height,walls,next);walls=std::move(next);traceWalls(tick);
        }
        eventMs+=millis(eventStart,Clock::now());
        if(walls.size()!=observedWalls.size()||!std::equal(walls.begin(),walls.end(),observedWalls.begin(),
            [](const Rect& a,const Rect& b){return a.x==b.x&&a.z==b.z&&a.w==b.w&&a.h==b.h;}))rebuildObserver();
        crowdbench_allocation::enabled.store(o.allocations,std::memory_order_relaxed);
        const auto begin=Clock::now();world.tick(1.f/30);const auto end=Clock::now();
        legionNav=legionMode?world.legionNavigator():nullptr;
        crowdbench_allocation::enabled.store(false,std::memory_order_relaxed);
        times.push_back(millis(begin,end));
        peakPending=std::max(peakPending,uint64_t(world.pathStats().pendingCount()));
        int arrived=0;
        for(auto& m:members) {
            const auto& u=*world.unit(m.id);
            bool checkedPlacement=false;
            const bool moved=u.x.v!=m.previousX||u.z.v!=m.previousZ;
            const double dx=double(int64_t(u.x.v)-m.previousX)/65536,dz=double(int64_t(u.z.v)-m.previousZ)/65536;
            if(m.moving) {
                m.path+=std::hypot(dx,dz);
                if(moved&&m.firstMove<0)m.firstMove=tick-m.commandTick;
                if(u.routeStamp!=m.stamp&&m.firstAdmission<0)m.firstAdmission=tick-m.commandTick;
                if(m.firstRoute<0&&std::any_of(u.orders.begin(),u.orders.end(),[](const Order& q){return q.hasSegment;}))
                    m.firstRoute=tick-m.commandTick;
                const bool pending=RetailReplayProbe::pending(world,m.id);
                if(m.pending&&!pending&&m.firstPendingClear<0)m.firstPendingClear=tick-m.commandTick;
                m.pending=pending;
                if(!moved&&!u.orders.empty())++m.stalled;
                m.near=std::hypot(double(u.x.toFloat()-m.gx),double(u.z.toFloat()-m.gz))<=m.radius;
                if(legionNav&&m.approachTick<0&&legionState(m.id)==5)m.approachTick=tick;
                if(u.orders.empty()&&m.retiredTick<0)m.retiredTick=tick;
                if(!u.orders.empty())m.retiredTick=-1;
                if(u.alive()&&u.orders.empty()&&u.speed.v==0&&m.near&&!moved) {
                    m.legal=world.mobilePlacement(u,footprintOrigin(u.x,u.type->footX),footprintOrigin(u.z,u.type->footZ),false);
                    checkedPlacement=true;illegalSamples+=!m.legal;
                    if(m.legal)++m.rest;else m.rest=0;
                } else m.rest=0;
                if(m.rest>=restTicks) {if(m.arrivedTick<0)m.arrivedTick=tick; ++arrived;}else m.arrivedTick=-1;
                if(m.crossedTick<0&&(m.vertical?u.z.toFloat()<m.crossLine:m.reverse?u.x.toFloat()<middle*16-32:u.x.toFloat()>middle*16+32)) {
                    m.crossedTick=tick;if(firstCross<0)firstCross=tick;lastCross=tick;
                }
                // Retail age classes (retailgrade.h: a body unmoved for 10
                // ticks is "recent", for 150 "stale"): still with orders for
                // 10..150 ticks = waiting, longer = parked. Split by whether
                // Legion holds the member on purpose (Holding, Waiting for its
                // field, Arrived, Trapped) or it simply makes no progress.
                // Retail has no such state: all of it is no_progress.
                if(moved||u.orders.empty())m.stillSince=tick;
                else if(const int still=tick-m.stillSince;still>=10) {
                    const int state=legionState(m.id);
                    ageTicks[(still>150?2:0)+(state>=2&&state<=5?0:1)]++;
                }
                // MV-06, sampled every 10 ticks like the audit's dump: a
                // moving member (Legion state Moving; Retail: orders and
                // speed) crawling at no more than cap/8 (+1/64 px).
                if(tick%10==0&&!u.orders.empty()) {
                    const bool movingState=legionNav?legionState(m.id)==1:u.speed.v>0;
                    if(movingState) {
                        ++movingSamples;
                        if(u.speed.v>0&&u.speed.v<=u.type->maxVel.v/8+1024) {
                            ++cap8Samples;
                            routeCrawlSamples+=routeLengthOf(legionNav,m.id)>0;
                        }
                    }
                }
            }
            if(!checkedPlacement&&(tick%60==0||tick==o.ticks)) {
                m.legal=u.alive()&&world.mobilePlacement(u,footprintOrigin(u.x,u.type->footX),footprintOrigin(u.z,u.type->footZ),false);
                illegalSamples+=!m.legal;
            }
            m.previousX=u.x.v;m.previousZ=u.z.v;
        }
        if(legionNav&&tick%10==0) {
            const uint64_t live=world.legionStats().liveGroups;
            liveGroupPeak=std::max(liveGroupPeak,live);
            if(tick>kLiveWarmup) {++liveGroupSamples;liveGroupSum+=live;liveGroupMin=std::min(liveGroupMin,live);}
        }
        if(arrived==totalMoving&&allAt<0)allAt=tick;
        if(arrived!=totalMoving)allAt=-1;
        observeAcceptance(tick);
        observeTrace(tick);
    }
    int arrived=0,physical=0,retired=0,crossed=0,illegalFinal=0,alive=0,finalCommand=0;
    uint64_t stalled=0;double totalPath=0,totalStraight=0,maxDistance=0;
    std::vector<int> arrivalTimes,firstMoves,firstAdmissions,firstRoutes,firstPendingClears;
    std::vector<int> approachTimes,retiredTimes,approachWaits;   // unreachable: AR-11 per-unit instrument
    for(const auto& m:members)if(m.moving) {
        if(m.approachTick>=0)approachTimes.push_back(m.approachTick);
        if(m.retiredTick>=0)retiredTimes.push_back(m.retiredTick);
        if(m.approachTick>=0&&m.retiredTick>=0)approachWaits.push_back(std::max(0,m.retiredTick-m.approachTick));
        const auto& u=*world.unit(m.id);alive+=u.alive();retired+=u.orders.empty();
        finalCommand=std::max(finalCommand,m.commandTick);
        physical+=m.near&&m.legal;illegalFinal+=!m.legal;crossed+=m.crossedTick>=0;
        const bool completed=m.rest>=restTicks&&m.legal;arrived+=completed;
        if(completed)arrivalTimes.push_back(m.arrivedTick);
        if(m.firstMove>=0)firstMoves.push_back(m.firstMove);
        if(m.firstAdmission>=0)firstAdmissions.push_back(m.firstAdmission);
        if(m.firstRoute>=0)firstRoutes.push_back(m.firstRoute);
        if(m.firstPendingClear>=0)firstPendingClears.push_back(m.firstPendingClear);
        totalPath+=m.path;totalStraight+=m.straight;stalled+=m.stalled;
        maxDistance=std::max(maxDistance,std::hypot(double(u.x.toFloat()-m.gx),double(u.z.toFloat()-m.gz)));
    }
    if(arrived!=totalMoving)allAt=-1;
    long peakRss=0;
#if defined(__unix__)
    rusage usage{};if(!getrusage(RUSAGE_SELF,&usage))peakRss=usage.ru_maxrss;
#if defined(__APPLE__)
    peakRss/=1024; // Darwin reports bytes; Linux reports KiB.
#endif
#endif
    const auto& retail=world.pathStats();double sum=0;for(double t:times)sum+=t;
    std::printf("{\"schema\":2,\"mode\":\"%s\",\"scenario\":\"%s\",\"units_per_player\":%d,\"players\":%d,\"total_units\":%zu,\"moving_percent\":%d,\"moving_units\":%d,\"ticks\":%d,\"workers\":%s,\"map_cells\":[%d,%d],",
        o.mode.c_str(),o.scenario.c_str(),o.units,o.players,members.size(),o.movingPercent,totalMoving,o.ticks,o.workers?"true":"false",width,height);
    std::printf("\"seed\":%u,\"latency_observation\":%s,",o.seed,o.latency?"true":"false");
    if(o.latency) {
        const auto ages=latency.pendingAges();
        std::printf("\"route_requests_observed\":%llu,\"route_deliveries_observed\":%zu,\"route_delivery_successes\":%zu,\"route_delivery_failures\":%zu,\"route_delivery_empty\":%llu,\"route_requests_still_pending\":%zu,\"route_request_cancelled\":%llu,\"route_request_replaced\":%llu,\"route_request_stale\":%llu,\"route_request_cleared\":%llu,\"route_telemetry_unmatched\":%llu,",
            (unsigned long long)latency.sequence,latency.deliveries.size(),latency.successful.size(),latency.failed.size(),
            (unsigned long long)latency.empty,latency.pending.size(),(unsigned long long)latency.cancellations[0],
            (unsigned long long)latency.cancellations[1],(unsigned long long)latency.cancellations[2],
            (unsigned long long)latency.cancellations[3],(unsigned long long)latency.unknown);
        std::printf("\"route_request_to_delivery_ticks_received_only_p50\":%.0f,\"route_request_to_delivery_ticks_received_only_p95\":%.0f,\"route_request_to_delivery_ticks_received_only_p99\":%.0f,\"route_request_to_success_ticks_received_only_p95\":%.0f,\"route_pending_age_ticks_p95\":%.0f,\"route_pending_age_ticks_max\":%.0f,",
            percentile(latency.deliveries,.5),percentile(latency.deliveries,.95),percentile(latency.deliveries,.99),
            percentile(latency.successful,.95),percentile(ages,.95),percentile(ages,1));
        uint64_t retired=latency.deliveries.size()+latency.pending.size();
        for(uint64_t count:latency.cancellations)retired+=count;
        std::printf("\"route_request_to_success_ticks_received_only_p50\":%.0f,\"route_request_to_success_ticks_received_only_p99\":%.0f,\"route_request_to_delivery_ticks_received_only_max\":%.0f,\"route_requests_censored\":%zu,\"route_lifecycle_balanced\":%s,",
            percentile(latency.successful,.5),percentile(latency.successful,.99),percentile(latency.deliveries,1),
            latency.pending.size(),retired==latency.sequence&&!latency.unknown?"true":"false");
        std::printf("\"route_request_to_delivery_wall_ms_received_only_p50\":%.6f,\"route_request_to_delivery_wall_ms_received_only_p95\":%.6f,\"route_request_to_delivery_wall_ms_received_only_p99\":%.6f,",
            percentile(latency.deliveryWallMs,.5),percentile(latency.deliveryWallMs,.95),percentile(latency.deliveryWallMs,.99));
    }
    std::printf("\"tick_ms_mean\":%.6f,\"tick_ms_p50\":%.6f,\"tick_ms_p95\":%.6f,\"tick_ms_p99\":%.6f,\"tick_ms_max\":%.6f,\"setup_ms\":%.3f,\"event_ms\":%.3f,",
        sum/times.size(),percentile(times,.5),percentile(times,.95),percentile(times,.99),percentile(times,1),setupMs,eventMs);
    std::printf("\"alive\":%d,\"physical_in_goal\":%d,\"orders_complete\":%d,\"arrived_settled\":%d,\"arrival_rest_ticks\":%d,\"all_arrived_tick\":%d,\"arrival_tick_p50\":%d,\"arrival_tick_p95\":%d,\"max_goal_distance_px\":%.4f,",
        alive,physical,retired,arrived,restTicks,allAt,milestone(arrivalTimes,totalMoving,.5),milestone(arrivalTimes,totalMoving,.95),maxDistance);
    // AR-11 (unreachable only, so no other row gains a key): when each unit reached its approach point
    // (Legion's state 5, read-only) and when its orders emptied; the wait is the gap between the two.
    if(o.scenario=="unreachable") {
        std::sort(approachWaits.begin(),approachWaits.end());
        std::printf("\"approach_arrived\":%zu,\"approach_arrive_tick_p50\":%d,\"approach_arrive_tick_p95\":%d,\"approach_arrive_tick_max\":%d,",
            approachTimes.size(),milestone(approachTimes,totalMoving,.5),milestone(approachTimes,totalMoving,.95),milestone(approachTimes,totalMoving,1.0));
        std::printf("\"orders_retired_tick_p50\":%d,\"orders_retired_tick_p95\":%d,\"orders_retired_tick_max\":%d,\"approach_wait_max\":%d,\"approach_wait_p50\":%d,",
            milestone(retiredTimes,totalMoving,.5),milestone(retiredTimes,totalMoving,.95),milestone(retiredTimes,totalMoving,1.0),
            approachWaits.empty()?-1:approachWaits.back(),approachWaits.empty()?-1:approachWaits[(approachWaits.size()-1)/2]);
    }
    std::printf("\"final_command_tick\":%d,\"whole_group_latest_order_ticks\":%d,\"authored_goal_radius_px\":%.4f,",finalCommand,allAt<0?-1:allAt-finalCommand,members.empty()?0:members.front().radius);
    std::printf("\"first_move_tick_p50\":%d,\"first_move_tick_p95\":%d,\"admission_stamp_tick_p95\":%d,\"initial_segment_available_tick_p95\":%d,\"pending_clear_tick_p95\":%d,\"stalled_unit_ticks\":%llu,\"path_length_px\":%.4f,\"path_to_initial_straight_ratio\":%.6f,",
        milestone(firstMoves,totalMoving,.5),milestone(firstMoves,totalMoving,.95),milestone(firstAdmissions,totalMoving,.95),milestone(firstRoutes,totalMoving,.95),
        milestone(firstPendingClears,totalMoving,.95),(unsigned long long)stalled,totalPath,totalStraight>0?totalPath/totalStraight:0);
    std::printf("\"crossed_middle\":%d,\"first_cross_tick\":%d,\"last_cross_tick\":%d,\"crossings_per_sim_second\":%.6f,\"illegal_footprint_samples\":%llu,\"illegal_final_movers\":%d,\"peak_pending\":%llu,",
        crossed,firstCross,lastCross,crossed*30.0/o.ticks,(unsigned long long)illegalSamples,illegalFinal,(unsigned long long)peakPending);
    std::printf("\"retail_work_sum\":%llu,\"retail_requests\":%llu,\"retail_completions\":%llu,\"retail_failures\":%llu,\"process_peak_rss_kib\":%ld,",
        (unsigned long long)retail.workSpent(),(unsigned long long)retail.requests(),(unsigned long long)retail.completions(),(unsigned long long)retail.failures(),
        peakRss);
    {
        uint64_t spinning=0,everTerrain=0,everCrowd=0,everTrapped=0,trappedMovers=0,reachableMovers=0,optimalUnits=0,everRaw=0,everNarrowed=0;
        int settleMax=-1;uint64_t finals[ca::ClassCount]{};double ratioSum=0,ratioMax=0;
        int sides=0,minority=0,sided=0;
        for(size_t i=0;i<members.size();++i) {
            const auto& t=tracks[i];spinning+=t.spinTicks>0;
            if(!members[i].moving)continue;
            everTerrain+=t.everTerrainStuck;everCrowd+=t.everCrowdHeld;everTrapped+=t.everTrapped;
            everRaw+=t.everRawTerrain;everNarrowed+=t.everNarrowed;
            trappedMovers+=t.movingAfterGrace;if(t.everTrapped)settleMax=std::max(settleMax,t.settleMax);
            if(t.finalClass>=0)++finals[t.finalClass];
            reachableMovers+=reachable(members[i],*world.unit(members[i].id));
            if(t.optimal>0&&t.pathAtGoal>=0) {const double r=t.pathAtGoal/t.optimal;++optimalUnits;ratioSum+=r;ratioMax=std::max(ratioMax,r);}
        }
        for(int p=0;p<int(acc.blocks.size());++p) {
            int count[2]{};
            for(size_t i=0;i<members.size();++i)if(members[i].player==p&&tracks[i].side>=0)++count[tracks[i].side];
            sides+=(count[0]>0)+(count[1]>0);minority+=std::min(count[0],count[1]);sided+=count[0]+count[1];
        }
        std::printf("\"acceptance_window_ticks\":%d,\"acceptance_progress_px\":%d,\"acceptance_trapped_grace_ticks\":%d,",ca::window,ca::progressPx,ca::trappedGrace);
        std::printf("\"units_spinning\":%llu,\"spin_unit_ticks\":%llu,\"heading_change_while_stationary_deg\":%.3f,\"travel_reversals_while_stationary\":%llu,\"spin_while_trapped_unit_ticks\":%llu,\"spin_while_crowd_held_unit_ticks\":%llu,",
            (unsigned long long)spinning,(unsigned long long)totals.spinTicks,totals.headingStationaryBam*360.0/65536,(unsigned long long)totals.travelReversals,
            (unsigned long long)totals.spinTrapped,(unsigned long long)totals.spinCrowdHeld);
        for(int c=0;c<ca::ClassCount;++c)std::printf("\"class_%s_unit_ticks\":%llu,\"final_%s\":%llu,",ca::className(c),(unsigned long long)totals.classTicks[c],ca::className(c),(unsigned long long)finals[c]);
        std::printf("\"units_ever_terrain_stuck\":%llu,\"units_ever_crowd_held\":%llu,\"units_ever_trapped\":%llu,\"static_reachable_movers\":%llu,",
            (unsigned long long)everTerrain,(unsigned long long)everCrowd,(unsigned long long)everTrapped,(unsigned long long)reachableMovers);
        std::printf("\"raw_terrain_contact_noprogress_unit_ticks\":%llu,\"narrowed_to_crowd_held_unit_ticks\":%llu,\"crowd_and_terrain_unit_ticks\":%llu,\"units_ever_raw_terrain_contact\":%llu,\"units_ever_narrowed\":%llu,",
            (unsigned long long)totals.rawTerrainTicks,(unsigned long long)totals.narrowedTicks,(unsigned long long)totals.crowdAndTerrainTicks,
            (unsigned long long)everRaw,(unsigned long long)everNarrowed);
        std::printf("\"trapped_moving_unit_ticks\":%llu,\"trapped_units_moving_after_grace\":%llu,\"trapped_settle_ticks_max\":%d,",
            (unsigned long long)totals.trappedMoving,(unsigned long long)trappedMovers,settleMax);
        std::printf("\"path_optimality_units\":%llu,\"path_optimality_ratio_mean\":%.6f,\"path_optimality_ratio_max\":%.6f,",
            (unsigned long long)optimalUnits,optimalUnits?ratioSum/optimalUnits:-1.0,optimalUnits?ratioMax:-1.0);
        std::printf("\"group_sides_taken\":%d,\"group_minority_side_fraction\":%.6f,\"group_progress_spread_px_mean\":%.3f,\"group_progress_spread_px_max\":%.3f,",
            sides,sided?double(minority)/sided:0.0,totals.spreadSamples?totals.spreadSum/totals.spreadSamples:0.0,totals.spreadMax);
    }
    {
        // Keys added 2026-10-08 (LEGION-PLAN 3.8 housekeeping). All are
        // deterministic observations; none feeds back into the World.
        std::printf("\"turn_rate\":%d,",o.turnRate>0?o.turnRate:2500);
        std::printf("\"age_waiting_held_by_design_unit_ticks\":%llu,\"age_waiting_no_progress_unit_ticks\":%llu,\"age_parked_held_by_design_unit_ticks\":%llu,\"age_parked_no_progress_unit_ticks\":%llu,",
            (unsigned long long)ageTicks[0],(unsigned long long)ageTicks[1],(unsigned long long)ageTicks[2],(unsigned long long)ageTicks[3]);
        std::printf("\"moving_state_samples\":%llu,\"cap8_moving_samples\":%llu,",(unsigned long long)movingSamples,(unsigned long long)cap8Samples);
        if(routeHook&&legionMode)std::printf("\"route_crawl_samples\":%llu,",(unsigned long long)routeCrawlSamples);
        else std::printf("\"route_crawl_samples\":null,");
        if(legionMode)std::printf("\"live_groups_peak\":%llu,\"live_groups_mean\":%.3f,\"live_groups_min_after_warmup\":%lld,",
            (unsigned long long)liveGroupPeak,liveGroupSamples?double(liveGroupSum)/liveGroupSamples:0.0,
            liveGroupSamples?(long long)liveGroupMin:-1ll);
        if(shared) {
            // AR-08: where completed orders ended, in cells from the click.
            std::vector<double> dist;int outside=0;
            for(const auto& m:members)if(m.moving) {
                const auto& u=*world.unit(m.id);if(!u.alive()||!u.orders.empty())continue;
                const double d=std::hypot(double(u.x.toFloat()-m.gx),double(u.z.toFloat()-m.gz));
                dist.push_back(d/16);outside+=d>m.radius;
            }
            std::sort(dist.begin(),dist.end());
            std::printf("\"complete_outside_radius\":%d,\"complete_dist_median\":%.3f,\"complete_dist_max\":%.3f,",outside,
                dist.empty()?-1.0:dist.size()%2?dist[dist.size()/2]:(dist[dist.size()/2-1]+dist[dist.size()/2])/2,
                dist.empty()?-1.0:dist.back());
        }
        if(opposing) {
            // MV-03: the longest run of ticks without a crossing, from the
            // first crossing until 95% have crossed (or the run ends). A run
            // with no crossing at all is one gap of the whole run.
            std::vector<int> ticks;for(const auto& m:members)if(m.moving&&m.crossedTick>=0)ticks.push_back(m.crossedTick);
            std::sort(ticks.begin(),ticks.end());
            const size_t need=size_t(std::ceil(0.95*totalMoving));
            int gap=o.ticks;
            if(!ticks.empty()) {
                gap=0;const size_t last=std::min(ticks.size(),std::max<size_t>(need,1))-1;
                for(size_t k=1;k<=last;++k)gap=std::max(gap,ticks[k]-ticks[k-1]-1);
                if(ticks.size()<need)gap=std::max(gap,o.ticks-ticks.back());
            }
            std::printf("\"cross_gap_max\":%d,",gap);
        }
        if(churn)std::printf("\"churn_reorders\":%llu,\"churn_structures_placed\":%llu,\"churn_structures_removed\":%llu,\"churn_structures_skipped\":%llu,",
            (unsigned long long)churnReorders,(unsigned long long)churnPlaced,(unsigned long long)churnRemoved,(unsigned long long)churnSkipped);
    }
    printBuild();
    printDiagnostics(world,o.profile);
    std::printf("\"allocation_counting\":%s,\"tick_cpp_allocation_calls\":%llu,\"tick_cpp_allocation_requested_bytes\":%llu,\"hash\":\"%016llx\"}\n",
        o.allocations?"true":"false",(unsigned long long)crowdbench_allocation::calls.load(),(unsigned long long)crowdbench_allocation::bytes.load(),(unsigned long long)world.stateHash());
    return 0;
}
inline int main(int argc,char** argv) {
    if(argc==2&&std::string_view(argv[1])=="--help") {
        std::puts("crowdbench [legacy-scenario...]\ncrowdbench --mode retail|legion --units N --players N --moving-percent N --ticks N --scenario NAME [--seed N] [--turn-rate N] [--workers] [--allocations] [--profile] [--latency] [--trace PATH]\n"
            "Scenarios: open doors bridges maze opposingcolumns opposingdoors opposingbridges crossingcolumns sharedgoal mixedfootprints exploration dynamicobstacle rapidreplacement unreachable recovery recovery-passive churn\n"
            "churn: staggered re-orders (a 1/30 slice of the movers every 4 ticks, to seeded random goals in their lane) and a 4x4 structure placed or removed every 20 ticks.\n"
            "--turn-rate N sets the movers' turnRate and turnInPlaceRate (BAM/tick; default 2500).\n"
            "Acceptance scenarios: jagged trapped crowdtrap singleunit groupdetour (see tools/crowdbench_acceptance.h for metric definitions)\n"
            "units is per player; movement percentage rounds down per player. Default execution is serial.\n"
            "arrived_settled requires live, empty orders, zero speed, legal footprint, authored goal area and 30 unchanged ticks.\n"
            "Latency percentiles are censored (-1) if that fraction has not reached the event. Initial segment availability is not search delivery; pending clear can include a failed or cancelled search.\n"
            "--latency separately measures actual queued request to delivery callbacks in simulation ticks, including admission wait; received-only percentiles exclude cancelled and outstanding requests, whose counts/ages are reported. Wall-ms measures elapsed harness time between the same callbacks, including observation/events. This diagnostic adds allocation overhead.\n"
            "C++ allocation counting is opt-in, only during ticks; it excludes direct malloc and adds measurement overhead.\n"
            "Recovery explicitly reissues commands after removing the barrier; recovery-passive preserves the original mission. Rapid replacement stops after the first half of the run.\n"
            "Trace writes per-tick hashes/counters and controller/route changes plus 30-tick unit snapshots; traces are outside tick timings.");return 0;
    }
    try{return run(parse(argc,argv));}catch(const std::exception& error){std::fprintf(stderr,"crowdbench: %s\n",error.what());return 2;}
}
}
