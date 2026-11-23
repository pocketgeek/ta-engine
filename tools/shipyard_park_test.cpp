// Factory output release and AI Navy Yard siting, on shipped units and the map
// from a 0.9.0 report: a Creon Normal AI put its Navy Yard (crenavy) in a
// landlocked 111-cell pond on a generated map, at (1800,5272). The yard's exit
// step then ordered each new Submersible (cresubm) 316 px south, into the next
// body of water; the sub never moved, held the pad, and the yard built nothing
// for 3m46s until it was killed.
//
// Retail releases a factory's output with GetBuilt (0x402220): the factory's
// queued QMove/QPatrol orders, else PARK in a ring around it. Our release ports
// that, keeping the ring only when the output can reach it (see
// World::parkProductionOutput). The AI's coastal projection must also refuse
// the pond: its first wet ring around an enemy start is the sea, not the pond.
//
// Flyers take the same release: the factory's rally alone, else PARK's flight
// branch, a single flight to a random point on a circle round the factory.
//
// Under Legion a run of ground outputs spreads over PARK's band (each claims
// a spot of its own, World::legionParkSpot) instead of queueing in a column
// out of the door; tools/legion_park_test.cpp covers it in depth.
#include "ai/ai.h"
#include "ai/builtinprofile.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "tnt/mapgen.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace tak::ai {
struct PlacementProbe {
    static bool reach(const Controller& c,const sim::World& w,const sim::UnitType* ship,
                      float fx,float fz,float tx,float tz) {
        float x,z;return c.reachablePoint(w,ship,fx,fz,tx,tz,x,z);
    }
    static bool body(const Controller& c,const sim::World& w,const sim::UnitType* ship,float x,float z) {
        return c.navalBodyAtLeast(w,ship,x,z,Controller::kMinNavalBodyCells);
    }
};
}

using namespace tak;
namespace {
int failures=0,cases=0;
void check(bool ok,const std::string& label) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",label.c_str());++cases;if(!ok)++failures;
}
struct Rect {int x,z,w,h;};
Rect footprint(const sim::Unit& u) {
    return {sim::footprintOrigin(u.x,u.type->footX),sim::footprintOrigin(u.z,u.type->footZ),u.type->footX,u.type->footZ};
}
bool intersects(const Rect& a,const Rect& b) {return a.x<b.x+b.w && b.x<a.x+a.w && a.z<b.z+b.h && b.z<a.z+a.h;}

// The reported room: generated map, 8 seats; the start spots it used.
constexpr auto kRecipe="~gen1~0a000100000000000000038001800108ffffffffff0004000000";
const std::vector<std::pair<float,float>> kStarts{{4352,1792},{5376,3328},{4352,4352},{3328,5376},
    {1792,4352},{768,3328},{1792,1792},{3328,768}};

void setupPondMap(sim::World& w,const sim::TypeRegistry& registry,hpi::Vfs& vfs,sim::PathfindingMode mode) {
    w.setSerialThreads(true);
    sim::MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath="maps/shipyard-pond.tnt";cfg.loadCrt=false;
    cfg.pathfindingMode=mode;cfg.slots.resize(1);sim::setupMatch(w,registry,cfg);
}

