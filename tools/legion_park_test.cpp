// Legion's spread PARK (World::legionParkSpot). A factory output with no rally
// takes GetBuilt's PARK; retail steers it to the ring's mid-radius along its
// bearing from the factory (4e33b0), and every output of one door shares that
// bearing. Under Legion's exact solo goal they queued behind each other in a
// column out of the door, three of eight Swordsmen short of the ring's inner
// radius. Legion now gives each output a spot of its own over the band (or
// beyond it once the band is full), never in the door lane.
//
// Each case trains a run of outputs with no rally and checks, every tick, that
// no output which has finished its PARK stands in the door lane; at the end
// every output is parked, out of the factory's rectangle and out of the lane,
// none overlaps another, and the factory produced about as fast as with a
// rally that sends every output far away (the unobstructed rate). Layouts: open
// ground, a cliff beside the factory (an unreachable plateau), a map corner;
// outputs: Swordsmen (both balances), Catapults (4x4, also a queue of 100),
// Submersibles at a Navy Yard. It prints each output's time from completion to the end of its PARK
// (park mean/max) and to clearing its birth footprint (clear).
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace tak;
namespace {
int failures=0,cases=0;
std::string only;   // optional second argument: run the cases whose label contains it
void check(bool ok,const std::string& label) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",label.c_str());++cases;if(!ok)++failures;
}
struct Rect {int x,z,w,h;};
Rect footprint(const sim::Unit& u) {
    return {sim::footprintOrigin(u.x,u.type->footX),sim::footprintOrigin(u.z,u.type->footZ),u.type->footX,u.type->footZ};
}
bool intersects(const Rect& a,const Rect& b) {return a.x<b.x+b.w && b.x<a.x+a.w && a.z<b.z+b.h && b.z<a.z+a.h;}

enum class Layout {Open,Cliff,Corner,Water};
const char* layoutName(Layout l) {
    return l==Layout::Open?"open":l==Layout::Cliff?"cliff":l==Layout::Corner?"corner":"water";
}
constexpr int kCells=256;
constexpr int kCliffCell=122;   // plateau from here east: 160 px east of the factory centre

struct Result {
    int produced=0,parked=0,inYard=0,inLane=0,laneTicks=0,overlaps=0,refusals=0,onPlateau=0;
    int insideInner=0,left=0,right=0,sectors=0,lastDone=-1;
    double meanPark=0,meanClear=0;int maxPark=0;
};

