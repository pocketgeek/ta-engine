// Legion static-sync bench (perf follow-up H): a base-heavy world, several movement planes,
// a few small localized static changes per step. Times each step's static sync on its own
// (LegionNavigator::staticLegal runs syncStatic first) and the world tick after it, and
// prints the final state hash and plane counters so two builds can be compared exactly.
//
//   legion_staticsync_bench [--size N] [--structures N] [--bases N] [--planes N] [--steps N]
//                           [--changes N] [--churn N] [--workers] [--check]
//
// --changes: blocking 1x1/2x2 features placed per step inside a random base; --churn: every
// that many steps one structure is destroyed and another built (0 off); --check compares
// every plane with a whole rebuild at the end. Builds the same with or without perf H, so
// one source times both trees.
// Prints "BENCH key=value ..." lines; wall times in ms. Asset-free.
#include "sim/sim.h"
#include "sim/footprint.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace tak::sim;
namespace {
struct Dist {
    std::vector<double> v;
    void print(const char* name) {
        if(v.empty())return;
        std::vector<double> s=v;std::sort(s.begin(),s.end());
        double sum=0;for(double x:s)sum+=x;
        auto q=[&](double f) {return s[std::min(s.size()-1,size_t(f*double(s.size()-1)+0.5))];};
        std::printf("BENCH %s n=%zu mean=%.4f median=%.4f p95=%.4f p99=%.4f max=%.4f total=%.2f\n",name,s.size(),
            sum/double(s.size()),q(0.5),q(0.95),q(0.99),s.back(),sum);
    }
};
}

