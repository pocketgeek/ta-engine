// Partial retail-state diagnostic. It deliberately reports incomplete state;
// use tools/re/probe_saved_movement.py to prepare inputs and compare traces.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <cmath>
#include <cstdlib>

namespace tak::sim {
struct RetailReplayProbe {
    static void gateComposedStart(World& world,Unit& u,int32_t base,bool missions=false) {
        world.setPathService(true);
        world.gameRng_=1;world.tickCounter_=0;
        world.editNavigationExploration().assign(32*32,0xffff);
        world.player(0).automaticGates=true;
        world.unitScripts_.erase(u.id);
        gateMoverStart(u,base);
        startSearchMovement(world,u,512*65536,696*65536,503,1,2);
        if (missions) world.order(u.id,512,696,false);
    }
    static void gateParkStart(World& world,Unit& u,int gate) {
        world.cancelPath(u);u.orders.clear();
        Order order;order.goal=order.groundMission=true;order.park.emplace();
        order.park->target=gate;order.park->padding=64;order.mission.flags=0x200u;
        u.orders.push_back(order);
    }
    static void gateRectangleStart(World& world,Unit& u,const UnitType* site) {
        world.cancelPath(u);u.orders.clear();
        u.orders.push_back(world.makeBuildOrder(u,site,Fixed::fromInt(512),Fixed::fromInt(800)));
    }
    static int gateRectangleTick(World& world,int gate,Unit& u,uint32_t tick) {
        world.tickCounter_=tick;world.rebuildOccupancy();world.tickAutomaticGates();
        world.tickUnitScript(*world.unit(gate));
        if (world.prepareBuildApproach(u)) return 1;
        searchMovementTick(world,u,tick);
        return u.orders.empty() ? 2 : 0;
    }
    static void gateMoverStart(Unit& u,int32_t base) {
        u.heading=retailHeadingToPort(0);u.speed=Fixed();u.baseSpeed=Fixed::raw(base);
        u.groundY=Fixed();u.groundPitch=u.groundRoll=u.variationPhase=0;
        u.groundGradeTick=u.groundMoveTick=u.groundScanTick=0;
        u.groundMovementMode=u.groundSpeedMode=0;u.groundTerrainFlags=0;u.bodyBlockStreak=0;
    }
    static void gatePairStart(World& world,Unit& first,Unit& second,int32_t base,bool close) {
        gateComposedStart(world,first,base,true);
        gateMoverStart(second,base);world.unitScripts_.erase(second.id);
        world.paths_.setEntityPool(0,1,3);
        world.order(first.id,512,792,false);world.order(second.id,close?512:768,close?696:344,false);
    }
    static void crowdStart(World& world,const std::vector<int>& ids,int32_t base,int budget,int gx,int gz) {
        world.setPathService(true);world.gameRng_=1;world.tickCounter_=0;
        world.editNavigationExploration().assign(size_t(world.hW_/2)*(world.hH_/2),0xffff);
        world.unitScripts_.clear();
        for (int id:ids) {
            auto& u=*world.unit(id);gateMoverStart(u,base);
            u.groundY=Fixed();
        }
        world.rebuildOccupancy();
        for (int id:ids) {
            auto& u=*world.unit(id);
            startSearchMovement(world,u,gx*65536,gz*65536,budget,1,int(ids.size()),id==ids.front());
            world.order(id,float(gx),float(gz),false);
        }
    }
    // Whole-World composition: the same initial state as crowdStart, but the
    // orders enter through World::order and every tick is a full World::tick,
    // so the comparison covers the real Retail-mode call order rather than a
    // hand-assembled one. Unit scripts are removed on both sides.
    static void crowdWorldStart(World& world,const std::vector<int>& ids,const std::vector<std::vector<std::pair<int,int>>>& goals,
                                int32_t base,int budget) {
        world.setPathService(true);world.gameRng_=1;world.tickCounter_=0;
        world.editNavigationExploration().assign(size_t(world.hW_/2)*(world.hH_/2),0xffff);
        // World::tick reaches scripts through the id index; drop both.
        world.unitScripts_.clear();world.unitScriptById_.clear();world.scriptYardById_.clear();
        // The native harness runs neither the wind update nor player
        // bookkeeping (411a90), both of which draw from the shared stream;
        // each is compared by its own check.
        world.windEnabled_=false;
        for (auto& player:world.players_) player.cacheClock.enabled=false;
        for (int id:ids) { auto& u=*world.unit(id);gateMoverStart(u,base);u.groundY=Fixed(); }
        world.rebuildOccupancy();
        // Production pools: World placed each Hunter in its owner's pool when
        // it spawned (5121d8); the harness builds the native pool the same way.
        world.paths_.clear();world.paths_.restoreTraversal(0,{},{});world.setPathBudget(budget);
        for (size_t i=0;i<ids.size();++i)
            for (size_t leg=0;leg<goals[i].size();++leg)
                world.order(ids[i],float(goals[i][leg].first),float(goals[i][leg].second),leg!=0);
    }
    // Native CRT seed at the first allocation; spawn draws slots from it.
    static void crtSeed(World& world,uint32_t seed) { world.windRng_=seed; }
    static int entityCapacity(const World& world,int player) {
        return int(world.entitySlots_.unitAt[size_t(player)].size());
    }
    static void crowdTick(World& world,const std::vector<int>& ids,uint32_t tick) {
        world.tickCounter_=tick;world.rebuildOccupancy();
        for (int id:ids) {
            auto& u=*world.unit(id);
            world.tickGroundMission(u);searchMovementTick(world,u,tick,false);
        }
        searchWorkerTick(world,tick);
    }
    static void gatePairTick(World& world,int gate,Unit& first,Unit& second,uint32_t tick) {
        world.tickCounter_=tick;world.rebuildOccupancy();world.tickAutomaticGates();
        world.tickUnitScript(*world.unit(gate));
        for (Unit* u:{&first,&second}) {
            world.tickGroundMission(*u);searchMovementTick(world,*u,tick,false);
        }
        searchWorkerTick(world,tick);
    }
    static void gateComposedTick(World& world,int gate,Unit& mover,uint32_t tick,bool missions=false,bool replace=false,int gx=512,int gz=696) {
        world.tickCounter_=tick;
        if (replace) world.order(mover.id,float(gx),float(gz),false);
        world.rebuildOccupancy();world.tickAutomaticGates();
        world.tickUnitScript(*world.unit(gate));
        if (missions) world.tickGroundMission(mover);
        searchMovementTick(world,mover,tick);
    }
    static void composedMissionState(FILE* output,World& world,Unit& u) {
        const auto* goal=world.groundMissionOrder(u);
        if (!goal && !u.orders.empty() && u.orders[World::currentLeg(u.orders)].buildRectangle)
            goal=&u.orders[World::currentLeg(u.orders)];
        const auto* mission=goal ? &goal->mission : u.standbyActive ? &u.standbyState : nullptr;
        std::fprintf(output,"{\"kind\":\"mission_movement\",\"result\":[%d,%d,%d,%d,%u,%u,%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]}\n",
            u.x.v,u.groundY.v,u.z.v,u.speed.v,unsigned(portHeadingToRetail(u.heading)),
            unsigned(u.groundPitch),unsigned(u.groundRoll),unsigned(u.groundTerrainFlags),u.bodyBlockStreak,
            u.groundMoveTick,unsigned(u.groundMovementMode),u.groundScanTick,world.gameRng_,unsigned(u.groundSpeedMode),
            unsigned(world.paths_.pending(u.id)),unsigned(goal ? (goal->park ? 33 : goal->buildRectangle ? 27 : 28) : mission ? 44 : 0),
            mission ? unsigned(mission->stage) : 0,mission ? mission->waitMask : 0,
            mission ? mission->deadline : 0,mission ? mission->pending : 0,mission ? mission->flags : 0,
            goal ? goal->missionRadius : 0,u.missionEvents,unsigned(goal && goal->controller));
    }
    static void gateComposedState(FILE* output,const World& world,int id) {
        const auto& unit=*world.unit(id);const auto& script=world.unitScripts_.at(id);
        // The printed thread words include sleep counts: pay any skipped ticks first.
        const auto& state=const_cast<World::UnitScript&>(script).state();
        std::fprintf(output,"{\"kind\":\"gate\",\"result\":[%u,%u,%u,%u",unsigned(unit.active),
            unsigned(script.yardOpen),unsigned(state.vm.active),world.gameRng_);
        for (auto v:state.vm.statics) std::fprintf(output,",%u",v);
        for (const auto& thread:state.vm.threads) for (auto v:thread.words) std::fprintf(output,",%u",v);
        for (const auto& piece:state.pieces) {
            std::fprintf(output,",%u",unsigned(piece.active));
            for (auto* values:{&piece.moveTarget,&piece.moveSpeed,&piece.turnTarget,&piece.turnSpeed,
                              &piece.spinTarget,&piece.spinAcceleration,&piece.move,&piece.turn})
                for (auto value:*values) std::fprintf(output,",%u",uint32_t(value));
        }
        const auto bodies=world.searchBodyRect(footprintOrigin(unit.x,unit.type->footX),
            footprintOrigin(unit.z,unit.type->footZ),unit.type->footX,unit.type->footZ);
        for (const auto* body:bodies.cells) std::fprintf(output,",%d",body ? body->id : 0);
        std::fprintf(output,"]}\n");
    }
    static void startSearchMovement(World& world,Unit& u,int32_t gx,int32_t gz,int budget,int first,int count,bool reset=true) {
        if (reset) { world.paths_.clear();world.paths_.restoreTraversal(0,{},{}); }
        if (std::getenv("TAK_SEARCH_DIAG")) world.paths_.setGradeHost({
            [&](int id,bool last){world.prepareSearchGrade(id,last);},
            [&](int id,int x,int z,int retry,PathCell start) {
                const int result=world.searchGrade(id,x,z,retry,start);
                const auto* subject=world.unit(id);
                std::fprintf(stderr,"GRADE %u %d %d %d\n",world.tickCounter_,
                    x-subject->type->footX/2,z-subject->type->footZ/2,result);
                return result;
            },[&](int id){world.finishSearchGrade(id);}});
        world.paths_.setEntityPool(u.player,first,count);world.setPathBudget(budget);
        u.orders.clear();
        Order goal;goal.goal=goal.groundMission=true;goal.controller=++world.nextMovementController_;
        goal.missionTarget=std::pair{Fixed::raw(gx),Fixed::raw(gz)};
        goal.x=footprintWaypoint(footprintCell(Fixed::raw(gx),u.type->footX),u.type->footX);
        goal.z=footprintWaypoint(footprintCell(Fixed::raw(gz),u.type->footZ),u.type->footZ);
        goal.segmentX=Fixed::fromInt(u.x.floorInt());goal.segmentZ=Fixed::fromInt(u.z.floorInt());goal.hasSegment=true;
        u.orders.push_back(goal);u.routeStamp=0;
        u.routeCrowded=u.routeTraffic=u.routeFailed=u.routeDetour=false;
        u.groundMovementMode=0;u.groundScanTick=0;
        world.requestPath(u,goal.x.toFloat(),goal.z.toFloat());
    }
    static void searchMovementTick(World& world,Unit& u,uint32_t tick,bool runWorker=true) {
        world.tickCounter_=tick;
        const auto x=u.x,z=u.z;const auto heading=u.heading;
        world.updateGroundTerrainFlags(u);world.tickNavigationMovement(u,u.baseSpeed);
        if (u.x!=x || u.z!=z || u.heading!=heading || u.type->canHover)
            u.groundY=world.surfaceHeight(u,tick,&u.groundPitch,&u.groundRoll);
        if (runWorker) searchWorkerTick(world,tick);
    }
    static void searchWorkerTick(World& world,uint32_t tick) {
        // Same ordering and production adapters as World::tick: movement and
        // height first, then worker admission/search/delivery and notifications.
        world.paths_.tick([&](int id,int x,int z) {
            const auto* subject=world.unit(id);
            return subject?world.cellScore(subject->type,x,z,id):int(kCellImpassable);
        },[&](int id,const std::vector<PathCell>& route,Fixed gx,Fixed gz,bool failed,bool crowded,bool traffic,bool detour) {
            world.deliverSearchRoute(id,route,gx,gz,failed,crowded,traffic,detour);
        },[&](int id,PathCell& start,int& heading,RetailCostSearch::Costs& costs) {
            world.refreshSearchRequest(id,start,heading,costs);
        },tick,[&](int id,uint32_t now){return world.admitSearchRequest(id,now);});
        for (const auto& event:world.paths_.takeNotifications())
            if (auto* subject=world.unit(event.unitId))
                if (auto* goal=world.navigationMissionOrder(*subject);goal && goal->controller==event.controller)
                    goal->mission.pending|=uint32_t(event.events);
    }
    static void searchMovementState(FILE* output,const World& world,const Unit& u) {
        const auto end=World::currentLeg(u.orders);const auto& goal=u.orders[end];
        std::fprintf(output,"{\"kind\":\"search_movement\",\"result\":[%d,%d,%d,%d,%u,%u,%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%zu",
            u.x.v,u.groundY.v,u.z.v,u.speed.v,unsigned(portHeadingToRetail(u.heading)),
            unsigned(u.groundPitch),unsigned(u.groundRoll),unsigned(u.groundTerrainFlags),u.bodyBlockStreak,
            u.groundMoveTick,unsigned(u.groundMovementMode),u.groundScanTick,world.gameRng_,unsigned(u.groundSpeedMode),
            unsigned(world.paths_.pending(u.id)),goal.mission.pending,unsigned(!goal.navigationExhausted),
            goal.navigationConsumed?size_t(1):end+2);
        if (goal.navigationConsumed) std::fprintf(output,",%d,%d",goal.x.floorInt(),goal.z.floorInt());
        else {
            std::fprintf(output,",%d,%d",u.orders.front().segmentX.floorInt(),u.orders.front().segmentZ.floorInt());
            for (size_t i=0;i<=end;++i) std::fprintf(output,",%d,%d",u.orders[i].x.floorInt(),u.orders[i].z.floorInt());
        }
        std::fprintf(output,",%u]}\n",unsigned(u.routeCrowded|u.routeTraffic<<1|u.routeFailed<<2|u.routeDetour<<3));
    }
    static void exploration(World& world,std::istream& input) {
        int width,height,viewer;size_t count;
        if (!(input>>width>>height>>viewer) || width!=world.hW_/2 || height!=world.hH_/2 || viewer< -1 || viewer>31)
            throw std::runtime_error("invalid exploration geometry/context");
        for (auto& cell:world.editNavigationExploration()) {
            unsigned mask;if (!(input>>mask) || mask>65535) throw std::runtime_error("invalid exploration mask");
            cell=uint16_t(mask);
        }
        world.restoredNavigationViewer_=viewer;
        if (!(input>>count) || count!=world.units_.size()) throw std::runtime_error("invalid sight cache count");
        std::set<int> seen;
        for (size_t i=0;i<count;++i) {
            int id,x,z,eye,distance;unsigned sh,active;
            if (!(input>>id>>x>>z>>eye>>distance>>sh>>active) || !seen.insert(id).second ||
                !world.unit(id) || x< -32768 || x>32767 || z< -32768 || z>32767 ||
                distance< -32768 || distance>32767 || sh>255 || active>1)
                throw std::runtime_error("invalid sight cache");
            world.unit(id)->sightFootprint={int16_t(x),int16_t(z),eye,int16_t(distance),uint8_t(sh),active!=0};
        }
    }
    static void movementTick(World& world,uint32_t tick) { world.tickCounter_=tick; }
    static uint32_t movementSeed(const World& world) { return world.gameRng_; }
    static void navigationRoute(World& world,Unit& u,const std::vector<RetailSteeringPoint>& points,uint32_t stamp) {
        world.cancelPath(u);u.orders.clear();
        for (size_t i=1;i<points.size();++i) {
            Order o;o.x=points[i].x;o.z=points[i].z;
            o.segmentX=points[i-1].x;o.segmentZ=points[i-1].z;o.hasSegment=true;
            if (i+1==points.size()) {
                o.goal=o.groundMission=true;o.controller=uint64_t(u.id);
                // Controlled goal input: not satisfied during this partial route.
                o.missionTarget=std::pair{Fixed::fromInt(32000),Fixed::fromInt(32000)};
            }
            u.orders.push_back(o);
        }
        u.routeStamp=int32_t(stamp);u.routeCrowded=u.routeTraffic=u.routeFailed=u.routeDetour=false;
        u.groundMovementMode=0;u.groundScanTick=0;
    }
    static size_t navigationPoints(const World& /*world*/,const Unit& u) {
        return u.orders.empty()?0:u.orders[World::currentLeg(u.orders)].navigationConsumed?1:
            World::currentLeg(u.orders)+2;
    }
    static const tak::cob::RetailScriptState* scriptState(const World& world,int id) {
        const auto it=world.unitScripts_.find(id);
        // Callers read thread words: pay any skipped ticks first.
        return it==world.unitScripts_.end() ? nullptr : &const_cast<World::UnitScript&>(it->second).state();
    }
    static void discardColdFactory(World& world,int id) {
        if (size_t(id)<world.unitScriptById_.size()) world.unitScriptById_[size_t(id)]=nullptr;
        world.unitScripts_.erase(id);
    }
    static void movementRate(World& world,int id,uint32_t ownerFlags) {
        if (auto it=world.unitScripts_.find(id);it!=world.unitScripts_.end())
            it->second.movementRate=(ownerFlags>>2)&3u;
    }
    static void factory(World& world,int id,const std::string& hex,const UnitType* target,
                        const std::optional<std::array<unsigned,4>>& values) {
        auto* unit=world.unit(id);
        if (!unit || !unit->type) throw std::runtime_error("missing script owner");
        if (!unit->type->script()) return;
        std::vector<uint8_t> bytes;
        if (hex.size()%2) throw std::runtime_error("invalid script bytes");
        auto digit=[](char c)->unsigned {
            if (c>='0' && c<='9') return unsigned(c-'0');
            if (c>='a' && c<='f') return unsigned(c-'a'+10);
            if (c>='A' && c<='F') return unsigned(c-'A'+10);
            throw std::runtime_error("invalid script hex digit");
        };
        for (size_t n=0;n<hex.size();n+=2) bytes.push_back(uint8_t(digit(hex[n])*16+digit(hex[n+1])));
        auto [it,inserted]=world.unitScripts_.try_emplace(id,*unit->type->script());
        if (!inserted) throw std::runtime_error("duplicate factory restore");
        it->second.state().restore(*unit->type->script(),bytes);
        // Restored units have captured slot IDs, not their temporary spawn IDs.
        // Keep the fast script lookup coherent with the restored map node.
        if (world.unitScriptById_.size()<=size_t(id)) world.unitScriptById_.resize(size_t(id)+1,nullptr);
        world.unitScriptById_[size_t(id)]=&it->second;
        if (world.scriptYardById_.size()<=size_t(id)) world.scriptYardById_.resize(size_t(id)+1,0);
        if (target) { unit->buildQueue.push_back(target); it->second.activated=true; }
        if (values) {
            it->second.activated=(*values)[0]!=0;
            it->second.ready=(*values)[1]!=0;
            it->second.yardOpen=(*values)[2]!=0;
            it->second.buggerOff=(*values)[3]!=0;
        }
    }
    static void wind(World& world, std::istream& input, int version) {
        world.windEnabled_=false;
        if (version<11) return;
        int present;
        if (!(input>>present) || present<0 || present>1) throw std::runtime_error("invalid wind presence");
        if (!present) return;
        auto& w=world.wind_; unsigned heading,flags;
        if (!(input>>w.minimum>>w.maximum>>w.nextTick>>w.x>>w.z>>w.speed>>heading>>flags>>world.windRng_) ||
            w.minimum<0 || w.maximum<w.minimum || w.maximum>32767 || heading>65535 || flags>65535)
            throw std::runtime_error("invalid wind state");
        w.heading=uint16_t(heading); w.flags=uint16_t(flags); w.generation=0;
        world.windEnabled_=true;
    }
    static void queuedBuild(World& world,int id,const UnitType* type,int32_t x,int32_t z) {
        auto* unit=world.unit(id);
        if (!unit || !unit->type || !unit->type->isBuilder || !type)
            throw std::runtime_error("invalid queued build owner/type");
        unit->orders.push_back(world.makeBuildOrder(*unit,type,Fixed::raw(x),Fixed::raw(z)));
    }
    static void featurePresence(World& world,std::istream& input) {
        int count;if (!(input>>count) || count < -1 || count>world.terW_*world.terH_)
            throw std::runtime_error("invalid initial feature count");
        if (count<0) return;
        std::map<int,std::string> present;
        for (int i=0;i<count;++i) {
            int id;std::string name;
            if (!(input>>id>>std::quoted(name)) || id<0 || id>=world.terW_*world.terH_ ||
                !present.emplace(id,name).second) throw std::runtime_error("invalid initial feature record");
        }
        unsigned removed=0,added=0,changed=0;
        // Restore identities and positions, then let the production placement
        // rules reconstruct footprints. Captured grades/cells are not imported.
        for (size_t id=0;id<world.mapPlacementCells_.size();++id) {
            const auto index=world.mapPlacementCells_[id].feature;
            if (index>=world.mapPlacementTypes_.size()) continue;
            const auto found=present.find(int(id));
            if (found==present.end() || found->second!=world.mapPlacementTypes_[index].name)
                world.removeMapFeature(int(id)%world.terW_,int(id)/world.terW_);
        }
        for (auto& feature:world.features_) {
            const auto found=present.find(feature.id);
            if (found!=present.end()) continue;
            feature.alive=false;++removed;
            if (feature.blocks) world.blockCells(feature.id%world.terW_,
                feature.id/world.terW_,feature.fx,feature.fz,false);
        }
        for (const auto& [id,name]:present) {
            const auto index=world.mapPlacementCells_.at(size_t(id)).feature;
            if (index<world.mapPlacementTypes_.size() && world.mapPlacementTypes_[index].name==name)
                continue;
            const auto type=std::find_if(world.featTypes_.begin(),world.featTypes_.end(),
                [&](const auto& t){return t.name==name;});
            if (type==world.featTypes_.end())
                throw std::runtime_error("saved feature definition is missing: "+name);
            const int ti=int(type-world.featTypes_.begin());
            auto tracked=world.featureIdx_.find(id);
            if (tracked!=world.featureIdx_.end()) {
                world.swapFeature(world.features_[tracked->second],ti);++changed;
            } else {
                world.addFeature(id,float(id%world.terW_*16+type->fx*8),
                    float(id/world.terW_*16+type->fz*8),type->energy,type->fx,type->fz,type->blocking,ti);++added;
            }
            const auto restored=world.mapPlacementCells_.at(size_t(id)).feature;
            if (restored>=world.mapPlacementTypes_.size() || world.mapPlacementTypes_[restored].name!=name)
                throw std::runtime_error("saved feature placement refused at "+std::to_string(id));
        }
        // Legacy navigation grids still serve non-pathfinding consumers.
        for (const auto& f:world.features_) if (f.alive && f.blocks)
            world.blockCells(f.id%world.terW_,f.id/world.terW_,f.fx,f.fz,true);
        std::fprintf(stderr,"restored feature presence: %u removed, %u added, %u replaced tracked features\n",
                     removed,added,changed);
    }
    static bool corpse(World& world,int id,int mode) {
        auto* unit=world.unit(id);
        if (!unit || mode<0 || mode>3) throw std::runtime_error("invalid corpse event");
        if (mode==3) { world.retireCorpse(*unit);return false; }
        const int type=mode ? world.statueTypeOf(unit->type,mode==2) : world.corpseTypeOf(unit->type);
        unit->deadFor=0;
        if (unit->type->isStructure()) world.blockFoot(*unit->type,unit->x.toFloat(),unit->z.toFloat(),false);
        return world.placeCorpse(*unit,type);
    }
    static void restoreRoute(World& world, Unit& unit, const std::vector<RetailSteeringPoint>& points,
                             uint32_t stamp, unsigned flags) {
        unit.routeStamp=int32_t(stamp);
        if (points.empty()) return;
        const Order goal = unit.orders.back();
        std::vector<Order> path;
        for (size_t i = points.size() > 1 ? 1 : 0; i < points.size(); ++i) {
            Order order;
            order.x = points[i].x; order.z = points[i].z;
            path.push_back(order);
        }
        if (path.back().x != goal.x || path.back().z != goal.z) path.push_back(goal);
        world.replaceLeg(unit, path);
        unit.orders.front().segmentX = points.front().x;
        unit.orders.front().segmentZ = points.front().z;
        unit.routeStamp = int32_t(stamp);
        if (!(flags & 2)) world.paths_.cancel(unit.id);
    }
    static int score(const World& world, const Unit& unit, int x, int z) {
        return world.cellScore(unit.type, x, z, unit.id);
    }
    static int rawGrade(const World& world,const Unit& unit,int x,int z,int width,int height,
                        int requester,uint32_t recent,uint32_t stale) {
        World::SearchGradePlane plane;
        plane.nav=&world.navFor(unit.type);
        plane.preparation.requestSlot=requester;
        plane.preparation.recent=recent;plane.preparation.stale=stale;
        return world.rawSearchGrade(plane,x,z,width,height);
    }
    static void restoreSearchDiagnostics(World& world) {
        if (!std::getenv("TAK_RESTORE_SEARCH_DIAG")) return;
        world.paths_.setGradeHost({
            [&](int id,bool last) {
                std::fprintf(stderr,"PREP %u %d %d\n",world.tickCounter_,id,int(last));
                world.prepareSearchGrade(id,last);
            },[&](int id,int x,int z,int retry,PathCell start) {
                const int result=world.searchGrade(id,x,z,retry,start);
                const auto* u=world.unit(id);
                std::fprintf(stderr,"TRACEGRADE %u %d %d %d %d\n",world.tickCounter_,id,
                    x-u->type->footX/2,z-u->type->footZ/2,result);
                return result;
            },[&](int id){world.finishSearchGrade(id);}});
    }
    static void restoreRequest(World& world, Unit& unit, unsigned flags) {
        world.cancelPath(unit);
        if (flags & 2) {
            const auto& goal=unit.orders[world.currentLeg(unit.orders)];
            world.requestPath(unit,goal.x.toFloat(),goal.z.toFloat());
        }
    }
    static void scheduler(World& world,std::istream& input,int players) {
        int cursor;
        if (!(input>>cursor)) throw std::runtime_error("missing scheduler cursor");
        std::array<int,10> slots{},wraps{};
        world.pathBudgetClassMask_=0;
        auto& pools=world.retailEntityPools_.emplace();
        for (int player=0;player<players;++player) {
            int first,count,priority;
            if (!(input>>first>>count>>slots[size_t(player)]>>wraps[size_t(player)]>>priority) ||
                priority<0 || priority>1 || first<1 || count<1 || first>65535 || count>65535 || first+count>65536)
                throw std::runtime_error("invalid entity pool");
            for (int earlier=0;earlier<player;++earlier) {
                const auto [a,n]=pools[size_t(earlier)];
                if (first<a+n && a<first+count) throw std::runtime_error("overlapping entity pools");
            }
            world.paths_.setEntityPool(player,first,count);
            pools[size_t(player)]={first,count};
            world.pathBudgetClassMask_|=uint32_t(priority)<<player;
        }
        world.paths_.restoreTraversal(cursor,slots,wraps);
    }
    static void boundary(World& world, uint32_t tick, uint32_t rng,bool allocation) {
        world.tickCounter_ = tick;
        world.gameRng_ = rng; // Captured STATE, never run it through the seed initializer.
        if (allocation && !world.retailEntityPools_) throw std::runtime_error("entity pools absent");
        world.retailAllocation_=allocation;
        if (allocation) for (const auto& unit:world.units_) {
            const auto [first,count]=(*world.retailEntityPools_)[size_t(unit.player)];
            if (unit.id<first || unit.id>=first+count) throw std::runtime_error("unit outside owner entity pool");
        }
        world.nextId_ = 1;
        for (const auto& unit : world.units_)
            world.nextId_ = std::max(world.nextId_, unit.id + 1);
        world.rebuildOccupancy();
    }
    static void grades(World& world,std::istream& input) {
        int count;
        if (!(input>>count) || count<0 || count>100) throw std::runtime_error("invalid grade plane count");
        for (int n=0;n<count;++n) {
            int id; uint32_t recent,stale; std::string packed;
            if (!(input>>id>>recent>>stale>>packed)) throw std::runtime_error("missing grade plane");
            const auto* unit=world.unit(id);
            if (!unit) throw std::runtime_error("missing grade plane owner");
            World::SearchGradePlane plane;
            plane.nav=&world.navFor(unit->type);
            plane.footX=unit->type->footX; plane.footZ=unit->type->footZ;
            plane.navVersion=plane.nav->version();
            plane.preparation.recent=recent; plane.preparation.stale=stale;
            const size_t size=size_t(plane.nav->width())*plane.nav->height();
            if (packed.size()!=size_t((plane.nav->height()+7)/8)*plane.nav->width()*8)
                throw std::runtime_error("invalid grade plane size");
            plane.cells.assign(size,0);
            for (size_t i=0;i<size;++i) {
                // Each word stores eight vertically adjacent cells.
                const size_t x=i%plane.nav->width(),z=i/plane.nav->width();
                const size_t byte=((z/8)*plane.nav->width()+x)*4+(z%8)/2;
                const char c=packed[byte*2+(z%2 ? 0 : 1)];
                const auto pos=std::string_view("0123456789abcdef").find(c);
                if (pos==std::string_view::npos) throw std::runtime_error("invalid grade plane hex");
                world.setSearchCell(plane,i,uint8_t(pos));
            }
            world.searchGrades_.push_back(std::move(plane));
        }
    }
    // tools/re/check_ground_owner.py: replay its native ground-word events
    // through World's own event handlers and compare every cell's owner as
    // searchBodyRect reports it. Native ids are mapped to World's on first
    // placement; a lifted body is held off the map as cargo is.
    static int groundOwner(const char* path) {
        std::ifstream input(path);
        std::string word;
        std::optional<World> world;
        std::map<std::pair<int,int>,UnitType> types;
        std::map<int,int> ids;
        int width=0,height=0,scenario=0,checks=0,bad=0,overlapped=0,legacyBad=0;
        auto type=[&](int fx,int fz)->const UnitType* {
            auto [it,fresh]=types.try_emplace({fx,fz});
            if (fresh) {
                auto& t=it->second;t.id="ground"+std::to_string(fx)+"x"+std::to_string(fz);
                t.footX=fx;t.footZ=fz;t.maxVel=Fixed::fromInt(1);t.maxHp=100;t.maxSlope=t.maxWaterSlope=255;
            }
            return &it->second;
        };
        auto at=[](int origin,int foot){return float(origin*16+foot*8);};
        while (input>>word) {
            if (word=="scenario") {
                input>>width>>height;++scenario;
                world.emplace();world->setTerrain(std::vector<uint8_t>(size_t(width)*height,0),width,height,0);
                ids.clear();
            } else if (word=="place") {
                int i,fx,fz,x,z;input>>i>>fx>>fz>>x>>z;
                if (auto it=ids.find(i);it!=ids.end()) {
                    auto& u=*world->unit(it->second);
                    u.x=Fixed::fromFloat(at(x,fx));u.z=Fixed::fromFloat(at(z,fz));u.inTransport=0;
                    world->groundStamp(u);
                } else ids[i]=world->spawn(type(fx,fz),at(x,fx),at(z,fz),0.0f,0);
            } else if (word=="move") {
                int i,x,z;input>>i>>x>>z;
                auto& u=*world->unit(ids.at(i));
                world->groundUnstamp(u);
                u.x=Fixed::fromFloat(at(x,u.type->footX));u.z=Fixed::fromFloat(at(z,u.type->footZ));
                world->groundStamp(u);
            } else if (word=="lift" || word=="die") {
                int i;input>>i;
                auto& u=*world->unit(ids.at(i));
                world->groundRemove(u);
                if (word=="die") u.deadFor=0; else u.inTransport=-1;
            } else if (word=="check") {
                std::vector<int> native(size_t(width)*height);
                for (auto& id:native) input>>id;
                const auto bodies=world->searchBodyRect(0,0,width,height);
                bool same=true,legacy=true;
                for (size_t c=0;c<native.size();++c) {
                    const int expected=native[c] ? ids.at(native[c]) : 0;
                    const Unit* body=bodies.cells[c];
                    // The rule before the ground word: the last covering
                    // unit in vector order.
                    int covering=0,last=0;
                    for (const auto& u:world->units_) if (u.alive() && !u.embarked()) {
                        const int ux=footprintOrigin(u.x,u.type->footX),uz=footprintOrigin(u.z,u.type->footZ);
                        const int cx=int(c%size_t(width)),cz=int(c/size_t(width));
                        if (cx>=ux && cx<ux+u.type->footX && cz>=uz && cz<uz+u.type->footZ) { ++covering;last=u.id; }
                    }
                    overlapped+=covering>1;
                    if ((body ? body->id : 0)!=expected) same=false;
                    if (last!=expected) legacy=false;
                }
                ++checks;legacyBad+=!legacy;
                if (!same && bad++<5) std::printf("mismatch: scenario %d check %d\n",scenario,checks);
            }
        }
        std::printf("World ground owners: %d checks over %d scenarios, %d overlapped cell samples, %d mismatches "
                    "(last-in-vector-order rule: %d)\n",checks,scenario,overlapped,bad,legacyBad);
        return bad || !legacyBad ? 1 : 0;
    }
};
}