// A Navy Yard on the pond, building Submersibles. Each must leave the pad and
// the yard must go on to the next.
void pondYard(const sim::TypeRegistry& registry,hpi::Vfs& vfs,sim::PathfindingMode mode,bool crusades) {
    sim::World w;setupPondMap(w,registry,vfs,mode);
    const auto* sub=registry.find("cresubm");
    auto yardType=*registry.find("crenavy");yardType.workerTime*=20;yardType.storage=1000000;
    const int yard=w.spawn(&yardType,1800,5272,0,0);
    w.player(0).mana=1000000;w.train(yard,sub,3);
    const Rect pad=footprint(*w.unit(yard));
    std::vector<int> subs;int firstDone=-1,firstClear=-1,tick=0;
    for(;tick<12000;++tick) {
        w.player(0).mana=1000000;w.tick(1.f/30);
        for(const auto& u:w.units())
            if(u.alive() && u.type==sub && std::find(subs.begin(),subs.end(),u.id)==subs.end())subs.push_back(u.id);
        if(!subs.empty()) {
            const auto* first=w.unit(subs.front());
            if(first && !first->underConstruction) {
                if(firstDone<0)firstDone=tick;
                if(firstClear<0 && !intersects(footprint(*first),pad))firstClear=tick;
            }
        }
        if(w.unit(yard)->buildQueue.empty() && firstClear>=0)break;
    }
    for(int settle=0;settle<900;++settle)w.tick(1.f/30);
    int finished=0,onPad=0;
    for(int id:subs)if(const auto* u=w.unit(id);u && u->alive() && !u->underConstruction) {
        ++finished;onPad+=intersects(footprint(*u),pad);
    }
    std::printf("pond balance=%s mode=%s subs=%zu finished=%d firstClearAfter=%d onPad=%d ticks=%d hash=%016llx\n",
        crusades?"Crusades":"standard",mode==sim::PathfindingMode::Legion?"Legion":"Retail",subs.size(),finished,
        firstClear<0?-1:firstClear-firstDone,onPad,tick,(unsigned long long)w.stateHash());
    check(firstClear>=0 && firstClear-firstDone<=600,"a Submersible built on the pond yard leaves the pad");
    check(finished==3 && onPad==0,"the pond yard builds its next Submersibles, none left on the pad");
}

// The AI's view of the same pond: no enemy start projects onto it, it is too
// small for a fleet, and the open sea a Creon yard used in that game still is.
void pondSiting(const sim::TypeRegistry& registry,hpi::Vfs& vfs) {
    sim::World w;setupPondMap(w,registry,vfs,sim::PathfindingMode::Retail);
    const auto* sub=registry.find("cresubm");
    const float padX=1802,padZ=5298;
    check(!w.pathExists(sub,1872,5616,padX,padZ),"fixture: the pond is not connected to the sea south of it");
    std::vector<std::pair<float,float>> enemies(kStarts.begin(),kStarts.end());
    enemies.erase(enemies.begin()+4);   // seat 4 placed the yard
    ai::Controller controller(4,registry,ai::builtinProfile(ai::Difficulty::Normal),1,ai::Difficulty::Normal,enemies);
    int accepted=0;
    for(const auto& [x,z]:enemies)accepted+=ai::PlacementProbe::reach(controller,w,sub,padX,padZ,x,z);
    check(accepted==0,"no enemy start's coastal projection lands in the landlocked pond");
    check(!ai::PlacementProbe::body(controller,w,sub,padX,padZ),"a 111-cell pond is too small a body for a Navy Yard");
    std::vector<std::pair<float,float>> west(kStarts.begin(),kStarts.end());
    west.erase(west.begin()+5);
    ai::Controller seat5(5,registry,ai::builtinProfile(ai::Difficulty::Normal),1,ai::Difficulty::Normal,west);
    int seaAccepted=0;
    for(const auto& [x,z]:west)seaAccepted+=ai::PlacementProbe::reach(seat5,w,sub,538,2194,x,z);
    check(seaAccepted>0 && ai::PlacementProbe::body(seat5,w,sub,538,2194),
          "the west sea a Creon yard used in the same game is still accepted");
}

