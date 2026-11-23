#include "sim/retailtrace.h"
#include "sim/retailscheduler.h"
#include "sim/retailrng.h"
#include "sim/retailgoal.h"
#include "sim/retailgrade.h"
#include "sim/sim.h"
#include <cstdlib>
#include <iostream>

namespace tak::sim {
struct RetailReplayProbe {
    static bool retiredPointKeepsDetachedStamp(bool detached,bool recent) {
        World world;UnitType type;
        const int id=world.spawn(&type,128,128);auto& u=*world.unit(id);
        Order first;first.goal=first.groundMission=true;first.controller=detached?0:1;
        Order next=first;next.controller=2;next.x=Fixed::fromInt(256);
        u.orders={first,next};u.routeStamp=recent?195:194;world.tickCounter_=200;
        world.dropLeg(u);
        return u.orders.size()==1 && u.orders.front().controller==2 &&
            u.routeStamp==((detached || recent)?(recent?195:194):0);
    }
    static bool completedPointDetaches(bool recent,int shape=0) {
        World world;world.setPathService(true);
        world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
        world.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
        UnitType type;type.footX=type.footZ=1;
        const int id=world.spawn(&type,128,128);auto& u=*world.unit(id);
        Order goal;goal.goal=goal.groundMission=true;goal.controller=1;
        goal.x=Fixed::fromInt(136);goal.z=Fixed::fromInt(128);
        goal.missionTarget=std::pair{u.x,u.z};goal.hasSegment=true;goal.segmentX=u.x;goal.segmentZ=u.z;
        UnitType building;
        if (shape==1) {
            goal.park.emplace();
            goal.park->ring=RetailRingGoal{8,8,0,4,0};
        } else if (shape==2) {
            goal.groundMission=false;goal.controller=0;goal.buildType=&building;
            goal.buildRectangle=RetailRectGoal{8,10,8,10};
        }
        u.orders={goal};u.routeStamp=recent?199:100;world.tickCounter_=200;
        world.tickNavigationMovement(u,u.baseSpeed);
        const auto& arrived=u.orders.back();
        if (arrived.controller || !arrived.navigationExhausted || arrived.navigationConsumed ||
            arrived.mission.pending!=0x500 || u.routeStamp!=(recent?199:0)) return false;
        u.x=Fixed::fromInt(136);u.bodyBlockStreak=2;
        const auto rng=world.gameRng_;const auto requests=world.paths_.requests();
        ++world.tickCounter_;world.tickNavigationMovement(u,u.baseSpeed);
        return u.orders.back().navigationConsumed && !world.paths_.pending(id) &&
            world.paths_.requests()==requests && world.gameRng_==rng;
    }
    static bool rectangleRetryAndFailure(bool reachable) {
        World world;world.setPathService(true);
        world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
        world.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
        UnitType mover,site;mover.maxVel=Fixed::fromInt(1);mover.footX=mover.footZ=4;
        mover.buildDist=reachable?512:0;site.footX=site.footZ=2;
        const int id=world.spawn(&mover,128,128);auto& u=*world.unit(id);
        u.orders={world.makeBuildOrder(u,&site,Fixed::fromInt(400),Fixed::fromInt(400))};
        world.tickCounter_=200;u.routeStamp=199;
        if (world.prepareBuildApproach(u) || !u.orders.back().controller ||
            u.orders.back().mission.waitMask!=0x700 || u.orders.back().mission.flags!=0x80508 ||
            !world.paths_.pending(id) || u.routeStamp!=199) return false;
        world.cancelPath(u);
        world.deliverSearchRoute(id,{{8,8},{9,8}},Fixed::fromInt(400),Fixed::fromInt(400),true,false,false,false);
        u.x=u.orders.back().x;u.z=u.orders.back().z;
        if (!world.requestPath(u,u.x.toFloat(),u.z.toFloat()) || !world.paths_.pending(id)) return false;
        world.cancelPath(u);
        world.deliverSearchRoute(id,{},Fixed::fromInt(400),Fixed::fromInt(400),true,false,false,false);
        if (!(u.orders.back().mission.pending&0x200)) return false;
        const bool handoff=world.prepareBuildApproach(u);
        return reachable
            ? handoff && !u.orders.empty() && u.orders.back().mission.waitMask==0 &&
                !(u.orders.back().mission.pending&0x200)
            : !handoff && u.orders.empty() && u.standbyActive && u.routeStamp==199;
    }
    static int gateOwnerGrade(int fx,int fz,int retry,bool capable,int owner,bool opened,bool frame,int dx,int dz) {
        tak::cob::File script;
        World world;world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
        world.setPlayerCount(2);world.player(0).automaticGates=capable;
        world.editNavigationExploration().assign(16*16,0xffff);
        UnitType mover;mover.footX=fx;mover.footZ=fz;
        UnitType gate;gate.maxVel=Fixed();gate.gate=true;gate.footX=gate.footZ=1;
        gate.yardMap=frame?"o":"c";
        const int id=world.spawn(&mover,400,400);
        const int blocker=world.spawn(&gate,float((10+dx)*16+8),float((10+dz)*16+8),{},owner);
        world.unitScripts_.try_emplace(blocker,script).first->second.yardOpen=opened;
        World::SearchGradePlane plane;plane.nav=&world.nav_;plane.footX=fx;plane.footZ=fz;
        plane.cells.assign(32*32,3);world.searchGrades_.push_back(std::move(plane));world.activeSearchGrade_=0;
        return world.searchGrade(id,10+fx/2,10+fz/2,retry,{0,0});
    }
    static int automaticGate(bool active,bool sameOwner,bool hasMover,bool moving,bool allocated,int size,int dx,int dz) {
        World world;world.setTerrain(std::vector<uint8_t>(16*16,0),16,16,0);
        world.setPlayerCount(2);
        UnitType gate;gate.maxVel=Fixed();gate.gate=true;gate.onOffable=true;
        gate.footX=gate.footZ=2;gate.yardMap="cccc";
        UnitType body;body.maxVel=hasMover ? Fixed::fromInt(1) : Fixed();body.footX=body.footZ=size;
        // The body came while the passage was open and a gate cannot close
        // on a held cell (507ae0), so the body holds the ground word where
        // they overlap (5066f0): place it first.
        const int other=world.spawn(&body,float((6+dx)*16+(size-1)*8),float((6+dz)*16+(size-1)*8),{},sameOwner?0:1);
        const int id=world.spawn(&gate,104,104);
        world.unit(other)->speed=Fixed::raw(moving?1:0);
        if (!allocated) world.unit(other)->deadFor=0;
        world.unit(id)->active=active;
        world.player(0).automaticGates=true;
        world.tickAutomaticGates();
        return world.unit(id)->active==active ? -1 : int(world.unit(id)->active);
    }
    static int gateGrade(bool gate,bool open,bool flooded,int x,int z,int width,int height) {
        tak::cob::File script;
        World world;world.setTerrain(std::vector<uint8_t>(8*8,0),8,8,flooded?32:0);
        world.setMapPlacementFeatures(std::vector<uint16_t>(8*8,0xffff),{});
        UnitType type;type.footX=type.footZ=3;type.gate=gate;type.maxVel=Fixed();
        type.yardMap="ocCo..cCo";
        const int id=world.spawn(&type,56,56);
        world.unitScripts_.try_emplace(id,script).first->second.yardOpen=open;
        World::SearchGradePlane plane;plane.nav=&world.nav_;
        return world.rawSearchGrade(plane,x,z,width,height);
    }
    static bool constructionSearchOccupancy(bool unfinished,int flight=0) {
        World world;world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
        UnitType mover;mover.footX=mover.footZ=4;mover.maxVel=Fixed::fromInt(1);
        UnitType body=mover;body.footX=body.footZ=2;
        body.canFly=flight!=0;
        const int id=world.spawn(&mover,104,104),blocker=world.spawn(&body,200,200);
        world.unit(blocker)->underConstruction=unfinished;
        world.unit(blocker)->flightGroundMode=flight==2?2:1;
        world.unit(blocker)->groundGradeTick=15;
        World::SearchGradePlane plane;plane.nav=&world.navFor(&mover);
        plane.footX=plane.footZ=4;plane.navVersion=plane.nav->version();
        plane.cells.assign(32*32,6);world.searchGrades_.push_back(std::move(plane));
        world.tickCounter_=200;world.prepareSearchGrade(id,false);
        const auto& cells=world.searchGrades_[size_t(world.activeSearchGrade_)].cells;
        return cells[9*32+9]==(flight==2?6:0) && cells[8*32+9]==(flight==2?6:4) && cells[7*32+9]==6;
    }
    static bool sharedSearchGradeSnapshot() {
        World batched,serial;
        batched.setPathProfiling(true);
        UnitType mover,wide,flyer;
        mover.footX=mover.footZ=1;mover.maxVel=Fixed::fromInt(1);
        wide=mover;wide.footX=wide.footZ=2;
        flyer=mover;flyer.canFly=true;
        for (World* world:{&batched,&serial}) {
            world->setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
            world->setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
            world->spawn(&mover,104,104);world->spawn(&wide,200,200);
            const int flying=world->spawn(&flyer,280,280);
            world->unit(flying)->flightGroundMode=2;
            world->tickCounter_=200;
        }
        auto compare=[&] {
            if (batched.stateHash()!=serial.stateHash() || batched.searchGrades_.size()!=serial.searchGrades_.size())
                return false;
            for (size_t i=0;i<batched.searchGrades_.size();++i)
                if (batched.searchGrades_[i].cells!=serial.searchGrades_[i].cells) return false;
            return true;
        };
        // Each request changes grade-plane requester/age state, but none changes
        // the body snapshot; retries and different footprints share it safely.
        auto requests=[&] {
            for (int i=0;i<6;++i) {
                const int id=batched.units_[size_t(i%2)].id;
                batched.prepareSearchGrade(id,i==4);serial.prepareSearchGrade(id,i==4);
                if (!compare()) return false;
                batched.finishSearchGrade(id);serial.finishSearchGrade(id);
                if (!compare()) return false;
            }
            return true;
        };
        {
            World::SearchGradeBatch batch(batched);
            if (!requests() || batched.searchGradeBodies_.size()!=2 || batched.pathDiagnostics().bodySnapshotRebuilds!=1)
                return false;
        }
        // Change eligibility, position and age at the SAME simulation tick.
        // A new batch and calls outside a batch must both observe those edits.
        for (World* world:{&batched,&serial}) {
            world->units_[0].x=Fixed::fromInt(120);world->units_[0].groundGradeTick=195;
            world->units_[2].flightGroundMode=1;
        }
        {
            World::SearchGradeBatch batch(batched);
            if (!requests() || batched.searchGradeBodies_.size()!=3 || batched.pathDiagnostics().bodySnapshotRebuilds!=2)
                return false;
        }
        for (World* world:{&batched,&serial}) world->units_[2].flightGroundMode=2;
        return requests() && batched.searchGradeBodies_.size()==2 && !batched.searchGradeBodiesValid_ &&
            batched.pathDiagnostics().bodySnapshotRebuilds==8 && serial.pathDiagnostics().bodySnapshotRebuilds==0;
    }
    static bool preciseSearchDestination(bool boat,int foot,int delta) {
        World world;world.setPathService(true);world.setPathBudget(12000);
        world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,boat?32:0);
        UnitType type;type.footX=type.footZ=foot;type.maxVel=Fixed::fromInt(1);
        type.floater=boat;type.domain=boat?UnitType::Domain::Water:UnitType::Domain::Ground;
        const int id=world.spawn(&type,104,104);
        auto& u=*world.unit(id);
        const Fixed x=Fixed::raw((768+(foot-1)*8)*65536+delta),z=Fixed::fromInt(256);
        Order goal;goal.goal=goal.groundMission=true;goal.controller=1;
        goal.x=x;goal.z=z;goal.missionTarget=std::pair{x,z};
        goal.missionRadius=uint32_t(-4);u.orders={goal};
        world.paths_.setGradeHost({});
        if (!world.requestPath(u,x.toFloat(),z.toFloat())) return false;
        bool delivered=false,matched=false;
        for (int tick=1;tick<=100 && !delivered;++tick)
            world.paths_.tick([](int,int,int){return 6;},
                [&](int,const std::vector<PathCell>& route,Fixed gx,Fixed gz,bool,bool,bool,bool) {
                    delivered=true;
                    matched=gx==x && gz==z && !route.empty() &&
                        route.back().x==footprintCell(x,foot) && route.back().z==footprintCell(z,foot);
                });
        return delivered && matched;
    }
    static std::vector<int> resetRoute(int fx,int fz,bool boat,uint32_t flags,bool active,
                           int32_t ux,int32_t uz,int32_t gx,int32_t gz,
                           const std::vector<RetailSteeringPoint>& points) {
        World world;world.setPathService(true);
        world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,boat?32:0);
        UnitType type;type.footX=fx;type.footZ=fz;type.maxVel=Fixed::fromInt(1);
        type.floater=boat;type.minWaterDepth=boat?13:-10000;type.maxWaterDepth=boat?10000:20;
        type.domain=boat?UnitType::Domain::Water:UnitType::Domain::Ground;
        const int id=world.spawn(&type,104,104);
        auto& u=*world.unit(id);u.x=Fixed::raw(ux);u.z=Fixed::raw(uz);
        for (size_t i=1;i<points.size();++i) {
            Order order;order.x=points[i].x;order.z=points[i].z;
            order.hasSegment=true;order.segmentX=points[i-1].x;order.segmentZ=points[i-1].z;
            if (i+1==points.size()) {
                order.goal=order.groundMission=true;order.controller=1;
                order.mission.flags=flags;order.navigationExhausted=!active;
                order.missionTarget=std::pair{Fixed::raw(gx),Fixed::raw(gz)};
            }
            u.orders.push_back(order);
        }
        Order queued;queued.goal=queued.groundMission=true;queued.controller=999;
        queued.x=Fixed::fromInt(432);queued.z=Fixed::fromInt(448);u.orders.push_back(queued);
        world.nextMovementController_=1;
        world.tickGroundMission(u);
        const auto end=World::currentLeg(u.orders);
        if (u.orders.size()!=end+2 || u.orders.back().controller!=999 ||
            u.orders.back().x!=queued.x || u.orders.back().z!=queued.z)
            throw std::runtime_error("reset changed the queued command");
        std::vector<int> result{!u.orders[end].navigationExhausted,int(end+2),
            u.orders.front().segmentX.floorInt(),u.orders.front().segmentZ.floorInt()};
        for (size_t i=0;i<=end;++i) result.insert(result.end(),{u.orders[i].x.floorInt(),u.orders[i].z.floorInt()});
        return result;
    }
    static void resetDestination(World& world,Unit& u) {
        auto& goal=u.orders.back();
        goal.missionTarget=goal.missionTarget.value_or(std::pair{goal.x,goal.z});
        goal.x=Fixed::fromInt(80);goal.z=Fixed::fromInt(96);
        goal.mission.stage=0;goal.mission.waitMask=0;
        world.tickGroundMission(u);
    }
    // Extended inputs (start cell, initial heading, type turn/road/water
    // multipliers) default to the fixed check_world_search.py fixture; the
    // randomized fuzz_world_search.py supplies them through --world-search2.
    struct SearchVariant { int sx=6,sz=6,heading=0,turn=-1,road=-1,water=-1; };
    static void search(int fx,int fz,bool boat,int budget,int gx,int gz,int exploration,bool admission,bool moving,bool terrain,
                       const std::vector<int>& grades) {
        search(fx,fz,boat,budget,gx,gz,exploration,admission,moving,terrain,grades,SearchVariant{});
    }
    static void search(int fx,int fz,bool boat,int budget,int gx,int gz,int exploration,bool admission,bool moving,bool terrain,
                       const std::vector<int>& grades,SearchVariant v) {
        World world;world.setPathService(true);world.setPathBudget(budget);
        std::vector<uint8_t> heights(32*32,0);
        if (terrain) heights.assign(grades.begin(),grades.end());
        world.setTerrain(heights,32,32,boat?32:0);
        UnitType type;type.footX=fx;type.footZ=fz;type.maxVel=Fixed::fromInt(1);
        type.turnRate=v.turn>=0?v.turn:moving?700:30;
        type.roadMult=Fixed::raw(v.road>=0?v.road:moving?98304:65536);
        type.waterMult=Fixed::raw(v.water>=0?v.water:moving?49152:65536);
        type.floater=boat;type.minWaterDepth=boat?13:-10000;type.maxWaterDepth=boat?10000:20;
        type.domain=boat?UnitType::Domain::Water:UnitType::Domain::Ground;
        const int id=world.spawn(&type,v.sx*16+fx*8,v.sz*16+fz*8);
        UnitType blocker=type;blocker.footX=blocker.footZ=2;
        if (terrain) world.spawn(&blocker,20*16+16,18*16+16);
        auto& u=*world.unit(id);
        u.heading=retailHeadingToPort(uint16_t(v.heading));u.groundTerrainFlags=0;u.routeStamp=0;u.baseSpeed=Fixed::fromInt(1);
        Order goal;goal.goal=goal.groundMission=true;goal.controller=1;
        goal.x=Fixed::fromInt(gx*16+fx*8);goal.z=Fixed::fromInt(gz*16+fz*8);
        goal.missionRadius=uint32_t(-4);goal.missionTarget=std::pair{goal.x,goal.z};
        goal.hasSegment=true;goal.segmentX=u.x;goal.segmentZ=u.z;u.orders={goal};
        world.paths_.setEntityPool(0,0,4);
        if (exploration>=0 && !terrain) {
            World::SearchGradePlane plane;plane.nav=&world.nav_;plane.footX=fx;plane.footZ=fz;
            plane.cells.assign(grades.begin(),grades.end());world.searchGrades_.push_back(std::move(plane));
        }
        if (exploration>=0) {
            auto& explored=world.editNavigationExploration();
            for (int z=0;z<16;++z) for (int x=0;x<16;++x)
                explored[size_t(z)*16+x]=exploration==0?65535:exploration==1?0:uint16_t(1<<((x/4+z/4)%2));
        }
        uint64_t queries=0;
        const bool trace=std::getenv("TAK_SEARCH_DIAG")!=nullptr;
        auto word=[&](int value) {
            for (int byte=0;byte<4;++byte) { queries^=(uint32_t(value)>>(byte*8))&255;queries*=1099511628211ull; }
        };
        world.paths_.setGradeHost({[&](int who,bool last){
                if (terrain) world.prepareSearchGrade(who,last);
                else world.activeSearchGrade_=exploration>=0?0:-1;
            },
            [&](int who,int cx,int cz,int retry,PathCell start) {
            const int x=cx-fx/2,z=cz-fz/2;word(x);word(z);
            const int result=exploration>=0 ? world.searchGrade(who,cx,cz,retry,start) :
                x<0 || z<0 || x>=32 || z>=32 ? 0:grades[size_t(z)*32+x];
            if (trace) std::cerr<<world.tickCounter_<<' '<<x<<' '<<z<<' '<<result<<'\n';
            return result;
        },[&](int who){if (terrain) world.finishSearchGrade(who);else world.activeSearchGrade_=-1;}});
        if (terrain) { world.prepareSearchGrade(id,false);world.finishSearchGrade(id); }
        if (!world.requestPath(u,goal.x.toFloat(),goal.z.toFloat())) throw std::runtime_error("search fixture rejected");
        for (int tick=1;tick<=5000 && world.paths_.pending(id);++tick) {
            world.tickCounter_=uint32_t(tick);
            if (moving) {
                const int x=v.sx+(tick<5?0:tick<12?1:tick<18?2:1),z=v.sz+(tick<10?0:1);
                u.x=Fixed::fromInt(x*16+fx*8);u.z=Fixed::fromInt(z*16+fz*8);
                u.heading=retailHeadingToPort(tick<12?0:tick<18?16384:32768);
                u.groundTerrainFlags=tick<10?0:tick<18?0x800:0x1000;
            }
            queries=14695981039346656037ull;
            world.paths_.tick([](int,int,int){return 0;},[&](int who,const std::vector<PathCell>& route,
                    Fixed x,Fixed z,bool failed,bool crowded,bool traffic,bool detour) {
                world.deliverSearchRoute(who,route,x,z,failed,crowded,traffic,detour);
            },[&](int who,PathCell& start,int& heading,RetailCostSearch::Costs& costs) {
                world.refreshSearchRequest(who,start,heading,costs);
            },uint32_t(tick),[&](int who,uint32_t now) {
                return !admission || world.admitSearchRequest(who,now);
            });
            for (const auto& event:world.paths_.takeNotifications())
                u.orders[World::currentLeg(u.orders)].mission.pending|=uint32_t(event.events);
            const auto& end=u.orders[World::currentLeg(u.orders)];
            std::cout<<tick<<' '<<world.paths_.pending(id)<<' '<<u.routeStamp<<' '<<queries<<' '<<end.mission.pending<<' '
                     <<!end.navigationExhausted<<' '<<(u.routeCrowded|u.routeTraffic<<1|u.routeFailed<<2|u.routeDetour<<3)
                     <<' '<<World::currentLeg(u.orders)+2<<' '<<u.orders.front().segmentX.floorInt()<<' '
                     <<u.orders.front().segmentZ.floorInt();
            for (size_t i=0;i<=World::currentLeg(u.orders);++i)
                std::cout<<' '<<u.orders[i].x.floorInt()<<' '<<u.orders[i].z.floorInt();
            if (terrain) {
                uint64_t cache=14695981039346656037ull;
                for (auto value:world.searchGrades_.front().cells)
                    for (int byte=0;byte<4;++byte) { cache^=(uint32_t(value)>>(byte*8))&255;cache*=1099511628211ull; }
                std::cout<<' '<<cache;
            }
            std::cout<<'\n';
        }
        if (world.paths_.pending(id)) throw std::runtime_error("search fixture did not finish");
        std::cout<<"END\n";
    }
    // The production retry ladder re-asks for a unit that is still pending.
    // Native 4e5268 calls 4e4f50(1), which returns at 4e4f68 once the
    // navigator's pending bit (+0x114 & 2) is set: the worker keeps its
    // search however far the unit has moved, and 416430 re-reads the start
    // only when it admits a request. A changed destination still restarts.
    // Native: 4e4f50 emulated after 0/2/9-cell drift, worker before == after.
    // Returns admissions by delivery, or -1 if the fixture went wrong.
    static int pendingRetryAfterDrift(int drift,bool retarget) {
        World world;world.setPathService(true);world.setPathBudget(300);
        world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
        world.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
        UnitType type;type.maxVel=Fixed::fromInt(1);type.footX=type.footZ=2;
        const int id=world.spawn(&type,6*16+16,20*16+16);auto& u=*world.unit(id);
        Order goal;goal.goal=goal.groundMission=true;goal.controller=9;
        goal.x=Fixed::fromInt(56*16+16);goal.z=Fixed::fromInt(20*16+16);
        goal.missionTarget=std::pair{goal.x,goal.z};
        goal.hasSegment=true;goal.segmentX=u.x;goal.segmentZ=u.z;u.orders={goal};
        if (!world.requestPath(u,goal.x.toFloat(),goal.z.toFloat())) return -1;
        bool delivered=false;
        auto tick=[&] {
            ++world.tickCounter_;
            world.paths_.tick([](int,int,int){return 0;},[&](int who,const std::vector<PathCell>& route,
                    Fixed x,Fixed z,bool failed,bool crowded,bool traffic,bool detour) {
                delivered=true;world.deliverSearchRoute(who,route,x,z,failed,crowded,traffic,detour);
            },[&](int who,PathCell& start,int& heading,RetailCostSearch::Costs& costs) {
                world.refreshSearchRequest(who,start,heading,costs);
            },world.tickCounter_,[&](int who,uint32_t now) { return world.admitSearchRequest(who,now); });
        };
        // Past every crowded-retry draw (at most 38 x halfCellTicks).
        world.tickCounter_=20000;
        // A production pool may place it anywhere in 250 slots: at most six
        // ticks of 43 scans reach it.
        for (int n=0;n<8 && world.paths_.requests()==0;++n) tick();
        const auto active=world.paths_.scheduler_.active;
        if (world.paths_.requests()!=1 || delivered || !world.paths_.pending(id) ||
            world.paths_.activeId_!=id) return -1;
        // Drift east, then fire the crowded branch of the retry ladder.
        u.x+=Fixed::fromInt(16*drift);u.orders.front().segmentX=u.x;
        u.routeCrowded=true;u.routeStamp=0;u.bodyBlockStreak=0;
        if (retarget) {
            // The controller's point is the destination World submits.
            u.orders.front().missionTarget=std::pair{goal.x,goal.z+Fixed::fromInt(64)};
            world.requestPath(u,goal.x.toFloat(),goal.z.toFloat()+64);
        }
        else world.tickNavigationMovement(u,Fixed());
        if (!world.paths_.pending(id)) return -1;
        if (!retarget && (world.paths_.requests()!=1 || world.paths_.activeId_!=id ||
                          !(world.paths_.scheduler_.active==active))) return -1;
        for (int n=0;n<2000 && !delivered;++n) tick();
        return delivered ? int(world.paths_.requests()) : -1;
    }
    // Production entity pools (no fixture setEntityPool). 511570 gives each
    // player maxunits contiguous records; 5121d8 places a unit in a CRT-ranked
    // free slot; 416430 scans those slots, holes included, at 7 work each.
    // Native: check_entity_slots.py (5121d8) and check_crowd_arrival.py
    // --world-tick, whose native pool is built by 5121d8 at the match's capacity.
    static std::vector<int> referenceSlots(uint32_t seed,int capacity,const std::vector<int>& owners) {
        std::array<std::vector<bool>,10> used;std::array<int,10> allocated{};std::vector<int> slots;
        for (int owner:owners) {
            auto& pool=used[size_t(owner)];if (pool.empty()) pool.assign(size_t(capacity),false);
            const int slot=retailAllocateSlot(seed,capacity,allocated[size_t(owner)]++,[&](int i){return bool(pool[size_t(i)]);});
            pool[size_t(slot)]=true;slots.push_back(slot);
        }
        return slots;
    }
    static bool productionPoolSlots() {
        World world;world.setPlayerCount(2);world.setGameSeed(2631);
        world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
        UnitType type;type.maxVel=Fixed::fromInt(1);type.footX=type.footZ=2;
        const std::vector<int> owners{0,1,0,1,0};
        std::vector<int> ids;
        for (size_t i=0;i<owners.size();++i) ids.push_back(world.spawn(&type,float(4+3*i)*16,64.0f,{},owners[i]));
        // Uncapped: retail's default maxunits; one shared CRT stream, but each
        // player's free slots are ranked within its own pool only.
        const auto expected=referenceSlots(2631,250,owners);
        for (size_t i=0;i<ids.size();++i) if (world.entitySlot(ids[i])!=expected[i]) return false;
        if (world.entitySlots_.unitAt[0].size()!=250 || world.entitySlots_.unitAt[1].size()!=250) return false;
        uint32_t crt=2631;for (size_t i=0;i<owners.size();++i) retailCrtRandom(crt);
        if (world.windRng_!=crt) return false;
        // A capture moves the unit to a new record in its new owner's pool.
        auto& moved=*world.unit(ids[0]);const int old=world.entitySlot(moved.id);
        world.captureUnit(moved,1);
        std::vector<bool> taken(250,false);
        for (size_t i=0;i<ids.size();++i) if (owners[i]==1) taken[size_t(expected[i])]=true;
        const int next=retailAllocateSlot(crt,250,2,[&](int i){return bool(taken[size_t(i)]);});
        return world.entitySlot(moved.id)==next && world.entitySlots_.unitAt[1][size_t(next)]==moved.id &&
            world.entitySlots_.unitAt[0][size_t(old)]==-1 && world.windRng_==crt;
    }
    // 512ae0 frees a record when it retires the unit (after the death
    // animation, where 512ee0 places the corpse), not when the corpse decays.
    static bool productionPoolReuse() {
        World world;world.setGameSeed(77);world.setUnitCap(20);
        world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
        UnitType type;type.maxVel=Fixed::fromInt(1);
        std::vector<int> ids;
        for (int i=0;i<20;++i) ids.push_back(world.spawn(&type,float(16+16*(i%10)),float(16+32*(i/10)),{},0));
        if (world.entitySlots_.unitAt[0].size()!=20) return false;
        auto& dying=*world.unit(ids[3]);dying.deadFor=5;dying.deathAnimationTicks=10;dying.corpseUntil=900;
        auto& retired=*world.unit(ids[11]);retired.deadFor=10;retired.deathAnimationTicks=10;retired.corpseUntil=900;
        const int freed=world.entitySlot(retired.id);
        const uint32_t before=world.windRng_;
        // One free record (the retired one): rank 0, still one CRT draw.
        const int reuse=world.spawn(&type,400,400,{},0);
        uint32_t crt=before;retailCrtRandom(crt);
        if (world.entitySlot(reuse)!=freed || world.entitySlot(retired.id)!=-1 || world.windRng_!=crt) return false;
        if (world.entitySlot(dying.id)<0) return false;
        // Full (the dying unit still holds its record): no draw. Retail
        // refuses the creation; the engine grows the pool by one slot.
        const int extra=world.spawn(&type,432,400,{},0);
        return world.windRng_==crt && world.entitySlot(extra)==20 && world.entitySlots_.unitAt[0].size()==21;
    }
    // Grow as needed: a pool starts at what 511570 allocates for the cap
    // (499590 clamps maxunits to 20..500; uncapped is the 250 default) and
    // gains a slot only when a unit finds it full. At the engine's default
    // cap of 2,000, 416430 then pays 7 x 500 a scan, not 7 x 2,000.
    static bool productionPoolCapacity() {
        UnitType type;type.maxVel=Fixed::fromInt(1);
        const std::pair<int,int> sizes[]={{20,20},{250,250},{500,500},{1000,500},{2000,500},{0,250},{5,20}};
        for (const auto& [cap,size]:sizes) {
            World world;world.setGameSeed(77);world.setUnitCap(cap);
            world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
            world.spawn(&type,64,64,{},0);
            if (World::entityPoolCapacity(cap)!=size || world.entitySlots_.unitAt[0].size()!=size_t(size) ||
                world.paths_.poolCapacity(0)!=size || !world.entitySlots_.unitAt[1].empty()) return false;
        }
        // A raised cap grows an allocated pool to its new size, never past 500.
        World raised;raised.setUnitCap(250);raised.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
        raised.spawn(&type,64,64,{},0);raised.setUnitCap(2000);
        if (raised.entitySlots_.unitAt[0].size()!=500) return false;
        // Default cap: 500 live units fill the pool, one CRT draw each; the
        // 501st takes the one new slot without a draw.
        World world;world.setGameSeed(2631);world.setUnitCap(2000);
        world.setTerrain(std::vector<uint8_t>(128*128,0),128,128,0);
        for (int i=0;i<500;++i) world.spawn(&type,float(16+16*(i%100)),float(16+16*(i/100)),{},0);
        uint32_t crt=2631;for (int i=0;i<500;++i) retailCrtRandom(crt);
        if (world.entitySlots_.unitAt[0].size()!=500 || world.windRng_!=crt) return false;
        const int extra=world.spawn(&type,16,400,{},0);
        return world.entitySlot(extra)==500 && world.entitySlots_.unitAt[0].size()==501 &&
            world.windRng_==crt && world.paths_.poolCapacity(0)==501;
    }
    // 416430 admits in slot order, paying 7 per scanned slot: at budget 300 a
    // tick scans 43 slots. Seed 2631 places units 1,2,3 in slots 65,60,248,
    // so unit 2 is admitted first, on tick 2. ID-addressed pools admitted
    // unit 1 on tick 1. A cursor restored to slot 249 wraps on its first scan
    // and stamps the wrap tick that weights the next initialization.
    static bool productionPoolAdmission(bool wrap) {
        World world;world.setPathService(true);world.setPathBudget(300);world.setGameSeed(2631);
        world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
        world.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
        UnitType type;type.maxVel=Fixed::fromInt(1);type.footX=type.footZ=2;
        std::vector<int> ids;
        for (int i=0;i<3;++i) {
            const int id=world.spawn(&type,6*16+16,float(10+12*i)*16+16);ids.push_back(id);
            auto& u=*world.unit(id);u.routeStamp=0;
            Order goal;goal.goal=goal.groundMission=true;goal.controller=uint64_t(9+i);
            goal.x=Fixed::fromInt(56*16+16);goal.z=u.z;goal.missionTarget=std::pair{goal.x,goal.z};
            goal.hasSegment=true;goal.segmentX=u.x;goal.segmentZ=u.z;u.orders={goal};
        }
        const auto slots=referenceSlots(2631,250,{0,0,0});
        if (slots!=std::vector<int>{65,60,248}) return false;
        for (int id:ids) {
            auto& u=*world.unit(id);
            if (!world.requestPath(u,u.orders.front().x.toFloat(),u.orders.front().z.toFloat())) return false;
        }
        world.tickCounter_=20000;
        if (wrap) world.paths_.restoreTraversal(0,{249},{5});
        std::vector<int> delivered,admittedAt;uint64_t admissions=0;
        for (int n=0;n<4000 && delivered.size()<ids.size();++n) {
            const uint32_t now=++world.tickCounter_;
            world.paths_.tick([](int,int,int){return 0;},[&](int who,const std::vector<PathCell>& route,
                    Fixed x,Fixed z,bool failed,bool crowded,bool traffic,bool detour) {
                delivered.push_back(who);world.deliverSearchRoute(who,route,x,z,failed,crowded,traffic,detour);
            },[&](int who,PathCell& start,int& heading,RetailCostSearch::Costs& costs) {
                world.refreshSearchRequest(who,start,heading,costs);
            },now,[&](int who,uint32_t at) { return world.admitSearchRequest(who,at); });
            for (;admissions<world.paths_.requests();++admissions) admittedAt.push_back(int(now-20000));
            if (wrap && n==0 && (world.paths_.scheduler_.lastWrapTick[0]!=int(now) || world.paths_.requests())) return false;
        }
        if (delivered.size()!=ids.size()) return false;
        // Slot 60 is scan 60 from slot 0, or scan 61 after wrapping 249 -> 0:
        // tick 2 either way, and slot order thereafter.
        return delivered==std::vector<int>{ids[1],ids[0],ids[2]} && admittedAt.front()==2;
    }
    // Whether a ground request from `player` joins retail's 5x budget class.
    static bool budgetClass(PathfindingMode mode,bool narrowed,int player) {
        World world;world.setPathService(true);world.setPlayerCount(4);
        world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
        world.setPathfindingMode(mode);
        if (narrowed) world.setPathBudgetClasses(1u);
        UnitType type;type.maxVel=Fixed::fromInt(1);
        const int id=world.spawn(&type,6*16+8,6*16+8,{},player);
        if (!world.requestPath(*world.unit(id),26*16+8,20*16+8)) return false;
        return world.paths_.priorityByPlayer_[size_t(player)]!=0;
    }
    // A ground goal in the requester's own footprint cell. Retail 4e54e0 still
    // submits (4e5540 push 1; call 4e4f50); 415170 accepts the start at once
    // (500 work, event 0x1000) and 4e4ea0 delivers no points. Native: probe
    // fuzz_world_search.py --same-cell. The other modes keep their cancel.
    static bool sameCellRequest(PathfindingMode mode) {
        World world;world.setPathService(true);world.setPlayerCount(2);
        world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);world.setPathfindingMode(mode);
        UnitType type;type.maxVel=Fixed::fromInt(1);type.footX=type.footZ=2;
        const int id=world.spawn(&type,6*16+16,6*16+16);auto& u=*world.unit(id);u.routeStamp=0;
        Order goal;goal.goal=goal.groundMission=true;goal.controller=9;
        goal.x=Fixed::fromInt(6*16+21);goal.z=Fixed::fromInt(6*16+19);goal.missionTarget=std::pair{goal.x,goal.z};
        goal.hasSegment=true;goal.segmentX=u.x;goal.segmentZ=u.z;u.orders={goal};
        const bool submitted=world.requestPath(u,goal.x.toFloat(),goal.z.toFloat());
        if (mode!=PathfindingMode::Retail) return !submitted && !world.paths_.pending(id);
        if (!submitted || !world.paths_.pending(id)) return false;
        world.tickCounter_=20;
        world.paths_.tick([](int,int,int){return 0;},[&](int who,const std::vector<PathCell>& route,
                Fixed x,Fixed z,bool failed,bool crowded,bool traffic,bool detour) {
            world.deliverSearchRoute(who,route,x,z,failed,crowded,traffic,detour);
        },[&](int who,PathCell& start,int& heading,RetailCostSearch::Costs& costs) {
            world.refreshSearchRequest(who,start,heading,costs);
        },20,[&](int who,uint32_t now) { return world.admitSearchRequest(who,now); });
        const auto events=world.paths_.takeNotifications();
        const auto& end=u.orders.front();
        // Completed in the admitting tick: 0x1000 for this controller, no
        // points, navigator inactive, stored point kept, no 0x200 for the
        // accepting circle, admission stamped.
        return !world.paths_.pending(id) && events.size()==1 && events[0].controller==9 &&
               events[0].events==0x1000 && u.orders.size()==1 && end.navigationExhausted &&
               end.x==goal.x && end.z==goal.z && end.controller==9 && !(end.mission.pending&0x200) &&
               u.routeStamp==20;
    }
    // Full World::tick: the mover's 4e5150 arrival check runs before the
    // scheduler 416430 (51e1e5..51e21b, then 4f6ca0), detaches the accepting
    // controller with 0x100|0x400 and cancels the submission through 4e54e0(0),
    // so the search never charges work. The order then retires on dispatch.
    static bool sameCellOrder(PathfindingMode mode,uint32_t& retired) {
        World world;world.setPathfindingMode(mode);
        world.setTerrain(std::vector<uint8_t>(32*32,100),32,32,20);world.setPathService(true);
        UnitType type;type.id=type.name="same-cell";type.maxVel=Fixed::fromFloat(70.f/30);
        type.turnRate=10000;type.maxHp=100;type.canMove=true;type.footX=type.footZ=2;
        const int id=world.spawn(&type,6*16+16,6*16+16,0,0);
        world.order(id,6*16+21,6*16+19,false);
        if (world.paths_.pending(id)!=(mode==PathfindingMode::Retail)) return false;
        for (int n=0;n<60;++n) {
            world.tick(1.f/30);
            if (world.paths_.pending(id)) return false;
            if (world.unit(id)->orders.empty()) { retired=world.tickCounter_;return true; }
        }
        return false;
    }
    static bool emptyDelivery(bool accepted) {
        World world;UnitType type;type.maxVel=Fixed::fromInt(1);
        const int id=world.spawn(&type,128,128);auto& u=*world.unit(id);
        Order goal;goal.goal=goal.groundMission=true;goal.controller=17;
        goal.x=goal.z=Fixed::fromInt(accepted?128:800);
        Order queued;queued.goal=true;queued.x=Fixed::fromInt(900);
        u.orders={goal,queued};u.routeStamp=77;u.groundScanTick=123;
        world.deliverSearchRoute(id,{},goal.x,goal.z,true,false,true,false);
        if (u.orders.size()!=2 || !u.orders.front().navigationExhausted ||
            u.orders.front().mission.pending!=(accepted?0u:0x200u) ||
            u.orders.front().controller!=17 || u.orders.back().x!=queued.x ||
            u.routeStamp!=77 || u.groundScanTick!=0 || !u.routeFailed || !u.routeTraffic) return false;
        world.deliverSearchRoute(id,{{8,8},{12,16}},goal.x,goal.z,false,false,false,false);
        return u.orders.size()==2 && !u.orders.front().navigationExhausted &&
            u.orders.front().mission.pending==(accepted?0u:0x200u) &&
            u.orders.front().controller==17 && u.orders.back().x==queued.x;
    }
    static void delivery(int fx,int fz,bool accepted,bool inactive,int flags,const std::vector<PathCell>& route) {
        World world;UnitType type;type.footX=fx;type.footZ=fz;
        const int id=world.spawn(&type,128,128);
        auto& u=*world.unit(id);
        Order goal;goal.x=Fixed::fromInt(256);goal.z=Fixed::fromInt(384);
        goal.goal=goal.groundMission=true;goal.controller=1;
        goal.hasSegment=true;goal.segmentX=u.x;goal.segmentZ=u.z;
        goal.navigationExhausted=inactive;
        goal.missionTarget=std::pair{Fixed::fromInt(accepted?128:2000),Fixed::fromInt(accepted?128:2000)};
        u.orders={goal};u.groundScanTick=123;u.routeStamp=77;
        world.deliverSearchRoute(id,route,goal.missionTarget->first,goal.missionTarget->second,
                                flags&4,flags&1,flags&2,flags&8);
        const auto& result=u.orders[World::currentLeg(u.orders)];
        std::cout<<!result.navigationExhausted<<' '<<result.mission.pending<<' '
                 <<u.groundScanTick<<' '<<u.routeStamp<<' '
                 <<(u.routeCrowded|u.routeTraffic<<1|u.routeFailed<<2|u.routeDetour<<3);
        if (!route.empty()) {
            std::cout<<' '<<World::currentLeg(u.orders)+2;
            std::cout<<' '<<u.orders.front().segmentX.floorInt()<<' '<<u.orders.front().segmentZ.floorInt();
            for (size_t i=0;i<=World::currentLeg(u.orders);++i)
                std::cout<<' '<<u.orders[i].x.floorInt()<<' '<<u.orders[i].z.floorInt();
        }
        std::cout<<'\n';
    }
    static int grade(int fx,int fz,int player,int sx,int sz,int retry,int x,int z,int cached,int mask) {
        World world;world.setPlayerCount(kMaxPlayers);
        world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
        UnitType type;type.footX=fx;type.footZ=fz;
        const int id=world.spawn(&type,128,128,{},player);
        auto& explored=world.editNavigationExploration();
        std::fill(explored.begin(),explored.end(),uint16_t(mask));
        World::SearchGradePlane plane;
        plane.nav=&world.nav_;plane.footX=fx;plane.footZ=fz;
        plane.cells.assign(32*32,uint8_t(cached));
        world.searchGrades_.push_back(std::move(plane));world.activeSearchGrade_=0;
        return world.searchGrade(id,x+fx/2,z+fz/2,retry,{sx+fx/2,sz+fz/2});
    }
};
}