namespace {
void frame(FILE* output, const tak::sim::World& world) {
    std::fprintf(output, "{\"kind\":\"frame\",\"tick\":%u,\"units\":[", world.tickCount());
    bool first = true;
    for (const auto& value : world.units()) {
        const auto* unit = &value;
        if (!unit->alive()) continue;
        std::fprintf(output, "%s{\"id\":%d,\"y_raw\":%d,\"x_raw\":%d,\"z_raw\":%d,\"heading\":%u,"
                     "\"speed_raw\":%d,\"base_speed_raw\":%d,\"route\":[", first ? "" : ",", unit->id,
                     unit->type->canFly?unit->flightY.v:unit->groundY.v, unit->x.v, unit->z.v, unsigned(tak::sim::portHeadingToRetail(unit->heading)),
                     unit->speed.v, unit->baseSpeed.v);
        bool firstOrder = true;
        bool buildApproach=false;
        for (const auto& order : unit->orders) {
            std::fprintf(output, "%s[%d,%d,%d,%d,%d]", firstOrder ? "" : ",",
                         order.x.v, order.z.v, int(order.hasSegment), order.segmentX.v, order.segmentZ.v);
            firstOrder = false;
            if (order.goal) { buildApproach=order.buildRectangle.has_value();break; }
        }
        std::fprintf(output, "],\"ground_refusal\":%d,\"ground_mode\":%u,\"ground_scan_tick\":%u,\"ground_missions\":[",
                     unit->bodyBlockStreak,unsigned(unit->groundMovementMode),unit->groundScanTick);
        bool firstMission=true;
        for (const auto& order:unit->orders) if (order.groundMission) {
            std::fprintf(output,"%s[%u,%u,%u,%u,%u,%llu,%u,%d,%d,%d,%d,%d]",firstMission ? "" : ",",
                unsigned(order.mission.stage),order.mission.waitMask,order.mission.deadline,
                order.mission.pending,order.missionRadius,
                static_cast<unsigned long long>(order.controller),order.mission.flags,order.groundResponse.mode,
                int(order.groundResponse.origin.x),int(order.groundResponse.origin.z),
                int(order.groundResponse.goal.x),int(order.groundResponse.goal.z));
            firstMission=false;
        }
        std::fprintf(output, "],\"flight_missions\":[");
        firstMission=true;
        if (unit->type->canFly) for (const auto& order:unit->orders) {
            std::fprintf(output,"%s[%d,%u,%u,%u,%u,%u,%d,%d]",firstMission ? "" : ",",
                order.flightMoveMission?59:62,unsigned(order.mission.stage),order.mission.waitMask,
                order.mission.deadline,order.mission.pending,order.mission.flags,order.x.v,order.z.v);
            firstMission=false;
        }
        std::fprintf(output, "],\"build_approach\":%s,\"script_threads\":[",buildApproach ? "true" : "false");
        if (const auto* script=tak::sim::RetailReplayProbe::scriptState(world,unit->id)) {
            for (size_t i=0;i<script->vm.threads.size();++i) {
                std::fprintf(output,"%s[",i ? "," : "");
                for (size_t j=0;j<8;++j) std::fprintf(output,"%s%u",j ? "," : "",script->vm.threads[i].words[j]);
                std::fprintf(output,"]");
            }
        }
        std::fprintf(output, "],\"scores_7x7\":[");
        const int cx = tak::sim::footprintCell(unit->x, unit->type->footX);
        const int cz = tak::sim::footprintCell(unit->z, unit->type->footZ);
        for (int dz = -3; dz <= 3; ++dz)
            for (int dx = -3; dx <= 3; ++dx)
                std::fprintf(output, "%s%d", dx == -3 && dz == -3 ? "" : ",",
                    tak::sim::RetailReplayProbe::score(world, *unit, cx + dx, cz + dz));
        std::fprintf(output, "]");
        if (unit->retailSite) {
            if (unit->retailSite->mission) {
                const auto& m=*unit->retailSite->mission;
                std::fprintf(output,",\"get_built\":[%u,%u,%u,%u,%d]",unsigned(m.stage),m.waitMask,
                    m.deadline,m.pending,unit->retailSite->builder);
            }
            const auto& s=unit->retailSite->progress;
            std::fprintf(output,",\"construction\":[%.9g,%u,%u,%u]",double(s.remaining),unsigned(s.hp),s.events,s.flags);
        }
        if (unit->retailBuild && unit->retailBuild->flying) {
            const auto& m=unit->retailBuild->mission;
            std::fprintf(output,",\"flying_construction\":[%u,%u,%u,%u,%u]",
                unsigned(m.stage),m.waitMask,m.deadline,m.pending,m.flags);
        }
        if (unit->type->isStructure() && unit->type->isBuilder) {
            const auto& cache=world.player(unit->player).buildCache;
            if (cache) {
                std::fprintf(output,",\"factory_queue\":[");bool firstType=true;
                for (const auto* type:unit->buildQueue) {
                    auto it=std::find_if(cache->entries.begin(),cache->entries.end(),
                        [&](const auto& entry){return entry.type==type;});
                    if (it==cache->entries.end()) throw std::runtime_error("queued factory type absent from catalogue");
                    std::fprintf(output,"%s%zu",firstType?"":",",size_t(it-cache->entries.begin())+1);firstType=false;
                }
                std::fprintf(output,"]");
            }
        }
        std::fprintf(output, "}");
        first = false;
    }
    std::fprintf(output, "],\"resources\":[");
    for (int i=0;i<world.numPlayers();++i) {
        const auto& player=world.player(i);
        std::fprintf(output,"%s{\"mana\":%.17g,\"income\":%.9g,\"storage\":%.9g}",
            i ? "," : "",player.mana,double(player.income),double(player.storage));
    }
    std::fprintf(output, "],\"ai\":[");
    for (int player=0;player<world.numPlayers();++player) {
        if (player) std::fputc(',',output);
        const auto& state=world.player(player).retailAi;
        if (!state) { std::fprintf(output,"null");continue; }
        std::fprintf(output,"{\"countdown\":%d,\"groups\":[",state->schedule.assignmentCountdown);
        bool firstGroup=true;
        for (unsigned i=0;i<100;++i) {
            if (!state->schedule.groups[i].present) continue;
            const auto& group=state->squads[i];
            std::fprintf(output,"%s[%u,%u,%u,%u,[",firstGroup ? "" : ",",i,
                state->schedule.groups[i].deadline,group.active,group.dirty);
            for (unsigned j=0;j<group.parameters.size();++j)
                std::fprintf(output,"%s%d",j ? "," : "",group.parameters[j]);
            std::fprintf(output,"],[");
            for (size_t j=0;j<group.members.size();++j)
                std::fprintf(output,"%s%d",j ? "," : "",group.members[j]);
            std::fprintf(output,"]]");firstGroup=false;
        }
        std::fprintf(output,"]}");
    }
    std::fprintf(output, "]}\n");
}
}