// A land factory parks its output in front of it, inside PARK's ring; a rally
// point still wins over PARK.
void landFactory(const sim::TypeRegistry& registry,sim::PathfindingMode mode,bool rally,int count=4) {
    sim::World w;w.setSerialThreads(true);w.setPathfindingMode(mode);
    w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    w.setMapPlacementFeatures(std::vector<uint16_t>(256*256,0xffff),{});
    w.buildNavClasses(registry);w.setPathService(true);
    auto barracks=*registry.find("arakeep");barracks.workerTime*=20;barracks.storage=1000000;
    const auto* sword=registry.find("arasword");
    const int id=w.spawn(&barracks,1800,1800,0,0);
    if(rally)w.order(id,2600,1000,false);
    w.player(0).mana=1000000;w.train(id,sword,count);
    const Rect yard=footprint(*w.unit(id));
    std::vector<int> out;bool parked=true,rallied=true;
    for(int tick=0;tick<6000;++tick) {
        w.player(0).mana=1000000;w.tick(1.f/30);
        for(const auto& u:w.units())
            if(u.alive() && u.type==sword && !u.underConstruction && std::find(out.begin(),out.end(),u.id)==out.end()) {
                out.push_back(u.id);
                // Its release, the tick it is finished.
                const bool park=std::any_of(u.orders.begin(),u.orders.end(),[](const auto& o){return bool(o.park);});
                parked=parked && park;
                rallied=rallied && !park && !u.orders.empty();
            }
        if(int(out.size())==count && std::all_of(out.begin(),out.end(),[&](int u){return w.unit(u)->orders.empty();}))break;
    }
    int front=0,inRing=0,beyond=0,inside=0,arrived=0,inYard=0,left=0,right=0;
    float minAngle=10,maxAngle=-10;
    const auto* f=w.unit(id);
    const int fx=sim::footprintOrigin(f->x,sword->footX),fz=sim::footprintOrigin(f->z,sword->footZ);
    for(int u:out) {
        const auto* s=w.unit(u);
        inYard+=intersects(footprint(*s),yard);
        const float dx=s->x.toFloat()-f->x.toFloat(),dz=s->z.toFloat()-f->z.toFloat();
        front+=dz>0 && std::abs(dx)<dz;
        // PARK's ring: footX*16 + padding 48..160 + half the Barracks diagonal.
        const int cx=sim::footprintOrigin(s->x,sword->footX)-fx,cz=sim::footprintOrigin(s->z,sword->footZ)-fz;
        const int cells2=cx*cx+cz*cz;
        const int half=int(std::sqrt(double(barracks.footX*barracks.footX+barracks.footZ*barracks.footZ))*8);
        const int inner=sword->footX*16+48+half,outer=3*(sword->footX*16+160+half);
        inRing+=cells2*256>=inner*inner && cells2*256<=outer*outer;
        beyond+=cells2*256>outer*outer;inside+=cells2*256<inner*inner;
        // Bearing off the door (south), and its side.
        const float angle=std::atan2(dx,dz);
        minAngle=std::min(minAngle,angle);maxAngle=std::max(maxAngle,angle);
        (dx<0?left:right)++;
        const float rx=s->x.toFloat()-2600,rz=s->z.toFloat()-1000;
        arrived+=rx*rx+rz*rz<160*160;
    }
    const std::string label=std::string(mode==sim::PathfindingMode::Legion?"Legion: ":"Retail: ");
    std::printf("land mode=%s rally=%d outputs=%zu parked=%d rallied=%d front=%d inRing=%d beyond=%d inside=%d "
                "left=%d right=%d spread=%.2f arrived=%d inYard=%d hash=%016llx\n",
        mode==sim::PathfindingMode::Legion?"Legion":"Retail",rally,out.size(),parked,rallied,front,inRing,beyond,inside,
        left,right,out.empty()?0.f:maxAngle-minAngle,arrived,inYard,(unsigned long long)w.stateHash());
    if(rally) {
        check(out.size()==4 && rallied && arrived==4 && inYard==0,label+"a Barracks with a rally point sends its output there, not to PARK");
    } else if(count==4) {
        check(out.size()==4 && parked,label+"a Barracks without a rally point releases its output with PARK");
        // Legion spreads the outputs round the door (some beside the
        // factory), so only the Retail navigator is held to its front.
        if(mode==sim::PathfindingMode::Legion)
            check(inRing==4 && inYard==0,label+"the parked output stands in PARK's ring, clear of the Barracks");
        else check(front==4 && inRing==4 && inYard==0,label+"the parked output stands in front of the Barracks, in PARK's ring");
    } else {
        // Legion spreads a run of outputs over the band, or beyond it once
        // the band is full (World::legionParkSpot); they used to queue in a
        // column out of the door, three of eight short of the inner radius.
        check(int(out.size())==count && parked && inYard==0,label+"eight parked outputs leave the Barracks");
        check(inRing+beyond==count && inside==0,label+"all eight stand in PARK's band or beyond it, none short of its inner radius");
        check(left>0 && right>0 && maxAngle-minAngle>=1.0f,label+"the eight spread round the door, not in a column");
    }
}

