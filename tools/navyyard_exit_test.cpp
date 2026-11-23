// How fast a finished ship gets out of a Navy Yard (crenavy), and whether idle
// ships at the door hold it in (the 0.9.2 known limit: "ships leave some Navy
// Yards slowly, and a crowd of Iron Clads can block a yard door").
//
// Each case trains six ships (Submersible 2x2, Iron Clad and Stern Wheeler
// 3x3) at a yard on open water, and at one whose door opens partly onto a
// shore (land in front of the door's east two columns), in both pathfinding
// modes. Measured per output: ticks from completion until it is clear of the
// yard's rectangle (the pad is free for the next) and until it is clear of the
// door (the yard grown by kDoorCells); at the end none may stand in the yard
// or still be on its way out. Then the same with eight idle Iron Clads
// parked a body's gap apart just off the door, and with one idle Iron Clad on
// the pad itself. Retail moves no idle body: on origin/main the crowd held the
// first 3x3 output wedged just out of the yard and the yard built two ships of
// six; the Iron Clad on the pad stopped the yard outright. Now the yard owner's
// idle ships in front of the yard move aside (World::clearYardDoor), and the
// yard builds as fast as with the door clear. Last, the eight Iron Clads guard
// the yard: they step aside too, at most once per ship, and end back on their
// guard near it.
//
// The remaining time out of a door facing a shore is retail's own: a boat
// whose forward probe meets a shore cell or a body within 176 px runs at a
// third of its speed (4dba80, retailGroundScan), so it crawls out of such a
// door. Exit times are printed per case (clear/door med and max).
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace tak;
namespace {
int failures=0,cases=0;
std::string only;bool trace=false;long traceIndex=0;int traceEvery=1;
void check(bool ok,const std::string& label) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",label.c_str());++cases;if(!ok)++failures;
}
struct Rect {int x,z,w,h;};
Rect footprint(const sim::Unit& u) {
    return {sim::footprintOrigin(u.x,u.type->footX),sim::footprintOrigin(u.z,u.type->footZ),u.type->footX,u.type->footZ};
}
bool intersects(const Rect& a,const Rect& b) {return a.x<b.x+b.w && b.x<a.x+a.w && a.z<b.z+b.h && b.z<a.z+a.h;}
constexpr int kCells=256,kDoorCells=4;

enum class Layout {Open,Shore};
const char* layoutName(Layout l) {return l==Layout::Open?"open":"shore";}

struct Result {
    int produced=0,stuck=0,lastDone=-1,refusals=0,inDoor=0,nudged=0;
    // Ward: guards back on their guard of the yard and near it at the end, the
    // farthest one's distance (px), and moves aside of one ship while the same
    // output was the first one leaving.
    int guarding=0,farthest=0,twice=0,asides=0;
    std::vector<int> clear,door;
    int median(std::vector<int> v) const {
        if(v.empty())return -1;
        std::sort(v.begin(),v.end());return v[v.size()/2];
    }
    int max(const std::vector<int>& v) const {return v.empty()?-1:*std::max_element(v.begin(),v.end());}
};

// Ward: the eight Iron Clads at the door guard the yard.
enum class Crowd {None,Door,Pad,Ward};
const char* crowdName(Crowd c) {return c==Crowd::None?"none":c==Crowd::Door?"door":c==Crowd::Pad?"pad":"ward";}
constexpr int kSimDoorCells=8;   // clearYardDoor's door area

