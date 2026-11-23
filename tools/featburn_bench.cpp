// featburn_bench -- explicit timing harness for feature burning and fading
// (World::tickBurning / World::tickFeatureFades), not a CTest.
//
// A retail map's real scenery (setupMatch, two monarchs, no AI) with scripted,
// deterministic scenery events that use only the production entry points:
//   nofire   nothing burns or fades (the common case)
//   sparse   one random flamable feature ignites every 60 ticks; fires spread
//            through the map's own spreadchance/sparktime/burn lifetimes
//   fire     5% of the flamable features ignite every 300 ticks (forest fires)
//   fade     a random feature starts its 34-tick reclaim fade every 10 ticks
// --gen CELLS replaces the retail map with a generated one at the densest tree
// setting (the retail maps hold 26..1250 tracked features).
// The event stream comes from the bench's own LCG, so base and candidate see
// the same events; the world's RNG is drawn only by the burns themselves.
//
//   --mode routine  time ONLY tickBurning + tickFeatureFades per tick (the
//                   tick counter is advanced by hand, nothing else runs)
//   --mode tick     time the complete World::tick (two idle monarchs)
//
// Prints per-tick ns (mean/median/p95/p99/max), work counters (features,
// burning per tick, ignitions, fades), and the final state hash plus a
// feature-state checksum: base and candidate must print the same hash.
#include "sim/matchsetup.h"
#include "hpi/hpi.h"
#include "tnt/mapgen.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace tak::sim {
struct RetailReplayProbe {
    static void ignite(World& w,size_t i) { w.igniteFeature(w.features_.at(i)); }
    static void fade(World& w,int id) { Unit caster;w.finishReclaim(caster,id); }
    static void routine(World& w) { ++w.tickCounter_;w.tickBurning();w.tickFeatureFades(); }
    static const std::vector<FeatType>& types(const World& w) { return w.featTypes_; }
};
}

