// Legion navigation (PathfindingMode 4) World tests, asset-free.
//
//   legion_world_test CASE
//
// Cases: clearance groupreuse jagged trapped crowdhold replace unreachable
// quota determinism. Each builds a small flat map with feature walls (the
// same placement plane a loaded match uses) and checks one requirement of
// docs/legion-pathfinding.md.
#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "sim/footprint.h"
#include "legion_observe.h"
#include "net/orderpack.h"
#include <array>
#include <climits>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace tak::sim;
namespace {
void check(bool ok,const std::string& message) {if(!ok)throw std::runtime_error(message);}

UnitType mover(int foot) {
    UnitType t{};t.id=t.name="legion-foot-"+std::to_string(foot);
    t.canMove=true;t.maxHp=100;t.footX=t.footZ=foot;t.sight=4096;t.maxVel=Fixed::raw(117964);
    t.accel=t.brake=Fixed::fromInt(10);t.turnRate=t.turnInPlaceRate=2500;t.halfCellTicks=3;t.buildTime=1;
    return t;
}

// The start offset of the baselined tests (tools/scenarios/README.scn-format.txt):
// every spawn shifts by 0, +1 or -1 cells on both axes while walls and orders stay
// put. The cases with committed bounds run offsets 0,+1,-1 (the 3-offset median of
// the baseline) plus +2,-2 where the 3-offset spread exceeds the key's band
// (median-of-5, PLAN 3.0), and gate on the median.
int g_shift=0;
constexpr int kShifts[5]={0,1,-1,2,-2};
template<class T,size_t N> T medianOf(const T (&v)[N]) {
    std::vector<T> x(v,v+N);std::sort(x.begin(),x.end());return x[(N-1)/2];
}

struct Fixture;
void printLeft(Fixture& f,const std::vector<int>& ids);
struct Fixture {
    World world;
    int width,height;
    std::vector<uint16_t> cells;
    Fixture(int w,int h,bool serial=true,std::vector<uint8_t> heights={}):width(w),height(h),cells(size_t(w)*h,0xffff) {
        world.setGameSeed(7);world.setSerialThreads(serial);world.setPathService(true);
        world.setPathfindingMode(PathfindingMode::Legion);
        world.setPlayerCount(2);world.setTeam(0,0);world.setTeam(1,1);
        if(heights.empty())heights.assign(size_t(w)*h,100);
        world.setTerrain(heights,w,h,64);
    }
    void wall(int x,int z) {
        if(x<0||z<0||x>=width||z>=height)return;
        cells[size_t(z)*width+x]=0;world.blockCells(x,z,1,1,true);
    }
    void rect(int x,int z,int w,int h) {for(int j=0;j<h;++j)for(int i=0;i<w;++i)wall(x+i,z+j);}
    void open(int x,int z,int w,int h) {
        for(int j=0;j<h;++j)for(int i=0;i<w;++i)cells[size_t(z+j)*width+x+i]=0xffff;
        world.blockCells(x,z,w,h,false);
    }
    void publish() {world.setMapPlacementFeatures(cells,{{"legion-wall",1,1,true,true,false,0}});}
    int spawn(const UnitType& t,int cx,int cz,int player=0) {
        const int id=world.spawn(&t,float((cx+g_shift)*16),float((cz+g_shift)*16),std::nullopt,player);
        check(id>0,"spawn failed");return id;
    }
    void start() {
        world.tick(1.f/30);
        world.updateNavigationExploration();
        auto& explored=world.editNavigationExploration();
        std::fill(explored.begin(),explored.end(),0xffff);
    }
    bool legal(int id) {
        const auto& u=*world.unit(id);
        return world.mobilePlacement(u,footprintOrigin(u.x,u.type->footX),footprintOrigin(u.z,u.type->footZ),false);
    }
};

// Per-case motion metrics come from the shared const observer
// (tools/legion_observe.h). A Watch holds one: set `cfg` (groups, gates,
// sides, pairs, decisionEvery) before the first observe(), or leave it and
// one group of every id passed to the first call is watched. spins() and
// reversals() read the observer's `spins` / `reversals` keys (a spin is a
// heading change without any movement; a reversal is a return to a 16 px
// cell the body left less than 90 ticks earlier).
struct Watch {
    tak::legion_observe::Config cfg;
    std::unique_ptr<tak::legion_observe::Observer> obs;
    int64_t tick=0;
    Watch() {cfg.decisionEvery=0;}
    void observe(const World& w,const std::vector<int>& ids) {
        if(!obs) {
            if(cfg.groups.empty()) {tak::legion_observe::Group g;g.name="all";g.ids=ids;cfg.groups.push_back(std::move(g));}
            obs=std::make_unique<tak::legion_observe::Observer>(cfg);
        }
        obs->sample(w,tick++);
    }
    tak::legion_observe::Keys keys() const {return obs?obs->report():tak::legion_observe::Keys{};}
    int64_t key(std::string_view name,int64_t fallback=0) const {return tak::legion_observe::Observer::get(keys(),name,fallback);}
    uint64_t spins() const {return uint64_t(key("spins"));}
    uint64_t reversals() const {return uint64_t(key("reversals"));}
};

// The observer's AR-08 completion-distance keys of one group, as one text:
// how many members finished, how far from the click (cells) the median and
// the farthest finished, and how many finished outside the packed-disc radius.
std::string completionKeys(const tak::legion_observe::Keys& k,const std::string& group) {
    using tak::legion_observe::Observer;
    const std::string p="g."+group+".";
    char buf[160];
    std::snprintf(buf,sizeof buf,"complete_n=%lld complete_outside_radius=%lld complete_dist_median=%lld complete_dist_max=%lld",
        (long long)Observer::get(k,p+"complete_n",0),(long long)Observer::get(k,p+"complete_outside_radius",0),
        (long long)Observer::get(k,p+"complete_dist_median",-1),(long long)Observer::get(k,p+"complete_dist_max",-1));
    return buf;
}

void printLeft(Fixture& f,const std::vector<int>& ids) {
    for(int id:ids)if(!f.world.unit(id)->orders.empty()) {
        const auto& u=*f.world.unit(id);
        std::printf("  left id=%d at %.1f,%.1f goal %.1f,%.1f state=%d\n",id,u.x.toFloat()/16,u.z.toFloat()/16,
            u.orders.back().x.toFloat()/16,u.orders.back().z.toFloat()/16,f.world.legionNavigator()->unitState(id));
    }
}

void clearance() {
    // Terrain with slopes, water and jagged feature walls: Legion's static
    // plane must agree with mobilePlacement (bodies aside) at every origin.
    const int W=96,H=80;
    std::vector<uint8_t> heights(size_t(W)*H);
    uint32_t r=12345;
    for(int z=0;z<H;++z)for(int x=0;x<W;++x) {
        r=r*1103515245u+12345u;
        int h=100+int((x*7+z*3)%40)-((r>>16)%9==0?60:0);
        if(x>60&&z>50)h=30;   // a lake
        heights[size_t(z)*W+x]=uint8_t(std::clamp(h,0,255));
    }
    Fixture f(W,H,true,heights);
    for(int i=0;i<40;++i)f.wall(10+i,20+(i%3));          // jagged ridge
    for(int i=0;i<25;++i)f.wall(30+(i*i)%7,40+i);
    f.publish();
    std::vector<UnitType> types;
    for(int foot=1;foot<=4;++foot) {auto t=mover(foot);t.maxSlope=8+foot*4;types.push_back(t);}
    types.push_back(mover(2));types.back().footX=3;types.back().id=types.back().name="legion-rect";
    std::vector<int> ids;
    for(auto& t:types)ids.push_back(f.spawn(t,2+int(ids.size())*0,2));
    f.start();
    auto* legion=f.world.legionNavigator();
    check(legion!=nullptr,"legion navigator not created in Legion mode");
    uint64_t compared=0,legalCount=0;
    for(int id:ids) {
        const auto& u=*f.world.unit(id);
        for(int z=-1;z<=H;++z)for(int x=-1;x<=W;++x) {
            // Bodies only matter where this unit's own footprint is: the plane
            // excludes mobile bodies, so compare away from every other unit.
            bool nearBody=false;
            for(int other:ids)if(other!=id) {
                const auto& o=*f.world.unit(other);
                const int ox=footprintOrigin(o.x,o.type->footX),oz=footprintOrigin(o.z,o.type->footZ);
                if(x<ox+o.type->footX&&x+u.type->footX>ox&&z<oz+o.type->footZ&&z+u.type->footZ>oz)nearBody=true;
            }
            if(nearBody)continue;
            const bool a=legion->staticLegal(u,x,z),b=f.world.mobilePlacement(u,x,z,false);
            if(a!=b)throw std::runtime_error("plane differs from mobilePlacement for "+u.type->id+" at "+
                std::to_string(x)+","+std::to_string(z)+" plane="+std::to_string(a));
            ++compared;legalCount+=a;
        }
    }
    check(legalCount>1000&&legalCount<compared,"clearance fixture is degenerate");
    std::printf("clearance compared=%llu legal=%llu\n",(unsigned long long)compared,(unsigned long long)legalCount);
}

void groupreuse() {
    Fixture f(160,64);
    f.rect(70,0,4,28);f.rect(70,36,4,28);   // a door
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<40;++i)ids.push_back(f.spawn(type,10+(i%8)*3,14+(i/8)*3));
    f.start();
    for(size_t i=0;i<ids.size();++i)f.world.order(ids[i],float((110+(int(i)%8)*4)*16),float((12+int(i)/8*4)*16),false);
    for(int t=0;t<5;++t)f.world.tick(1.f/30);
    auto* legion=f.world.legionNavigator();
    const int group=legion->unitGroup(ids[0]);
    for(int id:ids)check(legion->unitGroup(id)==group,"group order split into several groups");
    // Fields started, not finished: since W4 C1 a lattice group's first field
    // pauses once it covers the group (it is never "built" to done).
    auto started=[&](const LegionNavigator::Stats& st) {uint64_t n=0;for(uint64_t k:st.fieldsStartedByKind)n+=k;return n;};
    const auto s=f.world.legionStats();
    check(started(s)==1,"one group must build exactly one field, started "+std::to_string(started(s)));
    int routeSeen=0;   // the committed detour's length (observation hook), the longest sampled
    for(int t=0;t<6000;++t) {
        f.world.tick(1.f/30);
        for(int id:ids)routeSeen=std::max(routeSeen,legion->routeLength(id));
    }
    int arrived=0;
    for(int id:ids)arrived+=f.world.unit(id)->orders.empty();
    const auto e=f.world.legionStats();
    check(legion->routeLength(-1)==0,"routeLength of a non-member is not 0");
    check((e.detours>0)==(routeSeen>0),"routeLength disagrees with the detours planned: "+std::to_string(e.detours)+" detours, longest route "+std::to_string(routeSeen));
    printLeft(f,ids);
    std::printf("  detours=%llu cells=%llu holds=%llu\n",(unsigned long long)f.world.legionStats().detours,(unsigned long long)f.world.legionStats().detourCells,(unsigned long long)f.world.legionStats().holds);
    std::printf("groupreuse arrived=%d fields=%llu work=%llu\n",arrived,(unsigned long long)e.fieldsBuilt,(unsigned long long)e.fieldWork);
    check(arrived==int(ids.size()),"group did not pass the door");
    check(started(e)<=2,"field rebuilt per member");
}

// A sawtooth ridge with one gap at the far end: units must slide along the
// teeth (never stall pressing into them) and route around through the gap.
void jaggedRun(int count,int stride,int foot,const char* label) {
    Fixture f(128,96);
    for(int i=0;i<70;++i) {
        const int x=30+i,z=20+i/2+((i%5)<2?(i%5):0);
        f.wall(x,z);f.wall(x,z+1);
        if(i%7==3)f.wall(x,z-1);
    }
    for(int z=0;z<20;++z)f.wall(30,z);
    f.publish();
    const auto type=mover(foot);
    std::vector<int> ids;
    for(int i=0;i<count;++i)ids.push_back(f.spawn(type,40+(i%4)*(foot+2),8+(i/4)*(foot+2)));
    f.start();
    for(size_t i=0;i<ids.size();++i)f.world.order(ids[i],float((44+int(i%4)*stride)*16),float((62+int(i/4)*stride)*16),false);
    Watch motion;uint64_t pressing=0;
    for(int t=0;t<4000;++t) {
        f.world.tick(1.f/30);
        motion.observe(f.world,ids);
        for(int id:ids) {
            const auto& u=*f.world.unit(id);
            check(f.legal(id),"illegal footprint on jagged terrain");
            // Pressing: an ordered body that Legion is moving (not holding)
            // yet has no speed -- the signature of pushing into a wall.
            if(!u.orders.empty()&&f.world.legionNavigator()->unitState(id)==1&&u.speed==Fixed())++pressing;
        }
    }
    int arrived=0;for(int id:ids)arrived+=f.world.unit(id)->orders.empty();
    for(int id:ids)if(!f.world.unit(id)->orders.empty()) {
        const auto& u=*f.world.unit(id);
        std::printf("  left id=%d at %.1f,%.1f goal %.1f,%.1f state=%d\n",id,u.x.toFloat()/16,u.z.toFloat()/16,
            u.orders.back().x.toFloat()/16,u.orders.back().z.toFloat()/16,f.world.legionNavigator()->unitState(id));
    }
    // AR-08 (W3 no-worse keys): each unit has its own goal, so the completion distance is measured from
    // that goal (cells); a unit counts as outside its radius when it finished more than 3 cells from it.
    std::vector<double> own;int outside=0;
    for(size_t i=0;i<ids.size();++i) {
        const auto& u=*f.world.unit(ids[i]);
        if(!u.orders.empty())continue;
        const double d=std::hypot(u.x.toFloat()/16-double(44+int(i%4)*stride),u.z.toFloat()/16-double(62+int(i/4)*stride));
        own.push_back(d);outside+=d>3;
    }
    std::printf("jagged %s arrived=%d/%zu spins=%llu reversals=%llu pressing=%llu complete_n=%zu complete_outside_radius=%d"
        " complete_dist_median=%.1f complete_dist_max=%.1f\n",label,arrived,ids.size(),
        (unsigned long long)motion.spins(),(unsigned long long)motion.reversals(),(unsigned long long)pressing,own.size(),outside,
        own.empty()?-1.0:(std::sort(own.begin(),own.end()),own[(own.size()-1)/2]),own.empty()?-1.0:own.back());
    check(arrived==int(ids.size()),std::string("units stuck against jagged terrain: ")+label);
    check(motion.spins()==0,"units turned in place while stuck");
    check(pressing==0,"units pressed into terrain");
}
void jagged() {
    jaggedRun(1,0,2,"single-2x2");
    jaggedRun(1,0,3,"single-3x3");
    jaggedRun(1,0,4,"single-4x4");
    jaggedRun(12,6,2,"group-2x2");
}

void trapped() {
    // A sealed pocket: the body inside stops at once and never moves, turns
    // or rocks; its order waits out the grace period, then retires.
    {
        Fixture f(64,64);
        f.rect(10,10,12,1);f.rect(10,21,12,1);f.rect(10,10,1,12);f.rect(21,10,1,12);
        f.publish();
        const auto type=mover(2);
        const int inside=f.spawn(type,15,15),outside=f.spawn(type,40,15);
        f.start();
        f.world.order(inside,50*16,50*16,false);f.world.order(outside,50*16,50*16,false);
        Watch motion;
        int clearedAt=-1;uint64_t moved=0;
        int32_t x=f.world.unit(inside)->x.v,z=f.world.unit(inside)->z.v;
        for(int t=0;t<9200;++t) {
            f.world.tick(1.f/30);motion.observe(f.world,{inside});
            const auto& u=*f.world.unit(inside);
            moved+=u.x.v!=x||u.z.v!=z;x=u.x.v;z=u.z.v;
            if(clearedAt<0&&u.orders.empty())clearedAt=t;
        }
        std::printf("trapped cleared_at=%d moved_ticks=%llu spins=%llu reversals=%llu\n",clearedAt,
            (unsigned long long)moved,(unsigned long long)motion.spins(),(unsigned long long)motion.reversals());
        check(moved==0,"trapped unit moved");
        check(clearedAt>=8950&&clearedAt<=9060,"trapped order not retired after the grace period");
        check(motion.spins()==0&&motion.reversals()==0,"trapped unit turned or rocked");
        check(f.world.legionStats().trapped>=1,"trapped classification not recorded");
        check(f.world.unit(outside)->orders.empty(),"free unit did not arrive");
    }
    // The same pocket opens before the grace period ends: the held order
    // resumes and the unit arrives (change-driven wake-up, no polling).
    {
        Fixture f(64,64);
        f.rect(10,10,12,1);f.rect(10,21,12,1);f.rect(10,10,1,12);f.rect(21,10,1,12);
        f.publish();
        const auto type=mover(2);const int inside=f.spawn(type,15,15);
        f.start();
        f.world.order(inside,50*16,50*16,false);
        for(int t=0;t<300;++t)f.world.tick(1.f/30);
        check(!f.world.unit(inside)->orders.empty(),"trapped order retired too early");
        f.open(21,14,1,4);f.publish();
        for(int t=0;t<1500;++t)f.world.tick(1.f/30);
        std::printf("trapped reopened arrived=%d\n",int(f.world.unit(inside)->orders.empty()));
        check(f.world.unit(inside)->orders.empty(),"unit did not resume after the pocket opened");
        const auto& u=*f.world.unit(inside);
        check(std::abs(u.x.toFloat()-800)<20&&std::abs(u.z.toFloat()-800)<20,"resumed unit did not reach its goal");
    }
    // Legion 10,000 stage 5 (the trapped rule on RouteView::reach): static
    // churn on another island after the route is complete must not hold the
    // verdict off for ever. The churn bumps the shared static epoch without
    // touching the route, so nothing refreshes the route; an epoch test
    // ("built on older geometry: unknown") left the body Waiting for the
    // whole run and its order never retired (the verifier's pocket.scn,
    // 2026-10-16). The churn stops at tick 60: a trapped body re-registers
    // on every static change anywhere (trappedEpoch), so under churn that
    // never stops its grace restarts and no order retires -- inherited from
    // the starting revision.
    {
        Fixture f(256,64);
        f.rect(126,0,4,64);
        f.rect(18,18,12,2);f.rect(18,28,12,2);f.rect(18,18,2,12);f.rect(28,18,2,12);
        f.publish();
        const auto type=mover(2);
        std::vector<int> outside;
        for(int i=0;i<10;++i)outside.push_back(f.spawn(type,40+3*i,40));
        const int inside=f.spawn(type,23,23);
        f.start();
        for(const int id:outside)f.world.order(id,100*16,32*16,false);
        f.world.order(inside,100*16,32*16,false);
        int clearedAt=-1;
        for(int t=0;t<9300&&clearedAt<0;++t) {
            if(t>=20&&t<60&&t%10==0)   // a blocking feature placed and lifted on the far island (churn ... toggle)
                f.world.addFeature(30*256+200,200*16+16.f,30*16+16.f,0,2,2,(t/10)%2==0,-1,true);
            f.world.tick(1.f/30);
            if(f.world.unit(inside)->orders.empty())clearedAt=t;
        }
        std::printf("trapped churn=island cleared_at=%d trapped=%llu\n",clearedAt,(unsigned long long)f.world.legionStats().trapped);
        check(clearedAt>=0,"trapped order never retired after churn on another island");
        check(f.world.legionStats().trapped>=1,"trapped classification not recorded after churn on another island");
        for(const int id:outside)check(f.world.unit(id)->orders.empty(),"free unit did not arrive after churn on another island");
    }
    // Legion 10,000 stage 7: the same island, churn that never stops (an improvement every other change, all on the
    // far island). A trapped member re-registers only after an improvement touching its own component's box
    // (trappedReopened), so its grace runs out and the order retires (until stage 7 every change anywhere restarted
    // it: c2c25707's island-forever, inherited through stage 6). Then the same churn with the pocket opened at tick
    // 300: that improvement touches the pocket, the member re-registers and arrives.
    for(const bool opens:{false,true}) {
        Fixture f(256,64);
        f.rect(126,0,4,64);
        f.rect(18,18,12,2);f.rect(18,28,12,2);f.rect(18,18,2,12);f.rect(28,18,2,12);
        f.publish();
        const auto type=mover(2);
        const int inside=f.spawn(type,23,23);
        f.start();
        f.world.order(inside,100*16,32*16,false);
        int clearedAt=-1;
        for(int t=0;t<9400&&clearedAt<0;++t) {
            if(t>=20&&t%10==0)f.world.addFeature(30*256+200,200*16+16.f,30*16+16.f,0,2,2,(t/10)%2==0,-1,true);
            if(opens&&t==300) {f.open(28,22,2,4);f.publish();}
            f.world.tick(1.f/30);
            if(f.world.unit(inside)->orders.empty())clearedAt=t;
        }
        const auto a=f.world.legionNavigator()->accounting();
        std::printf("trapped churn=forever opens=%d cleared_at=%d kept=%llu reopened=%llu\n",int(opens),clearedAt,
                    (unsigned long long)a.trappedKept,(unsigned long long)a.trappedReopened);
        if(!opens) {
            check(clearedAt>=8950&&clearedAt<=9200,"trapped order did not retire under never-ending churn on another island");
            check(a.trappedKept>0&&a.trappedReopened==0,"trapped member re-registered on changes that could not reopen it");
        } else {
            const auto& u=*f.world.unit(inside);
            check(clearedAt>=0&&clearedAt<3000,"trapped member did not resume when its pocket opened under churn");
            check(std::abs(u.x.toFloat()-1600)<24&&std::abs(u.z.toFloat()-512)<24,"reopened trapped member did not reach its goal");
            check(a.trappedReopened>0,"the pocket's opening was not seen as a reopening");
        }
    }
}

// Legion 10,000 stage 7: closures apart from improvements. A group walks east on its field; a 2x2 rock off its way
// (inside the field's window) is lifted at tick 60 -- an improvement only: the route is touched (its "unreached" no
// longer proves anything) but keeps steering, and its refresh waits kImproveDefer (150) ticks; a rock placed at
// tick 400 -- a closure -- makes it stale at once. Every body arrives.
void improvedefer() {
    Fixture f(160,64);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<4;++i)ids.push_back(f.spawn(type,6+(i%2)*3,30+(i/2)*3));
    f.start();
    f.world.addFeature(10*160+80,80*16+16.f,10*16+16.f,0,2,2,true,-1,true);   // a rock off the way
    for(const int id:ids)f.world.order(id,150*16,32*16,false);
    const auto* nav=f.world.legionNavigator();
    auto acct=[&] {return nav->accounting();};
    uint64_t opened=0,closed=0,promoted=0;
    int done=-1;
    for(int t=0;t<4000&&done<0;++t) {
        if(t==60) {
            f.world.addFeature(10*160+80,80*16+16.f,10*16+16.f,0,2,2,false,-1,true);   // lifted: an improvement
            opened=acct().routesOpened;closed=acct().routesClosed;promoted=acct().improveRefreshes;
        }
        if(t==62) {
            check(acct().routesOpened>opened,"improvedefer: the lifted rock did not reach the route as an improvement");
            check(acct().routesClosed==closed,"improvedefer: an improvement was taken for a closure");
        }
        if(t==60+140)check(acct().improveRefreshes==promoted,"improvedefer: the improvement refresh was not deferred");
        if(t==60+160)check(acct().improveRefreshes==promoted+1,"improvedefer: the deferred improvement refresh never came");
        if(t==400) {
            closed=acct().routesClosed;
            f.world.addFeature(54*160+100,100*16+16.f,54*16+16.f,0,2,2,true,-1,true);   // placed off the way: a closure
        }
        if(t==402)check(acct().routesClosed>closed,"improvedefer: the placed rock did not reach the route as a closure");
        f.world.tick(1.f/30);
        bool all=true;for(const int id:ids)all&=f.world.unit(id)->orders.empty();
        if(all)done=t;
    }
    const auto a=acct();
    std::printf("improvedefer done=%d opened=%llu closed=%llu improve_refreshes=%llu improve_demands=%llu\n",done,
                (unsigned long long)a.routesOpened,(unsigned long long)a.routesClosed,(unsigned long long)a.improveRefreshes,
                (unsigned long long)a.improveDemands);
    check(done>=0,"improvedefer: a body did not arrive");
}

// Legion 10,000 stage 7: a chain whose regions are relabelled while it builds re-resolves them on the new geometry
// (resolveChainNode: the dirty tile rebuilt, the route kept) instead of leaving them off the route. The stage-6 chains
// fixture with a 2x2 rock toggled every tick in four open transit sectors east of the wall during the first 300
// ticks (none of them severs a portal): resolves happen, no region is abandoned, every body arrives.
void chainresolve() {
    Fixture f(512,320);
    for(int z=0;z<320;++z)
        if(!(z>=40&&z<52)&&!(z>=150&&z<170)&&!(z>=262&&z<268)) {f.wall(250,z);f.wall(251,z);}
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int s=0;s<8;++s)for(int i=0;i<6;++i)ids.push_back(f.spawn(type,10+(i%3)*3,16+s*36+(i/3)*3));
    f.start();
    for(size_t i=0;i<ids.size();++i) {
        const int s=int(i/6);
        f.world.order(ids[i],float((470-(s%3)*12)*16),float((24+((s*5)%8)*36)*16),false);
    }
    const int rocks[4][2]={{300,80},{340,200},{380,120},{420,250}};
    int t=0,done=0;
    for(;t<9000&&done<int(ids.size());++t) {
        if(t>=2&&t<300)for(const auto& r:rocks)
            f.world.addFeature(r[1]*512+r[0],float(r[0]*16+16),float(r[1]*16+16),0,2,2,t%2==0,-1,true);
        f.world.tick(1.f/30);
        done=0;for(const int id:ids)done+=f.world.unit(id)->orders.empty();
    }
    const auto a=f.world.legionNavigator()->accounting();
    std::printf("chainresolve ticks=%d arrived=%d/%zu chains=%llu resolves=%llu abandons=%llu repairs=%llu\n",t,done,ids.size(),
                (unsigned long long)a.chains,(unsigned long long)a.chainResolves,(unsigned long long)a.chainAbandons,
                (unsigned long long)a.chainRepairs);
    check(a.chains>=4,"chainresolve: the squads did not take tile chains");
    check(a.chainResolves>0,"chainresolve: no chain region was re-resolved on the new geometry");
    check(a.chainAbandons==0,"chainresolve: a chain region was left off its route by a local change");
    check(done==int(ids.size()),"chainresolve: a body never arrived");
}

// ---- Route requests on large maps (2026-11-15; docs/legion-pathfinding.md "Hierarchical routes and tiles", "Static
// changes and routes", section 9). On a 2048 x 2048 map a route over the whole component takes an eighth of the field
// allowance; these cases build the five situations that asked for one where a small route does, on small maps with
// the allowance shrunk (setFieldBytesForTest: never below two whole-map fields, so a whole-component window is half
// of it), and check the routes, the storage charged and what the bodies do.
struct RouteWatch {
    const LegionNavigator* nav;
    uint64_t peak=0;
    explicit RouteWatch(World& w):nav(w.legionNavigator()) {}
    void sample() {peak=std::max<uint64_t>(peak,nav->accounting().bytes.fields);}
};
struct SmallAllowance {
    explicit SmallAllowance(uint64_t bytes) {LegionNavigator::setFieldBytesForTest(bytes);}
    ~SmallAllowance() {LegionNavigator::setFieldBytesForTest(0);}
};

// A lone mover whose first bounded window does not see round a wall. The window fails it and the group is full: it
// took the whole component's window. Where that window is large (over 1/32 of the allowance) the group now plans: a
// chain, a few tiles. With the game's allowance a 512 x 512 component is not large and the window stays.
void fullplan() {
    struct Run {int done=-1;uint64_t chains=0,plans=0,peak=0;};
    const auto run=[](uint64_t bytes) {
        SmallAllowance allowance(bytes);
        Fixture f(512,512);
        f.rect(60,150,4,212);   // the first window (body and goal plus 34 cells) lies inside its span
        f.publish();
        const auto type=mover(2);
        const int id=f.spawn(type,12,256);
        f.start();
        f.world.order(id,80*16,256*16,false);
        RouteWatch watch(f.world);
        Run out;
        for(int t=0;t<4000&&out.done<0;++t) {
            f.world.tick(1.f/30);watch.sample();
            if(f.world.unit(id)->orders.empty())out.done=t;
        }
        const auto a=watch.nav->accounting();
        out.chains=a.chains;out.plans=a.fullPlans;out.peak=watch.peak;
        return out;
    };
    const uint64_t whole=4ull*512*512;
    const Run large=run(1),small=run(0);
    std::printf("fullplan large: done=%d chains=%llu full_plans=%llu field_bytes_peak=%llu | game allowance: done=%d chains=%llu full_plans=%llu field_bytes_peak=%llu\n",
                large.done,(unsigned long long)large.chains,(unsigned long long)large.plans,(unsigned long long)large.peak,
                small.done,(unsigned long long)small.chains,(unsigned long long)small.plans,(unsigned long long)small.peak);
    check(large.done>=0&&small.done>=0,"fullplan: the mover did not arrive");
    check(large.chains>=1&&large.plans>=1,"fullplan: a full group on a large component did not plan a chain");
    check(large.peak<whole/4,"fullplan: a full group on a large component took the whole component's window");
    check(small.chains==0&&small.plans==0&&small.peak>=whole,"fullplan: a full group took no window where its component is not large");
}

// Two batches of 30 sent to one point 40 ticks apart, as a computer player sends them: two commands. The second
// batch's demand is its own 30 bodies (the first batch's field was tried for a share before the plan and does not
// serve it), so it takes a chain as the first did; counted with the first batch it was "high demand" and built the
// window of its whole way. One command of 60 keeps its one exact field.
void demandown() {
    struct Run {int arrived=0;uint64_t chains=0,peak=0;};
    const auto run=[](bool oneCommand) {
        Fixture f(512,320);
        for(int z=0;z<320;++z)
            if(!(z>=40&&z<52)&&!(z>=150&&z<170)&&!(z>=262&&z<268)) {f.wall(250,z);f.wall(251,z);}
        f.publish();
        const auto type=mover(2);
        std::vector<int> north,south;
        for(int i=0;i<30;++i) {north.push_back(f.spawn(type,10+(i%6)*3,20+(i/6)*3));south.push_back(f.spawn(type,10+(i%6)*3,280+(i/6)*3));}
        f.start();
        RouteWatch watch(f.world);
        Run out;
        for(int t=0;t<6000;++t) {
            if(t==0)for(const int id:north)f.world.order(id,470*16,160*16,false);
            if(t==(oneCommand?0:40))for(const int id:south)f.world.order(id,470*16,160*16,false);
            f.world.tick(1.f/30);watch.sample();
            out.arrived=0;
            for(const auto* ids:{&north,&south})for(const int id:*ids)out.arrived+=f.world.unit(id)->orders.empty();
            if(out.arrived==60)break;
        }
        out.chains=watch.nav->accounting().chains;out.peak=watch.peak;
        return out;
    };
    const Run two=run(false),one=run(true);
    std::printf("demandown two commands: arrived=%d/60 chains=%llu field_bytes_peak=%llu | one command: arrived=%d/60 chains=%llu field_bytes_peak=%llu\n",
                two.arrived,(unsigned long long)two.chains,(unsigned long long)two.peak,one.arrived,(unsigned long long)one.chains,(unsigned long long)one.peak);
    check(two.arrived==60&&one.arrived==60,"demandown: a body did not arrive");
    check(two.chains>=2,"demandown: the second command to the point did not take a chain (counted with the first)");
    check(two.peak<4ull*512*320/2,"demandown: a batch of 30 built the window of its whole way");
    check(one.chains==0,"demandown: one command of 60 bodies took chains (its own crowd is its demand)");
}

// A corpse or a building on the goal of a body under way. seedcover: a 1 x 1 feature lands on the goal's footprint;
// the goal origin is illegal now but has legal neighbours. When the route is next searched (a rock lifted beside
// it: an improvement, whose refresh chooses the route afresh) the region search enters the goal through those
// neighbours and the route stays a chain; it skipped the illegal goal, found nothing, and the plan fell back to the
// window -- the whole map, the seed having no component. seedburied: a 2 x 2 feature covers the whole footprint: no
// step leaves the seed, the chain's refresh reaches nobody, the group plans again and has no goal to search from; its
// window holds one cell and is charged for one tile (it was charged the whole map); the body is trapped either way.
void seedcover() {
    struct Run {int done=-1;uint64_t chains=0,fallbacks=0,bound=0,peak=0,trapped=0;};
    const auto run=[](bool buried) {
        Fixture f(512,320);
        for(int z=0;z<320;++z)
            if(!(z>=40&&z<52)&&!(z>=150&&z<170)&&!(z>=262&&z<268)) {f.wall(250,z);f.wall(251,z);}
        f.publish();
        const auto type=mover(2);
        const int id=f.spawn(type,20,100);
        f.start();
        f.world.addFeature(140*512+300,300*16+16.f,140*16+16.f,0,2,2,true,-1,true);   // a rock beside the way
        f.world.order(id,470*16,160*16,false);   // the goal origin is (469,159): cells 469..470 x 159..160
        RouteWatch watch(f.world);
        Run out;
        for(int t=0;t<(buried?600:5000)&&out.done<0;++t) {
            if(t==30) {
                check(watch.nav->accounting().chains==1,"seedcover: the body's first route is no chain (the case tests nothing)");
                if(buried)f.world.addFeature(159*512+469,470*16.f,160*16.f,0,2,2,true,-1,true);
                else f.world.addFeature(160*512+470,470*16+8.f,160*16+8.f,0,1,1,true,-1,true);
            }
            if(t==80&&!buried)f.world.addFeature(140*512+300,300*16+16.f,140*16+16.f,0,2,2,false,-1,true);   // the rock lifted
            f.world.tick(1.f/30);watch.sample();
            if(f.world.unit(id)->orders.empty())out.done=t;
        }
        const auto a=watch.nav->accounting();
        out.chains=a.chains;out.fallbacks=a.routeFallbacks;out.bound=a.seedBoundStarts;out.peak=watch.peak;
        out.trapped=f.world.legionStats().trapped;
        return out;
    };
    const uint64_t whole=4ull*512*320;
    const Run cover=run(false),buried=run(true);
    std::printf("seedcover done=%d chains=%llu fallbacks=%llu field_bytes_peak=%llu | seedburied seed_bound=%llu fallbacks=%llu trapped=%llu field_bytes_peak=%llu\n",
                cover.done,(unsigned long long)cover.chains,(unsigned long long)cover.fallbacks,(unsigned long long)cover.peak,
                (unsigned long long)buried.bound,(unsigned long long)buried.fallbacks,(unsigned long long)buried.trapped,(unsigned long long)buried.peak);
    check(cover.done>=0,"seedcover: the mover did not arrive beside its covered goal");
    check(cover.chains>=3&&cover.fallbacks==0,"seedcover: the search did not enter the covered goal through its neighbours");
    check(cover.peak<whole/4,"seedcover: a covered goal cost a whole-map window");
    check(buried.fallbacks>=1&&buried.trapped>=1,"seedburied: the buried goal's group did not fall back to its window and trap its body (the case tests nothing)");
    check(buried.bound>=1&&buried.peak<whole/4,"seedburied: a field that cannot leave its seed was charged its window");
}

// The seed-bound charge is taken back when the build can leave its seeds after all. Two bodies under way on
// whole-map windows (the hierarchy off: every plan a window); a feature buries the second one's goal. Both routes
// are stale in that tick: the first group's refresh starts, and its initialisation and first slice take the tick's
// refresh allowance; the second's starts seed-bound and gets no slice. The feature is lifted before the next tick:
// the first slice finds the seed's steps open and charges the window before it writes.
void seedrestore() {
    LegionNavigator::setHierarchicalRoutesForTest(false);
    struct Reset {~Reset() {LegionNavigator::setHierarchicalRoutesForTest(true);}} reset;
    const int S=1024;
    Fixture f(S,S);
    f.publish();
    const auto type=mover(2);
    const int far=f.spawn(type,20,20),near=f.spawn(type,20,500);
    f.start();
    f.world.order(far,1000*16,1000*16,false);
    f.world.tick(1.f/30);   // its own issue tick: the lower group id, the older request
    f.world.order(near,900*16,500*16,false);   // goal origin (899,499)
    for(int t=0;t<60;++t)f.world.tick(1.f/30);
    const auto* nav=f.world.legionNavigator();
    const uint64_t whole=4ull*S*S;
    f.world.addFeature(499*S+899,900*16.f,500*16.f,0,2,2,true,-1,true);
    f.world.tick(1.f/30);
    const auto a0=nav->accounting();
    std::printf("seedrestore buried: seed_bound=%llu restores=%llu fields=%zu field_bytes=%llu\n",(unsigned long long)a0.seedBoundStarts,
                (unsigned long long)a0.seedBoundRestores,f.world.legionStats().liveFields,(unsigned long long)a0.bytes.fields);
    check(a0.seedBoundStarts==1&&a0.seedBoundRestores==0,"seedrestore: the buried goal's refresh did not start seed-bound and unserved (the case tests nothing)");
    f.world.addFeature(499*S+899,900*16.f,500*16.f,0,2,2,false,-1,true);   // lifted: the seed has steps again
    f.world.tick(1.f/30);
    const auto a1=nav->accounting();
    std::printf("seedrestore lifted: restores=%llu fields=%zu field_bytes=%llu\n",(unsigned long long)a1.seedBoundRestores,
                f.world.legionStats().liveFields,(unsigned long long)a1.bytes.fields);
    check(a1.seedBoundRestores==1,"seedrestore: the window was not charged when its seed got a step");
    check(a1.bytes.fields>=a0.bytes.fields+whole-65536,"seedrestore: the restored window is not charged in full");
    int t=0;
    for(;t<9000&&!f.world.unit(near)->orders.empty();++t)f.world.tick(1.f/30);
    check(f.world.unit(near)->orders.empty(),"seedrestore: the body of the uncovered goal did not arrive");
}

// A body cut off from its goal after it registered, on a map that keeps changing elsewhere. An enclosure with one
// gap; the body inside is ordered to a point outside; the gap is closed while it walks. No route on its plane
// reaches it, and the whole component built to prove that was touched by the next change anywhere and built again
// for as long as the unit kept its order, the unit Waiting. It is trapped by its component now: it walks on toward
// its goal until the wall stops it, asks for no field, and its order retires after the grace period. With the gap
// reopened in time (reopen) it goes on as the member it is -- no new registration -- and arrives.
void cutoff() {
    struct Run {int trappedAt=-1,clearedAt=-1;uint64_t fields=0,cutTraps=0,resumed=0,registrations=0,registered=0,walked=0;bool arrived=false;};
    const auto run=[](bool reopen) {
        Fixture f(256,256);
        f.rect(20,20,104,4);f.rect(20,120,104,4);f.rect(20,20,4,104);f.rect(120,20,4,46);f.rect(120,76,4,48);   // gap x 120..123, z 66..75
        f.publish();
        const auto type=mover(2);
        const int id=f.spawn(type,40,70);
        f.start();
        f.world.order(id,220*16,70*16,false);
        Run out;
        const int32_t x0=f.world.unit(id)->x.v;
        const int ticks=reopen?4000:9800;
        for(int t=0;t<ticks;++t) {
            if(t==30) {f.rect(120,66,4,10);f.publish();}
            if(reopen&&t==1200) {f.open(120,66,4,10);f.publish();}
            if(t%7==0)f.world.addFeature(230*256+20,20*16+16.f,230*16+16.f,0,2,2,(t/7)%2==0,-1,true);   // churn in a far corner
            f.world.tick(1.f/30);
            const int state=f.world.legionNavigator()->unitState(id);
            if(out.trappedAt<0&&state==5) {
                out.trappedAt=t;out.walked=uint64_t((f.world.unit(id)->x.v-x0)>>16);
                out.registered=f.world.legionStats().registrations;
            }
            if(out.clearedAt<0&&f.world.unit(id)->orders.empty())out.clearedAt=t;
            if(reopen&&out.clearedAt>=0)break;
        }
        const auto a=f.world.legionNavigator()->accounting();
        const auto s=f.world.legionStats();
        out.fields=s.fieldsBuilt;out.cutTraps=a.cutTraps;out.resumed=a.trappedResumed;out.registrations=s.registrations;
        const auto& u=*f.world.unit(id);
        out.arrived=std::abs(u.x.toFloat()-220*16)<24&&std::abs(u.z.toFloat()-70*16)<24;
        return out;
    };
    const Run closed=run(false),reopened=run(true);
    std::printf("cutoff closed: trapped_at=%d walked_px=%llu cleared_at=%d fields=%llu cut_traps=%llu | reopened: trapped_at=%d cleared_at=%d arrived=%d resumed=%llu registrations_after_trap=%llu fields=%llu\n",
                closed.trappedAt,(unsigned long long)closed.walked,closed.clearedAt,(unsigned long long)closed.fields,(unsigned long long)closed.cutTraps,
                reopened.trappedAt,reopened.clearedAt,int(reopened.arrived),(unsigned long long)reopened.resumed,(unsigned long long)(reopened.registrations-reopened.registered),(unsigned long long)reopened.fields);
    check(closed.trappedAt>=0&&closed.trappedAt<1500&&closed.cutTraps==1,"cutoff: the cut-off body was not trapped by its component");
    check(closed.walked>=40*16,"cutoff: the cut-off body did not walk on toward its goal before it was trapped");
    check(closed.fields<=12,"cutoff: fields were built over and over for a body no route can reach");
    check(closed.clearedAt>=closed.trappedAt+8950&&closed.clearedAt<=closed.trappedAt+9060,"cutoff: the cut-off order did not retire after the grace period");
    check(reopened.arrived&&reopened.clearedAt>=0,"cutoff: the body did not arrive after its way reopened");
    check(reopened.resumed==1&&reopened.registrations==reopened.registered,"cutoff: the reopened body was registered afresh instead of going on in place");
}

// The cut test asks every seed, not one cell standing for the group. Two bodies ordered in one tick to goals 12
// cells apart are one group with two seeds; a wall then splits the map between the goals, each body on its goal's
// side. The group's component cell is the first goal's: the second body's component is another, yet its own goal
// is beside it. Both arrive, neither is trapped.
void splitseeds() {
    Fixture f(256,128);
    f.publish();
    const auto type=mover(2);
    const int west=f.spawn(type,20,60),east=f.spawn(type,236,60);
    f.start();
    f.world.order(west,100*16,60*16,false);
    f.world.order(east,112*16,60*16,false);
    check(f.world.legionNavigator()->unitGroup(west)==f.world.legionNavigator()->unitGroup(east),"splitseeds: the two goals did not form one group (the case tests nothing)");
    f.rect(105,0,2,128);f.publish();
    int t=0;
    for(;t<3000&&!(f.world.unit(west)->orders.empty()&&f.world.unit(east)->orders.empty());++t)f.world.tick(1.f/30);
    const auto& w=*f.world.unit(west);const auto& e=*f.world.unit(east);
    std::printf("splitseeds ticks=%d west=%.1f,%.1f east=%.1f,%.1f trapped=%llu cut_traps=%llu\n",t,w.x.toFloat()/16,w.z.toFloat()/16,
                e.x.toFloat()/16,e.z.toFloat()/16,(unsigned long long)f.world.legionStats().trapped,
                (unsigned long long)f.world.legionNavigator()->accounting().cutTraps);
    check(w.orders.empty()&&e.orders.empty(),"splitseeds: a body beside its own goal did not arrive");
    check(std::abs(w.x.toFloat()-100*16)<24&&std::abs(e.x.toFloat()-112*16)<24,"splitseeds: a body did not reach its own goal");
    check(f.world.legionStats().trapped==0,"splitseeds: a body in its own goal's component was trapped");
}

// The new route state is copied with the world and hashed. The run holds a seed-bound window (a buried goal), a
// plan over the seeds' cells and a trapped cut-off body (a body penned in after its order), a full group that plans
// (a body whose first window lies behind a wall) and chains a churned gap keeps touching. Every 25 ticks the world
// is cloned; the clone then gets the same orders and changes as the original for 20 ticks and must have the same
// state hash after each. (Group::awaited is a plain member of the group, copied with it; legion_refreshfirst runs
// it serial against workers.)
void routeclone() {
    SmallAllowance allowance(1);
    Fixture f(512,320);
    for(int z=0;z<320;++z)
        if(!(z>=40&&z<52)&&!(z>=150&&z<170)&&!(z>=262&&z<268)) {f.wall(250,z);f.wall(251,z);}
    f.rect(60,100,60,4);f.rect(60,160,60,4);f.rect(60,100,4,64);f.rect(116,100,4,24);f.rect(116,136,4,28);   // a pen, gap z 124..135
    f.rect(180,180,4,110);   // the wall a first window lies behind
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int s=0;s<6;++s)for(int i=0;i<4;++i)ids.push_back(f.spawn(type,10+(i%2)*3,20+s*48+(i/2)*3));
    const int penned=f.spawn(type,80,130),buried=f.spawn(type,30,300),behind=f.spawn(type,140,235);
    f.start();
    const auto input=[&](World& w,int t) {
        if(t<6)for(int i=0;i<4;++i)w.order(ids[size_t(t*4+i)],float((470-(t%3)*12)*16),float((30+((t*5)%6)*48)*16),false);
        if(t==6) {
            w.order(penned,480*16,130*16,false);
            w.order(buried,440*16,300*16,false);
            w.order(behind,200*16,235*16,false);
            w.addFeature(299*512+439,440*16.f,300*16.f,0,2,2,true,-1,true);   // the buried goal
        }
        if(t==40)for(int k=0;k<6;++k)w.addFeature((124+2*k)*512+116,116*16+32.f,(124+2*k)*16+16.f,0,4,2,true,-1,true);   // the pen closes
        if(t>=60&&t%9==0)w.addFeature(140*512+256,256*16+16.f,140*16+16.f,0,2,2,(t/9)%2==0,-1,true);   // churn beside the middle gap
        if(t==400)w.addFeature(299*512+439,440*16.f,300*16.f,0,2,2,false,-1,true);
    };
    std::vector<std::pair<int,std::unique_ptr<World>>> clones;
    uint64_t compared=0;
    for(int t=0;t<900;++t) {
        input(f.world,t);
        f.world.tick(1.f/30);
        const uint64_t hash=f.world.stateHash();
        for(auto& [since,copy]:clones) {
            input(*copy,t);
            copy->tick(1.f/30);
            check(copy->stateHash()==hash,"routeclone: a clone of tick "+std::to_string(since)+" differs from the original at tick "+std::to_string(t));
            ++compared;
        }
        clones.erase(std::remove_if(clones.begin(),clones.end(),[&](const auto& c) {return t-c.first>=20;}),clones.end());
        if(t%25==7)clones.emplace_back(t,f.world.clone());
    }
    const auto a=f.world.legionNavigator()->accounting();
    std::printf("routeclone compared=%llu seed_bound=%llu seed_plans=%llu cut_traps=%llu awaited_ticks=%llu full_plans=%llu chains=%llu hash=%016llx\n",
                (unsigned long long)compared,(unsigned long long)a.seedBoundStarts,(unsigned long long)a.seedPlans,(unsigned long long)a.cutTraps,
                (unsigned long long)a.awaitedTicks,(unsigned long long)a.fullPlans,(unsigned long long)a.chains,(unsigned long long)f.world.stateHash());
    check(a.seedBoundStarts>0&&a.seedPlans>0&&a.cutTraps>0&&a.fullPlans>0,"routeclone: the run held no seed-bound window, no seeds-only plan, no cut-off body or no full group's plan (the case tests nothing)");
}

// Legion 10,000 stage 7: traffic in coarse route selection. A wall crosses a 640 x 384 map with two gaps; with the
// map empty the walker's chain takes the north gap (the region search's choice on this geometry), but here 121 idle
// bodies (another player's) stand in the sector before it. The region search charges the regions by their soft share
// (trafficPenalty), so the chain goes through the south gap instead and the walker arrives; without the charge (a
// deliberate mutation) it takes the north gap into the idle block.
void trafficlane() {
    Fixture f(640,384);
    for(int z=0;z<384;++z)if(!(z>=2&&z<34)&&!(z>=250&&z<282)) {f.wall(320,z);f.wall(321,z);}
    f.publish();
    const auto type=mover(2);
    std::vector<int> idle;
    for(int z=3;z<=33;z+=3)for(int x=290;x<=317;x+=3)idle.push_back(f.spawn(type,x,z,1));
    const int walker=f.spawn(type,20,60);
    f.start();
    for(int t=0;t<120;++t)f.world.tick(1.f/30);   // the idle bodies become soft (two still scans)
    f.world.order(walker,620*16,360*16,false);
    int done=-1,maxZAtWall=-1;
    for(int t=0;t<9000&&done<0;++t) {
        f.world.tick(1.f/30);
        const auto& u=*f.world.unit(walker);
        const int x=int(u.x.v>>20),z=int(u.z.v>>20);
        if(x>=300&&x<=340)maxZAtWall=std::max(maxZAtWall,z);
        if(u.orders.empty())done=t;
    }
    const auto a=f.world.legionNavigator()->accounting();
    std::printf("trafficlane done=%d max_z_at_wall=%d chains=%llu traffic_charges=%llu\n",done,maxZAtWall,
                (unsigned long long)a.chains,(unsigned long long)a.trafficCharges);
    check(a.chains>=1,"trafficlane: the walker did not take a chain");
    check(a.trafficCharges>0,"trafficlane: the search charged no region for traffic");
    check(maxZAtWall>=240,"trafficlane: the walker's route went through the idle block instead of round it");
    check(done>=0,"trafficlane: the walker did not arrive");
}

// Perf follow-up I: the route invalidation index (sim/legion_routebins.h). Four walled lanes, ten groups in each, every
// group its own order (its own issue tick) to its own goal along its lane, on two movement planes; blocking features
// toggle and structures are built and destroyed at spatially separated spots of lanes 0 and 2 only, units are
// re-ordered (fields replaced, refreshes discarded) and a quarter of them die (groups erased). With the index on and
// off (LegionNavigator::setRouteIndexForTest) the checkpoint hashes, the reached groups' decisions
// (routeIndexStats().decisions: ids and closure / improvement flags in visit order) and the closure / improvement
// counts agree, serial equals workers, and the index visits far fewer groups than the walk of every group (the routes
// of lanes 1 and 3 are never candidates). The ctest runs it under TAK_LEGION_VERIFY: every tick checks the index
// against the groups' fields, every static change against the walk.
struct RouteIndexRun {uint64_t hash=0,trail=0,closed=0,opened=0;int alive=0,arrived=0;LegionNavigator::RouteIndexStats r;};
RouteIndexRun routeindexRun(bool index,bool serial) {
    LegionNavigator::setRouteIndexForTest(index);
    Fixture f(320,262,serial);
    for(int k=1;k<4;++k)f.rect(0,k*66-2,320,2);   // lanes z 0..61, 66..127, 132..193, 198..261
    f.publish();
    const auto small=mover(1),large=mover(2);
    UnitType keep{};keep.id=keep.name="legion-routeindex-keep";keep.maxHp=100;keep.footX=keep.footZ=4;keep.buildTime=1;keep.maxVel=Fixed();
    std::vector<int> ids,keeps;
    for(int k=0;k<40;++k)ids.push_back(f.spawn(k%3==0?small:large,6+(k%2)*14,10+(k/10)*66+(k/2%5)*10));
    f.start();
    // Churn spots in lanes 0 and 2, on the routes and beside them: x, z, period (a blocking feature placed, lifted, ...).
    static constexpr int kSpots[6][3]={{90,30,7},{200,12,9},{290,50,11},{120,160,13},{230,140,8},{300,185,10}};
    RouteIndexRun out;
    for(int t=0;t<1500;++t) {
        if(t<40)f.world.order(ids[size_t(t)],float((200+(t*37)%110)*16),float((6+(t/10)*66+(t*53)%50)*16),false);
        if(t==500)for(size_t i=1;i<ids.size();i+=5)
            if(const Unit* u=f.world.unit(ids[i]);u&&u->alive())f.world.order(ids[i],float((40+int(i)*5)*16),float((8+int(i)/10*66+int(i)%40)*16),false);
        if(t==800)for(size_t i=3;i<ids.size();i+=4)if(Unit* u=f.world.unit(ids[i]))u->hp=Fixed();
        for(const auto& s:kSpots)if(t%s[2]==0)
            f.world.addFeature(s[1]*f.width+s[0],s[0]*16+8.f,s[1]*16+8.f,0,1,1,(t/s[2])%2==0,-1,true);
        if(t==200||t==700)keeps.push_back(f.world.spawn(&keep,float(160*16),float((t==200?40:150)*16),std::nullopt,1));
        if((t==450||t==1100)&&!keeps.empty()) {
            if(Unit* u=f.world.unit(keeps.front()))u->hp=Fixed();
            keeps.erase(keeps.begin());
        }
        f.world.tick(1.f/30);
        if(t%150==149)out.trail=(out.trail^f.world.stateHash())*0x100000001b3ull;
    }
    out.hash=f.world.stateHash();
    const auto* nav=f.world.legionNavigator();
    out.r=nav->routeIndexStats();
    out.closed=nav->accounting().routesClosed;out.opened=nav->accounting().routesOpened;
    for(int id:ids)if(const Unit* u=f.world.unit(id);u&&u->alive()) {++out.alive;out.arrived+=u->orders.empty();}
    LegionNavigator::setRouteIndexForTest(true);
    return out;
}
// ... and a pending refresh that covers other ground than the field it replaces: single bodies on long routes across a
// wall with two gaps (corridors and chains: tile-listed routes), gap A closed under them so their refreshes re-plan
// through gap B, and churn beside gap B while those refreshes build. A change there reaches only the refresh (`next`),
// which the index must list too (it failed with a listing of the field alone: the decisions differed).
struct RouteIndexRefreshRun {uint64_t hash=0,trail=0,decisions=0,touched=0,tiled=0;};
RouteIndexRefreshRun routeindexRefreshRun(bool index) {
    LegionNavigator::setRouteIndexForTest(index);
    Fixture f(640,256);
    f.rect(320,0,4,20);f.rect(320,36,4,164);f.rect(320,216,4,40);   // gap A z 20..35, gap B z 200..215
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int k=0;k<8;++k)ids.push_back(f.spawn(type,20,24+k*8));
    f.start();
    RouteIndexRefreshRun out;
    for(int t=0;t<700;++t) {
        if(t<8)f.world.order(ids[size_t(t)],float((600+t*4)*16),float((24+t*8)*16),false);
        if(t==150)for(int z=20;z<36;++z)for(int x=320;x<324;++x)f.world.addFeature(z*f.width+x,x*16+8.f,z*16+8.f,0,1,1,true,-1,true);
        if(t>=150&&t<450&&t%3==0) {
            f.world.addFeature(222*f.width+330,330*16+8.f,222*16+8.f,0,1,1,(t/3)%2==0,-1,true);
            f.world.addFeature(194*f.width+300,300*16+8.f,194*16+8.f,0,1,1,(t/3)%2==1,-1,true);
        }
        f.world.tick(1.f/30);
        if(t%100==99)out.trail=(out.trail^f.world.stateHash())*0x100000001b3ull;
    }
    out.hash=f.world.stateHash();
    const auto* nav=f.world.legionNavigator();
    out.decisions=nav->routeIndexStats().decisions;out.touched=nav->routeIndexStats().touched;
    out.tiled=nav->accounting().corridors+nav->accounting().chains;
    LegionNavigator::setRouteIndexForTest(true);
    return out;
}
void routeindex() {
    const auto on=routeindexRun(true,true),off=routeindexRun(false,true),workers=routeindexRun(true,false);
    auto line=[](const char* name,const RouteIndexRun& x) {
        std::printf("routeindex %s hash=%016llx arrived=%d/%d calls=%llu indexed=%llu walk_groups=%llu candidates=%llu touched=%llu "
                    "closed=%llu opened=%llu decisions=%016llx lists=%llu entries_added=%llu entries_scanned=%llu stale_dropped=%llu "
                    "compactions=%llu entries_peak=%llu bytes=%llu\n",name,(unsigned long long)x.hash,x.arrived,x.alive,
                    (unsigned long long)x.r.calls,(unsigned long long)x.r.indexed,(unsigned long long)x.r.walkGroups,
                    (unsigned long long)x.r.candidates,(unsigned long long)x.r.touched,(unsigned long long)x.closed,
                    (unsigned long long)x.opened,(unsigned long long)x.r.decisions,(unsigned long long)x.r.lists,
                    (unsigned long long)x.r.entriesAdded,(unsigned long long)x.r.entriesScanned,(unsigned long long)x.r.staleDropped,
                    (unsigned long long)x.r.compactions,(unsigned long long)x.r.entriesPeak,(unsigned long long)x.r.bytes);
    };
    line("index",on);line("walk",off);line("index-workers",workers);
    check(on.hash==off.hash&&on.trail==off.trail,"routeindex: the route index changed the world");
    check(on.r.decisions==off.r.decisions&&on.r.touched==off.r.touched,"routeindex: the route index reached other groups");
    check(on.closed==off.closed&&on.opened==off.opened,"routeindex: the route index changed closures or improvements");
    check(workers.hash==on.hash&&workers.trail==on.trail&&workers.r.decisions==on.r.decisions,"routeindex: serial and workers differ");
    check(off.r.indexed==0&&on.r.indexed>0,"routeindex: the switch did not select the index");
    check(on.r.candidates*3<on.r.walkGroups*2,"routeindex: the index did not leave out the lanes no change reaches");
    check(on.r.touched>0&&on.closed>0&&on.opened>0,"routeindex: the churn reached no route (the case tests nothing)");
    const auto ron=routeindexRefreshRun(true),roff=routeindexRefreshRun(false);
    std::printf("routeindex refresh index hash=%016llx decisions=%016llx touched=%llu tiled=%llu | walk hash=%016llx decisions=%016llx touched=%llu\n",
                (unsigned long long)ron.hash,(unsigned long long)ron.decisions,(unsigned long long)ron.touched,(unsigned long long)ron.tiled,
                (unsigned long long)roff.hash,(unsigned long long)roff.decisions,(unsigned long long)roff.touched);
    check(ron.tiled>0,"routeindex refresh: no corridor or chain was built (the case tests nothing)");
    check(ron.hash==roff.hash&&ron.trail==roff.trail,"routeindex refresh: the route index changed the world");
    check(ron.decisions==roff.decisions&&ron.touched==roff.touched,"routeindex refresh: the route index reached other groups");
}

void longroute() {
    // A connected route farther than 16-bit potentials reach: the serpentine
    // of tools/scenarios/serplong.scn (30 lanes of 600 cells, 14 high, about
    // 17,500 cells to the click). A 16-bit field saturated at 65535, so the
    // bodies never got a finite potential and stood trapped; the field must
    // reach them (87,356 and 87,331) and they must walk.
    constexpr int kLanes=30,kLength=600,kLane=14;
    const int boxH=2+kLanes*kLane+(kLanes-1)*2;
    Fixture f(16+kLength+4+16,16+boxH+16);
    f.rect(16,16,kLength+4,2);f.rect(16,16+boxH-2,kLength+4,2);
    f.rect(16,16,2,boxH);f.rect(18+kLength,16,2,boxH);
    for(int k=0;k+1<kLanes;++k)f.rect(k%2?18+kLane:18,18+(k+1)*kLane+k*2,kLength-kLane,2);   // gaps alternate ends
    f.publish();
    const auto type=mover(2);
    const std::vector<int> ids{f.spawn(type,26,25),f.spawn(type,30,25)};
    f.start();
    for(int id:ids)f.world.order(id,30*16,float((18+(kLanes-1)*(kLane+2)+7)*16),false);
    std::vector<int32_t> x0;for(int id:ids)x0.push_back(f.world.unit(id)->x.v);
    auto* nav=f.world.legionNavigator();
    std::vector<int> potential(ids.size(),-1);
    for(int t=0;t<400;++t) {
        f.world.tick(1.f/30);
        if(t==100)for(size_t i=0;i<ids.size();++i) {
            const auto& u=*f.world.unit(ids[i]);
            potential[i]=nav->fieldPotential(ids[i],footprintOrigin(u.x,2),footprintOrigin(u.z,2));
        }
    }
    std::vector<int> moved;
    for(size_t i=0;i<ids.size();++i)moved.push_back(int((int64_t(f.world.unit(ids[i])->x.v)-x0[i])>>20));
    std::printf("longroute potential=%d,%d moved_cells=%d,%d states=%d,%d trapped=%llu\n",potential[0],potential[1],moved[0],moved[1],
        nav->unitState(ids[0]),nav->unitState(ids[1]),(unsigned long long)f.world.legionStats().trapped);
    for(size_t i=0;i<ids.size();++i) {
        check(potential[i]>65535&&potential[i]<INT_MAX,"the field does not reach a body past the 16-bit range");
        check(nav->unitState(ids[i])!=5,"a body on a long connected route is trapped");
        check(moved[i]>=24,"a body on a long connected route did not walk");
    }
    check(f.world.legionStats().trapped==0,"trapped classification recorded on a connected route");
}

void crowdhold() {
    // A one-body corridor plugged by an enemy that never moves: the column
    // behind it must hold still -- no turning in place, no back-and-forth.
    Fixture f(128,40);
    f.rect(40,0,30,18);f.rect(40,21,30,19);    // corridor rows 18..20 (3 cells)
    f.publish();
    const auto type=mover(2);
    const int plug=f.spawn(type,55,20,1);(void)plug;
    std::vector<int> ids;
    for(int i=0;i<20;++i)ids.push_back(f.spawn(type,8+(i%5)*3,10+(i/5)*4));
    f.start();
    for(int id:ids)f.world.order(id,110*16,20*16,false);
    Watch motion;
    for(int t=0;t<1500;++t) {
        f.world.tick(1.f/30);
        if(t>600)motion.observe(f.world,ids);
    }
    int holding=0;for(int id:ids)holding+=f.world.legionNavigator()->unitState(id)==2;
    std::printf("crowdhold holding=%d spins=%llu reversals=%llu\n",holding,(unsigned long long)motion.spins(),(unsigned long long)motion.reversals());
    check(motion.spins()==0,"held crowd turned in place");
    check(motion.reversals()<=4,"held crowd oscillated");
    for(int id:ids)check(f.legal(id),"illegal footprint in held crowd");
}

// AR-10 (PLAN 3.4, W5 step 6): the claims invariant -- no cell claimed by two
// members, no lost claim, no slot on a point that is gone (ring spots too).
void checkClaims(World& world,const char* where) {
    const auto a=world.legionNavigator()->claimsAudit();
    if(a.overlaps+a.missing+a.dangling)
        std::printf("%s: claims overlaps=%d missing=%d dangling=%d\n",where,a.overlaps,a.missing,a.dangling);
    check(a.overlaps+a.missing+a.dangling==0,"the claims invariant failed (overlapping, lost or dangling slot claims)");
}
// A group sent across open ground with a large standing block of idle
// bodies on its straight way (of the same player, then of another): it
// must plan round the block, not walk into it and wait against its face.
// Every member arrives in about the time of the way round, nobody spins.
// W8 step 1 (PLAN 3.2 T2, C10): nearObstacle -- the static clearance memo plus the per-query
// soft part -- against an exhaustive search on 1e5 random origins (no command, a command
// without arrivals, and each command with settled arrivals: their own arrivals are not soft
// to them, everyone else's are), including the map edge; and every member's vertex probe
// against an exhaustive visibility scan of its descent chain. The map is corner-1x48's
// ragged corner with an idle block of the other player (soft to every command) and a
// settled 24-body arrival of player 0 (soft to every command but its own).
void lanegeom() {
    Fixture f(220,170);
    f.rect(80,60,40,110);f.rect(150,0,4,78);
    for(const auto& [x,z]:std::vector<std::pair<int,int>>{{81,58},{86,57},{91,59},{96,58},{101,57},{106,59},{111,58},{116,57},
        {78,59},{79,57},{77,61},{76,64},{79,66},{78,73}})f.wall(x,z);
    f.publish();
    const auto type=mover(2);
    std::vector<int> idle,early,ids;
    for(int c=0;c<6;++c)for(int r=0;r<4;++r)idle.push_back(f.spawn(type,125+c*3,20+r*3,1));
    for(int i=0;i<24;++i)early.push_back(f.spawn(type,20+(i%6)*3,20+(i/6)*3));
    for(int i=0;i<48;++i)ids.push_back(f.spawn(type,14+(i%8)*3,104+(i/8)*3));
    f.start();
    for(int id:early)f.world.order(id,60*16,40*16,false);
    for(int t=0;t<900;++t)f.world.tick(1.f/30);
    for(int id:ids)f.world.order(id,190*16,25*16,false);
    auto* legion=f.world.legionNavigator();
    int bad=0;
    for(int t=1;t<=1800;++t) {
        f.world.tick(1.f/30);
        if(t%600==0) {
            std::string report;
            bad+=legion->laneGeometryCheck(*f.world.unit(ids[0]),34000,uint64_t(t)*2654435761u,&report);
            std::printf("lanegeom t=%d %s",t,report.c_str());
        }
    }
    const auto s=f.world.legionStats();
    const uint64_t bent=s.vertexCalls-s.vertexVisible;
    std::printf("lanegeom vertex calls=%llu found=%llu visible=%llu near=%llu blocked=%llu work=%llu found_of_bent_permille=%llu\n",
        (unsigned long long)s.vertexCalls,(unsigned long long)s.vertexFound,(unsigned long long)s.vertexVisible,
        (unsigned long long)s.vertexNear,(unsigned long long)s.vertexBlocked,(unsigned long long)s.vertexWork,
        (unsigned long long)(bent?s.vertexFound*1000/bent:0));
    check(bad==0,"nearObstacle or the vertex probe differs from the exhaustive search");
    check(s.vertexFound>0,"the vertex probe never found a vertex at the corner");
}

int staticblockRun(int owner) {
    Fixture f(200,100);f.publish();
    const auto type=mover(2);
    std::vector<int> block,ids;
    if(owner>=0)for(int c=0;c<10;++c)for(int r=0;r<12;++r)block.push_back(f.spawn(type,92+c*3,32+r*3,owner));
    for(int i=0;i<40;++i)ids.push_back(f.spawn(type,20+(i%8)*3,44+(i/8)*3));
    f.start();
    for(int t=0;t<90;++t)f.world.tick(1.f/30);   // the block has stood still a while
    for(int id:ids)f.world.order(id,160*16,50*16,false);
    Watch motion;
    int t=0,arrived=0,half=-1;
    for(;t<4000&&arrived<int(ids.size());++t) {
        f.world.tick(1.f/30);motion.observe(f.world,ids);
        arrived=0;for(int id:ids)arrived+=f.world.unit(id)->orders.empty();
        if(half<0&&2*arrived>=int(ids.size()))half=t;
    }
    arrived=0;for(int id:ids)arrived+=f.world.unit(id)->orders.empty();
    for(int id:ids)check(f.legal(id),"illegal footprint");
    const auto s=f.world.legionStats();
    std::printf("staticblock owner=%d arrived=%d/%zu ticks=%d half=%d holds=%llu detours=%llu spins=%llu reversals=%llu\n",owner,arrived,ids.size(),t,half,
        (unsigned long long)s.holds,(unsigned long long)s.detours,(unsigned long long)motion.spins(),(unsigned long long)motion.reversals());
    if(std::getenv("STATIC_VERBOSE"))printLeft(f,ids);
    if(std::getenv("STATIC_REPORT"))return t;   // measure only (e.g. on an older build)
    check(arrived==int(ids.size()),"group did not get round the standing block");
    check(motion.spins()==0,"group spun at the standing block");
    // The way round the block (by one side, clear of its face) is barely
    // longer than the straight 140 cells. On open ground (no block) half the
    // group arrives at 1226 ticks and all of it at 1803; round the block it
    // streams by one side in a narrower file, so the tail is longer. Without
    // soft obstacles only 11 of 40 had arrived after 4000 ticks.
    check(half<=1800,"half the group took far longer than the way round");
    check(t<=3600,"group took far longer than the way round");
    return t;
}
void staticblock() {
    if(std::getenv("STATIC_OPEN"))staticblockRun(-1);   // reference: no block
    staticblockRun(0);staticblockRun(1);
}

// W4 (T7 Stage B1): idle upkeep. A group that holds in place (here: guards
// of an idle friend) keeps its finished field and goes stale whenever static
// obstacles change anywhere on the map; before B1 Legion refreshed it all the
// same, work nobody steers by. Part 1 parks a group of guards, then runs 600
// ticks beside continuous "construction" (a far cell toggled every 10 ticks:
// the static epoch advances and every cached field goes stale) and requires
// ZERO refresh field work for it (failed by construction on the W4 step-0
// head: 60 refreshes, 764640 relaxations). Part 2: a parked group whose route
// a new wall cuts, ordered on again, re-plans and arrives. Part 3: a group
// held at a plugged gap re-plans through a gap that opens, by the blocked
// re-request alone (no member moves), as soon as the base does.
void staticidle() {
    // Part 2: the order-on re-plan. A wall with one far gap appears after the group parked.
    {
        Fixture f(200,100);
        f.publish();
        const auto type=mover(2);
        std::vector<int> ids;
        for(int i=0;i<12;++i)ids.push_back(f.spawn(type,10+(i%4)*3,40+(i/4)*3));
        f.start();
        for(int id:ids)f.world.order(id,60*16,50*16,false);
        int ticks=0;
        auto parked=[&] {for(int id:ids)if(!f.world.unit(id)->orders.empty())return false;return true;};
        for(;ticks<3000&&!parked();++ticks)f.world.tick(1.f/30);
        check(parked(),"staticidle: the group never parked");
        for(int t=0;t<90;++t)f.world.tick(1.f/30);
        // The wall cuts the straight way to the new goal; its only gap is at the bottom.
        for(int z=0;z<85;++z)f.world.blockCells(100,z,1,1,true);
        const uint64_t before=f.world.legionStats().fieldWork;
        for(int id:ids)f.world.order(id,150*16,50*16,false);
        int cut=0;
        for(;cut<4000;++cut) {
            f.world.tick(1.f/30);
            if(parked())break;
        }
        const uint64_t built=f.world.legionStats().fieldWork-before;
        std::printf("staticidle replan arrived_after=%d field_work=%llu\n",cut,(unsigned long long)built);
        check(parked(),"staticidle: the group did not reach the goal past the new wall");
        // 120 ticks + the build time: the walk to the gap and back is well under 2000 ticks; the
        // re-plan itself must not stall the order (no body idle > 120 ticks before the field exists).
        check(built>0,"staticidle: the cut route was not re-planned");
    }
    // Part 3: the blocked re-request. A pair sent through the one gap in a
    // wall finds it plugged by another player's idle body: both end up
    // holding (the group is inactive). A second gap, closed by a feature,
    // then opens (a static change): no member moves, so only the blocked
    // re-request (a member held kBlockedRetry ticks outside its area on a way
    // only soft bodies close) starts the refresh that finds the new gap. Base
    // (refresh on every change) and B1 both arrive at once; without the
    // blocked rule the refresh waits for a body to move again.
    {
        Fixture f(200,100);
        f.rect(100,0,2,48);f.rect(100,50,2,2);f.rect(100,54,2,46);   // the second gap (z 52-53): a feature closes it
        f.publish();
        const auto type=mover(2);
        const int plug=f.spawn(type,101,49,1);   // another player's idle body in the gap
        std::vector<int> ids;
        for(int i=0;i<2;++i)ids.push_back(f.spawn(type,80,46+i*4));
        f.start();
        auto gate=[&](bool closed) {f.world.addFeature(52*200+100,101*16.f,53*16.f,0,2,2,closed,-1,true);};
        gate(true);
        for(int t=0;t<90;++t)f.world.tick(1.f/30);   // the plug has stood still a while
        for(int id:ids)f.world.order(id,150*16,50*16,false);
        auto parked=[&] {for(int id:ids)if(!f.world.unit(id)->orders.empty())return false;return true;};
        for(int t=0;t<900;++t)f.world.tick(1.f/30);
        int holding=0;
        for(int id:ids)holding+=f.world.legionNavigator()->unitState(id)==2;
        if(std::getenv("STATIC_VERBOSE"))std::printf("  plug at %.1f,%.1f\n",f.world.unit(plug)->x.toFloat()/16,f.world.unit(plug)->z.toFloat()/16);
        if(std::getenv("STATIC_VERBOSE"))for(int id:ids)std::printf("  unit %d state %d at %.0f,%.0f\n",id,f.world.legionNavigator()->unitState(id),f.world.unit(id)->x.toFloat()/16,f.world.unit(id)->z.toFloat()/16);
        check(!parked(),"staticidle: the plugged group arrived before the second gap opened");
        gate(false);
        int open=0;
        for(;open<3000&&!parked();++open)f.world.tick(1.f/30);
        const auto s=f.world.legionStats();
        std::printf("staticidle blocked holding=%d arrived_after=%d blocked_rerequests=%llu refresh_suppressed=%llu demand_resumes=%llu\n",holding,open,
            (unsigned long long)s.blockedRerequests,(unsigned long long)s.refreshSuppressed,(unsigned long long)s.demandResumes);
        check(parked(),"staticidle: a blocked group did not re-plan through the gap that opened");
        // Measured: 549 ticks at the step-0 base (refresh on every change) and with B1 (the plugged
        // body had been blocked > 120 ticks, so the refresh starts at once); 892 with the blocked
        // rule removed (the refresh waits until a body happens to move). Bound: base + 120.
        check(open<=549+120,"staticidle: the blocked re-request did not start the refresh in time");
    }
    // Part 1: parked groups beside continuous construction. Twelve bodies guard
    // an idle friend in a corridor: they walk up to it and hold with their
    // orders, their group and its finished field alive, nothing steering by the
    // field. A cell toggled every 10 ticks inside the field's reach is the
    // construction.
    Fixture f(200,100);
    f.rect(0,0,200,44);f.rect(0,56,200,44);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<12;++i)ids.push_back(f.spawn(type,10+(i%4)*3,45+(i/4)*3));
    const int friendly=f.spawn(type,100,50);
    f.start();
    for(int id:ids)f.world.guard(id,friendly,false);
    // 2400 ticks, not 900 (W5 exit): the guards walk ~90 cells (~700 ticks), then twelve of them settle round
    // the ward; a reach kind never takes the settle rule (its owner ends the approach), so the last few keep
    // routing round the engaged ones until the crowd has packed (quiet by tick ~2000 here, measured; at the
    // W4 head it was quiet by 900 because the settle rule parked them).
    for(int t=0;t<2400;++t)f.world.tick(1.f/30);
    int held=0;
    // Holding, or Engaged (6: braked within 70 px of the ward by its guard order, AR-06)
    for(int id:ids) {const int s=f.world.legionNavigator()->unitState(id);held+=!f.world.unit(id)->orders.empty()&&(s==2||s==6);}
    check(held>=6,"staticidle: the guards are not holding with their orders");
    const auto base=f.world.legionStats();
    for(int t=0;t<600;++t) {
        if(t%10==0)f.world.addFeature(50*200+60,60*16+8.f,50*16+8.f,0,1,1,t%20==0,-1,true);
        f.world.tick(1.f/30);
    }
    if(std::getenv("STATIC_VERBOSE"))for(int id:ids)std::printf("  guard %d state %d at %.1f,%.1f\n",id,f.world.legionNavigator()->unitState(id),f.world.unit(id)->x.toFloat()/16,f.world.unit(id)->z.toFloat()/16);
    const auto now=f.world.legionStats();
    const uint64_t idle=now.fieldWorkRefreshIdle-base.fieldWorkRefreshIdle,moving=now.fieldWorkRefreshMoving-base.fieldWorkRefreshMoving;
    std::printf("staticidle parked holding=%d refresh_idle=%llu refresh_moving=%llu refresh_completed=%llu refresh_suppressed=%llu groups=%zu\n",
        held,(unsigned long long)idle,(unsigned long long)moving,(unsigned long long)(now.refreshCompleted-base.refreshCompleted),
        (unsigned long long)now.refreshSuppressed,f.world.legionNavigator()->stats().liveGroups);
    check(idle+moving==0,"staticidle: parked groups were refreshed beside continuous construction (B1 removes this work)");
}

// Landed flyers stand on the ground grid (retail stamps a mode-1 flyer into
// word +0, so mobilePlacement refuses a ground step into one). A group sent
// across a field of them -- a block of 30 and 12 scattered -- must plan and
// steer round them like any standing body: no overlap, no pushing into
// them, every member arrives. layout: 0 block+scattered, 1 none (open
// reference), 2 the block takes off soon after the order (its cells must
// clear at once for ground steering). Retail runs the same layout for
// reference (measured, not asserted).
// An unfinished ground body (a conjure in progress, a decaying site) stands in
// retail's ground grid, so mobilePlacement refuses a step into it, but World::occ_
// leaves it out. Legion once saw free ground there: a body whose way ran through
// the site held against it with no blocker to detour round or ask, and walked on
// only once the site finished (a Beast Handler's repeat-conjured Hunters, each
// waiting out the next one's conjure). The mover must walk round the site, which
// stays unfinished and unmoved (nothing may part or yield it).
void unfinishedsite() {
    Fixture f(48,24);
    f.publish();
    const UnitType t=mover(2);
    const int site=f.spawn(t,12,12),body=f.spawn(t,6,12);
    f.world.unit(site)->underConstruction=true;
    f.start();
    const Fixed sx=f.world.unit(site)->x,sz=f.world.unit(site)->z;
    tak::net::Command c;c.kind=tak::net::Cmd::Move;c.player=0;c.unitId=body;c.x=36*16.f;c.z=12*16.f;
    TypeRegistry reg;applyCommand(f.world,reg,c);
    int arrived=-1;
    // The straight walk takes ~267 ticks; an unattended site decays well after 300.
    for(int k=0;k<300&&arrived<0;++k) {
        f.world.tick(1.f/30);
        check(f.legal(body),"unfinishedsite: the mover stands on the site");
        if(f.world.unit(body)->orders.empty())arrived=k;
    }
    const Unit* s=f.world.unit(site);
    std::printf("unfinishedsite arrived=%d\n",arrived);
    check(s&&s->alive()&&s->underConstruction&&s->x==sx&&s->z==sz,"unfinishedsite: the site was moved, finished or lost");
    check(arrived>=0,"unfinishedsite: the mover waited at the unfinished site");
}

struct FlyerRun {uint64_t hash=0;int arrived=0,ticks=0,half=-1,most=-1;uint64_t holds=0,pushes=0,stuck=0,overlap=0,spins=0,reversals=0;};
FlyerRun landedflyersRun(int layout,bool legion,int flyerFoot,bool serial=true) {
    Fixture f(200,100,serial);f.publish();
    if(!legion)f.world.setPathfindingMode(PathfindingMode::Retail);
    const auto type=mover(2);
    UnitType flyer{};flyer.id=flyer.name="legion-flyer";
    flyer.canFly=flyer.canMove=true;flyer.maxHp=100;flyer.footX=flyer.footZ=flyerFoot;flyer.sight=4096;
    flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);flyer.turnRate=1200;flyer.cruiseAlt=80;flyer.buildTime=1;
    // FLYER_AS_GROUND: the same layout of idle ground bodies (reference).
    const UnitType& standing=std::getenv("FLYER_AS_GROUND")&&flyerFoot==2?type:flyer;
    std::vector<int> flyers,block,ids;
    if(layout!=1) {
        const int pitch=flyerFoot+1;
        for(int c=0;c<5;++c)for(int r=0;r<6;++r)block.push_back(f.spawn(standing,90+c*pitch,38+r*pitch,c%2));
        for(int i=0;i<12;++i)flyers.push_back(f.spawn(standing,60+(i%4)*10+(i/4)*3,30+(i%4)*7+(i/4)*9,1));
        flyers.insert(flyers.end(),block.begin(),block.end());
    }
    for(int i=0;i<40;++i)ids.push_back(f.spawn(type,20+(i%8)*3,44+(i/8)*3));
    f.start();
    for(int t=0;t<90;++t)f.world.tick(1.f/30);
    for(int id:flyers)check(f.world.unit(id)->flightGroundMode==1,"flyer not landed");
    const bool asGround=&standing==&type;
    for(int id:ids)f.world.order(id,160*16,50*16,false);
    Watch motion;FlyerRun r;
    {
        // Ground group, plus the standing bodies as a second group: the
        // observer counts a landed flyer standing on a ground body
        // (illegal_overlap_ticks) in the group that holds the flyer.
        tak::legion_observe::Group gg,gf;gg.name="ground";gg.ids=ids;gf.name="flyers";gf.ids=flyers;
        motion.cfg.groups.push_back(gg);
        if(!flyers.empty())motion.cfg.groups.push_back(gf);
    }
    for(;r.ticks<5000&&r.arrived<int(ids.size());++r.ticks) {
        if(layout==2&&r.ticks==150)for(int id:block)f.world.order(id,190*16,95*16,false);
        f.world.tick(1.f/30);motion.observe(f.world,ids);
        r.arrived=0;
        for(int id:ids) {
            const auto& u=*f.world.unit(id);
            r.arrived+=u.orders.empty();
            const int ux=footprintOrigin(u.x,2),uz=footprintOrigin(u.z,2);
            bool touching=false;
            for(int fid:flyers) {
                const auto& v=*f.world.unit(fid);
                if(v.flightGroundMode!=1||(asGround&&!v.orders.empty()))continue;
                const int vx=footprintOrigin(v.x,flyerFoot),vz=footprintOrigin(v.z,flyerFoot);
                if(ux<=vx+flyerFoot&&vx<=ux+2&&uz<=vz+flyerFoot&&vz<=uz+2)touching=true;
            }
            // Pushing: an ordered body steered as moving, next to a landed
            // flyer, yet without speed. Stuck: any ordered body standing
            // still against one (holding there counts).
            if(legion&&touching&&!u.orders.empty()&&f.world.legionNavigator()->unitState(id)==1&&u.speed==Fixed())++r.pushes;
            if(touching&&!u.orders.empty()&&u.speed==Fixed())++r.stuck;
        }
        if(r.half<0&&2*r.arrived>=int(ids.size()))r.half=r.ticks;
        if(r.most<0&&10*r.arrived>=9*int(ids.size()))r.most=r.ticks;
    }
    for(int id:ids)check(f.legal(id),"illegal footprint");
    r.hash=f.world.stateHash();
    r.holds=legion?f.world.legionStats().holds:0;r.spins=motion.spins();r.reversals=motion.reversals();
    r.overlap=uint64_t(motion.key("g.flyers.illegal_overlap_ticks"));
    std::printf("landedflyers %s layout=%d foot=%d arrived=%d/%zu ticks=%d half=%d p90=%d holds=%llu pushes=%llu stuck=%llu overlap=%llu spins=%llu reversals=%llu\n",
        legion?"legion":"retail",layout,flyerFoot,r.arrived,ids.size(),r.ticks,r.half,r.most,(unsigned long long)r.holds,(unsigned long long)r.pushes,(unsigned long long)r.stuck,
        (unsigned long long)r.overlap,(unsigned long long)r.spins,(unsigned long long)r.reversals);
    if(std::getenv("STATIC_VERBOSE"))printLeft(f,ids);
    return r;
}
// Bounds are the median-of-5 start offsets of the committed build (W0
// baseline, one-sided). Offsets 0 / +1 / -1 / +2 / -2 (legion):
//   open (layout 1)  ticks 1803 / 1724 / 1897 / 1880 / 1902
//   block, foot 2    ticks 2566 / 2777 / 2827 / 2575 / 2799, stuck 1039 / 662 / 1520 / 1244 / 1176
//   block, foot 3    ticks 2188 / 2402 / 2564 / 2832 / 2341, stuck 187 / 81 / 290 / 582 / 159
//   takes off (2)    ticks 2476 / 2414 / never (39/40 at -1) / 2369 / 2389, half 1345 / 1458 / 1490 / 1272 / 1438
//   pushes 0 everywhere. Every spread is over 1.10, so every band is the full 1.20.
// The old bounds (pushes <= 40, stuck <= 6000, ticks <= 2x the open run, half <=
// 1.25x the open run) were measured at offset 0 and sat 1.4x-5.8x from it.
struct FlyerBase {int ticks;uint64_t stuck,pushes;double band;};
constexpr int kLandedOpenTicks=1880;           // layout 1, none standing: the way round
// 2026-10-19 friendly departure: a long local detour keeps travel speed, so the stream reaches the parked
// flyers sooner, before they have lifted, and stands against them longer while finishing faster (medians,
// foot 2 / 3: ticks 2700 -> 2567 / 2432 -> 2332, stuck 996 -> 1568 / 171 -> 250). Ticks are the new medians;
// stuck bases with the 1.20 band cover the new medians by about 7% / 10%.
// 2026-10-19 protocol 280 (flow-around shuffles at travel speed): the same again for the flow-around steps round
// the stream's own bodies (medians, foot 2 / 3: ticks 2567 -> 2207 / 2332 -> 2162, stuck 1568 -> 1760 / 250 -> 218).
// The foot-2 stuck base moves 1400 -> 1570 (the band covers the new median by about 7%); the rest is unchanged.
// 2026-11-02 (no facing cap on unturned steps, no flow-around behind the aim, the press): the stream threads the
// scattered flyers faster and stands less in all, but its queue heads wait at the cells next to a flyer. Every
// "stuck" tick is a hold for movers of its own order (steerWhy: hold_movers; a merge head waiting for a file
// passing at travel speed; under the old rules the same heads waited as long at cells away from the flyers). Medians over the
// 5 shifts, foot 2 / 3: ticks 2207 -> 2162 / 2162 -> 1837, stuck 1760 -> 2088 / 218 -> 731 (1819/1937/2290/2088/
// 1607 and 731/966/543/948/551), while ordered bodies standing anywhere fall 9820 -> 6604 / 5027 -> 2795. The
// three rules each put the waits beside the flyers (the press alone 646, the backward rule alone 539, both off 201
// at foot 3) and no variant of the backward rule keeps the suite's gains (backward options ranked last: foot-3
// stuck 501 and over the 64 fixtures reversals 7 better / 32 worse). Stuck bases 1570 -> 1950 and 230 -> 680 (the
// band covers the new medians by about 12% / 12%); ticks unchanged.
constexpr FlyerBase kLandedBlock[2]={{2567,1950,0,1.20},{2332,680,0,1.20}};      // layout 0, flyer foot 2 and 3
constexpr FlyerBase kLandedOff={2414,0,0,1.20};          // layout 2, the block takes off
constexpr int kLandedOffHalf=1438;
void landedflyers() {
    const bool report=std::getenv("STATIC_REPORT")!=nullptr;
    if(std::getenv("FLYER_RETAIL")) {landedflyersRun(0,false,2);landedflyersRun(1,false,2);}
    FlyerRun open[5],block[2][5],off[5];
    for(int i=0;i<5;++i) {
        g_shift=kShifts[i];
        open[i]=landedflyersRun(1,true,2);
        block[0][i]=landedflyersRun(0,true,2);block[1][i]=landedflyersRun(0,true,3);
        off[i]=landedflyersRun(2,true,2);
    }
    g_shift=0;
    if(report)return;
    // Before the fix (flyers missing from Legion's view of the ground):
    // 23/40 and 4/40 arrived in 5000 ticks, 64164 and 99588 member-ticks
    // standing against a flyer. After: as for the same layout of idle
    // ground bodies (FLYER_AS_GROUND), 2631 ticks, 2536 of those ticks
    // (the stream filing along the block's face).
    // Before: 31/40 in 5000 ticks (the scattered flyers held the rest).
    auto med=[](const FlyerRun (&r)[5],auto field) {
        std::vector<int64_t> v;for(const auto& x:r)v.push_back(int64_t(field(x)));
        std::sort(v.begin(),v.end());return v[2];
    };
    for(int i=0;i<5;++i) {
        for(int k=0;k<2;++k) {
            const auto& r=block[k][i];
            check(r.overlap==0,"ground body overlapped a landed flyer");
            check(r.spins==0,"group spun at the landed flyers");
        }
        check(off[i].overlap==0&&off[i].spins==0,"group failed after the flyers took off");
    }
    // Offset 0 is the original fixture: everyone arrives. (At offset -1 one
    // member is still on the way when the block has taken off: AR-01, the tail.)
    for(int k=0;k<2;++k)check(block[k][0].arrived==40,"group did not get round the landed flyers");
    check(off[0].arrived==40,"group failed after the flyers took off");
    check(landedflyersRun(2,true,2,false).hash==off[0].hash,"workers run differs from the serial run");
    check(med(open,[](const FlyerRun& r) {return r.ticks;})<=kLandedOpenTicks*1.20,"group took longer than the way round");
    for(int k=0;k<2;++k) {
        const auto& b=kLandedBlock[k];
        check(med(block[k],[](const FlyerRun& r) {return r.pushes;})<=b.pushes*b.band,"group pushed into the landed flyers");
        check(med(block[k],[](const FlyerRun& r) {return r.stuck;})<=b.stuck*b.band,"group stood against the landed flyers");
        check(med(block[k],[](const FlyerRun& r) {return r.ticks;})<=b.ticks*b.band,"group took far longer than the way round");
    }
    check(med(off,[](const FlyerRun& r) {return r.ticks;})<=kLandedOff.ticks*kLandedOff.band,"group did not use the ground the flyers left");
    check(med(off,[](const FlyerRun& r) {return r.half;})<=kLandedOffHalf*kLandedOff.band,"group did not use the ground the flyers left");
}

// Idle landed flyers make way for an allied ground group (World::
// requestLegionLift): 12 landed flyers stand on the straight way of 40
// ground bodies. mode 0: own flyers -- they lift, the group walks under
// them as on open ground, and they land again on the spots they left;
// 1: an enemy's -- they stay landed obstacles; 2: own flyers on guard
// orders (a guard within reach stays landed) -- they do not lift; 3: own
// flyers ON the group's destination -- they lift, the group settles there,
// and they land nearby once, without bobbing; 4: no flyers (reference).
struct LiftRun {uint64_t hash=0;int arrived=0,ticks=0,half=-1,lifted=0,takeoffs=0,maxTakeoffs=0,landed=0,home=0;
                uint64_t overlap=0,flyerOverlap=0,spins=0,detour=0,lifts=0;};
LiftRun liftflyersRun(int mode,bool serial=true) {
    Fixture f(200,100,serial);f.publish();
    const auto type=mover(2);
    UnitType flyer{};flyer.id=flyer.name="legion-flyer";
    flyer.canFly=flyer.canMove=true;flyer.maxHp=100;flyer.footX=flyer.footZ=2;flyer.sight=4096;
    flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);flyer.turnRate=1200;flyer.cruiseAlt=80;flyer.buildTime=1;
    std::vector<int> flyers,ids;
    const int owner=mode==1?1:0;
    if(mode!=4)for(int i=0;i<12;++i)flyers.push_back(f.spawn(flyer,mode==3?150+(i%4)*4:70+(i%4)*3,mode==3?44+(i/4)*4:46+(i/4)*3,owner));
    // Guards: a post inside the flyers' reach (70 px) keeps them landed.
    const int post=mode==2?f.spawn(type,64,40,0):0;
    for(int i=0;i<40;++i)ids.push_back(f.spawn(type,20+(i%8)*3,44+(i/8)*3));
    f.start();
    for(int t=0;t<90;++t)f.world.tick(1.f/30);
    for(int id:flyers)check(f.world.unit(id)->flightGroundMode==1,"flyer not landed");
    if(mode==2) {
        for(int id:flyers)f.world.guard(id,post,false);
        for(int t=0;t<30;++t)f.world.tick(1.f/30);
    }
    std::vector<std::pair<int,int>> spots;
    for(int id:flyers) {const auto& v=*f.world.unit(id);spots.push_back({footprintOrigin(v.x,2),footprintOrigin(v.z,2)});}
    std::vector<uint8_t> mode0(flyers.size(),1);std::vector<int> takeoffs(flyers.size(),0);
    for(int id:ids)f.world.order(id,160*16,50*16,false);
    Watch motion;LiftRun r;
    std::vector<int32_t> startZ;for(int id:ids)startZ.push_back(f.world.unit(id)->z.v);
    int done=-1;
    for(;r.ticks<6000;++r.ticks) {
        f.world.tick(1.f/30);motion.observe(f.world,ids);
        r.arrived=0;
        for(size_t k=0;k<ids.size();++k) {
            const auto& u=*f.world.unit(ids[k]);
            r.arrived+=u.orders.empty();
            // Detour: how far a body strays across the straight way (cells).
            r.detour=std::max<uint64_t>(r.detour,uint64_t(std::abs(int64_t(u.z.v)-startZ[k])>>20));
            const int ux=footprintOrigin(u.x,2),uz=footprintOrigin(u.z,2);
            for(int fid:flyers) {
                const auto& v=*f.world.unit(fid);
                if(v.flightGroundMode!=1)continue;
                const int vx=footprintOrigin(v.x,2),vz=footprintOrigin(v.z,2);
                if(ux<vx+2&&vx<ux+2&&uz<vz+2&&vz<uz+2)++r.overlap;
            }
        }
        for(size_t a=0;a<flyers.size();++a) {
            const auto& v=*f.world.unit(flyers[a]);
            if(mode0[a]==1&&v.flightGroundMode==2)++takeoffs[a];
            if(std::getenv("LIFT_TRACE")&&mode0[a]!=v.flightGroundMode)std::printf("  t=%d flyer %d mode %d->%d at %d,%d lift=%d arrived=%d\n",r.ticks,flyers[a],mode0[a],v.flightGroundMode,footprintOrigin(v.x,2),footprintOrigin(v.z,2),int(v.legionLift),r.arrived);
            mode0[a]=v.flightGroundMode;
            if(v.flightGroundMode!=1)continue;
            for(size_t b=a+1;b<flyers.size();++b) {
                const auto& o=*f.world.unit(flyers[b]);
                if(o.flightGroundMode!=1)continue;
                const int ax=footprintOrigin(v.x,2),az=footprintOrigin(v.z,2),bx=footprintOrigin(o.x,2),bz=footprintOrigin(o.z,2);
                if(ax<bx+2&&bx<ax+2&&az<bz+2&&bz<az+2)++r.flyerOverlap;
            }
        }
        if(r.half<0&&2*r.arrived>=int(ids.size()))r.half=r.ticks;
        if(r.arrived==int(ids.size())&&done<0)done=r.ticks;
        // After the group is in, let lifted flyers land again (quiet time,
        // then a descent), then stop.
        if(done>=0&&r.ticks>=done+600)break;
    }
    if(done>=0)r.ticks=done;
    for(int id:ids)check(f.legal(id),"illegal footprint");
    for(size_t a=0;a<flyers.size();++a) {
        const auto& v=*f.world.unit(flyers[a]);
        r.lifted+=takeoffs[a]>0;r.takeoffs+=takeoffs[a];r.maxTakeoffs=std::max(r.maxTakeoffs,takeoffs[a]);
        r.landed+=v.flightGroundMode==1;
        r.home+=v.flightGroundMode==1&&footprintOrigin(v.x,2)==spots[a].first&&footprintOrigin(v.z,2)==spots[a].second;
    }
    r.hash=f.world.stateHash();r.spins=motion.spins();r.lifts=f.world.legionStats().lifts;
    std::printf("liftflyers mode=%d arrived=%d/%zu ticks=%d half=%d detour=%llu lifted=%d takeoffs=%d max=%d landed=%d home=%d/%zu overlap=%llu flyer_overlap=%llu spins=%llu lifts=%llu\n",
        mode,r.arrived,ids.size(),r.ticks,r.half,(unsigned long long)r.detour,r.lifted,r.takeoffs,r.maxTakeoffs,r.landed,r.home,flyers.size(),
        (unsigned long long)r.overlap,(unsigned long long)r.flyerOverlap,(unsigned long long)r.spins,(unsigned long long)f.world.legionStats().lifts);
    return r;
}
void liftflyers() {
    const bool report=std::getenv("STATIC_REPORT")!=nullptr;
    const auto open=liftflyersRun(4);
    const auto own=liftflyersRun(0);
    const auto enemy=liftflyersRun(1);
    const auto guards=liftflyersRun(2);
    const auto onGoal=liftflyersRun(3);
    if(report)return;
    // Own flyers: all lift, the group walks through as on open ground, and
    // every flyer lands again on its own spot, once.
    check(own.arrived==40&&own.overlap==0&&own.flyerOverlap==0&&own.spins==0,"group failed under lifting flyers");
    check(own.lifted==12&&own.maxTakeoffs==1,"own idle flyers did not lift exactly once");
    check(own.landed==12&&own.home==12,"lifted flyers did not land again on their spots");
    // The detour allows open + 3 cells, not + 1, by user decision W3-4 (2026-10-09): under W3 the last body
    // re-chooses its slot along the formation's west face AT the destination (own run 5 vs open 2), a
    // steering gap round a settled crowd accepted for W3. Moved to W9's exit by user decision W5-1
    // (2026-10-10): HARD EXIT GATE at W9, back to open + 1.
    constexpr uint64_t kLiftDetourSlack=3;   // W3-4; W9 restores 1 (W5-1)
    check(own.ticks<=open.ticks*11/10&&own.half<=open.half*11/10,"group slowed by flyers that lift");
    check(own.detour<=open.detour+kLiftDetourSlack,"group detoured round flyers that lift");
    // An enemy's flyers never lift: obstacles, planned and steered round.
    check(enemy.lifted==0&&enemy.landed==12,"enemy flyers lifted");
    check(enemy.arrived==40&&enemy.overlap==0&&enemy.spins==0,"group failed round enemy flyers");
    check(enemy.detour>open.detour,"group did not go round the enemy flyers");
    // Flyers with orders never lift (guards go their own way; Legion never
    // asks one of them).
    check(guards.lifts==0,"flyers with orders were lifted");
    check(guards.arrived==40&&guards.overlap==0,"group failed round guarding flyers");
    // Flyers on the destination: lift once, land nearby, no bobbing.
    check(onGoal.arrived==40&&onGoal.overlap==0&&onGoal.flyerOverlap==0,"group failed on the flyers' spots");
    check(onGoal.lifted>0&&onGoal.maxTakeoffs<=1,"flyers on the destination bobbed");
    check(onGoal.landed==12,"flyers on the destination did not land again");
    check(liftflyersRun(0,false).hash==own.hash,"workers run differs from the serial run");
}

void replace() {
    Fixture f(128,96);
    f.rect(60,0,4,40);f.rect(60,48,4,48);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<30;++i)ids.push_back(f.spawn(type,10+(i%6)*3,30+(i/6)*3));
    f.start();
    uint32_t r=99;
    for(int round=0;round<25;++round) {
        for(int id:ids) {
            r=r*1664525u+1013904223u;
            f.world.order(id,float((8+int(r>>8)%110)*16),float((8+int(r>>20)%80)*16),round%3==2);
        }
        for(int t=0;t<7;++t)f.world.tick(1.f/30);
    }
    for(size_t i=0;i<ids.size();++i)f.world.order(ids[i],float((96+int(i%6)*4)*16),float((30+int(i/6)*4)*16),false);
    for(int t=0;t<4000;++t)f.world.tick(1.f/30);
    int arrived=0;for(int id:ids)arrived+=f.world.unit(id)->orders.empty();
    const auto s=f.world.legionStats();
    std::printf("replace arrived=%d groups=%llu bytes=%zu\n",arrived,(unsigned long long)s.groups,s.bytes);
    check(arrived==int(ids.size()),"units lost after rapid replacement");
    check(s.bytes<4u*1024*1024,"stale replacement state retained");
}

void unreachable() {
    Fixture f(96,64);
    f.rect(48,0,3,64);   // full wall
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<16;++i)ids.push_back(f.spawn(type,10+(i%4)*3,20+(i/4)*3));
    f.start();
    for(int id:ids)f.world.order(id,80*16,30*16,false);
    Watch motion;
    for(int t=0;t<9100;++t) {f.world.tick(1.f/30);motion.observe(f.world,ids);}
    int cleared=0;for(int id:ids)cleared+=f.world.unit(id)->orders.empty();
    std::printf("unreachable cleared=%d spins=%llu\n",cleared,(unsigned long long)motion.spins());
    check(cleared==int(ids.size()),"unreachable orders not retired");
    check(motion.spins()==0,"unreachable units spun");
}

void slotblock() {
    // A shared-point order whose arrival slots are covered mid-approach (a
    // wall stands in for a corpse or new building): no member may claim a
    // covered slot, so every order completes or retires -- no livelock of
    // claim, re-register, claim -- and nobody spins.
    Fixture f(96,64);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<16;++i)ids.push_back(f.spawn(type,8+(i%4)*3,20+(i/4)*3));
    f.start();
    for(int id:ids)f.world.order(id,60*16,32*16,false);
    Watch motion;
    int done=-1;
    for(int t=0;t<4000;++t) {
        if(t==150) {f.rect(61,30,2,5);f.rect(57,35,4,1);f.publish();}
        f.world.tick(1.f/30);motion.observe(f.world,ids);
        int left=0;for(int id:ids)left+=!f.world.unit(id)->orders.empty();
        if(!left&&done<0)done=t;
    }
    int cleared=0;for(int id:ids)cleared+=f.world.unit(id)->orders.empty();
    std::printf("slotblock cleared=%d at=%d spins=%llu reversals=%llu\n",cleared,done,
        (unsigned long long)motion.spins(),(unsigned long long)motion.reversals());
    printLeft(f,ids);
    check(cleared==int(ids.size()),"shared-point orders livelocked on covered slots");
    for(int id:ids)check(f.legal(id),"illegal footprint after slot cover");
    check(motion.spins()==0,"units spun around covered slots");
}

void quota() {
    // Many independent single-unit orders on a large map exceed the field
    // cap: evicted fields rebuild deterministically and nobody starves.
    // Since Legion 10,000 stage 4 the cap is a fixed byte allowance (128 MiB,
    // far above 64 fields of this map); the fixture sets it to what the old
    // cap of 48 whole maps of cells was here, so it still exercises eviction,
    // now through the byte allowance.
    LegionNavigator::setFieldBytesForTest(48ull*320*320*4);
    struct Reset {~Reset() {LegionNavigator::setFieldBytesForTest(0);}} reset;
    Fixture f(320,320);
    f.rect(150,0,4,150);f.rect(150,170,4,150);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<64;++i)ids.push_back(f.spawn(type,10+(i%8)*4,10+(i/8)*4));
    f.start();
    for(size_t i=0;i<ids.size();++i) {
        f.world.order(ids[i],float((200+int(i%8)*12)*16),float((40+int(i/8)*30)*16),false);
        f.world.tick(1.f/30);   // a separate issue tick: separate groups
    }
    for(int t=0;t<9000;++t)f.world.tick(1.f/30);
    int arrived=0;for(int id:ids)arrived+=f.world.unit(id)->orders.empty();
    const auto s=f.world.legionStats();
    std::printf("quota arrived=%d fields=%llu evictions=%llu bytes=%llu\n",arrived,(unsigned long long)s.fieldsBuilt,
        (unsigned long long)s.fieldEvictions,(unsigned long long)s.bytes);
    check(arrived==int(ids.size()),"orders starved under the field cap");
    check(s.fieldEvictions>0,"fixture did not exercise eviction");
}

void pens() {
    // More independent orders than the field budget holds whole-map fields,
    // each penned in its own small walled region behind a wall stub: fields
    // are sized to their region, so nobody is evicted mid-route or starved.
    constexpr int P=24,N=12;   // pen size (cells), pens per side
    Fixture f(P*N,P*N);
    std::vector<int> ids;std::vector<std::pair<int,int>> goals;
    const auto type=mover(2);
    for(int j=0;j<N;++j)for(int i=0;i<N;++i) {
        const int x=i*P,z=j*P;
        f.rect(x,z,P,1);f.rect(x,z+P-1,P,1);f.rect(x,z,1,P);f.rect(x+P-1,z,1,P);
        f.rect(x+P/2,z+4,2,P-8);
    }
    f.publish();
    for(int j=0;j<N;++j)for(int i=0;i<N;++i) {ids.push_back(f.spawn(type,i*P+5,j*P+P/2));goals.push_back({i*P+P-5,j*P+P/2});}
    f.start();
    for(size_t i=0;i<ids.size();++i) {
        f.world.order(ids[i],float(goals[i].first*16),float(goals[i].second*16),false);
        f.world.tick(1.f/30);   // a separate issue tick: separate groups
    }
    int arrived=0,all=-1;
    for(int t=0;t<3000&&all<0;++t) {
        f.world.tick(1.f/30);
        arrived=0;for(int id:ids)arrived+=f.world.unit(id)->orders.empty();
        if(arrived==int(ids.size()))all=t;
    }
    const auto s=f.world.legionStats();
    std::printf("pens all_arrived_tick=%d arrived=%d/%zu fields=%llu evictions=%llu bytes=%llu\n",all,arrived,ids.size(),(unsigned long long)s.fieldsBuilt,
        (unsigned long long)s.fieldEvictions,(unsigned long long)s.bytes);
    check(ids.size()>48,"fixture does not exceed the whole-map field cap");
    check(arrived==int(ids.size()),"penned orders starved under the field budget");
    check(s.fieldEvictions==0,"small-region fields were evicted");
    // Every group gets its field at once (242 ticks); waiting for whole-map
    // field slots in waves took 609.
    check(all>=0&&all<400,"penned orders waited for field slots");
    for(int id:ids)check(f.legal(id),"illegal footprint in pens");
}

void formation() {
    // Open ground, a formation sent far (beyond the direct-line reach) to the
    // same lattice translated: every body should travel its own row, so the
    // army keeps its shape and nobody crosses into a neighbour's lane.
    Fixture f(480,128);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids,rows;
    for(int i=0;i<200;++i) {ids.push_back(f.spawn(type,32+(i/15)*3,36+(i%15)*3));rows.push_back(36+(i%15)*3);}
    f.start();
    for(size_t i=0;i<ids.size();++i)f.world.order(ids[i],float((420+int(i/15)*3)*16),float(rows[i]*16),false);
    float drift=0;
    for(int t=0;t<9000;++t) {
        f.world.tick(1.f/30);
        for(size_t i=0;i<ids.size();++i)drift=std::max(drift,std::abs(f.world.unit(ids[i])->z.toFloat()/16-float(rows[i])));
    }
    int arrived=0;for(int id:ids)arrived+=f.world.unit(id)->orders.empty();
    printLeft(f,ids);
    std::printf("formation arrived=%d/%zu max_row_drift_cells=%.2f\n",arrived,ids.size(),drift);
    check(arrived==int(ids.size()),"formation did not arrive on open ground");
    check(drift<=1.0f,"formation members left their lanes on open ground");
}

uint64_t scenario(bool serial) {
    Fixture f(128,64,serial);
    f.rect(60,0,4,28);f.rect(60,34,4,30);
    f.publish();
    std::vector<UnitType> types{mover(2),mover(3)};
    std::vector<int> ids;
    for(int i=0;i<60;++i)ids.push_back(f.spawn(types[size_t(i%2)],8+(i%10)*4,8+(i/10)*6,i%2));
    f.start();
    for(size_t i=0;i<ids.size();++i)f.world.order(ids[i],float((100-int(i%10)*3)*16),float((12+int(i/10)*6)*16),false);
    for(int t=0;t<1500;++t)f.world.tick(1.f/30);
    return f.world.stateHash();
}
// Units killed mid-move (hp to zero) or stripped of their orders without a
// cancel leave Legion: groups, members and point claims shrink to nothing, and the
// survivors still arrive.
void deathsshared() {
    // Half of a shared-point group dies mid-approach (corpses can land on
    // arrival slots): every survivor must still settle, with no spinning,
    // and Legion's containers must empty.
    Fixture f(160,64);
    f.rect(70,0,4,28);f.rect(70,36,4,28);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<40;++i)ids.push_back(f.spawn(type,10+(i%8)*3,14+(i/8)*3));
    f.start();
    for(int id:ids)f.world.order(id,126.f*16,32.f*16,false);
    for(int t=0;t<60;++t)f.world.tick(1.f/30);
    std::vector<int> live;
    for(size_t i=0;i<ids.size();++i) {if(i%2)f.world.unit(ids[i])->hp=Fixed();else live.push_back(ids[i]);}
    Watch motion;
    for(int t=0;t<6000;++t) {f.world.tick(1.f/30);motion.observe(f.world,live);}
    int arrived=0;for(int id:live)arrived+=f.world.unit(id)->orders.empty();
    const auto e=f.world.legionStats();
    std::printf("deathsshared arrived=%d/%zu members=%zu groups=%zu spins=%llu\n",arrived,live.size(),
        e.liveMembers,e.liveGroups,(unsigned long long)motion.spins());
    printLeft(f,live);
    check(arrived==int(live.size()),"shared-point survivors did not arrive");
    for(int id:live)check(f.legal(id),"illegal survivor footprint");
    check(motion.spins()==0,"shared-point survivors spun");
    check(e.liveMembers==0&&e.liveGroups==0,"Legion containers did not empty");
}

void deaths() {
    Fixture f(160,64);
    f.rect(70,0,4,28);f.rect(70,36,4,28);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<40;++i)ids.push_back(f.spawn(type,10+(i%8)*3,14+(i/8)*3));
    f.start();
    // Distinct goals: shared-point slots under fresh corpses are a separate
    // concern (arrival slots), not membership.
    for(size_t i=0;i<ids.size();++i)f.world.order(ids[i],float((110+(int(i)%8)*4)*16),float((12+int(i)/8*4)*16),false);
    for(int t=0;t<60;++t)f.world.tick(1.f/30);
    std::vector<int> live;
    for(size_t i=0;i<ids.size();++i) {
        if(i%2) {f.world.unit(ids[i])->hp=Fixed();} else live.push_back(ids[i]);
    }
    // Orders cleared without a cancel (a path that forgets cancelPath): the
    // member is stale and must be pruned too.
    for(size_t i=0;i<ids.size();i+=4)f.world.unit(ids[i])->orders.clear();
    for(int t=0;t<120;++t)f.world.tick(1.f/30);
    for(size_t i=1;i<ids.size();i+=2) {
        const auto* u=f.world.unit(ids[i]);
        check(!u||!u->alive(),"hp=0 did not kill the unit");
    }
    auto mid=f.world.legionStats();
    std::printf("deaths mid members=%zu groups=%zu points=%zu\n",mid.liveMembers,mid.liveGroups,mid.livePoints);
    size_t moving=0;for(int id:live)moving+=!f.world.unit(id)->orders.empty();
    check(mid.liveMembers<=moving,"dead or orderless units are still Legion members");
    for(int t=0;t<6000;++t)f.world.tick(1.f/30);
    int arrived=0;for(int id:live)arrived+=f.world.unit(id)->orders.empty();
    const auto e=f.world.legionStats();
    std::printf("deaths arrived=%d/%zu members=%zu groups=%zu points=%zu fields=%zu\n",arrived,live.size(),
        e.liveMembers,e.liveGroups,e.livePoints,e.liveFields);
    printLeft(f,live);
    check(arrived==int(live.size()),"survivors did not arrive");
    check(e.liveMembers==0&&e.liveGroups==0&&e.livePoints==0&&e.liveFields==0,"Legion containers did not empty");
}

// One command sends two units to goals within the cluster radius, one inside
// a sealed pocket. They must not share a field: the cut-off member is trapped
// and retires; it never descends to its teammate's seed and holds forever.
void splitgoal() {
    Fixture f(64,64);
    f.rect(30,10,10,1);f.rect(30,19,10,1);f.rect(30,10,1,10);f.rect(39,10,1,10);
    f.publish();
    const auto type=mover(2);
    const int a=f.spawn(type,10,14),b=f.spawn(type,10,20);
    f.start();
    f.world.order(a,34*16,14*16,false);   // inside the pocket
    f.world.order(b,44*16,14*16,false);   // outside, 10 cells away
    int cleared=-1;
    for(int t=0;t<9200;++t) {
        f.world.tick(1.f/30);
        if(cleared<0&&f.world.unit(a)->orders.empty())cleared=t;
    }
    std::printf("splitgoal cut-off cleared_at=%d other_arrived=%d\n",cleared,int(f.world.unit(b)->orders.empty()));
    check(f.world.unit(b)->orders.empty(),"reachable member did not arrive");
    check(cleared>=0,"cut-off member never retired");
}

// A click deep inside a blocked mass (beyond the near goal search) walks to
// the nearest reachable legal point, as Retail does, instead of trapping.
void farclick() {
    Fixture f(128,64);
    f.rect(60,0,68,64);
    f.publish();
    const auto type=mover(2);const int id=f.spawn(type,10,30);
    f.start();
    f.world.order(id,110*16,30*16,false);
    int cleared=-1;
    for(int t=0;t<3000&&cleared<0;++t) {f.world.tick(1.f/30);if(f.world.unit(id)->orders.empty())cleared=t;}
    const auto& u=*f.world.unit(id);
    std::printf("farclick cleared_at=%d x=%.1f trapped=%llu\n",cleared,u.x.toFloat()/16,(unsigned long long)f.world.legionStats().trapped);
    check(cleared>=0&&u.x.toFloat()/16>50,"far click did not walk to the nearest reachable point");
}

// A goal behind a full wall, from a region with room to move: the body walks
// to the region's nearest point to the goal and stops there -- zero speed,
// constant heading, no pushing against the wall -- and its order drops on
// arrival (kApproachRetire = 0; user decision 2026-10-08, PLAN W2 AR-11).
void approachhold() {
    Fixture f(96,64);
    f.rect(48,0,3,64);
    f.publish();
    const auto type=mover(2);const int id=f.spawn(type,10,30);
    f.start();
    f.world.order(id,80*16,30*16,false);
    Watch motion;
    int stillFrom=-1,cleared=-1;int32_t x=f.world.unit(id)->x.v,z=f.world.unit(id)->z.v,heading=f.world.unit(id)->heading.v;
    uint64_t movedAfter=0,turnedAfter=0;
    for(int t=0;t<3000;++t) {
        f.world.tick(1.f/30);motion.observe(f.world,{id});
        const auto& u=*f.world.unit(id);
        if(stillFrom<0&&f.world.legionNavigator()->unitState(id)==5)stillFrom=t;
        if(cleared<0&&u.orders.empty())cleared=t;
        if(stillFrom>=0&&t>stillFrom) {movedAfter+=u.x.v!=x||u.z.v!=z;turnedAfter+=u.heading.v!=heading;}
        x=u.x.v;z=u.z.v;heading=u.heading.v;
    }
    const auto& u=*f.world.unit(id);
    std::printf("approachhold held_at=%d cleared_at=%d x=%.1f z=%.1f moved_after=%llu turned_after=%llu spins=%llu speed=%d\n",stillFrom,cleared,
        u.x.toFloat()/16,u.z.toFloat()/16,(unsigned long long)movedAfter,(unsigned long long)turnedAfter,
        (unsigned long long)motion.spins(),u.speed.v);
    check(stillFrom>=0,"approaching unit never held");
    check(u.x.toFloat()/16>44&&u.x.toFloat()/16<48.5f&&std::abs(u.z.toFloat()/16-30)<2,"unit did not walk to the nearest reachable point");
    check(movedAfter==0&&turnedAfter==0&&motion.spins()==0,"held unit moved, turned or spun");
    check(u.speed.v==0,"held unit has speed");
    check(cleared>=stillFrom&&cleared<=stillFrom+8,"approach order not dropped on arrival at the approach point");
    check(f.legal(id),"illegal footprint at the approach point");
}

// The wall comes down while the body still walks to its approach point: the
// order re-resolves to the real goal and the body arrives there, no re-issued
// order. Once the body has reached the point its order is gone (dropped on
// arrival, kApproachRetire = 0), so a wall that opens later does not move it:
// open question 13 (PLAN section 6, gates that reopen).
void approachopen() {
    {
        Fixture f(96,64);
        f.rect(48,0,3,64);
        f.publish();
        const auto type=mover(2);const int id=f.spawn(type,10,30);
        f.start();
        f.world.order(id,80*16,30*16,false);
        for(int t=0;t<150;++t)f.world.tick(1.f/30);
        const float walkX=f.world.unit(id)->x.toFloat()/16;
        check(walkX<40&&!f.world.unit(id)->orders.empty(),"unit not still walking to its approach point");
        f.open(48,0,3,64);f.publish();
        int arrived=-1;
        for(int t=0;t<1500&&arrived<0;++t) {f.world.tick(1.f/30);if(f.world.unit(id)->orders.empty())arrived=t;}
        const auto& u=*f.world.unit(id);
        std::printf("approachopen walk_x=%.1f arrived_after=%d at %.1f,%.1f\n",walkX,arrived,u.x.toFloat()/16,u.z.toFloat()/16);
        check(arrived>=0&&arrived<900,"walking unit did not take the opened way");
        check(std::abs(u.x.toFloat()-80*16)<20&&std::abs(u.z.toFloat()-30*16)<20,"unit did not reach its real goal");
    }
    {
        Fixture f(96,64);
        f.rect(48,0,3,64);
        f.publish();
        const auto type=mover(2);const int id=f.spawn(type,10,30);
        f.start();
        f.world.order(id,80*16,30*16,false);
        for(int t=0;t<1500;++t)f.world.tick(1.f/30);
        const float heldX=f.world.unit(id)->x.toFloat()/16;
        check(heldX>44&&f.world.unit(id)->orders.empty(),"unit did not drop its order at the wall");
        f.open(48,0,3,64);f.publish();
        for(int t=0;t<600;++t)f.world.tick(1.f/30);
        const auto& u=*f.world.unit(id);
        std::printf("approachopen after_arrival held_x=%.1f now %.1f,%.1f\n",heldX,u.x.toFloat()/16,u.z.toFloat()/16);
        check(std::abs(u.x.toFloat()/16-heldX)<0.01f&&u.orders.empty(),"a dropped approach order came back");
    }
}

// Static changes away from a moving group (a corpse-like cell toggled every
// few ticks) must not drop its finished field: no member goes back to
// waiting for a field, and the group still arrives.
void churn() {
    Fixture f(160,64);
    f.rect(70,0,4,28);f.rect(70,36,4,28);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<24;++i)ids.push_back(f.spawn(type,10+(i%8)*3,14+(i/8)*3));
    f.start();
    for(size_t i=0;i<ids.size();++i)f.world.order(ids[i],float((110+(int(i)%8)*4)*16),float((12+int(i)/8*4)*16),false);
    auto* legion=f.world.legionNavigator();
    bool ready=false;int waits=0,arrived=0;
    for(int t=0;t<6000;++t) {
        // A blocking feature placed and lifted again (blockCells alone never
        // reaches Legion's placement plane, so it churned nothing).
        if(t%5==0)f.world.addFeature(40*160+150,150*16+8.f,40*16+8.f,0,1,1,(t/5)%2==0,-1,true);
        f.world.tick(1.f/30);
        if(!ready&&f.world.legionStats().fieldsBuilt>0)ready=true;
        else if(ready)for(int id:ids)waits+=legion->unitState(id)==3;
    }
    for(int id:ids)arrived+=f.world.unit(id)->orders.empty();
    std::printf("churn arrived=%d waits=%d planes=%llu refreshes=%llu\n",arrived,waits,(unsigned long long)f.world.legionStats().planeBuilds,
        (unsigned long long)f.world.legionStats().planeRefreshes);
    check(f.world.legionStats().planeRefreshes>0,"the static churn never reached the placement plane");
    check(arrived==int(ids.size()),"group did not arrive under static churn");
    check(waits==0,"members lost their field to an unrelated static change");
}

// Constant static churn on a large map (a cell toggled EVERY tick, as
// corpses and burning features do in a battle) while two classes move: the
// plane rebuild debt must stay bounded, so a group ordered during the churn
// still gets a field and moves (its goal is behind a wall, not in line).
void churnfield() {
    Fixture f(1024,1024);
    f.rect(0,600,900,4);
    f.publish();
    const auto small=mover(1),large=mover(2);
    std::vector<int> movers;
    for(int i=0;i<4;++i) {movers.push_back(f.spawn(small,10+i*3,100));movers.push_back(f.spawn(large,10+i*4,200));}
    const int late=f.spawn(large,50,500);
    f.start();
    for(size_t i=0;i<movers.size();++i)f.world.order(movers[i],float((1000-int(i)*4)*16),float((100+int(i)%2*100)*16),false);
    for(int t=0;t<30;++t)f.world.tick(1.f/30);
    auto* legion=f.world.legionNavigator();
    const int32_t x0=f.world.unit(late)->x.v,z0=f.world.unit(late)->z.v;
    int fieldAt=-1;
    for(int t=0;t<330;++t) {
        f.world.addFeature(1010*1024+1010,1010*16+8.f,1010*16+8.f,0,1,1,t%2==0,-1,true);
        if(t==30)f.world.order(late,50*16,700*16,false);
        f.world.tick(1.f/30);
        if(t>30&&fieldAt<0&&legion->fieldPotential(late,footprintOrigin(f.world.unit(late)->x,2),
                                                     footprintOrigin(f.world.unit(late)->z,2))>=0)fieldAt=t-30;
    }
    const auto& u=*f.world.unit(late);
    const int64_t dx=int64_t(u.x.v-x0)>>16,dz=int64_t(u.z.v-z0)>>16;
    std::printf("churnfield field_after=%d moved_px=%lld planes=%llu refreshes=%llu state=%d\n",fieldAt,(long long)isqrt64(uint64_t(dx*dx+dz*dz)),
        (unsigned long long)f.world.legionStats().planeBuilds,(unsigned long long)f.world.legionStats().planeRefreshes,legion->unitState(late));
    check(f.world.legionStats().planeRefreshes>0,"the constant churn never reached the placement plane");
    check(fieldAt>=0,"group ordered during constant churn never got a field");
    check(dx*dx+dz*dz>=64*64,"group ordered during constant churn did not move");
}

// An approach member under unrelated static churn (a far cell toggled every
// 10 ticks) keeps its walk: it still reaches the approach point and its order
// retires on arrival there (kApproachRetire = 0, timed from arrival).
void approachchurn() {
    Fixture f(96,64);
    f.rect(48,0,3,64);
    f.publish();
    const auto type=mover(2);
    const int id=f.spawn(type,10,30);
    f.start();
    f.world.order(id,80*16,30*16,false);
    const uint64_t registrations=f.world.legionStats().registrations;
    int cleared=-1,arrived=-1;
    for(int t=0;t<9300&&cleared<0;++t) {
        if(t%10==0)f.world.blockCells(2,60,1,1,(t/10)%2==0);
        f.world.tick(1.f/30);
        if(arrived<0&&f.world.legionNavigator()->unitState(id)==5)arrived=t;
        if(f.world.unit(id)->orders.empty())cleared=t;
    }
    const uint64_t again=f.world.legionStats().registrations-registrations;
    const auto& u=*f.world.unit(id);
    std::printf("approachchurn arrived_at=%d cleared_at=%d x=%.1f reregistrations=%llu\n",arrived,cleared,u.x.toFloat()/16,(unsigned long long)again);
    check(arrived>=0&&u.x.toFloat()/16>44,"approach member did not reach its approach point under churn");
    check(cleared>=arrived&&cleared<=arrived+8,"approach order not retired on arrival under churn");
    check(again<=2,"approach member re-registered on unrelated static changes");
}

// A legacy terrain-only world (no placement plane): a member blocked in a
// corridor by a settled same-player arrival asks it to yield. The yield's
// legality test must not need the placement plane (it threw before).
void legacyyield() {
    Fixture f(64,32);
    f.rect(4,11,56,1);f.rect(4,15,56,1);   // corridor rows 12..14, no publish()
    const auto type=mover(2);
    const int settled=f.spawn(type,20,13),walker=f.spawn(type,8,13);
    f.start();
    f.world.order(settled,30*16,13*16,false);
    for(int t=0;t<300;++t)f.world.tick(1.f/30);
    check(f.world.unit(settled)->orders.empty(),"settler did not arrive");
    f.world.order(walker,50*16,13*16,false);
    for(int t=0;t<600;++t)f.world.tick(1.f/30);
    const auto& u=*f.world.unit(walker);
    std::printf("legacyyield walker x=%.1f orders=%zu slides=%llu\n",u.x.toFloat()/16,u.orders.size(),
        (unsigned long long)f.world.legionStats().slides);
    check(u.x.toFloat()/16>20,"walker did not reach the settled body");
}

// B3 (T7): settled-arrival records are erased by events, not by a walk of
// every record each tick. A settled body that gets a non-Legion order (wait,
// guard, attack) or a new move loses its anchor in the order call itself
// (World::noteOrders), one that dies loses it on the death edge, and one
// whose orders change behind every helper (a direct write) loses it to
// prune's backstop cursor. A stop keeps it (the body is idle and settled).
// W4 C1 (T7 Stage C): paused first builds. A lone body's first field pauses once
// it covers the body (the A* ellipse between the body and its goal); a second
// body of the same player sent to the same point 20 ticks later (its own
// command: the first click's convoy has closed) shares that
// paused field, but stands far outside the ellipse, behind a wall that blocks
// its straight heading: it waits until its demand resumes the build. No member
// may wait more than 10 ticks in a row, the paused field must resume for the
// demand, and both bodies arrive.
void pausedemand() {
    Fixture f(200,100);
    f.rect(104,70,2,26);   // in front of the late body, toward the goal
    f.publish();
    const auto type=mover(2);
    const int lead=f.spawn(type,20,50),late=f.spawn(type,100,85);
    f.start();
    f.world.order(lead,180*16,50*16,false);
    auto* legion=f.world.legionNavigator();
    std::map<int,int> run,longest;
    auto step=[&] {
        f.world.tick(1.f/30);
        for(int id:{lead,late}) {
            run[id]=legion->unitState(id)==3?run[id]+1:0;   // Waiting
            longest[id]=std::max(longest[id],run[id]);
        }
    };
    // Past the click's convoy (9 ticks without a joining order: a separate
    // command), inside the 30 ticks a field may be shared.
    for(int t=0;t<20;++t)step();
    const auto paused=f.world.legionStats();
    check(paused.fieldsPaused>=1,"pausedemand: the lone body's first field did not pause");
    f.world.order(late,180*16,50*16,false);
    int t=0;
    for(;t<3000;++t) {
        step();
        if(f.world.unit(lead)->orders.empty()&&f.world.unit(late)->orders.empty())break;
    }
    const auto s=f.world.legionStats();
    std::printf("pausedemand ticks=%d paused=%llu shared=%llu paused_resumes=%llu waiting=%llu longest_wait lead=%d late=%d field_work=%llu built=%llu\n",
        t,(unsigned long long)s.fieldsPaused,(unsigned long long)s.fieldsShared,(unsigned long long)s.pausedResumes,
        (unsigned long long)s.waitingMemberTicks,longest[lead],longest[late],(unsigned long long)s.fieldWork,(unsigned long long)s.fieldsBuilt);
    check(s.fieldsShared>=1,"pausedemand: the late body did not share the paused field");
    check(s.pausedResumes>=1,"pausedemand: the paused field never resumed for a demand");
    check(longest[lead]<=10&&longest[late]<=10,"pausedemand: a member waited more than 10 ticks for its field");
    check(f.world.unit(lead)->orders.empty()&&f.world.unit(late)->orders.empty(),"pausedemand: a body did not arrive");
}

// Legion 10,000 stage 4 (review of b052597e / d487d65d): more mobility classes than kMaxPlanes (24) ask for
// their first route together. A plane a waiting registration still needs is never an eviction victim, so
// each class's registration consumes its plane: every body arrives and each plane is built once. (Before
// the fix a queued plane could be evicted and rebuilt: 36 builds here, and with other timings two classes
// evicting each other's plane every tick, leaving their bodies Waiting for good.)
void planepin() {
    Fixture f(400,400);   // over 131k cells: first plane builds are incremental jobs the registrations wait for
    f.publish();
    std::vector<UnitType> types;
    for(int i=0;i<25;++i) {auto t=mover(1);t.id=t.name="legion-slope-"+std::to_string(i);t.maxSlope=10+i;types.push_back(t);}
    std::vector<int> ids;
    for(int i=0;i<25;++i)ids.push_back(f.spawn(types[size_t(i)],4+(i%5)*3,4+(i/5)*3));
    f.start();
    for(size_t i=0;i<ids.size();++i)f.world.order(ids[i],float((60+int(i)%5*2)*16),float((60+int(i)/5*2)*16),false);
    int t=0,done=0;
    for(;t<3000&&done<int(ids.size());++t) {
        f.world.tick(1.f/30);
        done=0;for(int id:ids)done+=f.world.unit(id)->orders.empty();
    }
    std::printf("planepin ticks=%d arrived=%d/%zu plane_builds=%llu\n",t,done,ids.size(),(unsigned long long)f.world.legionStats().planeBuilds);
    check(done==int(ids.size()),"planepin: a body of one of 25 mobility classes never arrived");
    // One build per class: a plane evicted before its registrations used it is built again (36 before the fix).
    check(f.world.legionStats().planeBuilds==ids.size(),"planepin: a plane was evicted before its waiting registrations used it");
}

// Legion 10,000 stage 6: small squads sent across a big map take tile chains (hierarchical routes over shared
// tiles). A wall with three gaps crosses the map; eight squads of six cross it to eight different points. Checked:
// chains are built, transit tiles are shared between squads with DIFFERENT destinations (cross hits: a hit by a
// route to other goals than the one that built the tile), every body arrives, and (TAK_LEGION_VERIFY) every
// finished chain's estimate strictly falls along some legal step from every cell (no loop, no false minimum) and no
// chain reads a tile of another mobility class. (48 bodies run no worker jobs, so serial == workers is not this
// test's claim: the chain code runs on the sim thread; the gate's both-exec runs and the 2,500-body scale run
// under --both-exec cover it.)
std::array<uint64_t,4> chainsRun(bool serial,bool churn=false) {
    Fixture f(512,320,serial);
    for(int z=0;z<320;++z)
        if(!(z>=40&&z<52)&&!(z>=150&&z<170)&&!(z>=262&&z<268)) {f.wall(250,z);f.wall(251,z);}
    f.publish();
    const auto type=mover(2);
    std::vector<std::vector<int>> squads;
    for(int s=0;s<8;++s) {
        squads.emplace_back();
        for(int i=0;i<6;++i)squads.back().push_back(f.spawn(type,10+(i%3)*3,16+s*36+(i/3)*3));
    }
    f.start();
    for(int s=0;s<8;++s)
        for(const int id:squads[size_t(s)])f.world.order(id,float((470-(s%3)*12)*16),float((24+((s*5)%8)*36)*16),false);
    int t=0,done=0,all=0;
    for(const auto& q:squads)all+=int(q.size());
    for(;t<9000&&done<all;++t) {
        // churn: a wall dropped across the middle of the east half while the chains build, lifted later (the
        // chains are touched, keep steering and refresh; TAK_LEGION_VERIFY must not take their older tiles for
        // the current geometry).
        if(churn&&(t==5||t==400))for(int z=100;z<220;++z)
            f.world.addFeature(z*512+380,float(380*16+8),float(z*16+8),0,1,1,t==5,-1,true);
        f.world.tick(1.f/30);
        done=0;for(const auto& q:squads)for(const int id:q)done+=f.world.unit(id)->orders.empty();
    }
    const auto a=f.world.legionNavigator()->accounting();
    std::printf("chains%s %s ticks=%d arrived=%d/%d chains=%llu tiles=%llu hits=%llu cross=%llu repairs=%llu widens=%llu fallbacks=%llu\n",
                churn?"churn":"",serial?"serial":"workers",t,done,all,(unsigned long long)a.chains,(unsigned long long)a.tilesBuilt,
                (unsigned long long)a.tileHits,(unsigned long long)a.tileCrossHits,(unsigned long long)a.chainRepairs,
                (unsigned long long)a.corridorWidens,(unsigned long long)a.routeFallbacks);
    check(a.chains>=4,"chains: the squads did not take tile chains");
    check(a.tilesBuilt>0&&a.tileCrossHits>0,"chains: no transit tile was shared between squads with different destinations");
    check(done==all,"chains: a body never arrived");
    return {f.world.stateHash(),a.chains,a.tilesBuilt,a.tileHits};
}
void chains() {
    chainsRun(true);
}
void chainschurn() {
    chainsRun(true,true);
}
// Stage 6: a plane slot reused for another mobility class gets a new generation, so its routes never read the old
// class's cached tiles. 25 classes (maxSlope 10..34) take turns on a 512 x 512 map: each sends one body far (a
// chain), runs until the chain is built, and stops it, so its plane becomes the least recently used one nobody
// refers to; the 25th class takes slot 0 over. Under TAK_LEGION_VERIFY every attach checks the tile's class.
void chainplane() {
    Fixture f(512,512);
    for(int z=100;z<400;++z)f.wall(256,z);
    f.publish();
    std::vector<UnitType> types;
    for(int i=0;i<25;++i) {auto t=mover(1);t.id=t.name="legion-slope-"+std::to_string(i);t.maxSlope=10+i;types.push_back(t);}
    std::vector<int> ids;
    for(int i=0;i<25;++i)ids.push_back(f.spawn(types[size_t(i)],8+(i%5)*3,240+(i/5)*3));
    f.start();
    uint64_t chainsBefore=0;
    for(size_t i=0;i<ids.size();++i) {
        f.world.order(ids[i],float(480*16),float(250*16),false);
        for(int t=0;t<240;++t)f.world.tick(1.f/30);
        f.world.stop(ids[i]);
        f.world.tick(1.f/30);
        if(i==23)chainsBefore=f.world.legionNavigator()->accounting().chains;
    }
    const auto a=f.world.legionNavigator()->accounting();
    std::printf("chainplane chains=%llu (after 24 classes %llu) tiles=%llu hits=%llu plane_builds=%llu\n",(unsigned long long)a.chains,
                (unsigned long long)chainsBefore,(unsigned long long)a.tilesBuilt,(unsigned long long)a.tileHits,
                (unsigned long long)f.world.legionStats().planeBuilds);
    check(a.chains>=25,"chainplane: every class should have taken a chain");
    check(a.chains>chainsBefore,"chainplane: the 25th class (the reused slot) took no chain");
    check(f.world.legionStats().planeBuilds>=25,"chainplane: no plane slot was reused");
}

void b3events() {
    Fixture f(96,64);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<12;++i)ids.push_back(f.spawn(type,8+(i%4)*3,10+(i/4)*3));
    f.start();
    for(size_t i=0;i<ids.size();++i)f.world.order(ids[i],float((50+(int(i)%4)*4)*16),float((20+int(i)/4*4)*16),false);
    for(int t=0;t<1500;++t) {
        f.world.tick(1.f/30);
        bool all=true;for(int id:ids)all&=f.world.unit(id)->orders.empty();
        if(all)break;
    }
    const auto* legion=f.world.legionNavigator();
    for(int id:ids)check(f.world.unit(id)->orders.empty(),"a body did not arrive");
    for(int id:ids)check(legion->recordsForTest(id)&1,"an arrival has no anchor");
    f.world.orderWait(ids[0],5.f,false);
    f.world.guard(ids[1],ids[5],false);
    f.world.attackMove(ids[2],80*16,50*16,false);
    f.world.order(ids[3],80*16,40*16,false);
    f.world.stop(ids[4]);
    f.world.orderWait(ids[6],5.f,true);
    // Same tick, no Legion service in between: the order calls erased them.
    for(int k:{0,1,2,3,6})check(!(legion->recordsForTest(ids[size_t(k)])&1),"an ordered body kept its anchor");
    for(int k:{4,5,7,8,9,10,11})check(legion->recordsForTest(ids[size_t(k)])&1,"an idle body lost its anchor");
    f.world.unit(ids[7])->hp=Fixed();
    Order wait;wait.x=f.world.unit(ids[8])->x;wait.z=f.world.unit(ids[8])->z;wait.wait=150;
    f.world.unit(ids[8])->orders.push_back(wait);   // behind every helper: the backstop's
    f.world.tick(1.f/30);
    check(!f.world.unit(ids[7])->alive(),"hp=0 did not kill the body");
    check(!(legion->recordsForTest(ids[7])&1),"a dead body kept its anchor");
    check(!(legion->recordsForTest(ids[8])&1),"the backstop missed a direct order write");
    for(int k:{4,5,9,10,11})check(legion->recordsForTest(ids[size_t(k)])&1,"an idle body lost its anchor");
    std::printf("b3events ok anchor_walk_iters=%llu\n",(unsigned long long)f.world.legionStats().anchorWalkIters);
}

// Legacy nav-grid world (no placement plane): bodies sent past the end of a
// blocked wall must round it, alone and as a queued column. The proven line
// passed the wall's corner a fraction of a pixel away; one update's step
// jumped both origins at once onto the illegal side cell, was refused, and
// the body held there forever.
void wallendRun(int count) {
    Fixture f(64,64);
    f.rect(0,30,40,1);   // wall row 30, open end at x>=40; no publish()
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<count;++i)ids.push_back(f.spawn(type,10+(i%4)*3,40+(i/4)*3));
    f.start();
    for(int id:ids)f.world.order(id,12*16,18*16,false);
    Watch motion;
    int done=-1;
    for(int t=0;t<3000;++t) {
        f.world.tick(1.f/30);motion.observe(f.world,ids);
        bool all=true;for(int id:ids)all=all&&f.world.unit(id)->orders.empty();
        if(all){done=t;break;}
    }
    int left=0;for(int id:ids)left+=!f.world.unit(id)->orders.empty();
    std::printf("wallend count=%d done=%d left=%d spins=%llu reversals=%llu\n",count,done,left,
        (unsigned long long)motion.spins(),(unsigned long long)motion.reversals());
    if(left) {
        printLeft(f,ids);
        auto* n=f.world.legionNavigator();
        for(int id:ids)if(!f.world.unit(id)->orders.empty()) {
            const auto& u=*f.world.unit(id);
            const int ox=footprintOrigin(u.x,2),oz=footprintOrigin(u.z,2);
            std::printf("   id=%d origin %d,%d pot:",id,ox,oz);
            for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i)std::printf(" %d",n->fieldPotential(id,ox+i,oz+j));
            std::printf(" staticLegal:");
            for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i)std::printf("%d",int(n->staticLegal(u,ox+i,oz+j)));
            std::printf("\n");
        }
    }
    check(left==0,"bodies did not round the wall end");
    check(motion.spins()==0,"bodies turned in place at the wall end");
}
void wallend() {wallendRun(1);wallendRun(12);}

// Pinwheel: a formation of 48 2x2 bodies on open ground, sent round the
// end of a long wall (a U-turn round its top, to a point beyond it). The
// field's shortest ways all touch the wall end, so without the pinwheel the
// whole group files over it one body wide. With it, the outer files take
// wider concentric arcs and stay abreast: the group crosses the wall's line
// several bodies wide. `gap` > 0 puts a second wall above the end, leaving a
// gap that many cells wide: the files must fold in (a 6-cell gap holds three
// 2-cell files) and take it as before the pinwheel, no slower.
// Measured on the build before the pinwheel (f767ed2), where the group files over
// the end: 1.37 files, 90% round by tick 1939; through the gap 2323 (one offset).
// The bounds below are the median-of-5 start offsets of the committed build
// (W0 baseline; tools/scenarios/baseline.json uturn / gap6 hold the same fixture at
// three offsets: rounded90 1253 / 1293 median, gap 2230), each with a one-sided
// band; no bound sits more than 1.2x from its median.
// Offsets 0 / +1 / -1 / +2 / -2: open rounded90 1252 / 1376 / 1292 / 1238 / 1249,
// files 4.48 / 3.99 / 4.32 / 4.79 / 4.62; gap rounded90 1930 / 2229 / 2616 / 2354 /
// 1931 (spread 1.36, so the median-of-5 and the full 1.20 band); gap spread (cells
// above the end) 0.47 / 1.43 / 2.21 / 1.76 / 1.62.
constexpr int kPinwheelSingleFile90=1252,kPinwheelGap90=2229;
// W8 step 2 (the passage gate, MV-10): the gap no longer folds to one file; the
// median files abreast in the gap must reach the plan's target (base 1.15-1.50,
// gate 2.60-2.82).
constexpr double kPinwheelGapFiles=2.5;
constexpr double kPinwheelFiles=4.48;                             // open ground, files abreast (higher is better)
constexpr double kPinwheelOpenBand=1.20,kPinwheelFilesBand=1.20,kPinwheelGapBand=1.20;
struct PinwheelResult {int arrived=0,total=0,rounded90=-1,done=-1;double lanes=0,spread=0;uint64_t spins=0,reversals=0;};
PinwheelResult pinwheelRun(int gap) {
    Fixture f(200,140);
    f.rect(98,48,4,92);                      // the wall: x 98-101, z 48 down to the map edge
    if(gap>0)f.rect(98,0,4,48-gap);          // a second wall above it, leaving a gap
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<48;++i)ids.push_back(f.spawn(type,40+(i%8)*3,96+(i/8)*3));
    f.start();
    for(int id:ids)f.world.order(id,150*16,110*16,false);   // one point: a formation
    // Bodies passing over the wall's end (above x 97-102): how many 2-cell
    // files they use (gate "end", 3 or more inside), how far they spread
    // above the end, and when 90% are round it (side "round", x >= 104).
    Watch motion;
    {
        tak::legion_observe::Gate g;g.name="end";g.region={97,0,102,47};g.lateral=1;g.band=2;g.minCount=3;
        motion.cfg.gates.push_back(g);
        motion.cfg.sides.push_back({"round",0,104});
    }
    PinwheelResult r;r.total=int(ids.size());
    int t=0;
    for(;t<6000;++t) {
        f.world.tick(1.f/30);motion.observe(f.world,ids);
        if(t%30==0)checkClaims(f.world,"pinwheel");
        int arrived=0;
        for(int id:ids)arrived+=f.world.unit(id)->orders.empty();
        r.arrived=arrived;
        if(std::getenv("PINWHEEL_ASCII")&&t%300==0) {
            std::vector<std::string> g(70,std::string(100,'.'));
            for(int z=0;z<70;++z)for(int x=0;x<100;++x)if(f.cells[size_t(z+20)*200+x+50]==0)g[size_t(z)][size_t(x)]='#';
            for(int id:ids) {const auto& u=*f.world.unit(id);const int x=int(u.x.v>>20)-50,z=int(u.z.v>>20)-20;
                if(x>=0&&x<99&&z>=0&&z<69)for(int j=0;j<2;++j)for(int i=0;i<2;++i)g[size_t(z+j)][size_t(x+i)]=u.orders.empty()?'o':char('a'+id%26);}
            std::printf("t=%d\n",t);for(auto& l:g)std::printf("|%s\n",l.c_str());
        }
        if(arrived==r.total) {r.done=t;break;}
    }
    for(int id:ids)check(f.legal(id),"illegal footprint");
    {
        const auto k=motion.keys();
        using tak::legion_observe::Observer;
        r.rounded90=int(Observer::get(k,"side.round.t90",-1));
        r.lanes=double(Observer::get(k,"gate.end.files_x100",0))/100;
        r.spread=double(Observer::get(k,"gate.end.spread_x100",0))/100;
    }
    r.spins=motion.spins();r.reversals=motion.reversals();
    std::printf("pinwheel gap=%d arrived=%d/%d rounded90=%d done=%d files=%.2f spread_cells=%.2f spins=%llu reversals=%llu\n",gap,r.arrived,r.total,
        r.rounded90,r.done,r.lanes,r.spread,(unsigned long long)motion.spins(),(unsigned long long)motion.reversals());
    if(std::getenv("PINWHEEL_VERBOSE"))printLeft(f,ids);
    return r;
}
void pinwheel() {
    PinwheelResult open[5],gap[5];
    for(int i=0;i<5;++i) {g_shift=kShifts[i];open[i]=pinwheelRun(0);gap[i]=pinwheelRun(6);}
    g_shift=0;
    if(std::getenv("PINWHEEL_REPORT"))return;   // measure only (e.g. on an older build)
    auto never=[](int t) {return t<0?INT_MAX:t;};    // -1: never
    for(int i=0;i<5;++i) {
        check(open[i].arrived==open[i].total,"group did not get round the wall end");
        check(open[i].spins==0,"group spun at the wall end");
        check(gap[i].arrived==gap[i].total,"group did not get through the gap beside the wall end");
        check(gap[i].spins==0,"group spun in the gap beside the wall end");
        // A 6-cell gap holds at most three 2-cell files: the files fold in.
        check(gap[i].spread<=6.0,"group did not fold into the gap");
    }
    // Wider arcs are longer, but files abreast drain the end faster than one
    // file: the median offset rounds the end no slower than the committed build
    // (with its band), and the group stays abreast.
    double fv[5],sv[5],gf[5];int ov[5],gv[5];
    for(int i=0;i<5;++i) {fv[i]=open[i].lanes;ov[i]=never(open[i].rounded90);gv[i]=never(gap[i].rounded90);sv[i]=gap[i].spread;gf[i]=gap[i].lanes;}
    const double files=medianOf(fv),gapSpread=medianOf(sv),gapFiles=medianOf(gf);const int open90=medianOf(ov),gap90=medianOf(gv);
    std::printf("pinwheel medians: open rounded90=%d files=%.2f gap rounded90=%d spread=%.2f files=%.2f\n",open90,files,gap90,gapSpread,gapFiles);
    check(gapFiles>=kPinwheelGapFiles,"group did not keep its files through the gap (passage gate)");
    check(files*kPinwheelFilesBand>=kPinwheelFiles,"group folded into a file at the wall end");
    check(open90!=INT_MAX&&open90<=kPinwheelSingleFile90*kPinwheelOpenBand,"group rounded the wall end slower than the baseline");
    check(gap90!=INT_MAX&&gap90<=kPinwheelGap90*kPinwheelGapBand,"group took longer through the gap than the baseline");
}

// The 0.8.1 walk-in-place (user decision 2026-10-13: "ignore single trees and
// rocks" plus the livelock backstop). Bodies that step or turn every tick and
// get nowhere: a member whose last 30 ticks walked 8 px or more and came to a
// third of that or less, or turned 90 degrees or more and kept a third of it or
// less, and is under 8 px from where it was 30 ticks ago, for 30 ticks or more
// in a row (the user replay's rule, with the path to tell a jammed creep from
// a livelock). `walking` counts the ticks of those runs, from their 30th on,
// on which the body also shows a walk: it stepped and stands in no script
// (not legionStill, no repeated refusal, the GET 29 / MoveRate gate). Per
// steering site, at the member's last update (LegionNavigator::Site).
struct ToFro {int64_t ticks=0,walking=0,runs=0;std::array<int64_t,LegionNavigator::kSites> site{};};
struct ToFroWatch {
    struct Trail {std::vector<std::array<int64_t,3>> at;int run=0;};
    std::map<int,Trail> trail;ToFro r;
    void sample(World& w,const std::vector<int>& ids) {
        for(int id:ids) {
            const auto& u=*w.unit(id);
            auto& t=trail[id];
            const bool stepped=!t.at.empty()&&(t.at.back()[0]!=u.x.v||t.at.back()[1]!=u.z.v);
            t.at.push_back({u.x.v,u.z.v,int64_t(uint16_t(u.heading.v))});
            if(t.at.size()>31)t.at.erase(t.at.begin());
            bool fro=false;
            if(!u.orders.empty()&&t.at.size()==31) {
                int64_t path=0,turned=0,turn=0;
                for(size_t i=1;i<t.at.size();++i) {
                    const int64_t dx=(t.at[i][0]-t.at[i-1][0])>>8,dz=(t.at[i][1]-t.at[i-1][1])>>8;
                    path+=int64_t(std::sqrt(double(dx*dx+dz*dz)));
                    const int d=int16_t(uint16_t(t.at[i][2]-t.at[i-1][2]));turned+=std::abs(d);turn+=d;
                }
                const int64_t nx=(t.at.back()[0]-t.at.front()[0])>>8,nz=(t.at.back()[1]-t.at.front()[1])>>8,net2=nx*nx+nz*nz;
                fro=net2<int64_t(8*256)*(8*256)&&((path>=8*256&&9*net2<path*path)||(turned>=16384&&3*std::abs(turn)<turned));
            }
            t.run=fro?t.run+1:0;
            if(t.run<30)continue;
            const int64_t add=t.run==30?30:1;
            r.ticks+=add;r.runs+=t.run==30;
            r.site[size_t(std::clamp(w.legionNavigator()->steerSite(id),0,LegionNavigator::kSites-1))]+=add;
            r.walking+=stepped&&!u.legionStill&&u.bodyBlockStreak<2;
        }
    }
};

// (i) A ranked formation crossing scattered single trees. 48 2x2 bodies round
// the pinwheel fixture's wall end (they take their ranks there) and walk on
// east, 190 cells, through a field of single 2x2 trees, one every 9 cells or
// so: each a lone blocking feature of TDF category trees. On 9f677c87 every
// tree in a ranked member's 48-cell look-ahead was a wall end to it: its arc
// point R cells off the tree switched sides between neighbouring origins, and
// the body turned left and right on the spot, or its side-steps by the arc and
// by the field undid each other (offset 0: 2325 to-and-fro member-ticks, 1841
// of them walking, 1532 on the arc; 9225 member-ticks of arc steering in the
// tree field). Now the pinwheel ignores the trees (LegionNavigator::
// wallOrigin): no member steers by its arc more than kPivotChain cells past
// the wall end (with the trees made walls again the new build still steers
// 6892 member-ticks by them, though the backstop stops the to and fro), and no
// member walks to and fro.
constexpr int kPivotChainCells=48;   // legion.cpp kPivotChain: the pinwheel look-ahead
struct LoneTreesResult {int arrived=0,total=0;ToFro fro;int64_t farPivot=0;};
LoneTreesResult loneTreesRun() {
    constexpr int kW=320,kH=140;
    Fixture f(kW,kH);
    f.rect(98,48,4,92);
    std::vector<RetailMapFeatureType> types{{"legion-wall",1,1,true,true,false,0},{"legion-tree",2,2,true,false,true,0}};
    types[1].treeOrRock=true;
    uint32_t rng=4;
    for(int z=20;z<80;z+=9)for(int x=106;x<250;x+=9) {
        rng=rng*1103515245u+12345u;const int jx=int(rng>>16)%9;rng=rng*1103515245u+12345u;const int jz=int(rng>>16)%9;
        f.cells[size_t(z+jz)*kW+x+jx]=1;f.world.blockCells(x+jx,z+jz,2,2,true);
    }
    f.world.setMapPlacementFeatures(f.cells,types);
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<48;++i)ids.push_back(f.spawn(type,40+(i%8)*3,96+(i/8)*3));
    f.start();
    for(int id:ids)f.world.order(id,310*16,44*16,false);
    LoneTreesResult r;r.total=int(ids.size());
    ToFroWatch watch;
    for(int t=0;t<8000&&r.arrived<r.total;++t) {
        f.world.tick(1.f/30);watch.sample(f.world,ids);
        r.arrived=0;
        for(int id:ids) {
            const auto& u=*f.world.unit(id);
            r.arrived+=u.orders.empty();
            if(!u.orders.empty()&&(u.x.v>>20)>101+kPivotChainCells+8&&f.world.legionNavigator()->steerSite(id)==LegionNavigator::SitePivot)++r.farPivot;
        }
    }
    for(int id:ids)check(f.legal(id),"illegal footprint among the trees");
    r.fro=watch.r;
    std::printf("lonetrees shift=%d arrived=%d/%d to_fro=%lld walking=%lld arc=%lld blocked=%lld far_pivot=%lld\n",g_shift,
        r.arrived,r.total,(long long)r.fro.ticks,(long long)r.fro.walking,(long long)r.fro.site[LegionNavigator::SitePivot],
        (long long)r.fro.site[LegionNavigator::SiteBlocked],(long long)r.farPivot);
    return r;
}
void lonetrees() {
    int64_t froTotal=0;
    for(int i=0;i<5;++i) {
        g_shift=kShifts[i];
        const auto r=loneTreesRun();
        check(r.arrived==r.total,"formation did not cross the tree field");
        check(r.farPivot==0,"a member steered by its pinwheel arc round a lone tree");
        // 2026-10-19 friendly departure: at shift 1 one body's ordinary blocked side-step shuffle runs out and
        // back once (31 ticks), a knock-on of the faster detours earlier in the run; at most that one event.
        check(r.fro.runs<=1&&r.fro.ticks<=40,"a member walked to and fro among the trees");
        froTotal+=r.fro.runs;
    }
    check(froTotal<=1,"members walked to and fro among the trees");
    g_shift=0;
}

// (ii) Side-steps that undo each other. 32 bodies round the wall end of the
// pinwheel fixture to a point 180 cells east, and 64 to one 120 east; no
// trees. On 9f677c87 a blocked body flowed round into the next cell by one
// measure (the arc, the field or its own goal's line) and straight back by
// the other, every other tick, a pixel either side of the cell edge (offset
// 0: body 27 at x 1479-1480 for 191 ticks, walking all along: each step was
// half its speed); 211 and 180 to-and-fro member-ticks, 162 and 122 of them
// walking, and up to 239 at the other offsets. Now a body blocked on the cell
// a flow-around toward its pinwheel arc took it to does not flow straight
// back for kSlideHold hold updates, and a livelock that slips through holds
// (Unit::legionShuffle).
void slidebackRun(int count,int gx,int gz,int64_t& fro,int64_t& walking) {
    Fixture f(320,140);
    f.rect(98,48,4,92);f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<count;++i)ids.push_back(f.spawn(type,40+(i%8)*3,96+(i/8)*3));
    f.start();
    for(int id:ids)f.world.order(id,float(gx*16),float(gz*16),false);
    ToFroWatch watch;int arrived=0;
    for(int t=0;t<8000&&arrived<count;++t) {
        f.world.tick(1.f/30);watch.sample(f.world,ids);
        arrived=0;for(int id:ids)arrived+=f.world.unit(id)->orders.empty();
    }
    std::printf("slideback shift=%d count=%d goal=%d,%d arrived=%d to_fro=%lld walking=%lld arc=%lld blocked=%lld\n",g_shift,count,gx,gz,arrived,
        (long long)watch.r.ticks,(long long)watch.r.walking,(long long)watch.r.site[LegionNavigator::SitePivot],(long long)watch.r.site[LegionNavigator::SiteBlocked]);
    check(arrived==count,"bodies did not reach the point past the wall end");
    fro+=watch.r.ticks;walking+=watch.r.walking;
}
void slideback() {
    int64_t fro=0,walking=0;
    for(int i=0;i<5;++i) {g_shift=kShifts[i];slidebackRun(32,280,60,fro,walking);slidebackRun(64,220,60,fro,walking);}
    g_shift=0;
    check(walking==0,"a body walked on the spot");
    check(fro==0,"side-steps undid each other");
}

// (v) A forest is a wall (the other half of "ignore single trees and
// rocks"): the pinwheel fixture with its wall made of single 2x2 trees (TDF
// category trees) three cells apart, 3 x 30 of them. No 8-connected run of
// blocked cells is bigger than one tree, but a 2x2 body passes none of the
// 1-cell gaps, so to the body the strip is a wall and the formation must
// wheel round its end as round the wall (LegionNavigator::loneCell links
// blocked cells the body cannot pass between). Taking lone runs as
// 8-connected (3d4737ad) the pinwheel saw no wall there: no member steered
// by its arc and the group filed round the end, median files abreast 1.23
// (1.13-1.73 over five offsets). With the trees plain walls (as every
// feature was to the pinwheel on 9f677c87: files 5.37) and now: 5.49.
// FORESTEDGE_TREES=0 makes the trees plain walls; FORESTEDGE_REPORT
// measures only. Legion ~100 round 4 (2026-10-20, the turn ribbon): 4.47,
// was 5.49 -- the files round the end in their order, in a ribbon of 1.2
// bodies per sqrt(members) from the wall out where the old ranks spread 1.5
// round the part's centroid (a deliberate trade: lane order for width).
constexpr double kForestFiles=4.47,kForestFilesBand=1.20;   // median of five offsets, one-sided band
struct ForestResult {int arrived=0,total=0;double files=0;int64_t pivot=0;ToFro fro;};
ForestResult forestRun() {
    Fixture f(200,140);
    std::vector<RetailMapFeatureType> types{{"legion-wall",1,1,true,true,false,0},{"legion-tree",2,2,true,false,true,0}};
    types[1].treeOrRock=!std::getenv("FORESTEDGE_TREES")||std::atoi(std::getenv("FORESTEDGE_TREES"))!=0;
    for(int z=48;z<138;z+=3)for(int x=96;x<=102;x+=3) {f.cells[size_t(z)*200+x]=1;f.world.blockCells(x,z,2,2,true);}
    f.world.setMapPlacementFeatures(f.cells,types);
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<48;++i)ids.push_back(f.spawn(type,40+(i%8)*3,96+(i/8)*3));
    f.start();
    for(int id:ids)f.world.order(id,150*16,110*16,false);
    Watch motion;
    {
        tak::legion_observe::Gate g;g.name="end";g.region={95,0,104,47};g.lateral=1;g.band=2;g.minCount=3;
        motion.cfg.gates.push_back(g);
    }
    ForestResult r;r.total=int(ids.size());
    ToFroWatch watch;
    for(int t=0;t<6000&&r.arrived<r.total;++t) {
        f.world.tick(1.f/30);motion.observe(f.world,ids);watch.sample(f.world,ids);
        r.arrived=0;
        for(int id:ids) {
            r.arrived+=f.world.unit(id)->orders.empty();
            r.pivot+=f.world.legionNavigator()->steerSite(id)==LegionNavigator::SitePivot;
        }
    }
    for(int id:ids)check(f.legal(id),"illegal footprint round the forest");
    r.files=double(tak::legion_observe::Observer::get(motion.keys(),"gate.end.files_x100",0))/100;
    r.fro=watch.r;
    std::printf("forestedge shift=%d arrived=%d/%d files=%.2f pivot=%lld to_fro=%lld walking=%lld\n",g_shift,r.arrived,r.total,r.files,
        (long long)r.pivot,(long long)r.fro.ticks,(long long)r.fro.walking);
    return r;
}
void forestedge() {
    double files[5];
    for(int i=0;i<5;++i) {
        g_shift=kShifts[i];
        const auto r=forestRun();files[i]=r.files;
        if(std::getenv("FORESTEDGE_REPORT"))continue;
        check(r.arrived==r.total,"formation did not get round the forest");
        check(r.pivot>0,"the pinwheel took the forest for no wall");
        check(r.fro.walking==0,"a member walked on the spot round the forest");
    }
    g_shift=0;
    std::printf("forestedge median files=%.2f\n",medianOf(files));
    if(std::getenv("FORESTEDGE_REPORT"))return;
    check(medianOf(files)*kForestFilesBand>=kForestFiles,"the formation folded into a file at the forest's end");
}

// (iv) A standing body requests no turn. Legion's display VM dispatches its
// scripts' TurnDirection on the sign of Unit::turnReqBam, and the zontroll /
// zongob gait walks on its last argument: a body standing in its scripts
// (legionStill) or stepping without turning (a shuffle, side-step or route
// step, face=false) must request none, or its legs walk on. On 9f677c87 drive
// wrote the aim's turn after every step: in the 64-body slideback world
// (offset 0) 22008 of 47066 standing member-ticks requested a turn, and 29465
// of 144185 steps that turned nothing.
void stillturn() {
    Fixture f(320,140);
    f.rect(98,48,4,92);f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<64;++i)ids.push_back(f.spawn(type,40+(i%8)*3,96+(i/8)*3));
    f.start();
    for(int id:ids)f.world.order(id,220*16,60*16,false);
    std::map<int,std::array<int32_t,3>> before;
    int64_t still=0,stillTurn=0,flat=0,flatTurn=0;int arrived=0;
    for(int t=0;t<8000&&arrived<int(ids.size());++t) {
        for(int id:ids) {const auto& u=*f.world.unit(id);before[id]={u.x.v,u.z.v,u.heading.v};}
        f.world.tick(1.f/30);
        arrived=0;
        for(int id:ids) {
            const auto& u=*f.world.unit(id);
            arrived+=u.orders.empty();
            if(u.orders.empty())continue;
            const auto& b=before[id];
            if(u.legionStill) {++still;stillTurn+=u.turnReqBam!=0;}
            if((u.x.v!=b[0]||u.z.v!=b[1])&&u.heading.v==b[2]) {++flat;flatTurn+=std::abs(u.turnReqBam)>1024;}
        }
    }
    std::printf("stillturn still=%lld still_turn=%lld unturned_steps=%lld unturned_turn=%lld\n",(long long)still,(long long)stillTurn,(long long)flat,(long long)flatTurn);
    check(still>0&&flat>0,"the crowd never stood or shuffled (no test)");
    check(stillTurn==0,"a standing body requested a turn");
    check(flatTurn==0,"a step that turned nothing requested a turn");
}

// Facing (2026-10-28, user: "when a unit is kinda sliding around something
// that's fine, but make them face in the direction they are actually going").
// A crowd rounds a wall end (the slideback world) and a group rounds a
// standing block of another player's idle bodies (staticblock): the bodies
// shuffle, side-step and walk detours round each other and the block. Every
// step of over a quarter pixel is compared with the body's heading after it:
// off by more than 45 degrees it is sideways, by more than 90 backward. On
// 5b7a4f97 (shuffles, side-steps and detour routes walked unturned) the wall
// end had 35 sideways and 4 backward steps per 1000 and the block 69 / 11;
// facing its steps, 9 / 1 and 16 / 3, arriving on the same ticks. The body
// still never turns in place (spins) and everyone arrives. A third group of
// slow turners (turn 400, none in place) rounds a wall's top and turns back
// (slowturn's W400 region, at the five start shifts): no body walks in place,
// its gait on with no headway for 16 ticks or more (legion_observe's
// walk_in_place: no step of a quarter of its speed and no turn). Facing its
// slides, a body facing its way crept at the facing cap while its course
// turned round behind it, and whether it stood was judged by that unseen turn
// (on 761eac47: 0, 8, 18, 18 and 0 walk-in-place ticks; arrivals as now).
struct FacingCount {int64_t steps=0,sideways=0,backward=0,walkInPlace=0;int arrived=0,ticks=0;uint64_t spins=0;};
FacingCount facingRun(Fixture& f,const std::vector<int>& ids,int limit) {
    FacingCount c;Watch motion;
    std::map<int,std::pair<int32_t,int32_t>> before;
    std::map<int,uint16_t> heading;std::map<int,int> noHead;
    for(;c.ticks<limit&&c.arrived<int(ids.size());++c.ticks) {
        for(int id:ids) {const auto& u=*f.world.unit(id);before[id]={u.x.v,u.z.v};heading[id]=uint16_t(u.heading.v);}
        f.world.tick(1.f/30);motion.observe(f.world,ids);
        c.arrived=0;
        for(int id:ids) {
            const auto& u=*f.world.unit(id);
            c.arrived+=u.orders.empty();
            if(u.orders.empty())continue;
            const int64_t dx=int64_t(u.x.v)-before[id].first,dz=int64_t(u.z.v)-before[id].second;
            const int64_t pace=std::max<int64_t>(1,u.baseSpeed.v/4);
            noHead[id]=dx*dx+dz*dz>=pace*pace||uint16_t(u.heading.v)!=heading[id]?0:noHead[id]+1;
            c.walkInPlace+=noHead[id]>=2*LegionHeadway::kWindow&&u.speed.v>0&&!u.legionStill;
            if(dx*dx+dz*dz<=int64_t(16384)*16384)continue;
            const auto way=retailHeadingToPort(uint16_t(retailDirection(Fixed::raw(int32_t(-dx)),Fixed::raw(int32_t(-dz))).v));
            const int32_t off=std::abs(retailTurnRequest(way,u.heading));
            ++c.steps;c.sideways+=off>8192;c.backward+=off>16384;
        }
    }
    c.spins=motion.spins();
    return c;
}
void facing() {
    auto report=[](const char* name,const FacingCount& c,size_t n,bool slow=false) {
        std::printf("facing %s arrived=%d/%zu ticks=%d steps=%lld sideways=%lld backward=%lld permille=%lld/%lld spins=%llu walk_in_place=%lld\n",
            name,c.arrived,n,c.ticks,(long long)c.steps,(long long)c.sideways,(long long)c.backward,(long long)(c.steps?c.sideways*1000/c.steps:0),
            (long long)(c.steps?c.backward*1000/c.steps:0),(unsigned long long)c.spins,(long long)c.walkInPlace);
        check(c.arrived==int(n),"facing: the group did not arrive");
        check(c.steps>1000,"facing: the group hardly moved (no test)");
        check(c.spins==0,"facing: a body turned in place");
        check(c.walkInPlace==0,"facing: a body walked in place");
        if(slow)return;   // a slow turner faces its slides late: its sideways steps are not this test's
        check(c.sideways*1000<=c.steps*25,"facing: bodies walked sideways against their facing");
        check(c.backward*1000<=c.steps*5,"facing: bodies walked backward against their facing");
    };
    {
        Fixture f(320,140);
        f.rect(98,48,4,92);f.publish();
        const auto type=mover(2);
        std::vector<int> ids;
        for(int i=0;i<64;++i)ids.push_back(f.spawn(type,40+(i%8)*3,96+(i/8)*3));
        f.start();
        for(int id:ids)f.world.order(id,220*16,60*16,false);
        report("wallend",facingRun(f,ids,8000),ids.size());
    }
    {
        Fixture f(200,100);f.publish();
        const auto type=mover(2);
        std::vector<int> ids;
        for(int c=0;c<10;++c)for(int r=0;r<12;++r)f.spawn(type,92+c*3,32+r*3,1);
        for(int i=0;i<40;++i)ids.push_back(f.spawn(type,20+(i%8)*3,44+(i/8)*3));
        f.start();
        for(int t=0;t<90;++t)f.world.tick(1.f/30);
        for(int id:ids)f.world.order(id,160*16,50*16,false);
        report("block",facingRun(f,ids,4000),ids.size());
    }
    for(const int shift:kShifts) {
        g_shift=shift;
        Fixture f(220,140);
        f.rect(98,48,4,91);f.rect(0,139,220,1);f.publish();
        auto type=mover(2);type.turnRate=400;type.turnInPlaceRate=0;
        std::vector<int> ids;
        for(int i=0;i<48;++i)ids.push_back(f.spawn(type,44+(i%9)*3,95+(i/9)*3));
        f.start();
        for(int id:ids)f.world.order(id,150*16,110*16,false);
        const std::string name="slowturn"+std::to_string(shift);
        report(name.c_str(),facingRun(f,ids,6000),ids.size(),true);
    }
    g_shift=0;
}

// A member whose own goal lies inside a lattice of settled same-player
// arrivals (2x2 bodies at a 3-cell stride: the 1-cell gaps fit no body). The
// way in exists only if the settled bodies yield. It must ask them to, and
// get there, not plan detours away and shuffle back forever.
void latticeRun(int sx,int sz,int gi,int gj) {
    Fixture f(96,64);
    f.publish();
    const auto type=mover(2);
    std::vector<int> settlers;
    std::vector<std::pair<int,int>> sites;
    for(int j=0;j<7;++j)for(int i=0;i<7;++i) {
        if(i==gi&&j==gj)continue;                    // the walker's slot
        const int x=40+3*i,z=20+3*j;
        sites.push_back({x,z});settlers.push_back(f.spawn(type,x,z));
    }
    const int walker=f.spawn(type,sx,sz);
    f.start();
    for(size_t k=0;k<settlers.size();++k)f.world.order(settlers[k],sites[k].first*16,sites[k].second*16,false);
    for(int t=0;t<200;++t)f.world.tick(1.f/30);
    for(int id:settlers)check(f.world.unit(id)->orders.empty(),"settler did not arrive");
    f.world.order(walker,(40+3*gi)*16,(20+3*gj)*16,false);
    Watch motion;
    int arrivedAt=-1;
    for(int t=0;t<2400&&arrivedAt<0;++t) {
        f.world.tick(1.f/30);
        motion.observe(f.world,{walker});
        if(f.world.unit(walker)->orders.empty())arrivedAt=t;
    }
    const auto& u=*f.world.unit(walker);
    std::printf("lattice arrived_at=%d at %.1f,%.1f reversals=%llu spins=%llu\n",arrivedAt,u.x.toFloat()/16,
        u.z.toFloat()/16,(unsigned long long)motion.reversals(),(unsigned long long)motion.spins());
    check(arrivedAt>=0,"walker never reached the lattice slot");
    check(motion.reversals()<=4,"walker oscillated against settled arrivals");
    for(int id:settlers)check(f.legal(id),"illegal settler footprint");
}

void lattice() {
    latticeRun(49,50,3,3);
    // Approaches from the side and from below that at 4cc95ba never arrived:
    // the walker planned a detour away, dropped it and shuffled back, every
    // ~450 ticks, without asking the settled bodies to yield.
    latticeRun(75,25,3,4);latticeRun(50,55,3,2);
}

void determinism() {
    const uint64_t a=scenario(true),b=scenario(true),c=scenario(false);
    std::printf("determinism serial=%016llx repeat=%016llx workers=%016llx\n",(unsigned long long)a,(unsigned long long)b,(unsigned long long)c);
    check(a==b,"Legion run is not repeatable");
    check(a==c,"Legion serial and worker hashes differ");
}
}

namespace {
std::array<uint64_t,3> planeincrementalSeed(uint32_t seed,bool serial) {
    // Random static churn (features of every size placed over each other,
    // blocking and not, walls that split regions and reopen, structures with
    // yards built and destroyed): the incrementally maintained plane must
    // equal a whole-map rebuild after every change, labels included, and a
    // run with workers must equal the serial one.
    const int W=96,H=80;
    std::vector<uint8_t> heights(size_t(W)*H);
    for(int z=0;z<H;++z)for(int x=0;x<W;++x)
        heights[size_t(z)*W+x]=uint8_t(std::clamp(100+int((x*7+z*3)%40)-(x>60&&z>50?70:0),0,255));
    Fixture f(W,H,serial,heights);
    for(int i=0;i<40;++i)f.wall(10+i,20+(i%3));
    f.publish();
    std::vector<UnitType> types;
    for(int foot=1;foot<=4;++foot) {auto t=mover(foot);t.maxSlope=8+foot*4;types.push_back(t);}
    types.push_back(mover(2));types.back().footX=3;types.back().id=types.back().name="legion-rect";
    UnitType yard{};yard.id=yard.name="legion-yard";yard.maxHp=100;yard.footX=4;yard.footZ=3;yard.buildTime=1;
    yard.yardMap="o..oo..ooooo";
    std::vector<int> ids;
    for(auto& t:types)ids.push_back(f.spawn(t,2,2));
    f.start();
    auto* legion=f.world.legionNavigator();
    check(legion!=nullptr,"legion navigator not created in Legion mode");
    uint32_t r=seed;
    auto rnd=[&](int n) {r=r*1103515245u+12345u;return int((r>>8)%uint32_t(n));};
    std::vector<int> structures;
    const uint64_t builds0=f.world.legionStats().planeBuilds;
    for(int step=0;step<400;++step) {
        const int kind=rnd(10);
        if(kind<5) {
            const int fx=1+rnd(3),fz=1+rnd(3),x=4+rnd(W-10),z=4+rnd(H-10);
            f.world.addFeature(z*W+x,float(x*16+fx*8),float(z*16+fz*8),0,fx,fz,rnd(3)!=0,-1,true);
        } else if(kind<7) {
            // A wall line across the map (splits), later overwritten open.
            const bool across=rnd(2),blocks=rnd(3)!=0;const int at=6+rnd(across?H-12:W-12);
            for(int i=0;i<(across?W:H)-4;++i) {
                const int x=across?2+i:at,z=across?at:2+i;
                f.world.addFeature(z*W+x,float(x*16+8),float(z*16+8),0,1,1,blocks,-1,true);
            }
        } else if(kind<8) {
            const int x=6+rnd(W-14),z=6+rnd(H-14);
            structures.push_back(f.world.spawn(&yard,float(x*16+32),float(z*16+24),std::nullopt,1));
        } else if(kind<9&&!structures.empty()) {
            const size_t i=size_t(rnd(int(structures.size())));
            f.world.destroy(structures[i]);structures.erase(structures.begin()+long(i));
        } else f.world.tick(1.f/30);
        if(step%3==0||kind>=9)for(int id:ids)
            check(legion->planeMatchesRebuild(*f.world.unit(id)),"incremental plane differs from a rebuild at step "+
                std::to_string(step)+" for "+f.world.unit(id)->type->id);
    }
    const auto stats=f.world.legionStats();
    std::printf("planeincremental %s refreshes=%llu relabels=%llu\n",serial?"serial":"workers",
        (unsigned long long)stats.planeRefreshes,(unsigned long long)stats.planeRelabels);
    (void)builds0;
    check(stats.planeRefreshes>50,"churn never exercised the incremental path");
    return {stats.planeRefreshes,stats.planeRelabels,f.world.stateHash()};
}
void planeincremental() {
    for(uint32_t seed:{987654321u,7u,42u,2026u})
        check(planeincrementalSeed(seed,true)==planeincrementalSeed(seed,false),
              "plane refresh serial and workers differ (seed "+std::to_string(seed)+")");
}
}

namespace {
// Perf H: static refreshes read structures through a spatial index (buckets by footprint origin). A base-heavy
// map whose structures straddle bucket edges, sit on the map's edges and are large and small, under feature and
// structure churn while movers route over it and a new class's plane builds in the background (its pending
// rectangles applied after): with the index the run must equal the reference walk of every structure -- state
// hash, plane counters and every work class -- serial and with workers, and every plane must equal a whole
// rebuild (planeMatchesRebuild rebuilds by the walk). `odd`: a map that is no multiple of the bucket size, and
// footprints wider and deeper than a bucket on its far edges (a query margin clipped to the small kinds passes the
// plain run but not this one).
struct StaticIndexRun {
    uint64_t hash=0,refreshes=0,relabels=0,builds=0;std::array<uint64_t,size_t(NavWork::Count)> work{};
    bool operator==(const StaticIndexRun&) const=default;
};
StaticIndexRun staticindexRun(bool index,bool serial,bool odd) {
    LegionNavigator::setStructureIndexForTest(index);
    const int W=odd?157:160,H=odd?121:128;
    std::vector<uint8_t> heights(size_t(W)*H);
    for(int z=0;z<H;++z)for(int x=0;x<W;++x)heights[size_t(z)*W+x]=uint8_t(100+(x*5+z*3)%30);
    Fixture f(W,H,serial,heights);
    for(int i=0;i<30;++i)f.wall(60+i,64);
    f.publish();
    std::vector<UnitType> kinds;
    for(const auto& [fx,fz]:std::vector<std::pair<int,int>>{{1,1},{2,2},{3,2},{4,4},{6,5},{2,5}}) {
        UnitType t{};t.id=t.name="legion-struct-"+std::to_string(fx)+"x"+std::to_string(fz);
        t.maxHp=500;t.footX=fx;t.footZ=fz;t.buildTime=1;t.sight=64;t.maxVel=Fixed();   // a structure (isStructure)
        kinds.push_back(t);
    }
    kinds[3].yardMap="o..ooooo..oooooo";   // open cells inside a 4x4
    if(odd)for(const auto& [fx,fz]:std::vector<std::pair<int,int>>{{20,3},{3,18},{17,17}}) {
        UnitType t=kinds[0];t.footX=fx;t.footZ=fz;t.id=t.name="legion-struct-"+std::to_string(fx)+"x"+std::to_string(fz);
        kinds.push_back(t);
    }
    std::vector<UnitType> movers;
    for(int foot=1;foot<=3;++foot) {auto t=mover(foot);t.maxSlope=10+foot*5;movers.push_back(t);}
    auto late=mover(2);late.footX=3;late.id=late.name="legion-late";   // its plane builds during the churn
    uint32_t r=20261030u;
    auto rnd=[&](int n) {r=r*1103515245u+12345u;return int((r>>8)%uint32_t(n));};
    std::vector<int> structures;
    auto build=[&](int cx,int cz) {
        const UnitType& t=kinds[size_t(rnd(int(kinds.size())))];
        const int id=f.world.spawn(&t,float(cx*16+t.footX*8),float(cz*16+t.footZ*8),std::nullopt,rnd(2));
        if(id>0)structures.push_back(id);
    };
    // Bases straddling the 16-cell bucket edges, and a row along each map edge.
    for(int b=0;b<4;++b)for(int j=0;j<6;++j)for(int i=0;i<6;++i)build(10+b*36+i*7-(i>2?4:0),12+j*7+(b%2)*40);
    for(int i=0;i<12;++i) {build(i*13,0);build(0,i*10);build(W-6,i*10);build(i*13,H-5);}
    if(odd)for(int i=0;i<8;++i) {build(W-1-i,H-1-2*i);build(i*19,H-1);build(W-1,i*15);}
    std::vector<int> ids;
    for(auto& t:movers)for(int k=0;k<6;++k)ids.push_back(f.spawn(t,20+k*4,110,0));
    f.start();
    auto* legion=f.world.legionNavigator();
    check(legion!=nullptr,"legion navigator not created in Legion mode");
    for(size_t k=0;k<ids.size();++k)f.world.order(ids[k],(20+int(k%6)*20)*16,(8+int(k%4)*30)*16,false);
    int lateId=0;
    for(int step=0;step<360;++step) {
        for(int c=0,n=1+rnd(4);c<n;++c) {
            const int fx=1+rnd(2),fz=1+rnd(2),x=2+rnd(W-6),z=2+rnd(H-6);
            f.world.addFeature(500000+step*8+c,float(x*16+fx*8),float(z*16+fz*8),0,fx,fz,rnd(4)!=0,-1,true);
        }
        if(step%5==4&&!structures.empty()) {
            const size_t i=size_t(rnd(int(structures.size())));
            f.world.destroy(structures[i]);structures.erase(structures.begin()+long(i));
            build(2+rnd(W-10),2+rnd(H-10));
        }
        if(step==20)lateId=f.spawn(late,140,110,0);
        f.world.tick(1.f/30);
        if(step%45==44)for(int id:ids)if(const Unit* u=f.world.unit(id))
            check(legion->planeMatchesRebuild(*u),"plane differs from the reference rebuild at step "+std::to_string(step));
    }
    if(const Unit* u=f.world.unit(lateId))check(legion->planeMatchesRebuild(*u),"late plane differs from the reference rebuild");
    StaticIndexRun out;
    const auto s=f.world.legionStats();
    out.hash=f.world.stateHash();out.refreshes=s.planeRefreshes;out.relabels=s.planeRelabels;out.builds=s.planeBuilds;
    out.work=legion->accounting().total;
    std::printf("staticindex index=%d %s hash=%016llx refreshes=%llu relabels=%llu builds=%llu connectivity=%llu\n",index?1:0,
        serial?"serial":"workers",(unsigned long long)out.hash,(unsigned long long)out.refreshes,(unsigned long long)out.relabels,
        (unsigned long long)out.builds,(unsigned long long)out.work[size_t(NavWork::Connectivity)]);
    LegionNavigator::setStructureIndexForTest(true);
    check(out.refreshes>50,"the churn never exercised the incremental refresh");
    return out;
}
void staticindex() {
    for(const bool odd:{false,true}) {
        const auto ref=staticindexRun(false,true,odd);
        check(staticindexRun(true,true,odd)==ref,"structure index run differs from the reference walk (serial)");
        check(staticindexRun(true,false,odd)==ref,"structure index run differs from the reference walk (workers)");
        check(staticindexRun(false,false,odd)==ref,"reference walk serial and workers differ");
    }
}
}

namespace {
void planeprebuild() {
    // Planes of classes already on the map build in the background, a
    // bounded slice per tick, while features change under them; the result
    // must equal a whole-map rebuild, and no synchronous build may happen.
    const int W=384,H=320;
    std::vector<uint8_t> heights(size_t(W)*H);
    for(int z=0;z<H;++z)for(int x=0;x<W;++x)
        heights[size_t(z)*W+x]=uint8_t(std::clamp(100+int((x*7+z*3)%40)-(x>260&&z>200?70:0),0,255));
    Fixture f(W,H,true,heights);
    for(int i=0;i<300;++i)f.wall(10+i,120+(i%3));
    f.publish();
    std::vector<UnitType> types;
    for(int foot=1;foot<=4;++foot) {auto t=mover(foot);t.maxSlope=8+foot*4;types.push_back(t);}
    types.push_back(mover(2));types.back().footX=3;types.back().id=types.back().name="legion-rect";
    std::vector<int> ids;
    for(auto& t:types)ids.push_back(f.spawn(t,2,2));
    f.start();
    auto* legion=f.world.legionNavigator();
    check(legion!=nullptr,"legion navigator not created in Legion mode");
    uint32_t r=24680u;
    auto rnd=[&](int n) {r=r*1103515245u+12345u;return int((r>>8)%uint32_t(n));};
    for(int step=0;step<400;++step) {
        if(step%2==0) {
            const int fx=1+rnd(3),fz=1+rnd(3),x=4+rnd(W-10),z=4+rnd(H-10);
            f.world.addFeature(z*W+x,float(x*16+fx*8),float(z*16+fz*8),0,fx,fz,rnd(3)!=0,-1,true);
        }
        if(step%37==0) {
            const int at=6+rnd(H-12),blocks=rnd(2);
            for(int x=2;x<W-2;++x)f.world.addFeature(at*W+x,float(x*16+8),float(at*16+8),0,1,1,blocks,-1,true);
        }
        f.world.tick(1.f/30);
    }
    const uint64_t built=f.world.legionStats().planeBuilds;
    check(built==types.size(),"expected one background build per class, got "+std::to_string(built));
    for(int id:ids)check(legion->planeMatchesRebuild(*f.world.unit(id)),"background plane differs from a rebuild for "+
        f.world.unit(id)->type->id);
    std::printf("planeprebuild built=%llu\n",(unsigned long long)built);
}
}

namespace {
void penstale() {
    // A static change in one walled region leaves another region's field
    // valid (it cannot reach any step inside it); a change inside the
    // region stales the field and it is rebuilt.
    constexpr int P=40;
    Fixture f(2*P+8,P+8);
    auto pen=[&](int x,int z) {f.rect(x,z,P,1);f.rect(x,z+P-1,P,1);f.rect(x,z,1,P);f.rect(x+P-1,z,1,P);};
    pen(0,0);pen(P+4,0);
    f.publish();
    const auto type=mover(2);
    const int id=f.spawn(type,4,P/2);
    f.start();
    f.world.order(id,float((P-5)*16),float((P/2)*16),false);
    for(int t=0;t<20;++t)f.world.tick(1.f/30);
    const auto before=f.world.legionStats();
    check(before.fieldsBuilt>=1,"no field for the penned order");
    for(int t=0;t<40;++t) {
        const int x=P+4+10+t%8,z=10;
        f.world.addFeature(z*f.width+x,float(x*16+8),float(z*16+8),0,1,1,t%2==0,-1,true);
        f.world.tick(1.f/30);
    }
    const auto other=f.world.legionStats();
    check(other.planeRefreshes>before.planeRefreshes,"the other region's changes never reached the plane");
    check(other.fieldsBuilt==before.fieldsBuilt,"a change in another region rebuilt the field");
    check(!f.world.unit(id)->orders.empty(),"arrived before the in-region change");
    f.world.addFeature(30*f.width+20,float(20*16+8),float(30*16+8),0,1,1,true,-1,true);
    for(int t=0;t<10;++t)f.world.tick(1.f/30);
    const auto inside=f.world.legionStats();
    check(inside.fieldsBuilt>other.fieldsBuilt,"a change inside the region did not refresh its field");
    std::printf("penstale fields=%llu/%llu/%llu refreshes=%llu\n",(unsigned long long)before.fieldsBuilt,
        (unsigned long long)other.fieldsBuilt,(unsigned long long)inside.fieldsBuilt,(unsigned long long)inside.planeRefreshes);
}
}

// ---- boats and hovercraft ------------------------------------------------
// Legion plans boats (Domain::Water) and hovercraft (Domain::Hover) on the
// same footprint-origin plane, built from mobilePlacement's own predicate
// with the type's water-depth and slope limits. Sea level is 64 in these
// fixtures; a height below it is water that deep.
namespace {
UnitType boat(int foot,int minDepth=13) {
    auto t=mover(foot);t.id=t.name="legion-boat-"+std::to_string(foot)+"-"+std::to_string(minDepth);
    t.domain=UnitType::Domain::Water;t.floater=true;t.minWaterDepth=minDepth;t.maxWaterDepth=10000;
    // Ships specify only a travel turn rate.
    t.turnRate=900;t.turnInPlaceRate=0;
    return t;
}
UnitType hover(int foot) {
    auto t=mover(foot);t.id=t.name="legion-hover-"+std::to_string(foot);
    t.domain=UnitType::Domain::Hover;t.maxSlope=10;t.maxWaterSlope=24;
    return t;
}
Weapon gun() {
    Weapon weapon;weapon.name="legion-naval-gun";weapon.range=120;weapon.damage=100;
    weapon.reload=0.1f;weapon.aimTol=32767;return weapon;
}
// An archipelago: deep sea (height 20), an island with a shallow shelf (56:
// too shallow for a 13-deep keel, fine for hover) and jagged cliffs, a land
// barrier east of it pierced by one narrow strait, a river mouth cut into the
// north shore, and a gentle beach on the south shore.
constexpr int kSeaW=176,kSeaH=104;
std::vector<uint8_t> archipelago() {
    std::vector<uint8_t> h(size_t(kSeaW)*kSeaH,20);
    auto at=[&](int x,int z)->uint8_t&{return h[size_t(z)*kSeaW+x];};
    for(int z=0;z<kSeaH;++z)for(int x=0;x<kSeaW;++x) {
        const int dx=x-70,dz=z-52,d2=dx*dx+dz*dz;
        if(d2<=14*14)at(x,z)=uint8_t(120+(x*5+z*3)%9);     // island, jagged cliffs
        else if(d2<=19*19)at(x,z)=56;                       // shelf
        // North shore with a river mouth (x 30..37) running inland.
        if(z<10&&!(x>=30&&x<=37))at(x,z)=110;
        // South beach: an 8-per-cell ramp from the sea to land (z 74..85),
        // leaving a 6-cell channel south of the island's shelf. (Its bottom
        // rows are off the map: retailMapBoundary cuts high southern land.)
        if(z>=74)at(x,z)=uint8_t(std::min(110,20+(z-74)*8));
        // Barrier at x 116..123, sea to land, with a strait z 47..56 (10
        // cells): the only water between the two seas.
        if(x>=116&&x<=123&&(z<47||z>56))at(x,z)=110;
    }
    return h;
}
}

namespace {
void navalclearance() {
    // Boats with several keel depths and footprints, hovercraft with slope
    // limits: the plane equals mobilePlacement at every origin, on shores,
    // shelves, the strait, the river mouth and the beach; and after random
    // static churn (piers, blocking features, docks with yards) the
    // incremental plane equals a whole-map rebuild.
    Fixture f(kSeaW,kSeaH,true,archipelago());
    for(int i=0;i<20;++i)f.wall(40+i,30+(i%3));            // a jagged reef of features
    f.publish();
    std::vector<UnitType> types;
    types.push_back(boat(2));types.push_back(boat(3,4));types.push_back(boat(4,30));types.push_back(boat(3,13));
    types.back().footZ=2;types.back().id=types.back().name="legion-boat-rect";
    types.push_back(hover(2));types.push_back(hover(3));types.back().maxWaterSlope=6;
    std::vector<int> ids;
    for(size_t i=0;i<types.size();++i)ids.push_back(f.spawn(types[i],6,40+int(i)*6));
    f.start();
    auto* legion=f.world.legionNavigator();
    check(legion!=nullptr,"legion navigator not created in Legion mode");
    for(int id:ids)check(legion->mission(*f.world.unit(id))==LegionMission::None,"idle unit has a mission");
    for(int id:ids) {
        f.world.order(id,float(150*16),float(52*16),false);
        check(legion->mission(*f.world.unit(id))==LegionMission::Move,"Legion does not route "+f.world.unit(id)->type->id);
    }
    f.world.tick(1.f/30);
    uint64_t compared=0,legalCount=0;
    for(int id:ids) {
        const auto& u=*f.world.unit(id);
        uint64_t mine=0;
        for(int z=-1;z<=kSeaH;++z)for(int x=-1;x<=kSeaW;++x) {
            bool nearBody=false;
            for(int other:ids)if(other!=id) {
                const auto& o=*f.world.unit(other);
                const int ox=footprintOrigin(o.x,o.type->footX),oz=footprintOrigin(o.z,o.type->footZ);
                if(x<ox+o.type->footX&&x+u.type->footX>ox&&z<oz+o.type->footZ&&z+u.type->footZ>oz)nearBody=true;
            }
            if(nearBody)continue;
            const bool a=legion->staticLegal(u,x,z),b=f.world.mobilePlacement(u,x,z,false);
            if(a!=b)throw std::runtime_error("plane differs from mobilePlacement for "+u.type->id+" at "+
                std::to_string(x)+","+std::to_string(z)+" plane="+std::to_string(a));
            ++compared;legalCount+=a;mine+=a;
        }
        check(mine>500&&mine<uint64_t(kSeaW)*kSeaH*9/10,"degenerate plane for "+u.type->id);
    }
    // Spot checks of the domains themselves.
    const auto& keel=*f.world.unit(ids[0]);const auto& skim=*f.world.unit(ids[4]);
    check(!legion->staticLegal(keel,70,52),"island interior is land");
    check(legion->staticLegal(skim,70,52)&&!legion->staticLegal(skim,70,52-15),"hover: the island top is open, its cliff is not");
    check(!legion->staticLegal(keel,70,35)&&legion->staticLegal(skim,70,35),"shelf: too shallow for a keel, fine for hover");
    check(legion->staticLegal(keel,119,51)&&legion->staticLegal(keel,10,60),"strait and open sea are navigable");
    check(legion->staticLegal(skim,20,88)&&legion->staticLegal(skim,20,80),"hover climbs the beach");
    check(!legion->staticLegal(keel,20,88)&&!legion->staticLegal(keel,20,80),"boat stays off the beach");
    // Static churn: piers (blocking features), buoys (non-blocking) and
    // docks with yards. The incremental plane must equal a rebuild.
    UnitType yard{};yard.id=yard.name="legion-dock";yard.maxHp=100;yard.footX=4;yard.footZ=3;yard.buildTime=1;
    yard.yardMap="o..oo..ooooo";
    uint32_t r=97531u;
    auto rnd=[&](int n) {r=r*1103515245u+12345u;return int((r>>8)%uint32_t(n));};
    std::vector<int> structures;
    for(int step=0;step<240;++step) {
        const int kind=rnd(10);
        if(kind<6) {
            const int fx=1+rnd(3),fz=1+rnd(3),x=4+rnd(kSeaW-10),z=4+rnd(kSeaH-10);
            f.world.addFeature(z*kSeaW+x,float(x*16+fx*8),float(z*16+fz*8),0,fx,fz,rnd(3)!=0,-1,true);
        } else if(kind<7) {
            const int x=6+rnd(kSeaW-14),z=6+rnd(kSeaH-14);
            structures.push_back(f.world.spawn(&yard,float(x*16+32),float(z*16+24),std::nullopt,1));
        } else if(kind<8&&!structures.empty()) {
            const size_t i=size_t(rnd(int(structures.size())));
            f.world.destroy(structures[i]);structures.erase(structures.begin()+long(i));
        } else f.world.tick(1.f/30);
        if(step%3==0||kind>=8)for(int id:ids)
            check(legion->planeMatchesRebuild(*f.world.unit(id)),"incremental naval plane differs from a rebuild at step "+
                std::to_string(step)+" for "+f.world.unit(id)->type->id);
    }
    std::printf("navalclearance compared=%llu legal=%llu refreshes=%llu\n",(unsigned long long)compared,
        (unsigned long long)legalCount,(unsigned long long)f.world.legionStats().planeRefreshes);
}

// A fleet crosses the archipelago: around the island and its shelf, then
// through the 10-cell strait, to a point beyond it. Every body stays legal
// every tick, Legion serves the move, everyone arrives, no spinning, and
// serial == workers.
uint64_t navalislandRun(bool serial,int foot,int count,bool print) {
    Fixture f(kSeaW,kSeaH,serial,archipelago());
    f.publish();
    const auto type=boat(foot);
    std::vector<int> ids;
    for(int i=0;i<count;++i)ids.push_back(f.spawn(type,6+(i%4)*(foot+1),34+(i/4)*(foot+1)));
    f.start();
    auto* legion=f.world.legionNavigator();
    for(size_t i=0;i<ids.size();++i)f.world.order(ids[i],float(150*16),float(52*16),false);
    for(int id:ids)check(legion->mission(*f.world.unit(id))==LegionMission::Move,"Legion does not route the fleet");
    Watch motion;int last=-1;
    std::map<int,bool> passedStrait;
    for(int t=0;t<9000;++t) {
        f.world.tick(1.f/30);
        motion.observe(f.world,ids);
        bool all=true;
        for(int id:ids) {
            check(f.legal(id),"boat "+std::to_string(id)+" on an illegal origin at tick "+std::to_string(t));
            const auto& u=*f.world.unit(id);
            if(u.x>Fixed::fromInt(116*16)&&u.x<Fixed::fromInt(124*16))passedStrait[id]=true;
            all&=u.orders.empty();
        }
        if(all) {last=t;break;}
    }
    int arrived=0,east=0;
    for(int id:ids) {arrived+=f.world.unit(id)->orders.empty();east+=f.world.unit(id)->x>Fixed::fromInt(124*16);}
    if(print)printLeft(f,ids);
    if(print)std::printf("navalisland foot=%d count=%d arrived=%d east=%d strait=%zu tick=%d spins=%llu reversals=%llu hash=%016llx\n",
        foot,count,arrived,east,passedStrait.size(),last,(unsigned long long)motion.spins(),
        (unsigned long long)motion.reversals(),(unsigned long long)f.world.stateHash());
    check(arrived==count&&east==count,"fleet did not cross the strait");
    check(int(passedStrait.size())==count,"a boat bypassed the strait");
    check(motion.spins()==0,"a boat spun in place");
    return f.world.stateHash();
}
void navalisland() {
    const uint64_t a=navalislandRun(true,3,12,true);
    navalislandRun(true,4,8,true);
    navalislandRun(true,2,24,true);
    check(a==navalislandRun(false,3,12,false),"serial and workers differ");
}

// Hovercraft cross the beach from land into the sea and back onto land;
// boats go up a narrow river mouth, and boats sent onto the beach stop at
// the water's edge. Everyone stays legal every tick.
void hovershore() {
    Fixture f(kSeaW,kSeaH,true,archipelago());
    f.publish();
    const auto skim=hover(2);const auto keel=boat(3);
    std::vector<int> hovers,boats;
    for(int i=0;i<10;++i)hovers.push_back(f.spawn(skim,20+(i%5)*3,87+(i/5)*3));
    for(int i=0;i<4;++i)boats.push_back(f.spawn(keel,96+i*5,64));
    f.start();
    auto* legion=f.world.legionNavigator();
    // Out to sea past the island (land -> beach -> water).
    for(int id:hovers)f.world.order(id,float(100*16),float(30*16),false);
    for(int id:hovers)check(legion->mission(*f.world.unit(id))==LegionMission::Move,"Legion does not route hovercraft");
    auto run=[&](const std::vector<int>& ids,int limit) {
        Watch motion;
        for(int t=0;t<limit;++t) {
            f.world.tick(1.f/30);motion.observe(f.world,ids);
            bool all=true;
            for(int id:hovers)check(f.legal(id),"hovercraft "+std::to_string(id)+" on an illegal origin at "+
                std::to_string(f.world.unit(id)->x.toFloat()/16)+","+std::to_string(f.world.unit(id)->z.toFloat()/16)+" tick "+std::to_string(t));
            for(int id:boats)check(f.legal(id),"boat on an illegal origin");
            for(int id:ids)all&=f.world.unit(id)->orders.empty();
            if(all)return std::pair{t,motion.spins()};
        }
        return std::pair{-1,motion.spins()};
    };
    const auto out=run(hovers,6000);
    std::printf("hovershore out tick=%d spins=%llu\n",out.first,(unsigned long long)out.second);
    printLeft(f,hovers);
    check(out.first>=0,"hovercraft did not reach the sea");
    for(int id:hovers)check(f.world.unit(id)->z<Fixed::fromInt(50*16),"hovercraft not at sea");
    // Back onto land up the beach further east; the boats go up the narrow
    // river mouth in the north shore.
    for(int id:hovers)f.world.order(id,float(140*16),float(84*16),false);
    for(int id:boats)f.world.order(id,float(34*16),float(3*16),false);
    for(int id:boats)check(legion->mission(*f.world.unit(id))==LegionMission::Move,"Legion does not route boats");
    std::vector<int> both=hovers;both.insert(both.end(),boats.begin(),boats.end());
    const auto back=run(both,6000);
    printLeft(f,both);
    int landed=0,river=0;
    for(int id:hovers)landed+=f.world.unit(id)->z>Fixed::fromInt(80*16);
    for(int id:boats)river+=f.world.unit(id)->z<Fixed::fromInt(12*16);
    std::printf("hovershore back tick=%d spins=%llu landed=%d river=%d\n",back.first,(unsigned long long)back.second,landed,river);
    check(back.first>=0&&landed==int(hovers.size()),"hovercraft did not land up the beach");
    check(river==int(boats.size()),"boats did not enter the river mouth");
    // Boats sent onto the beach: the goal resolves to the nearest water
    // they can float in (within 24 cells), so they stop at the water's edge.
    for(int id:boats)f.world.order(id,float(100*16),float(88*16),false);
    for(int t=0;t<3600;++t) {
        f.world.tick(1.f/30);
        for(int id:boats)check(f.legal(id),"boat on an illegal origin");
    }
    int held=0;
    for(int id:boats) {
        const auto& u=*f.world.unit(id);
        std::printf("  boat %d at %.1f,%.1f orders=%zu state=%d\n",id,u.x.toFloat()/16,u.z.toFloat()/16,u.orders.size(),legion->unitState(id));
        held+=u.z>Fixed::fromInt(66*16)&&std::abs((u.x-Fixed::fromInt(100*16)).toFloat())<24*16&&u.speed==Fixed();
    }
    std::printf("hovershore beach held=%d\n",held);
    check(held==int(boats.size()),"boats sent onto the beach did not stop at the water's edge");
    // A mixed selection (ground units, hovercraft, boats) ordered to one sea
    // point just off the beach: one group per class sharing the point's
    // area; boats and hovercraft arrive, ground units stop at the shore.
    auto foot=mover(2);foot.maxWaterDepth=4;
    std::vector<int> walkers;
    // Spawned clear of the hovercraft that landed up the beach in the phase before (z up to ~88: a walker
    // spawned at z 88 overlapped one where stage 8's arrivals land them, an illegal origin from its first tick).
    for(int i=0;i<4;++i)walkers.push_back(f.spawn(foot,130+i*3,94));
    for(int t=0;t<2;++t)f.world.tick(1.f/30);
    std::vector<int> mixed=walkers;mixed.insert(mixed.end(),hovers.begin(),hovers.begin()+4);
    mixed.insert(mixed.end(),boats.begin(),boats.end());
    for(int id:mixed)f.world.order(id,float(100*16),float(68*16),false);
    for(int t=0;t<3600;++t) {
        f.world.tick(1.f/30);
        for(int id:mixed)check(f.legal(id),"mixed selection: illegal origin for "+std::to_string(id));
    }
    int done=0,ashore=0;
    for(int id:mixed) {
        const auto& u=*f.world.unit(id);done+=u.orders.empty()&&u.speed==Fixed();
        if(std::find(walkers.begin(),walkers.end(),id)!=walkers.end())ashore+=u.z>Fixed::fromInt(74*16);
    }
    printLeft(f,mixed);
    std::printf("hovershore mixed done=%d/%zu ashore=%d\n",done,mixed.size(),ashore);
    check(done==int(mixed.size())&&ashore==int(walkers.size()),"mixed selection did not settle by class");
    check(out.second==0&&back.second==0,"a hovercraft spun in place");
}

// The mission families on water: patrol laps around the island, fight-move
// across it, an attack approach around it and a guard escort. Each must be
// served by Legion and do its job.
void navalmissions() {
    auto armed=[] {auto t=boat(3);t.weapon=gun();t.weapons.push_back(t.weapon);return t;};
    {   // Patrol: around the island and back, several laps.
        Fixture f(kSeaW,kSeaH,true,archipelago());f.publish();
        const auto t=boat(3);
        std::vector<int> ids;for(int i=0;i<6;++i)ids.push_back(f.spawn(t,30+(i%3)*4,48+(i/3)*4));
        f.start();
        for(int id:ids)f.world.patrol(id,float(104*16),float(52*16));
        std::vector<int> laps(ids.size(),0);std::vector<bool> out(ids.size(),false);bool legion=false;
        for(int n=0;n<9000;++n) {
            f.world.tick(1.f/30);
            for(size_t k=0;k<ids.size();++k) {
                const auto& u=*f.world.unit(ids[k]);check(f.legal(ids[k]),"patrol boat illegal");
                legion|=f.world.legionNavigator()->mission(u)==LegionMission::Patrol;
                // A lap: the outbound leg ends (the current goal flips from
                // the patrol point back to the start) east of the island.
                const bool outbound=u.orders[World::currentLeg(u.orders)].x>Fixed::fromInt(80*16);
                if(out[k]&&!outbound&&u.x>Fixed::fromInt(80*16))++laps[k];
                out[k]=outbound;
            }
        }
        const int least=*std::min_element(laps.begin(),laps.end());
        for(size_t k=0;k<ids.size();++k) {
            const auto& u=*f.world.unit(ids[k]);
            std::printf("  patrol %d at %.1f,%.1f laps=%d orders=%zu state=%d\n",ids[k],u.x.toFloat()/16,u.z.toFloat()/16,
                laps[k],u.orders.size(),f.world.legionNavigator()->unitState(ids[k]));
        }
        std::printf("navalmissions patrol least=%d legion=%d\n",least,legion);
        check(legion&&least>=2,"boat patrol around the island");
    }
    {   // Fight-move across the island.
        Fixture f(kSeaW,kSeaH,true,archipelago());f.publish();
        const auto t=armed();
        std::vector<int> ids;for(int i=0;i<6;++i)ids.push_back(f.spawn(t,30+(i%3)*4,48+(i/3)*4));
        f.start();
        for(int id:ids)f.world.attackMove(id,float(104*16),float(52*16),false);
        bool legion=false;int done=-1;
        for(int n=0;n<6000&&done<0;++n) {
            f.world.tick(1.f/30);bool all=true;
            for(int id:ids) {
                const auto& u=*f.world.unit(id);check(f.legal(id),"fight-move boat illegal");
                legion|=f.world.legionNavigator()->mission(u)==LegionMission::Fight;all&=u.orders.empty();
            }
            if(all)done=n;
        }
        std::printf("navalmissions fight done=%d legion=%d\n",done,legion);
        check(legion&&done>=0,"boat fight-move across the island");
    }
    {   // Attack approach: the target sits behind the island.
        Fixture f(kSeaW,kSeaH,true,archipelago());f.publish();
        const auto t=armed();auto target=boat(3);target.maxVel=Fixed();
        const int id=f.spawn(t,40,52),enemy=f.spawn(target,100,52,1);
        f.start();
        f.world.attack(id,enemy,false);
        bool legion=false;int killed=-1;
        for(int n=0;n<6000&&killed<0;++n) {
            f.world.tick(1.f/30);
            legion|=f.world.legionNavigator()->mission(*f.world.unit(id))==LegionMission::Attack;
            check(f.legal(id),"attacking boat illegal");
            const auto* e=f.world.unit(enemy);if(!e||!e->alive())killed=n;
        }
        std::printf("navalmissions attack killed=%d legion=%d\n",killed,legion);
        check(legion&&killed>=0,"boat attack approach around the island");
    }
    {   // Guard: an escort follows its charge around the island.
        Fixture f(kSeaW,kSeaH,true,archipelago());f.publish();
        const auto t=boat(3);
        const int charge=f.spawn(t,40,52),escort=f.spawn(t,34,44);
        f.start();
        f.world.guard(escort,charge,false);f.world.order(charge,float(104*16),float(52*16),false);
        bool legion=false;
        for(int n=0;n<6000;++n) {
            f.world.tick(1.f/30);check(f.legal(escort),"escort illegal");
            legion|=f.world.legionNavigator()->mission(*f.world.unit(escort))==LegionMission::Guard;
        }
        const auto& c=*f.world.unit(charge);const auto& e=*f.world.unit(escort);
        const float d=fxLen(c.x-e.x,c.z-e.z).toFloat();
        std::printf("navalmissions guard dist=%.1f legion=%d\n",d,legion);
        // Follow_Ground's ring for a 3x3 escort of a 3x3 charge: 128..176 px
        // (origin cells, so up to a cell's rounding past either edge).
        check(legion&&c.orders.empty()&&d>=112.f&&d<=192.f&&!e.orders.empty()&&e.orders.front().guard,"boat guard escort");
    }
}
// A control formation (Alt+N: squad -N) commanded through the real command
// path (Cmd::SetSquad, then one shared-point Cmd::Move per member, as the
// HUD issues it) across rocks and through a narrow gap. A formation is wider
// than any fixed straggler radius, and it arrives one by one through the gap:
// every member must settle legally near the point and stay settled (no
// endless rejoin re-orders), with no spin.
struct SquadResult {int lastOrdered=0;float far=0;};
SquadResult squadRun() {
    Fixture f(220,100);
    f.rect(96,0,4,46);f.rect(96,52,4,48);           // a wall with a 6-cell gap
    f.rect(150,38,3,3);f.rect(168,58,4,2);f.rect(140,60,2,5);f.rect(175,40,2,6); // rocks
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<120;++i)ids.push_back(f.spawn(type,14+(i%12)*3,32+(i/12)*4));
    f.start();
    TypeRegistry registry;
    auto command=[&](tak::net::Cmd kind,int id,int target,float x,float z) {
        tak::net::Command c;c.kind=kind;c.player=0;c.unitId=id;c.targetId=target;c.x=x;c.z=z;c.queue=0;
        applyCommand(f.world,registry,c);
    };
    for(int id:ids)command(tak::net::Cmd::SetSquad,id,-1,0,0);
    for(int t=0;t<600;++t)f.world.tick(1.f/30);      // assigning the formation gathers it
    const float px=160*16,pz=50*16;
    for(int id:ids)command(tak::net::Cmd::Move,id,0,px,pz);
    Watch motion;
    {
        tak::legion_observe::Group g;g.name="squad";g.ids=ids;g.clickX=int(px);g.clickZ=int(pz);
        motion.cfg.groups.push_back(g);
    }
    std::map<int,int> reorders;std::map<int,bool> idle;
    int lastOrdered=-1;
    constexpr int kTicks=12000;
    for(int t=0;t<kTicks;++t) {
        f.world.tick(1.f/30);
        motion.observe(f.world,ids);
        if(t%30==0)checkClaims(f.world,"squadformation");
        for(int id:ids) {
            const auto& u=*f.world.unit(id);
            check(f.legal(id),"illegal footprint in a commanded formation");
            const bool now=u.orders.empty();
            if(idle.count(id)&&idle[id]&&!now) {++reorders[id];lastOrdered=t;}
            if(!now)lastOrdered=std::max(lastOrdered,t);
            idle[id]=now;
        }
    }
    int settled=0,maxReorders=0;float far=0;
    for(int id:ids) {
        const auto& u=*f.world.unit(id);
        settled+=u.orders.empty()&&u.speed==Fixed();
        maxReorders=std::max(maxReorders,reorders[id]);
        far=std::max(far,fxLen(u.x-Fixed::fromFloat(px),u.z-Fixed::fromFloat(pz)).toFloat());
    }
    printLeft(f,ids);
    std::printf("squadformation settled=%d/%zu last_ordered=%d max_reorders=%d far=%.0f spins=%llu reversals=%llu %s\n",settled,ids.size(),
        lastOrdered,maxReorders,far,(unsigned long long)motion.spins(),(unsigned long long)motion.reversals(),completionKeys(motion.keys(),"squad").c_str());
    // Offset +1 leaves one member unsettled for good (AR-01, the destination tail);
    // offset 0, the original fixture, must settle everyone.
    if(g_shift==0)check(settled==int(ids.size()),"formation members never settled");
    check(maxReorders<=1,"a settled formation member was re-ordered repeatedly");
    check(motion.spins()==0,"formation members turned in place");
    return {lastOrdered,far};
}
// Bounds are the median-of-5 start offsets of the committed build (W0 baseline)
// with a one-sided 1.20 band (the spread over the offsets is 2.0x and 1.33x).
// Offsets 0 / +1 / -1 / +2 / -2: last_ordered 5980 / 11999 (one member never
// settles: AR-01) / 8773 / 11426 / 6668, far 460 / 352 / 380 / 359 / 347. The old
// bounds were 9000 ticks (kTicks-3000) and the native formation radius 701
// (2*sqrt(area) cells), measured at offset 0 only, 1.5x from its values.
constexpr int kSquadLastOrdered=8773;
constexpr float kSquadFar=359.f;
constexpr double kSquadLastBand=1.20,kSquadFarBand=1.20;
void squadformation() {
    int lv[5];float fv[5];
    for(int i=0;i<5;++i) {g_shift=kShifts[i];const auto r=squadRun();lv[i]=r.lastOrdered;fv[i]=r.far;}
    g_shift=0;
    const int last=medianOf(lv);const float far=medianOf(fv);
    std::printf("squadformation medians: last_ordered=%d far=%.0f\n",last,far);
    check(last<=kSquadLastOrdered*kSquadLastBand,"formation members kept being re-ordered");
    // Near its spot: no farther than the baseline from the point (the native
    // formation radius is 701 px).
    check(far<=kSquadFar*kSquadFarBand,"a formation member settled far from the point");
}
// The formation re-form (World::legionStandbyCheck, our Legion counterpart of
// Retail's standby group check). An idle member of an Alt+N formation shoved
// out of its slot -- farther than Retail's tolerance (2*sqrt(area) cells, 9.8
// for six 2x2 bodies) but inside the W6 rejoin's reach -- walks back to its own
// slot at a standby wake; a Ctrl group member shoved the same way stays where
// it was left. Then the slot is walled off with the member shoved outside the
// wall: it walks once, gives the slot up and is never re-ordered again. A
// member whose slot a body stands on waits (no walk) until the body leaves. A
// member moved on its own keeps its slot and walks back to it, as Retail pulls
// it back to its group. A formation stopped short of its point keeps no slot to
// walk to or back to.
void formationreform() {
    Fixture f(128,112);
    f.publish();
    const auto type=mover(2);
    std::vector<int> form,ctrl;
    for(int i=0;i<6;++i)form.push_back(f.spawn(type,10+(i%3)*3,20+(i/3)*3));
    for(int i=0;i<6;++i)ctrl.push_back(f.spawn(type,10+(i%3)*3,80+(i/3)*3));
    f.start();
    for(int id:form)f.world.setSquad(id,-1);
    for(int id:ctrl)f.world.setSquad(id,2);
    for(int id:form)f.world.order(id,40*16,24*16,false);
    for(int id:ctrl)f.world.order(id,40*16,84*16,false);
    for(int t=0;t<1800;++t)f.world.tick(1.f/30);
    for(int id:form)check(f.world.unit(id)->orders.empty(),"formationreform: the formation never settled");
    for(int id:ctrl)check(f.world.unit(id)->orders.empty(),"formationreform: the Ctrl group never settled");
    auto east=[&](const std::vector<int>& ids) {
        return *std::max_element(ids.begin(),ids.end(),[&](int a,int b){return f.world.unit(a)->x<f.world.unit(b)->x;});
    };
    auto shove=[&](int id,int cells) {
        Unit& u=*f.world.unit(id);const Fixed x=u.x;
        for(int i=0;i<cells*4;++i)f.world.commitGroundStep(u,Fixed::fromInt(4),Fixed());
        check(u.x-x>=Fixed::fromInt(cells*16-8),"formationreform: the shove was refused");
    };
    // Runs `ticks` ticks and returns how many times the unit went from idle to carrying orders.
    auto watch=[&](int id,int ticks) {
        int orders=0;bool idle=f.world.unit(id)->orders.empty();
        for(int t=0;t<ticks;++t) {
            f.world.tick(1.f/30);
            const bool now=f.world.unit(id)->orders.empty();
            orders+=idle&&!now;idle=now;
            check(f.legal(id),"formationreform: illegal footprint");
        }
        return orders;
    };
    const int a=east(form),b=east(ctrl);
    const Fixed ax=f.world.unit(a)->x,az=f.world.unit(a)->z,bx0=f.world.unit(b)->x;
    shove(a,12);shove(b,12);
    const Fixed bx=f.world.unit(b)->x,bz=f.world.unit(b)->z;
    int formOrders=0,ctrlOrders=0;
    {
        bool idleA=true,idleB=true;
        for(int t=0;t<900;++t) {
            f.world.tick(1.f/30);
            const bool nowA=f.world.unit(a)->orders.empty(),nowB=f.world.unit(b)->orders.empty();
            formOrders+=idleA&&!nowA;ctrlOrders+=idleB&&!nowB;idleA=nowA;idleB=nowB;
        }
    }
    const auto& ua=*f.world.unit(a);const auto& ub=*f.world.unit(b);
    const float back=fxLen(ua.x-ax,ua.z-az).toFloat();
    std::printf("formationreform shove: member walks=%d back_px=%.1f idle=%d | ctrl walks=%d moved_px=%.1f (shoved %.1f)\n",
        formOrders,back,int(ua.orders.empty()),ctrlOrders,fxLen(ub.x-bx,ub.z-bz).toFloat(),(bx-bx0).toFloat());
    check(formOrders==1,"formationreform: the shoved formation member did not walk back once");
    check(ua.orders.empty()&&back<=32.f,"formationreform: the formation member did not settle back in its slot");
    check(ctrlOrders==0&&ub.x==bx&&ub.z==bz,"formationreform: a Ctrl group member re-formed");
    // A body standing in the slot: the member waits where it was shoved (no walk) until the body leaves.
    shove(a,12);
    const int blocker=f.world.spawn(&type,ax.toFloat(),az.toFloat(),std::nullopt,0);
    check(blocker>0&&f.world.unit(blocker)->x==ax&&f.world.unit(blocker)->z==az,"formationreform: the blocker missed the slot");
    const int waits=watch(a,900);
    std::printf("formationreform occupied: walks=%d\n",waits);
    check(waits==0,"formationreform: a member walked to a slot a body stands on");
    f.world.order(blocker,80*16,60*16,false);
    const int freed=watch(a,1200);
    const float freedBack=fxLen(f.world.unit(a)->x-ax,f.world.unit(a)->z-az).toFloat();
    std::printf("formationreform freed: walks=%d back_px=%.1f\n",freed,freedBack);
    check(freed==1&&freedBack<=32.f,"formationreform: the member did not walk back once its slot was free");
    // Moved on its own: the member is ordered 20 cells east by itself and arrives there. Its slot stays (a
    // formation member's own move is not a formation move): once idle it walks back to it.
    f.world.order(a,float((footprintOrigin(ax,2)+20)*16+16),az.toFloat(),false);
    int soloWalks=0;
    {
        bool idle=false,arrived=false;
        for(int t=0;t<1500;++t) {
            f.world.tick(1.f/30);
            const bool now=f.world.unit(a)->orders.empty();
            if(now&&!arrived&&fxLen(f.world.unit(a)->x-ax,f.world.unit(a)->z-az).toFloat()>=16*18.f)arrived=true;
            soloWalks+=arrived&&idle&&!now;idle=now;
        }
        check(arrived,"formationreform: the solo move never arrived");
    }
    const float soloBack=fxLen(f.world.unit(a)->x-ax,f.world.unit(a)->z-az).toFloat();
    std::printf("formationreform solo: walks_back=%d back_px=%.1f\n",soloWalks,soloBack);
    check(soloWalks==1&&f.world.unit(a)->orders.empty()&&soloBack<=32.f,
          "formationreform: a formation member moved on its own did not walk back to its slot");
    // Stopped short: the formation is sent 60 cells east and stopped a third of the way. No member walks on
    // to the slot it was heading for, nor back to the one it left. Then it is sent home again.
    for(int id:form)f.world.order(id,100*16,24*16,false);
    for(int t=0;t<180;++t)f.world.tick(1.f/30);
    for(int id:form)f.world.stop(id);
    int stopped=0;float away=0;
    for(int id:form)stopped+=watch(id,900);
    away=fxLen(f.world.unit(a)->x-ax,f.world.unit(a)->z-az).toFloat();
    std::printf("formationreform stopped: walks=%d from_old_slot_px=%.1f\n",stopped,away);
    check(away>=16*12.f,"formationreform: the stopped formation did not get far enough to test");
    check(stopped==0,"formationreform: a formation stopped short walked on or back");
    for(int id:form)f.world.order(id,40*16,24*16,false);
    for(int t=0;t<1800;++t)f.world.tick(1.f/30);
    for(int id:form)check(f.world.unit(id)->orders.empty(),"formationreform: the formation never settled again");
    const int e=east(form);
    const Fixed hx=f.world.unit(e)->x,hz=f.world.unit(e)->z;
    // Give up: shove it 16 cells out, wall a ring 14 cells round the point (its slot inside, itself outside).
    shove(e,16);
    for(int i=-14;i<=14;++i) {f.wall(40+i,24-14);f.wall(40+i,24+14);f.wall(40-14,24+i);f.wall(40+14,24+i);}
    f.publish();
    const int tries=watch(e,2400),later=watch(e,1800);
    const auto& ug=*f.world.unit(e);
    std::printf("formationreform walled: walks=%d later=%d idle=%d from_slot_px=%.1f\n",tries,later,int(ug.orders.empty()),
        fxLen(ug.x-hx,ug.z-hz).toFloat());
    check(tries==1,"formationreform: a member whose slot is walled off did not walk exactly once");
    check(later==0&&ug.orders.empty(),"formationreform: a member that gave its slot up was re-ordered");
}
// A Legion formation slot is the member's while it is in the formation: a
// change of owner (514da0: the new object has no squad) drops it at once, and
// resetForReplay clears every slot (hashed state a rejoin's replay from tick 0
// must not start with).
void formationslotreset() {
    Fixture f(128,112);
    f.publish();
    const auto type=mover(2);
    std::vector<int> form;
    for(int i=0;i<6;++i)form.push_back(f.spawn(type,10+(i%3)*3,20+(i/3)*3));
    f.spawn(type,100,90,1);   // the ally to give to
    f.start();
    for(int id:form)f.world.setSquad(id,-1);
    for(int id:form)f.world.order(id,40*16,24*16,false);
    for(int t=0;t<1800;++t)f.world.tick(1.f/30);
    for(int id:form)check(f.world.legionFormationSlot(id)!=nullptr,"formationslotreset: a settled member holds no slot");
    f.world.setTeam(1,0);   // an ally to give to
    check(f.world.giveUnit(form[0],0,1),"formationslotreset: the member could not be given away");
    check(f.world.legionFormationSlot(form[0])==nullptr,"formationslotreset: a member given away kept its formation slot");
    check(f.world.legionFormationSlot(form[1])!=nullptr,"formationslotreset: giving one member away dropped another's slot");
    f.world.resetForReplay();
    for(int id:form)check(f.world.legionFormationSlot(id)==nullptr,"formationslotreset: a formation slot survived resetForReplay");
    std::printf("formationslotreset: ok\n");
}
// The classic AI's squads are Retail's formations while the AI flags them (Cmd::AiGroup, World::aiFormation: a
// strike squad attacking): in Legion a flagged squad's member shoved out of its slot walks back as an Alt+N member
// does, and its member that never moved with it is gathered by the W6 rest-time
// rejoin as an Alt+N formation's is; an unflagged squad's members stay where
// they were left.
void formationreformai() {
    Fixture f(128,112);
    f.publish();
    f.world.player(1).automaticGates=true;
    const auto type=mover(2);
    std::vector<int> flagged,plain;
    for(int i=0;i<6;++i)flagged.push_back(f.spawn(type,10+(i%3)*3,20+(i/3)*3,1));
    for(int i=0;i<6;++i)plain.push_back(f.spawn(type,10+(i%3)*3,80+(i/3)*3,1));
    // A member of each squad that never moves with it (a straggler the squad was given far off).
    const int lone=f.spawn(type,100,60,1),loneP=f.spawn(type,100,100,1);
    f.start();
    for(int id:flagged)f.world.setAiGroup(1,id,23,false);
    for(int id:plain)f.world.setAiGroup(1,id,24,false);
    f.world.setAiGroup(1,lone,23,false);f.world.setAiGroup(1,loneP,24,false);
    f.world.setAiGroup(1,0,23,true);
    check(f.world.aiFormation(1,23)&&!f.world.aiFormation(1,24),"formationreformai: the AI squad flags");
    for(int id:flagged)f.world.order(id,40*16,24*16,false);
    for(int id:plain)f.world.order(id,40*16,84*16,false);
    for(int t=0;t<1800;++t)f.world.tick(1.f/30);
    for(int id:flagged)check(f.world.unit(id)->orders.empty(),"formationreformai: the flagged squad never settled");
    for(int id:plain)check(f.world.unit(id)->orders.empty(),"formationreformai: the plain squad never settled");
    auto east=[&](const std::vector<int>& ids) {
        return *std::max_element(ids.begin(),ids.end(),[&](int a,int b){return f.world.unit(a)->x<f.world.unit(b)->x;});
    };
    const int a=east(flagged),b=east(plain);
    const Fixed ax=f.world.unit(a)->x,az=f.world.unit(a)->z;
    for(int id:{a,b}) {
        Unit& u=*f.world.unit(id);const Fixed x=u.x;
        for(int i=0;i<48;++i)f.world.commitGroundStep(u,Fixed::fromInt(4),Fixed());
        check(u.x-x>=Fixed::fromInt(12*16-8),"formationreformai: the shove was refused");
    }
    const Fixed bx=f.world.unit(b)->x,bz=f.world.unit(b)->z;
    int walksA=0,walksB=0;bool idleA=true,idleB=true;
    for(int t=0;t<900;++t) {
        f.world.tick(1.f/30);
        const bool nowA=f.world.unit(a)->orders.empty(),nowB=f.world.unit(b)->orders.empty();
        walksA+=idleA&&!nowA;walksB+=idleB&&!nowB;idleA=nowA;idleB=nowB;
    }
    const auto& ua=*f.world.unit(a);const auto& ub=*f.world.unit(b);
    const float back=fxLen(ua.x-ax,ua.z-az).toFloat();
    std::printf("formationreformai: flagged walks=%d back_px=%.1f | unflagged walks=%d moved_px=%.1f\n",walksA,back,walksB,
        fxLen(ub.x-bx,ub.z-bz).toFloat());
    check(walksA==1&&ua.orders.empty()&&back<=32.f,"formationreformai: the flagged AI squad member did not walk back");
    check(walksB==0&&ub.x==bx&&ub.z==bz,"formationreformai: an unflagged AI squad member re-formed");
    // The stragglers: the flagged squad's is gathered to its point by the W6 rest-time rejoin, as an Alt+N
    // formation's is; the unflagged squad's stays where it was put.
    const auto& ul=*f.world.unit(lone);const auto& up=*f.world.unit(loneP);
    const float gathered=fxLen(ul.x-Fixed::fromInt(40*16),ul.z-Fixed::fromInt(24*16)).toFloat();
    std::printf("formationreformai stragglers: flagged from_point_px=%.1f idle=%d | unflagged at %.0f,%.0f idle=%d\n",
        gathered,int(ul.orders.empty()),up.x.toFloat()/16,up.z.toFloat()/16,int(up.orders.empty()));
    check(ul.orders.empty()&&gathered<=16*16.f,"formationreformai: the flagged squad's straggler was not gathered");
    check(up.orders.empty()&&up.x==Fixed::fromInt(100*16)&&up.z==Fixed::fromInt(100*16),
          "formationreformai: the unflagged squad's straggler moved");
}
}

// A formation (Alt+1) of 20 ground bodies and 8 flyers given one order.
// Measures whether the flyers ride over the front of the ground while it is
// under way and land in front of it, how far they stray from the ground, whether
// they fly like flyers on the way (keep up with the ground in 30-tick windows,
// face the way they move) and whether they land as soon as their order is done
// (no hovering).
// A landed flyer behind the front of the ground: a ground body within 40 px of its lane (across the way (dx,dz))
// stands ahead of it -- it came down among the ground's slots rather than in front of them.
bool landedBehind(World& w,const Unit& u,const std::vector<int>& ground,float dx,float dz) {
    if(u.flightGroundMode!=1)return false;
    for(int id:ground) {
        const auto& g=*w.unit(id);
        const float ex=g.x.toFloat()-u.x.toFloat(),ez=g.z.toFloat()-u.z.toFloat();
        if(std::abs(ez*dx-ex*dz)<=40.f&&ex*dx+ez*dz>0.f)return true;
    }
    return false;
}
// Over the front (the user's ask: flyers ride over the units in front, not the middle): in each 30-tick window
// the ground marched >= 9 px, along its way d, leadTop is the mean along offset of the 3 leading flyers from the
// ground's leading body (0: over it; the old centroid station sat ~-190), leadAll the mean along offset of all
// flyers from the ground's centroid (the old station: <= 0), and maxOff the farthest an airborne flyer strays
// from its nearest ground body. At the end, along the last window's d: landedBehind counts landed flyers
// with a ground body ahead of it in its lane (they land in front of the ground, not among its slots).
struct MixedRun {int groundDone=-1,flyersDone=-1;float maxAway=0,meanAway=0,endAway=0;uint64_t hash=0;int landedAhead=0;
    int windows=0,stalls=0,moving=0,misaligned=0,maxHover=0,maxLandDelay=0,landed=0;double flown=0,marched=0;
    double leadTop=0,leadAll=0;int frontWindows=0,landedBehind=0;float maxOff=0;int cruising=0,crawling=0;};
MixedRun mixedformationRun(bool legion,tak::net::Cmd kind,bool serial=true) {
    Fixture f(260,100,serial);
    // A wall across the straight line: the ground detours through the gap
    // at its south end, so the formation turns twice on the way.
    f.rect(110,8,4,72);f.publish();
    if(!legion)f.world.setPathfindingMode(PathfindingMode::Retail);
    const auto type=mover(2);
    UnitType flyer{};flyer.id=flyer.name="legion-flyer";
    flyer.canFly=flyer.canMove=true;flyer.maxHp=100;flyer.footX=flyer.footZ=3;flyer.sight=4096;
    flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);flyer.turnRate=1200;flyer.cruiseAlt=80;flyer.buildTime=1;
    flyer.vtolStandby=true;   // defaultmissiontype=VTOL_standby: an idle flyer lands
    std::vector<int> ground,flyers,all;
    for(int i=0;i<20;++i)ground.push_back(f.spawn(type,14+(i%5)*3,40+(i/5)*4));
    for(int i=0;i<8;++i)flyers.push_back(f.spawn(flyer,16+(i%4)*4,58+(i/4)*4));
    all=ground;all.insert(all.end(),flyers.begin(),flyers.end());
    f.start();
    TypeRegistry registry;
    auto command=[&](tak::net::Cmd k,int id,int target,float x,float z) {
        tak::net::Command c;c.kind=k;c.player=0;c.unitId=id;c.targetId=target;c.x=x;c.z=z;c.queue=0;
        applyCommand(f.world,registry,c);
    };
    for(int id:all)command(tak::net::Cmd::SetSquad,id,-1,0,0);
    for(int t=0;t<300;++t)f.world.tick(1.f/30);
    const float px=220*16,pz=50*16;
    // As the move UI does: flyers keep their offset from the selection's
    // centre, clamped to 60 px per axis; Legion surface movers share the point.
    float sx=0,sz=0;
    for(int id:all){sx+=f.world.unit(id)->x.toFloat();sz+=f.world.unit(id)->z.toFloat();}
    sx/=float(all.size());sz/=float(all.size());
    for(int id:all) {
        const auto& u=*f.world.unit(id);
        const bool offset=kind==tak::net::Cmd::Move&&(u.type->canFly||!legion);
        command(kind,id,0,offset?px+std::clamp(u.x.toFloat()-sx,-60.f,60.f):px,
                offset?pz+std::clamp(u.z.toFloat()-sz,-60.f,60.f):pz);
    }
    MixedRun r;Watch motion;double sum=0;int samples=0;
    std::map<int,std::pair<float,float>> windowStart;float windowCx=0,windowCz=0,dirX=1,dirZ=0;
    // The way each flyer has flown (px, summed tick by tick): a formation flyer circling its station (2026-11-06)
    // can end a window where it began, so a stall is judged on the way flown, not the displacement.
    std::map<int,float> path,pathAt;std::map<int,std::pair<float,float>> lastAt;
    std::vector<std::pair<float,float>> groundAt;   // the ground's positions at the window's start
    std::map<int,int> hover,idleAt,hoverRun;
    const int ticks=kind==tak::net::Cmd::Patrol?3000:6000;
    auto centroid=[&](float& cx,float& cz) {
        double sx=0,sz=0;for(int id:ground){sx+=f.world.unit(id)->x.toFloat();sz+=f.world.unit(id)->z.toFloat();}
        cx=float(sx/ground.size());cz=float(sz/ground.size());
    };
    for(int t=0;t<ticks;++t) {
        f.world.tick(1.f/30);motion.observe(f.world,ground);
        // Retail lets a ground unit walk under a descending flyer (507d10
        // takes airborne word +2 as free) and the flyer then lands on it
        // (tools/re/check_landing_moving_ground.py); a formation member's
        // FormationStandby (407779) walks it out again from under the
        // landed flyer. Legion refuses that touchdown (flyerLandingOccupied).
        for(int id:ground) {
            if(f.legal(id))continue;
            const auto& u=*f.world.unit(id);
            const int ux=footprintOrigin(u.x,u.type->footX),uz=footprintOrigin(u.z,u.type->footZ);
            const bool underFlyer=!legion&&std::any_of(flyers.begin(),flyers.end(),[&](int o) {
                const auto& v=*f.world.unit(o);
                const int vx=footprintOrigin(v.x,v.type->footX),vz=footprintOrigin(v.z,v.type->footZ);
                return v.flightGroundMode!=2&&vx<ux+u.type->footX&&ux<vx+v.type->footX&&
                       vz<uz+u.type->footZ&&uz<vz+v.type->footZ;
            });
            check(underFlyer,"illegal footprint in a mixed formation");
        }
        bool groundBusy=false,flyersBusy=false;
        for(int id:ground)groundBusy|=!f.world.unit(id)->orders.empty();
        for(int id:flyers)flyersBusy|=!f.world.unit(id)->orders.empty();
        if(!groundBusy&&r.groundDone<0)r.groundDone=t;
        if(!flyersBusy&&r.flyersDone<0)r.flyersDone=t;
        for(int id:flyers) {
            const auto& u=*f.world.unit(id);
            if(const auto at=lastAt.find(id);at!=lastAt.end())
                path[id]+=std::hypot(u.x.toFloat()-at->second.first,u.z.toFloat()-at->second.second);
            lastAt[id]={u.x.toFloat(),u.z.toFloat()};
            // Crawling: airborne on its way (beyond 300 px of its point) while the ground marches, under half its
            // own speed -- held to the ground's pace, as the stations held it until 2026-11-06.
            if(t>=300&&groundBusy&&u.flightGroundMode==2&&!u.orders.empty()&&
               std::hypot(u.x.toFloat()-u.orders.back().x.toFloat(),u.z.toFloat()-u.orders.back().z.toFloat())>300.f) {
                ++r.cruising;r.crawling+=u.speed.v<u.baseSpeed.v/2;
            }
        }
        if(t%30==0) {
            // Keeping up: over each 30-tick window in which the ground's
            // centroid moved at least 12 px, a travelling flyer covers at least
            // 40% of that distance -- or of the ground's leading body's advance
            // along it, if less: a flyer over a front that has stopped (a
            // patrol's leading bodies turning at its point) holds there. The
            // floor was 9 px until the 2026-11-02 steering: with it the patrol's
            // return leg has one window in which the front crawls 11 px (0.37
            // px a tick, turning at its point) and a flyer holding over it flew
            // 4.1 px against the 4.4 asked (712 windows, one stall); under 0.4
            // px a tick the front is turning, not marching.
            float cx,cz;centroid(cx,cz);
            const float marched=std::hypot(cx-windowCx,cz-windowCz);
            float advance=marched;
            if(marched>0&&groundAt.size()==ground.size()) {
                const float ux=(cx-windowCx)/marched,uz=(cz-windowCz)/marched;
                float was=-1e9f,now=-1e9f;
                for(size_t i=0;i<ground.size();++i) {
                    was=std::max(was,groundAt[i].first*ux+groundAt[i].second*uz);
                    now=std::max(now,f.world.unit(ground[i])->x.toFloat()*ux+f.world.unit(ground[i])->z.toFloat()*uz);
                }
                advance=std::min(marched,now-was);
            }
            groundAt.clear();
            for(int id:ground)groundAt.push_back({f.world.unit(id)->x.toFloat(),f.world.unit(id)->z.toFloat()});
            for(int id:flyers) {
                const auto& u=*f.world.unit(id);
                const auto it=windowStart.find(id);
                // (Its own final approach, inside 300 px of its point, is retail's.)
                const bool travelling=!u.orders.empty()&&std::hypot(u.x.toFloat()-u.orders.back().x.toFloat(),
                    u.z.toFloat()-u.orders.back().z.toFloat())>300.f;
                if(t>=300&&groundBusy&&it!=windowStart.end()&&travelling&&marched>=12.f) {
                    const float flown=std::hypot(u.x.toFloat()-it->second.first,u.z.toFloat()-it->second.second);
                    ++r.windows;r.stalls+=path[id]-pathAt[id]<0.4f*std::max(advance,0.f);r.flown+=flown;r.marched+=marched;
                }
                windowStart[id]={u.x.toFloat(),u.z.toFloat()};pathAt[id]=path[id];
            }
            if(t>=300&&groundBusy&&marched>=9.f) {
                dirX=(cx-windowCx)/marched;dirZ=(cz-windowCz)/marched;
                float front=-1e9f;
                for(int id:ground)front=std::max(front,(f.world.unit(id)->x.toFloat()-cx)*dirX+(f.world.unit(id)->z.toFloat()-cz)*dirZ);
                std::vector<float> along;
                for(int id:flyers)along.push_back((f.world.unit(id)->x.toFloat()-cx)*dirX+(f.world.unit(id)->z.toFloat()-cz)*dirZ);
                std::sort(along.rbegin(),along.rend());
                r.leadTop+=(along[0]+along[1]+along[2])/3-front;
                double all=0;for(float a:along)all+=a;
                r.leadAll+=all/double(along.size());++r.frontWindows;
            }
            windowCx=cx;windowCz=cz;
        }
        if(groundBusy&&t>=300)for(int id:flyers) {
            const auto& u=*f.world.unit(id);
            if(u.flightGroundMode!=2||u.orders.empty())continue;
            float off=1e9f;
            for(int g:ground)off=std::min(off,std::hypot(u.x.toFloat()-f.world.unit(g)->x.toFloat(),u.z.toFloat()-f.world.unit(g)->z.toFloat()));
            r.maxOff=std::max(r.maxOff,off);
        }
        for(int id:flyers) {
            const auto& u=*f.world.unit(id);
            // Flying like a flyer: while it moves on an order it faces the
            // way it moves (within 30 degrees).
            if(!u.orders.empty()&&u.flightGroundMode==2&&u.speed>Fixed::fromFloat(0.3f)) {
                ++r.moving;
                const uint16_t way=uint16_t(retailDirection(Fixed::raw(u.flightVelocity.x),Fixed::raw(u.flightVelocity.z)).v);
                const int off=int16_t(uint16_t(way-uint16_t(u.heading.v)));
                r.misaligned+=std::abs(off)>0x1555;
            }
            // Hovering: airborne, all but still, and not descending.
            const bool still=u.flightGroundMode==2&&u.speed<Fixed::fromFloat(0.25f)&&!u.landing&&t>=60;
            hoverRun[id]=still?hoverRun[id]+1:0;
            r.maxHover=std::max(r.maxHover,hoverRun[id]);
            // Landing: from the tick its orders end to the start of its descent.
            if(u.orders.empty()&&u.flightGroundMode==2&&!idleAt.count(id))idleAt[id]=t;
            if(!u.orders.empty())idleAt.erase(id);
            if(const auto it=idleAt.find(id);it!=idleAt.end()&&(u.landing||u.flightGroundMode==1)&&it->second>=0) {
                r.maxLandDelay=std::max(r.maxLandDelay,t-it->second);it->second=-1;
            }
        }
        if(groundBusy&&t>=150) {
            float cx,cz;centroid(cx,cz);
            float leadX=-1e9f;for(int id:ground)leadX=std::max(leadX,f.world.unit(id)->x.toFloat());
            for(int id:flyers) {
                const auto& u=*f.world.unit(id);
                const float d=std::hypot(u.x.toFloat()-cx,u.z.toFloat()-cz);
                r.maxAway=std::max(r.maxAway,d);sum+=d;++samples;
                // Landed far ahead: beyond the ground's leading body (they land just in front of it).
                if(t%30==0&&u.flightGroundMode==1&&u.x.toFloat()>leadX+300)++r.landedAhead;
            }
        }
        if(std::getenv("MIXED_TRACE")&&t%150==0) {
            float cx,cz;centroid(cx,cz);
            std::printf("  t=%d ground %.0f,%.0f busy=%d |",t,cx,cz,int(groundBusy));
            for(int id:flyers) {const auto& u=*f.world.unit(id);
                std::printf(" %.0f,%.0f/%zu%s%d",u.x.toFloat(),u.z.toFloat(),u.orders.size(),
                    u.orders.empty()?"":u.orders.front().formationLevel?"F":"",int(u.flightGroundMode));}
            std::printf("\n");
        }
        if(kind!=tak::net::Cmd::Patrol&&r.groundDone>=0&&r.flyersDone>=0&&t>r.groundDone+600)break;
    }
    float cx,cz;centroid(cx,cz);
    for(int id:flyers)r.endAway=std::max(r.endAway,std::hypot(f.world.unit(id)->x.toFloat()-cx,f.world.unit(id)->z.toFloat()-cz));
    for(int id:flyers)r.landed+=f.world.unit(id)->flightGroundMode==1;
    for(const auto& [id,at]:idleAt)if(at>=0)r.maxLandDelay=std::max(r.maxLandDelay,9999);
    r.meanAway=samples?float(sum/samples):0;r.hash=f.world.stateHash();
    for(int id:flyers)r.landedBehind+=landedBehind(f.world,*f.world.unit(id),ground,dirX,dirZ);
    if(r.frontWindows) {r.leadTop/=r.frontWindows;r.leadAll/=r.frontWindows;}
    const char* name=kind==tak::net::Cmd::Move?"move":kind==tak::net::Cmd::AttackMove?"fight":"patrol";
    std::printf("mixedformation %s %s ground_done=%d flyers_done=%d away_max=%.0f away_mean=%.0f end_away=%.0f landed_ahead=%d "
        "pace=%.2f stalls=%d/%d misaligned=%d/%d max_hover=%d land_delay=%d landed=%d spins=%llu lead_top=%.0f lead_all=%.0f "
        "max_off=%.0f landed_behind=%d crawling=%d/%d hash=%016llx\n",
        legion?"legion":"retail",name,r.groundDone,r.flyersDone,r.maxAway,r.meanAway,r.endAway,r.landedAhead,
        r.marched>0?r.flown/r.marched:0.0,r.stalls,r.windows,r.misaligned,r.moving,r.maxHover,r.maxLandDelay,r.landed,
        (unsigned long long)motion.spins(),r.leadTop,r.leadAll,r.maxOff,r.landedBehind,r.crawling,r.cruising,(unsigned long long)r.hash);
    return r;
}
void mixedformation() {
    for(auto kind:{tak::net::Cmd::Move,tak::net::Cmd::AttackMove,tak::net::Cmd::Patrol}) {
        mixedformationRun(false,kind);   // Retail: reported, the 417e02 re-forming port
        const auto r=mixedformationRun(true,kind);
        if(std::getenv("MIXED_REPORT"))continue;   // measurement only
        // Flyers stay with the ground: an airborne flyer within 240 px of a ground body (the
        // old centroid station strayed to 185, the front stations and landing points to 186,
        // the rings over the front, 2026-11-06, to 190).
        check(r.maxOff<=240.f,"a formation flyer strayed from the ground");
        check(r.landedAhead==0,"a formation flyer landed ahead of the ground");
        // Over the front, not the middle: the 3 leading flyers within 80 px of the ground's
        // leading body along its way (front stations -19..-49, the old centroid station
        // -187..-241) and the flyers' mean 60 px or more ahead of the ground's centroid
        // (108..154; old -18..-24).
        check(r.frontWindows>0&&r.leadTop>=-80,"formation flyers did not ride over the front of the ground");
        check(r.leadAll>=60,"formation flyers rode over the middle of the ground");
        // Flying with the formation, not parked over it: they keep the
        // ground's pace in every window (the way flown: circling, a flyer can
        // end a window where it began) and face the way they move. Circling
        // over the front (2026-11-06) they turn all the while: 8.5 / 8.3 / 8.3%
        // of their moving ticks face more than 30 degrees off their way (on the
        // old stations 3.3 / 4.8 / 3.1; Retail's re-forming flyers here 10.7).
        check(r.windows>0&&r.stalls==0,"a formation flyer stalled behind the ground");
        check(r.flown>=0.8*r.marched&&r.flown<=1.5*r.marched,"formation flyers did not keep the ground's pace");
        check(r.moving>0&&r.misaligned*10<=r.moving,"formation flyers did not face the way they flew");
        // Never hovering in place: at most the arrival's own brake.
        check(r.maxHover<=60,"a formation flyer hovered");
        // Flying, not crawling (user request 2026-11-06): on their way they circle over the front at half their
        // own speed or more, not held to the ground's pace (crawling 1,725 / 2,672 / 5,513 of 13,212 / 13,487 /
        // 21,600 ticks: forming up near the goal, a patrol turning at its points; on the stations before, 13,021 /
        // 12,186 / 20,237 of 13,540 / 13,189 / 21,600).
        check(r.cruising>0&&r.crawling*3<=r.cruising,"formation flyers crawled at the ground's pace");
        if(kind!=tak::net::Cmd::Patrol) {
            check(r.groundDone>=0&&r.flyersDone>=0,"the mixed formation never finished");
            check(r.endAway<=300.f,"formation flyers settled away from the ground");
            // Landing as soon as the order is done (retail's standby takes 3 ticks).
            check(r.landed==8,"formation flyers did not land");
            // Landed in front of the ground, not among its slots.
            check(r.landedBehind==0,"a formation flyer landed behind the front of the ground");
            check(r.maxLandDelay<=10,"a formation flyer waited before landing");
        }
    }
    const auto a=mixedformationRun(true,tak::net::Cmd::Move,true),b=mixedformationRun(true,tak::net::Cmd::Move,false);
    check(a.hash==b.hash,"mixed formation: serial and workers differ");
}

// Formation flyers over the front (the user's report: an Alt+N formation's flyers sat over its middle). An Alt+1
// formation of 30 ground bodies -- a 10 x 3 block, wide across its way -- and 12 flyers, more than one row of
// lanes, on an open field: a move east, then under way a move south that re-faces it. In each 30-tick window the
// ground marched >= 9 px, along its way: lead_top is the mean offset of the 3 leading flyers from the ground's
// leading body, lead_all the flyers' mean offset from the ground's centroid, front_row the flyers within 32 px
// of the leading body's line or ahead of it, and depth the farthest flyer behind it (rows fill front first).
// At the end every flyer has landed, in front of the ground. Then the same flyers alone (no ground: their own
// orders, as before) land at their own points.
struct FrontPhase {double top=0,all=0,row=0,span=0;float depth=0;int n=0;};
struct FrontRun {FrontPhase east,south;int landed=0,behind=0;int done=-1;uint64_t hash=0;};
// cold: the click as a real match's first one -- on a map over 131k cells (400 x 400: a plane's first build is an
// incremental job the registrations wait for, as on Ulasem), on the spawn tick (no settle, so nothing prebuilt the
// plane), and as the client sends it (one explicit selection order, Cmd::Order). The ground waits in the
// registration FIFO for a few ticks; its flyers must still keep station (0.8.4 regression: release A read the
// waiting ground as stalled and freed every flyer, which flew straight to the goal).
FrontRun flyerfrontRun(bool serial=true,bool cold=false) {
    // 240 cells deep: the south leg stops 640 px from the map's south edge, room for the three rows of landing lanes
    // in front of the ground. On the 220-cell map of the first version the third row lay past the edge, where the
    // clamp then put it; since 2026-10-30 lanes within a lane of the edge send the whole wing to its own points.
    Fixture f(cold?400:260,cold?400:240,serial);f.publish();
    const auto type=mover(2);
    UnitType flyer{};flyer.id=flyer.name="legion-flyer";
    flyer.canFly=flyer.canMove=true;flyer.maxHp=100;flyer.footX=flyer.footZ=3;flyer.sight=4096;
    flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);flyer.turnRate=1200;flyer.cruiseAlt=80;flyer.buildTime=1;
    flyer.vtolStandby=true;
    std::vector<int> ground,flyers,all;
    for(int i=0;i<30;++i)ground.push_back(f.spawn(type,20+(i/10)*3,60+(i%10)*3));
    for(int i=0;i<12;++i)flyers.push_back(f.spawn(flyer,8+(i%6)*4,100+(i/6)*4));
    all=ground;all.insert(all.end(),flyers.begin(),flyers.end());
    f.start();
    TypeRegistry registry;
    auto command=[&](tak::net::Cmd k,int id,float x,float z,int target=0) {
        tak::net::Command c;c.kind=k;c.player=0;c.unitId=id;c.targetId=target;c.x=x;c.z=z;c.queue=0;
        applyCommand(f.world,registry,c);
    };
    // As the move UI does: flyers keep their offset from the selection's centre (clamped to 60 px per axis);
    // Legion surface movers share the point. Cold, the commands go packed as the client's explicit order.
    uint32_t orderSerial=1;
    auto click=[&](float px,float pz) {
        float sx=0,sz=0;
        for(int id:all){sx+=f.world.unit(id)->x.toFloat();sz+=f.world.unit(id)->z.toFloat();}
        sx/=float(all.size());sz/=float(all.size());
        std::vector<tak::net::Command> cmds;
        for(int id:all) {
            const auto& u=*f.world.unit(id);
            tak::net::Command c;c.kind=tak::net::Cmd::Move;c.player=0;c.unitId=id;c.queue=0;
            c.x=u.type->canFly?px+std::clamp(u.x.toFloat()-sx,-60.f,60.f):px;
            c.z=u.type->canFly?pz+std::clamp(u.z.toFloat()-sz,-60.f,60.f):pz;
            cmds.push_back(c);
        }
        if(cold)cmds=tak::net::packClick(cmds,{true,px,pz},orderSerial);
        for(const auto& c:cmds)applyCommand(f.world,registry,c);
    };
    for(int id:all)command(tak::net::Cmd::SetSquad,id,0,0,-1);
    for(int t=0;t<(cold?0:300);++t)f.world.tick(1.f/30);
    click(190*16,74*16);
    FrontRun r;float wx=0,wz=0;
    auto centroid=[&](float& cx,float& cz) {
        double sx=0,sz=0;for(int id:ground){sx+=f.world.unit(id)->x.toFloat();sz+=f.world.unit(id)->z.toFloat();}
        cx=float(sx/ground.size());cz=float(sz/ground.size());
    };
    centroid(wx,wz);
    float dx=1,dz=0;
    for(int t=0;t<7000;++t) {
        if(t==900) {float cx,cz;centroid(cx,cz);click(cx,200*16);}
        f.world.tick(1.f/30);
        bool busy=false;for(int id:all)busy|=!f.world.unit(id)->orders.empty();
        if(!busy&&r.done<0)r.done=t;
        // The landing: a flyer that came down on another steps out of the stack first (retail's air traffic,
        // VTOL_Standby and VTOL_LandIfCan) and then searches for a spot; give it the time.
        if(r.done>=0) {if(t>r.done+900)break;continue;}
        if(t%30)continue;
        float cx,cz;centroid(cx,cz);
        const float marched=std::hypot(cx-wx,cz-wz);
        if(marched>=9.f) {
            dx=(cx-wx)/marched;dz=(cz-wz)/marched;
            bool ground_busy=false;for(int id:ground)ground_busy|=!f.world.unit(id)->orders.empty();
            FrontPhase* ph=t>=300&&t<900?&r.east:t>=1500?&r.south:nullptr;
            if(ph&&ground_busy) {
                float front=-1e9f;
                for(int id:ground)front=std::max(front,(f.world.unit(id)->x.toFloat()-cx)*dx+(f.world.unit(id)->z.toFloat()-cz)*dz);
                std::vector<float> along;
                for(int id:flyers)along.push_back((f.world.unit(id)->x.toFloat()-cx)*dx+(f.world.unit(id)->z.toFloat()-cz)*dz);
                std::sort(along.rbegin(),along.rend());
                ph->top+=(along[0]+along[1]+along[2])/3-front;
                double sum=0;int row=0;for(float a:along){sum+=a;row+=a>=front-32;}
                ph->all+=sum/double(along.size());ph->row+=row;ph->depth=std::max(ph->depth,front-along.back());++ph->n;
                // Across: the flyers' lateral span over the ground's.
                float glo=1e9f,ghi=-1e9f,flo=1e9f,fhi=-1e9f;
                for(int id:ground) {const float s=(f.world.unit(id)->z.toFloat()-cz)*dx-(f.world.unit(id)->x.toFloat()-cx)*dz;glo=std::min(glo,s);ghi=std::max(ghi,s);}
                for(int id:flyers) {const float s=(f.world.unit(id)->z.toFloat()-cz)*dx-(f.world.unit(id)->x.toFloat()-cx)*dz;flo=std::min(flo,s);fhi=std::max(fhi,s);}
                ph->span+=ghi>glo?(fhi-flo)/(ghi-glo):1.f;
            }
        }
        wx=cx;wz=cz;
    }
    for(int id:flyers) {
        const auto& u=*f.world.unit(id);
        r.landed+=u.flightGroundMode==1;
        r.behind+=landedBehind(f.world,u,ground,dx,dz);
    }
    for(auto* ph:{&r.east,&r.south})if(ph->n){ph->top/=ph->n;ph->all/=ph->n;ph->row/=ph->n;ph->span/=ph->n;}
    r.hash=f.world.stateHash();
    std::printf("flyerfront%s east lead_top=%.0f lead_all=%.0f front_row=%.1f span=%.2f depth=%.0f windows=%d | south lead_top=%.0f lead_all=%.0f "
        "front_row=%.1f span=%.2f depth=%.0f windows=%d | done=%d landed=%d landed_behind=%d hash=%016llx\n",cold?" cold":"",r.east.top,r.east.all,r.east.row,
        r.east.span,r.east.depth,r.east.n,r.south.top,r.south.all,r.south.row,r.south.span,r.south.depth,r.south.n,r.done,r.landed,r.behind,
        (unsigned long long)r.hash);
    return r;
}
// The same 12 flyers as an air-only formation: no ground, so no station; each flies its own point and lands there.
float flyerfrontAirOnly() {
    Fixture f(260,220);f.publish();
    UnitType flyer{};flyer.id=flyer.name="legion-flyer";
    flyer.canFly=flyer.canMove=true;flyer.maxHp=100;flyer.footX=flyer.footZ=3;flyer.sight=4096;
    flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);flyer.turnRate=1200;flyer.cruiseAlt=80;flyer.buildTime=1;
    flyer.vtolStandby=true;
    std::vector<int> flyers;
    for(int i=0;i<12;++i)flyers.push_back(f.spawn(flyer,8+(i%6)*4,100+(i/6)*4));
    f.start();
    TypeRegistry registry;
    std::map<int,std::pair<float,float>> goal;
    for(int id:flyers) {
        tak::net::Command c;c.kind=tak::net::Cmd::SetSquad;c.player=0;c.unitId=id;c.targetId=-1;applyCommand(f.world,registry,c);
    }
    for(int t=0;t<300;++t)f.world.tick(1.f/30);
    for(int id:flyers) {
        const auto& u=*f.world.unit(id);
        tak::net::Command c;c.kind=tak::net::Cmd::Move;c.player=0;c.unitId=id;c.x=190*16+u.x.toFloat()-(8+10)*16;c.z=u.z.toFloat();c.queue=0;
        goal[id]={c.x,c.z};applyCommand(f.world,registry,c);
    }
    for(int t=0;t<4000;++t)f.world.tick(1.f/30);
    float worst=0;
    for(int id:flyers) {
        const auto& u=*f.world.unit(id);
        check(u.flightGroundMode==1&&u.orders.empty(),"an air-only formation's flyer did not land");
        worst=std::max(worst,std::hypot(u.x.toFloat()-goal[id].first,u.z.toFloat()-goal[id].second));
    }
    std::printf("flyerfront air-only farthest_from_point=%.0f\n",worst);
    return worst;
}
// Formation flyers over narrow ground: a block `cols` bodies across and `rows` along its way (one body; a column
// one or two wide) and 8 flyers moving east on an open field. In each 30-tick window the ground marched >= 9 px:
// top the mean offset of the 3 leading flyers from the ground's leading body, depth the farthest flyer behind it,
// behind the flyers behind the ground's centroid. Lanes one per 64 px of a narrow front would put the 8 in a file
// 448 px long trailing it; they stand at most twice as deep as wide instead (2 lanes, 4 rows: 192 px).
struct NarrowRun {double top=0,behind=0;float depth=0;int n=0;};
NarrowRun flyerfrontNarrow(int cols,int rows) {
    Fixture f(260,220);f.publish();
    const auto type=mover(2);
    UnitType flyer{};flyer.id=flyer.name="legion-flyer";
    flyer.canFly=flyer.canMove=true;flyer.maxHp=100;flyer.footX=flyer.footZ=3;flyer.sight=4096;
    flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);flyer.turnRate=1200;flyer.cruiseAlt=80;flyer.buildTime=1;
    flyer.vtolStandby=true;
    std::vector<int> ground,flyers,all;
    for(int i=0;i<cols*rows;++i)ground.push_back(f.spawn(type,20+(i/cols)*3,100+(i%cols)*3));
    for(int i=0;i<8;++i)flyers.push_back(f.spawn(flyer,8+(i%4)*4,100+(i/4)*4));
    all=ground;all.insert(all.end(),flyers.begin(),flyers.end());
    f.start();
    TypeRegistry registry;
    auto command=[&](tak::net::Cmd k,int id,float x,float z,int target=0) {
        tak::net::Command c;c.kind=k;c.player=0;c.unitId=id;c.targetId=target;c.x=x;c.z=z;c.queue=0;
        applyCommand(f.world,registry,c);
    };
    for(int id:all)command(tak::net::Cmd::SetSquad,id,0,0,-1);
    for(int t=0;t<300;++t)f.world.tick(1.f/30);
    for(int id:all) {
        const auto& u=*f.world.unit(id);
        command(tak::net::Cmd::Move,id,u.type->canFly?190*16+std::clamp(u.x.toFloat()-30*16,-60.f,60.f):190*16,
                u.type->canFly?104*16+std::clamp(u.z.toFloat()-104*16,-60.f,60.f):104*16);
    }
    NarrowRun r;float wx=0;
    for(int t=0;t<2400;++t) {
        f.world.tick(1.f/30);
        if(t%30||t<300)continue;
        double sx=0;for(int id:ground)sx+=f.world.unit(id)->x.toFloat();
        const float cx=float(sx/ground.size());
        bool busy=false;for(int id:ground)busy|=!f.world.unit(id)->orders.empty();
        if(busy&&cx-wx>=9.f) {
            float front=-1e9f;for(int id:ground)front=std::max(front,f.world.unit(id)->x.toFloat()-cx);
            std::vector<float> along;for(int id:flyers)along.push_back(f.world.unit(id)->x.toFloat()-cx);
            std::sort(along.rbegin(),along.rend());
            r.top+=(along[0]+along[1]+along[2])/3-front;r.depth=std::max(r.depth,front-along.back());
            for(float a:along)r.behind+=a<-1.f;
            ++r.n;
        }
        wx=cx;
    }
    if(r.n){r.top/=r.n;r.behind/=r.n;}
    std::printf("flyerfront narrow %dx%d lead_top=%.0f depth=%.0f behind_centroid=%.1f windows=%d\n",cols,rows,r.top,r.depth,r.behind,r.n);
    return r;
}
void flyerfront() {
    const auto r=flyerfrontRun();
    // The leading flyers over the ground's leading body on both headings, neither behind it nor beyond the
    // ring they circle (measured on the stations: east -2, south -20; the old centroid station's spiral: east
    // +77, wider than a 3-deep block is deep, south -120; circling since 2026-11-06, a ring reaching
    // kOrbitAhead + kOrbit = 112 px beyond its station: east 56, south 23).
    for(const auto* ph:{&r.east,&r.south}) {
        check(ph->n>0,"flyerfront: no window measured");
        check(ph->top>=-80,"flyerfront: the leading flyers did not ride over the ground's leading body");
        check(ph->top<=120,"flyerfront: the leading flyers rode ahead of the ground");
    }
    // East the front is 10 bodies wide: the flyers spread across all of it (span 1.01; circling 1.22; the spiral
    // 0.34), half of them on the front line (a row of 7 lanes; circling 7.8) and the rest in the next row, not
    // deeper (83 px, circling 114; 2 rows = 128).
    check(r.east.span>=0.85,"flyerfront: the flyers did not spread across the front");
    check(r.east.row>=6,"flyerfront: the front row was not filled first");
    check(r.east.depth<=200,"flyerfront: flyers sat deeper than the second row");
    check(r.done>=0&&r.landed==12,"flyerfront: the formation did not finish and land");
    check(r.behind==0,"flyerfront: a flyer landed behind the front of the ground");
    check(flyerfrontAirOnly()<=120,"flyerfront: an air-only formation's flyer landed away from its point");
    // Narrow ground (measured: depth 193 / 219 / 220, a lane per 64 px of front 448 / 455 / 455): the 8 flyers
    // over the front in at most 4 rows (2 lanes; 192 px plus the lag), the leading ones over the leading body,
    // and over a 10-deep column none behind its middle (the file down a column had 4 of 8 behind it).
    for(auto [cols,rows]:{std::pair{1,1},std::pair{1,10},std::pair{2,10}}) {
        const auto n=flyerfrontNarrow(cols,rows);
        check(n.n>0,"flyerfront narrow: no window measured");
        check(n.top>=-80&&n.top<=120,"flyerfront narrow: the leading flyers were not over the ground's leading body");
        check(n.depth<=260,"flyerfront narrow: the flyers trailed in a file behind narrow ground");
        if(rows>1)check(n.behind<=0.5,"flyerfront narrow: flyers sat behind the middle of a column");
    }
    check(flyerfrontRun(false).hash==r.hash,"flyerfront: serial and workers differ");
    // Cold (the first click of a match on a big map, as the client sends it): the same rows over the front while the
    // ground's registrations wait for the plane's first build (measured: east -1, south -1; with the waiting ground
    // read as stalled, every flyer was released on the first tick and the leading ones were 1154 px ahead of the
    // ground's front east; the south click, with the plane built, kept station).
    const auto c=flyerfrontRun(true,true);
    for(const auto* ph:{&c.east,&c.south}) {
        check(ph->n>0,"flyerfront cold: no window measured");
        check(ph->top>=-80&&ph->top<=120,"flyerfront cold: the leading flyers were not over the ground's leading body");
    }
    check(c.east.depth<=200,"flyerfront cold: flyers sat deeper than the second row");
    check(c.done>=0&&c.landed==12&&c.behind==0,"flyerfront cold: the formation did not finish and land in front of the ground");
    check(flyerfrontRun(false,true).hash==c.hash,"flyerfront cold: serial and workers differ");
}


// Group awareness: two moving groups plan round each other as wholes.
// Cases: two own groups crossing paths at right angles; two own groups head
// on in a corridor wide enough to pass (each keeps right); an enemy group
// head on, seen (sight 4096 px) and not seen (sight 16 px); and an
// attack-move straight at the enemy (no avoidance). Reports, per case: the
// tick everyone arrived (or -1), the share of samples standing, contacts
// between bodies of the two groups, spins, and the detour (mean travelled
// over straight-line distance, %).
struct AwareResult {int done=-1,arrived=0,total=0;double stopped=0,contacts=0,detour=0;uint64_t spins=0,reversals=0;};
AwareResult awareRun(const char* name,int kind,int sightB) {
    // kind 0 crossing, 1 head-on corridor (own), 2 head-on enemy, 3 attack-move into enemy
    Fixture f(200,140);
    if(kind>=1) {f.rect(0,40,200,2);f.rect(0,100,200,2);}   // corridor z 42-99 (58 cells)
    f.publish();
    auto ta=mover(2),tb=mover(2);tb.id=tb.name="legion-foot-2b";tb.sight=sightB;ta.sight=sightB;
    std::vector<int> a,b;
    const int per=24;
    for(int i=0;i<per;++i) {
        if(kind==0) {a.push_back(f.spawn(ta,20+(i%6)*3,62+(i/6)*3));b.push_back(f.spawn(tb,88+(i%6)*3,112+(i/6)*3,0));}
        else {a.push_back(f.spawn(ta,20+(i%6)*3,64+(i/6)*3));b.push_back(f.spawn(tb,160+(i%6)*3,64+(i/6)*3,kind>=2?1:0));}
    }
    f.start();
    std::vector<std::pair<int,int>> goal;
    for(int id:a) {
        const auto& u=*f.world.unit(id);
        const int gx=kind==0?180:185,gz=70;(void)u;
        if(kind==3)f.world.attackMove(id,float(gx*16),float(gz*16),false);else f.world.order(id,float(gx*16),float(gz*16),false);
    }
    // A separate command for an own group B (one selection would be one
    // convoy, which never plans round itself).
    if(kind<2)for(int t=0;t<100;++t)f.world.tick(1.f/30);
    for(int id:b) {
        const auto& u=*f.world.unit(id);
        const int gx=kind==0?96:8,gz=kind==0?10:70;(void)u;
        f.world.order(id,float(gx*16),float(gz*16),false);
    }
    std::vector<int> all=a;all.insert(all.end(),b.begin(),b.end());
    // The observer's decision samples (every 10 ticks) give the standing
    // share, the A x B proximity contacts (centre cells within 2 on both
    // axes) and the detour; no pair loop here.
    Watch motion;AwareResult r;r.total=int(all.size());
    {
        tak::legion_observe::Group ga,gb;ga.name="A";ga.ids=a;gb.name="B";gb.ids=b;
        motion.cfg.groups={ga,gb};
        motion.cfg.pairs.push_back({"A","B",2});
        motion.cfg.decisionEvery=10;
    }
    for(int t=0;t<6000;++t) {
        f.world.tick(1.f/30);motion.observe(f.world,all);
        int arrived=0;
        for(int id:all)arrived+=f.world.unit(id)->orders.empty()||!f.world.unit(id)->alive();
        r.arrived=arrived;
        if(std::getenv("AWARE_ASCII")&&t%150==0) {
            std::vector<std::string> g(140,std::string(200,'.'));
            for(int z=0;z<140;++z)for(int x=0;x<200;++x)if(f.cells[size_t(z)*200+x]==0)g[size_t(z)][size_t(x)]='#';
            for(int id:a) {const auto& u=*f.world.unit(id);g[size_t(u.z.v>>20)][size_t(u.x.v>>20)]='A';}
            for(int id:b) {const auto& u=*f.world.unit(id);g[size_t(u.z.v>>20)][size_t(u.x.v>>20)]='B';}
            std::printf("t=%d\n",t);for(auto& l:g)std::printf("|%s\n",l.c_str());
        }
        if(arrived==r.total) {r.done=t;break;}
    }
    {
        const auto k=motion.keys();
        using tak::legion_observe::Observer;
        r.stopped=double(Observer::get(k,"stopped_permille",0))/10;
        r.contacts=double(Observer::get(k,"pair.A.B.permille_x100",0))/100;
        r.detour=double(Observer::get(k,"detour_permille",0))/10;
    }
    r.spins=motion.spins();r.reversals=motion.reversals();
    std::printf("aware case=%s arrived=%d/%d done=%d stopped=%.1f%% contacts_permille=%.2f detour=%.1f%% spins=%llu reversals=%llu\n",name,r.arrived,r.total,r.done,
        r.stopped,r.contacts,r.detour,(unsigned long long)r.spins,(unsigned long long)r.reversals);
    return r;
}
void aware() {
    const auto cross=awareRun("cross",0,4096);
    const auto head=awareRun("headon",1,4096);
    const auto seen=awareRun("enemy-seen",2,4096);
    const auto unseen=awareRun("enemy-unseen",2,16);
    const auto attack=awareRun("attack",3,4096);
    if(std::getenv("AWARE_REPORT"))return;
    for(const AwareResult* r:std::initializer_list<const AwareResult*>{&cross,&head,&seen,&unseen}) {
        check(r->arrived==r->total,"aware: groups did not get past each other");
        check(r->spins==0,"aware: spin");
    }
    (void)attack;
}

// ---- T1 probes (W3's instruments; measurement only) -------------------------
// pocket, deadend, tail, settlelatency and doorplug print numbers for the
// army-throughput and settling work (docs/legion-pathfinding.md, "Known
// weaknesses"). They read state and use the test hooks only; the one that
// changes behaviour is settlelatency's rest stride, a process-wide test hook
// it restores. Each asserts determinism only: a repeated run and a workers
// run of one configuration print and hash exactly as the serial run. Bounds
// come with the baselines.
namespace {
int envInt(const char* key,int fallback) {const char* e=std::getenv(key);return e?std::atoi(e):fallback;}
template<class T> T quantile(std::vector<T> v,double q) {
    if(v.empty())return T(-1);
    std::sort(v.begin(),v.end());
    return v[std::min(v.size()-1,size_t(q*double(v.size())))];
}
// The T1 I4 work counters, the completion counters and the I2 slot shape of
// the formation assigned last.
std::string t1Counters(World& w) {
    const auto* nav=w.legionNavigator();
    if(!nav)return " | retail";
    static const std::set<std::string_view> keep{"crowd_window_ring_cells","crowd_settle_visits","rechoice_bfs_cells",
        "formation_ring_cells","join_iterations","midroute_completions","outside_area_completions","completion_dist_max"};
    std::string s=" |";
    std::string byState=" move_calls_by_state=";
    char buf[96];
    LegionNavigator::forEachStat(nav->stats(),[&](const char* name,uint64_t v) {
        const std::string_view n(name);
        if(n.substr(0,20)=="move_calls_by_state_") {byState+=(byState.back()=='='?"":"/")+std::to_string(v);return;}
        if(!keep.count(n))return;
        std::snprintf(buf,sizeof buf," %s=%llu",name,(unsigned long long)v);s+=buf;
    });
    s+=byState;
    const auto shape=nav->lastSlotShape();
    std::snprintf(buf,sizeof buf," | slot_shape valid=%d members=%d slots=%d frontage=%d rms_along=%lld rms_across=%lld limit=%lld",
        int(shape.valid),shape.members,shape.slots,shape.frontage,(long long)shape.rmsAlong,(long long)shape.rmsAcross,(long long)shape.limit);
    s+=buf;
    return s;
}
struct ProbeRun {std::string text;uint64_t hash=0;int a=-1,b=-1,c=-1;};   // a, b, c: probe-specific numbers for W2_REQUIRE
// Determinism: the same run again and with workers prints and hashes the same.
void checkRepeatable(const char* name,const std::function<ProbeRun(bool)>& run,const ProbeRun& serial) {
    const ProbeRun again=run(true),workers=run(false);
    std::printf("%s determinism serial=%016llx repeat=%016llx workers=%016llx\n",name,(unsigned long long)serial.hash,
        (unsigned long long)again.hash,(unsigned long long)workers.hash);
    check(again.hash==serial.hash&&again.text==serial.text,std::string(name)+": a repeated run differs");
    check(workers.hash==serial.hash&&workers.text==serial.text,std::string(name)+": serial and workers differ");
}
// Holding members by what stands in front of them (a body touching them on
// the downhill side of their own field, else nearer the click): a settled
// body of the order, a Holding member of the order, or something else.
struct HoldClass {int held=0,behindOwnHeld=0,behindSettled=0,other=0;};
HoldClass holdClasses(World& w,const std::vector<int>& ids,float tx,float tz) {
    HoldClass r;
    auto* nav=w.legionNavigator();
    if(!nav)return r;
    std::map<int64_t,std::vector<int>> grid;   // 64 px buckets
    auto key=[](int64_t bx,int64_t bz) {return (bz<<32)^(bx&0xffffffff);};
    for(int id:ids) {const auto& u=*w.unit(id);grid[key(int64_t(u.x.v)>>22,int64_t(u.z.v)>>22)].push_back(id);}
    for(int id:ids) {
        const auto& u=*w.unit(id);
        if(u.orders.empty()||nav->unitState(id)!=2)continue;
        ++r.held;
        const int foot=u.type->footX;
        const int own=nav->fieldPotential(id,footprintOrigin(u.x,foot),footprintOrigin(u.z,foot));
        const float ux=u.x.toFloat(),uz=u.z.toFloat();
        const float dl=std::hypot(tx-ux,tz-uz)+1e-3f;
        bool settled=false,held=false;
        const int64_t bx=int64_t(u.x.v)>>22,bz=int64_t(u.z.v)>>22;
        for(int64_t j=-1;j<=1;++j)for(int64_t i=-1;i<=1;++i) {
            const auto found=grid.find(key(bx+i,bz+j));
            if(found==grid.end())continue;
            for(int o:found->second) {
                if(o==id)continue;
                const auto& v=*w.unit(o);
                const float ox=v.x.toFloat()-ux,oz=v.z.toFloat()-uz;
                const float d=std::hypot(ox,oz);
                if(d>=float(foot+v.type->footX)*8.f+12.f)continue;
                const int p=own>=0?nav->fieldPotential(id,footprintOrigin(v.x,v.type->footX),footprintOrigin(v.z,v.type->footZ)):-1;
                const bool ahead=own>=0&&p>=0?p<own:(ox*(tx-ux)+oz*(tz-uz))/dl>0.3f*d;
                if(!ahead)continue;
                if(v.orders.empty())settled=true;
                else if(nav->unitState(o)==2)held=true;
            }
        }
        if(settled)++r.behindSettled;else if(held)++r.behindOwnHeld;else ++r.other;
    }
    return r;
}

// pocket (MV-01): 615 or 304 foot-2 bodies in a wall-bounded pocket (or the
// same spot with no walls) ordered to one point far east past the wall's
// underside, either 64 orders per tick (the client's selection split) or all
// in one tick. POCKET_N/POCKET_CAP/POCKET_OPEN/POCKET_TICKS pick one run.
ProbeRun pocketRun(int n,int cap,bool open,int ticks,bool serial) {
    Fixture f(640,300,serial);
    if(!open) {f.rect(138,60,6,115);f.rect(138,174,290,12);}
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<n;++i)ids.push_back(f.spawn(type,60+(i%26)*3,108+(i/26)*3));
    f.start();
    const float tx=461*16,tz=206*16;
    auto* nav=f.world.legionNavigator();
    size_t issued=0;
    int t90=-1,arrived2000=-1,arrived4000=-1,arrived8000=-1,holding1000=-1,end=ticks;
    HoldClass at1000;
    for(int t=0;t<ticks;++t) {
        if(issued<ids.size()) {
            const size_t k=cap>0?std::min(ids.size()-issued,size_t(cap)):ids.size()-issued;
            for(size_t i=0;i<k;++i)f.world.order(ids[issued+i],tx,tz,false);
            issued+=k;
        }
        f.world.tick(1.f/30);
        const int now=t+1;
        if(now%250)continue;
        int arrived=0,holding=0;
        for(int id:ids) {const auto& u=*f.world.unit(id);if(u.orders.empty())++arrived;else holding+=nav->unitState(id)==2;}
        if(t90<0&&arrived*10>=n*9)t90=now;
        if(now==1000) {holding1000=holding;at1000=holdClasses(f.world,ids,tx,tz);}
        if(now<=2000)arrived2000=arrived;
        if(now<=4000)arrived4000=arrived;
        arrived8000=arrived;
        if(arrived==n) {end=now;break;}
    }
    std::vector<double> dist;
    for(int id:ids) {const auto& u=*f.world.unit(id);if(!u.orders.empty())dist.push_back(std::hypot(u.x.toFloat()-tx,u.z.toFloat()-tz));}
    const HoldClass last=holdClasses(f.world,ids,tx,tz);
    char buf[640];
    std::snprintf(buf,sizeof buf,"pocket n=%d cap=%d open=%d ticks=%d end=%d arrived@2000=%d @4000=%d @%d=%d t90=%d holding@1000=%d"
        " held1000[behind_own_held=%d settled=%d other=%d] active=%zu active_med_dist=%.0f held_end[behind_own_held=%d settled=%d other=%d]",
        n,cap,int(open),ticks,end,arrived2000,arrived4000,ticks,arrived8000,t90,holding1000,at1000.behindOwnHeld,at1000.behindSettled,at1000.other,
        dist.size(),quantile(dist,0.5),last.behindOwnHeld,last.behindSettled,last.other);
    return {std::string(buf)+t1Counters(f.world),f.world.stateHash()};
}
void pocket() {
    const int ticks=envInt("POCKET_TICKS",8000);
    if(std::getenv("POCKET_N")||std::getenv("POCKET_CAP")||std::getenv("POCKET_OPEN")) {
        const auto r=pocketRun(envInt("POCKET_N",615),envInt("POCKET_CAP",64),envInt("POCKET_OPEN",0)!=0,ticks,true);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        return;
    }
    ProbeRun check304;
    for(int n:{615,304})for(int open:{1,0})for(int cap:{64,0}) {
        const auto r=pocketRun(n,cap,open!=0,ticks,true);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        std::fflush(stdout);
        if(n==304&&open==0&&cap==64)check304=r;
    }
    checkRepeatable("pocket",[&](bool serial) {return pocketRun(304,64,false,ticks,serial);},check304);
}

// deadend (AR-02): bodies ordered to the closed end of a dead-end corridor
// (width 2/4/6 cells, 100 cells long, optionally opening into a 20x12 room),
// as one order or as an Alt+N squad; Retail runs the same as the parity
// reference. DEADEND_W/N/ROOM/SQUAD/RETAIL pick one run.
ProbeRun deadendRun(int width,int count,bool room,bool squad,bool retail,bool serial) {
    Fixture f(260,100,serial);
    if(retail)f.world.setPathfindingMode(PathfindingMode::Retail);
    const int roomL=room?20:0,roomH=room?12:0,zc=48+width/2;
    if(room) {
        const int zr0=zc-roomH/2;
        f.rect(100,0,100,48);f.rect(100,48+width,100,52-width);
        f.rect(200,0,roomL,zr0);f.rect(200,zr0+roomH,roomL,100-zr0-roomH);f.rect(200+roomL,0,60-roomL,100);
    } else {
        f.rect(100,0,140,48);f.rect(100,48+width,140,52-width);f.rect(240,48,20,width);
    }
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<count;++i)ids.push_back(f.spawn(type,14+(i%12)*3,32+(i/12)*4));
    f.start();
    TypeRegistry registry;
    auto command=[&](tak::net::Cmd kind,int id,float x,float z) {
        tak::net::Command c;c.kind=kind;c.player=0;c.unitId=id;c.targetId=-1;c.x=x;c.z=z;c.queue=0;
        applyCommand(f.world,registry,c);
    };
    if(squad) {for(int id:ids)command(tak::net::Cmd::SetSquad,id,0,0);for(int t=0;t<600;++t)f.world.tick(1.f/30);}
    const float px=float((room?200+roomL/2:236)*16),pz=float(zc*16);
    for(int id:ids)command(tak::net::Cmd::Move,id,px,pz);
    std::map<int,int> doneAt;
    constexpr int kTicks=12000;
    int ticks=0;
    Watch watch;
    {
        tak::legion_observe::Group g;g.name="all";g.ids=ids;g.clickX=int(px);g.clickZ=int(pz);
        watch.cfg.groups.push_back(g);
    }
    for(;ticks<kTicks&&doneAt.size()<ids.size();++ticks) {
        f.world.tick(1.f/30);watch.observe(f.world,ids);
        for(int id:ids)if(!doneAt.count(id)&&f.world.unit(id)->orders.empty())doneAt[id]=ticks;
    }
    std::vector<int> done;for(auto& [id,t]:doneAt)done.push_back(t);
    int holding=0,moving=0;float nearestHolder=-1,farthestSettled=0;
    for(int id:ids) {
        const auto& u=*f.world.unit(id);
        const float d=std::hypot(u.x.toFloat()-px,u.z.toFloat()-pz)/16;
        if(u.orders.empty()) {farthestSettled=std::max(farthestSettled,d);continue;}
        ++holding;moving+=u.speed>Fixed();
        nearestHolder=nearestHolder<0?d:std::min(nearestHolder,d);
    }
    char buf[400];
    std::snprintf(buf,sizeof buf,"deadend mode=%s width=%d room=%d squad=%d n=%d settled=%zu/%d holding=%d moving=%d done_tick p50=%d p90=%d max=%d"
        " farthest_settled_cells=%.1f nearest_holder_cells=%.1f",
        retail?"retail":"legion",width,int(room),int(squad),count,doneAt.size(),count,holding,moving,quantile(done,0.5),quantile(done,0.9),
        done.empty()?-1:*std::max_element(done.begin(),done.end()),farthestSettled,nearestHolder);
    return {std::string(buf)+" | "+completionKeys(watch.keys(),"all")+t1Counters(f.world),f.world.stateHash(),int(doneAt.size())};
}
// Funnel claims and the funnel give-way (Legion ~100 round 3, 2026-10-20). A queue reaching a corridor one body
// wide (2 cells, 2x2 bodies) arrives on both sides of the corridor's first origin; on the parent two bodies wait
// either side of it, each standing on a cell of it, for good: no further body enters, and the orders of the
// crowd outside never end (the closed corridor: 27 and 9 of 120 settle at start offsets 0 and -1; the room at the
// end of one: none of 40 where the column straddles the corridor's row, offsets +12 and +14). Every order must end.
void funnelmouth() {
    struct Case {int count;bool room;int shift;};
    const Case cases[]={{120,false,0},{120,false,-1},{40,true,12},{40,true,14}};
    std::string fail;
    for(const auto& c:cases) {
        g_shift=c.shift;
        const auto r=deadendRun(2,c.count,c.room,false,false,true);
        g_shift=0;
        std::printf("funnelmouth shift=%d %s hash=%016llx\n",c.shift,r.text.substr(0,r.text.find(" |")).c_str(),(unsigned long long)r.hash);
        std::fflush(stdout);
        if(r.a<c.count)fail+=" "+std::to_string(c.count)+(c.room?"room":"closed")+"@"+std::to_string(c.shift)+":"+std::to_string(r.a);
    }
    check(fail.empty(),"funnelmouth: orders still open at a corridor's mouth:"+fail);
}
// Ties between funnel claims (Legion ~100 round 3 review, 2026-10-20). A field of 1x1 pillars 3 cells apart leaves
// gaps 2 cells wide, one origin wide for a 2x2 body, so every origin in it is a funnel way (passageWay). Two bodies
// waiting for neighbouring ways at one potential each cover the other's way: when an equal potential also closed a
// way to a waiter, both held for good with no body on either way (giveWay never ran): of 72 bodies on one click,
// 4, 9 and 12 orders were still open after 9000 ticks at offsets 0, +1 and -3. Every order must end.
ProbeRun pillarFieldRun(bool serial) {
    Fixture f(260,100,serial);
    for(int x=80;x<160;x+=3)for(int z=5;z<=95;z+=3)f.wall(x,z);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int j=0;j<6;++j)for(int i=0;i<12;++i)ids.push_back(f.spawn(type,14+i*3,30+j*4));
    f.start();
    TypeRegistry registry;
    const float px=220*16,pz=50*16;
    for(int id:ids) {
        tak::net::Command c;c.kind=tak::net::Cmd::Move;c.player=0;c.unitId=id;c.targetId=-1;c.x=px;c.z=pz;c.queue=0;
        applyCommand(f.world,registry,c);
    }
    int ticks=0,open=int(ids.size());
    for(;ticks<9000&&open;++ticks) {
        f.world.tick(1.f/30);
        open=0;for(int id:ids)open+=!f.world.unit(id)->orders.empty();
    }
    char buf[160];
    std::snprintf(buf,sizeof buf,"pillarfield n=%zu settled=%zu ticks=%d",ids.size(),ids.size()-size_t(open),ticks);
    return {buf,f.world.stateHash(),int(ids.size())-open};
}
void funneltie() {
    std::string fail;
    for(int shift:{0,1,-3}) {
        g_shift=shift;
        const auto r=pillarFieldRun(true);
        g_shift=0;
        std::printf("funneltie shift=%d %s hash=%016llx\n",shift,r.text.c_str(),(unsigned long long)r.hash);
        std::fflush(stdout);
        if(r.a<72)fail+=" @"+std::to_string(shift)+":"+std::to_string(r.a);
    }
    check(fail.empty(),"funneltie: orders still open in a pillar field:"+fail);
}
void deadend() {
    if(std::getenv("DEADEND_W")||std::getenv("DEADEND_N")) {
        const auto r=deadendRun(envInt("DEADEND_W",4),envInt("DEADEND_N",120),envInt("DEADEND_ROOM",0)!=0,envInt("DEADEND_SQUAD",0)!=0,
            envInt("DEADEND_RETAIL",0)!=0,true);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        return;
    }
    ProbeRun w4;
    for(int retail:{0,1})for(int width:{2,4,6})for(int count:{40,120})for(int room:{0,1})for(int squad:{0,1}) {
        const auto r=deadendRun(width,count,room!=0,squad!=0,retail!=0,true);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        std::fflush(stdout);
        if(!retail&&width==4&&count==120&&!room&&!squad)w4=r;
    }
    checkRepeatable("deadend",[&](bool serial) {return deadendRun(4,120,false,false,false,serial);},w4);
}

// tail (AR-01): one same-point order on a blank map; how many orders stay
// open on bodies that have stood still (speed 0) for 900+ ticks. Cases: open
// 304 and 570 bodies about 2.6k px, 380 bodies to a map corner 8.9k px away,
// and a wave (150 settled at the point first, then 304 more).
ProbeRun tailRun(const std::string& name,bool serial) {
    const auto type=mover(2);
    const bool open=name=="open304"||name=="open570",corner=name=="corner380",wave=name=="wave";
    check(open||corner||wave,"unknown tail case");
    const int n=name=="open570"?570:corner?380:304;
    const int ticks=name=="open304"||wave?8000:12000;
    Fixture f(corner?600:640,corner?300:220,serial);f.publish();
    std::vector<int> ids,first;
    float tx=0,tz=0;
    if(corner) {
        for(int i=0;i<n;++i)ids.push_back(f.spawn(type,500+(i/20)*3,200+(i%20)*3));
        f.start();
        tx=12*16;tz=12*16;
    } else if(open) {
        const int cols=(n+15)/16;
        for(int i=0;i<n;++i)ids.push_back(f.spawn(type,40+(i/16)*3,80+(i%16)*3));
        f.start();
        tx=float((40+cols*3/2+160)*16);tz=float((80+24)*16);
    } else {
        for(int i=0;i<150;++i)first.push_back(f.spawn(type,40+(i/16)*3,80+(i%16)*3));
        f.start();
        tx=float((40+30+160)*16);tz=float((80+24)*16);
        for(int id:first)f.world.order(id,tx,tz,false);
        for(int t=0;t<4000;++t)f.world.tick(1.f/30);
        for(int i=0;i<n;++i)ids.push_back(f.spawn(type,40+(i/16)*3,80+(i%16)*3));
        for(int t=0;t<30;++t)f.world.tick(1.f/30);
    }
    int firstSettled=0;for(int id:first)firstSettled+=f.world.unit(id)->orders.empty();
    for(int id:ids)f.world.order(id,tx,tz,false);
    auto* nav=f.world.legionNavigator();
    std::map<int,int> streak,maxStreak,arrivedAt;
    for(int t=1;t<=ticks;++t) {
        f.world.tick(1.f/30);
        for(int id:ids) {
            const auto& u=*f.world.unit(id);
            if(u.orders.empty()) {arrivedAt.try_emplace(id,t);streak[id]=0;continue;}
            if(u.speed.v>0)streak[id]=0;else maxStreak[id]=std::max(maxStreak[id],++streak[id]);
        }
    }
    int held900Ever=0,openStill900=0,active=0,holdingNow=0,longest=0;
    std::vector<int> at;std::vector<double> dist;
    for(int id:ids) {
        const auto& u=*f.world.unit(id);
        held900Ever+=maxStreak[id]>=900;longest=std::max(longest,maxStreak[id]);
        if(u.orders.empty()) {at.push_back(arrivedAt[id]);continue;}
        ++active;openStill900+=streak[id]>=900;holdingNow+=nav&&nav->unitState(id)==2;
        dist.push_back(std::hypot(u.x.toFloat()-tx,u.z.toFloat()-tz));
    }
    char buf[400];
    std::snprintf(buf,sizeof buf,"tail case=%s n=%d ticks=%d%s gone=%zu/%d arrival p50=%d p90=%d max=%d active=%d holding=%d open_still900=%d"
        " held900_ever=%d longest_still=%d active_dist max=%.0f",
        name.c_str(),n,ticks,wave?(" wave1_settled="+std::to_string(firstSettled)+"/150").c_str():"",at.size(),n,quantile(at,0.5),quantile(at,0.9),
        at.empty()?-1:*std::max_element(at.begin(),at.end()),active,holdingNow,openStill900,held900Ever,longest,dist.empty()?0.0:quantile(dist,1.0));
    return {std::string(buf)+t1Counters(f.world),f.world.stateHash()};
}
void tail() {
    if(const char* only=std::getenv("TAIL_CASE")) {
        const auto r=tailRun(only,true);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        return;
    }
    ProbeRun open304;
    for(const char* name:{"open304","open570","corner380","wave"}) {
        const auto r=tailRun(name,true);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        std::fflush(stdout);
        if(std::string_view(name)=="open304")open304=r;
    }
    checkRepeatable("tail",[&](bool serial) {return tailRun("open304",serial);},open304);
}

// settlelatency (AR-05): squadformation's scenario at rest stride 1/2/4
// (LegionNavigator::setRestStrideForTest). Order-completion ticks, and the
// gap from each body's last position change to its order completing.
ProbeRun settlelatencyRun(int stride,bool serial) {
    LegionNavigator::setRestStrideForTest(stride);
    Fixture f(220,100,serial);
    f.rect(96,0,4,46);f.rect(96,52,4,48);
    f.rect(150,38,3,3);f.rect(168,58,4,2);f.rect(140,60,2,5);f.rect(175,40,2,6);
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids;
    for(int i=0;i<120;++i)ids.push_back(f.spawn(type,14+(i%12)*3,32+(i/12)*4));
    f.start();
    TypeRegistry registry;
    auto command=[&](tak::net::Cmd kind,int id,float x,float z) {
        tak::net::Command c;c.kind=kind;c.player=0;c.unitId=id;c.targetId=-1;c.x=x;c.z=z;c.queue=0;
        applyCommand(f.world,registry,c);
    };
    for(int id:ids)command(tak::net::Cmd::SetSquad,id,0,0);
    for(int t=0;t<600;++t)f.world.tick(1.f/30);
    const float px=160*16,pz=50*16;
    for(int id:ids)command(tak::net::Cmd::Move,id,px,pz);
    std::map<int,int> doneAt,lastTravel;
    std::map<int,std::pair<int32_t,int32_t>> was;
    for(int id:ids) {const auto& u=*f.world.unit(id);was[id]={u.x.v,u.z.v};lastTravel[id]=-1;}
    constexpr int kTicks=12000;
    for(int t=0;t<kTicks;++t) {
        f.world.tick(1.f/30);
        for(int id:ids) {
            if(doneAt.count(id))continue;
            const auto& u=*f.world.unit(id);
            if(u.x.v!=was[id].first||u.z.v!=was[id].second) {lastTravel[id]=t;was[id]={u.x.v,u.z.v};}
            if(u.orders.empty())doneAt[id]=t;
        }
    }
    LegionNavigator::setRestStrideForTest(2);
    std::vector<int> done,gap;
    int over500=0;
    for(auto& [id,t]:doneAt) {done.push_back(t);gap.push_back(t-lastTravel[id]);over500+=t-lastTravel[id]>500;}
    char buf[400];
    std::snprintf(buf,sizeof buf,"settlelatency stride=%d settled=%zu/%zu done_tick p50=%d p75=%d p90=%d p95=%d max=%d gap p50=%d p90=%d max=%d over500=%d",
        stride,doneAt.size(),ids.size(),quantile(done,0.5),quantile(done,0.75),quantile(done,0.9),quantile(done,0.95),
        done.empty()?-1:*std::max_element(done.begin(),done.end()),quantile(gap,0.5),quantile(gap,0.9),
        gap.empty()?-1:*std::max_element(gap.begin(),gap.end()),over500);
    return {std::string(buf)+t1Counters(f.world),f.world.stateHash()};
}
void settlelatency() {
    ProbeRun base;
    for(int stride:{1,2,4}) {
        const auto r=settlelatencyRun(stride,true);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        if(stride==2)base=r;
    }
    checkRepeatable("settlelatency",[&](bool serial) {return settlelatencyRun(2,serial);},base);
}

// doorplug: order A (60 bodies) settles beyond an 8-cell door (its point
// `ax` cells east: 112 = its area spills into the door mouth, 124 = clear of
// it); then order B (60 more, from the same start) is sent through the door
// to a point far beyond. How many of B cross, arrive, or complete short of
// the door, and how many of A's settled bodies B displaced.
ProbeRun doorplugRun(int ax,bool serial) {
    Fixture f(240,100,serial);
    f.rect(100,0,4,46);f.rect(100,54,4,46);   // door z 46..53
    f.publish();
    const auto type=mover(2);
    std::vector<int> a,b;
    for(int i=0;i<60;++i)a.push_back(f.spawn(type,14+(i%10)*3,36+(i/10)*4));
    f.start();
    for(int id:a)f.world.order(id,float(ax*16),50*16,false);
    constexpr int kSettle=3000,kPass=6000;
    for(int t=0;t<kSettle;++t)f.world.tick(1.f/30);
    int aSettled=0,aBeyond=0;
    std::map<int,std::pair<int32_t,int32_t>> aAt;
    for(int id:a) {const auto& u=*f.world.unit(id);aSettled+=u.orders.empty();aBeyond+=u.x.toFloat()>=104*16;aAt[id]={u.x.v,u.z.v};}
    for(int i=0;i<60;++i)b.push_back(f.spawn(type,14+(i%10)*3,36+(i/10)*4));
    f.world.tick(1.f/30);
    const float bx=200*16,bz=50*16;
    for(int id:b)f.world.order(id,bx,bz,false);
    std::map<int,int> crossedAt,doneAt;
    int shortOfDoor=0;
    for(int t=0;t<kPass;++t) {
        f.world.tick(1.f/30);
        for(int id:b) {
            const auto& u=*f.world.unit(id);
            if(!crossedAt.count(id)&&u.x.toFloat()>=104*16)crossedAt[id]=t;
            if(!doneAt.count(id)&&u.orders.empty()) {doneAt[id]=t;shortOfDoor+=u.x.toFloat()<100*16;}
        }
    }
    int aMoved=0,bMoving=0,bHolding=0;
    for(int id:a) {const auto& u=*f.world.unit(id);aMoved+=std::abs(u.x.v-aAt[id].first)+std::abs(u.z.v-aAt[id].second)>(32<<16);}
    auto* nav=f.world.legionNavigator();
    std::vector<int> crossed,done;std::vector<double> bX;
    for(int id:b) {const auto& u=*f.world.unit(id);bX.push_back(u.x.toFloat()/16);if(!u.orders.empty()) {bMoving+=u.speed>Fixed();bHolding+=nav->unitState(id)==2;}}
    for(auto& [id,t]:crossedAt)crossed.push_back(t);
    for(auto& [id,t]:doneAt)done.push_back(t);
    char buf[512];
    std::snprintf(buf,sizeof buf,"doorplug a_point_x=%d a_settled=%d/60 a_beyond_door=%d b_crossed=%zu/60 cross_tick p50=%d p90=%d b_done=%zu done_tick p50=%d p90=%d"
        " b_done_short_of_door=%d b_open=%zu b_moving=%d b_holding=%d b_x p10/med/p90=%.0f/%.0f/%.0f a_displaced=%d",
        ax,aSettled,aBeyond,crossedAt.size(),quantile(crossed,0.5),quantile(crossed,0.9),doneAt.size(),quantile(done,0.5),quantile(done,0.9),
        shortOfDoor,b.size()-doneAt.size(),bMoving,bHolding,quantile(bX,0.1),quantile(bX,0.5),quantile(bX,0.9),aMoved);
    return {std::string(buf)+t1Counters(f.world),f.world.stateHash()};
}
void doorplug() {
    ProbeRun mouth;
    for(int ax:{112,124}) {
        const auto r=doorplugRun(ax,true);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        if(ax==112)mouth=r;
    }
    checkRepeatable("doorplug",[](bool serial) {return doorplugRun(112,serial);},mouth);
}

// ---- W2 step 0 probes (PLAN 3.4 AR-03, 3.5 RB-04): measurement only. Like the T1
// probes they assert determinism (a repeat and a workers run give the same text
// and hash), never the W2 targets, which the later steps raise: the printed
// values are the "can fail" record and the commit message quotes them.

// AR-03: a formation (Alt+1 = squad -1) of 20 movers that also holds a member
// with no speed. FormAgg takes the slowest member's baseSpeed. Modes: 0 the
// movers alone, 1 a 4x4 structure, 2 a mover still under construction,
// 3 a Keep (canmove with maxVel 0), 4 the structure in a group (+1) squad,
// 5 a 6x6 builder factory (isBuilder, workerTime) in the house's place.
// Target (3.4): modes 1-3 reach 20/20 by 1350, as mode 0 does (1299).
ProbeRun structsquadRun(int mode,bool serial) {
    Fixture f(200,80,serial);f.publish();
    const auto type=mover(2);
    UnitType house{};house.id=house.name="legion-house";house.canMove=false;house.maxHp=100;house.footX=house.footZ=4;
    house.maxVel=Fixed();house.buildTime=1;
    UnitType keep=mover(4);keep.id=keep.name="legion-keep";keep.maxVel=Fixed();
    std::vector<int> ids;
    for(int i=0;i<20;++i)ids.push_back(f.spawn(type,14+(i%5)*3,30+(i/5)*4));
    int extra=-1;
    if(mode==1||mode==4)extra=f.spawn(house,40,50);
    UnitType factory=house;factory.id=factory.name="legion-factory-s";factory.footX=factory.footZ=6;factory.isBuilder=true;factory.workerTime=1000;
    if(mode==5)extra=f.spawn(factory,40,50);
    if(mode==2) {extra=f.spawn(type,40,50);}
    if(mode==3)extra=f.spawn(keep,40,50);
    f.start();
    if(mode==2)f.world.unit(extra)->underConstruction=true;
    TypeRegistry registry;
    auto command=[&](tak::net::Cmd kind,int id,int target,float x,float z) {
        tak::net::Command c;c.kind=kind;c.player=0;c.unitId=id;c.targetId=target;c.x=x;c.z=z;c.queue=0;
        applyCommand(f.world,registry,c);
    };
    for(int id:ids)command(tak::net::Cmd::SetSquad,id,mode==4?1:-1,0,0);
    if(extra>0)command(tak::net::Cmd::SetSquad,extra,mode==4?1:-1,0,0);
    for(int t=0;t<60;++t)f.world.tick(1.f/30);
    for(int id:ids)command(tak::net::Cmd::Move,id,0,150*16,40*16);
    int firstDone=-1,allDone=-1;
    for(int t=0;t<1500;++t) {
        f.world.tick(1.f/30);
        int done=0;for(int id:ids)done+=f.world.unit(id)->orders.empty();
        if(done&&firstDone<0)firstDone=t;
        if(done==int(ids.size())&&allDone<0)allDone=t;
    }
    int done=0;long meanX=0;
    for(int id:ids) {done+=f.world.unit(id)->orders.empty();meanX+=f.world.unit(id)->x.floorInt()/16;}
    ProbeRun r;char buf[200];
    std::snprintf(buf,sizeof buf,"structsquad mode=%d done=%d/%zu first_done=%d all_done=%d mean_x_cells=%ld (start 14-26, goal 150)",
        mode,done,ids.size(),firstDone,allDone,meanX/long(ids.size()));
    r.text=buf;r.hash=f.world.stateHash();r.a=done;r.b=allDone;return r;
}
// W2_REQUIRE=1 turns the W2 targets into asserts (off in ctest until the step that meets them lands).
bool requireW2() {return std::getenv("W2_REQUIRE")!=nullptr;}
void structsquad() {
    for(int mode=0;mode<=5;++mode) {
        const auto r=structsquadRun(mode,true);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        if(requireW2())check(r.a==20&&r.b>=0&&r.b<=1350,"AR-03: a formation with a structure or immobile member must arrive 20/20 by tick 1350");
        checkRepeatable("structsquad",[&](bool serial) {return structsquadRun(mode,serial);},r);
    }
}

// AR-03: a squad-member factory's produced units join its squad (the documented
// inheritance, sim.cpp "Auto-join"), the factory is a structure, and the army
// that comes out is ordered 80 cells away. mode 0: factory in no squad; 1: in
// formation -1; 2: in group +1. `direct` skips production: the movers stand
// west of the factory from the start (the audit's probe_factorysquad), all in
// the factory's squad. Target (3.4): modes 1 and 2 arrive as mode 0.
ProbeRun factorysquadRun(int mode,int movers,bool serial,bool direct=false) {
    Fixture f(200,80,serial);f.publish();
    const auto type=mover(2);
    UnitType factory{};factory.id=factory.name="legion-factory";factory.footX=6;factory.footZ=6;factory.maxHp=1000;
    factory.isBuilder=true;factory.workerTime=1000;factory.buildTime=1;
    factory.maxVel=Fixed();   // a structure: UnitType defaults maxVel to 1 (a slow 6x6 mover)
    const int fid=direct?f.spawn(factory,40,50):f.spawn(factory,30,40);
    std::vector<int> standing;
    if(direct)for(int i=0;i<movers;++i)standing.push_back(f.spawn(type,14+(i%4)*3,36+(i/4)*3));
    f.start();
    TypeRegistry registry;
    auto command=[&](tak::net::Cmd kind,int id,int target,float x,float z) {
        tak::net::Command c;c.kind=kind;c.player=0;c.unitId=id;c.targetId=target;c.x=x;c.z=z;c.queue=0;
        applyCommand(f.world,registry,c);
    };
    if(mode)command(tak::net::Cmd::SetSquad,fid,mode==1?-1:1,0,0);
    if(direct&&mode)for(int id:standing)command(tak::net::Cmd::SetSquad,id,mode==1?-1:1,0,0);
    if(!direct)f.world.setRepeat(fid,&type);
    std::vector<int> made=standing;int stopped=0;
    for(int t=0;t<4000&&!stopped&&!direct;++t) {
        f.world.tick(1.f/30);
        made.clear();
        for(const auto& u:f.world.units())if(u.id!=fid&&u.alive()&&!u.underConstruction&&u.type==&type)made.push_back(u.id);
        if(int(made.size())>=movers) {f.world.stop(fid);stopped=t;}
    }
    check(int(made.size())>=movers,"factorysquad: the factory produced no army");
    made.resize(size_t(movers));
    int squadded=0;for(int id:made)squadded+=f.world.unit(id)->squad!=0;
    for(int t=0;t<60;++t)f.world.tick(1.f/30);
    const float startX=f.world.unit(made.front())->x.toFloat();
    for(int id:made)command(tak::net::Cmd::Move,id,0,110*16,40*16);
    int allDone=-1;
    for(int t=0;t<3000;++t) {
        f.world.tick(1.f/30);
        int done=0;for(int id:made)done+=f.world.unit(id)->orders.empty();
        if(done==movers&&allDone<0)allDone=t;
    }
    int done=0;long moved=0;
    for(int id:made) {done+=f.world.unit(id)->orders.empty();moved+=long(f.world.unit(id)->x.toFloat()-startX)/16;}
    ProbeRun r;char buf[200];
    std::snprintf(buf,sizeof buf,"factorysquad%s mode=%d movers=%d squadded=%d done=%d/%d all_done=%d mean_moved_cells=%ld",
        direct?"-direct":"",mode,movers,squadded,done,movers,allDone,moved/long(movers));
    r.text=buf;r.hash=f.world.stateHash();r.a=done;r.b=allDone;return r;
}
void factorysquad() {
    for(bool direct:{false,true})for(int movers:{1,4,12})for(int mode=0;mode<=2;++mode) {
        const auto r=factorysquadRun(mode,movers,true,direct);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        if(requireW2()) {
            const auto base=factorysquadRun(0,movers,true,direct);
            check(r.a==movers&&r.b>=0&&r.b*100<=base.b*105,"AR-03: units of a squad-member factory must arrive as fast as without the squad");
        }
        if(movers==12)checkRepeatable("factorysquad",[&](bool serial) {return factorysquadRun(mode,movers,serial,direct);},r);
    }
}

// RB-04: a Legion builder on patrol meets a damaged own structure beside its route:
// the repair detour is an Order with patrolRepair. `retail_ticks` counts the ticks
// a patrol-repair leg is current while LegionNavigator::mission() says None
// (Retail's steering); `legion_ticks` the ticks Legion routes it;
// `retail_searches` the Retail path requests queued during the run.
// Target (3.5): retail_ticks 0, retail_searches 0, legion_ticks > 0, repaired and
// patrol resumed. The `-moving` variant repairs a mobile ally that walks out of
// reach (but stays inside the leash) once the repair has begun: RepairUnit's
// look re-installs the leg's rectangle round it, and the builder must follow.
ProbeRun patrolrepairRun(bool serial,bool moving) {
    Fixture f(128,96,serial);f.publish();
    UnitType builder{};builder.id=builder.name="legion-worker";builder.isBuilder=builder.canMove=builder.canReclaim=builder.canPatrol=true;
    builder.upright=true;builder.maxHp=100;builder.footX=builder.footZ=2;builder.maxVel=Fixed::fromInt(3);
    builder.accel=builder.brake=Fixed::fromInt(10);builder.turnRate=builder.turnInPlaceRate=10000;builder.halfCellTicks=3;
    builder.sight=160;builder.sightHeight=24;builder.workerTime=100;builder.buildDist=80;builder.storage=10000;builder.buildTime=1;
    UnitType lode{};lode.id=lode.name="legion-lode";lode.side="ARA";lode.maxHp=100;lode.maxVel=Fixed();
    lode.footX=lode.footZ=4;lode.buildTime=100;lode.buildCost=10;
    UnitType ally=builder;ally.id=ally.name="legion-ally";ally.isBuilder=false;ally.buildTime=100;ally.buildCost=10;
    builder.workerTime=moving?10:100;if(moving)builder.buildDist=16;
    const int id=f.spawn(builder,16,32);
    const int damaged=moving?f.spawn(ally,24,32):f.spawn(lode,40,37);
    f.start();
    f.world.unit(damaged)->hp=Fixed::fromInt(moving?20:25);
    if(!moving)f.world.blockFoot(lode,float((40+g_shift)*16),float((37+g_shift)*16),true);
    f.world.player(0).mana=10000;
    const auto searches0=f.world.pathStats().requests();
    f.world.patrol(id,64*16,32*16);
    bool repaired=false,resumed=false,started=false,moved=false;int repairTicks=0,retailTicks=0,legionTicks=0;
    float chase=0;Fixed x0,z0;
    for(int t=0;t<2400;++t) {
        f.world.tick(1.f/30);
        const auto& u=*f.world.unit(id);
        if(!u.orders.empty()) {
            const auto& leg=u.orders[World::currentLeg(u.orders)];
            if(leg.patrolRepair) {
                ++repairTicks;
                const auto* nav=f.world.legionNavigator();
                (nav&&nav->mission(u)!=LegionMission::None?legionTicks:retailTicks)++;
            }
        }
        // The ally walks off (4 cells across and 2 down, out of reach) once the repair has
        // begun; `chase_px` is how far the builder then walks before the repair is done.
        if(moved&&!repaired)chase=std::max(chase,std::hypot((u.x-x0).toFloat(),(u.z-z0).toFloat()));
        started|=f.world.unit(damaged)->hp>Fixed::fromInt(20);
        if(moving&&started&&!moved) {f.world.order(damaged,float((28+g_shift)*16),float((34+g_shift)*16),false);moved=true;x0=u.x;z0=u.z;}
        if(f.world.unit(damaged)->hp==Fixed::fromInt(100))repaired=true;
        if(repaired&&u.x.toFloat()>50*16)resumed=true;
    }
    const long searches=long(f.world.pathStats().requests()-searches0);
    int waypoints=0;for(const auto& o:f.world.unit(id)->orders)waypoints+=o.goal&&o.patrol;
    ProbeRun r;char buf[260];
    std::snprintf(buf,sizeof buf,"patrolrepair%s repaired=%d resumed=%d patrol_waypoints=%d repair_leg_ticks=%d retail_ticks=%d legion_ticks=%d retail_searches=%ld chase_px=%d",
        moving?"-moving":"",int(repaired),int(resumed),waypoints,repairTicks,retailTicks,legionTicks,searches,int(chase));
    r.text=buf;r.hash=f.world.stateHash();r.a=retailTicks+int(searches);r.b=legionTicks;r.c=repaired&&resumed&&waypoints==2&&(!moving||chase>=16);return r;
}
void patrolrepair() {
    for(bool moving:{false,true}) {
        const auto r=patrolrepairRun(true,moving);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        if(requireW2())check(r.a==0&&r.b>0&&r.c,"RB-04: a patrol-repair leg must be routed by Legion (0 Retail ticks and searches), the repair done and the patrol resumed");
        checkRepeatable(moving?"patrolrepair-moving":"patrolrepair",[&](bool serial) {return patrolrepairRun(serial,moving);},r);
    }
}
// ---- W7 step 0 fixtures (PLAN 4 W7 step 0) -----------------------------------
// rb02: the verifiers' probe, promoted. A 40-body group routes past a clot of Legion members
// that stands in its way: Trapped (an unreachable goal inside a walled ring; mode 1x), Holding
// (queued at a dead-end corridor mouth; mode 2x), or the same clot Stopped (the soft control,
// variant 2). Variant 0 has no clot. plug: six 4x4 bodies parked Trapped at a 3-cell gap that
// 2x2 bodies fit through. Measurement only: the numbers W7's Trapped-soft and approach-clearance
// steps move (RB-02: ring N=104 done@3000 7/40, N=208 20/40 by 6000, plug 0/40 in 9000, the
// Holding queue p50 2353 on the audit's head).
struct Rb02Result {int done=0,done3000=0,holding=0,p50=-1,p90=-1,max=-1;uint64_t hash=0;};
Rb02Result rb02Run(int mode,int blockN,bool serial=true) {
    const int kind=mode/10,variant=mode%10;
    Fixture f(260,120,serial);
    if(kind==1) {f.rect(128,44,16,16);f.open(130,46,12,12);}
    if(kind==2) {f.rect(128,0,4,46);f.rect(136,0,4,46);f.rect(128,0,12,2);}
    f.publish();
    const auto type=mover(2);
    std::vector<int> ids,block;
    for(int i=0;i<40;++i)ids.push_back(f.spawn(type,14+(i%8)*3,44+(i/8)*4));
    if(variant)for(int i=0;i<blockN;++i)block.push_back(f.spawn(type,100+(i%26)*2,72+(i/26)*2));
    f.start();
    const float bx=(kind==1?135.5f:133.5f)*16,bz=(kind==1?51.5f:6.f)*16;
    for(int id:block)f.world.order(id,bx,bz,false);
    for(int t=0;t<(kind==1?2400:3600);++t)f.world.tick(1.f/30);
    if(variant==2) {for(int id:block)f.world.stop(id);for(int t=0;t<120;++t)f.world.tick(1.f/30);}
    // The clot's member states when the followers set off (0 none .. 5 Trapped).
    int clot[8]={};
    for(int id:block)++clot[std::clamp(f.world.legionNavigator()->unitState(id),0,7)];
    for(int id:ids)f.world.order(id,230*16.f,52*16.f,false);
    std::map<int,int> doneAt;
    Rb02Result r;
    for(int t=0;t<6000;++t) {
        f.world.tick(1.f/30);
        for(int id:ids)if(!doneAt.count(id)&&f.world.unit(id)->orders.empty())doneAt[id]=t;
        if(t==2999)r.done3000=int(doneAt.size());
    }
    std::vector<int> ticks;for(auto& [id,t]:doneAt)ticks.push_back(t);std::sort(ticks.begin(),ticks.end());
    auto pct=[&](double q) {return ticks.empty()?-1:ticks[size_t(std::min<double>(ticks.size()-1,q*ticks.size()))];};
    for(int id:ids)r.holding+=!f.world.unit(id)->orders.empty();
    r.done=int(doneAt.size());r.p50=pct(0.5);r.p90=pct(0.9);r.max=ticks.empty()?-1:ticks.back();r.hash=f.world.stateHash();
    const auto st=f.world.legionStats();
    std::printf("rb02 %s mode=%d block=%d done=%d/40 done@3000=%d holding=%d done_tick p50=%d p90=%d max=%d detours=%llu parts=%llu clot_none=%d clot_holding=%d clot_waiting=%d clot_arrived=%d clot_trapped=%d hash=%016llx\n",
        kind==1?"ring":"corridor",mode,variant?blockN:0,r.done,r.done3000,r.holding,r.p50,r.p90,r.max,
        (unsigned long long)st.detours,(unsigned long long)st.parts,clot[0],clot[2],clot[3],clot[4],clot[5],(unsigned long long)r.hash);
    return r;
}
Rb02Result rb02PlugRun(int mode,bool serial=true) {
    Fixture f(260,120,serial);
    f.rect(150,0,2,120);f.open(150,50,2,3);                // a 3-cell gap: 2x2 fits, 4x4 does not
    f.publish();
    const auto small=mover(2),big=mover(4);
    std::vector<int> ids,plug;
    for(int i=0;i<40;++i)ids.push_back(f.spawn(small,14+(i%8)*3,44+(i/8)*4));
    if(mode==1)for(int i=0;i<6;++i)plug.push_back(f.spawn(big,120+(i%3)*5,45+(i/3)*5));
    f.start();
    for(int id:plug)f.world.order(id,200*16.f,51*16.f,false);
    for(int t=0;t<1200;++t)f.world.tick(1.f/30);
    for(int id:ids)f.world.order(id,230*16.f,52*16.f,false);
    std::map<int,int> doneAt;
    for(int t=0;t<9000;++t) {
        f.world.tick(1.f/30);
        for(int id:ids)if(!doneAt.count(id)&&f.world.unit(id)->orders.empty())doneAt[id]=t;
    }
    std::vector<int> ticks;for(auto& [id,t]:doneAt)ticks.push_back(t);std::sort(ticks.begin(),ticks.end());
    auto pct=[&](double q) {return ticks.empty()?-1:ticks[size_t(std::min<double>(ticks.size()-1,q*ticks.size()))];};
    Rb02Result r;
    int through=0;
    for(int id:ids) {const auto& u=*f.world.unit(id);r.holding+=!u.orders.empty();through+=u.x.toFloat()/16>152;}
    r.done=int(doneAt.size());r.p50=pct(0.5);r.p90=pct(0.9);r.max=ticks.empty()?-1:ticks.back();r.hash=f.world.stateHash();
    const auto st=f.world.legionStats();
    std::printf("rb02 plug mode=%d done=%d/40 holding=%d through_gap=%d done_tick p50=%d p90=%d max=%d detours=%llu parts=%llu trapped=%llu hash=%016llx\n",
        mode,r.done,r.holding,through,r.p50,r.p90,r.max,(unsigned long long)st.detours,(unsigned long long)st.parts,
        (unsigned long long)st.trapped,(unsigned long long)r.hash);
    return r;
}
void rb02() {
    const bool full=std::getenv("TAK_RB02_FULL")!=nullptr;
    const int only=std::getenv("TAK_RB02_MODE")?std::atoi(std::getenv("TAK_RB02_MODE")):-1;
    auto want=[&](int mode) {return only<0?true:only==mode;};
    const int variants[]={1,2,0};
    for(int v:variants) {
        if(v!=1&&!full)continue;
        for(int n:{52,104,208})if(want(10+v)&&(v||n==52))rb02Run(10+v,n);
        if(want(20+v))rb02Run(20+v,208);
    }
    if(want(100))rb02PlugRun(1);
    if(full&&want(101))rb02PlugRun(0);
    // Determinism, on the cheapest row: the same run again and with workers.
    const auto a=rb02Run(11,52),b=rb02Run(11,52,false);
    check(a.hash==b.hash&&a.done==b.done,"rb02: serial and workers differ");
}

// The W7 stream fixtures: groups that cross, meet head on or pass an enemy, in the awareness
// code's two arms (ON: the base; OFF: LegionNavigator::setAwareOffForTest). Each stream is one
// command (its click at `issue`); the report is the arrival tick of everyone, the standing share,
// A x B contacts, the longest run a body stood with orders (hold_max), the Stats counters W7
// step 0 exports (parts, detours, aware encounters / builds / latency, give-way) and the
// route-behind metric (legion_observe::BehindProbe) of each (later, earlier) pair.
struct Stream {
    int count,cols,x,z,pitch,gx,gz,issue,player=0,sight=4096;
};
struct StreamSpec {
    const char* name;int w,h;
    std::vector<std::array<int,4>> walls;     // x z w h
    std::vector<Stream> streams;
    std::vector<std::pair<int,int>> behind;   // (later stream, earlier stream)
    int ticks;bool awareOff=false;
};
struct AwareOffGuard {
    explicit AwareOffGuard(bool off) {LegionNavigator::setAwareOffForTest(off);}
    ~AwareOffGuard() {LegionNavigator::setAwareOffForTest(false);}
};
ProbeRun streamRun(const StreamSpec& sp,bool serial) {
    AwareOffGuard guard(sp.awareOff);
    Fixture f(sp.w,sp.h,serial);
    for(const auto& r:sp.walls)f.rect(r[0],r[1],r[2],r[3]);
    f.publish();
    std::vector<std::vector<int>> ids(sp.streams.size());
    std::vector<int> all,issueOf;
    std::vector<UnitType> types(sp.streams.size());   // the world keeps a pointer to each type
    for(size_t s=0;s<sp.streams.size();++s) {
        const auto& st=sp.streams[s];
        auto& type=types[s];type=mover(2);type.id=type.name="legion-foot-2s"+std::to_string(s);type.sight=st.sight;
        for(int i=0;i<st.count;++i)
            ids[s].push_back(f.spawn(type,st.x+(i%st.cols)*st.pitch,st.z+(i/st.cols)*st.pitch,st.player));
        all.insert(all.end(),ids[s].begin(),ids[s].end());
        issueOf.insert(issueOf.end(),ids[s].size(),st.issue);
    }
    f.start();
    Watch motion;
    auto streamName=[](size_t s) {return std::string(1,char('A'+s));};
    for(size_t s=0;s<ids.size();++s) {
        tak::legion_observe::Group g;g.name=streamName(s);g.ids=ids[s];motion.cfg.groups.push_back(g);
        for(size_t t=s+1;t<ids.size();++t)motion.cfg.pairs.push_back({streamName(s),streamName(t),2});
    }
    motion.cfg.decisionEvery=10;
    std::vector<tak::legion_observe::BehindProbe> behind;
    for(const auto& [late,early] : sp.behind) {
        tak::legion_observe::BehindProbe bp;bp.name=streamName(size_t(late))+"_after_"+streamName(size_t(early));
        for(int id:ids[size_t(late)])bp.late.push_back({id,int64_t(sp.streams[size_t(late)].gx)*16,int64_t(sp.streams[size_t(late)].gz)*16});
        bp.early=ids[size_t(early)];
        behind.push_back(std::move(bp));
    }
    std::map<int,std::pair<int32_t,int32_t>> prev;std::map<int,int> run;
    int holdMax=0,done=-1,arrived=0;
    for(int t=0;t<sp.ticks;++t) {
        for(size_t s=0;s<sp.streams.size();++s)
            if(sp.streams[s].issue==t)
                for(int id:ids[s])f.world.order(id,float(sp.streams[s].gx*16),float(sp.streams[s].gz*16),false);
        f.world.tick(1.f/30);motion.observe(f.world,all);
        for(auto& b:behind)b.sample(f.world);
        arrived=0;
        for(size_t q=0;q<all.size();++q) {
            const int id=all[q];
            const auto& u=*f.world.unit(id);
            const bool issued=t>=issueOf[q],over=issued&&(u.orders.empty()||!u.alive());
            arrived+=over;
            const auto last=prev.find(id);
            if(issued&&!over&&last!=prev.end()) {
                const int64_t dx=int64_t(u.x.v)-last->second.first,dz=int64_t(u.z.v)-last->second.second,cap=int64_t(u.baseSpeed.v)/8;
                int& r=run[id];
                if(dx*dx+dz*dz<=cap*cap)holdMax=std::max(holdMax,++r);else r=0;
            }
            prev[id]={u.x.v,u.z.v};
        }
        if(arrived==int(all.size())) {done=t;break;}
    }
    using tak::legion_observe::Observer;
    const auto k=motion.keys();
    const auto st=f.world.legionStats();
    char buf[1800];int n=0;
    n+=std::snprintf(buf+n,sizeof buf-size_t(n),"%s aware=%s arrived=%d/%zu done=%d stopped=%.1f%% contacts_permille=%.2f hold_max=%d spins=%llu reversals=%llu",
        sp.name,sp.awareOff?"off":"on",arrived,all.size(),done,double(Observer::get(k,"stopped_permille",0))/10,
        double(Observer::get(k,"pair.A.B.permille_x100",0))/100,holdMax,(unsigned long long)motion.spins(),(unsigned long long)motion.reversals());
    n+=std::snprintf(buf+n,sizeof buf-size_t(n)," parts=%llu detours=%llu aware_encounters=%llu replans=%llu builds=%llu latency_max=%llu latency_mean=%llu giveway_ticks=%llu giveway_timeouts=%llu",
        (unsigned long long)st.parts,(unsigned long long)st.detours,(unsigned long long)st.awareEncounters,(unsigned long long)st.awareReplans,
        (unsigned long long)st.awareBuilds,(unsigned long long)st.awareLatencyMax,
        (unsigned long long)(st.awareLatencyN?st.awareLatencySum/st.awareLatencyN:0),(unsigned long long)st.giveWayTicks,(unsigned long long)st.giveWayTimeouts);
    tak::legion_observe::Keys bk;for(const auto& b:behind)b.report(bk);
    for(const auto& [key,v]:bk) {
        const auto dot=key.find('.',7);   // "behind.NAME.metric"
        const auto metric=key.substr(dot+1);
        if(metric=="members"||metric=="straight_cells")continue;
        n+=std::snprintf(buf+n,sizeof buf-size_t(n)," %s=%lld",(key.substr(7,dot-7)+"."+key.substr(dot+1)).c_str(),(long long)v);
        if(n>=int(sizeof buf)-80)break;
    }
    ProbeRun r;r.text=buf;r.hash=f.world.stateHash();r.a=done;r.b=arrived;r.c=int(all.size());
    return r;
}
// Runs one spec in both arms and prints; determinism on the ON arm when asked (W7_DET=1, or always when cheap).
void streamCase(StreamSpec sp,bool determinism) {
    for(bool off:{false,true}) {
        sp.awareOff=off;
        const auto r=streamRun(sp,true);
        std::printf("%s hash=%016llx\n",r.text.c_str(),(unsigned long long)r.hash);
        check(r.a>=0||r.b>0,std::string(sp.name)+": nothing arrived");
        if(determinism&&!off)checkRepeatable(sp.name,[&](bool serial) {auto s=sp;s.awareOff=false;return streamRun(s,serial);},r);
    }
}
// awarebig: the MV-09 verifier's 768x768 maps -- 24 against 24 crossing, head on in a 56-cell corridor, and an
// enemy head on in the same corridor. Far larger than aware's 200x140: the awareness corridor tests, the whole-map
// field builds (the 48/66-tick aware refresh) and the encounter all happen at 700-cell distances.
void awarebig() {
    const char* only=std::getenv("AWAREBIG_KIND");
    const bool det=std::getenv("W7_DET")!=nullptr;
    auto want=[&](const char* k) {return !only||std::string(only)==k;};
    const std::vector<std::array<int,4>> corridor{{0,355,768,2},{0,413,768,2}};
    if(want("cross"))
        streamCase({"awarebig-cross",768,768,{},{{24,6,40,380,3,720,386,0},{24,6,380,720,3,386,40,100}},{{1,0}},9000},det);
    if(want("headon"))
        streamCase({"awarebig-headon",768,768,corridor,{{24,6,40,372,3,730,386,0},{24,6,728,372,3,38,386,100}},{{1,0}},9000},det);
    if(want("enemy"))
        streamCase({"awarebig-enemy",768,768,corridor,{{24,6,40,372,3,730,386,0},{24,6,728,372,3,38,386,100,1}},{{1,0}},9000},det);
}
// awaredense: two formations of 200 head on in a 40-cell corridor, 17 bodies across (34 of 40 cells, 85% fill), the
// density MV-03 fights at: is the formation path also sound when no half-band fits?
void awaredense() {
    const std::vector<std::array<int,4>> corridor{{0,40,400,2},{0,82,400,2}};
    streamCase({"awaredense",400,124,corridor,{{200,17,20,45,2,380,62,0},{200,17,350,45,2,20,62,100}},{{1,0}},9000},
        std::getenv("W7_DET")!=nullptr);
}
// crossthree: three 24-body streams whose lines cross at one point 60 degrees apart (angles 0, 120, 240 through the
// centre), commands 60 ticks apart. A cycle of give-ways among three streams must resolve: report hold_max (no hold
// beyond 600 ticks) and the give-way counters.
void crossthree() {
    const int cx=150,cz=150,r=110;
    auto at=[&](double deg,double sign,int& x,int& z) {
        const double a=deg*3.14159265358979/180.0;
        x=cx+int(std::lround(sign*r*std::cos(a)))-8;z=cz+int(std::lround(sign*r*std::sin(a)))-5;   // block origin, centred about 8x6 cells
    };
    std::vector<Stream> streams;
    for(int i=0;i<3;++i) {
        int sx,sz,gx,gz;at(120.0*i,1,sx,sz);at(120.0*i,-1,gx,gz);
        streams.push_back({24,6,sx,sz,3,gx+8,gz+5,60*i});
    }
    streamCase({"crossthree",300,300,{},streams,{{1,0},{2,0},{2,1}},7000},true);
}
// crosslong: a stream of 200 (20 bodies long = 60 cells, 10 across) crossing a 24-body group at right angles --
// the long stream the later group cannot wait out (decision 5: it routes behind the tail, no wait at the edge).
void crosslong() {
    streamCase({"crosslong",300,200,{},{{200,20,10,95,3,285,100,0},{24,6,142,180,3,150,10,400}},{{1,0}},6000},
        std::getenv("W7_DET")!=nullptr);
}

// Settling beside bodies of another footprint (2026-10-14 Legion review, item 2). The settle rule's queue test
// read a ring body of another footprint at its centre converted with the waiting body's own footprint: half a
// cell off where the parities differ, and in the middle of a big body, which can read a step above the waiting
// body while its cells reach farther down the field. Bodies of two footprints on a grid (`rows` to a column,
// `pitch` cells apart, column-major; `layout` gives each one's footprint), one click to a point 100 cells east.
// On the parent a body of the smaller footprint ends among settled bodies of the other, the line to its own slot
// blocked by one of them, and no ring body reads nearer, so it never queues and its order never ends: the 2x2
// second in the 4x4 crowd's fifth column stops against a settled 4x4 read 1 above it, though that 4x4 stands
// nearer the click and covers origins 13 below it; the 2x2 last in the 3x3/2x2 crowd's fourth column stops
// against a settled 3x3 read 1 above it, whose origins reach 8 below. Every order must end.
struct MixedSettleRun {int done=0,last=-1;std::string open;uint64_t hash=0;};
MixedSettleRun mixedsettleRun(std::string_view layout,int rows,int pitch,int ticks=4000) {
    Fixture f(200,120);f.publish();
    const std::array<UnitType,4> types{mover(1),mover(2),mover(3),mover(4)};
    std::vector<int> ids;
    for(size_t i=0;i<layout.size();++i) {
        const int foot=layout[i]-'0',x=20+int(i)/rows*pitch,z=60-rows*pitch/2+int(i)%rows*pitch;
        const int id=f.world.spawn(&types[size_t(foot-1)],float(x*16+foot*8),float(z*16+foot*8),std::nullopt,0);
        check(id>0,"spawn failed");ids.push_back(id);
    }
    f.start();
    for(int id:ids)f.world.order(id,120*16,60*16,false);
    MixedSettleRun r;
    for(int t=0;t<ticks&&r.done<int(ids.size());++t) {
        f.world.tick(1.f/30);
        int done=0;for(int id:ids)done+=f.world.unit(id)->orders.empty();
        if(done>r.done)r.last=t;
        r.done=done;
    }
    for(size_t i=0;i<ids.size();++i)
        if(!f.world.unit(ids[i])->orders.empty())r.open+=" "+std::to_string(ids[i])+"("+layout[i]+"x"+layout[i]+")";
    if(std::getenv("TAK_MIXDUMP"))   // every body at the end: id, footprint, centre px, open order, Legion state, slot
        for(size_t i=0;i<ids.size();++i) {
            const auto& u=*f.world.unit(ids[i]);const auto* nav=f.world.legionNavigator();
            int32_t sx=-(1<<16),sz=-(1<<16);if(nav)nav->goalPoint(u.id,sx,sz);
            std::printf("MIXDUMP id=%d f=%d x=%d z=%d open=%d st=%d sx=%d sz=%d\n",u.id,u.type->footX,int(u.x.v>>16),int(u.z.v>>16),
                        int(!u.orders.empty()),nav?nav->unitState(u.id):0,sx>>16,sz>>16);
        }
    r.hash=f.world.stateHash();
    return r;
}
void mixedsettle() {
    struct Case {const char* name;std::string_view layout;int rows,pitch;};
    const Case cases[]={
        {"4x4/2x2","444424442444424424444444444424442444424442444244424442244444444444444444",9,5},
        {"3x3/2x2","2332233232332233322232332333233223332322322232322223333332233323233222232323323323223232232322222323",10,3}};
    std::string open;
    for(const auto& c:cases) {
        const auto r=mixedsettleRun(c.layout,c.rows,c.pitch);
        std::printf("mixedsettle %s bodies=%zu done=%d last_done=%d open:%s hash=%016llx\n",c.name,c.layout.size(),r.done,r.last,
            r.open.empty()?" none":r.open.c_str(),(unsigned long long)r.hash);
        if(!r.open.empty())open+=std::string(" ")+c.name;
    }
    check(open.empty(),"mixedsettle: an order beside settled bodies of another footprint never ended:"+open);
}
// Random one-click formations of two footprints (Legion ~100 round 3, 2026-10-20): the screen the 2026-10-14
// review ran on a scratch harness (12,800 layouts), kept as a case. Layout `seed` (splitmix64): footprints
// 4x4/2x2, 4x4/3x3 or 3x3/2x2, 30 to 100 bodies, 10-90% of the larger, shuffled, in column order (the larger
// first) or alternating, 6 to 12 rows at a pitch of the larger footprint plus one, one click 100 cells east of
// the column. Every order must end within 5,000 ticks. TAK_MIXRANDOM=FROM,TO screens that range of seeds and
// reports each layout with an open order; without it the case runs the seeds listed below.
std::string mixedRandomLayout(uint64_t seed,int& rows,int& pitch) {
    uint64_t z=seed*0x9e3779b97f4a7c15ull+0x632be59bd9b4e019ull;
    auto next=[&z]() {z+=0x9e3779b97f4a7c15ull;uint64_t x=z;x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;x=(x^(x>>27))*0x94d049bb133111ebull;return x^(x>>31);};
    static const char pairs[3][2]={{'4','2'},{'4','3'},{'3','2'}};
    const auto& pair=pairs[next()%3];
    const int n=30+int(next()%71),big=10+int(next()%81),mode=int(next()%3);
    rows=6+int(next()%7);pitch=pair[0]-'0'+1;
    std::string layout;
    for(int i=0;i<n;++i)layout+=int(next()%100)<big?pair[0]:pair[1];
    if(mode==1)std::sort(layout.begin(),layout.end(),std::greater<char>());
    else if(mode==2)for(int i=0;i<n;++i)layout[size_t(i)]=(i/rows)%2?pair[1]:pair[0];
    return layout;
}
void mixedrandom() {
    // The two layouts the base left open (5717: a 4x4, id 4; 9651: a 3x3, id 9), each held off its own slot by a
    // settled arrival of its own footprint standing across it.
    std::vector<uint64_t> seeds={5717,9651};
    if(const char* range=std::getenv("TAK_MIXRANDOM")) {
        unsigned long long a=0,b=0;
        if(std::sscanf(range,"%llu,%llu",&a,&b)!=2||b<a)throw std::runtime_error("TAK_MIXRANDOM=FROM,TO");
        seeds.clear();for(unsigned long long k=a;k<=b;++k)seeds.push_back(k);
    }
    std::string open;
    for(const uint64_t seed:seeds) {
        int rows=0,pitch=0;
        const std::string layout=mixedRandomLayout(seed,rows,pitch);
        const auto r=mixedsettleRun(layout,rows,pitch,5000);
        if(!r.open.empty()||!std::getenv("TAK_MIXRANDOM"))
            std::printf("mixedrandom seed=%llu bodies=%zu rows=%d done=%d last_done=%d open:%s hash=%016llx\n",(unsigned long long)seed,
                        layout.size(),rows,r.done,r.last,r.open.empty()?" none":r.open.c_str(),(unsigned long long)r.hash);
        if(!r.open.empty())open+=" "+std::to_string(seed);
        std::fflush(stdout);
    }
    check(open.empty(),"mixedrandom: a layout kept an order open:"+open);
}

// Slots by arrival exchange within one movement plane (2026-10-14 Legion review, after the commit review of
// 1861aff9). farroom's room (tools/scenarios/farroom.scn: the door on its far side), its west part a pond behind an
// 8-a-cell beach, deep enough to stop a ground unit (maxWaterDepth 4) and not a hovercraft; farroom's block of 60,
// 2x2 ground units and 2x2 hovercraft alternating, clicked to the room's middle: one point, one group per movement
// class, every slot legal for the plane it was found on. A member that comes through the door takes the deepest
// slot a member still outside holds, here a hovercraft's in the pond. The exchange checked the footprint only, so a
// ground body took a slot in the water and gave it back on its next update (regoal), losing its own. Every member's
// goal must be legal for its own type on every tick.
void entryplanes() {
    constexpr int W=240,H=110;
    std::vector<uint8_t> heights(size_t(W)*H,100);
    for(int z=35;z<65;++z)for(int x=150;x<170;++x)heights[size_t(z)*W+x]=uint8_t(std::min(100,30+std::max(0,x-160)*8));
    Fixture f(W,H,true,heights);
    f.rect(148,33,34,2);f.rect(148,65,34,2);f.rect(148,35,2,30);f.rect(180,35,2,12);f.rect(180,53,2,12);
    f.publish();
    auto dry=mover(2);dry.id=dry.name="legion-foot-2-dry";dry.maxWaterDepth=4;
    const auto skim=hover(2);
    std::vector<int> ids;
    for(int i=0;i<60;++i)ids.push_back(f.spawn(i%2?skim:dry,14+(i%10)*3,36+(i/10)*4));
    f.start();
    auto* legion=f.world.legionNavigator();
    for(int id:ids)f.world.order(id,float(165*16),float(50*16),false);
    int illegal=0,first=-1,firstId=0,done=0,last=-1;
    for(int t=0;t<9000&&done<int(ids.size());++t) {
        f.world.tick(1.f/30);
        int d=0;
        for(int id:ids) {
            const auto& u=*f.world.unit(id);
            d+=u.orders.empty();
            int32_t gx=0,gz=0;
            if(!legion->goalPoint(id,gx,gz))continue;
            if(!legion->staticLegal(u,footprintOrigin(Fixed::raw(gx),2),footprintOrigin(Fixed::raw(gz),2))) {
                ++illegal;
                if(first<0) {first=t;firstId=id;}
            }
        }
        if(d>done)last=t;
        done=d;
    }
    std::printf("entryplanes illegal_goal_member_ticks=%d first=%d@%d done=%d/%zu last_done=%d hash=%016llx\n",illegal,firstId,
                first,done,ids.size(),last,(unsigned long long)f.world.stateHash());
    check(illegal==0,"entryplanes: a member held a goal its own type cannot stand on (a slot exchanged across movement planes)");
}

// Destination markers (LegionNavigator::destinationOf, display only; the client's drawUnitSpots). One mixed-speed click
// of 18 bodies (2x2 fast, 2x2 at half speed, 3x3 at three quarters), which takes an arrival layout: every member that
// holds a spot reports its own (no two members the same on any tick), every body reports one before it settles, and
// each body settles on the last spot it reported. A unit before its order, and every unit of the same click in Retail
// pathfinding, reports none. Reading the spots every tick leaves the state hash as it is without them.
struct DestinationsRun {uint64_t hash=0;int done=0,shared=0,never=0,off=0,earlyTrue=0;std::string offIds;};
DestinationsRun destinationsRun(PathfindingMode mode,bool read) {
    Fixture f(200,120);f.world.setPathfindingMode(mode);f.publish();
    auto fast=mover(2);auto slow=mover(2);slow.id=slow.name="legion-foot-2-slow";slow.maxVel=Fixed::raw(117964/2);
    auto mid=mover(3);mid.maxVel=Fixed::raw(117964*3/4);
    std::vector<int> ids;
    for(int i=0;i<18;++i) {
        const UnitType& t=i%3==0?fast:i%3==1?slow:mid;
        ids.push_back(f.spawn(t,20+(i/6)*5,48+(i%6)*5));
    }
    f.start();
    DestinationsRun r;
    Fixed x,z;
    if(read)for(int id:ids)r.earlyTrue+=f.world.legionDestination(id,x,z);
    for(int id:ids)f.world.order(id,120*16,60*16,false);
    std::map<int,std::pair<int32_t,int32_t>> last;
    for(int t=0;t<5000&&r.done<int(ids.size());++t) {
        f.world.tick(1.f/30);
        int done=0;
        std::set<std::pair<int32_t,int32_t>> seen;
        for(int id:ids) {
            done+=f.world.unit(id)->orders.empty();
            if(!read||!f.world.legionDestination(id,x,z))continue;
            if(!seen.insert({x.v,z.v}).second)++r.shared;
            last[id]={x.v,z.v};
        }
        r.done=done;
    }
    if(read)for(int id:ids) {
        const auto& u=*f.world.unit(id);
        const auto found=last.find(id);
        if(found==last.end()) {++r.never;continue;}
        // A spot is a footprint's centre; the body settles on it to within a pixel.
        if(std::abs(u.x.v-found->second.first)>(1<<16)||std::abs(u.z.v-found->second.second)>(1<<16)) {
            ++r.off;r.offIds+=" "+std::to_string(id)+"@("+std::to_string(u.x.v>>16)+","+std::to_string(u.z.v>>16)+")!=("+
                std::to_string(found->second.first>>16)+","+std::to_string(found->second.second>>16)+")";
        }
    }
    r.hash=f.world.stateHash();
    return r;
}
void destinations() {
    const auto legion=destinationsRun(PathfindingMode::Legion,true);
    const auto plain=destinationsRun(PathfindingMode::Legion,false);
    const auto retail=destinationsRun(PathfindingMode::Retail,true);
    std::printf("destinations legion done=%d shared=%d never=%d off=%d%s early=%d hash=%016llx plain=%016llx retail never=%d early=%d\n",
        legion.done,legion.shared,legion.never,legion.off,legion.offIds.c_str(),legion.earlyTrue,(unsigned long long)legion.hash,
        (unsigned long long)plain.hash,retail.never,retail.earlyTrue);
    check(legion.done==18,"destinations: the click did not settle");
    check(legion.earlyTrue==0,"destinations: a unit with no order reported a spot");
    check(legion.shared==0,"destinations: two members reported the same spot");
    check(legion.never==0,"destinations: a body settled without reporting a spot");
    check(legion.off==0,"destinations: a body settled off the last spot it reported:"+legion.offIds);
    check(legion.hash==plain.hash,"destinations: reading the spots changed the state hash");
    check(retail.never==18&&retail.earlyTrue==0,"destinations: a Retail pathfinding unit reported a spot");
}

}

int main(int argc,char** argv) {
    LegionNavigator::setInstrumentProbes(true);   // lanegeom reads the vertex probe's counters
    const std::map<std::string_view,std::function<void()>> cases{
        {"clearance",clearance},{"unfinishedsite",unfinishedsite},{"groupreuse",groupreuse},{"jagged",jagged},{"trapped",trapped},{"longroute",longroute},
        {"crowdhold",crowdhold},{"replace",replace},{"unreachable",unreachable},{"quota",quota},{"pens",pens},
        {"determinism",determinism},{"formation",formation},{"slotblock",slotblock},
        {"staticblock",staticblock},{"deaths",deaths},{"deathsshared",deathsshared},{"splitgoal",splitgoal},{"farclick",farclick},{"churn",churn},
        {"approachhold",approachhold},{"approachopen",approachopen},
        {"churnfield",churnfield},{"planeincremental",planeincremental},{"planeprebuild",planeprebuild},{"penstale",penstale},{"legacyyield",legacyyield},{"approachchurn",approachchurn},{"lattice",lattice},{"wallend",wallend},
        {"navalclearance",navalclearance},{"navalisland",navalisland},{"hovershore",hovershore},{"navalmissions",navalmissions},{"squadformation",squadformation},{"formationreform",formationreform},{"formationreformai",formationreformai},{"formationslotreset",formationslotreset},
        {"pinwheel",pinwheel},{"lanegeom",lanegeom},{"landedflyers",landedflyers},{"mixedformation",mixedformation},{"flyerfront",flyerfront},{"liftflyers",liftflyers},{"aware",aware},
        {"pocket",pocket},{"deadend",deadend},{"tail",tail},{"settlelatency",settlelatency},{"doorplug",doorplug},
        {"staticidle",staticidle},{"b3events",b3events},{"pausedemand",pausedemand},{"planepin",planepin},{"chains",chains},{"chainschurn",chainschurn},{"chainplane",chainplane},{"structsquad",structsquad},{"factorysquad",factorysquad},{"patrolrepair",patrolrepair},
        {"lonetrees",lonetrees},{"slideback",slideback},{"forestedge",forestedge},{"stillturn",stillturn},{"facing",facing},{"rb02",rb02},{"awarebig",awarebig},{"awaredense",awaredense},{"crossthree",crossthree},{"crosslong",crosslong},{"mixedsettle",mixedsettle},{"mixedrandom",mixedrandom},{"funnelmouth",funnelmouth},{"funneltie",funneltie},{"entryplanes",entryplanes},
        {"improvedefer",improvedefer},{"chainresolve",chainresolve},{"trafficlane",trafficlane},{"staticindex",staticindex},{"routeindex",routeindex},
        {"fullplan",fullplan},{"demandown",demandown},{"seedcover",seedcover},{"seedrestore",seedrestore},{"cutoff",cutoff},{"splitseeds",splitseeds},{"routeclone",routeclone},{"destinations",destinations}};
    try {
        if(argc<2) {for(const auto& [name,fn]:cases)fn();}
        else {
            const auto found=cases.find(argv[1]);
            if(found==cases.end())throw std::runtime_error("unknown case");
            found->second();
        }
    } catch(const std::exception& e) {std::fprintf(stderr,"legion_world_test: %s\n",e.what());return 1;}
    std::puts("ok");
    return 0;
}