Result run(const sim::TypeRegistry& registry,sim::PathfindingMode mode,Layout layout,const char* shipName,int count,
           Crowd crowd) {
    sim::World w;w.setSerialThreads(true);w.setPathfindingMode(mode);
    // Deep water (sea level 100) with land north of the yard; the shore layout
    // also puts land south-east of the door, so the door opens partly onto
    // the shore.
    constexpr float fx=1800,fz=1792;
    std::vector<uint8_t> heights(kCells*kCells,0);
    for(int z=0;z<kCells;++z)for(int x=0;x<kCells;++x) {
        // The yard covers cells 109..115 x 108..116; its door is row 116.
        const bool land=z<108 || (layout==Layout::Shore && x>=114 && z>=117 && z<127);
        if(land)heights[size_t(z)*kCells+x]=160;
    }
    w.setTerrain(heights,kCells,kCells,100);
    w.setMapPlacementFeatures(std::vector<uint16_t>(kCells*kCells,0xffff),{});
    w.buildNavClasses(registry);w.setPathService(true);
    auto yardType=*registry.find("crenavy");yardType.workerTime*=4;yardType.storage=1000000;
    const auto* ship=registry.find(shipName);
    const auto* iron=registry.find("creiron");
    const int id=w.spawn(&yardType,fx,fz,0,0);
    const Rect yard=footprint(*w.unit(id));
    const Rect door{yard.x-kDoorCells,yard.z-kDoorCells,yard.w+2*kDoorCells,yard.h+2*kDoorCells};
    // Idle Iron Clads parked in front of the door, a body's gap apart, or one
    // on the pad.
    const float doorZ=float((yard.z+yard.h)*16);
    std::vector<int> idle;
    if(crowd==Crowd::Door || crowd==Crowd::Ward)
        for(int i=0;i<8;++i) {
            // On the shore layout the crowd shifts west, onto the water.
            const float x=(layout==Layout::Shore?fx-208:fx-96)+float(i%4)*64;
            idle.push_back(w.spawn(iron,x,doorZ+40+float(i/4)*64,0,0));
        }
    if(crowd==Crowd::Pad)idle.push_back(w.spawn(iron,fx,fz+32,0,0));
    if(crowd==Crowd::Ward)for(int k:idle)w.guard(k,id,false);
    for(int t=0;t<(crowd==Crowd::Ward ? 300 : 30);++t)w.tick(1.f/30);
    const Rect simDoor{yard.x-kSimDoorCells,yard.z-kSimDoorCells,yard.w+2*kSimDoorCells,yard.h+2*kSimDoorCells};
    std::vector<std::optional<std::pair<sim::Fixed,sim::Fixed>>> lastAside(idle.size());
    std::vector<std::vector<int>> asideFor(idle.size());
    int finished=0;bool refusedSince=false;
    w.player(0).mana=1000000;w.train(id,ship,count);
    struct Out {int id,done,clear=-1,door=-1;Rect birth;bool rested=false;};
    std::vector<Out> outs;Result r;uint32_t lastRetry=0;int tick=0;
    for(;tick<20000;++tick) {
        w.player(0).mana=1000000;w.tick(1.f/30);
        const auto* f=w.unit(id);
        if(f->productionRetryTick && f->productionRetryTick!=lastRetry)++r.refusals;
        lastRetry=f->productionRetryTick;
        int leaving=0;
        for(const auto& u:w.units())
            if(u.alive() && u.type==ship && !u.underConstruction && u.builtBy==id && !u.orders.empty() &&
               intersects(footprint(u),simDoor) && (!leaving || u.id<leaving))leaving=u.id;
        {   // A refusal episode: the next site refused since the last output finished.
            const auto* f=w.unit(id);
            const int now=int(std::count_if(w.units().begin(),w.units().end(),
                [&](const sim::Unit& o){return o.builtBy==id && !o.underConstruction;}));
            if(now!=finished) {finished=now;refusedSince=false;}
            refusedSince|=!f->productionSiteId && !f->buildQueue.empty() && w.tickCount()<f->productionRetryTick;
        }
        for(size_t i=0;i<idle.size();++i)if(const auto* u=w.unit(idle[i]);u && u->alive()) {
            std::optional<std::pair<sim::Fixed,sim::Fixed>> aside;
            for(const auto& o:u->orders)if(o.productionExit)aside=o.productionExit;
            if(aside && aside!=lastAside[i]) {
                ++r.asides;
                // What it made way for: the factory's outputs finished so far,
                // and whether its next one's site has been refused since.
                leaving=finished*2+(refusedSince ? 1 : 0)+1;
                if(leaving && std::find(asideFor[i].begin(),asideFor[i].end(),leaving)!=asideFor[i].end())++r.twice;
                asideFor[i].push_back(leaving);
            }
            lastAside[i]=aside;
        }
        for(const auto& u:w.units()) {
            if(!u.alive() || u.type!=ship || u.underConstruction || u.id==id)continue;
            auto at=std::find_if(outs.begin(),outs.end(),[&](const Out& o){return o.id==u.id;});
            if(at==outs.end()) {
                if(std::find(idle.begin(),idle.end(),u.id)!=idle.end())continue;   // a crowd body, not an output
                outs.push_back({u.id,tick,-1,-1,footprint(u)});at=outs.end()-1;r.lastDone=tick;
            }
            if(at->clear<0 && !intersects(footprint(u),yard))at->clear=tick;
            // An output that finished its PARK and was later given an order:
            // the door clearing moved a parked ship (printed as nudged; with a
            // free door it still happens when the next output leaves past it).
            if(at->clear>=0 && u.orders.empty())at->rested=true;
            else if(at->rested && !u.orders.empty()) {++r.nudged;at->rested=false;}
            if(at->door<0 && !intersects(footprint(u),door))at->door=tick;
            if(trace && at-outs.begin()==traceIndex && at->door<0 && (tick-at->done)%traceEvery==0) {
                std::printf("  t=%d x=%.2f z=%.2f h=%d req=%d speed=%.3f stamp=%d orders=%zu",tick-at->done,
                    u.x.toFloat(),u.z.toFloat(),int(u.heading.v),int(u.turnReqBam),u.speed.toFloat(),int(u.routeStamp),u.orders.size());
                for(const auto& o:u.orders)std::printf(" [%s goal=%.0f,%.0f seg=%d %.0f,%.0f exh=%d stage=%d]",o.park?"park":"leg",
                    o.x.toFloat(),o.z.toFloat(),int(o.hasSegment),o.segmentX.toFloat(),o.segmentZ.toFloat(),int(o.navigationExhausted),
                    int(o.mission.stage));
                std::printf("\n");
            }
        }
        if(int(outs.size())==count && std::all_of(outs.begin(),outs.end(),[](const Out& o){return o.clear>=0;}))break;
    }
    for(int settle=0;settle<1500;++settle)w.tick(1.f/30);
    r.produced=int(outs.size());
    if(crowd==Crowd::Ward)for(int k:idle)if(const auto* u=w.unit(k);u && u->alive()) {
        const auto& o=u->orders;
        const int dx=u->x.floorInt()-w.unit(id)->x.floorInt(),dz=u->z.floorInt()-w.unit(id)->z.floorInt();
        const int d=int(std::sqrt(double(dx)*dx+double(dz)*dz));
        r.farthest=std::max(r.farthest,d);
        // Within the guard ring's outer edge (World::guardRing) and two bodies.
        const int outer=(std::max(yard.w,yard.h)+u->type->footX+2)*16+3*16+2*u->type->footX*16;
        const bool aside=std::any_of(o.begin(),o.end(),[](const sim::Order& q){return bool(q.productionExit);});
        const bool ok=!o.empty() && o[sim::World::currentLeg(o)].guard && o[sim::World::currentLeg(o)].targetId==id &&
            !aside && d<=outer;
        r.guarding+=ok;
        if(!ok)std::printf("  guard %d: orders=%zu guard=%d aside=%d d=%d outer=%d\n",k,o.size(),
            int(!o.empty() && o[sim::World::currentLeg(o)].guard),int(aside),d,outer);
    }
    // An output stuck: still in the yard, or still on its way out (it never
    // finished its PARK) by the end.
    for(const auto& o:outs) {
        if(o.clear>=0)r.clear.push_back(o.clear-o.done);
        if(o.door>=0)r.door.push_back(o.door-o.done);
        if(const auto* u=w.unit(o.id);u && u->alive() && (intersects(footprint(*u),yard) || !u->orders.empty())) {
            ++r.stuck;
            const auto& q=u->orders;
            std::printf("  stuck %d at %d,%d goal %d,%d speed=%.2f inYard=%d orders=%zu park=%d exit=%d streak=%d\n",o.id,
                u->x.floorInt(),u->z.floorInt(),q.empty()?0:q.back().x.floorInt(),q.empty()?0:q.back().z.floorInt(),
                u->speed.toFloat(),int(intersects(footprint(*u),yard)),q.size(),int(!q.empty() && bool(q.front().park)),
                int(!q.empty() && bool(q.back().productionExit)),int(u->bodyBlockStreak));
        }
    }
    // Every idle ship that stood in front of the door is out of the yard's way
    // by the end: clear of the yard and of the strip straight out of its door
    // (6 cells).
    const Rect strip{yard.x,yard.z,yard.w,yard.h+6};
    for(int k:idle)if(const auto* u=w.unit(k);u && u->alive())r.inDoor+=intersects(footprint(*u),strip);
    std::printf("exit mode=%s layout=%s ship=%s crowd=%s produced=%d/%d stuck=%d lastDone=%d refusals=%d inDoor=%d nudged=%d "
                "asides=%d twice=%d guarding=%d farthest=%d clear med=%d max=%d door med=%d max=%d ticks=%d hash=%016llx\n",
        mode==sim::PathfindingMode::Legion?"Legion":"Retail",layoutName(layout),shipName,crowdName(crowd),r.produced,count,r.stuck,
        r.lastDone,r.refusals,r.inDoor,r.nudged,r.asides,r.twice,r.guarding,r.farthest,r.median(r.clear),r.max(r.clear),r.median(r.door),r.max(r.door),tick,
        (unsigned long long)w.stateHash());
    std::printf("  door:");for(const auto& o:outs)std::printf(" %d",o.door<0?-1:o.door-o.done);std::printf("\n");
    return r;
}
}