int main(int argc, char** argv) {
    using namespace tak;
    using namespace tak::sim;
    if (argc==3 && std::string(argv[1])=="--ground-owner") return RetailReplayProbe::groundOwner(argv[2]);
    if (argc==7 && std::string(argv[2])=="--crowd-movement") {
        auto vfs=hpi::mountRetailRoot(argv[1]);
        TypeRegistry registry;setupRegistry(registry,vfs,std::stoi(argv[4])!=0);
        MatchConfig config;config.vfs=&vfs;config.mapPath=hpi::findMap(vfs,argv[3]);config.slots.resize(1);
        World world;setupMatch(world,registry,config);
        std::ifstream input(argv[5]);
        int count,rounds,budget,gx,gz;int32_t base;
        if (!(input>>count>>rounds>>budget>>gx>>gz>>base) || count<1 || count>1000 || rounds<1 || rounds>5000 || budget<1) return 2;
        const auto* type=registry.find("zonter");if (!type) return 2;
        std::vector<int> ids;
        for (int i=0;i<count;++i) {
            int x,z;if (!(input>>x>>z)) return 2;
            ids.push_back(world.spawn(type,float(x),float(z)));
        }
        RetailReplayProbe::crowdStart(world,ids,base,budget,gx,gz);
        FILE* output=std::fopen(argv[6],"w");if (!output) return 2;
        for (int tick=1;tick<=rounds;++tick) {
            RetailReplayProbe::crowdTick(world,ids,uint32_t(tick));
            for (int id:ids) RetailReplayProbe::composedMissionState(output,world,*world.unit(id));
        }
        std::fclose(output);return 0;
    }
    if (argc==7 && std::string(argv[2])=="--crowd-world") {
        // Input: count rounds budget base crt unitCap, then "x z legs gx gz..."
        // per unit; legs after the first are queued. unitCap sizes the
        // production pools (World::entityPoolCapacity).
        auto vfs=hpi::mountRetailRoot(argv[1]);
        TypeRegistry registry;setupRegistry(registry,vfs,std::stoi(argv[4])!=0);
        MatchConfig config;config.vfs=&vfs;config.mapPath=hpi::findMap(vfs,argv[3]);config.slots.resize(1);
        config.pathfindingMode=PathfindingMode::Retail;
        World world;setupMatch(world,registry,config);
        std::ifstream input(argv[5]);
        int count,rounds,budget,unitCap;int32_t base;uint32_t crt;
        if (!(input>>count>>rounds>>budget>>base>>crt>>unitCap) || count<1 || count>1000 || rounds<1 || rounds>5000 ||
            budget<1 || unitCap<0 || (unitCap>0 && count>unitCap)) return 2;
        const auto* type=registry.find("zonter");if (!type) return 2;
        if (!world.units().empty()) return 2;
        world.setUnitCap(unitCap);
        RetailReplayProbe::crtSeed(world,crt);
        std::vector<int> ids;std::vector<std::vector<std::pair<int,int>>> goals;
        for (int i=0;i<count;++i) {
            int x,z,legs;if (!(input>>x>>z>>legs) || legs<1 || legs>8) return 2;
            ids.push_back(world.spawn(type,float(x),float(z)));goals.emplace_back();
            for (int leg=0;leg<legs;++leg) { int gx,gz;if (!(input>>gx>>gz)) return 2;goals.back().emplace_back(gx,gz); }
        }
        if (std::getenv("TAK_CROWD_RNG")) world.setRngObserver([](const World::RngObservation& o) {
            std::fprintf(stderr,"RNG tick=%u bound=%d before=%u after=%u ret=%x %s:%u %s\n",o.tick,o.bound,o.seedBefore,o.seedAfter,
                o.retailReturnAddress,o.caller.file_name(),unsigned(o.caller.line()),o.caller.function_name());
        });
        RetailReplayProbe::crowdWorldStart(world,ids,goals,base,budget);
        // The production pool World built, for the harness to check against
        // the native allocation: capacity, then each Hunter's slot.
        std::printf("SLOTS %d",RetailReplayProbe::entityCapacity(world,0));
        for (int id:ids) std::printf(" %d",world.entitySlot(id));
        std::printf("\n");std::fflush(stdout);
        FILE* output=std::fopen(argv[6],"w");if (!output) return 2;
        for (int tick=1;tick<=rounds;++tick) {
            world.tick(1.f/30);
            if (world.tickCount()!=uint32_t(tick)) return 3;
            for (int id:ids) RetailReplayProbe::composedMissionState(output,world,*world.unit(id));
        }
        std::fclose(output);return 0;
    }
    if ((argc==7 || argc==8 || argc==9 || argc==10) && (std::string(argv[2])=="--gate-composed" || std::string(argv[2])=="--gate-composed-missions" || std::string(argv[2])=="--gate-composed-queue" || std::string(argv[2])=="--gate-composed-replace" || std::string(argv[2])=="--gate-composed-pair" || std::string(argv[2])=="--gate-composed-pair-close" || std::string(argv[2])=="--gate-composed-park" || std::string(argv[2])=="--gate-composed-rectangle" || std::string(argv[2])=="--gate-composed-rectangle-blocked" || std::string(argv[2])=="--gate-composed-rectangle-reachable")) {
        const bool missions=std::string(argv[2])!="--gate-composed";
        const bool rectangleReachable=std::string(argv[2])=="--gate-composed-rectangle-reachable";
        const bool rectangleBlocked=rectangleReachable || std::string(argv[2])=="--gate-composed-rectangle-blocked";
        const bool rectangle=rectangleBlocked || std::string(argv[2])=="--gate-composed-rectangle";
        const bool park=std::string(argv[2])=="--gate-composed-park";
        const bool queued=std::string(argv[2])=="--gate-composed-queue";
        const bool close=std::string(argv[2])=="--gate-composed-pair-close";
        const bool pair=close || std::string(argv[2])=="--gate-composed-pair";
        const bool replace=std::string(argv[2])=="--gate-composed-replace";
        if (park ? argc!=9 : replace ? (argc!=8 && argc!=10) : argc!=7) return 2;
        const int parkLossAt=park?std::stoi(argv[7]):0,parkFollowAt=park?std::stoi(argv[8]):0;
        const int replaceAt=replace?std::stoi(argv[7]):0;
        const int replaceX=argc==10?std::stoi(argv[8]):512,replaceZ=argc==10?std::stoi(argv[9]):696;
        auto vfs=hpi::mountRetailRoot(argv[1]);
        TypeRegistry registry;setupRegistry(registry,vfs,std::stoi(argv[4])!=0);
        const auto* gateType=registry.find(argv[3]);const auto* moverType=registry.find("verpult");
        const int rounds=std::stoi(argv[5]);const int32_t base=std::stoi(argv[6]);
        if (!gateType || !gateType->gate || !moverType || rounds<1 || rounds>5000 || base<=0) return 2;
        UnitType reachableMover=*moverType;
        if (rectangleReachable) { reachableMover.buildDist=1024;moverType=&reachableMover; }
        World world;
        // Optional authored relief (64x64 height bytes, sea 0) for ramp routes;
        // the native harness writes the same heights into its cell records.
        std::vector<uint8_t> gateHeights(64*64,0);
        if (const char* path=std::getenv("TAK_GATE_HEIGHTS"); path && *path) {
            std::ifstream relief(path,std::ios::binary);
            if (!relief.read(reinterpret_cast<char*>(gateHeights.data()),std::streamsize(gateHeights.size())))
                return 2;
        }
        world.setTerrain(gateHeights,64,64,0);world.buildNavClasses(registry);
        const int gx=(512-(gateType->footX-1)*8)/16,gz=(512-(gateType->footZ-1)*8)/16;
        std::vector<uint16_t> features(64*64,0xffff);
        for (int x=0;x<64;++x) if (x<gx || x>=gx+gateType->footX) features[size_t(gz)*64+x]=0xfffc;
        if (rectangleBlocked) for (int x=0;x<64;++x) features[40*64+x]=0xfffc;
        world.setMapPlacementFeatures(features,{});
        world.blockCells(0,gz,gx,1,true);world.blockCells(gx+gateType->footX,gz,64-gx-gateType->footX,1,true);
        const int gate=world.spawn(gateType,512,512),mover=world.spawn(moverType,512,344);
        const int follower=pair?world.spawn(moverType,close?512:192,close?232:120):0;
        auto& moving=*world.unit(mover);
        if (pair) RetailReplayProbe::gatePairStart(world,moving,*world.unit(follower),base,close);
        else RetailReplayProbe::gateComposedStart(world,moving,base,missions);
        if (rectangle) RetailReplayProbe::gateRectangleStart(world,moving,gateType);
        if (park) RetailReplayProbe::gateParkStart(world,moving,gate);
        if (queued) world.order(mover,512,344,true);
        for (int tick=1;tick<=rounds;++tick) {
            if (tick==parkLossAt) moving.missionEvents|=8;
            if (tick==parkFollowAt) world.order(mover,512,696,true);
            const int handoff=rectangle ? RetailReplayProbe::gateRectangleTick(world,gate,moving,uint32_t(tick)) : 0;
            if (!rectangle) {
                if (pair) RetailReplayProbe::gatePairTick(world,gate,moving,*world.unit(follower),uint32_t(tick));
                else RetailReplayProbe::gateComposedTick(world,gate,moving,uint32_t(tick),missions,tick==replaceAt,replaceX,replaceZ);
            }
            if (missions) RetailReplayProbe::composedMissionState(stdout,world,moving);
            else RetailReplayProbe::searchMovementState(stdout,world,moving);
            if (pair) RetailReplayProbe::composedMissionState(stdout,world,*world.unit(follower));
            RetailReplayProbe::gateComposedState(stdout,world,gate);
            if (handoff) { std::printf("{\"kind\":\"%s\",\"tick\":%d}\n",handoff==1?"build_handoff":"build_abort",tick);break; }
        }
        return 0;
    }
    if (argc != 4 && !(argc==5 && std::string(argv[4])=="--feature-map") && !(argc == 6 && (std::string(argv[4]) == "--grades-unit" ||
                                    std::string(argv[4]) == "--placement-queries" ||
                                    std::string(argv[4]) == "--corpse-features" ||
                                    std::string(argv[4]) == "--ground-height-queries" ||
                                    std::string(argv[4]) == "--ground-steps" ||
                                    std::string(argv[4]) == "--ground-brake-steps" ||
                                    std::string(argv[4]) == "--ground-travel-steps" ||
                                    std::string(argv[4]) == "--ground-navigation-steps" ||
                                    std::string(argv[4]) == "--search-movement" ||
                                    std::string(argv[4]) == "--raw-grade-queries" ||
                                    std::string(argv[4]) == "--live-grade-queries"))) {
        std::fprintf(stderr, "usage: retail_replay_probe RETAIL_ROOT INPUT OUTPUT_JSONL [--feature-map | --corpse-features FILE | --ground-brake-steps FILE | --ground-height-queries FILE | --grades-unit ID | --placement-queries FILE | --raw-grade-queries FILE | --live-grade-queries FILE]\n");
        return 2;
    }
    try {
        std::ifstream input(argv[2]);
        std::string magic, mapName;
        int version, count, steps, players;
        uint32_t tick, rng;
        if (!(input >> magic >> version >> tick >> rng >> count >> steps >> players >> std::quoted(mapName)) ||
            magic != "TAK_MOVEMENT_PROBE" || version < 1 || version > 37 || count < 1 || count > 20000 ||
            steps < 0 || steps > 3000 || players < 1 || players > 10)
            throw std::runtime_error("invalid diagnostic input header");
        const bool staticQueries=steps==0 && argc>=5 &&
            (std::string(argv[4])=="--raw-grade-queries" ||
             std::string(argv[4])=="--live-grade-queries" ||
             std::string(argv[4])=="--ground-height-queries" ||
             std::string(argv[4])=="--ground-steps" ||
             std::string(argv[4])=="--ground-brake-steps" ||
             std::string(argv[4])=="--ground-travel-steps" ||
             std::string(argv[4])=="--ground-navigation-steps" ||
             std::string(argv[4])=="--search-movement" ||
             std::string(argv[4])=="--placement-queries" || std::string(argv[4])=="--feature-map" ||
             std::string(argv[4])=="--corpse-features");
        auto vfs = hpi::mountRetailRoot(argv[1]);
        TypeRegistry registry;
        unsigned crusades=0;
        if (version>=34 && (!(input>>crusades) || crusades>1))
            throw std::runtime_error("invalid captured roster selection");
        setupRegistry(registry, vfs, crusades!=0);
        MatchConfig config;
        config.vfs = &vfs;
        config.mapPath = hpi::findMap(vfs, mapName);
        if (config.mapPath.empty()) throw std::runtime_error("saved map not found");
        config.slots.resize(size_t(players)); // No fresh monarchs or AI.
        for (int player = 0; player < players; ++player)
            config.slots[size_t(player)].team = player; // Diagnostic default, not restored alliances.
        World world;
        setupMatch(world, registry, config);
        world.setPathService(true);
        RetailReplayProbe::restoreSearchDiagnostics(world);
        RetailReplayProbe::wind(world,input,version);
        if (version>=7) RetailReplayProbe::scheduler(world,input,players);
        if (version>=26) RetailReplayProbe::featurePresence(world,input);
        std::vector<int> ids;
        struct Goal { int id; int32_t x, z; uint32_t stamp; unsigned flags;
                      std::vector<RetailSteeringPoint> route; bool hasMission;
                      RetailMissionState mission; uint32_t radius, events;
                      Order build; RetailGroundResponse response; };
        std::vector<Goal> goals;
        for (int i = 0; i < count; ++i) {
            std::string typeName;
            int id, player, heading, fx, fz, moving, building;
            int32_t x, z, speed, base, gx, gz;
            if (!(input >> id >> player >> std::quoted(typeName) >> x >> z >> heading >> speed >> base >>
                  fx >> fz >> moving >> gx >> gz >> building) || id < 1 || id > 65535 ||
                player < 0 || player >= players || heading < 0 || heading > 65535 ||
                (moving != 0 && moving != 1) || (building != 0 && building != 1) ||
                std::find(ids.begin(), ids.end(), id) != ids.end())
                throw std::runtime_error("invalid diagnostic unit record");
            unsigned movementFlags = 0, routeFlags = 0;
            uint32_t routeStamp = 0;
            uint32_t scanTick = 0;
            std::vector<RetailSteeringPoint> route;
            if (version >= 2) {
                int routeCount;
                if (!(input >> movementFlags >> routeStamp >> routeFlags >> routeCount) ||
                    movementFlags > 65535 || routeFlags > 255 || routeCount < -1 || routeCount > 64)
                    throw std::runtime_error("invalid diagnostic navigator record");
                for (int j = 0; j < routeCount; ++j) {
                    int rx, rz;
                    if (!(input >> rx >> rz) || rx < -32768 || rx > 32767 || rz < -32768 || rz > 32767)
                        throw std::runtime_error("invalid captured route point");
                    route.push_back({Fixed::fromInt(rx), Fixed::fromInt(rz)});
                }
            }
            if (version >= 3 && !(input >> scanTick))
                throw std::runtime_error("invalid diagnostic scan deadline");
            bool hasMission = false;
            unsigned missionKind = 0;
            RetailMissionState mission;
            RetailGroundResponse response;
            unsigned moveOrder=0,fireOrder=0;
            uint32_t radius = 0, events = 0;
            int32_t standbyHeight=0;
            if (version >= 4) {
                unsigned present, stage;
                if (!(input >> present) || present > (version >= 13 ? 3u : version >= 5 ? 2u : 1u))
                    throw std::runtime_error("invalid diagnostic mission presence");
                missionKind = present;
                hasMission = present == 1;
                if (present) {
                    if (!(input >> stage >> mission.waitMask >> mission.deadline >> mission.pending >>
                          mission.flags >> radius >> events) || stage > (present == 2 ? 1u : present == 1 && version>=15 ? 3u : 2u) || (present == 1 && !moving) || (present == 2 && moving))
                        throw std::runtime_error("invalid diagnostic ground mission");
                    mission.stage = uint8_t(stage);
                    if (present==1 && version>=15) {
                        int ox,oz,gx,gz;
                        if (!(input>>response.mode>>ox>>oz>>gx>>gz>>moveOrder>>fireOrder) ||
                            std::min({ox,oz,gx,gz})< -32768 || std::max({ox,oz,gx,gz})>32767 ||
                            moveOrder>3 || fireOrder>3) throw std::runtime_error("invalid ground response state");
                        response.origin={int16_t(ox),int16_t(oz)}; response.goal={int16_t(gx),int16_t(gz)};
                    }
                    if (present==3 && (!(input>>standbyHeight) || moving || radius>3))
                        throw std::runtime_error("invalid VTOL standby state");
                }
            }
            const UnitType* type = registry.find(typeName);
            Order build;
            if (version >= 6) {
                int present;
                if (!(input >> present) || present < 0 || present > 1)
                    throw std::runtime_error("invalid builder presence");
                if (present) {
                    std::string name;
                    RetailRectGoal rectangle;
                    if (!(input >> std::quoted(name) >> build.buildX.v >> build.buildZ.v >>
                        rectangle.minX >> rectangle.maxX >> rectangle.minZ >> rectangle.maxZ >>
                        build.mission.waitMask >> build.mission.pending) || !moving ||
                        rectangle.minX > rectangle.maxX || rectangle.minZ > rectangle.maxZ ||
                        !(build.buildType=registry.find(name)))
                        throw std::runtime_error("invalid builder approach");
                    build.buildRectangle=rectangle;
                    build.mission.stage=1;
                }
            }
            if (!type || type->footX != fx || type->footZ != fz)
                throw std::runtime_error("type/footprint mismatch for " + typeName);
            // Spawn supplies type defaults; raw values below avoid float loss.
            int temporary = world.spawn(type, 0, 0, 0, player);
            RetailReplayProbe::discardColdFactory(world,temporary);
            Unit* unit = world.unit(temporary);
            unit->id = id;
            unit->x = unit->homeX = unit->stuckX = Fixed::raw(x);
            unit->z = unit->homeZ = unit->stuckZ = Fixed::raw(z);
            unit->heading = retailHeadingToPort(uint16_t(heading));
            unit->speed = Fixed::raw(speed);
            unit->baseSpeed = Fixed::raw(base);
            unit->underConstruction = building;
            unit->groundMovementMode = uint8_t((movementFlags >> 5) & 7);
            unit->groundTerrainFlags = uint16_t(movementFlags&0x1800);
            if (version>=25 && !type->isStructure()) unit->routeStamp=int32_t(routeStamp);
            unit->groundScanTick = scanTick;
            if (version>=7) {
                unsigned outcome;
                if (!(input>>unit->groundGradeTick>>outcome))
                    throw std::runtime_error("invalid occupancy timestamp/outcome");
                unit->routeCrowded=(outcome&1)!=0;
                unit->routeTraffic=(outcome&2)!=0;
                unit->routeFailed=(outcome&4)!=0;
                unit->routeDetour=(outcome&8)!=0;
            }
            if (version>=33) {
                unsigned mode;
                if (!(input>>mode) || mode>3) throw std::runtime_error("invalid ground occupancy mode");
                if (type->canFly) unit->flightGroundMode=uint8_t(mode);
            }
            unit->standbyAllowed = version < 5 || moving || missionKind == 2 || missionKind==3;
            unit->guardNoMoveAllowed=false;
            if (missionKind == 2 || missionKind==3) {
                unit->standbyState = mission;
                unit->standbyActive = true;
                unit->missionEvents = events;
                if (missionKind==3) {
                    if (!type->canFly) throw std::runtime_error("VTOL standby owner cannot fly");
                    unit->flightGroundMode=uint8_t(radius); unit->flightY=Fixed::raw(standbyHeight);
                }
            }
            unit->bodyBlockStreak = movementFlags & 4 ? 2 : movementFlags & 8 ? 1 : 0;
            unit->groundSpeedMode=uint8_t((movementFlags>>8)&7);
            if (type->isStructure()) {
                // Canonical footprint anchors preserve the raw cell origin even
                // when the saved subpixel position cannot round-trip via float.
                world.blockFoot(*type, footprintWaypoint(footprintCell(unit->x, fx), fx).toFloat(),
                                footprintWaypoint(footprintCell(unit->z, fz), fz).toFloat(), true);
            }
            ids.push_back(id);
            if (hasMission && version>=15) { unit->moveState=int(moveOrder); unit->fireState=int(fireOrder); }
            if (moving) goals.push_back({id, gx, gz, routeStamp, routeFlags, std::move(route), hasMission, mission, radius, events,build,response});
        }
        RetailReplayProbe::boundary(world, tick, rng,false);
        if (version>=8) RetailReplayProbe::grades(world,input);
        struct QueuedBuild { int id; const UnitType* type; int32_t x,z; };
        std::vector<QueuedBuild> queuedBuilds;
        if (version>=9) {
            int count;
            if (!(input>>count) || count<0 || count>20000) throw std::runtime_error("invalid queued build count");
            for (int i=0;i<count;++i) {
                int id; int32_t x,z; std::string name;
                if (!(input>>id>>std::quoted(name)>>x>>z)) throw std::runtime_error("invalid queued build");
                const auto* type=registry.find(name);
                if (!type || !world.unit(id)) throw std::runtime_error("missing queued build type/owner");
                queuedBuilds.push_back({id,type,x,z});
            }
        }
        if (version>=10) {
            int count;
            if (!(input>>count) || count<0 || count>20000) throw std::runtime_error("invalid flight count");
            for (int i=0;i<count;++i) {
                int id,orders; int32_t y; uint32_t events; RetailFlightVector velocity;
                if (!(input>>id>>y>>velocity.x>>velocity.y>>velocity.z>>events>>orders) || orders<1 || orders>20000)
                    throw std::runtime_error("invalid flight state");
                auto* unit=world.unit(id);
                if (!unit || !unit->type->canFly ||
                    (!staticQueries && (unit->type->isBuilder || !unit->type->weapons.empty())))
                    throw std::runtime_error("unsupported flight patrol owner");
                unit->flightY=Fixed::raw(y); unit->flightVelocity=velocity;
                unit->missionEvents=events;
                unit->flightNavigation={{unit->x.v,y,unit->z.v},{},32768};
                for (int n=0;n<orders;++n) {
                    Order order; unsigned stage; uint32_t x,z;
                    if (!(input>>x>>z>>stage>>order.mission.waitMask>>order.mission.deadline>>
                            order.mission.pending>>order.mission.flags) || stage>2)
                        throw std::runtime_error("invalid flight mission");
                    order.x=Fixed::raw(std::bit_cast<int32_t>(x)); order.z=Fixed::raw(std::bit_cast<int32_t>(z));
                    order.mission.stage=uint8_t(stage); order.patrol=true; order.goal=true;
                    int present;
                    if (!(input>>present) || present<0 || present>1) throw std::runtime_error("invalid flight controller presence");
                    if (present) {
                        RetailFlightGoal goal; unsigned flags,heading; int radius;
                        if (!(input>>goal.point.x>>goal.point.y>>goal.point.z>>flags>>radius>>heading) ||
                            flags>65535 || heading>65535 || radius<-32768 || radius>32767)
                            throw std::runtime_error("invalid flight controller");
                        goal.flags=uint16_t(flags); goal.heading=uint16_t(heading); goal.radius=int16_t(radius);
                        order.flightGoal=goal;
                    }
                    unit->orders.push_back(order);
                }
            }
        }
        if (version>=11) {
            int count;
            if (!(input>>count) || count<0 || count>20000) throw std::runtime_error("invalid guard count");
            for (int i=0;i<count;++i) {
                int id; unsigned stage; RetailMissionState m; uint32_t events;
                if (!(input>>id>>stage>>m.waitMask>>m.deadline>>m.pending>>m.flags>>events) || stage>255)
                    throw std::runtime_error("invalid guard mission");
                auto* unit=world.unit(id);
                if (!unit || !unit->type->isStructure()) throw std::runtime_error("invalid guard owner");
                m.stage=uint8_t(stage); unit->guardNoMoveState=m; unit->missionEvents=events;
                unit->guardNoMoveAllowed=unit->guardNoMoveActive=true;
            }
        }
        if (version>=12) {
            int count;
            if (!(input>>count) || count<0 || count>20000) throw std::runtime_error("invalid script count");
            for (int i=0;i<count;++i) {
                int id; std::string hex,target;
                if (!(input>>id>>hex>>std::quoted(target))) throw std::runtime_error("invalid saved script");
                std::optional<std::array<unsigned,4>> values;
                if (version>=14) {
                    values.emplace();
                    for (auto& value:*values)
                        if (!(input>>value) || value>1) throw std::runtime_error("invalid factory unit value");
                }
                const auto* type=target.empty() ? nullptr : registry.find(target);
                if (!target.empty() && !type) throw std::runtime_error("missing factory output type");
                RetailReplayProbe::factory(world,id,hex,type,values);
                if (version>=29) {
                    uint32_t occupancy;
                    if (!(input>>occupancy) || occupancy>5) throw std::runtime_error("invalid script occupancy");
                    world.unit(id)->scriptOccupancy=occupancy;
                }
            }
        }
        if (version>=16) {
            int clocks;
            if (!(input>>clocks) || (clocks!=0 && clocks!=world.numPlayers()))
                throw std::runtime_error("invalid player cache count");
            for (int player=0;player<clocks;++player) {
                unsigned enabled; auto& clock=world.player(player).cacheClock;
                if (!(input>>enabled>>clock.lastRefresh) || enabled>1)
                    throw std::runtime_error("invalid player cache clock");
                clock.enabled=enabled!=0;
            }
        }
        if (version>=17) {
            int states;
            if (!(input>>states) || (states!=0 && states!=world.numPlayers()))
                throw std::runtime_error("invalid AI player count");
            for (int player=0;player<states;++player) {
                unsigned enabled;
                if (!(input>>enabled) || enabled>1) throw std::runtime_error("invalid AI presence");
                if (!enabled) continue;
                auto& ai=world.player(player).retailAi.emplace();
                unsigned initialized,count;
                if (!(input>>ai.schedule.assignmentCountdown>>initialized>>ai.scenarioDeadline>>count) || initialized>1 || count>100)
                    throw std::runtime_error("invalid AI schedule");
                ai.initialized=initialized!=0;
                std::set<int> members;
                for (unsigned n=0;n<count;++n) {
                    unsigned slot,kind,deadline,active,dirty,size;
                    if (!(input>>slot>>kind>>deadline>>active>>dirty) || slot>=100 || kind>4 || ai.schedule.groups[slot].present)
                        throw std::runtime_error("invalid AI squad");
                    auto& squad=ai.squads[slot];
                    ai.schedule.groups[slot]={true,deadline};squad.kind=RetailAiSquadKind(kind);squad.active=active;squad.dirty=dirty;
                    for (auto& parameter:squad.parameters)
                        if (!(input>>parameter)) throw std::runtime_error("missing AI parameter");
                    if (!(input>>size) || size>20000) throw std::runtime_error("invalid AI membership count");
                    for (unsigned m=0;m<size;++m) {
                        int id;
                        if (!(input>>id) || !world.unit(id) || world.unit(id)->player!=player || !members.insert(id).second)
                            throw std::runtime_error("invalid AI member");
                        squad.members.push_back(id);
                    }
                }
                if (version>=27) {
                    ai.anchorsRestored=true;
                    unsigned anchors;
                    if (!(input>>anchors) || anchors>131072) throw std::runtime_error("invalid AI anchor count");
                    for (unsigned i=0;i<anchors;++i) {
                        int reference,x,z;
                        if (!(input>>reference>>x>>z) || !reference || reference < -65536 || reference>65536 ||
                            x < -32768 || x>32767 || z < -32768 || z>32767 ||
                            !ai.anchors.emplace(reference,std::pair{int16_t(x),int16_t(z)}).second)
                            throw std::runtime_error("invalid AI anchor record");
                    }
                }
            }
        }
        if (version>=18) {
            int states;
            if (!(input>>states) || (states!=0 && states!=world.numPlayers()))
                throw std::runtime_error("invalid build-cache player count");
            for (int player=0;player<states;++player) {
                unsigned present;
                if (!(input>>present) || present>1) throw std::runtime_error("invalid build-cache presence");
                if (!present) continue;
                auto& owner=world.player(player);
                auto& cache=owner.buildCache.emplace();
                auto value=[&](auto& target) {
                    if (!(input>>target) || !std::isfinite(target) || target<0)
                        throw std::runtime_error("invalid resource history");
                };
                value(owner.mana); value(owner.storage);
                value(cache.resources.income); value(cache.resources.usage);
                for (auto& sample:cache.resources.samples) for (auto& v:sample) value(v);
                unsigned count;
                if (!(input>>count) || count>65535) throw std::runtime_error("invalid build catalogue size");
                std::set<const UnitType*> types;
                for (unsigned i=0;i<count;++i) {
                    RetailBuildCacheEntry entry;
                    std::string name;int weapon,total,completed,priority;
                    auto& p=entry.inputs;
                    if (!(input>>std::quoted(name)>>p.flags>>p.secondaryFlags>>weapon>>p.cost>>p.desired>>total>>completed>>priority) ||
                        weapon<-32768 || weapon>32767 || !std::isfinite(p.cost) || p.cost<0 || p.cost>1e9f ||
                        total<0 || total>32767 || completed<0 || completed>total || priority<0 || priority>100)
                        throw std::runtime_error("invalid build catalogue entry");
                    entry.type=registry.find(name);
                    if (!entry.type || !types.insert(entry.type).second) throw std::runtime_error("missing/duplicate build catalogue type");
                    int actual=0,ready=0;
                    for (int id:ids) {
                        const auto& unit=*world.unit(id);
                        if (unit.player==player && unit.type==entry.type) { ++actual;ready+=!unit.underConstruction; }
                    }
                    if (total!=actual || completed!=ready) throw std::runtime_error("build catalogue construction counts disagree");
                    p.weapon=int16_t(weapon);p.count=int16_t(total);p.completed=int16_t(completed);
                    entry.priority=uint8_t(priority);
                    if (version>=19) {
                        value(entry.income);value(entry.storage);entry.hasEconomy=true;
                    }
                    if (version>=23) {
                        auto& c=entry.construction.emplace();
                        value(c.type.inverseTime);value(c.workerTime);c.type.cost=p.cost;
                        if (!(input>>c.type.maxHp>>c.emitter.radius>>c.emitter.height>>c.emitter.capacity) ||
                            c.type.maxHp>32767 || c.emitter.radius<0 || c.emitter.height<0 || c.emitter.capacity>100000)
                            throw std::runtime_error("invalid catalogue construction metadata");
                    }
                    cache.entries.push_back(entry);
                }
                for (int id:ids) if (world.unit(id)->player==player && !types.contains(world.unit(id)->type))
                    throw std::runtime_error("unit missing from build catalogue");
            }
        }
        if (version>=20) {
            unsigned count;
            if (!(input>>count) || (count && count!=unsigned(world.numPlayers())))
                throw std::runtime_error("invalid resource accounting player count");
            for (unsigned i=0;i<count;++i) {
                unsigned present;
                if (!(input>>present) || present>1) throw std::runtime_error("invalid resource accounting presence");
                if (!present) continue;
                auto& owner=world.player(int(i));
                if (!owner.buildCache) throw std::runtime_error("resource history absent");
                auto& r=owner.retailResources.emplace();
                if (!(input>>r.allocation>>r.capacityOverride>>r.totalProduced>>r.excess) ||
                    !std::isfinite(r.allocation) || r.allocation<0 || !std::isfinite(r.capacityOverride) ||
                    !std::isfinite(r.totalProduced) || !std::isfinite(r.excess))
                    throw std::runtime_error("invalid resource accounting state");
                r.stored=float(owner.mana);r.capacity=owner.storage;
                r.produced=owner.buildCache->resources.income;r.requested=owner.buildCache->resources.usage;
            }
            if (!(input>>count) || count>ids.size()) throw std::runtime_error("invalid construction site count");
            for (unsigned i=0;i<count;++i) {
                int id;unsigned hp;RetailConstructionSite s;
                if (!(input>>id>>s.progress.remaining>>hp>>s.progress.events>>s.progress.flags>>
                        s.type.inverseTime>>s.type.cost>>s.type.maxHp) || hp>32767 || s.type.maxHp>32767 ||
                    !std::isfinite(s.progress.remaining) || s.progress.remaining<=0 || s.progress.remaining>1 ||
                    !std::isfinite(s.type.inverseTime) || s.type.inverseTime<0 ||
                    !std::isfinite(s.type.cost) || s.type.cost<0)
                    throw std::runtime_error("invalid construction site state");
                auto* u=world.unit(id);
                if (!u || !u->underConstruction || u->retailSite) throw std::runtime_error("invalid construction site owner");
                s.progress.hp=uint16_t(hp);u->retailSite=s;u->hp=Fixed::fromInt(int(hp));u->buildBegun=true;
            }
            if (!(input>>count) || count>ids.size()) throw std::runtime_error("invalid construction job count");
            for (unsigned i=0;i<count;++i) {
                int id;unsigned working;RetailConstructionJob job;
                if (!(input>>id>>job.target>>job.workerTime>>working>>job.activityDeadline>>
                        job.mission.waitMask>>job.mission.deadline>>job.mission.pending) || working>1 ||
                    !std::isfinite(job.workerTime) || job.workerTime<0)
                    throw std::runtime_error("invalid construction job");
                auto* u=world.unit(id);auto* site=world.unit(job.target);
                if (!u || !site || !site->retailSite || u->retailBuild || !u->orders.empty() ||
                    u->player!=site->player || !world.player(u->player).retailResources)
                    throw std::runtime_error("invalid construction job owner/target");
                job.working=working!=0;job.mission.stage=3;u->retailBuild=job;u->buildSiteId=job.target;
            }
        }
        if (version>=21) {
            unsigned count;
            if (!(input>>count) || count>ids.size()) throw std::runtime_error("invalid construction emitter count");
            for (unsigned i=0;i<count;++i) {
                int id;unsigned particles;RetailConstructionEmitter emitter;
                if (!(input>>id>>emitter.capacity>>emitter.radius>>emitter.height>>particles) ||
                    emitter.capacity>100000 || particles>emitter.capacity)
                    throw std::runtime_error("invalid construction emitter");
                auto* u=world.unit(id);
                if (!u || u->constructionEmitter) throw std::runtime_error("invalid emitter owner");
                for (unsigned n=0;n<particles;++n) {
                    RetailConstructionParticle p;
                    if (!(input>>p.x>>p.y>>p.z>>p.speed>>p.ceiling))
                        throw std::runtime_error("invalid construction particle");
                    emitter.particles.push_back(p);
                }
                u->constructionEmitter=std::move(emitter);
            }
        }
        if (version>=24) {
            unsigned count;
            if (!(input>>count) || count>ids.size()) throw std::runtime_error("invalid GetBuilt count");
            for (unsigned i=0;i<count;++i) {
                int id,builder;unsigned stage;RetailMissionState m;
                if (!(input>>id>>builder>>stage>>m.waitMask>>m.deadline>>m.pending) || stage>2)
                    throw std::runtime_error("invalid GetBuilt waiting state");
                auto* u=world.unit(id);
                if (!u || !u->retailSite || u->retailSite->mission || (builder && !world.unit(builder)))
                    throw std::runtime_error("invalid GetBuilt owner/builder");
                m.stage=uint8_t(stage);u->retailSite->mission=m;u->retailSite->builder=builder;
                u->missionEvents=u->retailSite->progress.events;
            }
        }
        if (version>=28) {
            unsigned players;
            if (!(input>>players) || (players && players!=unsigned(world.numPlayers())))
                throw std::runtime_error("invalid build planner player count");
            for (unsigned owner=0;owner<players;++owner) {
                unsigned present;
                if (!(input>>present) || present>1) throw std::runtime_error("invalid build planner presence");
                if (!present) continue;
                auto& cache=world.player(owner).buildCache;
                unsigned limited,population,count;
                if (!cache || !(input>>limited>>population>>count) || limited>1 || count!=cache->entries.size())
                    throw std::runtime_error("invalid build planner catalogue");
                unsigned actual=0;for (const auto& entry:cache->entries) actual+=unsigned(entry.inputs.count);
                if (population!=actual) throw std::runtime_error("build planner population disagrees");
                auto& planner=cache->planner.emplace(tak::sim::RetailBuildCache::Planner{});planner.limited=limited!=0;
                for (unsigned i=0;i<count;++i) {
                    RetailBuildCache::PlannerType type;
                    unsigned weight,special,size;
                    if (!(input>>std::quoted(type.faction)>>weight>>special>>size) ||
                        type.faction.size()>255 || weight>255 || special>1 || size>65535)
                        throw std::runtime_error("invalid build planner type");
                    type.weight=uint8_t(weight);type.special=special!=0;
                    for (unsigned j=0;j<size;++j) {
                        unsigned choice;
                        if (!(input>>choice) || !choice || choice>count)
                            throw std::runtime_error("invalid build planner menu reference");
                        type.choices.push_back(uint16_t(choice));
                    }
                    planner.types.push_back(std::move(type));
                }
            }
        }
        if (version>=30) {
            unsigned count;
            if (!(input>>count) || count>20000) throw std::runtime_error("invalid sector link count");
            for (unsigned i=0;i<count;++i) {
                int id,x,z;
                if (!(input>>id>>x>>z) || !world.unit(id) || x<0 || z<0 || x>=world.mapW()/8 || z>=world.mapH()/8)
                    throw std::runtime_error("invalid sector link");
                world.unit(id)->flightSectorX=x;world.unit(id)->flightSectorZ=z;
            }
        }
        if (version>=31) {
            unsigned count;
            if (!(input>>count) || count>ids.size()) throw std::runtime_error("invalid flying construction count");
            for (unsigned i=0;i<count;++i) {
                int id;unsigned working,stage,flags,heading;int radius;int32_t y;
                RetailConstructionJob job;RetailFlightVector velocity;RetailFlightGoal goal;uint32_t events;
                if (!(input>>id>>job.target>>job.workerTime>>working>>job.activityDeadline>>
                    job.mission.waitMask>>job.mission.deadline>>job.mission.pending>>stage>>
                    job.flyingOwnerFlags>>job.mission.flags>>events))
                    throw std::runtime_error("invalid flying construction mission");
                if (version<32) throw std::runtime_error("flying construction requires probe 32 movement thresholds");
                unsigned distance;
                if (!(input>>job.flyingSlowSpeed>>job.flyingFastSpeed>>distance) || distance>65535 ||
                    job.flyingSlowSpeed<0 || job.flyingFastSpeed<job.flyingSlowSpeed)
                    throw std::runtime_error("invalid flying movement thresholds");
                job.flyingBuildDistance=uint16_t(distance);
                if (!(input>>y>>velocity.x>>velocity.y>>velocity.z>>
                    goal.point.x>>goal.point.y>>goal.point.z>>flags>>radius>>heading) ||
                    working>1 || (stage!=5 && stage!=6) || flags>65535 || heading>65535 ||
                    radius<-32768 || radius>32767 || !std::isfinite(job.workerTime) || job.workerTime<0)
                    throw std::runtime_error("invalid flying construction state");
                auto* u=world.unit(id);auto* site=world.unit(job.target);
                if (!u || !u->type->canFly || !u->type->isBuilder || !site || !site->retailSite ||
                    u->retailBuild || !u->orders.empty() || u->player!=site->player)
                    throw std::runtime_error("invalid flying construction owner/target");
                job.flying=true;job.working=working!=0;job.mission.stage=uint8_t(stage);
                u->retailBuild=job;u->buildSiteId=job.target;u->missionEvents=events;
                RetailReplayProbe::movementRate(world,id,job.flyingOwnerFlags);
                u->flightY=Fixed::raw(y);u->flightVelocity=velocity;
                u->flightNavigation={{u->x.v,y,u->z.v},{},32768};
                goal.flags=uint16_t(flags);goal.radius=int16_t(radius);goal.heading=uint16_t(heading);
                Order order;order.x=Fixed::raw(goal.point.x);order.z=Fixed::raw(goal.point.z);
                order.goal=true;order.buildType=site->type;order.flightGoal=goal;u->orders.push_back(order);
            }
        }
        if (version>=35) {
            size_t count;
            if (!(input>>count) || count!=ids.size()) throw std::runtime_error("invalid surface height count");
            std::set<int> seen;
            for (size_t i=0;i<count;++i) {
                int id;int32_t y;uint32_t stamp;unsigned phase;
                if (!(input>>id>>y>>stamp>>phase) || !world.unit(id) || !seen.insert(id).second || phase>65535)
                    throw std::runtime_error("invalid surface height state");
                auto& u=*world.unit(id);
                if (!u.type->canFly) u.groundY=Fixed::raw(y);
                u.groundMoveTick=stamp;u.variationPhase=uint16_t(phase);
                if (version>=36) {
                    unsigned pitch,roll;
                    if (!(input>>pitch>>roll) || pitch>65535 || roll>65535)
                        throw std::runtime_error("invalid surface angles");
                    if (!u.type->canFly) { u.groundPitch=uint16_t(pitch);u.groundRoll=uint16_t(roll); }
                }
            }
        }
        if (version>=37) RetailReplayProbe::exploration(world,input);
        std::string extra;
        if (input >> extra) throw std::runtime_error("trailing diagnostic input");
        for (const Goal& goal : goals) {
            // Cold route reconstruction is one of the explicit limitations.
            world.order(goal.id, Fixed::raw(goal.x).toFloat(), Fixed::raw(goal.z).toFloat(), false);
            auto* unit = world.unit(goal.id);
            if (!unit || unit->orders.empty()) throw std::runtime_error("movement order was rejected");
            unit->orders.back().x = Fixed::raw(goal.x);
            unit->orders.back().z = Fixed::raw(goal.z);
            RetailReplayProbe::restoreRoute(world, *unit, goal.route, goal.stamp, goal.flags);
            auto& order = unit->orders.back();
            order.groundMission = goal.hasMission;
            order.mission = goal.mission;
            order.missionRadius = goal.radius;
            order.groundResponse = goal.response;
            if (goal.build.buildRectangle) {
                order.buildRectangle=goal.build.buildRectangle;
                order.buildType=goal.build.buildType;
                order.buildX=goal.build.buildX; order.buildZ=goal.build.buildZ;
                order.mission=goal.build.mission;
            }
            RetailReplayProbe::restoreRequest(world,*unit,goal.flags);
            unit->missionEvents = goal.events;
        }
        for (const auto& queued:queuedBuilds)
            RetailReplayProbe::queuedBuild(world,queued.id,queued.type,queued.x,queued.z);
        // Initial setup draws must not advance the observed starting RNG state.
        RetailReplayProbe::boundary(world, tick, rng,version>=22);
        struct CloseFile {void operator()(FILE* f) const {std::fclose(f);}};
        std::unique_ptr<FILE, CloseFile> output(std::fopen(argv[3], "wx"));
        if (!output) throw std::runtime_error("output must be a new writable file");
        if (argc==5 && std::string(argv[4])=="--feature-map") {
            if (steps!=0) throw std::runtime_error("feature map inspection requires zero steps");
            std::fprintf(output.get(),"%d %d\n",world.nav().width(),world.nav().height());
            for (const auto& cell:world.mapPlacementCells()) {
                const auto& types=world.mapPlacementTypes();
                const auto name=cell.feature<types.size()?types[cell.feature].name:std::to_string(cell.feature);
                std::fprintf(output.get(),"%s %u %u %u %u %u\n",name.c_str(),
                    unsigned(cell.backX),unsigned(cell.backZ),unsigned(cell.height),unsigned(cell.low),
                    unsigned(cell.feature<types.size() && types[cell.feature].clearable));
            }
            return 0;
        }
        std::fprintf(output.get(), "{\"kind\":\"metadata\",\"schema\":1,\"complete_state\":false,"
                     "\"rng_before\":%u,\"crusades\":%u,\"limitations\":[\"routes absent from older captures\",\"pending path-search state\",\"unsupported mission scheduling; outer-render CRT consumers; missing wind in older captures\","
                     "\"unsupported orders\",\"factory production lifecycle and yard occupancy\",\"saved features\","
                     "\"combat and economy defaults\",\"occupied AI planners, special group assignment and per-unit AI maintenance\","
                     "\"retail resource production, consumption and availability classification\",\"COB host queries and lifecycle notifications\",\"retired unit slot reuse and pool exhaustion\",\"movement timers and flags\"]}\n", rng,crusades);
        world.setRngObserver([&](const World::RngObservation& draw) {
            std::fprintf(output.get(), "{\"kind\":\"rng\",\"tick\":%u,\"bound\":%d,"
                         "\"seed_before\":%u,\"seed_after\":%u,\"result\":%u,\"source_line\":%u}\n",
                         draw.tick, draw.bound, draw.seedBefore, draw.seedAfter, draw.result, draw.caller.line());
        });
        world.setCrtRngObserver([&](const World::RngObservation& draw) {
            std::fprintf(output.get(),"{\"kind\":\"crt\",\"tick\":%u,\"return_address\":%u,"
                "\"seed_before\":%u,\"seed_after\":%u,\"result\":%u}\n",
                draw.tick,draw.retailReturnAddress,draw.seedBefore,draw.seedAfter,draw.result);
        });
        if (argc==6 && std::string(argv[4])=="--corpse-features") {
            if (steps!=0) throw std::runtime_error("corpse feature inspection requires zero steps");
            std::ifstream queries(argv[5]);
            if (!queries) throw std::runtime_error("corpse events unavailable");
            std::map<int,std::pair<Fixed,Fixed>> positions;
            for (const auto& unit:world.units()) positions[unit.id]={unit.x,unit.z};
            int id,mode;
            while (queries>>id) {
                if (!(queries>>mode) || !positions.contains(id)) throw std::runtime_error("invalid corpse event");
                const auto before=world.mapPlacementCells();
                auto* unit=world.unit(id);
                if (mode!=3) { unit->x=positions.at(id).first;unit->z=positions.at(id).second; }
                const bool placed=RetailReplayProbe::corpse(world,id,mode);
                std::fprintf(output.get(),"{\"kind\":\"corpse_feature\",\"id\":%d,\"placed\":%u,\"cells\":[",id,unsigned(placed));
                bool comma=false;
                for (size_t i=0;i<before.size();++i) {
                    const auto& after=world.mapPlacementCells()[i];
                    if (before[i].feature==after.feature && (after.feature!=0xfffe ||
                        (before[i].backX==after.backX && before[i].backZ==after.backZ))) continue;
                    const auto& types=world.mapPlacementTypes();
                    const auto name=after.feature<types.size()?types[after.feature].name:std::to_string(after.feature);
                    std::fprintf(output.get(),"%s[%zu,\"%s\",%u,%u]",comma?",":"",i,name.c_str(),
                        after.feature==0xfffe?unsigned(after.backX):0,after.feature==0xfffe?unsigned(after.backZ):0);
                    comma=true;
                }
                std::fprintf(output.get(),"]}\n");
            }
            if (!queries.eof()) throw std::runtime_error("malformed corpse event");
        }
        if (argc==6 && std::string(argv[4])=="--search-movement") {
            std::ifstream queries(argv[5]);int id,budget,rounds,first,count;int32_t gx,gz;
            std::vector<Unit*> movers;int runRounds=0,runBudget=0;
            while (queries>>id) {
                if (!(queries>>budget>>rounds>>gx>>gz>>first>>count) || rounds<1 || rounds>5000 || budget<1 ||
                    first<0 || count<1 || id<first || int64_t(id)>=int64_t(first)+count ||
                    (!movers.empty() && (rounds!=runRounds || budget!=runBudget || id<=movers.back()->id)))
                    throw std::runtime_error("invalid search/movement fixture");
                auto* u=world.unit(id);
                if (!u || u->type->canFly || u->type->isStructure()) throw std::runtime_error("invalid search/movement unit");
                RetailReplayProbe::startSearchMovement(world,*u,gx,gz,budget,first,count,movers.empty());
                movers.push_back(u);runRounds=rounds;runBudget=budget;
            }
            if (movers.empty() || !queries.eof()) throw std::runtime_error("invalid search/movement fixture rows");
            for (int n=1;n<=runRounds;++n) {
                for (auto* u:movers) RetailReplayProbe::searchMovementTick(world,*u,tick+uint32_t(n),false);
                RetailReplayProbe::searchWorkerTick(world,tick+uint32_t(n));
                for (const auto* u:movers) RetailReplayProbe::searchMovementState(output.get(),world,*u);
            }
        }
        if (argc==6 && std::string(argv[4])=="--ground-navigation-steps") {
            std::ifstream queries(argv[5]);
            if (!queries) throw std::runtime_error("navigation steps unavailable");
            int id;uint32_t tick,clock;unsigned count;
            while (queries>>id>>tick>>clock>>count) {
                if (count==1 || count>64) throw std::runtime_error("invalid initial route size");
                std::vector<RetailSteeringPoint> points(count);
                for (auto& point:points) if (!(queries>>point.x.v>>point.z.v))
                    throw std::runtime_error("incomplete navigation route");
                auto* u=world.unit(id);
                if (!u || u->type->canFly || u->type->isStructure()) throw std::runtime_error("invalid navigation step");
                if (!tick) {
                    if (!count) throw std::runtime_error("initial navigation route is missing");
                    RetailReplayProbe::navigationRoute(world,*u,points,clock);continue;
                }
                RetailReplayProbe::movementTick(world,tick);
                if (count) RetailReplayProbe::navigationRoute(world,*u,points,tick-1);
                const auto x=u->x,z=u->z;const auto heading=u->heading;
                world.updateGroundTerrainFlags(*u);world.tickNavigationMovement(*u,u->baseSpeed);
                if (u->x!=x || u->z!=z || u->heading!=heading || u->type->canHover)
                    u->groundY=world.surfaceHeight(*u,clock,&u->groundPitch,&u->groundRoll);
                std::fprintf(output.get(),"{\"kind\":\"ground_navigation_step\",\"result\":[%d,%d,%d,%d,%u,%u,%u,%u,%d,%u,%zu,%u,%u,%u,%u]}\n",
                    u->x.v,u->groundY.v,u->z.v,u->speed.v,unsigned(portHeadingToRetail(u->heading)),
                    unsigned(u->groundPitch),unsigned(u->groundRoll),unsigned(u->groundTerrainFlags),
                    u->bodyBlockStreak,u->groundMoveTick,RetailReplayProbe::navigationPoints(world,*u),
                    unsigned(u->groundMovementMode),u->groundScanTick,RetailReplayProbe::movementSeed(world),unsigned(u->groundSpeedMode));
            }
            if (!queries.eof()) throw std::runtime_error("malformed navigation step");
        }
        if (argc==6 && std::string(argv[4])=="--ground-travel-steps") {
            std::ifstream queries(argv[5]);
            if (!queries) throw std::runtime_error("travel steps unavailable");
            int id;uint32_t tick,clock;unsigned mode;std::array<RetailSteeringPoint,3> points;
            while (queries>>id>>tick>>clock>>mode) {
                for (auto& point:points) if (!(queries>>point.x.v>>point.z.v))
                    throw std::runtime_error("incomplete travel segment");
                auto* u=world.unit(id);
                if (!u || u->type->canFly || u->type->isStructure() || mode>7)
                    throw std::runtime_error("invalid travel step");
                RetailReplayProbe::movementTick(world,tick);u->groundMovementMode=uint8_t(mode);
                const auto x=u->x,z=u->z;const auto heading=u->heading;
                world.updateGroundTerrainFlags(*u);
                const auto step=world.steerGround(*u,points[0],points[1],points[2],u->baseSpeed);
                world.commitGroundStep(*u,step.s,step.c);
                if (u->x!=x || u->z!=z || u->heading!=heading || u->type->canHover)
                    u->groundY=world.surfaceHeight(*u,clock,&u->groundPitch,&u->groundRoll);
                std::fprintf(output.get(),"{\"kind\":\"ground_travel_step\",\"result\":[%d,%d,%d,%d,%u,%u,%u,%u,%d,%u]}\n",
                    u->x.v,u->groundY.v,u->z.v,u->speed.v,unsigned(portHeadingToRetail(u->heading)),
                    unsigned(u->groundPitch),unsigned(u->groundRoll),unsigned(u->groundTerrainFlags),
                    u->bodyBlockStreak,u->groundMoveTick);
            }
            if (!queries.eof()) throw std::runtime_error("malformed travel step");
        }
        if (argc==6 && std::string(argv[4])=="--ground-brake-steps") {
            std::ifstream queries(argv[5]);
            if (!queries) throw std::runtime_error("braking steps unavailable");
            int id;uint32_t tick,clock;int32_t speed,facing;
            while (queries>>id>>tick>>speed>>clock>>facing) {
                auto* u=world.unit(id);
                if (!u || u->type->canFly || u->type->isStructure() || speed< -1 || facing< -1 || facing>65535)
                    throw std::runtime_error("invalid braking step");
                RetailReplayProbe::movementTick(world,tick);
                if (speed>=0) u->speed=Fixed::raw(speed);
                const auto x=u->x,z=u->z;const auto heading=u->heading;
                world.updateGroundTerrainFlags(*u);
                world.brakeGround(*u,facing<0?std::nullopt:std::optional<Bam>(retailHeadingToPort(uint16_t(facing))));
                if (u->x!=x || u->z!=z || u->heading!=heading || u->type->canHover)
                    u->groundY=world.surfaceHeight(*u,clock,&u->groundPitch,&u->groundRoll);
                std::fprintf(output.get(),"{\"kind\":\"ground_brake_step\",\"result\":[%d,%d,%d,%d,%u,%u,%u,%u,%d,%u]}\n",
                    u->x.v,u->groundY.v,u->z.v,u->speed.v,unsigned(portHeadingToRetail(u->heading)),
                    unsigned(u->groundPitch),unsigned(u->groundRoll),unsigned(u->groundTerrainFlags),
                    u->bodyBlockStreak,u->groundMoveTick);
            }
            if (!queries.eof()) throw std::runtime_error("malformed braking step");
        }
        if (argc==6 && std::string(argv[4])=="--ground-steps") {
            std::ifstream queries(argv[5]);
            if (!queries) throw std::runtime_error("ground steps unavailable");
            int id;int32_t dx,dz;
            while (queries>>id>>dx>>dz) {
                auto* u=world.unit(id);
                if (!u || u->type->canFly || u->type->isStructure()) throw std::runtime_error("invalid ground step");
                world.commitGroundStep(*u,Fixed::raw(dx),Fixed::raw(dz));
                std::fprintf(output.get(),"{\"kind\":\"ground_step\",\"result\":[%d,%d,%d,%d]}\n",
                    u->x.v,u->z.v,u->speed.v,u->bodyBlockStreak);
            }
            if (!queries.eof()) throw std::runtime_error("malformed ground step");
        }
        if (argc==6 && std::string(argv[4])=="--ground-height-queries") {
            std::ifstream queries(argv[5]);
            if (!queries) throw std::runtime_error("height queries unavailable");
            int id;int32_t x,z,y;unsigned heading,oldPitch,oldRoll;uint32_t clock;
            while (queries>>id>>x>>z>>y>>heading>>clock>>oldPitch>>oldRoll) {
                const auto* subject=world.unit(id);
                if (!subject || subject->type->canFly || heading>65535 || oldPitch>65535 || oldRoll>65535)
                    throw std::runtime_error("invalid height query");
                auto u=*subject;u.x=Fixed::raw(x);u.z=Fixed::raw(z);u.groundY=Fixed::raw(y);
                u.heading=retailHeadingToPort(uint16_t(heading));
                uint16_t pitch=uint16_t(oldPitch),roll=uint16_t(oldRoll);
                const auto height=world.surfaceHeight(u,clock,&pitch,&roll);
                std::fprintf(output.get(),"{\"kind\":\"ground_height\",\"result\":%d,\"pitch\":%u,\"roll\":%u}\n",
                    height.v,unsigned(pitch),unsigned(roll));
            }
            if (!queries.eof()) throw std::runtime_error("malformed height query");
        }
        if (argc==6 && std::string(argv[4])=="--placement-queries") {
            std::ifstream queries(argv[5]);
            if (!queries) throw std::runtime_error("placement queries unavailable");
            int id,x,z,moving;
            while (queries>>id>>x>>z>>moving) {
                const auto* subject=world.unit(id);
                if (!subject || (moving!=0 && moving!=1)) throw std::runtime_error("invalid placement query");
                std::fprintf(output.get(),"{\"kind\":\"placement\",\"result\":%u}\n",
                             unsigned(world.mobilePlacement(*subject,x,z,moving!=0)));
            }
            if (!queries.eof()) throw std::runtime_error("malformed placement query");
        }
        if (argc==6 && std::string(argv[4])=="--live-grade-queries") {
            std::ifstream queries(argv[5]);
            if (!queries) throw std::runtime_error("live grade queries unavailable");
            int id,x,z;
            while (queries>>id) {
                if (!(queries>>x>>z)) throw std::runtime_error("malformed live grade query");
                const auto* subject=world.unit(id);
                if (!subject) throw std::runtime_error("invalid live grade query");
                std::fprintf(output.get(),"{\"kind\":\"live_grade\",\"result\":%d}\n",
                    RetailReplayProbe::score(world,*subject,x,z));
            }
            if (!queries.eof()) throw std::runtime_error("malformed live grade query");
        }
        if (argc==6 && std::string(argv[4])=="--raw-grade-queries") {
            std::ifstream queries(argv[5]);
            if (!queries) throw std::runtime_error("raw grade queries unavailable");
            int id,x,z,width,height,requester;uint32_t recent,stale;
            while (queries>>id) {
                if (!(queries>>x>>z>>width>>height>>requester>>recent>>stale))
                    throw std::runtime_error("malformed raw grade query");
                const auto* subject=world.unit(id);
                if (!subject || width<=0 || height<=0) throw std::runtime_error("invalid raw grade query");
                std::fprintf(output.get(),"{\"kind\":\"raw_grade\",\"result\":%d}\n",
                    RetailReplayProbe::rawGrade(world,*subject,x,z,width,height,requester,recent,stale));
            }
            if (!queries.eof()) throw std::runtime_error("malformed raw grade query");
        }
        if (argc==6 && std::string(argv[4])=="--grades-unit") {
            const auto* subject=world.unit(std::stoi(argv[5]));
            if (!subject) throw std::runtime_error("grade-dump unit is absent");
            const auto& grid=world.navFor(subject->type);
            std::fprintf(output.get(),"{\"kind\":\"initial_search_grades\",\"id\":%d,\"width\":%d,\"height\":%d,\"grades\":[",
                         subject->id,grid.width(),grid.height());
            for (int z=0;z<grid.height();++z)
                for (int x=0;x<grid.width();++x)
                    std::fprintf(output.get(),"%s%d",x || z ? "," : "",
                        RetailReplayProbe::score(world,*subject,x+subject->type->footX/2,z+subject->type->footZ/2));
            std::fprintf(output.get(),"]}\n");
        }
        frame(output.get(), world);
        for (int i = 0; i < steps; ++i) {
            world.tick(1.0f / 30.0f);
            frame(output.get(), world);
        }
        if (std::fflush(output.get()) != 0 || std::ferror(output.get()))
            throw std::runtime_error("failed writing diagnostic output");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "retail_replay_probe: %s\n", error.what());
        return 1;
    }
}