int main(int argc,char** argv) {
    int size=512,structures=3000,bases=8,planes=6,steps=400,changes=4,churn=10;
    bool workers=false,checkPlanes=false;
    for(int i=1;i<argc;++i) {
        auto num=[&](int& v) {if(i+1<argc)v=std::atoi(argv[++i]);};
        if(!std::strcmp(argv[i],"--size"))num(size);
        else if(!std::strcmp(argv[i],"--structures"))num(structures);
        else if(!std::strcmp(argv[i],"--bases"))num(bases);
        else if(!std::strcmp(argv[i],"--planes"))num(planes);
        else if(!std::strcmp(argv[i],"--steps"))num(steps);
        else if(!std::strcmp(argv[i],"--changes"))num(changes);
        else if(!std::strcmp(argv[i],"--churn"))num(churn);
        else if(!std::strcmp(argv[i],"--workers"))workers=true;
        else if(!std::strcmp(argv[i],"--check"))checkPlanes=true;
        else {std::fprintf(stderr,"unknown option %s\n",argv[i]);return 2;}
    }
    const int W=size,H=size;
    std::vector<uint8_t> heights(size_t(W)*H);
    for(int z=0;z<H;++z)for(int x=0;x<W;++x)heights[size_t(z)*W+x]=uint8_t(100+(x*7+z*3)%24);
    World world;
    world.setGameSeed(7);world.setSerialThreads(!workers);world.setPathService(true);
    world.setPathfindingMode(PathfindingMode::Legion);
    world.setPlayerCount(2);world.setTeam(0,0);world.setTeam(1,1);
    world.setTerrain(heights,W,H,64);
    world.setMapPlacementFeatures(std::vector<uint16_t>(size_t(W)*H,0xffff),{{"bench-wall",1,1,true,true,false,0}});

    // Movement classes: one plane each (footprint and slope limit differ).
    std::vector<UnitType> movers(size_t(std::max(1,planes)));
    for(size_t k=0;k<movers.size();++k) {
        UnitType& t=movers[k];
        t.id=t.name="bench-mover-"+std::to_string(k);
        t.canMove=true;t.maxHp=100;t.footX=1+int(k%4);t.footZ=t.footX+(k>=4?1:0);t.sight=256;
        t.maxSlope=10+int(k)*3;t.maxVel=Fixed::raw(117964);t.accel=t.brake=Fixed::fromInt(10);
        t.turnRate=t.turnInPlaceRate=2500;t.halfCellTicks=3;t.buildTime=1;
    }
    // Structure types: 2x2..4x4, some with a yard map.
    std::vector<UnitType> kinds(4);
    for(size_t k=0;k<kinds.size();++k) {
        UnitType& t=kinds[k];
        t.id=t.name="bench-structure-"+std::to_string(k);
        t.maxHp=1000;t.footX=2+int(k%3);t.footZ=2+int((k+1)%3);t.buildTime=1;t.sight=64;t.maxVel=Fixed();
        if(k==3)t.yardMap=std::string(size_t(t.footX*t.footZ),'o'),t.yardMap[0]='.';
    }
    // Bases: square blocks of structures on a stride-5 lattice (lanes between them).
    const int per=(structures+std::max(1,bases)-1)/std::max(1,bases);
    const int side=std::max(1,int(std::ceil(std::sqrt(double(per)))));
    const int span=side*5;
    std::vector<std::array<int,2>> origin;
    const int cols=std::max(1,int(std::ceil(std::sqrt(double(std::max(1,bases))))));
    for(int b=0;b<bases;++b) {
        const int gx=b%cols,gz=b/cols;
        origin.push_back({8+gx*(W-16)/cols,8+gz*(H-16)/cols});
    }
    std::vector<int> built;
    uint32_t r=12345u;
    auto rnd=[&](int n) {r=r*1103515245u+12345u;return int((r>>8)%uint32_t(std::max(1,n)));};
    for(int i=0;i<structures;++i) {
        const auto& o=origin[size_t(i%std::max(1,bases))];
        const int slot=i/std::max(1,bases);
        const int cx=o[0]+(slot%side)*5,cz=o[1]+(slot/side)*5;
        if(cx+5>=W||cz+5>=H)continue;
        const UnitType& t=kinds[size_t(i)%kinds.size()];
        const int id=world.spawn(&t,float(cx*16+t.footX*8),float(cz*16+t.footZ*8),std::nullopt,i%2);
        if(id>0)built.push_back(id);
    }
    std::vector<int> walkers;
    for(size_t k=0;k<movers.size();++k)
        walkers.push_back(world.spawn(&movers[k],float((4+int(k)*6)*16+8),float((H-6)*16+8),std::nullopt,0));
    world.tick(1.f/30);
    auto* legion=world.legionNavigator();
    if(!legion) {std::fprintf(stderr,"no Legion navigator\n");return 1;}
    for(int id:walkers)if(const Unit* u=world.unit(id))legion->staticLegal(*u,2,2);   // every plane built now
    std::printf("BENCH setup size=%d structures=%zu bases=%d side=%d planes=%zu workers=%d\n",W,built.size(),
        bases,side,movers.size(),workers?1:0);

    Dist sync,tick;
    using Clock=std::chrono::steady_clock;
    auto ms=[](Clock::time_point a,Clock::time_point b) {return std::chrono::duration<double,std::milli>(b-a).count();};
    int featureId=0;
    for(int step=0;step<steps;++step) {
        for(int c=0;c<changes;++c) {
            const auto& o=origin[size_t(rnd(int(origin.size())))];
            const int fx=1+rnd(2),fz=1+rnd(2),x=std::min(W-4,o[0]+rnd(span)),z=std::min(H-4,o[1]+rnd(span));
            world.addFeature(1000000+featureId++,float(x*16+fx*8),float(z*16+fz*8),0,fx,fz,true,-1,true);
        }
        if(churn>0&&step%churn==churn-1&&!built.empty()) {
            const size_t i=size_t(rnd(int(built.size())));
            const Unit* gone=world.unit(built[i]);
            const float gx=gone?gone->x.toFloat():0,gz=gone?gone->z.toFloat():0;
            const UnitType* t=gone?gone->type:nullptr;
            world.destroy(built[i]);built.erase(built.begin()+long(i));
            if(t) {const int id=world.spawn(t,gx,gz,std::nullopt,1);if(id>0)built.push_back(id);}
        }
        const Unit* probe=world.unit(walkers[0]);
        const auto t0=Clock::now();
        if(probe)legion->staticLegal(*probe,2,2);
        const auto t1=Clock::now();
        world.tick(1.f/30);
        const auto t2=Clock::now();
        sync.v.push_back(ms(t0,t1));tick.v.push_back(ms(t1,t2));
    }
    sync.print("sync_ms");tick.print("tick_ms");
    const auto s=world.legionStats();
    std::printf("BENCH result hash=%016llx plane_builds=%llu plane_refreshes=%llu plane_relabels=%llu features=%zu\n",
        (unsigned long long)world.stateHash(),(unsigned long long)s.planeBuilds,(unsigned long long)s.planeRefreshes,
        (unsigned long long)s.planeRelabels,world.features().size());
    if(checkPlanes) {
        int bad=0;
        for(int id:walkers)if(const Unit* u=world.unit(id))bad+=!legion->planeMatchesRebuild(*u);
        std::printf("BENCH check planes_differing=%d\n",bad);
        if(bad)return 1;
    }
    return 0;
}
