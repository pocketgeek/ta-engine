// Explicit performance harness, not a CTest: the full workloads take minutes.
#include "sim/matchsetup.h"
#include "ai/ai.h"
#include "ai/builtinprofile.h"
#include "hpi/hpi.h"
#include "tnt/mapgen.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

int main(int argc,char** argv) try {
    using namespace tak;
    std::string mode="movement",data="assets/game",mapName;
    int count=16000,ticks=3600,unitCap=0;
    bool crusades=false,serial=false;
    for (int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if (arg=="--serial") {serial=true;continue;}
        if (arg=="--crusades") {crusades=true;continue;}
        if (arg=="--help") {
            std::puts("simperf [--mode movement|match|patrol|crowd] [--units 16000] [--unit-cap 2000] [--ticks 3600] [--data assets/game] [--map NAME] [--crusades] [--serial]\n"
                      "movement: synthetic flat terrain, unarmed moving units; excludes scripts/AI/rendering.\n"
                      "match: generated flat map, mixed retail armies and eight Absurd AIs; includes combat/deaths.\n"
                      "patrol: same mixed retail armies, allied and patrolling; includes scripts, excludes AI/combat/rendering.\n"
                      "crowd: allied mixed armies converging near their starting positions; includes scripts and destination congestion.\n"
                      "--map selects a retail map for match/patrol/crowd; the default remains the generated flat map.\n"
                      "--unit-cap sets each player's cap independently of the initial army size (8 initial units allow natural AI growth).\n"
                      "Times exclude setup and do not measure rendering. ms/median_ms/p95_ms/p99_ms/max_ms are core ticks\n"
                      "(AI + world tick); complete_* ticks add the lockstep stateHash due every 30th tick; wall_seconds\n"
                      "also counts the once-per-second diagnostic scan and output, service_seconds does not.");
            return 0;
        }
        if (i+1>=argc) throw std::invalid_argument("missing option value");
        const std::string value=argv[++i];
        if (arg=="--mode") mode=value;
        else if (arg=="--data") data=value;
        else if (arg=="--map") mapName=value;
        else if (arg=="--units") count=std::stoi(value);
        else if (arg=="--unit-cap") unitCap=std::stoi(value);
        else if (arg=="--ticks") ticks=std::stoi(value);
        else throw std::invalid_argument("unknown option: "+arg);
    }
    if (count<1 || count>16000 || ticks<1 || ticks>18000)
        throw std::invalid_argument("units must be 1..16000 and ticks 1..18000");
    if(unitCap<0 || unitCap>2000)throw std::invalid_argument("unit cap must be 0..2000 (0 uses the initial army size)");
    if (mode!="movement" && mode!="match" && mode!="patrol" && mode!="crowd") throw std::invalid_argument("invalid mode");
    if (mode!="movement" && count%8) throw std::invalid_argument("army unit count must be divisible by 8");

    if (mode=="movement" && !mapName.empty()) throw std::invalid_argument("--map requires match, patrol or crowd mode");

    // Lifetimes exceed World and its AI controllers: unit types and profiles are referenced.
    hpi::Vfs vfs;
    sim::TypeRegistry registry;
    sim::UnitType mover{};
    ai::Profile profile;
    sim::World world;
    world.setSerialThreads(serial);
    std::vector<std::unique_ptr<ai::Controller>> controllers;
    std::vector<std::pair<float,float>> crowdGoals;
    const auto setupStart=std::chrono::steady_clock::now();
    if (mode=="movement") {
        world.setTerrain(std::vector<uint8_t>(1280*1024,100),1280,1024,20);
        world.setMapPlacementFeatures(std::vector<uint16_t>(1280*1024,0xffff),{});
        world.setPathService(true);
        world.setPlayerCount(8);world.setUnitCap(2000);
        mover.name=mover.id="probe";
        mover.maxVel=sim::Fixed::fromFloat(70.f/30);mover.turnRate=10000;
        mover.maxHp=100;mover.canMove=true;mover.footX=mover.footZ=2;
        for (int i=0;i<count;++i) {
            const int x=256+(i%200)*40,z=256+(i/200)*40;
            const int id=world.spawn(&mover,float(x),float(z),0,i/2000);
            world.order(id,float(x+11000),float(z),false);
        }
    } else {
        vfs=hpi::mountRetailRoot(data);
        sim::setupRegistry(registry,vfs,crusades);
        mapgen::Params map;
        // Flat and empty: no water, trees, rocks, relief, kits or neutral mana.
        map.widthCells=map.heightCells=768;map.players=8;
        map.waterDensity=map.treeDensity=map.rockDensity=map.reliefDensity=map.manaDensity=0;
        map.plateauDensity=map.mountainDensity=map.townDensity=0;
        sim::MatchConfig config;
        config.vfs=&vfs;
        config.mapPath=mapName.empty() ? mapgen::encodeMapId(map) : hpi::findMap(vfs,mapName);
        if (config.mapPath.empty()) throw std::invalid_argument("map not found: "+mapName);
        config.stressTest=count>8;config.stressImmediate=true;config.unitCap=(((count/8)-1)*100+94)/95;
        for (int p=0;p<8;++p) config.slots.push_back({true,p%5,mode=="patrol" || mode=="crowd" ? 0 : p,2,true,false});
        const auto starts=sim::setupMatch(world,registry,config);
        if (world.units().size()!=size_t(count))
            throw std::runtime_error("map placed only "+std::to_string(world.units().size())+
                " units; expected "+std::to_string(count)+" including monarchs; reduce --units or choose another map");
        world.setUnitCap(unitCap ? unitCap : count/8);
        std::printf("map=%s terrain_cells=%dx%d\n",config.mapPath.c_str(),
                    world.nav().width(),world.nav().height());
        if (mode=="patrol") {
            for (const auto& u:world.units())
                if (u.type && !u.type->isStructure())
                    world.patrol(u.id,float(world.nav().width()*16)-u.x.toFloat(),u.z.toFloat());
        }
        if (mode=="crowd") {
            crowdGoals=starts;
            for (auto& goal:crowdGoals) goal.second+=300;
            for (const auto& u:world.units())
                if (u.type && !u.type->isStructure())
                    world.order(u.id,crowdGoals[size_t(u.player)].first,crowdGoals[size_t(u.player)].second,false);
        }
        profile=ai::builtinProfile(ai::Difficulty::Absurd);
        for (int p=0;p<8 && mode=="match";++p) {
            std::vector<std::pair<float,float>> enemies;
            for (const auto& u:world.units())
                if (u.player!=p && u.type && u.type->commander)
                    enemies.emplace_back(u.x.toFloat(),u.z.toFloat());
            controllers.push_back(std::make_unique<ai::Controller>(p,registry,profile,2002+p,
                ai::Difficulty::Absurd,enemies));
        }
    }
    std::printf("mode=%s requested=%d initial=%zu setup_seconds=%.3f\n",mode.c_str(),count,
        world.units().size(),std::chrono::duration<double>(std::chrono::steady_clock::now()-setupStart).count());
    // durations: core tick (AI + world tick). complete: core plus the stateHash due on that tick.
    std::vector<double> durations,complete;
    durations.reserve(size_t(ticks));complete.reserve(size_t(ticks));
    int minAlive=int(world.units().size());
    double hashMs=0,aiMs=0,worldMs=0,aiTotalMs=0,worldTotalMs=0;uint64_t firedTicks=0,hitEvents=0;int hashTicks=0;
    std::vector<std::pair<int32_t,int32_t>> previousPositions;
    std::vector<int> previousIds;
    for (const auto& u:world.units()) {previousPositions.emplace_back(u.x.v,u.z.v);previousIds.push_back(u.id);}
    const auto start=std::chrono::steady_clock::now();
    for (int tick=0;tick<ticks;++tick) {
        const auto tickStart=std::chrono::steady_clock::now();
        for (auto& controller:controllers)
            controller->tick(world,uint32_t(tick),[&](const net::Command& c){sim::applyCommand(world,registry,c);});
        const auto worldStart=std::chrono::steady_clock::now();
        const double tickAiMs=std::chrono::duration<double,std::milli>(worldStart-tickStart).count();
        world.tick(1.f/30);
        const double tickWorldMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-worldStart).count();
        aiMs+=tickAiMs;worldMs+=tickWorldMs;aiTotalMs+=tickAiMs;worldTotalMs+=tickWorldMs;
        hitEvents+=world.hits().size();
        durations.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-tickStart).count());
        // The due lockstep hash is service work: time it before the diagnostic scan and output below,
        // which stay outside every tick sample (stateHash is const, so the order changes no result).
        const bool hashDue=tick%30==29;
        uint64_t hash=0;double tickHashMs=0;
        if (hashDue) {
            const auto hashStart=std::chrono::steady_clock::now();
            hash=world.stateHash();
            tickHashMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-hashStart).count();
            hashMs+=tickHashMs;++hashTicks;
        }
        complete.push_back(durations.back()+tickHashMs);
        if (hashDue) {
            int living=0,moving=0,firing=0,stationary=0,nearGoal=0;
            std::vector<std::pair<int32_t,int32_t>> positions;
            std::vector<int> ids;
            positions.reserve(world.units().size());ids.reserve(world.units().size());
            size_t previous=0;
            for (size_t i=0;i<world.units().size();++i) {
                const auto& u=world.units()[i];
                living+=u.alive();moving+=u.alive() && u.speed.v>0;
                firing+=u.firedWeapons!=0;
                const std::pair<int32_t,int32_t> position{u.x.v,u.z.v};
                while(previous<previousIds.size() && previousIds[previous]<u.id)++previous;
                stationary+=u.alive() && previous<previousIds.size() && previousIds[previous]==u.id && previousPositions[previous]==position;
                positions.push_back(position);ids.push_back(u.id);
                if (u.alive() && !crowdGoals.empty()) {
                    const auto [gx,gz]=crowdGoals[size_t(u.player)];
                    const float dx=u.x.toFloat()-gx,dz=u.z.toFloat()-gz;
                    nearGoal+=dx*dx+dz*dz<=512.f*512.f;
                }
            }
            previousPositions.swap(positions);previousIds.swap(ids);
            minAlive=std::min(minAlive,living);firedTicks+=firing;
            std::printf("tick=%d hash=%016llx ms=%.3f alive=%d moving=%d firing=%d projectiles=%zu stationary_1s=%d near_goal_512=%d ai_ms=%.3f world_ms=%.3f"
                        " hash_tick_ms=%.3f complete_ms=%.3f\n",tick+1,
                static_cast<unsigned long long>(hash),durations.back(),living,moving,firing,world.projectiles().size(),stationary,nearGoal,aiMs,worldMs,
                tickHashMs,complete.back());
            aiMs=worldMs=0;
            std::fflush(stdout);
        }
    }
    const double wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    int alive=0,moving=0,displaced=0;
    for (const auto& u:world.units()) {
        alive+=u.alive();moving+=u.alive() && u.speed.v>0;
        displaced+=u.alive() && (u.x!=u.homeX || u.z!=u.homeZ);
    }
    double coreSum=0,completeSum=0;
    for (double d:durations) coreSum+=d;
    for (double d:complete) completeSum+=d;
    // Ticks over the 30 Hz budget (1000/30 ms).
    const auto overBudget=[](const std::vector<double>& v){return std::count_if(v.begin(),v.end(),[](double d){return d>1000.0/30;});};
    const auto coreOver=overBudget(durations),completeOver=overBudget(complete);
    std::sort(durations.begin(),durations.end());
    std::sort(complete.begin(),complete.end());
    std::printf("minimum_sampled_alive=%d sampled_firing=%llu hit_events=%llu hash_ms=%.3f p99_ms=%.3f\n",
        minAlive,static_cast<unsigned long long>(firedTicks),static_cast<unsigned long long>(hitEvents),hashMs,durations[durations.size()*99/100]);
    std::printf("alive=%d moving=%d displaced=%d allocated=%zu sim_seconds=%.3f wall_seconds=%.3f speed=%.3fx median_ms=%.3f p95_ms=%.3f max_ms=%.3f\n",
        alive,moving,displaced,world.units().size(),ticks/30.0,wall,(ticks/30.0)/wall,
        durations[durations.size()/2],durations[durations.size()*95/100],durations.back());
    // Same percentile indexing as the core fields above.
    std::printf("core_mean_ms=%.3f core_over_budget=%td complete_mean_ms=%.3f complete_median_ms=%.3f complete_p95_ms=%.3f complete_p99_ms=%.3f"
                " complete_max_ms=%.3f complete_over_budget=%td hash_ticks=%d service_seconds=%.3f ai_total_ms=%.3f world_total_ms=%.3f hash_total_ms=%.3f\n",
        coreSum/double(durations.size()),coreOver,completeSum/double(complete.size()),complete[complete.size()/2],
        complete[complete.size()*95/100],complete[complete.size()*99/100],complete.back(),completeOver,hashTicks,
        completeSum/1000,aiTotalMs,worldTotalMs,hashMs);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr,"simperf: %s\n",e.what());return 1;
}
