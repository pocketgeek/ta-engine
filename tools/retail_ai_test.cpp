#include "sim/retailai.h"
#include "sim/retailaipatrol.h"
#include "sim/retailplacement.h"
#include "sim/retailmap.h"
#include "sim/retailrng.h"
#include "sim/retailplayer.h"
#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "hpi/hpi.h"
#include "tnt/tnt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

using namespace tak::sim;

int main(int argc,char** argv) {
    if (argc==4 && std::strcmp(argv[1],"--map-features")==0) {
        auto vfs=tak::hpi::mountRetailRoot(argv[2]);
        const auto path=tak::hpi::findMap(vfs,argv[3]);
        const auto map=tak::tnt::Map::load(vfs.read(path),path);
        World world;
        world.setTerrain(map.heights,map.width,map.height,map.seaLevel,&map.features);
        registerMapFeatures(world,map,vfs);
        std::printf("%d %d\n",map.width,map.height);
        for (size_t i=0;i<world.mapPlacementCells().size();++i) {
            const auto& c=world.mapPlacementCells()[i];
            const auto name=c.feature<world.mapPlacementTypes().size()
                ? world.mapPlacementTypes()[c.feature].name : std::to_string(c.feature);
            std::printf("%s %u %u %u %u %u\n",name.c_str(),unsigned(c.backX),unsigned(c.backZ),
                        unsigned(c.height),unsigned(c.low),
                        unsigned(c.feature<world.mapPlacementTypes().size() &&
                                 world.mapPlacementTypes()[c.feature].clearable));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--map-boundary")==0) {
        int width,height,pixelHeight,sea,water;
        while (std::scanf("%d %d %d %d %d",&width,&height,&pixelHeight,&sea,&water)==5) {
            std::vector<RetailMapBoundaryCell> cells(size_t(width)*height);
            for (auto& c:cells) {
                unsigned feature,h,low;
                if (std::scanf("%u %u %u",&feature,&h,&low)!=3) return 2;
                c={uint16_t(feature),uint8_t(h),uint8_t(low)};
            }
            retailMapBoundary(width,height,pixelHeight,sea,water!=0,
                [&](int x,int z)->RetailMapBoundaryCell& {return cells.at(size_t(z)*width+x);});
            for (const auto& c:cells) std::printf("%u ",unsigned(c.feature));
            std::putchar('\n');
        }
        return 0;
    }
    if (argc==2 && (std::strcmp(argv[1],"--mobile-placement")==0 ||
                   std::strcmp(argv[1],"--mobile-placement-sparse")==0)) {
        const bool sparse=std::strcmp(argv[1],"--mobile-placement-sparse")==0;
        int x,z,fx,fz,width,height,sea,maxDepth,minDepth,slope,waterSlope;
        unsigned self,moving,count,entities;
        while (std::scanf("%d %d %d %d %d %d %d %d %d %d %d %u %u %u %u",
                &x,&z,&fx,&fz,&width,&height,&sea,&maxDepth,&minDepth,&slope,&waterSlope,
                &self,&moving,&count,&entities)==15) {
            std::vector<uint32_t> flags(count);
            for (auto& f:flags) if (std::scanf("%u",&f)!=1) return 2;
            std::vector<RetailPlacementEntity> units(entities);
            for (auto& u:units) {
                unsigned valid,live,mover,id;
                if (std::scanf("%u %u %u %u",&valid,&live,&mover,&id)!=4) return 2;
                u={valid!=0,live!=0,mover!=0,uint16_t(id)};
            }
            std::map<std::pair<int,int>,RetailPlacementCell> cells;
            unsigned cellCount=unsigned(width)*unsigned(height);
            if (sparse && std::scanf("%u",&cellCount)!=1) return 2;
            for (unsigned i=0;i<cellCount;++i) {
                int cx=int(i%unsigned(width)),cz=int(i/unsigned(width));
                if (sparse && std::scanf("%d %d",&cx,&cz)!=2) return 2;
                unsigned entity,feature,high,low,bx,bz;
                if (std::scanf("%u %u %u %u %u %u",&entity,&feature,&high,&low,&bx,&bz)!=6) return 2;
                cells[{cx,cz}]={uint16_t(entity),uint16_t(feature),uint8_t(high),uint8_t(low),uint8_t(bx),uint8_t(bz)};
            }
            const bool result=retailMobilePlacement(x,z,fx,fz,width,height,sea,maxDepth,minDepth,
                slope,waterSlope,uint16_t(self),moving!=0,count,
                [&](int cx,int cz){return cells.at({cx,cz});},
                [&](uint16_t feature){return flags.at(feature);},
                [&](uint16_t entity){return entity<units.size()?units[entity]:RetailPlacementEntity{};});
            std::printf("%u\n",unsigned(result));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--patrol-points")==0) {
        int x,z,radius,width,height;unsigned seed,rejections;
        while (std::scanf("%d %d %d %d %d %u %u",&x,&z,&radius,&width,&height,&seed,&rejections)==7) {
            unsigned queries=0,draws=0;
            auto points=retailAiPatrolPoints(x,z,radius,width,height,
                [&](int n){++draws;return retailRandom(seed,n);},
                [&](int32_t,int32_t){return queries++>=rejections;});
            std::printf("%u %u %u %u",unsigned(bool(points)),seed,draws,queries);
            if (points) for (auto [px,pz]:*points) std::printf(" %d %d",px,pz);
            std::putchar('\n');
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--factory-plan")==0) {
        float allocation,cost;unsigned queued,population,choice,seed;
        while (std::scanf("%f %u %u %u %f %u",&allocation,&queued,&population,&choice,&cost,&seed)==6) {
            unsigned selections=0,output=0;std::vector<int> calls;
            retailAiPlanFactory(allocation,queued!=0,uint16_t(population),[&]{++selections;return uint16_t(choice);},
                [&](uint16_t){return cost;},[&](int n){calls.push_back(n);return retailRandom(seed,n);},
                [&](uint16_t id){output=id;});
            std::printf("%u %u %u %zu",output,selections,seed,calls.size());
            for (int n:calls) std::printf(" %d",n);
            std::putchar('\n');
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--build-weight")==0) {
        int limited,desired,count,preference,priority;
        while (std::scanf("%d %d %d %d %d",&limited,&desired,&count,&preference,&priority)==5)
            std::printf("%d\n",retailAiBuildWeight(limited!=0,desired,int16_t(count),uint8_t(preference),int8_t(priority)));
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--build-choice")==0) {
        unsigned count,special,seed;
        while (std::scanf("%u %u %u",&count,&special,&seed)==3) {
            std::vector<RetailAiBuildChoice> choices(count);
            for (auto& choice:choices) {
                unsigned id;int kind,faction;
                if (std::scanf("%u %d %d %d",&id,&choice.weight,&kind,&faction)!=4) return 2;
                choice.type=uint16_t(id);choice.special=kind!=0;choice.sameFaction=faction!=0;
            }
            std::vector<int> calls;
            const auto selected=retailAiChooseBuild(choices,special!=0,[&](int n){
                calls.push_back(n);return retailRandom(seed,n);
            });
            std::printf("%u %u %zu",unsigned(selected),seed,calls.size());
            for (int n:calls) std::printf(" %d",n);
            std::putchar('\n');
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--base-radius")==0) {
        int area;unsigned category;
        while (std::scanf("%d %u",&area,&category)==2)
            std::printf("%d\n",retailAiBaseRadius(area,category));
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--category")==0) {
        RetailAiUnitClass unit;
        unsigned category,missionFlags;
        int constructor,mover,mission,capacity;
        while (std::scanf("%u %u %u %d %d %d %u %d %u",&unit.flags,&unit.typeFlags,
                   &unit.secondaryFlags,&constructor,&mover,&mission,&missionFlags,&capacity,&category)==9) {
            unit.constructor=constructor!=0;unit.mover=mover!=0;unit.mission=mission!=0;
            unit.missionFlags=uint8_t(missionFlags);unit.capacity=int16_t(capacity);
            std::printf("%d\n",int(retailAiCategory(unit,category)));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--centroid")==0) {
        unsigned count;
        while (std::scanf("%u",&count)==1) {
            RetailAiCentroid centroid;
            for (unsigned i=0;i<count;++i) {
                int valid,x,z;
                if (std::scanf("%d %d %d",&valid,&x,&z)!=3) return 2;
                if (valid) centroid.add(x,z);
            }
            auto center=centroid.center();
            std::printf("%d %d %d\n",int(center.has_value()),center?center->first:0,center?center->second:0);
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--nearest-base")==0) {
        int x,z,minimum;
        while (std::scanf("%d %d %d",&x,&z,&minimum)==3) {
            std::array<RetailAiBaseCandidate,20> bases;
            for (auto& base:bases) {
                int occupied,valid,bx,bz,count;
                if (std::scanf("%d %d %d %d %d",&occupied,&valid,&bx,&bz,&count)!=5) return 2;
                base.occupied=occupied!=0;base.eligibleCount=count;
                if (valid) base.center=std::pair{bx,bz};
            }
            std::printf("%u\n",retailNearestAiBase(bases,x,z,minimum));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--resources")==0) {
        float stored;
        while (std::scanf("%f",&stored)==1) {
            RetailResourceHistory history;
            if (std::scanf("%f %f",&history.income,&history.usage)!=2) return 2;
            for (auto& sample:history.samples) for (float& v:sample)
                if (std::scanf("%f",&v)!=1) return 2;
            history.advance();
            std::printf("%d",int(history.shortfall(stored)));
            for (const auto& sample:history.samples) for (float v:sample)
                std::printf(" %u",std::bit_cast<uint32_t>(v));
            std::putchar('\n');
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--priority")==0) {
        RetailBuildPriority type; unsigned population,shortfall,seed;
        int count,completed,weapon; float ratio;
        while (std::scanf("%u %u %d %d %d %d %f %u %f %u %u",&type.flags,&type.secondaryFlags,
                &count,&completed,&weapon,&type.desired,&type.cost,&population,&ratio,&shortfall,&seed)==11) {
            type.count=int16_t(count); type.completed=int16_t(completed); type.weapon=int16_t(weapon);
            std::vector<int> calls;
            const auto score=retailBuildPriority(type,uint16_t(population),ratio,shortfall!=0,
                [&](int n){calls.push_back(n); return retailRandom(seed,n);});
            std::printf("%u %u %zu",unsigned(score),seed,calls.size());
            for (int n:calls) std::printf(" %d",n);
            std::putchar('\n');
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--empty")==0) {
        unsigned kind,tick,seed,active,dirty; int target,parameter;
        while (std::scanf("%u %u %u %d %d %u %u",&kind,&tick,&seed,&target,&parameter,&active,&dirty)==7) {
            if (kind>4) return 2;
            RetailAiSquad squad; squad.kind=RetailAiSquadKind(kind);
            squad.parameters[0]=target;squad.parameters[5]=parameter;squad.active=active;squad.dirty=dirty;
            RetailAiGroupClock clock;
            retailTickAiSquad(squad,clock,tick,[&](int n){return retailRandom(seed,n);},
                [](auto&){},[](auto&){std::abort();});
            std::printf("%u %u %d %d %u %u\n",clock.deadline,seed,squad.parameters[0],squad.parameters[5],squad.active,squad.dirty);
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--oracle")==0) {
        int countdown; unsigned tick,seed,count,mutate;
        while (std::scanf("%d %u %u %u %u",&countdown,&tick,&seed,&count,&mutate)==5) {
            if (count>100) return 2;
            RetailAiSchedule state; state.assignmentCountdown=countdown;
            for (unsigned i=0;i<count;++i) {
                unsigned slot,deadline;
                if (std::scanf("%u %u",&slot,&deadline)!=2 || slot>=100) return 2;
                state.groups[slot]={true,deadline};
            }
            std::vector<int> events;
            retailTickAiSchedule(state,tick,[&](int n){return retailRandom(seed,n);},
                [&]{events.push_back(-1); if (mutate) state.groups[98]={true,tick};},
                [&]{events.push_back(-2); if (mutate) state.groups[0]={true,tick};},
                [&](unsigned i,auto& group){events.push_back(int(i)); group.deadline=tick+17u;});
            std::printf("%d %u %zu",state.assignmentCountdown,seed,events.size());
            for (int event:events) std::printf(" %d",event);
            for (const auto& group:state.groups) std::printf(" %u %u",unsigned(group.present),group.deadline);
            std::putchar('\n');
        }
        return 0;
    }
    RetailAiSchedule state;
    state.assignmentCountdown=1;
    state.groups[2]={true,9}; state.groups[5]={true,10}; state.groups[99]={true,0};
    uint32_t seed=1; std::vector<int> events;
    retailTickAiSchedule(state,10,[&](int n){return retailRandom(seed,n);},
        [&]{events.push_back(-1);},[&]{events.push_back(-2);},
        [&](unsigned i,auto& group){events.push_back(int(i));group.deadline=100;});
    if (events!=std::vector<int>{-1,-2,2,5} || state.groups[99].deadline!=0) return 1;
    events.clear();
    retailTickAiSchedule(state,11,[&](int n){return retailRandom(seed,n);},
        [&]{events.push_back(-1);},[&]{events.push_back(-2);},
        [&](unsigned i,auto&){events.push_back(int(i));});
    if (!events.empty()) return 1;
    std::puts("PASS: AI scheduling orders refresh, assignment and due groups; skips slot 99");
    World world;
    auto& ai=world.player(0).retailAi.emplace();
    ai.schedule.groups[1]={true,1};ai.squads[1].kind=RetailAiSquadKind::Base;
    std::vector<World::RngObservation> draws;
    world.setRngObserver([&](const auto& draw){draws.push_back(draw);});
    world.tick(1.0f/30.0f);
    if (draws.size()!=1 || draws[0].bound!=30 || ai.schedule.groups[1].deadline!=draws[0].result+16) return 1;
    const auto hash=world.stateHash(); ++ai.schedule.groups[1].deadline;
    if (world.stateHash()==hash) return 1;
    UnitType type;type.canMove=true;type.maxVel=Fixed::fromInt(2);
    const int id=world.spawn(&type,256,256);
    ai.squads[1].members.push_back(id);ai.schedule.groups[1].deadline=2;
    bool stopped=false;
    try { world.tick(1.0f/30.0f); }
    catch (const std::runtime_error& e) { stopped=std::strstr(e.what(),"occupied squad planner")!=nullptr; }
    if (!stopped) return 1;
    std::puts("PASS: restored AI clocks run and hash in World; unsupported occupied planning stops explicitly");
    {
        World assigned;
        UnitType baseType;baseType.id="base";baseType.maxHp=100;baseType.maxVel=Fixed();
        UnitType mobile;mobile.id="mobile";mobile.maxHp=100;mobile.maxVel=Fixed::fromInt(1);mobile.canMove=true;
        const int base1=assigned.spawn(&baseType,1000,1000,0,0);
        const int base2=assigned.spawn(&baseType,300,160,0,0);
        const int newcomer=assigned.spawn(&mobile,300,160,0,0);
        for (int id:{base1,base2,newcomer}) {
            assigned.unit(id)->standbyAllowed=false;assigned.unit(id)->guardNoMoveAllowed=false;
        }
        auto& state=assigned.player(0).retailAi.emplace();
        state.initialized=true;state.anchorsRestored=true;state.schedule.assignmentCountdown=1;
        state.anchors.emplace(1,std::pair<int16_t,int16_t>{20,10});
        for (unsigned i=1;i<=2;++i) {
            state.schedule.groups[i]={true,0xffffffffu};state.squads[i].kind=RetailAiSquadKind::Base;
            state.squads[i].active=1;
        }
        state.squads[1].parameters[0]=1;state.squads[1].members={base1};
        state.squads[2].members={base2};
        // Base two's member centre is nearer. Base one's explicit anchor wins
        // only after moving base two's actual centre away.
        assigned.unit(base2)->x=Fixed::fromInt(600);
        assigned.tick(1.f/30);
        if (state.squads[1].members!=std::vector<int>{base1,newcomer} ||
            assigned.unit(newcomer)->moveState!=1 || assigned.unit(newcomer)->fireState!=2 ||
            state.squads[1].parameters[8]!=0) return 1;
        assigned.tick(1.f/30);
        if (state.squads[1].parameters[8]!=1 || state.squads[1].dirty!=1 || state.squads[2].parameters[8]!=0) return 1;
        const auto before=assigned.stateHash();state.anchors[1].first++;
        if (assigned.stateHash()==before) return 1;
        std::puts("PASS: nearest base assignment uses anchors and updates active squad presence next tick");
        // Without an anchor, every live member contributes, including mobile
        // units. Their integer coordinates are averaged before conversion.
        state.anchors.clear();state.squads[1].parameters[0]=0;
        assigned.unit(base1)->x=Fixed::fromInt(1000);
        assigned.unit(base1)->z=Fixed::fromInt(160);
        assigned.unit(newcomer)->x=Fixed::fromInt(0);
        const int next=assigned.spawn(&mobile,500,160,0,0);
        assigned.unit(next)->standbyAllowed=false;assigned.unit(next)->guardNoMoveAllowed=false;
        state.schedule.assignmentCountdown=1;
        assigned.tick(1.f/30);
        if (state.squads[1].members!=std::vector<int>{base1,newcomer,next}) return 1;
        std::puts("PASS: unanchored base centre includes mobile squad members");
    }
    World cached;
    UnitType structure; structure.id="cache-test";structure.maxHp=100;structure.maxVel=Fixed();structure.buildTime=1000;
    const int completedId=cached.spawn(&structure,256,256);
    const int constructionId=cached.spawn(&structure,512,512);
    for (int id:{completedId,constructionId}) {
        cached.unit(id)->standbyAllowed=false;cached.unit(id)->guardNoMoveAllowed=false;
    }
    cached.unit(constructionId)->underConstruction=true;
    cached.unit(constructionId)->hp=Fixed::fromInt(50);
    auto& player=cached.player(0);
    player.cacheClock={true,0xfffffffau}; // wraps to a refresh on tick one
    auto& cache=player.buildCache.emplace();
    RetailBuildCacheEntry entry;entry.type=&structure;entry.inputs.cost=2040;
    entry.inputs.weapon=-1;cache.entries.push_back(entry);
    unsigned start=0;
    for (;;++start) { auto seed=retailSeed(start); if (retailRandom(seed,30)==0) break; }
    cached.setGameSeed(start);
    draws.clear();cached.setRngObserver([&](const auto& draw){draws.push_back(draw);});
    cached.tick(1.0f/30.0f);
    if (draws.size()<2 || draws[0].bound!=30 || draws[0].result!=0 || draws[1].bound!=30 ||
        cache.entries[0].inputs.count!=2 || cache.entries[0].inputs.completed!=1) {
        std::fprintf(stderr,"cache counts %d/%d; draws",cache.entries[0].inputs.count,cache.entries[0].inputs.completed);
        for (const auto& d:draws) std::fprintf(stderr," %d:%u",d.bound,d.result);
        std::fputc('\n',stderr);return 1;
    }
    auto cacheHash=cached.stateHash();cache.resources.samples[0][1]=9;
    if (cached.stateHash()==cacheHash) return 1;
    auto& planner=cache.planner.emplace(tak::sim::RetailBuildCache::Planner{});
    planner.types.push_back({{1},"test",100,false});
    auto checkPlannerHash=[&](auto change) {
        const auto before=cached.stateHash();change();return before!=cached.stateHash();
    };
    if (!checkPlannerHash([&]{planner.limited=true;}) ||
        !checkPlannerHash([&]{planner.types[0].choices.push_back(1);}) ||
        !checkPlannerHash([&]{planner.types[0].faction="other";}) ||
        !checkPlannerHash([&]{planner.types[0].weight=50;}) ||
        !checkPlannerHash([&]{planner.types[0].special=true;})) return 1;
    cached.unit(completedId)->hp=Fixed();
    player.cacheClock={true,0xfffffffau};cached.setGameSeed(start);
    cached.tick(1.0f/30.0f);
    if (cache.entries[0].inputs.count!=1 || cache.entries[0].inputs.completed!=0) {
        std::fprintf(stderr,"post-death cache counts %d/%d\n",cache.entries[0].inputs.count,cache.entries[0].inputs.completed);return 1;
    }
    RetailResourceHistory history;
    history.usage=10;history.advance();
    if (history.shortfall(100)) return 1; // newest sample is excluded
    history.advance();if (!history.shortfall(100)) return 1;
    for (int i=0;i<29;++i) history.advance();
    if (history.shortfall(100)) return 1;
    std::puts("PASS: World refreshes build counts, records resource history and hashes priority state");
}