Result run(const sim::TypeRegistry& registry,sim::PathfindingMode mode,Layout layout,
           const char* factoryName,const char* outputName,int count,bool rally,int speed) {
    sim::World w;w.setSerialThreads(true);w.setPathfindingMode(mode);
    const bool water=layout==Layout::Water;
    std::vector<uint8_t> heights(kCells*kCells,water?0:100);
    if(layout==Layout::Cliff)
        for(int z=0;z<kCells;++z)for(int x=kCliffCell;x<kCells;++x)heights[size_t(z)*kCells+x]=255;
    w.setTerrain(heights,kCells,kCells,water?100:20);
    w.setMapPlacementFeatures(std::vector<uint16_t>(kCells*kCells,0xffff),{});
    w.buildNavClasses(registry);w.setPathService(true);
    auto factory=*registry.find(factoryName);factory.workerTime*=speed;factory.storage=1000000;
    const auto* out=registry.find(outputName);
    float fx=1800,fz=1792;
    if(layout==Layout::Corner) {fx=float(kCells*16-160+(factory.footX%2)*8);fz=float(kCells*16-240);}
    const int id=w.spawn(&factory,fx,fz,0,0);
    if(rally)w.order(id,fx,fz+1600>kCells*16-64?fz-1600:fz+1600,false);
    w.player(0).mana=1000000;w.train(id,out,count);
    const Rect yard=footprint(*w.unit(id));
    const float cx=w.unit(id)->x.toFloat(),cz=w.unit(id)->z.toFloat();
    const float laneHalf=float(std::max(48,8*std::min(factory.footX,factory.footZ)));
    float doorX=0,doorZ=1;bool doorKnown=false;
    struct Out {int id;int done=-1,parked=-1;bool park=false;Rect birth{};int clear=-1;};
    std::vector<Out> outs;
    auto inLane=[&](const sim::Unit& u) {
        const float vx=u.x.toFloat()-cx,vz=u.z.toFloat()-cz;
        const float along=vx*doorX+vz*doorZ,cross=vx*doorZ-vz*doorX;
        return along>0 && std::abs(cross)<laneHalf+8.f*float(std::max(u.type->footX,u.type->footZ));
    };
    Result r;uint32_t lastRetry=0;int tick=0;
    for(;tick<30000;++tick) {
        w.player(0).mana=1000000;w.tick(1.f/30);
        const auto* f=w.unit(id);
        if(f->productionRetryTick && f->productionRetryTick!=lastRetry)++r.refusals;
        lastRetry=f->productionRetryTick;
        for(const auto& u:w.units()) {
            if(!u.alive() || u.type!=out || u.underConstruction)continue;
            auto at=std::find_if(outs.begin(),outs.end(),[&](const Out& o){return o.id==u.id;});
            if(at==outs.end()) {
                outs.push_back({u.id,tick,-1,std::any_of(u.orders.begin(),u.orders.end(),[](const auto& o){return bool(o.park);}),footprint(u)});
                at=outs.end()-1;r.lastDone=tick;
                if(!doorKnown) {
                    const float vx=u.x.toFloat()-cx,vz=u.z.toFloat()-cz,len=std::sqrt(vx*vx+vz*vz);
                    if(len>1) {doorX=vx/len;doorZ=vz/len;doorKnown=true;}
                }
            }
            if(at->parked<0 && u.orders.empty())at->parked=tick;
            if(at->clear<0 && !intersects(footprint(u),at->birth))at->clear=tick;
        }
        // No output that has finished its PARK stands in the door lane.
        int lane=0;
        for(const auto& o:outs)if(const auto* u=w.unit(o.id);u && u->alive() && o.park && o.parked>=0 && u->orders.empty())
            lane+=inLane(*u);
        if(lane) {++r.laneTicks;r.inLane=std::max(r.inLane,lane);}
        if(int(outs.size())==count && std::all_of(outs.begin(),outs.end(),[](const Out& o){return o.parked>=0;}))break;
    }
    for(int settle=0;settle<300;++settle)w.tick(1.f/30);
    r.produced=int(outs.size());
    const int half=int(std::sqrt(double(factory.footX*factory.footX+factory.footZ*factory.footZ))*8);
    const int innerMin=out->footX*16+48+half;
    unsigned sectorMask=0;int64_t sum=0;
    for(size_t i=0;i<outs.size();++i) {
        const auto* u=w.unit(outs[i].id);
        if(!u || !u->alive())continue;
        if(outs[i].parked>=0 && u->orders.empty()) {
            ++r.parked;const int t=outs[i].parked-outs[i].done;sum+=t;r.maxPark=std::max(r.maxPark,t);
        }
        r.inYard+=intersects(footprint(*u),yard);
        r.onPlateau+=layout==Layout::Cliff && sim::footprintOrigin(u->x,u->type->footX)+u->type->footX>kCliffCell;
        for(size_t j=i+1;j<outs.size();++j)if(const auto* v=w.unit(outs[j].id);v && v->alive())
            r.overlaps+=intersects(footprint(*u),footprint(*v));
        const float vx=u->x.toFloat()-cx,vz=u->z.toFloat()-cz;
        const float dist=std::sqrt(vx*vx+vz*vz);
        r.insideInner+=dist<float(innerMin)-16;
        const float cross=vx*doorZ-vz*doorX;
        (cross<0?r.left:r.right)++;
        const float angle=std::atan2(cross,vx*doorX+vz*doorZ);
        sectorMask|=1u<<(int(std::floor((angle+3.14159265f)/(2*3.14159265f)*16))&15);
    }
    r.sectors=__builtin_popcount(sectorMask);
    r.meanPark=r.parked?double(sum)/r.parked:0;
    {int64_t c=0;int n=0;for(const auto& o:outs)if(o.clear>=0){c+=o.clear-o.done;++n;}r.meanClear=n?double(c)/n:0;}
    for(const auto& o:outs)if(const auto* u=w.unit(o.id);u && u->alive() && u->orders.empty() && inLane(*u))++r.inLane;
    std::printf("%s mode=%s layout=%s factory=%s output=%s rally=%d speed=%d produced=%d parked=%d lastDone=%d "
                "park mean=%.1f max=%d clear=%.1f inYard=%d inLane=%d laneTicks=%d overlaps=%d refusals=%d insideInner=%d "
                "left=%d right=%d sectors=%d plateau=%d ticks=%d hash=%016llx\n",
        "park",mode==sim::PathfindingMode::Legion?"Legion":"Retail",layoutName(layout),factoryName,outputName,rally,speed,
        r.produced,r.parked,r.lastDone,r.meanPark,r.maxPark,r.meanClear,r.inYard,r.inLane,r.laneTicks,r.overlaps,r.refusals,
        r.insideInner,r.left,r.right,r.sectors,r.onPlateau,tick,(unsigned long long)w.stateHash());
    return r;
}