static void word(uint64_t& hash, int value) {
    for (int byte = 0; byte < 4; ++byte) {
        hash ^= (uint32_t(value) >> (byte * 8)) & 255; hash *= 1099511628211ull;
    }
}
static bool profilingPreservesSearch() {
    using namespace tak::sim;
    std::array<PathService,2> services;
    std::array<std::vector<int>,2> events;
    services[0].setProfiling(true);
    for (size_t i=0;i<services.size();++i) {
        auto& service=services[i];
        service.setBudget(137);
        service.setGradeHost({[&,i](int id,bool last) {
            events[i].insert(events[i].end(),{1,id,int(last)});
        },{},[&,i](int id) { events[i].insert(events[i].end(),{2,id}); }});
        for (int id:{1,2}) service.request(id,{2,2},{27,19},32,24,
            Fixed::fromInt(432),Fixed::fromInt(304),0,false,0,49152,{}, {},0,uint64_t(id));
    }
    for (uint32_t tick=1;tick<10000;++tick) {
        for (size_t i=0;i<services.size();++i) {
            services[i].tick([&,i](int id,int x,int z) {
                events[i].insert(events[i].end(),{3,id,x,z});
                return (z==2 && x>=2 && x<=27) || (x==27 && z>=2 && z<=19) ? 6 : 0;
            },[&,i](int id,const std::vector<PathCell>& route,Fixed x,Fixed z,
                    bool failed,bool crowded,bool traffic,bool detour) {
                events[i].insert(events[i].end(),{4,id,x.v,z.v,int(failed),int(crowded),int(traffic),int(detour),int(route.size())});
                for (auto p:route) events[i].insert(events[i].end(),{p.x,p.z});
            },{},tick);
            for (auto e:services[i].takeNotifications())
                events[i].insert(events[i].end(),{5,e.unitId,int(e.controller),e.events});
        }
        if (events[0]!=events[1] || services[0].pendingCount()!=services[1].pendingCount() ||
            services[0].workSpent()!=services[1].workSpent() || services[0].requests()!=services[1].requests() ||
            services[0].completions()!=services[1].completions() || services[0].failures()!=services[1].failures()) return false;
        if (!services[0].pendingCount()) {
            const auto profile=services[0].diagnostics(),disabled=services[1].diagnostics();
            return profile.initializations>0 && profile.executionSlices>0 && profile.resetCells>=32*24 &&
                profile.scratchBytes>0 && profile.initializationNanoseconds+profile.preparationNanoseconds+
                    profile.executionNanoseconds>0 && disabled.initializations==0 && disabled.executionSlices==0 &&
                disabled.initializationNanoseconds==0 && disabled.preparationNanoseconds==0 && disabled.executionNanoseconds==0;
        }
    }
    return false;
}
static int selfTest() {
    using namespace tak::sim;
    int failures = 0;
    auto check = [&](bool condition, const char* label) {
        if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
    };
    check(profilingPreservesSearch(),"opt-in stage timings preserve every query, delivery tick, notification and work charge");
    // 0x4f6379: every player is in retail's 5x path-budget class unless a
    // campaign mission narrows it; only Retail mode applies the classes.
    for (int player:{0,3})
        check(RetailReplayProbe::budgetClass(PathfindingMode::Retail,false,player),"retail player in 5x budget class");
    check(RetailReplayProbe::budgetClass(PathfindingMode::Retail,true,0),"campaign human keeps 5x budget class");
    check(!RetailReplayProbe::budgetClass(PathfindingMode::Retail,true,3),"non-strategic campaign player has 1x budget class");
    check(RetailReplayProbe::gateGrade(true,false,false,3,2,1,1)==3,"closed gate c passage has special grade");
    check(RetailReplayProbe::gateGrade(true,false,false,4,2,1,1)==3,"closed gate C passage has special grade");
    check(RetailReplayProbe::gateGrade(true,false,false,2,2,1,1)==0,"gate frame remains blocked");
    check(RetailReplayProbe::gateGrade(false,false,false,3,2,1,1)==0,"ordinary closed yard is not a gate");
    check(RetailReplayProbe::gateGrade(true,true,false,3,2,1,1)==6,"open gate passage is ordinary terrain");
    check(RetailReplayProbe::gateGrade(true,false,true,3,2,1,1)==0,"gate passage does not bypass forbidden water depth");
    check(RetailReplayProbe::automaticGate(false,true,true,true,true,1,-1,0)==1,"moving owner opens adjacent gate");
    check(RetailReplayProbe::automaticGate(false,false,true,true,true,1,-1,0)==-1,"enemy cannot open gate");
    check(RetailReplayProbe::automaticGate(true,true,true,false,true,1,0,0)==-1,"stopped passage occupant holds gate");
    check(RetailReplayProbe::automaticGate(true,true,true,false,true,1,-1,0)==0,"stopped outside body permits closing");
    check(RetailReplayProbe::gateOwnerGrade(1,1,0,true,0,false,false,0,0)==4,"capable owner receives traversable gate grade");
    check(RetailReplayProbe::gateOwnerGrade(1,1,0,true,1,false,false,0,0)==3,"other owner retains special gate grade");
    check(RetailReplayProbe::gateOwnerGrade(1,1,0,false,0,false,false,0,0)==3,"manual gate capability does not bypass passage");
    check(RetailReplayProbe::completedPointDetaches(false),"arrival detaches controller but advances stored points without re-requesting");
    check(RetailReplayProbe::completedPointDetaches(true),"arrival preserves a very recent admission timestamp");
    check(RetailReplayProbe::productionPoolSlots(),
          "production pools: 5121d8 ranked CRT slot per owner pool, capacity maxunits, capture reallocates");
    check(RetailReplayProbe::productionPoolCapacity(),
          "production pools: start at retail's maxunits for the cap (20..500, 250 uncapped), grow only when full");
    check(RetailReplayProbe::productionPoolReuse(),
          "production pools: retirement frees a record for reuse, a dying unit keeps it");
    check(RetailReplayProbe::productionPoolAdmission(false),
          "production pools: 416430 admits in slot order and pays for every scanned hole");
    check(RetailReplayProbe::productionPoolAdmission(true),
          "production pools: the slot cursor wraps at the pool capacity and stamps the wrap tick");
    for (int drift : {0,1,2,9})
        check(RetailReplayProbe::pendingRetryAfterDrift(drift,false)==1,
              ("retry ladder leaves a pending search alone after "+std::to_string(drift)+"-cell drift").c_str());
    check(RetailReplayProbe::pendingRetryAfterDrift(2,true)==2,
          "a changed destination still restarts the pending search");
    for (bool reachable : {false,true})
        check(RetailReplayProbe::rectangleRetryAndFailure(reachable),
              "partial build route retries its perimeter and failure respects builder reach");
    for (int shape : {1,2}) for (bool recent : {false,true})
        check(RetailReplayProbe::completedPointDetaches(recent,shape),
              "ring/rectangle arrival detaches, consumes stored points and suppresses retries");
    for (bool detached:{false,true}) for (bool recent:{false,true})
        check(RetailReplayProbe::retiredPointKeepsDetachedStamp(detached,recent),
            "retiring a move only expires its stamp while detaching a live controller");
    check(RetailReplayProbe::constructionSearchOccupancy(false),"completed stationary body ages cached occupancy");
    check(RetailReplayProbe::constructionSearchOccupancy(true),"unfinished stationary body ages cached occupancy");
    check(RetailReplayProbe::constructionSearchOccupancy(true,1),"unfinished landed flyer ages cached occupancy");
    check(RetailReplayProbe::constructionSearchOccupancy(false,1),"landed flyer ages cached occupancy");
    check(RetailReplayProbe::constructionSearchOccupancy(false,2),"airborne flyer does not age ground occupancy");
    check(RetailReplayProbe::sharedSearchGradeSnapshot(),"scoped body snapshots preserve grade bytes and hashes across requests and same-tick changes");
    for (bool boat : {false,true}) {
        for (int foot : {2,3,5}) for (int delta : {-1,0,1})
            check(RetailReplayProbe::preciseSearchDestination(boat,foot,delta),
                  "search preserves raw mission destination across float-rounded cell boundaries");
        const auto reset=[&](int endpoint,uint32_t flags,bool active) {
            const std::vector<RetailSteeringPoint> points{
                {Fixed::fromInt(80),Fixed::fromInt(96)},
                {Fixed::fromInt(136),Fixed::fromInt(104)},
                {Fixed::fromInt(endpoint),Fixed::fromInt(104)}};
            return RetailReplayProbe::resetRoute(1,1,boat,flags,active,104*65536,104*65536,
                                                426*65536+16384,106*65536+32768,points);
        };
        const std::vector<int> direct{1,2,104,104,424,104};
        check(reset(168,0,true)==direct,"distant old route is replaced without losing the queued command");
        check(reset(264,0,true)==direct,"exact half-distance does not retain the old route");
        check(reset(328,0,false)==std::vector<int>{1,3,80,96,136,104,328,104},
              "nearby old endpoint reactivates the preserved route");
        check(reset(168,0x400000,true)==std::vector<int>{0,3,80,96,136,104,168,104},
              "mission retention flag preserves points but disables navigation");
        check(reset(168,0x10000000,false)==direct,"forced direct segment requires an inactive old navigator");
        check(reset(168,0x10000000,true)==std::vector<int>{0,3,80,96,136,104,168,104},
              "active old navigator does not take the forced-direct exception");
    }
    check(RetailReplayProbe::sameCellRequest(PathfindingMode::Retail),
          "Retail submits a same-cell goal; it completes at once with 0x1000 and no points");
    {
        uint32_t retail=0;
        check(RetailReplayProbe::sameCellOrder(PathfindingMode::Retail,retail),
              "Retail same-cell order: arrival cancels the submission and the order retires");
    }
    check(RetailReplayProbe::emptyDelivery(false),
          "empty route disables navigation and notifies failure without discarding the mission queue");
    check(RetailReplayProbe::emptyDelivery(true),
          "empty route suppresses failure for an accepted goal and a later route reactivates navigation");
    check(RetailReplayProbe::grade(2,6,7,8,25,2,2,5,11,128)==3,
          "World search uses the requesting owner's explored bit");
    check(RetailReplayProbe::grade(2,6,7,8,25,2,2,5,11,1)==5,
          "another player's exploration cannot reveal a World search cell");
    check(RetailReplayProbe::grade(2,6,7,8,25,2,8,5,11,0)==3,
          "World search preserves the hidden start-column exemption");
    check(RetailReplayProbe::grade(2,6,7,8,25,2,-1,5,11,128)==0,
          "World search rejects out-of-map cells even when explored");
    {
        RetailGradeContext c{64,64,2,2,20,20,3,0,true};
        int liveCalls=0, cacheCalls=0;
        auto unseen=[](int,int) { return false; };
        auto cached=[&](int,int) { ++cacheCalls; return 10; };
        auto live=[&](int,int) { ++liveCalls; return 6; };
        auto noOwner=[](int,int) { return -1; };
        check(retailSearchGrade(c,40,40,unseen,cached,live,noOwner)==5 && cacheCalls==0,
              "unseen diagonal bypasses both cached and live grades");
        check(retailSearchGrade(c,20,40,unseen,cached,live,noOwner)==2 && liveCalls==0,
              "unseen start-column exemption and live-query margin are crosses");
        auto seen=[](int,int) { return true; };
        check(retailSearchGrade(c,40,40,seen,cached,live,noOwner)==6 && liveCalls==1,
              "distant cached traffic uses live grade on early retry");
        c.retry=2;
        check(retailSearchGrade(c,40,40,seen,cached,live,noOwner)==2 && liveCalls==1,
              "late retry retains cached grade and masks its independent tag bit");
        c.retry=0;
        std::vector<std::pair<int,int>> probes;
        auto owner=[&](int x,int z) {
            probes.emplace_back(x,z); return x==41 && z==40 ? 3 : -1;
        };
        check(retailSearchGrade(c,40,40,seen,[](int,int) { return 3; },live,owner)==4
              && probes==std::vector<std::pair<int,int>>{{40,40},{41,41},{41,40}},
              "friendly special blocker follows diagonal and edge probe order");
        c.footX=c.footZ=4;
        std::pair<int,int> queried{-1,-1};
        check(retailSearchVisible(c,64,64,7,9,[&](int x,int z) {
            queried={x,z}; return uint16_t(8);
        }) && queried==std::pair<int,int>{4,5}, "coarse visibility includes footprint offset");
        c.width=c.height=8; c.footX=c.footZ=2;
        std::vector<int> cells(64,15);
        retailAgeGradeRect(c,0,0,2,2,false,
            [&](int x,int z) { return cells.at(size_t(z*8+x)); },
            [&](int x,int z,int grade) { cells.at(size_t(z*8+x))=8|grade; });
        check(cells[0]==10 && cells[2]==12 && cells[18]==12 && cells[3]==15,
              "clipped occupancy interior overlaps clipped border and retains tags");
        RetailGradePreparation state{80,0,0};
        std::vector<RetailGradeBody> bodies{{1,0x1000001,50,true},
                                          {2,0x1000001,80,true},
                                          {3,0x1000001,90,true}};
        std::vector<int> events;
        auto refresh=[&](const RetailGradeBody& b) { events.push_back(b.slot); };
        auto age=[&](const RetailGradeBody& b,bool stale) { events.push_back(b.slot+(stale?20:10)); };
        state.prepare(100,bodies[0],bodies,false,refresh,age);
        check(events==std::vector<int>{1,12} && state.recent==90 && state.requestSlot==1,
              "preparation ages half-open timestamp intervals and excludes requester");
        state.finish(&bodies[0],age);
        check(events==std::vector<int>{1,12,11} && state.requestSlot==0,
              "search cleanup restores requester occupancy");
        events.clear(); state.prepare(100,bodies[0],bodies,true,refresh,age);
        check(events==std::vector<int>{1,1,2,3}, "last retry refreshes requester again in slot scan");
    }
    {
        uint32_t seed=1;
        check(retailAllocateSlot(seed,4,4,[](int) { return true; })==-1 && seed==1,
              "full entity pool consumes no CRT draw");
        check(retailAllocateSlot(seed,4,2,[](int i) { return i==0 || i==2; })==1
              && seed==2745024, "allocation ranks free slots using the independent CRT stream");
        check(retailDamageWithSpread(476.0f,0)==405 &&
              retailDamageWithSpread(476.0f,16384)==476 &&
              retailDamageWithSpread(476.0f,32767)==546 &&
              retailDamageWithSpread(6.0f,0)==6,
              "native per-hit CRT spread has retail endpoints and truncation");
        RetailRectGoal rect{2,6,3,7};
        check(!rect.accepts(4,5) && rect.distance(4,5)==32 && rect.accepts(2,5),
              "rectangle interior is not an accepted perimeter goal");
        RetailRingGoal ring{0,0,80,120,100};
        check(ring.accepts(3,4) && !ring.accepts(0,4) && ring.distance(0,4)==16,
              "ring inner radius uses world distance, including exact boundary");
        std::vector<std::pair<int,int>> points;
        RetailRingGoal{}.enumerate([&](int x,int z) { points.emplace_back(x,z); });
        check(points.size()==16 && std::all_of(points.begin(),points.end(),[](auto p) {
            return p==std::pair<int,int>{0,0};
        }), "ring sampling retains all repeated goals");
    }
    for (int budget : {1, 37, 12000}) {
        RetailSearchAttempt a;
        a.trace.width = 40; a.trace.height = 32;
        a.trace.start = {8, 8}; a.trace.goal = {30, 8};
        a.trace.trafficRadius = 8; a.trace.cells.resize(40 * 32);
        a.trace.cells[8 * 40 + 30].flags = 4;
        a.initialDistance = 22 * 18;
        int spent = 0, result = 0;
        for (int n = 0; n < 1000 && result == 0; ++n) {
            auto slice = a.step([](int x, int z, int) {
                if (x < 0 || z < 0 || x >= 40 || z >= 32) return 0;
                return z == 8 && (x == 9 || x == 10) ? 5 : 6;
            }, [](int x, int z) { return retailGoalDistance(x - 30, z - 8); }, budget);
            spent += slice.work; result = int(slice.result);
        }
        check(result == 1 && spent == 206, "direct route work is independent of suspension");
        check(a.cost.cells.empty(), "direct arrival never seeds the cost search");
        check(a.route.points == std::vector<RetailReachability::Point>{{8,8},{30,8}}, "direct route endpoints");
        check(a.trace.nearTraffic && a.trace.otherTraffic && a.route.flags == 1,
              "direct route gives nearby traffic precedence");
    }
    for (int budget : {1, 37, 12000}) {
        RetailSearchAttempt a;
        a.trace.width = 16; a.trace.height = 12;
        a.trace.start = {1,1}; a.trace.goal = {12,8};
        a.trace.cells.resize(16*12); a.trace.cells[8*16+12].flags = 4;
        a.initialDistance = retailGoalDistance(11,7); a.retry = 3; a.heading = 0;
        auto grade = [](int x, int z, int) {
            return (z == 1 && x >= 1 && x <= 12) || (x == 12 && z >= 1 && z <= 8) ? 6 : 0;
        };
        auto distance = [](int x, int z) { return retailGoalDistance(x-12,z-8); };
        int result = 0, headingReads=0; bool seeded = false;
        for (int n = 0; n < 1000 && result == 0; ++n) {
            const auto slice = a.step(grade,distance,budget,[&] { ++headingReads;return 49152; });
            result = int(slice.result);
            if (!seeded && a.phase == 2) {
                seeded = true;
                check(a.cost.processed == 0 && a.spread == 4 && a.nodeLimit == 384,
                      "handoff seeds an unprocessed cost search with retry limit");
                check(a.cost.cells[17].direction == 6 && (a.cost.cells[17].flags & 0x21) == 0x21,
                      "handoff retains trace flags and replaces start heading");
            }
        }
        check(seeded && result == 1 && a.route.flags == 0, "cost search completes the forced corridor");
        check(headingReads==1,"cost handoff samples the changed heading exactly once");
        check(a.route.points == std::vector<RetailReachability::Point>{{1,1},{11,1},{12,2},{12,8}},
              "cost arrival matches retail's diagonal corridor corner");
    }
    std::vector<RetailCostSearch::Cell> cells(160 * 16);
    std::vector<RetailReachability::Point> path{{2,8}};
    for (int x = 3; x < 153; ++x) {
        int z = path.back().z;
        cells[size_t(z*160+x)].direction = 6; path.push_back({x,z});
        int nextZ = z == 8 ? 9 : 8;
        cells[size_t(nextZ*160+x)].direction = uint8_t(z == 8 ? 4 : 0);
        cells[size_t(nextZ*160+x)].flags = 64; path.push_back({x,nextZ});
    }
    const auto route = retailReconstructRoute(cells,160,path.front(),path.back(),19,8);
    check(route.flags == 12, "detour retains partial flag and clears traffic flags");
    check(route.points.size() == 64 && route.points.front() == path.front()
          && route.points.back() != path.back(), "corner ring keeps the start-side 64 points");
    cells[8*160+3].direction = 2;
    cells[8*160+4].direction = 6;
    bool rejected = false;
    try { (void)retailReconstructRoute(cells,160,{2,8},{3,8},0,8); }
    catch (const std::logic_error&) { rejected = true; }
    check(rejected, "malformed cyclic chain is rejected");
    uint32_t stamp = 0;
    check(retailInitialPathWeight(100,stamp,20,0,false) == 98304 && stamp == 100,
          "initial age starts at the current tick");
    stamp = 1;
    check(retailInitialPathWeight(1000,stamp,500,3,true) == 20*65536,
          "age/load heuristic obeys retail upper bound");
    {
        RetailSearchAttempt a;
        a.trace.cells.resize(64); a.trace.cells[3].flags=0xff; a.trace.cells[3].direction=7;
        RetailSearchAttempt::Parameters p; p.width=8; p.height=8; p.start={3,3};
        std::vector<int> events;
        auto result=a.initialize(p,[&](bool) { events.push_back(1); },
            [&] { events.push_back(2); return std::vector<RetailReachability::Point>{{5,3},{3,5}}; },
            [&](int,int) { events.push_back(3); return true; },
            [&](int,int) { events.push_back(4); return 99; });
        check(result.result==RetailSearchAttempt::Result::Complete && result.work==500
              && result.notification==0x1000 && a.route.points.empty(),
              "controller acceptance completes initialization without a route");
        check(events==std::vector<int>{1,2,3}, "acceptance precedes distance evaluation");
        check(a.trace.goal==RetailReachability::Point{5,3}, "equidistant goals preserve controller order");
        check(a.trace.cells[3].flags==0 && a.trace.cells[3].direction==7,
              "initialization clears flags while retaining scratch directions");
    }
    {
        RetailSearchWorker worker;
        std::vector<int> retries; std::vector<bool> refreshes;
        int acceptanceQueries=0; bool complete=false;
        for (int n=0;n<100 && !complete;++n) {
            auto result=worker.dispatch(n==0,12000,[&](int retry) {
                retries.push_back(retry);
                RetailSearchAttempt::Parameters p;
                p.width=16; p.height=12; p.start={1,1}; p.heading=49152;
                return p;
            },[&](bool last) { refreshes.push_back(last); },
            [] { return std::vector<RetailReachability::Point>{{12,8}}; },
            [&](int x,int z) { ++acceptanceQueries; return x==12 && z==8; },
            [](int x,int z) { return retailGoalDistance(x-12,z-8); },
            [](int x,int z,int) {
                return (z==1 && x>=1 && x<=12) || (x==12 && z>=1 && z<=8) ? 6 : 0;
            });
            if (worker.attempt.phase==2) worker.attempt.nodeLimit=0;
            complete=result.result==RetailSearchAttempt::Result::Complete;
        }
        check(complete && retries==std::vector<int>{0,1,2,3}, "exhaustion performs exactly three retries");
        check(refreshes==std::vector<bool>{false,false,false,true}, "last retry selects the final grid refresh mode");
        check(acceptanceQueries==85 && worker.attempt.route.points.empty(),
              "final failure audits 81 controller cells and clears the route");
    }
    if (!failures) std::cout << "PASS: retail trace, handoff, route ring and initial weight regressions\n";
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc==2 && std::string(argv[1])=="--gate-owner") {
        int fx,fz,retry,capable,owner,opened,frame,dx,dz;
        while (std::cin>>fx>>fz>>retry>>capable>>owner>>opened>>frame>>dx>>dz)
            std::cout<<tak::sim::RetailReplayProbe::gateOwnerGrade(fx,fz,retry,capable,owner,opened,frame,dx,dz)<<'\n';
        return 0;
    }
    if (argc==2 && std::string(argv[1])=="--automatic-gate") {
        int active,owner,mover,moving,allocated,size,dx,dz;
        while (std::cin>>active>>owner>>mover>>moving>>allocated>>size>>dx>>dz)
            std::cout<<tak::sim::RetailReplayProbe::automaticGate(active,owner,mover,moving,allocated,size,dx,dz)<<'\n';
        return 0;
    }
    if (argc==2 && std::string(argv[1])=="--gate-grade") {
        int gate,open,flooded,x,z,width,height;
        while (std::cin>>gate>>open>>flooded>>x>>z>>width>>height)
            std::cout<<tak::sim::RetailReplayProbe::gateGrade(gate,open,flooded,x,z,width,height)<<'\n';
        return 0;
    }
    if (argc==2 && std::string(argv[1])=="--world-reset") {
        int fx,fz,boat,active,count;uint32_t flags;int32_t ux,uz,gx,gz;
        while (std::cin>>fx>>fz>>boat>>flags>>active>>ux>>uz>>gx>>gz>>count) {
            if (count<2 || count>64) return 2;
            std::vector<tak::sim::RetailSteeringPoint> points(count);
            for (auto& point:points) {
                int x,z;if (!(std::cin>>x>>z)) return 2;
                point={tak::sim::Fixed::fromInt(x),tak::sim::Fixed::fromInt(z)};
            }
            const auto result=tak::sim::RetailReplayProbe::resetRoute(fx,fz,boat!=0,flags,active!=0,ux,uz,gx,gz,points);
            for (size_t i=0;i<result.size();++i) std::cout<<(i?" ":"")<<result[i];
            std::cout<<'\n';
        }
        return 0;
    }
    if (argc==2 && std::string(argv[1])=="--world-destination") {
        using namespace tak::sim;
        int fx,fz,boat,x,z,blocked,reset;
        while (std::cin>>fx>>fz>>boat>>x>>z>>blocked>>reset) {
            World world;world.setPathService(true);
            world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,boat?32:0);
            UnitType type;type.footX=fx;type.footZ=fz;type.maxVel=Fixed::fromInt(1);
            type.floater=boat;type.minWaterDepth=boat?13:-10000;type.maxWaterDepth=boat?10000:20;
            type.domain=boat?UnitType::Domain::Water:UnitType::Domain::Ground;
            const int id=world.spawn(&type,6*16+fx*8,6*16+fz*8);
            if (blocked) world.blockCells(12,12,12,12,true);
            world.order(id,Fixed::raw(x).toFloat(),Fixed::raw(z).toFloat(),false);
            if (reset) RetailReplayProbe::resetDestination(world,*world.unit(id));
            const auto& goal=world.unit(id)->orders.back();
            const auto point=goal.missionTarget.value_or(std::pair{goal.x,goal.z});
            std::cout<<point.first.v<<' '<<point.second.v<<' '
                     <<footprintOrigin(point.first,fx)<<' '<<footprintOrigin(point.second,fz)<<' '
                     <<goal.segmentX.v<<' '<<goal.segmentZ.v<<' '<<goal.x.v<<' '<<goal.z.v<<'\n';
        }
        return 0;
    }
    if (argc==2 && std::string(argv[1])=="--world-search") {
        int fx,fz,boat,budget,gx,gz,exploration,admission,moving,terrain;
        while (std::cin>>fx>>fz>>boat>>budget>>gx>>gz>>exploration>>admission>>moving>>terrain) {
            if (exploration< -1 || exploration>2 || (terrain && exploration<0)) return 2;
            std::vector<int> grades(32*32);for (auto& grade:grades) if (!(std::cin>>grade)) return 2;
            tak::sim::RetailReplayProbe::search(fx,fz,boat!=0,budget,gx,gz,exploration,admission!=0,moving!=0,terrain!=0,grades);
        }
        return 0;
    }
    if (argc==2 && std::string(argv[1])=="--world-search2") {
        int fx,fz,boat,budget,gx,gz,exploration,admission,moving,terrain;
        tak::sim::RetailReplayProbe::SearchVariant v;
        while (std::cin>>fx>>fz>>boat>>budget>>gx>>gz>>exploration>>admission>>moving>>terrain
                       >>v.sx>>v.sz>>v.heading>>v.turn>>v.road>>v.water) {
            if (exploration< -1 || exploration>2 || (terrain && exploration<0)) return 2;
            std::vector<int> grades(32*32);for (auto& grade:grades) if (!(std::cin>>grade)) return 2;
            tak::sim::RetailReplayProbe::search(fx,fz,boat!=0,budget,gx,gz,exploration,admission!=0,moving!=0,terrain!=0,grades,v);
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--cached-footprint") {
        int x,z,fx,fz;
        while (std::cin >> x >> z >> fx >> fz) {
            std::array<int,5> grades;
            for (auto& grade:grades) std::cin >> grade;
            int n=0;
            std::vector<int> queries;
            const int result=tak::sim::retailCachedFootprintGrade(x,z,fx,fz,[&](int qx,int qz,int w,int h) {
                queries.insert(queries.end(),{qx,qz,w,h}); return grades.at(size_t(n++));
            });
            std::cout << result;
            for (int v:queries) std::cout << ' ' << v;
            std::cout << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--cached-body") {
        bool mover,requester;
        uint32_t stamp,recent,stale;
        while (std::cin >> mover >> requester >> stamp >> recent >> stale)
            std::cout << tak::sim::retailCachedBodyGrade(mover,requester,stamp,recent,stale) << '\n';
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--ring-point") {
        tak::sim::RetailRingGoal goal;int32_t x,z;int fx,fz;
        while (std::cin>>goal.x>>goal.z>>goal.innerRadius>>goal.outerTolerance>>x>>z>>fx>>fz) {
            const auto point=goal.navigationPoint(tak::sim::Fixed::raw(x),tak::sim::Fixed::raw(z),fx,fz);
            std::cout<<point.x.v<<' '<<point.z.v<<'\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--rect-point") {
        tak::sim::RetailRectGoal goal;
        int x,z,fx,fz;
        while (std::cin >> goal.minX >> goal.maxX >> goal.minZ >> goal.maxZ >> x >> z >> fx >> fz) {
            const auto [cx,cz]=goal.navigationCell(x,z);
            std::cout << (cx*16+fx*8)*65536 << ' ' << (cz*16+fz*8)*65536 << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--circle-radius") {
        int32_t radius;
        while (std::cin >> radius) std::cout << tak::sim::retailCircleRadiusSquared(radius) << '\n';
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--prepare-grade") {
        using namespace tak::sim;
        RetailGradePreparation state;
        uint32_t tick;
        int requester,last,count,finish;
        while (std::cin >> state.recent >> state.stale >> tick >> requester >> last >> count >> finish) {
            std::vector<RetailGradeBody> bodies(count);
            for (int i=0;i<count;++i) {
                auto& b=bodies[i]; b.slot=i+1;
                std::cin >> b.flags >> b.stamp >> b.hasMover;
            }
            std::vector<int> events;
            auto age=[&](const RetailGradeBody& b,bool stale) {
                events.insert(events.end(),{2,b.slot,int(stale)});
            };
            state.prepare(tick,bodies.at(size_t(requester-1)),bodies,last!=0,
                [&](const RetailGradeBody& b) { events.insert(events.end(),{1,b.slot}); },age);
            if (finish) state.finish(&bodies.at(size_t(requester-1)),age);
            std::cout << state.recent << ' ' << state.stale << ' ' << state.requestSlot;
            for (int value:events) std::cout << ' ' << value;
            std::cout << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--age-grade") {
        using namespace tak::sim;
        RetailGradeContext c;
        int x,z,fx,fz,stale;
        while (std::cin >> c.width >> c.height >> c.footX >> c.footZ >> x >> z >> fx >> fz >> stale) {
            std::vector<int> cells(size_t(c.width*c.height));
            for (auto& v:cells) std::cin >> v;
            uint64_t writes=14695981039346656037ull;
            retailAgeGradeRect(c,x,z,fx,fz,stale!=0,
                [&](int qx,int qz) { return cells.at(size_t(qz*c.width+qx)); },
                [&](int qx,int qz,int value) {
                    auto& cell=cells.at(size_t(qz*c.width+qx)); cell=(cell&8)|value;
                    word(writes,qx); word(writes,qz); word(writes,value);
                });
            uint64_t result=14695981039346656037ull;
            for (int value:cells) word(result,value);
            std::cout << result << ' ' << writes << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--terrain-grade") {
        int low,high,sea,maxDepth,minDepth,badMax,badMin,maxSlope,badSlope,maxWater,badWater,road;
        while (std::cin>>low>>high>>sea>>maxDepth>>minDepth>>badMax>>badMin>>maxSlope>>badSlope>>maxWater>>badWater>>road)
            std::cout<<tak::sim::retailTerrainGrade(low,high,sea,maxDepth,minDepth,badMax,badMin,
                maxSlope,badSlope,maxWater,badWater,road!=0)<<'\n';
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--visible") {
        using namespace tak::sim;
        RetailGradeContext c;
        int mw,mh,x,z,mask;
        while (std::cin >> c.width >> c.height >> c.footX >> c.footZ >> mw >> mh >> c.player >> x >> z >> mask) {
            std::vector<int> queries;
            const bool value=retailSearchVisible(c,mw,mh,x,z,[&](int qx,int qz) {
                queries.insert(queries.end(),{qx,qz}); return uint16_t(mask);
            });
            std::cout << value;
            for (int v : queries) std::cout << ' ' << v;
            std::cout << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--delivery") {
        int fx,fz,accepted,inactive,flags,count;
        while (std::cin>>fx>>fz>>accepted>>inactive>>flags>>count) {
            if (count<0 || count>256) return 2;
            std::vector<tak::sim::PathCell> route(static_cast<size_t>(count));
            for (auto& p:route) if (!(std::cin>>p.x>>p.z)) return 2;
            tak::sim::RetailReplayProbe::delivery(fx,fz,accepted,inactive,flags,route);
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--world-grade") {
        int fx,fz,player,sx,sz,retry,x,z,grade,mask;
        while (std::cin>>fx>>fz>>player>>sx>>sz>>retry>>x>>z>>grade>>mask)
            std::cout<<tak::sim::RetailReplayProbe::grade(fx,fz,player,sx,sz,retry,x,z,grade,mask)<<'\n';
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--grade") {
        using namespace tak::sim;
        RetailGradeContext c;
        int probe,x,z,visible,grade,live,bx,bz,owner;
        while (std::cin >> c.width >> c.height >> c.footX >> c.footZ >> c.startX >> c.startZ
               >> c.player >> c.retry >> probe >> x >> z >> visible >> grade >> live >> bx >> bz >> owner) {
            c.specialBlockerProbe=probe!=0;
            std::vector<int> queries;
            const int value=retailSearchGrade(c,x,z,[&](int,int) { return visible!=0; },
                [&](int,int) { return grade; },[&](int qx,int qz) {
                    queries.insert(queries.end(),{1,qx,qz}); return live;
                },[&](int qx,int qz) {
                    queries.insert(queries.end(),{2,qx,qz}); return qx==bx && qz==bz ? owner : -1;
                });
            std::cout << value;
            for (int v : queries) std::cout << ' ' << v;
            std::cout << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--goal") {
        using namespace tak::sim;
        int shape,a,b,c,d,e,x,z;
        while (std::cin >> shape >> a >> b >> c >> d >> e >> x >> z) {
            bool accepted; int distance;
            std::vector<std::pair<int,int>> points;
            const auto emit=[&](int px,int pz) { points.emplace_back(px,pz); };
            if (shape==0) {
                RetailCircleGoal g{a,b,c,d}; accepted=g.accepts(x,z); distance=g.distance(x,z);
                g.enumerate(emit);
            } else if (shape==1) {
                RetailRectGoal g{a,b,c,d}; accepted=g.accepts(x,z); distance=g.distance(x,z);
                g.enumerate(emit);
            } else {
                RetailRingGoal g{a,b,c,d,e}; accepted=g.accepts(x,z); distance=g.distance(x,z);
                g.enumerate(emit);
            }
            std::cout << accepted << ' ' << distance;
            for (auto [px,pz] : points) std::cout << ' ' << px << ' ' << pz;
            std::cout << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--slot") {
        uint32_t seed; int capacity,allocated;
        while (std::cin >> seed >> capacity >> allocated) {
            std::vector<int> flags(size_t(capacity),0);
            for (auto& f : flags) std::cin >> f;
            const int slot=tak::sim::retailAllocateSlot(seed,capacity,allocated,[&](int i) { return (flags[size_t(i)] & 0x1000000)!=0; });
            std::cout << slot << ' ' << seed << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--pool-capacity") {
        // The pool World starts a player with under each unit cap.
        int cap;
        while (std::cin >> cap) std::cout << tak::sim::World::entityPoolCapacity(cap) << '\n';
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--worker") {
        using namespace tak::sim;
        int width,height,sx,sz,gx,gz,budget,exhaust;
        while (std::cin >> width >> height >> sx >> sz >> gx >> gz >> budget >> exhaust) {
            std::vector<int> grades(size_t(width)*height);
            for (auto& g : grades) std::cin >> g;
            RetailSearchScheduler scheduler; RetailSearchWorker worker;
            std::array<RetailSearchScheduler::Player,10> players{};
            players[0] = {true,false,1,4};
            bool pending=true; uint32_t wrap=0;
            for (int tick=1; tick<=5000 && pending; ++tick) {
                uint64_t queries=14695981039346656037ull,events=queries,cells=queries;
                scheduler.tick(players,budget,1,tick,[&](auto) { return pending; },
                    [&](auto r) { return pending && r.player==0 && r.slot==1; },
                    [&](auto, bool admitted,int remaining) {
                        auto slice=worker.dispatch(admitted,remaining,[&](int retry) {
                            RetailSearchAttempt::Parameters p;
                            p.width=width; p.height=height; p.start={sx,sz}; p.trafficRadius=8;
                            wrap=uint32_t(scheduler.lastWrapTick[0]);
                            p.weight=retailInitialPathWeight(uint32_t(tick),wrap,1,retry,false);
                            scheduler.lastWrapTick[0]=int(wrap);
                            return p;
                        },[&](bool last) { word(events,2); word(events,last); },
                        [&] { return std::vector<RetailReachability::Point>{{gx,gz}}; },
                        [&](int x,int z) { return x==gx && z==gz; },
                        [&](int x,int z) { return retailGoalDistance(x-gx,z-gz); },
                        [&](int x,int z,int d) {
                            word(queries,x); word(queries,z); word(queries,d);
                            return x<0 || z<0 || x>=width || z>=height ? 0 : grades[size_t(z*width+x)];
                        });
                        if (exhaust && worker.attempt.phase==2 && worker.attempt.cost.processed==0)
                            worker.attempt.nodeLimit=0;
                        if (slice.notification) { word(events,1); word(events,slice.notification); }
                        const bool complete=slice.result==RetailSearchAttempt::Result::Complete;
                        if (complete) {
                            word(events,3); word(events,int(worker.attempt.route.points.size())); word(events,4);
                            pending=false;
                        }
                        return RetailSearchScheduler::Slice{slice.work,complete};
                    });
                auto& a=worker.attempt;
                const auto& plane=a.cost.cells.empty() ? a.trace.cells : a.cost.cells;
                // Before initialization retail already owns a zeroed map plane.
                if (plane.empty()) for (int i=0;i<width*height;++i) word(cells,0);
                else for (auto c : plane) word(cells,c.flags | int(c.direction)<<8);
                std::cout << tick << ' ' << scheduler.remaining << ' ' << (scheduler.active.player>=0)
                          << ' ' << worker.retry << ' ' << cells << ' ' << queries << ' ' << events
                          << ' ' << a.route.flags << ' ' << a.route.points.size();
                for (auto p : a.route.points) std::cout << ' ' << p.x*16+8 << ' ' << p.z*16+8;
                std::cout << '\n';
            }
            std::cout << "END\n";
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--init") {
        using namespace tak::sim;
        RetailSearchAttempt::Parameters params;
        int accepted, distanceValue, count, oldX, oldZ;
        while (std::cin >> params.width >> params.height >> params.start.x >> params.start.z
               >> params.retry >> params.weight >> accepted >> distanceValue >> oldX >> oldZ >> count) {
            RetailSearchAttempt a; a.trace.goal = {oldX,oldZ};
            std::vector<RetailReachability::Point> goals(size_t(count), RetailReachability::Point{});
            for (auto& p : goals) std::cin >> p.x >> p.z;
            a.trace.cells.resize(size_t(params.width)*params.height);
            for (auto& cell : a.trace.cells) {
                int f,d; std::cin >> f >> d; cell.flags=uint8_t(f); cell.direction=uint8_t(d);
            }
            uint64_t events=14695981039346656037ull, cells=events;
            auto result=a.initialize(params,[&](bool last) { word(events,1); word(events,last); },
                [&] { word(events,2); return goals; },
                [&](int,int) { word(events,3); return accepted; },
                [&](int,int) { word(events,4); return distanceValue; });
            for (auto c : a.trace.cells) word(cells,c.flags | int(c.direction)<<8);
            std::cout << int(result.result) << ' ' << result.work << ' ' << result.notification
                      << ' ' << a.trace.goal.x << ' ' << a.trace.goal.z << ' ' << a.initialDistance
                      << ' ' << a.weight << ' ' << cells << ' ' << events << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--selftest") return selfTest();
    if (argc == 2 && std::string(argv[1]) == "--weight") {
        uint32_t tick, last; int pending, retry, heavy, threshold;
        while (std::cin >> tick >> last >> pending >> retry >> heavy >> threshold) {
            const int weight = tak::sim::retailInitialPathWeight(tick, last, pending, retry, heavy != 0, threshold);
            std::cout << weight << ' ' << last << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--attempt") {
        tak::sim::RetailSearchAttempt attempt;
        auto& trace = attempt.trace;
        int budget;
        while (std::cin >> trace.width >> trace.height >> trace.start.x >> trace.start.z
               >> trace.goal.x >> trace.goal.z >> attempt.initialDistance >> attempt.heading
               >> attempt.retry >> attempt.weight >> trace.trafficRadius >> budget) {
            for (auto& v : attempt.cost.costs.step) std::cin >> v;
            for (auto& v : attempt.cost.costs.turn) std::cin >> v;
            auto& c = attempt.cost.costs;
            std::cin >> c.ground >> c.road >> c.slope >> c.traffic >> c.shortTurn >> c.minStraight;
            trace.cells.resize(size_t(trace.width) * trace.height);
            std::vector<int> grades(trace.cells.size());
            for (size_t i = 0; i < grades.size(); ++i) {
                int flags, direction; std::cin >> grades[i] >> flags >> direction;
                trace.cells[i].flags = uint8_t(flags); trace.cells[i].direction = uint8_t(direction);
            }
            for (int n = 0; n < 10000; ++n) {
                uint64_t queries = 14695981039346656037ull, hash = queries;
                auto result = attempt.step([&](int x, int z, int direction) {
                    word(queries, x); word(queries, z); word(queries, direction);
                    return x < 0 || z < 0 || x >= trace.width || z >= trace.height ? 0 : grades[size_t(z * trace.width + x)];
                }, [&](int x, int z) {
                    return tak::sim::retailGoalDistance(x - trace.goal.x, z - trace.goal.z);
                }, budget);
                const auto& cells = attempt.cost.cells.empty() ? trace.cells : attempt.cost.cells;
                for (auto cell : cells) word(hash, cell.flags | int(cell.direction) << 8);
                std::cout << int(result.result) << ' ' << result.work << ' ' << result.notification
                          << ' ' << attempt.partialDistance << ' ' << hash << ' ' << queries
                          << ' ' << attempt.route.flags << ' ' << attempt.route.points.size();
                for (auto p : attempt.route.points) std::cout << ' ' << p.x * 16 + 8 << ' ' << p.z * 16 + 8;
                std::cout << '\n';
                if (result.result != tak::sim::RetailSearchAttempt::Result::Searching) break;
            }
            std::cout << "END\n";
            attempt = {};
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--direction") {
        int x, z;
        while (std::cin >> x >> z) std::cout << tak::sim::retailSearchDirection(x, z) << '\n';
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--route") {
        int width, height, partial, radius;
        tak::sim::RetailReachability::Point start, end;
        while (std::cin >> width >> height >> start.x >> start.z >> end.x >> end.z >> partial >> radius) {
            std::vector<tak::sim::RetailCostSearch::Cell> cells(size_t(width) * height);
            for (auto& cell : cells) {
                int flags, direction;
                std::cin >> flags >> direction;
                cell.flags = uint8_t(flags); cell.direction = uint8_t(direction);
            }
            auto route = tak::sim::retailReconstructRoute(cells, width, start, end, partial, radius);
            std::cout << route.flags << ' ' << route.points.size();
            for (auto p : route.points) std::cout << ' ' << p.x * 16 + 8 << ' ' << p.z * 16 + 8;
            std::cout << '\n';
        }
        return 0;
    }
    using tak::sim::RetailReachability;
    RetailReachability trace;
    int budget, limit, tolerance;
    while (std::cin >> trace.width >> trace.height >> trace.start.x >> trace.start.z
           >> trace.goal.x >> trace.goal.z >> trace.trafficRadius >> tolerance >> budget >> limit) {
        trace.cells.resize(size_t(trace.width) * trace.height);
        std::vector<int> grades(trace.cells.size());
        for (size_t i = 0; i < grades.size(); ++i) {
            int flags, direction;
            std::cin >> grades[i] >> flags >> direction;
            trace.cells[i].flags = uint8_t(flags); trace.cells[i].direction = uint8_t(direction);
        }
        for (int n = 0; n < limit; ++n) {
            uint64_t queries = 14695981039346656037ull, cells = queries;
            auto grade = [&](int x, int z, int direction) {
                word(queries, x); word(queries, z); word(queries, direction);
                return x < 0 || z < 0 || x >= trace.width || z >= trace.height ? 0 : grades[size_t(z * trace.width + x)];
            };
            const int result = trace.step(grade, [&](int x, int z) {
                return tak::sim::retailGoalDistance(x - trace.goal.x, z - trace.goal.z, tolerance);
            }, budget);
            for (auto c : trace.cells) word(cells, c.flags | int(c.direction) << 8);
            std::cout << result << ' ' << trace.work;
            for (int value : {trace.phase, trace.best, trace.march.x, trace.march.z, trace.slopes,
                              trace.ground, trace.roads, trace.visited, trace.visitLimit,
                              trace.origin.x, trace.origin.z, trace.a.x, trace.a.z, trace.b.x, trace.b.z,
                              trace.dirA, trace.dirB, trace.started, trace.nearTraffic, trace.otherTraffic,
                              trace.directEnd.x, trace.directEnd.z}) std::cout << ' ' << value;
            std::cout << ' ' << cells << ' ' << queries << '\n';
            if (result != -2) break;
        }
        std::cout << "END\n";
        trace = {};
    }
}