// A rally whose only entry is an attack on a unit that has since died hands the
// output nothing; it must still park rather than stand on the pad.
void deadRally(const sim::TypeRegistry& registry,sim::PathfindingMode mode) {
    sim::World w;w.setSerialThreads(true);w.setPathfindingMode(mode);
    w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    w.setMapPlacementFeatures(std::vector<uint16_t>(256*256,0xffff),{});
    w.buildNavClasses(registry);w.setPathService(true);
    auto barracks=*registry.find("arakeep");barracks.workerTime*=20;barracks.storage=1000000;
    const auto* sword=registry.find("arasword");
    const int id=w.spawn(&barracks,1800,1800,0,0);
    const int victim=w.spawn(sword,2600,1000,1,0);
    w.attack(id,victim,false);
    const bool hasRally=!w.unit(id)->rally.empty();
    w.destroy(victim);   // self-destruct: two seconds, then gone
    for(int tick=0;tick<300 && w.unit(victim) && w.unit(victim)->alive();++tick)w.tick(1.f/30);
    const bool gone=!w.unit(victim) || !w.unit(victim)->alive();
    w.player(0).mana=1000000;w.train(id,sword,1);
    int out=0;bool parked=false;
    for(int tick=0;tick<3000 && !out;++tick) {
        w.player(0).mana=1000000;w.tick(1.f/30);
        for(const auto& u:w.units())
            if(u.alive() && u.type==sword && u.player==0 && !u.underConstruction) {
                out=u.id;parked=std::any_of(u.orders.begin(),u.orders.end(),[](const auto& o){return bool(o.park);});
            }
    }
    check(hasRally && gone && out && parked,std::string(mode==sim::PathfindingMode::Legion?"Legion: ":"Retail: ")+
          "a rally of only a dead attack target still parks the output");
}

// ---- Flyers -----------------------------------------------------------------
// Retail's GetBuilt copies a factory's rally to a flyer as to any output, and
// otherwise gives it PARK, whose flight branch (407ca0, 0x407e5a..0x407f62)
// flies it to ONE point at footX*16 + padding + half the factory's diagonal
// from the factory, at a random angle; padding is (rand(6)+rand(3)+6)*16. A
// flyer used to get a 60 px step south instead, with the rally behind it.
void flatWorld(sim::World& w,const sim::TypeRegistry& registry,sim::PathfindingMode mode) {
    w.setSerialThreads(true);w.setPathfindingMode(mode);
    w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    w.setMapPlacementFeatures(std::vector<uint16_t>(256*256,0xffff),{});
    w.buildNavClasses(registry);w.setPathService(true);
}
// What a flyer's order holds is VTOL_Move's stage 1 cell snap of the park's
// point (footprintWaypoint: up to 16 px per axis), plus the sine table's
// rounding, under a pixel.
constexpr float kSnap=16*1.4143f+1;
int halfDiagonal(const sim::UnitType& f) {
    return int(std::floor(std::sqrt(double(f.footX*f.footX+f.footZ*f.footZ))*16))/2;
}
std::string modeLabel(sim::PathfindingMode mode) {return mode==sim::PathfindingMode::Legion?"Legion: ":"Retail: ";}