int main(int argc,char** argv) try {
    using namespace tak;
    std::string data="assets/game",mapName="Black Forest",scene="nofire",mode="routine";
    int ticks=3000,gen=0;
    for (int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if (arg=="--help") {
            std::puts("featburn_bench [--data DIR] [--map NAME] [--scene nofire|sparse|fire|fade] "
                      "[--mode routine|tick] [--ticks 3000] [--gen CELLS]\n"
                      "--gen CELLS: a generated CELLSxCELLS map (seed 1, densest trees) instead of --map");
            return 0;
        }
        if (i+1>=argc) throw std::invalid_argument("missing option value");
        const std::string value=argv[++i];
        if (arg=="--data") data=value;
        else if (arg=="--map") mapName=value;
        else if (arg=="--scene") scene=value;
        else if (arg=="--mode") mode=value;
        else if (arg=="--ticks") ticks=std::stoi(value);
        else if (arg=="--gen") gen=std::stoi(value);
        else throw std::invalid_argument("unknown option: "+arg);
    }
    if (scene!="nofire" && scene!="sparse" && scene!="fire" && scene!="fade")
        throw std::invalid_argument("invalid scene");
    if (mode!="routine" && mode!="tick") throw std::invalid_argument("invalid mode");
    if (ticks<1 || ticks>100000) throw std::invalid_argument("ticks must be 1..100000");

    hpi::Vfs vfs=hpi::mountRetailRoot(data);
    sim::TypeRegistry registry;
    sim::setupRegistry(registry,vfs,false);
    sim::World world;
    sim::MatchConfig config;
    config.vfs=&vfs;
    if (gen) {
        mapgen::Params params;
        params.widthCells=params.heightCells=uint16_t(gen);
        params.treeDensity=255;
        params.plateauDensity=params.mountainDensity=params.townDensity=0;
        config.mapPath=mapgen::encodeMapId(params);
    } else config.mapPath=hpi::findMap(vfs,mapName);
    if (config.mapPath.empty()) throw std::invalid_argument("map not found: "+mapName);
    for (int p=0;p<2;++p) config.slots.push_back({true,p,p,1.0f,false,false});
    sim::setupMatch(world,registry,config);
    using Probe=sim::RetailReplayProbe;

    const auto& features=world.features();
    const auto& types=Probe::types(world);
    std::vector<size_t> flamable;
    for (size_t i=0;i<features.size();++i) {
        const auto& f=features[i];
        if (f.alive && f.type>=0 && types[size_t(f.type)].flamable && types[size_t(f.type)].hasBurnAnim)
            flamable.push_back(i);
    }
    std::printf("map=%s scene=%s mode=%s features=%zu flamable=%zu\n",config.mapPath.c_str(),
                scene.c_str(),mode.c_str(),features.size(),flamable.size());
    if (flamable.empty() && (scene=="sparse" || scene=="fire"))
        throw std::runtime_error("map has no flamable features");

    uint32_t lcg=12345;
    auto next=[&](uint32_t n){ lcg=lcg*1664525u+1013904223u;return uint32_t((uint64_t(lcg>>8)*n)>>24); };
    auto burningNow=[&]{ size_t n=0;for (const auto& f:features) n+=f.alive && f.burn;return n; };
    std::vector<double> ns;ns.reserve(size_t(ticks));
    uint64_t ignitionsAsked=0,fadesAsked=0,burningTicks=0,burningSum=0;
    size_t burningPeak=0;
    const int warm=std::min(300,ticks/10);
    for (int t=0;t<warm+ticks;++t) {
        // Scripted events (untimed), identical for every build.
        if (scene=="sparse" && t%60==0) { Probe::ignite(world,flamable[next(uint32_t(flamable.size()))]);++ignitionsAsked; }
        if (scene=="fire" && t%300==0)
            for (size_t k=0;k<flamable.size()/20;++k) { Probe::ignite(world,flamable[next(uint32_t(flamable.size()))]);++ignitionsAsked; }
        if (scene=="fade" && t%10==0 && !features.empty()) {
            const auto& f=features[next(uint32_t(features.size()))];
            if (f.alive && !f.fadeLeft) { Probe::fade(world,f.id);++fadesAsked; }
        }
        const auto t0=std::chrono::steady_clock::now();
        if (mode=="routine") Probe::routine(world); else world.tick(1.f/30);
        const double dt=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-t0).count();
        if (t<warm) continue;
        ns.push_back(dt);
        const size_t b=burningNow();
        burningSum+=b;burningTicks+=b>0;burningPeak=std::max(burningPeak,b);
    }
    uint64_t check=1469598103934665603ull;
    for (const auto& f:features) {
        for (const uint64_t v : {uint64_t(f.alive),uint64_t(f.burn),uint64_t(f.burnLeft),uint64_t(uint32_t(f.spreadIn)),
                                 uint64_t(uint32_t(f.type)),uint64_t(f.fadeLeft),uint64_t(f.burnSequence)})
            check=(check^v)*1099511628211ull;
    }
    std::vector<double> sorted=ns;std::sort(sorted.begin(),sorted.end());
    double sum=0;for (double v:ns) sum+=v;
    auto pct=[&](double p){ return sorted[std::min(sorted.size()-1,size_t(p*double(sorted.size())))]; };
    std::printf("ticks=%zu mean_ns=%.1f median_ns=%.1f p95_ns=%.1f p99_ns=%.1f max_ns=%.1f\n",ns.size(),
                sum/double(ns.size()),pct(0.5),pct(0.95),pct(0.99),sorted.back());
    std::printf("ignitions_asked=%llu fades_asked=%llu ticks_with_fire=%llu mean_burning=%.2f peak_burning=%zu\n",
                static_cast<unsigned long long>(ignitionsAsked),static_cast<unsigned long long>(fadesAsked),
                static_cast<unsigned long long>(burningTicks),double(burningSum)/double(ns.size()),burningPeak);
    std::printf("hash=%016llx features_check=%016llx\n",static_cast<unsigned long long>(world.stateHash()),
                static_cast<unsigned long long>(check));
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr,"featburn_bench: %s\n",e.what());return 1;
}
