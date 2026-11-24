// doorway_test -- a builder guarding or helping a factory must not hold its door.
//
// Reported (0.9.8): "when a builder is guarding a factory and/or helping build
// sometimes they stand in the doorway so nothing gets built". Retail's BUILD
// stage 2 (401deb) admits the next output only when no body stands on its pad
// and never shoves one off it (BUGGER_OFF is stored, never acted on), so a body
// parked in the yard stops production for good.
//
// Every shipped ground factory, at the middle of its buildangle range on a loaded
// map, makes four of its smallest ground unit with one of its side's builders
// (one that may help it, 0x519e60) standing in front of the door:
//  * guard: the builder guards the factory from the start (Follow_Ground 0x407f90:
//    it helps each output, and between outputs it follows the factory);
//  * assist: the builder helps the first output only, then stands where it was.
// Both pathfinding modes. Every output must finish, and once all is done no unit
// may stand in the factory's rectangle (its door rows included). A Shipyard
// guarded by a Battleship is held to the same, and guards of every size must come
// to rest inside their ring rather than walk in place short of it.
#include "sim/matchsetup.h"
#include "hpi/hpi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
namespace {
using namespace ta::sim;
struct Rect {int x,z,w,h;};
Rect footprint(const Unit& u) {
    return {footprintOrigin(u.x,u.type->footX),footprintOrigin(u.z,u.type->footZ),u.type->footX,u.type->footZ};
}
bool intersects(const Rect& a,const Rect& b) {return a.x<b.x+b.w && b.x<a.x+a.w && a.z<b.z+b.h && b.z<a.z+a.h;}
bool groundOutput(const UnitType* t) {
    return t && !t->isStructure() && !t->canFly && t->domain==UnitType::Domain::Ground;
}
enum class Mode {Guard,Assist};
bool doorCase(const TypeRegistry& registry,const UnitType& factory,const UnitType* output,const UnitType& helper,Mode mode,bool legion,int side) {
    auto producer=factory;producer.storage=1000000;
    World world;
    if(legion)world.setPathfindingMode(PathfindingMode::Legion);
    world.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    world.setMapPlacementFeatures(std::vector<uint16_t>(256*256,0xffff),{});
    world.buildNavClasses(registry);world.setPathService(true);
    const float cx=float(112*16+factory.footX%2*8),cz=float(112*16+factory.footZ%2*8);
    const int id=world.spawn(&producer,cx,cz,float(int16_t(factory.orientation))*6.28318530718f/65536.f,0);
    world.player(0).mana=1000000;
    world.train(id,output,4);
    // The first site shows where the pad (and so the door) is.
    int firstSite=0;
    for(int t=0;t<600 && !firstSite;++t) {
        world.tick(1.f/30);
        firstSite=world.unit(id)->productionSiteId;
    }
    if(!firstSite) {std::printf("FAIL: %s never placed its first %s\n",factory.id.c_str(),output->id.c_str());return false;}
    const Unit* site=world.unit(firstSite);
    float dx=site->x.toFloat()-cx,dz=site->z.toFloat()-cz;
    const float len=std::max(1.f,std::sqrt(dx*dx+dz*dz));
    dx/=len;dz/=len;
    // In front of the door, or level with it to either side.
    const float out=8.f*float(std::max(factory.footX,factory.footZ))+64.f;
    const float bx=cx+dx*out-dz*float(side)*out*0.75f,bz=cz+dz*out+dx*float(side)*out*0.75f;
    const int builder=world.spawn(&helper,bx,bz,0.f,0);
    if(mode==Mode::Guard)world.guard(builder,id,false);
    else world.assist(builder,firstSite,false);
    const Rect yard=footprint(*world.unit(id));
    int complete=0,tick=0,helped=0;
    // Once it has helped: its farthest from the factory's rectangle (px) and the ground it walked.
    int stray=0;double walked=0;float lastX=0,lastZ=0;
    for(;tick<20000 && complete<4;++tick) {
        world.tick(1.f/30);complete=0;
        const Unit* b=world.unit(builder);
        if(b && b->buildSiteId)helped=std::max(helped,1);
        if(b && helped) {
            const int bx0=footprintOrigin(b->x,b->type->footX)*16,bz0=footprintOrigin(b->z,b->type->footZ)*16;
            const int gx=std::max({0,yard.x*16-(bx0+b->type->footX*16),bx0-(yard.x+yard.w)*16});
            const int gz=std::max({0,yard.z*16-(bz0+b->type->footZ*16),bz0-(yard.z+yard.h)*16});
            stray=std::max(stray,std::max(gx,gz));
            if(lastX || lastZ)walked+=std::sqrt(double(b->x.toFloat()-lastX)*(b->x.toFloat()-lastX)+
                                                double(b->z.toFloat()-lastZ)*(b->z.toFloat()-lastZ));
            lastX=b->x.toFloat();lastZ=b->z.toFloat();
        }
        for(const auto& u:world.units())
            if(u.id!=id && u.id!=builder && u.alive() && u.type==output && !u.underConstruction)++complete;
    }
    for(int settle=0;settle<900;++settle)world.tick(1.f/30);
    int inYard=0;std::string who;
    for(const auto& u:world.units())
        if(u.id!=id && u.alive() && u.type && intersects(footprint(u),yard)) {
            ++inYard;who+=" "+u.type->id+(u.id==builder?"(helper)":"");
        }
    const bool pass=complete>=4 && inYard==0 && helped;
    std::printf("%s: mode=%s %s side=%d factory=%s output=%s helper=%s completed=%d helped=%d inYard=%d%s ticks=%d stray=%d walked=%d hash=%016llx\n",
        pass?"PASS":"FAIL",legion?"Legion":"Retail",mode==Mode::Guard?"guard":"assist",side,
        factory.id.c_str(),output->id.c_str(),helper.id.c_str(),complete,helped,inYard,who.c_str(),tick,stray,int(walked),
        (unsigned long long)world.stateHash());
    return pass;
}
// A guard of any size comes to rest in its ring (World::guardRing): a 4x4
// siege engine round a Barracks, a 4x4 Flagship round a Navy Yard. The mover's
// footprint-wide arrival once stopped such a guard short of the 48 px band,
// held there walking in place.
bool ringRest(const TypeRegistry& registry,const char* wardId,const char* guardId,bool water,bool legion) {
    const UnitType* wardType=registry.find(wardId);const UnitType* guardType=registry.find(guardId);
    if(!wardType || !guardType) {std::printf("FAIL: no %s or %s\n",wardId,guardId);return false;}
    World world;
    if(legion)world.setPathfindingMode(PathfindingMode::Legion);
    world.setTerrain(std::vector<uint8_t>(256*256,water?0:100),256,256,20);
    world.setMapPlacementFeatures(std::vector<uint16_t>(256*256,0xffff),{});
    world.buildNavClasses(registry);world.setPathService(true);
    const int ward=world.spawn(wardType,2000,2000,0.f,0);
    const int guard=world.spawn(guardType,2400,2300,0.f,0);
    world.guard(guard,ward,false);
    int stuck=0;Fixed lastX,lastZ;
    for(int tick=0;tick<3000;++tick) {
        world.tick(1.f/30);
        const Unit* g=world.unit(guard);
        if(tick>=2400 && g->x==lastX && g->z==lastZ && g->speed>Fixed())++stuck;
        lastX=g->x;lastZ=g->z;
    }
    const Unit* g=world.unit(guard);const Unit* w=world.unit(ward);
    const int dx=footprintOrigin(g->x,guardType->footX)-footprintOrigin(w->x,guardType->footX);
    const int dz=footprintOrigin(g->z,guardType->footZ)-footprintOrigin(w->z,guardType->footZ);
    const int inner=(std::max(wardType->footX,wardType->footZ)+guardType->footX+2)*16,outer=inner+48;
    const int squared=(dx*dx+dz*dz)*256;
    // One issued order: the guard. A route corner the navigator still holds ahead of it
    // (a non-goal leg, Order::goal false) is not a second order; the guard stands in its band.
    size_t goals=0;for(const auto& o:g->orders)goals+=o.goal;
    const bool pass=squared>=inner*inner && squared<=outer*outer && !stuck && goals==1 && g->orders.back().goal && g->orders.back().guard;
    std::printf("%s: ring mode=%s ward=%s guard=%s distance=%.0f band=%d-%d walkingInPlace=%d orders=%zu guard=%d\n",pass?"PASS":"FAIL",
        legion?"Legion":"Retail",wardId,guardId,std::sqrt(float(squared)),inner,outer,stuck,g->orders.size(),
        int(!g->orders.empty() && g->orders.front().guard));
    return pass;
}
// A Shipyard guarded by a Battleship from in front of its door: every Lurker
// must leave and the Battleship must not sit in the yard (on the Kingdoms data a
// Flagship did, from the first output on).
bool navalDoor(const TypeRegistry& registry,bool legion,int side) {
    auto yardType=*registry.find("armsy");yardType.storage=1000000;
    const UnitType* sub=registry.find("armsub");const UnitType* flag=registry.find("armbats");
    World world;
    if(legion)world.setPathfindingMode(PathfindingMode::Legion);
    world.setTerrain(std::vector<uint8_t>(256*256,0),256,256,20);
    world.setMapPlacementFeatures(std::vector<uint16_t>(256*256,0xffff),{});
    world.buildNavClasses(registry);world.setPathService(true);
    const int yard=world.spawn(&yardType,1800,1800,0.f,0);
    world.player(0).mana=1000000;world.train(yard,sub,4);
    int site=0;
    for(int t=0;t<600 && !site;++t) {world.tick(1.f/30);site=world.unit(yard)->productionSiteId;}
    if(!site) {std::printf("FAIL: armsy never placed its first armsub\n");return false;}
    float dx=world.unit(site)->x.toFloat()-1800,dz=world.unit(site)->z.toFloat()-1800;
    // A TA Shipyard builds at its centre (armsy: an open 8x8 yard, no door): then the
    // guard starts south of it, outside its footprint, instead of on the pad.
    if (dx*dx+dz*dz<1.f) dz=1.f;
    const float len=std::max(1.f,std::sqrt(dx*dx+dz*dz));dx/=len;dz/=len;
    const float out=8.f*float(std::max(yardType.footX,yardType.footZ))+64.f;
    const int guard=world.spawn(flag,1800+dx*out-dz*float(side)*out*0.75f,1800+dz*out+dx*float(side)*out*0.75f,0.f,0);
    world.guard(guard,yard,false);
    int complete=0,tick=0;
    for(;tick<20000 && complete<4;++tick) {
        world.tick(1.f/30);complete=0;
        for(const auto& u:world.units())if(u.alive() && u.type==sub && !u.underConstruction)++complete;
    }
    for(int settle=0;settle<900;++settle)world.tick(1.f/30);
    const bool inYard=intersects(footprint(*world.unit(guard)),footprint(*world.unit(yard)));
    const bool pass=complete>=4 && !inYard;
    std::printf("%s: naval mode=%s side=%d completed=%d guardInYard=%d ticks=%d\n",pass?"PASS":"FAIL",
        legion?"Legion":"Retail",side,complete,int(inYard),tick);
    return pass;
}
}
int main(int argc,char** argv) {
    if(argc<2)return 2;
    const std::string only=argc>2 ? argv[2] : "";
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);int failures=0,cases=0;
    {
        TypeRegistry registry;setupRegistry(registry,vfs);
        for(const auto& [name,type]:registry.types()) {
            if(!type.producesUnits() || !type.productionScript)continue;
            if(!only.empty() && name!=only)continue;
            const UnitType* small=nullptr;
            for(const auto& id:registry.buildable(name)) {
                const auto* t=registry.find(id);
                if(!groundOutput(t))continue;
                if(!small || t->footX*t->footZ<small->footX*small->footZ ||
                   (t->footX*t->footZ==small->footX*small->footZ && t->buildTime<small->buildTime))small=t;
            }
            if(!small)continue;
            // A builder of the factory's side that may help its output.
            const UnitType* helper=nullptr;
            for(const auto& [bn,b]:registry.types())
                if(b.isBuilder && !b.isStructure() && !b.canFly && b.side==type.side && b.domain==UnitType::Domain::Ground &&
                   (!b.builderLimited || std::find(b.buildMenu.begin(),b.buildMenu.end(),small->id)!=b.buildMenu.end()) &&
                   (!helper || b.buildDist<helper->buildDist))helper=&b;
            // (A side whose builders are all builderlimited without the output: any side's.)
            if(!helper)for(const auto& [bn,b]:registry.types())
                if(b.isBuilder && !b.isStructure() && !b.canFly && b.domain==UnitType::Domain::Ground && !b.builderLimited &&
                   (!helper || b.buildDist<helper->buildDist))helper=&b;
            if(!helper){std::printf("FAIL: %s has no helper for %s\n",name.c_str(),small->id.c_str());++failures;continue;}
            // A Gantry makes only the Krogoth (buildtime 382193): capped so four
            // can finish inside the case.
            UnitType quick=*small;quick.buildTime=std::min(quick.buildTime,20000.0f);
            for(bool legion:{false,true})for(Mode mode:{Mode::Guard,Mode::Assist})for(int side:{-1,0,1}) {
                ++cases;if(!doorCase(registry,type,&quick,*helper,mode,legion,side))++failures;
            }
        }
    }
    if(only.empty()) {
        TypeRegistry registry;setupRegistry(registry,vfs);
        for(bool legion:{false,true}) {
            for(const char* g:{"armbull","corkrog","armpw"}) {++cases;if(!ringRest(registry,"armlab",g,false,legion))++failures;}
            for(const char* g:{"armbats","armpt"}) {++cases;if(!ringRest(registry,"armsy",g,true,legion))++failures;}
            for(int side:{-1,0,1}) {++cases;if(!navalDoor(registry,legion,side))++failures;}
        }
    }
    std::printf("doorway: %d cases, %d failures\n",cases,failures);
    return failures||!cases?1:0;
}