void flyerFactory(const sim::TypeRegistry& registry,sim::PathfindingMode mode,const char* factoryId,const char* flyerId,
                  bool rally) {
    const auto* flyer=registry.find(flyerId);const auto* shipped=registry.find(factoryId);
    const std::string label=modeLabel(mode)+factoryId+"/"+flyerId+": ";
    if(!flyer || !shipped || !flyer->canFly || !shipped->isStructure()) {check(false,label+"fixture: a factory and a flyer");return;}
    sim::World w;flatWorld(w,registry,mode);
    auto factory=*shipped;factory.workerTime*=40;factory.storage=1000000;
    constexpr float kFx=2000,kFz=2000,kRx=3000,kRz=1200;
    const int id=w.spawn(&factory,kFx,kFz,0,0);
    if(rally)w.order(id,kRx,kRz,false);
    const int count=rally?3:12;
    w.player(0).mana=1000000;w.train(id,flyer,count);
    struct Out {int id;bool seen=false,resolved=false;size_t legs=0;float x=0,z=0;};
    // Retail's PARK lasts until its flight arrives: the leg keeps its park on
    // the way, so the AI still sees a parked newborn it is free to command
    // (the landing that follows arrival is not the park's).
    int unparkedFlight=0;
    std::vector<Out> out;
    int tick=0;
    for(;tick<20000;++tick) {
        w.player(0).mana=1000000;w.tick(1.f/30);
        for(const auto& u:w.units()) {
            if(!u.alive() || u.type!=flyer || u.underConstruction)continue;
            auto it=std::find_if(out.begin(),out.end(),[&](const Out& o){return o.id==u.id;});
            if(it==out.end()) {out.push_back({u.id});it=out.end()-1;}
            // The order it was released with, once its park (if any) has its point (not a step-out of the
            // newborns' stack, which leads its own order for a while: retail's air traffic).
            if(!it->resolved && !u.orders.empty() && !u.orders.front().stepOut &&
               (!u.orders.front().park || u.orders.front().park->attempts)) {
                it->resolved=true;it->legs=u.orders.size();
                it->x=u.orders.front().x.toFloat();it->z=u.orders.front().z.toFloat();
            }
            else if(it->resolved && !rally && !u.orders.empty() && u.orders.front().flightMoveMission &&
                    !u.orders.front().park)++unparkedFlight;   // still flying to the point, not landing there
            it->seen=true;
        }
        if(int(out.size())==count && w.unit(id)->buildQueue.empty())break;
    }
    for(int settle=0;settle<1800;++settle)w.tick(1.f/30);
    const auto* f=w.unit(id);
    const Rect yard=footprint(*f);
    const int inner=flyer->footX*16+96+halfDiagonal(factory),outer=flyer->footX*16+208+halfDiagonal(factory);
    int inRadius=0,straight=0,onPad=0,resolved=0;unsigned quadrants=0;bool behind=false;
    float minR=1e9f,maxR=0;
    for(const auto& o:out) {
        resolved+=o.resolved;
        if(const auto* u=w.unit(o.id);u && u->alive())onPad+=intersects(footprint(*u),yard);
        if(!o.resolved)continue;
        const float dx=o.x-f->x.toFloat(),dz=o.z-f->z.toFloat(),r=std::sqrt(dx*dx+dz*dz);
        minR=std::min(minR,r);maxR=std::max(maxR,r);
        inRadius+=r>=float(inner)-kSnap && r<=float(outer)+kSnap;
        quadrants|=1u<<((dx>=0?1:0)|(dz>=0?2:0));behind=behind || dz<0;
        // A rally step is the output's VTOL_Move (GetBuilt resolves QMove for
        // it), whose stage 1 snaps the point to its footprint grid.
        straight+=o.legs==1 && std::abs(o.x-kRx)<=16 && std::abs(o.z-kRz)<=16;
    }
    std::printf("flyer mode=%s factory=%s flyer=%s rally=%d outputs=%zu resolved=%d r=[%.1f,%.1f] want=[%d,%d] "
                "inRadius=%d quadrants=%x straight=%d onPad=%d ticks=%d hash=%016llx\n",
        mode==sim::PathfindingMode::Legion?"Legion":"Retail",factoryId,flyerId,rally,out.size(),resolved,
        minR,maxR,inner,outer,inRadius,quadrants,straight,onPad,tick,(unsigned long long)w.stateHash());
    check(int(out.size())==count && onPad==0,label+"every flyer is built and none is left on the factory pad");
    if(rally) {
        check(resolved==count && straight==count,label+"a rally flyer flies straight to the rally point, no hop before it");
    } else {
        check(resolved==count && inRadius==count,label+"a parked flyer's point lies within PARK's flight radius");
        // Twelve random bearings: three quadrants at least (all four miss one about one game in eight; the
        // newborns' step-outs since protocol 307 draw the shared random too, and the Spyhawks now leave one
        // empty).
        check(std::popcount(quadrants)>=3 && behind,label+"parked flyers spread round the factory, behind it too");
        check(unparkedFlight==0,label+"a parked flyer's leg stays a PARK until it arrives");
    }
}

