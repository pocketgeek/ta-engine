// Explicit microbenchmark, not a CTest: Controller::reviveIdle (our levels'
// idle casters raising bodies within 56 px) on a synthetic world. The world is
// never changed, so every call does the same work and gives the same commands;
// the printed stream hash must match between builds.
//
// ai_revive_bench [--units 4000] [--casters 64] [--bodies 0] [--layout uniform|near]
//                 [--map 12288] [--calls 2000] [--warm 200] [--seed 1]
//   --units    live non-caster units (players 0 and 1, spread over the map)
//   --casters  idle player-0 priests (canresurrect)
//   --bodies   settled raisable bodies; uniform = anywhere on the map, near =
//              within 64 px of a random caster (most casters find one)
//   --map      the square map's side in px
#include "ai/ai.h"
#include "net/lockstep.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace tak::ai {
// Controller's existing test friend (ai_behavior_test defines its own; this is
// a separate program).
struct PlacementProbe {
    static void revive(const Controller& c,const sim::World& w,std::vector<int>& assigned,const CommandSink& sink) {
        c.reviveIdle(w,assigned,sink);
    }
};
}

int main(int argc,char** argv) try {
    using namespace tak;
    int units=4000,casters=64,bodies=0,mapPx=12288,calls=2000,warm=200;
    uint32_t seed=1;
    std::string layout="uniform";
    for (int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if (i+1>=argc) throw std::invalid_argument("missing value for "+arg);
        const std::string v=argv[++i];
        if (arg=="--units") units=std::stoi(v);
        else if (arg=="--casters") casters=std::stoi(v);
        else if (arg=="--bodies") bodies=std::stoi(v);
        else if (arg=="--layout") layout=v;
        else if (arg=="--map") mapPx=std::stoi(v);
        else if (arg=="--calls") calls=std::stoi(v);
        else if (arg=="--warm") warm=std::stoi(v);
        else if (arg=="--seed") seed=uint32_t(std::stoul(v));
        else throw std::invalid_argument("unknown option "+arg);
    }
    if (layout!="uniform" && layout!="near") throw std::invalid_argument("layout must be uniform or near");
    if (casters<1 && layout=="near") throw std::invalid_argument("near needs casters");
    auto next=[&](uint32_t n) {seed=seed*1103515245u+12345u;return ((seed>>8)&0xffffff)%n;};

    sim::UnitType soldier;soldier.id="soldier";soldier.maxHp=100;soldier.maxVel=sim::Fixed::fromInt(2);
    soldier.canMove=true;soldier.footX=soldier.footZ=2;soldier.turnRate=soldier.turnInPlaceRate=1000;
    soldier.defaultMove=1;soldier.buildTime=1;
    auto priest=soldier;priest.id="priest";priest.canResurrect=true;priest.workerTime=10;
    sim::FeatType body;body.name="soldier_dead";body.resurrectable=true;body.decomposeTicks=100000;

    sim::World w;
    const int cells=mapPx/16;
    w.setTerrain(std::vector<uint8_t>(size_t(cells)*size_t(cells),100),cells,cells,20);
    w.setFeatureTypes({body});w.mapCorpse(&soldier,0);
    auto anywhere=[&]{return 64+float(next(uint32_t(mapPx-128)));};
    std::vector<std::pair<float,float>> casterAt;
    // Interleave the kinds the way a game's unit records are: casters, bodies
    // and the live army spawned in mixed order.
    const int total=units+casters+bodies;
    int placedU=0,placedC=0;
    for (int i=0;i<total;++i) {
        const int pick=int(next(uint32_t(total-i)));
        if (pick<casters-placedC) {
            const float x=anywhere(),z=anywhere();
            w.spawn(&priest,x,z,0,0);casterAt.emplace_back(x,z);++placedC;
        } else if (pick<casters-placedC+units-placedU) {
            w.spawn(&soldier,anywhere(),anywhere(),0,int(next(2)));++placedU;
        } else {
            float x=anywhere(),z=anywhere();
            if (layout=="near" && !casterAt.empty()) {
                const auto [cx,cz]=casterAt[next(uint32_t(casterAt.size()))];
                x=cx-64+float(next(128));z=cz-64+float(next(128));
            }
            const int id=w.spawn(&soldier,x,z,0,int(next(2)));
            auto& d=*w.unit(id);d.hp={};d.deadFor=200;d.corpseUntil=100000;
        }
    }
    w.resumeClocks(1,w.gameRngState());
    sim::TypeRegistry registry;ai::Profile profile;
    ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{float(mapPx)-200,200}});
    size_t emitted=0;uint64_t streamHash=1469598103934665603ull;
    std::vector<double> us;us.reserve(size_t(calls));
    std::vector<int> assigned;
    for (int i=0;i<warm+calls;++i) {
        assigned.clear();
        size_t n=0;uint64_t h=1469598103934665603ull;
        const auto t0=std::chrono::steady_clock::now();
        ai::PlacementProbe::revive(controller,w,assigned,[&](const net::Command& c) {
            ++n;h=(h^uint64_t(uint32_t(c.unitId)))*1099511628211ull;h=(h^uint64_t(uint32_t(c.targetId)))*1099511628211ull;
        });
        const auto t1=std::chrono::steady_clock::now();
        if (i==0) {emitted=n;streamHash=h;}
        else if (n!=emitted || h!=streamHash) throw std::runtime_error("calls disagree");
        if (i>=warm) us.push_back(std::chrono::duration<double,std::micro>(t1-t0).count());
    }
    std::vector<double> s=us;std::sort(s.begin(),s.end());
    double mean=0;for (double v:us) mean+=v;mean/=double(us.size());
    auto q=[&](double f){return s[std::min(s.size()-1,size_t(f*double(s.size())))];};
    std::printf("ai_revive_bench units=%d casters=%d bodies=%d layout=%s map=%d records=%zu calls=%d "
                "commands=%zu stream=%016llx mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f\n",
                units,casters,bodies,layout.c_str(),mapPx,w.units().size(),calls,emitted,
                static_cast<unsigned long long>(streamHash),mean,q(0.5),q(0.95),q(0.99),s.back());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr,"ai_revive_bench: %s\n",e.what());return 1;
}
