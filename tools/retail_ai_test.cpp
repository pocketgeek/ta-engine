#include "sim/retailai.h"
#include "sim/retailaipatrol.h"
#include "sim/retailplacement.h"
#include "sim/retailmap.h"
#include "sim/retailrng.h"
#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "hpi/hpi.h"
#include "tnt/tnt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

using namespace ta::sim;

int main(int argc,char** argv) {
    if (argc==4 && std::strcmp(argv[1],"--map-features")==0) {
        auto vfs=ta::hpi::mountRetailRoot(argv[2]);
        const auto path=ta::hpi::findMap(vfs,argv[3]);
        const auto map=ta::tnt::Map::load(vfs.read(path),path);
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
    return 0;
}