// A mobile producer's flyer takes the rally as a factory's does: alone, with no
// hop ahead of it. The producer here is a bare fixture; the flyer is shipped.
void flyerMobileRally(const sim::TypeRegistry& registry,sim::PathfindingMode mode) {
    const auto* shipped=registry.find("arafast");
    const std::string label=modeLabel(mode)+"mobile producer/arafast: ";
    if(!shipped || !shipped->canFly) {check(false,label+"fixture: a flyer");return;}
    sim::World w;flatWorld(w,registry,mode);
    auto flyer=*shipped;flyer.buildTime=1;flyer.buildCost=0;
    sim::UnitType producer{};producer.id=producer.name="mobile-producer";producer.footX=producer.footZ=6;
    producer.maxHp=10000;producer.isBuilder=true;producer.workerTime=1000;
    producer.maxVel=sim::Fixed::fromFloat(1);producer.canMove=true;
    constexpr float kRx=3000,kRz=1200;
    constexpr int kCount=3;
    const int id=w.spawn(&producer,2000,2000,0,0);
    w.setRepeat(id,&flyer);w.order(id,kRx,kRz,false);
    struct Out {int id;size_t legs=0;float x=0,z=0;bool resolved=false;};
    std::vector<Out> out;
    bool stopped=false;
    for(int tick=0;tick<3000;++tick) {
        w.tick(1.f/30);
        for(const auto& u:w.units()) {
            if(!u.alive() || u.type!=&flyer || u.underConstruction)continue;
            auto it=std::find_if(out.begin(),out.end(),[&](const Out& o){return o.id==u.id;});
            if(it==out.end()) {out.push_back({u.id});it=out.end()-1;}
            if(!it->resolved && !u.orders.empty() && !u.orders.front().stepOut) {
                it->resolved=true;it->legs=u.orders.size();
                it->x=u.orders.front().x.toFloat();it->z=u.orders.front().z.toFloat();
            }
        }
        if(!stopped && int(out.size())>=kCount) {w.stop(id);stopped=true;}
        if(stopped && std::all_of(out.begin(),out.end(),[](const Out& o){return o.resolved;}))break;
    }
    int straight=0;
    for(const auto& o:out)straight+=o.legs==1 && std::abs(o.x-kRx)<=16 && std::abs(o.z-kRz)<=16;
    std::printf("flyer mobile mode=%s outputs=%zu straight=%d hash=%016llx\n",
        mode==sim::PathfindingMode::Legion?"Legion":"Retail",out.size(),straight,(unsigned long long)w.stateHash());
    check(int(out.size())>=kCount && straight==int(out.size()),label+"a rally flyer flies straight to the rally point, no hop before it");
}

// PARK's flight branch centred on the flyer itself, without the diagonal, once
// its factory is gone (0x407f0a); around the live factory otherwise.
void flyerParkFallback(const sim::TypeRegistry& registry,sim::PathfindingMode mode) {
    const auto* flyer=registry.find("arafast");const auto* factory=registry.find("arakeep");
    for(bool dead:{false,true}) {
        sim::World w;flatWorld(w,registry,mode);
        const int f=w.spawn(factory,2000,2000,0,0);
        const int u=w.spawn(flyer,1400,1400,0,0);
        const float fx=w.unit(f)->x.toFloat(),fz=w.unit(f)->z.toFloat();
        if(dead) {
            w.destroy(f);
            for(int tick=0;tick<600 && w.unit(f) && w.unit(f)->alive();++tick)w.tick(1.f/30);
        }
        const bool gone=!w.unit(f) || !w.unit(f)->alive();
        auto* p=w.unit(u);
        const float ux=p->x.toFloat(),uz=p->z.toFloat();
        // What the factory's release queues for a flyer: one VTOL_Move carrying
        // the park until it runs. Padding 96, the smallest.
        w.order(u,ux,uz,false);
        p=w.unit(u);p->orders.back().park.emplace();p->orders.back().park->target=f;p->orders.back().park->padding=96;
        w.tick(1.f/30);
        p=w.unit(u);
        const bool resolved=!p->orders.empty() && p->orders.front().park && p->orders.front().park->attempts;
        const float cx=dead?ux:fx,cz=dead?uz:fz;
        const float dx=p->orders.empty()?0:p->orders.front().x.toFloat()-cx,dz=p->orders.empty()?0:p->orders.front().z.toFloat()-cz;
        const int want=flyer->footX*16+96+(dead?0:halfDiagonal(*factory));
        const float r=std::sqrt(dx*dx+dz*dz);
        std::printf("flyer fallback mode=%s dead=%d gone=%d resolved=%d r=%.1f want=%d\n",
            mode==sim::PathfindingMode::Legion?"Legion":"Retail",dead,gone,resolved,r,want);
        check(gone==dead && resolved && std::abs(r-float(want))<=kSnap,modeLabel(mode)+(dead
            ? "a flyer's PARK whose factory is gone circles the flyer itself, no diagonal"
            : "a flyer's PARK with a live factory circles the factory"));
    }
}