int main(int argc,char** argv) {
    if(argc!=2 && argc!=3)return 2;
    if(argc==3)only=argv[2];
    trace=std::getenv("EXIT_TRACE")!=nullptr;
    if(trace)traceIndex=std::atol(std::getenv("EXIT_TRACE"));
    if(const char* e=std::getenv("EXIT_EVERY"))traceEvery=std::max(1,std::atoi(e));
    auto vfs=hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,false);
    constexpr int kShips=6;
    for(auto mode:{sim::PathfindingMode::Retail,sim::PathfindingMode::Legion})
        for(auto layout:{Layout::Open,Layout::Shore})
            for(const char* ship:{"cresubm","creiron","crester"}) {
                const std::string label=std::string(mode==sim::PathfindingMode::Legion?"Legion ":"Retail ")+
                    layoutName(layout)+" "+ship+": ";
                if(!only.empty() && label.find(only)==std::string::npos)continue;
                const Result clear=run(registry,mode,layout,ship,kShips,Crowd::None);
                check(clear.produced==kShips && int(clear.clear.size())==kShips && clear.stuck==0 &&
                      clear.median(clear.clear)<=180,
                      label+"every ship leaves the yard, median "+std::to_string(clear.median(clear.clear))+" ticks");
                const Result door=run(registry,mode,layout,ship,kShips,Crowd::Door);
                check(door.produced==kShips && int(door.clear.size())==kShips && door.stuck==0,
                      label+"eight idle Iron Clads at the door hold no ship in");
                check(door.produced==kShips && door.median(door.clear)<=180 &&
                      door.lastDone<=clear.lastDone+clear.lastDone*15/100,
                      label+"with them the yard builds near its clear-door rate ("+std::to_string(door.lastDone)+
                      " vs "+std::to_string(clear.lastDone)+" ticks)");
                check(door.inDoor==0,label+"the idle Iron Clads made way");
                const Result pad=run(registry,mode,layout,ship,kShips,Crowd::Pad);
                check(pad.produced==kShips && int(pad.clear.size())==kShips && pad.inDoor==0,
                      label+"an idle Iron Clad on the pad moves off it and the yard builds");
                // Legion moves the yard's own parked outputs again only off one held
                // on the pad: none of these cases holds one.
                if(mode==sim::PathfindingMode::Legion)
                    check(clear.nudged==0 && door.nudged==0 && pad.nudged==0,
                          label+"no parked ship is moved again ("+std::to_string(clear.nudged)+", "+
                          std::to_string(door.nudged)+", "+std::to_string(pad.nudged)+")");
                const Result ward=run(registry,mode,layout,ship,kShips,Crowd::Ward);
                // Under Legion an Iron Clad leaving a door onto a shore was held in
                // the yard by the yard's own outputs, parked where the guards made
                // them give up their PARK: they now move off a held one too.
                // Under the Retail mover one shore Submersible is still edging
                // to its spot among the guards at the end (out of the yard).
                check(ward.produced==kShips && int(ward.clear.size())==kShips &&
                      (mode!=sim::PathfindingMode::Legion || ward.stuck==0),
                      label+"eight Iron Clads guarding the yard at its door let all six out of the yard ("+
                      std::to_string(int(ward.clear.size()))+", "+std::to_string(ward.stuck)+" not settled)");
                check(ward.guarding==8,label+"each guard ends back on its guard near the yard ("+
                      std::to_string(ward.guarding)+" of 8, farthest "+std::to_string(ward.farthest)+" px)");
                check(ward.twice==0,label+"no guard moves aside twice for one output ("+std::to_string(ward.twice)+")");
            }
    std::printf("navyyard exit: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