void parkCase(const sim::TypeRegistry& registry,const std::string& balance,Layout layout,
              const char* factory,const char* output,int count) {
    const std::string label="Legion "+balance+" "+layoutName(layout)+" "+factory+" x"+std::to_string(count)+" "+output+": ";
    if(!only.empty() && label.find(only)==std::string::npos)return;
    // A stress run (twenty times the worker time: an output every second or
    // two, faster than any of them can walk off the pad) for the door lane,
    // and a run at four times for the rate, where nothing need wait.
    const Result p=run(registry,sim::PathfindingMode::Legion,layout,factory,output,count,false,20);
    check(p.produced==count && p.parked==count,label+"every output is built and finishes its PARK");
    check(p.laneTicks==0 && p.inLane==0,label+"no parked output ever stands in the door lane");
    check(p.inYard==0 && p.overlaps==0,label+"every output leaves the factory, none overlapping another");
    check(p.insideInner==0,label+"none parks short of the ring's inner radius");
    check(p.left>0 && p.right>0 && p.sectors>=std::min(count,4),label+"the outputs spread round the factory, not in a column");
    // The factory does not wait on its door: about as fast as with a rally
    // that takes every output far away.
    const Result q=run(registry,sim::PathfindingMode::Legion,layout,factory,output,count,false,4);
    const Result ref=run(registry,sim::PathfindingMode::Legion,layout,factory,output,count,true,4);
    // Even a rally holds the next site for the ticks its output takes to walk
    // off the pad (a refused site waits 7..21 ticks); an output turning off
    // towards its spot takes a little longer to clear it than one walking
    // straight out (see `clear`), so the rate is held within 15%.
    check(q.produced==count && q.parked==count && q.laneTicks==0 && q.lastDone<=ref.lastDone+ref.lastDone*15/100,
          label+"production runs near the unobstructed rate ("+std::to_string(q.lastDone)+" vs "+std::to_string(ref.lastDone)+
          " ticks, refused sites "+std::to_string(q.refusals)+" vs "+std::to_string(ref.refusals)+")");
    // For comparison only: the Retail navigator's PARK on the same layout.
    if(layout==Layout::Open)run(registry,sim::PathfindingMode::Retail,layout,factory,output,count,false,4);
    if(layout==Layout::Cliff)check(p.onPlateau==0,label+"no output is sent onto the unreachable plateau");
}

// A long queue of big, slow outputs: 100 Catapults from a Keep. Walking round
// the Keep to a spot behind it a Catapult gains under half a body a crowd
// window, and the held-PARK release took that for a body held up: it parked
// them beside the Keep, they walled the door, and the factory jammed after 24.
void queueCase(const sim::TypeRegistry& registry) {
    const std::string label="Legion standard open arakeep x100 arapult (queue): ";
    if(!only.empty() && label.find(only)==std::string::npos)return;
    const Result q=run(registry,sim::PathfindingMode::Legion,Layout::Open,"arakeep","arapult",100,false,4);
    const Result ref=run(registry,sim::PathfindingMode::Legion,Layout::Open,"arakeep","arapult",100,true,4);
    check(q.produced==100 && q.parked==100 && q.laneTicks==0 && q.inYard==0 && q.overlaps==0,
          label+"every output is built and parks, none in the door lane");
    check(q.produced==100 && q.lastDone<=ref.lastDone+ref.lastDone*15/100,
          label+"production runs near the unobstructed rate ("+std::to_string(q.lastDone)+" vs "+std::to_string(ref.lastDone)+" ticks)");
}
}

int main(int argc,char** argv) {
    if(argc!=2 && argc!=3)return 2;
    if(argc==3)only=argv[2];
    auto vfs=hpi::mountRetailRoot(argv[1]);
    for(bool crusades:{false,true}) {
        sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,crusades);
        const std::string balance=crusades?"Crusades":"standard";
        parkCase(registry,balance,Layout::Open,"arakeep","arasword",40);
        if(crusades)continue;
        parkCase(registry,balance,Layout::Cliff,"arakeep","arasword",24);
        parkCase(registry,balance,Layout::Corner,"arakeep","arasword",24);
        parkCase(registry,balance,Layout::Open,"arakeep","arapult",8);
        parkCase(registry,balance,Layout::Water,"crenavy","cresubm",8);
        queueCase(registry);
    }
    std::printf("legion park: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