// Retail's structure placement (507400) tests the ground word, which holds a
// flyer only while it is landed (5066f0). An airborne flyer, such as one
// parked over a base, must not refuse a building site under it; a landed one
// does, as in retail. This is what the AI's site search (canPlace) sees.
void flyerPlacement(const sim::TypeRegistry& registry) {
    sim::World w;flatWorld(w,registry,sim::PathfindingMode::Retail);
    const auto* flyer=registry.find("arafast");const auto* keep=registry.find("arakeep");
    constexpr float kX=2000,kZ=2000;
    check(w.canPlace(keep,kX,kZ,0),"fixture: the site is free");
    const int u=w.spawn(flyer,kX,kZ,0,0);
    for(int tick=0;tick<30;++tick)w.tick(1.f/30);
    const bool landedBlocks=w.unit(u)->flightGroundMode==1 && !w.canPlace(keep,kX,kZ,0);
    w.order(u,kX,kZ+600,false);
    int airborneOver=0,airborneBlocked=0;
    for(int tick=0;tick<300;++tick) {
        w.tick(1.f/30);
        const auto* p=w.unit(u);
        const Rect site{sim::footprintOrigin(sim::Fixed::fromFloat(kX),keep->footX),
                        sim::footprintOrigin(sim::Fixed::fromFloat(kZ),keep->footZ),keep->footX,keep->footZ};
        if(p->flightGroundMode!=2 || !intersects(footprint(*p),site))continue;
        ++airborneOver;airborneBlocked+=!w.canPlace(keep,kX,kZ,0);
    }
    std::printf("flyer placement landedBlocks=%d airborneOver=%d airborneBlocked=%d\n",landedBlocks,airborneOver,airborneBlocked);
    check(landedBlocks,"a landed flyer holds the ground under it and refuses a building site, as in retail");
    check(airborneOver>0 && airborneBlocked==0,"an airborne flyer over a building site does not refuse it");
}
}

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    auto vfs=hpi::mountRetailRoot(argv[1]);
    auto generated=mapgen::generate(mapgen::decodeMapId(kRecipe),vfs);
    auto files=std::make_shared<hpi::Vfs::Files>();(*files)["maps/shipyard-pond.tnt"]=generated.map.save();vfs.setMapFiles(files);
    for(bool crusades:{true,false}) {
        sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,crusades);
        if(crusades)pondSiting(registry,vfs);
        for(auto mode:{sim::PathfindingMode::Retail,sim::PathfindingMode::Legion}) {
            pondYard(registry,vfs,mode,crusades);
            if(crusades)for(bool rally:{false,true})landFactory(registry,mode,rally);
            if(crusades && mode==sim::PathfindingMode::Legion)landFactory(registry,mode,false,8);
            if(crusades)deadRally(registry,mode);
            {
                for(bool rally:{false,true}) {
                    flyerFactory(registry,mode,"arakeep","arafast",rally);
                    if(!rally)flyerFactory(registry,mode,"creacad","credrag",rally);
                    if(!rally)flyerFactory(registry,mode,"tarcastl","tarship",rally);
                    if(!rally)flyerFactory(registry,mode,"verkeep","verpar",rally);
                }
                flyerMobileRally(registry,mode);
                flyerParkFallback(registry,mode);
            }
        }
        if(!crusades)flyerPlacement(registry);
    }
    std::printf("shipyard park: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
