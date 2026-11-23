#include "client/emotetiming.h"
#include "cob/retailvm.h"
#include "cob/retailpieces.h"
#include "cob/retailstate.h"
#include "sim/retailrng.h"
#include "sim/retailpiecepose.h"
#include "sim/footprint.h"
#include "tdo/tdo.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/retailhweffectdata.h"
#include "sim/retailanimationqueries.h"
#include "sim/retaildeath.h"
#include "sim/retailfeaturesink.h"
#include "sim/retailtransport.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <map>

namespace tak::sim {
struct RetailReplayProbe {
    static void hit(World& world,const Weapon& weapon,float x,float z,int from,int target) {
        world.applyHit(weapon,x,z,0,from,world.unit(target));
    }
    static bool corpse(World& world,int id,int type) {
        auto& unit=*world.unit(id);unit.hp={};unit.deadFor=120;unit.corpseUntil=1000;
        return world.placeCorpse(unit,type);
    }
    static void retire(World& world,int id) {world.retireCorpse(*world.unit(id));}
    static const auto& scriptStatics(const World& world,int id) { return world.unitScripts_.at(id).peek().vm.statics; }
    static void emitScript(World& world,int id) {world.notifyUnitScript(*world.unit(id),"Emit");}
    static void notifyScript(World& world,int id,const char* name) {world.notifyUnitScript(*world.unit(id),name);}
    // The A1 sleep skip: touching a VM pays its debt and makes it run next tick.
    static void touchScript(World& world,int id) {world.unitScripts_.at(id).state();}
    static uint32_t scriptDebt(const World& world,int id) {return world.unitScripts_.at(id).debt();}
    static const tak::cob::RetailVm& paidVm(World& world,int id) {return world.unitScripts_.at(id).state().vm;}
    static void repairTick(World& world,int id) {world.tickRepair(*world.unit(id),1.f/30);}
    // COB INBUILDSTANCE (SET 5), which the reclaim and build stance waits poll.
    static void setStance(World& world,int id,bool ready) {world.unitScripts_.at(id).ready=ready;}
    static void scriptTick(World& world,int id) {world.tickUnitScript(*world.unit(id));}
    static void clearScriptEvents(World& world) {world.scriptEmissions_.clear();}

    static void shoot(World& world,int from,int target) {world.fire(*world.unit(from),*world.unit(target),0);}
    static float manaShotCost(const Unit& u,int slot) {return World::manaShotCost(u,slot);}
    static void manaFallback(World& world,Unit& u,int slot) {world.manaFallback(u,slot);}
    static bool switchWeapon(World& world,Unit& u,int slot) {return world.switchWeaponSlot(u,slot,false);}
    // The release (530220, script-triggered) at a point, then 52af72's
    // fallback: tools/re/check_mana_fallback.py.
    static void release(World& world,Unit& u,int slot) {
        world.fire(u,World::WeaponTarget{nullptr,{300*65536,0,200*65536}},slot,true);
        world.manaFallback(u,slot);
    }
    static void flameTick(World& world,uint32_t tick) {
        world.tickCounter_=tick;const auto aircraft=world.updateAirOccupancy();world.tickFlames(aircraft);
    }
    static void straightTick(World& world,uint32_t tick) {
        world.tickCounter_=tick;const auto aircraft=world.updateAirOccupancy();world.tickStraightProjectiles(aircraft);
    }

    static std::vector<uint32_t> movementQueries(World& world,int id) {
        world.notifyUnitScript(*world.unit(id),"ReadMovement");
        return world.unitScripts_.at(id).peek().vm.statics;
    }
    static void cacheYard(World& world) {
        World::SearchGradePlane plane;plane.nav=&world.nav_;
        plane.footX=plane.footZ=1;plane.navVersion=plane.nav->version();
        plane.cells.assign(32*32,6);world.searchGrades_.push_back(std::move(plane));
        world.refreshSearchRect(world.searchGrades_.back(),0,0,32,32);
    }
    static int cachedYard(const World& world,int x,int z) {
        return world.searchGrades_.back().cells[size_t(z)*32+x]&7;
    }
    static bool tickYard(World& world,int id) {
        world.rebuildOccupancy();world.tickUnitScript(*world.unit(id));
        return world.unitScripts_.at(id).yardOpen;
    }
    static bool automaticYard(World& world,int id) {
        world.rebuildOccupancy();world.tickAutomaticGates();world.tickUnitScript(*world.unit(id));
        return world.unitScripts_.at(id).yardOpen;
    }
    static bool yardOpen(const World& world,int id) { return world.unitScripts_.at(id).yardOpen; }
    static uint64_t completedSearches(const World& world) { return world.paths_.completions(); }
    static void gateSeed(World& world) { world.gameRng_=1; }
    static void gateState(const World& world,int id) {
        const auto& unit=*world.unit(id);const auto& script=world.unitScripts_.at(id);
        // The printed thread words include sleep counts: pay any skipped ticks first.
        const auto& state=const_cast<World::UnitScript&>(script).state();
        std::cout<<unit.active<<' '<<script.yardOpen<<' '<<state.vm.active<<' '<<world.gameRng_;
        for (auto v:state.vm.statics) std::cout<<' '<<v;
        for (const auto& thread:state.vm.threads) for (auto v:thread.words) std::cout<<' '<<v;
        for (const auto& piece:state.pieces) {
            std::cout<<' '<<piece.active;
            for (auto* values:{&piece.moveTarget,&piece.moveSpeed,&piece.turnTarget,&piece.turnSpeed,
                              &piece.spinTarget,&piece.spinAcceleration,&piece.move,&piece.turn})
                for (auto value:*values) std::cout<<' '<<uint32_t(value);
        }
        const auto bodies=world.searchBodyRect(footprintOrigin(unit.x,unit.type->footX),
            footprintOrigin(unit.z,unit.type->footZ),unit.type->footX,unit.type->footZ);
        for (const auto* body:bodies.cells) std::cout<<' '<<(body ? body->id : 0);
        std::cout<<'\n';
    }
    static bool yard(World& world,int id,const char* request) {
        world.rebuildOccupancy();
        world.notifyUnitScript(*world.unit(id),request);
        if (world.scriptYardOpen(id)!=world.unitScripts_.at(id).yardOpen)
            throw std::runtime_error("indexed yard state differs from the script record");
        return world.unitScripts_.at(id).yardOpen;
    }
};
}

struct Host {
    uint32_t seed=1;
    std::vector<int32_t> events;
    bool waiting(bool,int,int) { return false; }
    uint32_t random(int32_t bound) { return tak::sim::retailRandom(seed,bound); }
    uint32_t get(int id,const std::array<uint32_t,4>&) {
        if (const char* profile=std::getenv("TAK_SCRIPT_ORACLE_PROFILE")) {
            const int mode=std::atoi(profile);
            if (id==4) return mode==3 ? 25 : 100;
            if (id==18) return 1;
            if (id==29) return mode==1 || mode==2 ? 100 : 0;
            if (id==28) return mode==2;
            if (id==34) return mode==1;
            if (id==32) return mode==3 ? 10 : 0;
            if (id==33) return mode==1 ? 100 : 0;
            if (id==46) return mode==3 ? 1 : 0;
            return 0;
        }
        return id==18 ? 1 : 100;
    }
    void set(int id,int value) { events.insert(events.end(),{id,value}); }
    void piece(uint32_t,int,int,int32_t,int32_t) {}
    void effect(uint32_t,int,int32_t) {}
    uint32_t sound(int,int32_t priority) { return uint32_t(priority); }
};

int main(int argc,char** argv) {
    if (argc==2 && std::string(argv[1])=="--mana-fallback") {
        // Rows: selector (3 = every slot), three manapershot costs, mana
        // before the release, the vetOverride (+0xf4), noveteran, the
        // experience (+0xf0) and the type's experiencepoints (+0x1c2). Prints
        // the selector and the mana after the release in 1/1024 units.
        using namespace tak::sim;
        int selected,noVeteran,experience,points;float c0,c1,c2,vet;double mana;
        while(std::cin>>selected>>c0>>c1>>c2>>mana>>vet>>noVeteran>>experience>>points) {
            UnitType type;type.maxHp=100;type.maxMana=1000;type.noVeteran=noVeteran!=0;
            type.experiencePoints=points;
            type.weaponSwitching=selected<3;
            Weapon gun;gun.range=400;gun.damage=1;gun.projVel=300;gun.reload=1;
            for(const float cost:{c0,c1,c2}) {gun.manaCost=cost;type.weapons.push_back(gun);}
            type.weapon=type.weapons.front();
            World world;
            world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int id=world.spawn(&type,200,200,{},0);
            Unit& u=*world.unit(id);
            u.weaponSlot=selected<3 ? selected : 0;u.mana=mana;
            // The experience as the kill credit sums it (a noveteran unit gains
            // it too); the override then sets the rank (getLevel 519310: 0 for
            // noveteran, 10 while +0xf4 is set, else experience over
            // experiencepoints capped at 10).
            u.xp=experience;world.setVeterancyOverride(id,vet);
            // A non-switcher releases every slot it holds (52ae90's selector 3).
            for(int slot=type.weaponSwitching ? u.weaponSlot : 0;slot<=(type.weaponSwitching ? u.weaponSlot : 2);++slot)
                RetailReplayProbe::release(world,u,slot);
            std::cout<<(type.weaponSwitching ? u.weaponSlot : 3)<<' '<<std::llround(u.mana*1024)<<'\n';
        }
        return 0;
    }
    if (argc==3 && std::string(argv[1])=="--features") {
        using namespace tak::sim;
        auto vfs=tak::hpi::mountRetailRoot(argv[2]);
        std::map<std::string,tak::tdf::Node> defs;
        int spacing=4;
        for(const auto& path:vfs.list("features")) {
            if(!path.ends_with(".tdf"))continue;
            const auto bytes=vfs.read(path);
            const auto root=tak::tdf::parseText(std::string(bytes.begin(),bytes.end()),path);
            for(const auto& name:root.childOrder) {
                std::string key=name;std::transform(key.begin(),key.end(),key.begin(),::tolower);
                const auto& node=root.children.at(name);defs[key]=node;
                spacing=std::max(spacing,4+int(std::max(node.numberOr("footprintx",1),node.numberOr("footprintz",1))));
            }
        }
        tak::tnt::Map map;std::vector<std::string> names;
        for(const auto& [name,node]:defs) {
            std::string category=node.valueOr("category","");
            std::transform(category.begin(),category.end(),category.begin(),::tolower);
            if(node.numberOr("damage",0)>0 && category!="mana") names.push_back(name);
        }
        if(names.empty())throw std::runtime_error("no authored scenery definitions found");
        constexpr int columns=32;
        map.width=columns*spacing+32;map.height=int((names.size()+columns-1)/columns)*spacing+32;
        map.heights.assign(size_t(map.width)*map.height,10);
        map.features.assign(map.heights.size(),0xffff);map.featureNames=names;
        std::vector<int> ids;
        for(size_t i=0;i<names.size();++i) {
            const int x=16+int(i%columns)*spacing,z=16+int(i/columns)*spacing;
            ids.push_back(z*map.width+x);map.features[ids.back()]=uint16_t(i);
        }
        World world;world.setTerrain(map.heights,map.width,map.height,0);
        registerMapFeatures(world,map,vfs);
        int destructible=0,protectedCount=0,damageOnly=0,dying=0;
        for(size_t i=0;i<names.size();++i) {
            const auto& node=defs.at(names[i]);const int id=ids[i];
            const auto* f=world.feature(id);
            const bool protectedFeature=node.numberOr("indestructible",0)!=0;
            if(!f && !protectedFeature)throw std::runtime_error("untracked destructible: "+names[i]);
            const int fx=int(node.numberOr("footprintx",1)),fz=int(node.numberOr("footprintz",1));
            const float x=float((id%map.width+fx-1)*16+8),z=float((id/map.width+fz-1)*16+8);
            const int hp=int(node.numberOr("damage",0));Weapon shot;shot.damage=hp;
            const auto generation=world.featGeneration();
            RetailReplayProbe::hit(world,shot,x,z,0,0);
            if(protectedFeature) {
                ++protectedCount;
                if(world.featGeneration()!=generation || (f && f->dmg))
                    throw std::runtime_error("indestructible scenery took damage: "+names[i]);
            } else {
                ++destructible;
                if(!node.numberOr("reclaimable",0) && !node.numberOr("flamable",0))++damageOnly;
                if(world.featGeneration()==generation)throw std::runtime_error("outer hit did not destroy: "+names[i]);
                // A seqnamedie plays first (494ce0 mode 0); the stage follows its end.
                if(f->dieLeft) {
                    if(f->type<0 || world.featureTypes()[f->type].name!=names[i] || !f->alive)
                        throw std::runtime_error("dying feature left its stage early: "+names[i]);
                    ++dying;
                    for(uint32_t left=f->dieLeft;left;--left)world.tick(1.0f/30);
                    if(f->dieLeft)throw std::runtime_error("death playback did not end: "+names[i]);
                }
                std::string dead=node.valueOr("featuredead","");
                std::transform(dead.begin(),dead.end(),dead.begin(),::tolower);
                if(dead.empty() || !defs.contains(dead)) {
                    if(f->alive)throw std::runtime_error("destroyed feature still alive: "+names[i]);
                } else if(f->type<0 || world.featureTypes()[f->type].name!=dead)
                    throw std::runtime_error("wrong destruction stage: "+names[i]);
            }
        }
        std::cout<<"PASS: "<<destructible<<" authored destructible scenery types ("<<damageOnly
                 <<" damage-only, "<<dying<<" with a death playback), "<<protectedCount
                 <<" indestructible types; outer-footprint hits and destruction stages\n";
        return 0;
    }
    if (argc==5 && std::string(argv[1])=="--gate-timeline") {
        using namespace tak::sim;
        auto vfs=tak::hpi::mountRetailRoot(argv[2]);
        TypeRegistry registry;setupRegistry(registry,vfs,std::stoi(argv[4])!=0);
        const auto* type=registry.find(argv[3]);
        if (!type || !type->gate || !type->script()) return 2;
        World world;
        world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
        world.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
        const int id=world.spawn(type,512,512);
        RetailReplayProbe::gateSeed(world);
        int active;
        while (std::cin>>active) {
            if (active< -1 || active>1) return 2;
            if (active>=0) world.setActive(id,active!=0);
            RetailReplayProbe::tickYard(world,id);
            RetailReplayProbe::gateState(world,id);
        }
        return 0;
    }
    if (argc==3 && std::string(argv[1])=="--gates") {
        using namespace tak::sim;
        auto vfs=tak::hpi::mountRetailRoot(argv[2]);
        unsigned count=0;
        for (bool crusades:{false,true}) {
            TypeRegistry registry;setupRegistry(registry,vfs,crusades);
            for (const auto& [name,type]:registry.types()) {
                if (!type.gate) continue;
                if (!type.script() || !type.onOffable) {
                    std::cerr<<name<<": gate activation inputs absent\n";return 1;
                }
                World world;
                world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
                world.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
                const int id=world.spawn(&type,512,512);
                for (int tick=0;tick<1000;++tick) RetailReplayProbe::tickYard(world,id);
                for (bool open:{false,true,false,true,false}) {
                    world.setActive(id,open);
                    bool actual=false;
                    for (int tick=0;tick<1000;++tick) actual=RetailReplayProbe::tickYard(world,id);
                    if (actual!=open) {
                        std::cerr<<name<<": balance="<<crusades<<" requested="<<open<<" yard="<<actual<<'\n';return 1;
                    }
                }
                world.setActive(id,true);
                for (int tick=0;tick<1000;++tick) RetailReplayProbe::tickYard(world,id);
                const size_t passage=type.yardMap.find_first_of("cC");
                if (passage==std::string::npos) return 1;
                const int px=footprintOrigin(world.unit(id)->x,type.footX)+int(passage%type.footX);
                const int pz=footprintOrigin(world.unit(id)->z,type.footZ)+int(passage/type.footX);
                UnitType body;body.footX=body.footZ=1;
                const int occupant=world.spawn(&body,float(px*16+8),float(pz*16+8));
                RetailReplayProbe::tickYard(world,id);
                world.setActive(id,false);
                bool opened=false;
                for (int tick=0;tick<1000;++tick) opened=RetailReplayProbe::tickYard(world,id);
                if (!opened) { std::cerr<<name<<": closed on an occupied passage\n";return 1; }
                world.unit(occupant)->x=world.unit(occupant)->z=Fixed::fromInt(960);
                for (int tick=0;tick<1000;++tick) opened=RetailReplayProbe::tickYard(world,id);
                if (opened) { std::cerr<<name<<": did not close after passage cleared\n";return 1; }
                world.player(0).automaticGates=true;
                const int outsideZ=footprintOrigin(world.unit(id)->z,type.footZ)-1;
                world.unit(occupant)->x=Fixed::fromInt(px*16+8);
                world.unit(occupant)->z=Fixed::fromInt(outsideZ*16+8);
                world.unit(occupant)->speed=Fixed::fromInt(1);
                for (int tick=0;tick<1000;++tick) opened=RetailReplayProbe::automaticYard(world,id);
                if (!opened || !world.unit(id)->active) { std::cerr<<name<<": approach did not open gate\n";return 1; }
                world.unit(occupant)->z=Fixed::fromInt(pz*16+8);
                world.unit(occupant)->speed=Fixed();
                for (int tick=0;tick<1000;++tick) opened=RetailReplayProbe::automaticYard(world,id);
                if (!opened || !world.unit(id)->active) { std::cerr<<name<<": stopped occupant did not hold gate\n";return 1; }
                world.unit(occupant)->z=Fixed::fromInt(outsideZ*16+8);
                for (int tick=0;tick<1000;++tick) opened=RetailReplayProbe::automaticYard(world,id);
                if (opened || world.unit(id)->active) { std::cerr<<name<<": cleared gate did not close automatically\n";return 1; }
                const int originZ=footprintOrigin(world.unit(id)->z,type.footZ);
                const int originX=footprintOrigin(world.unit(id)->x,type.footX);
                std::vector<uint16_t> corridor(64*64,0xffff);
                for (int x=0;x<64;++x)
                    if (x<originX || x>=originX+type.footX) corridor[size_t(pz)*64+x]=0xfffc;
                world.setMapPlacementFeatures(corridor,{});
                world.blockCells(0,pz,originX,1,true);
                world.blockCells(originX+type.footX,pz,64-originX-type.footX,1,true);
                world.unit(occupant)->z=Fixed::fromInt((originZ-6)*16+8);
                world.setPathService(true);world.setPathBudget(503);
                for (bool returning:{false,true}) {
                    const auto completions=RetailReplayProbe::completedSearches(world);
                    const int targetX=px*16+8;
                    const int targetZ=(returning ? originZ-6 : originZ+type.footZ+6)*16+8;
                    world.order(occupant,float(targetX),float(targetZ),false);
                    bool crossed=false;
                    for (int tick=0;tick<2400;++tick) {
                        world.tick(1.0f/30.0f);
                        const auto& moving=*world.unit(occupant);
                        const int cx=footprintOrigin(moving.x,1),cz=footprintOrigin(moving.z,1);
                        const int gx=footprintOrigin(world.unit(id)->x,type.footX);
                        if (cx>=gx && cx<gx+type.footX && cz>=originZ && cz<originZ+type.footZ) {
                            const char cell=type.yardMap[size_t(cz-originZ)*type.footX+cx-gx];
                            if (cell=='c' || cell=='C') {
                                if (RetailReplayProbe::completedSearches(world)==completions) {
                                    std::cerr<<name<<": crossed without a completed route search\n";return 1;
                                }
                                crossed=true;
                                if (!RetailReplayProbe::yardOpen(world,id)) {
                                    std::cerr<<name<<": mover entered a closed passage\n";return 1;
                                }
                            }
                        }
                    }
                    const auto& arrived=*world.unit(occupant);
                    if (!crossed || std::abs(arrived.x.floorInt()-targetX)>16 ||
                        std::abs(arrived.z.floorInt()-targetZ)>16 || RetailReplayProbe::yardOpen(world,id)) {
                        std::cerr<<name<<": traversal failed balance="<<crusades<<" crossed="<<crossed
                                 <<" position="<<arrived.x.floorInt()<<','<<arrived.z.floorInt()
                                 <<" target="<<targetX<<','<<targetZ<<" open="<<RetailReplayProbe::yardOpen(world,id)<<'\n';
                        return 1;
                    }
                }
                ++count;
            }
        }
        std::cout<<"PASS: "<<count<<" shipped gate scripts under both balances across activation, occupied closure and full World traversal\n";
        return count ? 0 : 1;
    }
    if (argc==3 && (std::string(argv[1])=="--factories" || std::string(argv[1])=="--units")) {
        const bool allUnits=std::string(argv[1])=="--units";
        auto vfs=tak::hpi::mountRetailRoot(argv[2]);
        tak::sim::TypeRegistry registry; tak::sim::setupRegistry(registry,vfs,false);
        unsigned count=0;
        for (const auto& [id,type]:registry.types()) {
            if (allUnits ? !type.script() : !type.productionScript) continue;
            const auto& file=*type.script();
            tak::cob::RetailScriptState state(file);
            struct FactoryHost:Host {
                uint32_t get(int id,const std::array<uint32_t,4>&) {
                    if (id==4) return 100;
                    if (id==18) return 1;
                    return 0;
                }
            } host;
            const std::array<uint32_t,4> args{};
            state.vm.start(file,file.scriptIndex("Create"),args);
            try {
                for (int tick=0;tick<10000;++tick) {
                    if (tick==50) state.vm.start(file,file.scriptIndex("Activate"),args);
                    if (tick==100) state.vm.start(file,file.scriptIndex("StartBuilding"),args);
                    if (tick==2000) state.vm.start(file,file.scriptIndex("StopBuilding"),args);
                    if (tick==2500) state.vm.start(file,file.scriptIndex("Deactivate"),args);
                    state.tick(file,1,host);
                }
            } catch (const std::exception& error) {
                std::cerr<<id<<": "<<error.what()<<'\n'; return 1;
            }
            ++count;
        }
        std::cout<<"PASS: "<<count<<(allUnits ? " unit" : " factory")<<" scripts, 10000 updates each across build lifecycle callbacks\n";
        return count ? 0 : 1;
    }
    // --origin: 4dd0f0's piece origin. --bounds: 4dd2a0's SweetSpot bounds over
    // the drawn (posed) vertex buffer, as the simulation uses. --local-bounds:
    // 4dd2a0 on a unit never drawn since load, whose buffer holds the authored
    // vertices. Outputs are unit-relative raw fixed point.
    if ((argc==7 || argc==9) && (std::string(argv[1])=="--origin" || std::string(argv[1])=="--bounds" ||
                                 std::string(argv[1])=="--local-bounds")) {
        auto file=tak::cob::load(std::filesystem::path(argv[2]));
        auto model=tak::tdo::load(std::filesystem::path(argv[3]));
        std::ifstream input(argv[4],std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        tak::cob::RetailScriptState state(file); state.restore(file,bytes);
        std::vector<tak::sim::RetailModelPiece> pieces;
        std::vector<std::array<int32_t,3>> vertices;
        auto lower=[](std::string s) { for (auto& c:s) c=char(std::tolower(static_cast<unsigned char>(c))); return s; };
        auto append=[&](auto&& self,const tak::tdo::Object& object,int parent)->void {
            int piece=-1;
            for (size_t i=0;i<file.pieces.size();++i)
                if (lower(file.pieces[i])==lower(object.name)) { piece=int(i); break; }
            const int index=int(pieces.size());
            pieces.push_back({object.offsetRaw,parent,piece});
            if(piece==std::stoi(argv[6]))vertices=object.verticesRaw;
            for (const auto& child:object.children) self(self,child,index);
        };
        append(append,model.root,-1);
        const uint16_t heading=uint16_t(std::stoi(argv[5]));
        const uint16_t pitch=argc==9 ? uint16_t(std::stoi(argv[7])) : 0,roll=argc==9 ? uint16_t(std::stoi(argv[8])) : 0;
        auto point=tak::sim::retailPieceOrigin(pieces,state.pieces,std::stoi(argv[6]),heading,pitch,roll);
        for(auto& vertex:vertices)
            for(int axis:{0,2})vertex[size_t(axis)]=std::bit_cast<int32_t>(0u-uint32_t(vertex[size_t(axis)]));
        if(std::string(argv[1])=="--bounds")
            point=tak::sim::retailPieceDrawnCenter(pieces,state.pieces,std::stoi(argv[6]),vertices,heading,pitch,roll);
        if(std::string(argv[1])=="--local-bounds") {
            tak::sim::RetailPieceBounds bounds;
            for(const auto& vertex:vertices)bounds.add(vertex);
            point=bounds.center();
        }
        for (auto v:point) std::cout<<v<<' ';
        std::cout<<'\n'; return 0;
    }
    if (argc>1 && std::string(argv[1])=="--rotate") {
        int32_t a,b; unsigned angle;
        while (std::cin>>a>>b>>angle) {
            tak::sim::retailRotatePair(a,b,uint16_t(angle));
            std::cout<<a<<' '<<b<<'\n';
        }
        return 0;
    }
    // tools/re/check_kill_credit.py: hp maxdamage -> the HP sample (51e07c);
    // kind hp maxdamage previous -> 512610's severity.
    if (argc==2 && std::string(argv[1])=="--hp-sample") {
        int hp,maxDamage;
        while (std::cin>>hp>>maxDamage)
            std::cout<<int(tak::sim::retailHpSample(int16_t(hp),maxDamage))<<'\n';
        return 0;
    }
    // damage kind activated modifier(16.16) -> the damage 51a140 hands on.
    if (argc==2 && std::string(argv[1])=="--damage-modifier") {
        int32_t damage,modifier;int kind,activated;
        while (std::cin>>damage>>kind>>activated>>modifier)
            std::cout<<int(int16_t(tak::sim::retailDamageModifier(damage,uint8_t(kind),activated!=0,modifier)))<<'\n';
        return 0;
    }
    if (argc==2 && std::string(argv[1])=="--severity") {
        int kind,hp,maxDamage,previous;
        while (std::cin>>kind>>hp>>maxDamage>>previous)
            std::cout<<int(tak::sim::retailDeathSeverity(uint8_t(kind),int16_t(hp),maxDamage,uint8_t(previous)))<<'\n';
        return 0;
    }
    // tools/re/check_kill_credit.py: tick height(16.16) sea canhover enabled
    // amount -> the deadly water's damage this tick (0 for none).
    if (argc==2 && std::string(argv[1])=="--deadly-water") {
        uint32_t tick;int32_t height,amount;int sea,hover,enabled;
        while (std::cin>>tick>>height>>sea>>hover>>enabled>>amount)
            std::cout<<tak::sim::retailDeadlyWaterDamage(tick,height,sea,hover!=0,enabled!=0,amount)<<'\n';
        return 0;
    }
    // tools/re/check_water_grave.py: y(16.16) bed sea -> the updates a water
    // grave's feature lives after its placement update (-1: it comes to rest
    // and stays), run as World runs it (retailFeatureSinkStep).
    if (argc==2 && std::string(argv[1])=="--water-grave") {
        int32_t y;int bed,sea;
        while (std::cin>>y>>bed>>sea) {
            tak::sim::RetailFeatureSink f{(3*16+5)<<16,y,(3*16+5)<<16,0,tak::sim::kRetailSinkVelocity,0};
            int32_t life=-1;
            for (int32_t update=1;update<(1<<20);++update) {
                const auto r=tak::sim::retailFeatureSinkStep(f,[&](int32_t,int32_t) { return bed; },uint8_t(sea),0x1fdb);
                if (r==tak::sim::RetailSinkResult::Moving) continue;
                if (r==tak::sim::RetailSinkResult::Removed) life=update-1;
                break;
            }
            std::cout<<life<<'\n';
        }
        return 0;
    }
    // tools/re/check_corpse_decay.py: decomposition, the update after the
    // placement one before which a reclaim reloads +0x3c (-1: none), the
    // reload -> the updates after the placement one until the removal (-1:
    // it stays). The closed form World keeps for a land corpse must agree
    // with the record stepped as a water grave's is (retailFeatureSinkStep
    // with no velocity).
    if (argc==2 && std::string(argv[1])=="--corpse-decay") {
        int32_t decompose,wake,reload;
        while (std::cin>>decompose>>wake>>reload) {
            int32_t life=tak::sim::retailCorpseDecayLife(decompose);
            // A corpse at rest left the update list at its first update: a
            // reload never counts there.
            if (wake>=0 && life>=0 && wake<=life) {
                const int32_t left=tak::sim::retailCorpseDecayHold(life-wake,reload);
                life=left<0 ? -1 : wake+left;
            }
            tak::sim::RetailFeatureSink f;f.decay=decompose;
            int32_t stepped=-1;
            for (int32_t update=1;update<(1<<20);++update) {
                if (update==wake+1) f.decay=reload;
                const auto r=tak::sim::retailFeatureSinkStep(f,[](int32_t,int32_t) { return 0; },0,0);
                if (r==tak::sim::RetailSinkResult::Moving) continue;
                if (r==tak::sim::RetailSinkResult::Removed) stepped=update-1;
                break;
            }
            if (stepped!=life) {
                std::cerr<<"closed form "<<life<<" != stepped "<<stepped<<" (decompose "<<decompose
                         <<" wake "<<wake<<" reload "<<reload<<")\n";
                return 1;
            }
            std::cout<<life<<'\n';
        }
        return 0;
    }
    // tools/re/check_kill_credit.py: deadly water on a unit carried at a host
    // height (the attach point World commits for its cargo) -> the damage.
    if (argc==2 && std::string(argv[1])=="--carried-water") {
        int32_t height,amount;int floater,waterline,sea;uint32_t tick;
        while (std::cin>>height>>floater>>waterline>>sea>>amount>>tick) {
            const auto at=tak::sim::retailAttachedPosition(tak::sim::retailAttachPoint({4096<<16,height,2048<<16},{},{},-1,0),
                floater!=0,uint8_t(waterline),uint8_t(sea));
            std::cout<<tak::sim::retailDeadlyWaterDamage(tick,at[1],sea,false,true,amount)<<'\n';
        }
        return 0;
    }
    // tools/re/check_veterancy.py: one row per case, answered in its order.
    //   c kind finished hitter victim killer points xp scoreoff count
    //                                  -> the killer's xp, the hitter player's kills and score, and the
    //                                     killer's own kill count (a word) after a death
    //   l xp points noveteran multiplier                 -> level, vetMul's float bits
    //   s xp level points noveteran multiplier           -> xp after setLevel
    if (argc==2 && std::string(argv[1])=="--veterancy") {
        std::string row;
        while (std::cin>>row) {
            if (row=="c") {
                int kind,finished,hitter,victim,killer,scoreOff;int32_t points,xp;unsigned count;
                std::cin>>kind>>finished>>hitter>>victim>>killer>>points>>xp>>scoreOff>>count;
                if (killer && tak::sim::retailKillCreditsExperience(uint8_t(kind),finished!=0,hitter,victim)) {
                    xp=tak::sim::retailAddExperience(xp,points);
                    count=uint16_t(count+1u);   // World::creditKill's ++kills
                }
                const bool counted=tak::sim::retailKillCountsForPlayer(uint8_t(kind),finished!=0,hitter,victim);
                std::cout<<xp<<' '<<(counted ? 1 : 0)<<' '<<(counted && !scoreOff ? points : 0)<<' '<<count<<'\n';
            } else if (row=="l") {
                tak::sim::UnitType type;tak::sim::Unit unit;int noVeteran;
                std::cin>>unit.xp>>type.experiencePoints>>noVeteran>>unit.vetOverride;
                type.noVeteran=noVeteran!=0;unit.type=&type;unit.veteran=unit.veteranLevel();
                std::cout<<unit.veteran<<' '<<std::bit_cast<uint32_t>(unit.vetMul())<<'\n';
            } else {
                int32_t xp,points;int level,noVeteran;float multiplier;
                std::cin>>xp>>level>>points>>noVeteran>>multiplier;
                std::cout<<tak::sim::retailExperienceForLevel(xp,level,points,noVeteran!=0,multiplier)<<'\n';
            }
        }
        return 0;
    }
    if ((argc==5 && std::string(argv[1])=="--state") ||
        ((argc==6 || argc==7) && std::string(argv[1])=="--state-start")) {
        auto file=tak::cob::load(std::filesystem::path(argv[2]));
        std::ifstream input(argv[3],std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        tak::cob::RetailScriptState state(file); state.restore(file,bytes);
        Host host;
        struct Event {int script;unsigned count;std::array<uint32_t,4> args;};
        std::multimap<int,Event> events;
        if(argc==7) {
            std::ifstream timeline(argv[6]);int tick;Event e;
            while(timeline>>tick>>e.script>>e.count>>e.args[0]>>e.args[1]>>e.args[2]>>e.args[3])
                events.emplace(tick,e);
        }
        for (int tick=argc>=6 ? -1 : 0;tick<std::stoi(argv[4]);++tick) {
            host.events.clear();
            if (tick<0) state.notify(file,std::stoi(argv[5]),host);
            else {
                auto [begin,end]=events.equal_range(tick);
                for(auto it=begin;it!=end;++it) {
                    const auto& e=it->second;
                    if(state.startArguments(file,e.script,e.args,e.count)) state.tick(file,0,host);
                }
                state.tick(file,1,host);
            }
            std::cout<<state.vm.active<<' '<<host.seed;
            for (auto v:state.vm.statics) std::cout<<' '<<v;
            for (const auto& t:state.vm.threads) for (auto v:t.words) std::cout<<' '<<v;
            for (const auto& p:state.pieces) {
                std::cout<<' '<<p.active;
                for (auto* values:{&p.moveTarget,&p.moveSpeed,&p.turnTarget,&p.turnSpeed,
                                  &p.spinTarget,&p.spinAcceleration,&p.move,&p.turn})
                    for (auto v:*values) std::cout<<' '<<uint32_t(v);
            }
            std::cout<<' '<<host.events.size();
            for (auto v:host.events) std::cout<<' '<<v;
            std::cout<<'\n';
        }
        return 0;
    }
    if (argc>1 && std::string(argv[1])=="--pieces") {
        uint32_t op; int axis,target,speed,elapsed;
        while (std::cin>>op>>axis>>target>>speed>>elapsed) {
            tak::cob::RetailPiece p;
            for (auto* values:{&p.moveTarget,&p.moveSpeed,&p.turnTarget,&p.turnSpeed,
                              &p.spinTarget,&p.spinAcceleration,&p.move,&p.turn})
                for (auto& value:*values) std::cin>>value;
            std::cin>>p.active;
            if (op) p.command(op,axis,target,speed);
            p.tick(elapsed);
            for (auto* values:{&p.moveTarget,&p.moveSpeed,&p.turnTarget,&p.turnSpeed,
                              &p.spinTarget,&p.spinAcceleration,&p.move,&p.turn})
                for (auto value:*values) std::cout<<value<<' ';
            std::cout<<p.active<<'\n';
        }
        return 0;
    }
    if (argc>1 && std::string(argv[1])=="--oracle") {
        unsigned nc,ns,nv,nt;
        while (std::cin>>nc>>ns>>nv>>nt) {
            tak::cob::File file;
            file.code.resize(nc); file.scripts.resize(ns); file.numStatics=nv;
            for (auto& word:file.code) std::cin>>word;
            for (auto& script:file.scripts) std::cin>>script.entry;
            tak::cob::RetailVm vm(nv); Host host;
            for (auto& value:vm.statics) std::cin>>value;
            for (auto& thread:vm.threads) for (auto& value:thread.words) std::cin>>value;
            std::cin>>vm.active>>host.seed;
            vm.rebuildThreadIndex();
            for (unsigned tick=0;tick<nt;++tick) {
                int elapsed; std::cin>>elapsed;
                host.events.clear(); vm.tick(file,elapsed,host);
                std::cout<<vm.active<<' '<<host.seed;
                for (auto value:vm.statics) std::cout<<' '<<value;
                for (const auto& thread:vm.threads) for (auto value:thread.words) std::cout<<' '<<value;
                std::cout<<' '<<host.events.size();
                for (auto value:host.events) std::cout<<' '<<value;
                std::cout<<'\n';
            }
        }
        return 0;
    }
    {
        using namespace tak::sim;
        auto require=[](bool ok,const char* message) {if(!ok)throw std::runtime_error(message);};
        {
            UnitType monarch, soldier, building;monarch.id="zonhunt";
            monarch.maxHp=soldier.maxHp=building.maxHp=100;
            monarch.maxVel=soldier.maxVel=Fixed::fromInt(1);
            building.maxVel={};building.canMove=true; // authored Keep flag does not make it mobile
            World emotes;TypeRegistry registry;
            const int king=emotes.spawn(&monarch,100,100,0,0);
            const int a=emotes.spawn(&soldier,200,100,0,0), b=emotes.spawn(&soldier,300,100,0,0);
            const int enemy=emotes.spawn(&soldier,400,100,0,1);
            const int keep=emotes.spawn(&building,500,100,0,0);
            tak::net::Command cmd;cmd.kind=tak::net::Cmd::Disco;
            const auto hash=emotes.stateHash();
            cmd.unitId=enemy;applyCommand(emotes,registry,cmd);
            cmd.unitId=keep;applyCommand(emotes,registry,cmd);
            require(!emotes.discoTarget(0),"emotes reject foreign units and stationary buildings even with canmove=1");
            emotes.unit(a)->repeatType=&soldier;
            cmd.unitId=a;applyCommand(emotes,registry,cmd);
            cmd.queue=1;cmd.unitId=b;applyCommand(emotes,registry,cmd);
            emotes.unit(a)->repeatType=nullptr;
            require(emotes.discoTarget(0,a) && emotes.discoTarget(0,b) && !emotes.discoTarget(0,king),
                "disco snapshots selected mobile units without adding the monarch");
            require(emotes.stateHash()==hash,"emote targets and triggers do not alter gameplay hash");
            const auto sequence=emotes.player(0).emoteSequence;
            emotes.tick(1.f/30.f);
            cmd.unitId=king;applyCommand(emotes,registry,cmd);
            require(emotes.discoTarget(0,king) && emotes.player(0).emoteSequence==sequence,
                "selection targets split across server ticks keep the same real-time trigger");
            for(int tick=0;tick<2400;++tick) emotes.tick(1.f/30.f);
            require(emotes.player(0).emoteSequence==sequence && emotes.discoTarget(0,a),
                "eight-times-speed simulation ticks cannot expire the viewer's emote");
            cmd.queue=0;cmd.kind=tak::net::Cmd::Headbang;cmd.unitId=0;applyCommand(emotes,registry,cmd);
            require(emotes.headbangTarget(0,king) && !emotes.headbangTarget(0,a) && !emotes.headbangTarget(0,b),
                "empty selection falls back to living monarch only");
            const auto second=emotes.player(0).emoteSequence;
            emotes.unit(king)->hp={};emotes.tick(1.f/30.f);
            cmd.kind=tak::net::Cmd::Disco;applyCommand(emotes,registry,cmd);
            cmd.unitId=king;applyCommand(emotes,registry,cmd);
            require(emotes.player(0).emoteSequence==second,"dead monarch and dead selection cannot trigger an emote");
            cmd.kind=tak::net::Cmd::Headbang;cmd.unitId=a;applyCommand(emotes,registry,cmd);
            cmd.queue=1;cmd.unitId=b;applyCommand(emotes,registry,cmd);
            cmd.unitId=keep;applyCommand(emotes,registry,cmd);
            require(emotes.headbangTarget(0,a) && emotes.headbangTarget(0,b) && !emotes.headbangTarget(0,keep),
                "headbang includes selected mobile units but excludes buildings");
            for(int speed : {0,1,4,8}) {
                tak::EmoteTiming timing;
                require(timing.observe(1,1000),"new emote starts the real-time track");
                const auto trigger=emotes.player(0).emoteSequence;
                uint64_t ticks=0;
                for(uint64_t elapsed=0;elapsed<=12000;elapsed+=250) {
                    const auto simulatedTicks=elapsed*uint64_t(speed)*30/1000;
                    while(ticks<simulatedTicks) {emotes.tick(1.f/30.f);++ticks;}
                    require(emotes.player(0).emoteSequence==trigger,"simulation speed cannot advance the emote trigger");
                    require(!timing.observe(1,1000+elapsed),"repeated snapshots and late selection batches do not restart music");
                    require(timing.active(1000+elapsed)==(elapsed<10000),"emote lasts exactly ten real seconds at every speed and pause");
                    require(timing.seconds(1000+elapsed)==float(elapsed)/1000.f,"animation phase follows real time");
                }
                require(timing.observe(2,14000) && timing.active(14000) && timing.seconds(14000)==0,
                    "a subsequent emote resets animation and music together");
            }
        }
        auto file=std::make_shared<tak::cob::File>();
        file->pieces={"root","muzzle"};file->scripts={{"Emit",0}};
        file->code={0x10021001,257,0x1000f000,1,
                    0x10021001,4*65536,0x1000b000,1,0,
                    0x10021001,258,0x1000f000,1,0x10065000};
        UnitType type;type.simulationScript=file;type.maxHp=100;
        type.productionModel={{{0,0,0},-1,0},{{32*65536,6*65536,12*65536},0,1}};
        World world;
        const int id=world.spawn(&type,200,200);auto* unit=world.unit(id);
        unit->groundY=Fixed::fromInt(50);unit->heading=Bam(32768);
        RetailReplayProbe::emitScript(world,id);
        const auto events=world.scriptEmissions();
        require(events.size()==2,"script emissions preserve multiple events in one notification");
        require(events[0].position==std::array<int32_t,3>{168*65536,56*65536,212*65536},
                "script emission captures instruction-time world position");
        require(events[0].position!=events[1].position,"piece movement separates successive emission origins");
        require(events[0].tick==world.tickCount() && events[0].unitId==id && events[0].code==257 && events[1].code==258,
                "script emission records owner, tick and code in order");
        const auto hash=world.stateHash();RetailReplayProbe::clearScriptEvents(world);
        require(world.stateHash()==hash,"cosmetic script event queue is not hashed");
        file->code={0x10021001,2,0x1000f000,1,
                    0x10021001,9*65536,0x1000b000,1,0,
                    0x10021001,5,0x1000f000,1,0x10065000};
        type.productionModel[1].emissionVertexCount=2;
        type.productionModel[1].emissionVertices={{{65536,0,0},{0,0,65536}}};
        RetailReplayProbe::emitScript(world,id);
        const auto points=world.scriptEmissions();
        require(points.size()==2 && points[0].code==2 && points[1].code==5,
                "point effects capture both instructions");
        require(points[0].pose.size()==2 && points[1].pose.size()==2,
                "point effects capture complete root-to-leaf pose");
        require(points[0].pose[1].move[0]!=points[1].pose[1].move[0] &&
                points[1].pose[1].move[0]==9*65536,
                "point effect poses retain instruction-time movement");
        require(points[0].vertices==type.productionModel[1].emissionVertices &&
                points[0].position==std::array<int32_t,3>{200*65536,50*65536,200*65536},
                "point effects retain authored vertices and body position separately");
        const auto pointHash=world.stateHash();RetailReplayProbe::clearScriptEvents(world);
        require(world.stateHash()==pointHash,"point pose capture remains unhashed");
        std::cout<<"PASS: instruction-time script effect events and cosmetic hash isolation\n";
    }
    {
        // WE A1: a world whose idle COB VMs skip their sleeping ticks hashes, and after paying
        // holds, exactly what a world whose VMs run every tick does. B's VMs are touched before
        // every tick, so they never skip. Sleeps of 1000 ms and 50-59 ms (RAND), and an outside
        // start (Poke) that lands mid-sleep and pays the debt.
        using namespace tak::sim;
        auto require=[](bool ok,const char* message) {if(!ok)throw std::runtime_error(message);};
        auto file=std::make_shared<tak::cob::File>();
        file->numStatics=2;file->scripts={{"Create",0},{"Poke",21}};
        file->code={0x10021001,1000,0x10013000,                 // sleep 1000 ms
                    0x10021004,0,0x10021001,1,0x10031000,0x10023004,0,   // static0 += 1
                    0x10021001,0,0x10021001,9,0x10041000,               // rand(0,9)
                    0x10021001,50,0x10031000,0x10013000,                // sleep 50+rand ms
                    0x10064000,0,                                       // loop
                    0x10021001,7,0x10023004,1,0x10065000};              // Poke: static1 = 7
        UnitType type;type.simulationScript=file;type.maxHp=100;
        World skipping,running;
        std::vector<int> ids;
        for (int k=0;k<5;++k) {
            ids.push_back(skipping.spawn(&type,100.f+40*k,200));
            require(running.spawn(&type,100.f+40*k,200)==ids.back(),"both worlds give the same ids");
        }
        uint64_t owed=0;uint32_t maxDebt=0;
        for (int tick=0;tick<900;++tick) {
            if (tick%37==11) {
                RetailReplayProbe::notifyScript(skipping,ids[size_t(tick%5)],"Poke");
                RetailReplayProbe::notifyScript(running,ids[size_t(tick%5)],"Poke");
            }
            for (int id:ids) RetailReplayProbe::touchScript(running,id);
            skipping.tick(1.f/30.f);running.tick(1.f/30.f);
            for (int id:ids) {
                const uint32_t debt=RetailReplayProbe::scriptDebt(skipping,id);
                owed+=debt;maxDebt=std::max(maxDebt,debt);
            }
            require(skipping.stateHash()==running.stateHash(),"a skipped VM's debt folds into stateHash exactly");
        }
        require(owed>0 && maxDebt>=20,"idle VMs skip their sleeping ticks");
        for (int id:ids) {
            const auto& a=RetailReplayProbe::paidVm(skipping,id);
            const auto& b=RetailReplayProbe::paidVm(running,id);
            require(a.statics==b.statics && a.statics[0]>0,"paid VMs ran every wake-up");
            for (size_t i=0;i<a.threads.size();++i)
                require(a.threads[i].words==b.threads[i].words,"paying a debt equals running the skipped ticks");
        }
        std::cout<<"PASS: COB sleep skip (max debt "<<maxDebt<<" ticks) hashes and pays like running every tick\n";
    }
    {
        using namespace tak::sim;
        auto require=[](bool ok,const char* message) {if(!ok)throw std::runtime_error(message);};
        for (bool lightning : {false, true}) for (bool flag : {false, true})
        for (bool available : {false, true}) {
            tak::sim::UnitType type, victim;
            type.maxHp=victim.maxHp=1000;type.hasNimbusArt=available;
            tak::sim::Weapon weapon;weapon.range=400;weapon.reload=10;weapon.damage=1;
            weapon.projVel=300;weapon.straight=!lightning;weapon.lightning=lightning;
            weapon.nimbus=flag;weapon.buildUp=0.25f;weapon.buildUpTicks=11;weapon.nimbusTicks=7;
            type.weapon=weapon;type.weapons={weapon};
            tak::sim::World world;
            world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int from=world.spawn(&type,200,200,0,0),target=world.spawn(&victim,350,200,0,1);
            RetailReplayProbe::shoot(world,from,target);
            require(!world.projectiles().empty(),"nimbus delay fixture creates a projectile");
            if(world.projectiles().empty())continue;
            const auto shot=world.projectiles().front();
            require(shot.start==world.tickCount()+1+uint32_t(flag && available ? 7 : 0),
                  "nimbustime (not builduptime) delays launch, given both nimbus flag and faction art");
            while(world.tickCount()<shot.start) {
                RetailReplayProbe::straightTick(world,world.tickCount()+1);
                require(!world.projectiles().empty() && world.projectiles().front().position==shot.position,
                      "projectile remains at its muzzle through the nimbus delay and activation");
            }
            RetailReplayProbe::straightTick(world,world.tickCount()+1);
            require(!world.projectiles().empty() && world.projectiles().front().position!=shot.position,
                  "projectile begins motion after activation");
        }
        for(bool startArt:{false,true}) for(bool endArt:{false,true}) {
            UnitType type,victim;type.maxHp=victim.maxHp=1000;type.hasNimbusArt=true;
            Weapon weapon;weapon.kind=Weapon::Kind::Wandering;weapon.nimbus=true;weapon.buildUpTicks=9;weapon.nimbusTicks=2;
            weapon.durationTicks=3;weapon.variationTicks=99;weapon.projVel=30;
            weapon.reload=100;weapon.damage=1;weapon.aoe=80;weapon.range=400;
            if(startArt)weapon.wanderStartTicks={2,1};
            weapon.wanderLoopTicks={2};
            if(endArt)weapon.wanderEndTicks={2,1};
            type.weapons={weapon};type.weapon=weapon;
            World stormWorld;
            stormWorld.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int from=stormWorld.spawn(&type,200,200,0,0),target=stormWorld.spawn(&victim,234,200,0,1);
            // A model-less mover has no support quad to stand on the terrain,
            // and the storm drifts at terrain height: 529dc0 measures in 3D.
            stormWorld.unit(target)->groundY=Fixed::fromInt(100);
            stormWorld.setStance(from,2);stormWorld.setStance(target,2);
            RetailReplayProbe::shoot(stormWorld,from,target);
            const auto origin=stormWorld.storms().front().x;
            const uint32_t activeAt=startArt ? 6 : 3;
            for(uint32_t tick=1;tick<=activeAt;++tick) {
                stormWorld.tick(1.f/30.f);
                require(stormWorld.storms().size()==1 && stormWorld.storms().front().x==origin,
                        "storm does not drift during waiting/start animation/activation");
                require(stormWorld.unit(target)->hp==Fixed::fromInt(1000),"startup cannot damage target");
            }
            require(stormWorld.storms().front().phase==World::Storm::Phase::Active &&
                    stormWorld.storms().front().end==activeAt+3,"storm duration starts after start art ends");
            stormWorld.unit(from)->hp={};
            for(unsigned step=1;step<=3;++step) {
                const auto before=stormWorld.unit(target)->hp;
                stormWorld.tick(1.f/30.f);
                require(stormWorld.unit(target)->hp<before,"active storm damages through expiry tick after caster death");
            }
            const auto after=stormWorld.unit(target)->hp;
            if(endArt) {
                require(stormWorld.storms().size()==1 && stormWorld.storms().front().phase==World::Storm::Phase::Ending,
                        "expiry starts ending art");
                auto previous=stormWorld.storms().front().x;
                for(unsigned step=0;step<2;++step) {
                    stormWorld.tick(1.f/30.f);
                    require(stormWorld.storms().size()==1 && stormWorld.storms().front().x>previous,
                            "ending storm continues moving");
                    require(stormWorld.unit(target)->hp==after,"ending storm cannot damage");
                    previous=stormWorld.storms().front().x;
                }
                stormWorld.tick(1.f/30.f);
            }
            require(stormWorld.storms().empty(),"storm retires after authored ending duration");
            require(stormWorld.unit(target)->hp==after,"retirement cannot damage");
        }
        {
            UnitType type,victim;type.hasNimbusArt=true;
            Weapon weapon;weapon.kind=Weapon::Kind::Wandering;weapon.nimbus=true;weapon.nimbusTicks=5;
            type.weapons={weapon};World stormWorld;
            stormWorld.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int from=stormWorld.spawn(&type,200,200,0,0),target=stormWorld.spawn(&victim,300,200,0,1);
            RetailReplayProbe::shoot(stormWorld,from,target);stormWorld.unit(from)->hp={};
            stormWorld.tick(1.f/30.f);
            require(stormWorld.storms().empty(),"caster death during waiting cancels storm");
        }
        {
            UnitType type,victim;type.maxHp=victim.maxHp=1000;
            Weapon weapon;weapon.kind=Weapon::Kind::Wandering;weapon.durationTicks=100;
            weapon.variationTicks=1;weapon.maxVariation=2;weapon.projVel=30;weapon.reload=100;
            weapon.wanderLoopTicks={2};type.weapons={weapon};type.weapon=weapon;
            World storms;
            storms.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int from=storms.spawn(&type,200,200,0,0),target=storms.spawn(&victim,400,300,0,1);
            storms.setStance(from,2);storms.setStance(target,2);
            RetailReplayProbe::shoot(storms,from,target);RetailReplayProbe::shoot(storms,from,target);
            unsigned sharedVariationDraws=0;
            storms.setRngObserver([&](const World::RngObservation& observation) {
                if(observation.bound==2001)++sharedVariationDraws;
            });
            for(unsigned tick=0;tick<10;++tick) {
                storms.tick(1.f/30.f);
                require(storms.storms().size()==2,"both private-stream storms remain active");
                const auto& a=storms.storms()[0];const auto& b=storms.storms()[1];
                require(a.wanderSeed==b.wanderSeed && a.velocity==b.velocity && a.x==b.x && a.z==b.z,
                        "identically aimed storms do not perturb one another's random stream");
            }
            require(sharedVariationDraws==0,"wandering no longer consumes fire-spread RNG samples");
        }
        {
            UnitType type,victim;type.maxHp=victim.maxHp=1000;
            Weapon weapon;weapon.kind=Weapon::Kind::Wandering;weapon.durationTicks=100;
            weapon.variationTicks=99;weapon.projVel=960;weapon.reload=100;weapon.wanderLoopTicks={2};
            type.weapons={weapon};World storms;
            storms.setTerrain(std::vector<uint8_t>(8*8,100),8,8,20);
            const int from=storms.spawn(&type,96,64,0,0),target=storms.spawn(&victim,112,64,0,1);
            storms.setStance(from,2);storms.setStance(target,2);
            RetailReplayProbe::shoot(storms,from,target);
            require(storms.storms().front().substeps==2 && storms.storms().front().baseVelocity[0]==32*65536,
                    "storm retains native quantized speed and substep count");
            storms.tick(1.f/30.f);storms.tick(1.f/30.f);
            require(storms.storms().front().x==Fixed::fromInt(192),
                    "storm integrates native substeps without clamping at the map edge");
        }
        std::cout<<"PASS: native storm waiting/start/active/end timeline and owner-death gates\n";
        {
            // Damageable is independent of reclaimable and flamable: authored
            // walls must enter the simulation even if builders cannot clear them.
            tak::hpi::Vfs vfs;auto files=std::make_shared<tak::hpi::Vfs::Files>();
            const std::string text=R"(
                [Wall] {
                    footprintx=4;
                    footprintz=4;
                    blocking=1;
                    damage=1200;
                    featuredead=Rubble;
                }
                [Rubble] {
                    footprintx=1;
                    footprintz=1;
                    blocking=0;
                    indestructible=1;
                }
            )";
            (*files)["features/wall.tdf"]={text.begin(),text.end()};vfs.setMapFiles(files);
            tak::tnt::Map map;map.width=map.height=64;map.heights.assign(64*64,10);
            map.features.assign(64*64,0xffff);map.featureNames={"Wall"};
            const int id=20*64+20;map.features[id]=0;
            World walls;walls.setTerrain(map.heights,64,64,0);registerMapFeatures(walls,map,vfs);
            require(walls.feature(id) && !walls.featureReclaimable(*walls.feature(id)),
                "non-reclaimable nonflammable wall is tracked for damage, not reclaim");
            UnitType builder;builder.isBuilder=builder.canMove=builder.canReclaim=true;
            builder.maxHp=100;builder.maxVel=Fixed::fromInt(1);
            const int worker=walls.spawn(&builder,280,280,0,0);
            walls.reclaim(worker,id,false);
            require(walls.unit(worker)->orders.empty(),"registering damage-only scenery does not enable reclaim orders");
            Weapon shot;shot.damage=600;
            RetailReplayProbe::hit(walls,shot,376,376,worker,0);
            require(walls.feature(id)->dmg==600,"nonflammable wall takes ordinary tail-cell damage");
            RetailReplayProbe::hit(walls,shot,376,376,worker,0);
            require(!walls.feature(id)->blocks && walls.featureTypes()[walls.feature(id)->type].name=="rubble",
                "damage-only wall changes to its authored nonblocking wreck stage");
        }
        // Death-spawned wrecks retain unit-owned visuals, but weapon hits must
        // damage their feature footprint and leave the authored neutral rubble.
        for(bool splash:{false,true}) for(bool permanent:{false,true}) {
            World wrecks;
            wrecks.setTerrain(std::vector<uint8_t>(64*64,10),64,64,0);
            wrecks.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
            FeatType body;body.name="keep-wreck";body.fx=body.fz=5;body.hp=1000;
            body.blocking=true;body.reclaimable=true;body.deadType=1;body.indestructible=permanent;
            FeatType rubble;rubble.name="rubble";rubble.fx=rubble.fz=1;rubble.hp=100;
            wrecks.setFeatureTypes({body,rubble});
            UnitType keep;keep.id="keep";keep.maxHp=100;keep.footX=keep.footZ=5;
            wrecks.mapCorpse(&keep,0);
            const int dead=wrecks.spawn(&keep,360,360,0,0);
            require(RetailReplayProbe::corpse(wrecks,dead,0),"install dead building footprint");
            const int anchor=20*64+20;
            Weapon shot;shot.damage=500;shot.aoe=splash?200:0;
            // Direct outer footprint hit, or splash whose impact is outside it.
            const float x=splash?420.f:392.f,z=392.f;
            const auto before=wrecks.stateHash();
            shot.unitsOnly=true;RetailReplayProbe::hit(wrecks,shot,x,z,0,0);
            require(wrecks.stateHash()==before,"unit-only weapons do not damage wreckage");
            shot.unitsOnly=false;RetailReplayProbe::hit(wrecks,shot,x,z,0,0);
            require(wrecks.unit(dead)->corpseUntil>0,"partial damage preserves corpse/reclaim target");
            require((wrecks.stateHash()!=before)==!permanent,"corpse damage is hashed and indestructibility respected");
            RetailReplayProbe::hit(wrecks,shot,x,z,0,0);
            if(permanent) {
                require(wrecks.unit(dead)->corpseUntil>0,"indestructible corpse survives lethal hits");
                continue;
            }
            require(wrecks.unit(dead)->corpseUntil==0,"destroyed wreck cannot be resurrected/reclaimed as original unit");
            require(wrecks.feature(anchor) && wrecks.feature(anchor)->alive &&
                    wrecks.feature(anchor)->type==1 && wrecks.feature(anchor)->dmg==0,
                    "authored smaller rubble replaces wreck without taking same hit twice");
            RetailReplayProbe::retire(wrecks,dead);
            require(wrecks.feature(anchor)->alive,"retiring old corpse cannot delete replacement rubble");
            shot.aoe=0;RetailReplayProbe::hit(wrecks,shot,328,328,0,0);
            require(!wrecks.feature(anchor)->alive,"replacement rubble can also be destroyed");
            // Reuse the anchor after reclaim/expiry and ensure no stale corpse hit index.
            const int second=wrecks.spawn(&keep,360,360,0,0);
            require(RetailReplayProbe::corpse(wrecks,second,0),"reuse destroyed corpse anchor");
            RetailReplayProbe::retire(wrecks,second);
            RetailReplayProbe::hit(wrecks,shot,392,392,0,0);
            require(wrecks.unit(second)->corpseUntil==0,"retired corpse anchor no longer receives damage");
        }
        {
            World bodies;
            bodies.setTerrain(std::vector<uint8_t>(64*64,10),64,64,0);
            bodies.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
            FeatType body;body.name="archer-corpse";body.fx=body.fz=2;body.hp=440;
            body.blocking=false;body.reclaimable=true;body.resurrectable=true;
            bodies.setFeatureTypes({body});
            UnitType archer;archer.maxHp=100;archer.footX=archer.footZ=2;
            const int dead=bodies.spawn(&archer,352,352,0,0);
            require(RetailReplayProbe::corpse(bodies,dead,0),"install nonblocking mobile corpse");
            Weapon shot;shot.damage=440;
            RetailReplayProbe::hit(bodies,shot,360,360,0,0);
            require(bodies.unit(dead)->corpseUntil==0 && bodies.features().empty(),
                    "mobile corpse without featuredead is removed without invented rubble");
        }
        // A large Taros tree must take a direct hit anywhere in its footprint,
        // even when neither its anchor nor its center is inside the splash radius.
        for (int size : {2,5}) for (float aoe : {0.f,160.f}) {
            World trees;
            trees.setTerrain(std::vector<uint8_t>(64*64,10),64,64,0);
            trees.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
            FeatType tree;tree.name="tartree";tree.fx=tree.fz=size;tree.hp=1000;
            tree.blocking=true;tree.projectileHeight=140;tree.deadType=1;
            FeatType stump;stump.name="smudge";stump.indestructible=true;
            trees.setFeatureTypes({tree,stump});
            const int id=20*64+20;
            trees.addFeature(id,320+size*8,320+size*8,0,size,size,true,0);
            const float hit=320+(size-1)*16+8;
            Weapon shot;shot.damage=400;shot.aoe=aoe;
            shot.unitsOnly=true;RetailReplayProbe::hit(trees,shot,hit,hit,0,0);
            require(trees.feature(id)->dmg==0,"units-only spells leave footprint tails undamaged");
            shot.unitsOnly=false;
            RetailReplayProbe::hit(trees,shot,hit,hit,0,0);
            require(trees.feature(id)->dmg==400,"direct tail hit damages the tree exactly once, including with splash");
            RetailReplayProbe::hit(trees,shot,hit,hit,0,0);
            require(trees.feature(id)->dmg==800,"successive outer-footprint hits accumulate");
            RetailReplayProbe::hit(trees,shot,hit,hit,0,0);
            require(trees.feature(id)->type==1 && !trees.feature(id)->blocks,
                "outer-footprint damage destroys the tree into its nonblocking smudge");
            UnitType walker;walker.maxHp=100;walker.maxVel=Fixed::fromInt(1);walker.maxWaterDepth=255;
            require(trees.canPlace(&walker,hit,hit),"destroyed tree releases the outer footprint for movement");
            RetailReplayProbe::hit(trees,shot,328,328,0,0);
            require(trees.feature(id)->type==1 && trees.feature(id)->dmg==0,
                "indestructible nonblocking smudge survives further fire");
        }

        {
            UnitType shooter;shooter.maxHp=100;
            Weapon arrow;arrow.ballistic=true;arrow.shotModel="araarrow";arrow.projVel=530;
            arrow.range=550;arrow.damage=476;arrow.reload=100;
            shooter.weapons={arrow};shooter.weapon=arrow;
            UnitType targetType;targetType.maxHp=2000;targetType.footX=targetType.footZ=2;
            targetType.modelTop=20*65536;
            targetType.projectileQuad=RetailCollisionQuad{{{-10*65536,-10*65536},{10*65536,-10*65536},
                {10*65536,10*65536},{-10*65536,10*65536}}};
            World world;world.setTerrain(std::vector<uint8_t>(64*64,10),64,64,0);
            RetailMapFeatureType mapTree;mapTree.name="tree";mapTree.projectileHeight=255;
            std::vector<uint16_t> raw(64*64,0xffff);const int treeId=25*64+28;
            raw[size_t(treeId)]=0;world.setMapPlacementFeatures(raw,{mapTree});
            FeatType tree;tree.name="tree";tree.hp=10000;tree.projectileHeight=255;
            world.setFeatureTypes({tree});world.addFeature(treeId,448,400,0,1,1,false,0,false);
            const int from=world.spawn(&shooter,400,400,0,0);
            const int target=world.spawn(&targetType,520,400,0,1);
            world.unit(from)->groundY=world.unit(target)->groundY=Fixed::fromInt(20);
            world.setStance(from,2);world.setStance(target,2);
            RetailReplayProbe::shoot(world,from,target);
            require(world.projectiles().size()==1 && world.projectiles()[0].ballistic3d,
                "Arabow-style authored arrow uses its 3D ballistic path");
            for(unsigned tick=0;tick<20 && world.feature(treeId)->dmg==0 &&
                    world.unit(target)->hp==Fixed::fromInt(2000);++tick) {
                world.tick(1.f/30.f);
                if(!world.projectiles().empty() && !world.projectiles()[0].spent) {
                    const auto& shot=world.projectiles()[0];
                    require(shot.x.v==shot.position[0] && shot.z.v==shot.position[2],
                        "ballistic 2D impact point tracks its native 3D trajectory");
                }
            }
            require(world.feature(treeId)->dmg==476 && world.unit(target)->hp==Fixed::fromInt(2000),
                "ballistic arrow stops at the feature top, damages the feature, and never reaches its selected unit target");
        }
        {
            UnitType shooter,targetType;shooter.maxHp=100;targetType.maxHp=2000;
            Weapon arrow;arrow.ballistic=true;arrow.shotModel="araarrow";arrow.projVel=530;
            arrow.range=550;arrow.damage=476;arrow.reload=100;
            shooter.weapons={arrow};shooter.weapon=arrow;
            targetType.footX=targetType.footZ=4;targetType.modelTop=80*65536;
            targetType.projectileQuad=RetailCollisionQuad{{{-24*65536,-24*65536},{24*65536,-24*65536},
                {24*65536,24*65536},{-24*65536,24*65536}}};
            World world;
            world.setTerrain(std::vector<uint8_t>(64*64,10),64,64,0);
            world.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
            FeatType tree;tree.name="tartree09";tree.fx=tree.fz=5;
            tree.hp=1000;tree.projectileHeight=140;tree.blocking=true;
            world.setFeatureTypes({tree});const int treeId=21*64+28;
            world.addFeature(treeId,488,376,0,5,5,true,0);
            const int from=world.spawn(&shooter,400,400,0,0);
            const int target=world.spawn(&targetType,600,400,0,1);
            world.unit(from)->groundY=Fixed::fromInt(30);world.unit(target)->groundY=Fixed::fromInt(10);
            world.setStance(from,2);world.setStance(target,2);
            for(int shot=0;shot<4;++shot) {
                RetailReplayProbe::shoot(world,from,target);
                for(int tick=0;tick<25;++tick) world.tick(1.f/30.f);
                if(shot<2) require(world.feature(treeId)->dmg==476*(shot+1),
                    "real projectile trajectory damages the outer row of a 5x5 tree");
                if(shot<3) require(world.unit(target)->hp==Fixed::fromInt(2000),
                    "tree shields the target until destroyed");
            }
            require(!world.feature(treeId)->alive && world.unit(target)->hp<Fixed::fromInt(2000),
                "after destroying a footprint-tail obstruction, subsequent shots reach the unit behind it");
        }

        {
            UnitType shooter;shooter.maxHp=100;
            Weapon cannon;cannon.ballistic=true;cannon.weaponArt="cannbmed";cannon.projVel=530;
            cannon.range=550;cannon.damage=476;cannon.reload=100;
            shooter.weapons={cannon};shooter.weapon=cannon;
            UnitType targetType;targetType.maxHp=2000;targetType.footX=targetType.footZ=2;
            targetType.modelTop=20*65536;
            targetType.projectileQuad=RetailCollisionQuad{{{-10*65536,-10*65536},{10*65536,-10*65536},
                {10*65536,10*65536},{-10*65536,10*65536}}};
            World world;world.setTerrain(std::vector<uint8_t>(64*64,10),64,64,0);
            RetailMapFeatureType mapTree;mapTree.name="tree";mapTree.projectileHeight=255;
            std::vector<uint16_t> raw(64*64,0xffff);const int treeId=25*64+28;
            raw[size_t(treeId)]=0;world.setMapPlacementFeatures(raw,{mapTree});
            FeatType tree;tree.name="tree";tree.hp=10000;tree.projectileHeight=255;
            world.setFeatureTypes({tree});world.addFeature(treeId,448,400,0,1,1,false,0,false);
            const int from=world.spawn(&shooter,400,400,0,0);
            const int target=world.spawn(&targetType,520,400,0,1);
            world.unit(from)->groundY=world.unit(target)->groundY=Fixed::fromInt(20);
            world.setStance(from,2);world.setStance(target,2);
            RetailReplayProbe::shoot(world,from,target);
            require(world.projectiles().size()==1 && world.projectiles()[0].ballistic3d &&
                    world.projectiles()[0].wsrc->shotModel.empty() &&
                    world.projectiles()[0].wsrc->weaponArt=="cannbmed" &&
                    world.projectiles()[0].life==std::numeric_limits<int32_t>::max(),
                "sprite-only BallisticWeapon uses native XYZ flight without a mesh or range expiry");
            const auto launch=world.projectiles()[0].position;
            for(unsigned tick=0;tick<20 && world.feature(treeId)->dmg==0 &&
                    world.unit(target)->hp==Fixed::fromInt(2000);++tick) {
                world.tick(1.f/30.f);
                if(!world.projectiles().empty() && !world.projectiles()[0].spent) {
                    const auto& shot=world.projectiles()[0];
                    require(shot.position[1]!=launch[1] && shot.x.v==shot.position[0] &&
                            shot.z.v==shot.position[2],
                        "sprite-only ballistic sprite follows the live 3D arc and collision position");
                }
            }
            require(world.feature(treeId)->dmg==476 && world.unit(target)->hp==Fixed::fromInt(2000),
                "sprite-only ballistic cannon collides with a blocking feature before its selected unit target");
        }
        {
            UnitType shooter;shooter.maxHp=100;
            Weapon arrow;arrow.ballistic=true;arrow.shotModel="araarrow";arrow.projVel=750;
            arrow.range=450;arrow.damage=213;arrow.reload=100;shooter.weapons={arrow};shooter.weapon=arrow;
            UnitType targetType;targetType.maxHp=2000;targetType.footX=targetType.footZ=2;
            targetType.modelTop=80*65536;
            targetType.projectileQuad=RetailCollisionQuad{{{-20*65536,-20*65536},{20*65536,-20*65536},
                {20*65536,20*65536},{-20*65536,20*65536}}};
            World world;world.setTerrain(std::vector<uint8_t>(64*64,10),64,64,0);
            RetailMapFeatureType mapTree;mapTree.name="tree";mapTree.projectileHeight=255;
            std::vector<uint16_t> raw(64*64,0xffff);const int treeId=25*64+28;
            raw[size_t(treeId)]=0;world.setMapPlacementFeatures(raw,{mapTree});
            FeatType tree;tree.name="tree";tree.hp=10000;tree.projectileHeight=255;
            world.setFeatureTypes({tree});world.addFeature(treeId,448,400,0,1,1,false,0,false);
            const int from=world.spawn(&shooter,225,400,0,0);
            const int target=world.spawn(&targetType,520,400,0,1);
            world.unit(from)->groundY=world.unit(target)->groundY=Fixed::fromInt(20);
            world.setStance(from,2);world.setStance(target,2);
            RetailReplayProbe::shoot(world,from,target);
            require(world.projectiles().size()==1 && world.projectiles()[0].ballistic3d,
                "Araarch-speed arrow enters World ballistic flight");
            const auto launch=world.projectiles()[0];
            require(launch.position==std::array<int32_t,3>{225*65536,20*65536,400*65536} &&
                    launch.velocity==std::array<int32_t,3>{819000,20100,0} &&
                    launch.angles==std::array<uint16_t,3>{0,16384,306} && launch.substeps==2,
                "World Araarch-speed launch matches native manager probe state");
            struct Snapshot { std::array<int32_t,3> position,velocity;std::array<uint16_t,3> angles; };
            constexpr std::array<Snapshot,8> nativeFlight{{
                {{{16383600,1344806,26214400}},{{819000,16024,0}},{{0,16384,204}}},
                {{{18021600,1370740,26214400}},{{819000,11948,0}},{{0,16384,152}}},
                {{{19659600,1388522,26214400}},{{819000,7872,0}},{{0,16384,100}}},
                {{{21297600,1398152,26214400}},{{819000,3796,0}},{{0,16384,48}}},
                {{{22935600,1399630,26214400}},{{819000,-280,0}},{{0,16384,65533}}},
                {{{24573600,1392956,26214400}},{{819000,-4356,0}},{{0,16384,65481}}},
                {{{26211600,1378130,26214400}},{{819000,-8432,0}},{{0,16384,65429}}},
                {{{27849600,1355152,26214400}},{{819000,-12508,0}},{{0,16384,65377}}},
            }};
            for(unsigned tick=1;tick<=9;++tick) {
                world.tick(1.f/30.f);
                if(tick<9) {
                    require(world.projectiles().size()==1 && world.feature(treeId)->dmg==0 &&
                            world.unit(target)->hp==Fixed::fromInt(2000),
                        "World keeps the matching native projectile active through tick 8");
                    const auto& shot=world.projectiles()[0];const auto& reference=nativeFlight[tick-1];
                    require(shot.position==reference.position && shot.velocity==reference.velocity &&
                            shot.angles==reference.angles,
                        "World ballistic state matches the native manager at every active tick");
                }
            }
            require(world.projectiles().empty() && world.feature(treeId)->dmg==213 &&
                    world.unit(target)->hp==Fixed::fromInt(2000),
                "World feature impact and projectile retirement match native manager tick 9");
            std::cout<<"PASS: Araarch-speed World projectile matches native pool trajectory and retires on tick 9\n";
        }
        std::cout<<"PASS: authored ballistic arrows collide with features before their selected unit target\n";
        std::cout<<"PASS: sprite-only ballistic shots use native XYZ motion and feature collision\n";
        {
            UnitType bomber;bomber.maxHp=100;bomber.canFly=true;bomber.defaultFire=0;
            Weapon egg;egg.kind=Weapon::Kind::Dropped;egg.ballistic=true;
            egg.projVel=10;egg.subSteps=1;egg.range=900;egg.damage=25;egg.noLead=true;
            bomber.weapons={egg};bomber.weapon=egg;
            UnitType targetType;targetType.maxHp=100;targetType.footX=targetType.footZ=2;
            targetType.modelTop=20*65536;
            targetType.projectileQuad=RetailCollisionQuad{{{-16*65536,-16*65536},
                {16*65536,-16*65536},{16*65536,16*65536},{-16*65536,16*65536}}};
            World dropped;dropped.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
            dropped.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
            const int bomberId=dropped.spawn(&bomber,400,400,0,0);
            const int targetId=dropped.spawn(&targetType,800,400,0,1);
            dropped.unit(bomberId)->flightY=Fixed::fromInt(100);
            dropped.setGameSeed(0);
            const auto expected=retailDroppedBallisticLaunch(
                {400*65536,100*65536,400*65536},{800*65536,0,400*65536},
                dropped.ballisticGravityRaw());
            uint32_t damageRoll=0;
            int damageDraws=0;
            dropped.setCrtRngObserver([&](const World::RngObservation& row) {
                if (row.retailReturnAddress==0x52a3ba) {
                    if (damageDraws==0) damageRoll=row.result;
                    ++damageDraws;
                }
            });
            RetailReplayProbe::shoot(dropped,bomberId,targetId);
            require(dropped.projectiles().size()==1 && dropped.projectiles()[0].ballistic3d,
                "DroppedBallistic creates a native XYZ projectile");
            const auto& shot=dropped.projectiles()[0];
            require(shot.position==expected.position && shot.velocity==expected.velocity &&
                    shot.angles==expected.angles && shot.substeps==1 && shot.flight==40,
                "dropped launch uses the authored QueryWeapon muzzle and gravity-derived native state");
            unsigned impacts=0;
            for(unsigned tick=0;tick<80 && !dropped.projectiles().empty();++tick) {
                dropped.tick(1.f/30.f);
                impacts+=unsigned(dropped.hits().size());
                for(const auto& hit:dropped.hits()) {
                    require(hit.position.has_value() &&
                        Fixed::raw((*hit.position)[0]).toFloat()==hit.x &&
                        Fixed::raw((*hit.position)[2]).toFloat()==hit.z &&
                        (*hit.position)[1]>0 && (*hit.position)[1]<=20*65536,
                        "dropped impact captures projectile contact XYZ above the target origin");
                    const auto saved=*hit.position;
                    const auto oldGround=dropped.unit(targetId)->groundY;
                    dropped.unit(targetId)->groundY=Fixed::fromInt(400);
                    require(*hit.position==saved,"impact position survives later target height changes");
                    dropped.unit(targetId)->groundY=oldGround;
                }
            }
            const int expectedDamage=retailDamageWithSpread(25.0f,damageRoll);
            require(impacts==1 && damageDraws==1 &&
                    dropped.unit(targetId)->hp==Fixed::fromInt(100-expectedDamage) &&
                    dropped.projectiles().empty(),
                "dropped ballistic reaches and damages its ground target once, with no expiry detonation");
            std::cout<<"PASS: gravity-driven dropped ballistic path, target impact and one-shot retirement\n";
        }
        for(bool blocked:{false,true}) {
            UnitType shooter;shooter.maxHp=100;
            Weapon weapon;weapon.beam=true;weapon.straight=true;weapon.projVel=240;
            weapon.range=240;weapon.damage=10;weapon.reload=100;weapon.shotSpin={65530,17,33};
            shooter.weapons.push_back(weapon);shooter.weapon=weapon;
            UnitType targetType;targetType.maxHp=100;targetType.footX=targetType.footZ=2;
            targetType.modelTop=20*65536;
            targetType.projectileQuad=RetailCollisionQuad{{{-10*65536,-10*65536},{10*65536,-10*65536},
                {10*65536,10*65536},{-10*65536,10*65536}}};
            World world;world.setTerrain(std::vector<uint8_t>(64*64,20),64,64,0);
            if(blocked) {
                RetailMapFeatureType tree;tree.name="tree";tree.projectileHeight=60;
                std::vector<uint16_t> features(64*64,0xffff);features[25*64+28]=0;
                world.setMapPlacementFeatures(features,{tree});
            }
            const int from=world.spawn(&shooter,400,400,0,0),target=world.spawn(&targetType,480,400,0,1);
            world.unit(from)->groundY=Fixed::fromInt(50);world.unit(target)->groundY=Fixed::fromInt(50);
            RetailReplayProbe::shoot(world,from,target);
            require(world.projectiles().size()==1 && world.unit(target)->hp==Fixed::fromInt(100),"straight shot does not damage instantly");
            const auto origin=world.projectiles()[0].position;
            RetailReplayProbe::straightTick(world,1);
            require(world.projectiles()[0].position==origin,"activation does not advance straight shot");
            world.unit(from)->hp=Fixed();world.unit(from)->deadFor=0;
            uint32_t tick=2;
            for(;tick<40 && !world.projectiles()[0].spent;++tick)RetailReplayProbe::straightTick(world,tick);
            const auto& shot=world.projectiles()[0];
            require(shot.spent && tick>3,"released straight shot survives owner death and collides after flight");
            const auto after=world.unit(target)->hp;
            require(blocked ? after==Fixed::fromInt(100) : after<Fixed::fromInt(100),"tree blocks ordinary shot damage");
            require(shot.angles[0]==uint16_t(uint32_t(65530)*(tick-2)),"model rotation advances each substep");
            RetailReplayProbe::straightTick(world,tick);
            require(world.projectiles()[0].life==0 && world.unit(target)->hp==after,"ordinary impact is dispatched once and shot expires next tick");
        }
        std::cout<<"PASS: ordinary straight shots fly in 3D, survive shooter death, collide and expire\n";
        for(uint32_t activationTick:{0u,1u}) {
            UnitType shooter;shooter.maxHp=100;
            Weapon weapon;weapon.beam=true;weapon.straight=true;weapon.projVel=240;weapon.range=240;
            shooter.weapons.push_back(weapon);UnitType victim;victim.maxHp=100;
            World world;world.setTerrain(std::vector<uint8_t>(64*64,20),64,64,0);
            const int from=world.spawn(&shooter,400,400,0,0),target=world.spawn(&victim,480,400,0,1);
            world.unit(from)->groundY=Fixed::fromInt(50);world.unit(target)->groundY=Fixed::fromInt(50);
            RetailReplayProbe::shoot(world,from,target);world.unit(from)->hp=Fixed();
            RetailReplayProbe::straightTick(world,activationTick);
            require(world.projectiles().size()==1 && world.projectiles()[0].life==0,
                "ordinary shot cancels for pending-death owner before or on activation");
        }

        for(bool killCaster:{false,true}) {
            UnitType shooter;shooter.maxHp=100;
            Weapon weapon;weapon.beam=true;weapon.lightning=true;weapon.projVel=240;
            weapon.range=240;weapon.damage=10;weapon.reload=100;
            auto definition=std::make_shared<tak::RetailLightningDefinition>();
            auto& effect=definition->initial;
            effect.width=16;effect.height=8;effect.capacity=16;
            effect.intensity=4*256;effect.decay=-256;effect.pixels.resize(128);
            effect.sources.push_back({{256,3*256},{14*256,3*256},1,0,false});
            weapon.lightningEffect=definition;shooter.weapons.push_back(weapon);shooter.weapon=weapon;
            UnitType victim;victim.maxHp=100;victim.footX=victim.footZ=2;victim.modelTop=20*65536;
            victim.projectileQuad=RetailCollisionQuad{{{-10*65536,-10*65536},{10*65536,-10*65536},
                {10*65536,10*65536},{-10*65536,10*65536}}};
            World world;world.setTerrain(std::vector<uint8_t>(64*64,20),64,64,0);
            const int from=world.spawn(&shooter,400,400,0,0),target=world.spawn(&victim,480,400,0,1);
            world.unit(from)->groundY=Fixed::fromInt(50);world.unit(target)->groundY=Fixed::fromInt(50);
            RetailReplayProbe::shoot(world,from,target);
            require(world.projectiles().size()==1 && world.unit(target)->hp==Fixed::fromInt(100),
                "lightning does not damage before its advancing head reaches the target");
            RetailReplayProbe::straightTick(world,1);RetailReplayProbe::straightTick(world,2);
            require(!world.projectiles()[0].lightningEffect->particles.empty(),"live lightning emits named particles");
            if(killCaster) {
                world.unit(from)->hp=Fixed();RetailReplayProbe::straightTick(world,3);
                require(world.projectiles()[0].life==0 && world.unit(target)->hp==Fixed::fromInt(100),
                    "lightning cancels on caster death even after activation");
            } else {
                uint32_t tick=3;
                for(;tick<40 && !world.projectiles()[0].spent;++tick)RetailReplayProbe::straightTick(world,tick);
                require(world.projectiles()[0].spent && world.unit(target)->hp<Fixed::fromInt(100),"lightning collides after travel");
                const auto hp=world.unit(target)->hp;
                RetailReplayProbe::straightTick(world,tick++);
                require(world.projectiles()[0].life>0,"lightning remains while its particles drain");
                for(;tick<50 && world.projectiles()[0].life>0;++tick)RetailReplayProbe::straightTick(world,tick);
                require(world.projectiles()[0].life==0 && world.unit(target)->hp==hp,"lightning fades without repeating damage");
            }
        }

        for(bool blocked:{false,true}) {
            UnitType shooter;shooter.maxHp=100;
            Weapon weapon;weapon.beam=true;weapon.flameKind=0;weapon.projVel=240;
            weapon.range=240;weapon.emitTime=30;weapon.damage=10;weapon.reload=100;
            shooter.weapons.push_back(weapon);shooter.weapon=weapon;
            UnitType targetType;targetType.maxHp=100;targetType.footX=targetType.footZ=2;
            targetType.modelTop=20*65536;
            targetType.projectileQuad=RetailCollisionQuad{{{-10*65536,-10*65536},{10*65536,-10*65536},
                {10*65536,10*65536},{-10*65536,10*65536}}};
            World world;world.setTerrain(std::vector<uint8_t>(64*64,20),64,64,0);
            if(blocked) {
                RetailMapFeatureType tree;tree.name="tree";tree.projectileHeight=60;
                std::vector<uint16_t> features(64*64,0xffff);features[25*64+28]=0;
                world.setMapPlacementFeatures(features,{tree});
            }
            const int from=world.spawn(&shooter,400,400,0,0),target=world.spawn(&targetType,480,400,0,1);
            world.unit(from)->groundY=Fixed::fromInt(50);world.unit(target)->groundY=Fixed::fromInt(50);
            RetailReplayProbe::shoot(world,from,target);
            require(world.flames().size()==1 && world.unit(target)->hp==Fixed::fromInt(100),"flame launch does not apply instant damage");
            const auto retainedAim=world.flames()[0].aimPoint;
            require(retainedAim[0]==480*65536 && world.flames()[0].muzzle[0]==400*65536,
                "flame retains independent launch aim and muzzle geometry");
            RetailReplayProbe::flameTick(world,1);
            const auto duration=world.flames()[0].lifetime;
            require(duration>1 && world.flames()[0].particles.empty(),"flame initial scan emits and damages nothing");
            for(uint32_t tick=2;tick<=1+duration;++tick) {
                if(tick==3)world.unit(from)->x=Fixed::fromInt(404);
                RetailReplayProbe::flameTick(world,tick);
                require(!world.flames().empty() && !world.flames()[0].particles.empty(),"active flame emits particles");
                require(world.flames()[0].particles.back().position[0]==world.unit(from)->x.v,
                    "new particle follows the current muzzle, not the launch point");
                require(world.flames()[0].muzzle==world.flames()[0].particles.back().position &&
                        world.flames()[0].aimPoint==retainedAim,
                    "stream admission follows the current muzzle but retains its captured aim");
                if(tick<1+duration)require(world.unit(target)->hp==Fixed::fromInt(100),"flame cannot damage before front arrival");
            }
            require(world.flames()[0].impacted,"flame front records a collision");
            const auto after=world.unit(target)->hp;
            require(blocked ? after==Fixed::fromInt(100) : after<Fixed::fromInt(100),"tree blocks flame damage; unobstructed unit takes the hit");
            for(uint32_t tick=2+duration;tick<12+duration;++tick)RetailReplayProbe::flameTick(world,tick);
            require(world.unit(target)->hp==after,"flame applies only one impact during continued emission");
            // An already released flame is not cancelled by boarding a carrier.
            world.unit(from)->inTransport=target;world.unit(from)->x=Fixed::fromInt(410);
            RetailReplayProbe::flameTick(world,12+duration);
            require(world.flames()[0].particles.back().position[0]==410*65536,
                "embarkation does not cancel an already released flame");
            world.unit(from)->hp=Fixed();
            const auto retainedMuzzle=world.flames()[0].muzzle;
            world.unit(from)->x=Fixed::fromInt(420);
            auto expectedParticles=world.flames()[0].particles;
            std::erase_if(expectedParticles,[](auto& particle){return !particle.tick();});
            RetailReplayProbe::flameTick(world,13+duration);
            require(!world.flames().empty() && world.flames()[0].particles.size()==expectedParticles.size(),
                "pending-death flame only drains existing particles");
            require(world.flames()[0].muzzle==retainedMuzzle && world.flames()[0].aimPoint==retainedAim,
                "draining stream keeps its last muzzle and original aim after owner death");
            for(size_t i=0;i<expectedParticles.size();++i) {
                const auto& actual=world.flames()[0].particles[i];const auto& expected=expectedParticles[i];
                require(actual.remaining==expected.remaining && actual.position==expected.position &&
                    actual.velocity==expected.velocity,"pending-death flame cannot replace an expiring particle with a fresh one");
            }
            for(uint32_t tick=14+duration;tick<50+duration;++tick)RetailReplayProbe::flameTick(world,tick);
            require(world.flames().empty(),"particles drain and flame disappears after owner death");
        }
        std::cout<<"PASS: live flame delays one impact, stops at trees, follows muzzle and drains after death\n";
        World terrain;
        std::vector<uint8_t> heights(32*32,10);heights[11*32+11]=30;
        terrain.setTerrain(heights,32,32,20);
        RetailMapFeatureType tree;tree.name="tree";tree.footX=tree.footZ=2;
        tree.projectileHeight=40;
        std::vector<uint16_t> features(32*32,0xffff);features[10*32+10]=0;
        terrain.setMapPlacementFeatures(features,{tree});
        auto collision=[&](int x,int y,int z,uint32_t flags=0) {
            int32_t speed=-19;
            return std::pair{terrain.projectileEnvironment({x,y,z},speed,flags),speed};
        };
        require(collision(160*65536,50*65536,160*65536)==std::pair{2,-19},"feature anchor contact includes its top");
        require(collision(176*65536,50*65536,176*65536)==std::pair{2,-19},"feature tail resolves anchor height above cell minimum");
        require(collision(176*65536,51*65536,176*65536)==std::pair{0,-19},"feature top uses minimum, not raised corner");
        require(collision(176*65536,10*65536,176*65536,0x1000)==std::pair{2,-19},"feature takes precedence over ground bounce");
        require(collision(176*65536,10*65536,176*65536,0x800)==std::pair{0,-19},"units-only bypasses map features");
        require(collision(208*65536,10*65536,208*65536,0x1000)==std::pair{0,4},"clear terrain bounces vertical velocity");
        require(collision(208*65536,20*65536-1,208*65536)==std::pair{2,-19},"fraction below sea impacts water");
        require(collision(-1,50*65536,160*65536)==std::pair{1,-19},"negative fractional position is outside map");
        require(collision(512*65536,50*65536,160*65536)==std::pair{1,-19},"far map boundary is outside map");
        terrain.setNoSeaLevelTrigger(true);
        require(collision(208*65536,20*65536-1,208*65536)==std::pair{0,-19},"map no-sea-trigger flag bypasses water");
        require(collision(208*65536,10*65536,208*65536)==std::pair{2,-19},"map no-sea-trigger flag retains terrain impacts");
        std::cout<<"PASS: world projectile environment resolves terrain and multi-cell feature collision heights\n";
        UnitType type;type.maxHp=100;type.modelTop=20*65536;
        type.projectileQuad=RetailCollisionQuad{{{-10*65536,-10*65536},{10*65536,-10*65536},
            {10*65536,10*65536},{-10*65536,10*65536}}};
        World world;world.setTerrain(std::vector<uint8_t>(32*32,10),32,32,0);
        world.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
        const int id=world.spawn(&type,200,200);auto* u=world.unit(id);
        u->heading=retailHeadingToPort(0);u->groundY=Fixed::fromInt(50);
        require(world.projectilePointInUnit(id,{200*65536,60*65536,200*65536}),"world collision uses model quad and body height");
        require(!world.projectilePointInUnit(id,{210*65536,60*65536,200*65536}),"selection-quad edge is excluded");
        require(!world.projectilePointInUnit(id,{200*65536,71*65536,200*65536}),"shot above body misses");
        int32_t vy=0;
        const std::array<int32_t,3> bodyPoint{200*65536,60*65536,200*65536};
        require(world.projectileCollision(bodyPoint,vy,0,1,{}).unitId==id,"primary cell routes to model collision");
        require(world.projectileCollision(bodyPoint,vy,0,0,{}).code==0,"same-owner primary body is excluded");
        require(world.projectileCollision(bodyPoint,vy,0,0,{},false,bodyPoint,1).code==2,
            "interception proximity precedes unit ownership");
        type.canFly=true;u->flightY=Fixed::fromInt(100);
        require(world.projectilePointInUnit(id,{200*65536,110*65536,200*65536}) &&
            !world.projectilePointInUnit(id,{200*65536,60*65536,200*65536}),"flying collision uses authoritative altitude");
        require(!world.projectilePointInUnit(999,{0,0,0}),"absent unit has no collision body");
        u->flightGroundMode=2;
        type.projectileQuad=RetailCollisionQuad{{{-2*65536,-2*65536},{2*65536,-2*65536},
            {2*65536,2*65536},{-2*65536,2*65536}}};
        const auto airborne=world.updateAirOccupancy();
        require(airborne[12*32+12]==id,"airborne footprint enters the secondary grid");
        require(world.projectileCollision({206*65536,120*65536,200*65536},vy,0,1,airborne).unitId==id,
            "secondary footprint uses inclusive height without a selection-quad test");
        require(world.projectileCollision({206*65536,120*65536+1,200*65536},vy,0,1,airborne).code==0,
            "secondary body excludes a point above its top");
        require(world.projectileCollision({206*65536,110*65536,200*65536},vy,0,0,airborne).code==0,
            "same-owner aircraft is excluded");
        std::cout<<"PASS: world projectile point query uses native model quad and altitude\n";
    }
    {
        using namespace tak::sim;
        auto require=[](bool ok,const char* message) {if(!ok)throw std::runtime_error(message);};
        {
            UnitType monarch;monarch.maxHp=1000;monarch.commander=true;
            UnitType soldier;soldier.maxHp=1000;
            World alerts;alerts.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
            int primary=alerts.spawn(&soldier,200,200,0,1);
            int ruler=alerts.spawn(&monarch,216,200,0,1);
            alerts.tick(1.f/30); // build the spatial index before testing splash
            Weapon blast;blast.damage=100;blast.aoe=64;blast.dmgType=3;
            RetailReplayProbe::hit(alerts,blast,200,200,0,primary);
            require(alerts.hits().back().attackedMonarchPlayers==2,
                "splash monarch damage records owner even when primary is not a monarch");
            alerts.clearHits();blast.aoe=0;blast.damage=0;
            RetailReplayProbe::hit(alerts,blast,216,200,0,ruler);
            require(alerts.hits().back().attackedMonarchPlayers==0,"zero damage does not alert");
            blast.damage=2000;
            RetailReplayProbe::hit(alerts,blast,216,200,0,ruler);
            require(alerts.hits().back().attackedMonarchPlayers==2,"fatal monarch hit retains alarm");
            alerts.tick(1.f/30);
            require(alerts.hits().empty(),"monarch events clear with tick impacts");
        }
        auto hitFile=std::make_shared<tak::cob::File>();
        hitFile->numStatics=4;hitFile->scripts={{"HitByWeapon",0}};
        for(uint32_t i=0;i<4;++i)
            hitFile->code.insert(hitFile->code.end(),{0x10021002,i,0x10023004,i});
        hitFile->code.push_back(0x10065000);
        UnitType victimType;victimType.maxHp=1000;victimType.simulationScript=hitFile;
        World impacts;
        impacts.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        const int direct=impacts.spawn(&victimType,300,200,0.f,1);
        const int splash=impacts.spawn(&victimType,320,200,0.f,1);
        impacts.tick(1.f/30); // populate the spatial index used by splash recipients
        impacts.unit(direct)->armBuff=2;
        Weapon blast;blast.damage=100;blast.aoe=100;blast.edge=0;blast.dmgType=3;
        RetailReplayProbe::hit(impacts,blast,300,200,0,direct);
        for(int id:{direct,splash}) {
            const auto* victim=impacts.unit(id);
            const auto& callbacks=victim->weaponAnimations;
            require(callbacks.count==1 && callbacks.at(0).kind==tak::RetailWeaponAnimation::Hit,
                "direct and splash damage each publish one HitByWeapon callback");
            const auto& hit=callbacks.at(0);
            require(hit.slot==3 && hit.damage==1000-victim->hp.floorInt() && hit.damage!=100,
                "flinch captures resolved recipient damage rather than the nominal weapon value");
            require(hit.heading==(id==direct?32768:16384),
                "flinch bearing comes from impact position and victim heading at the hit");
            require(RetailReplayProbe::scriptStatics(impacts,id)[0]==0,
                "damage queues the simulation callback without running it inline");
        }
        const uint16_t directDamage=impacts.unit(direct)->weaponAnimations.at(0).damage;
        impacts.unit(direct)->heading=Bam(12345);
        impacts.tick(1.f/30);
        require(RetailReplayProbe::scriptStatics(impacts,direct)==std::vector<uint32_t>{3,uint32_t(-400),0,directDamage},
            "script receives captured hit arguments despite subsequent victim rotation");
        for(int id:{direct,splash})impacts.unit(id)->weaponAnimations.clear();
        blast.status=Weapon::Status::Paralyzed;
        RetailReplayProbe::hit(impacts,blast,300,200,0,direct);
        require(impacts.unit(direct)->weaponAnimations.count==0 && impacts.unit(splash)->weaponAnimations.count==0,
            "status damage does not produce direct or splash flinch callbacks");

        auto takeoffFile=std::make_shared<tak::cob::File>();
        takeoffFile->numStatics=2;
        takeoffFile->scripts={{"Activate",0},{"BeginFlight",8}};
        takeoffFile->code={0x10021004,0,0x10021001,1,0x10031000,0x10023004,0,0x10065000,
                          0x10021004,1,0x10021001,1,0x10031000,0x10023004,1,0x10065000};
        UnitType flyingBuilder;flyingBuilder.maxHp=100;flyingBuilder.canFly=true;
        flyingBuilder.canMove=true;flyingBuilder.isBuilder=true;
        flyingBuilder.maxVel=Fixed::fromInt(2);flyingBuilder.accel=Fixed::fromFloat(0.5f);
        flyingBuilder.brake=Fixed::fromFloat(0.2f);flyingBuilder.cruiseAlt=150;
        flyingBuilder.buildDist=100;flyingBuilder.simulationScript=takeoffFile;
        UnitType placedOutput;placedOutput.maxHp=100;placedOutput.buildTime=10000;
        World takeoffWorld;
        takeoffWorld.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
        const int flyingWorker=takeoffWorld.spawn(&flyingBuilder,1000,1000,0);
        takeoffWorld.queueBuild(flyingWorker,&placedOutput,1120,1056,false);
        takeoffWorld.tick(1.f/30);
        require(RetailReplayProbe::scriptStatics(takeoffWorld,flyingWorker)==std::vector<uint32_t>{1,1},
            "placed flying build activates and requests BeginFlight in the authoritative script");
        require(takeoffWorld.unit(flyingWorker)->flightBeginCallbackSerial==1 &&
            takeoffWorld.unit(flyingWorker)->flightGroundMode==2,
            "placed build publishes one takeoff callback before construction starts");
        for(int tick=0;tick<4;++tick)takeoffWorld.tick(1.f/30);
        require(RetailReplayProbe::scriptStatics(takeoffWorld,flyingWorker)==std::vector<uint32_t>{1,1},
            "ongoing placed construction does not restart takeoff callbacks");

        auto repairFile=std::make_shared<tak::cob::File>();
        repairFile->numStatics=4;
        repairFile->scripts={{"StartBuilding",0},{"StopBuilding",18}};
        repairFile->code={0x10022000,0x10022000,
            0x10021002,0,0x10023004,0,0x10021002,1,0x10023004,1,
            0x10021004,2,0x10021001,1,0x10031000,0x10023004,2,0x10065000,
            0x10021004,3,0x10021001,1,0x10031000,0x10023004,3,0x10065000};
        UnitType repairer;repairer.maxHp=100;repairer.isBuilder=true;repairer.canMove=true;
        repairer.workerTime=1;repairer.simulationScript=repairFile;
        UnitType damaged;damaged.maxHp=100;damaged.buildTime=100;damaged.buildCost=100;
        World repairWorld;
        repairWorld.setTerrain(std::vector<uint8_t>(32*32,100),32,32,20);
        const int worker=repairWorld.spawn(&repairer,200,200,0);
        const int repaired=repairWorld.spawn(&damaged,220,200,0);
        repairWorld.unit(repaired)->hp=Fixed::fromInt(50);
        repairWorld.player(0).mana=0;
        const uint16_t repairHeading=uint16_t(retailDirection(Fixed::fromInt(-20),Fixed()).v-
            portHeadingToRetail(repairWorld.unit(worker)->heading));
        // RepairUnit's approach (407190 stages 0-1): the worker already
        // touches the target, so its rectangle reports arrival on the first
        // update; the next RepairUnit wake starts the work.
        repairWorld.repair(worker,repaired,false);
        repairWorld.tick(1.f/30);
        require(!repairWorld.unit(worker)->workScriptWorking && repairWorld.unit(worker)->repairId==repaired,
            "repair approaches before requesting its pose");
        RetailReplayProbe::repairTick(repairWorld,worker);
        require(repairWorld.unit(worker)->workScriptWorking &&
            RetailReplayProbe::scriptStatics(repairWorld,worker)[2]==0,
            "repair requests its pose before mana payment and defers script execution");
        RetailReplayProbe::scriptTick(repairWorld,worker);
        require(RetailReplayProbe::scriptStatics(repairWorld,worker)==
            std::vector<uint32_t>{repairHeading,1,1,0},
            "repair StartBuilding receives captured relative heading and stance arguments");
        RetailReplayProbe::repairTick(repairWorld,worker);
        RetailReplayProbe::scriptTick(repairWorld,worker);
        require(RetailReplayProbe::scriptStatics(repairWorld,worker)[2]==1,
            "ongoing repair does not restart its pose each update");
        repairWorld.unit(repaired)->hp=Fixed::fromInt(100);
        RetailReplayProbe::repairTick(repairWorld,worker);
        require(!repairWorld.unit(worker)->workScriptWorking &&
            RetailReplayProbe::scriptStatics(repairWorld,worker)[3]==1,
            "completed repair stops its controller");
        repairWorld.unit(repaired)->hp=Fixed::fromInt(50);
        repairWorld.repair(worker,repaired,false);
        repairWorld.tick(1.f/30);
        RetailReplayProbe::repairTick(repairWorld,worker);
        RetailReplayProbe::scriptTick(repairWorld,worker);
        repairWorld.cancelBuilds(worker);repairWorld.cancelBuilds(worker);
        require(!repairWorld.unit(worker)->workScriptWorking &&
            RetailReplayProbe::scriptStatics(repairWorld,worker)[2]==2 &&
            RetailReplayProbe::scriptStatics(repairWorld,worker)[3]==2,
            "repair cancellation stops its controller exactly once");

        // The worker already touches the feature: RECLAIM's arrival is
        // immediate, and it then waits for INBUILDSTANCE (406250 stage 2),
        // which this counting script never sets on its own.
        repairer.canReclaim=true;
        repairWorld.addFeature(99,220,200,0,1,1,false,-1,false);
        repairWorld.reclaim(worker,99,false);
        for(int tick=0;tick<3;++tick)repairWorld.tick(1.f/30);
        require(repairWorld.unit(worker)->workScriptWorking &&
            RetailReplayProbe::scriptStatics(repairWorld,worker)[2]==3,
            "feature reclaim starts its authored controller even with no mana yield");
        for(int tick=0;tick<3;++tick)repairWorld.tick(1.f/30);
        require(RetailReplayProbe::scriptStatics(repairWorld,worker)[2]==3 &&
            repairWorld.unit(worker)->orders.size()==1 &&
            repairWorld.unit(worker)->orders.front().mission.stage==2,
            "ongoing reclaim does not restart its pose and waits for its stance");
        RetailReplayProbe::setStance(repairWorld,worker,true);
        for(int tick=0;tick<20;++tick)repairWorld.tick(1.f/30);
        require(!repairWorld.unit(worker)->workScriptWorking &&
            RetailReplayProbe::scriptStatics(repairWorld,worker)[3]==3 &&
            repairWorld.feature(99)->fadeLeft>0,
            "completed reclaim stops its controller");
        repairWorld.addFeature(100,220,200,0,1,1,false,-1,false);
        repairWorld.reclaim(worker,100,false);
        for(int tick=0;tick<4;++tick)repairWorld.tick(1.f/30);
        require(repairWorld.unit(worker)->workScriptWorking,"the next reclaim is working");
        repairWorld.cancelBuilds(worker);repairWorld.cancelBuilds(worker);
        require(!repairWorld.unit(worker)->workScriptWorking &&
            RetailReplayProbe::scriptStatics(repairWorld,worker)[3]==4,
            "reclaim cancellation stops its controller exactly once");

        TypeRegistry workRegistry;
        tak::net::Command workCommand;workCommand.unitId=worker;workCommand.player=0;
        workCommand.kind=tak::net::Cmd::Repair;workCommand.targetId=repaired;
        applyCommand(repairWorld,workRegistry,workCommand);
        for(int tick=0;tick<4;++tick)repairWorld.tick(1.f/30);
        require(repairWorld.unit(worker)->repairId==repaired &&
            repairWorld.unit(worker)->workScriptWorking,
            "Repair command reaches its controller through the ordinary update loop");
        const auto firstWorkCounts=RetailReplayProbe::scriptStatics(repairWorld,worker);
        const int replacement=repairWorld.spawn(&damaged,220,210,0);
        repairWorld.unit(replacement)->hp=Fixed::fromInt(50);
        workCommand.targetId=replacement;applyCommand(repairWorld,workRegistry,workCommand);
        require(!repairWorld.unit(worker)->workScriptWorking &&
            RetailReplayProbe::scriptStatics(repairWorld,worker)[3]==firstWorkCounts[3]+1,
            "replacement Repair command retires the previous controller immediately");
        for(int tick=0;tick<4;++tick)repairWorld.tick(1.f/30);
        require(repairWorld.unit(worker)->repairId==replacement &&
            RetailReplayProbe::scriptStatics(repairWorld,worker)[2]==firstWorkCounts[2]+1,
            "replacement target starts its own repair controller once");
        // 40762d: work stops on a target that moves (its MoveRate tier; a
        // unit without a script, its speed), never on distance.
        repairWorld.unit(replacement)->speed=Fixed::fromInt(1);
        RetailReplayProbe::repairTick(repairWorld,worker);
        require(!repairWorld.unit(worker)->workScriptWorking,
            "repair controller retires when its target starts moving");
        repairWorld.unit(replacement)->speed=Fixed();
        for(int tick=0;tick<30 && !repairWorld.unit(worker)->workScriptWorking;++tick)repairWorld.tick(1.f/30);
        require(repairWorld.unit(worker)->workScriptWorking,"the repair resumes once its target stands still");
        repairWorld.addFeature(101,220,200,0,1,1,false,-1,false);
        workCommand.kind=tak::net::Cmd::Reclaim;workCommand.targetId=101;
        applyCommand(repairWorld,workRegistry,workCommand);
        require(!repairWorld.unit(worker)->workScriptWorking,
            "Reclaim command retires the preceding repair controller");
        for(int tick=0;tick<4;++tick)repairWorld.tick(1.f/30);
        require(repairWorld.unit(worker)->reclaimId==101 &&
            repairWorld.unit(worker)->workScriptWorking,
            "Reclaim command starts its controller through the ordinary update loop");
        const auto beforeStop=RetailReplayProbe::scriptStatics(repairWorld,worker);
        workCommand.kind=tak::net::Cmd::Stop;applyCommand(repairWorld,workRegistry,workCommand);
        applyCommand(repairWorld,workRegistry,workCommand);
        require(!repairWorld.unit(worker)->workScriptWorking &&
            RetailReplayProbe::scriptStatics(repairWorld,worker)[3]==beforeStop[3]+1,
            "Stop command retires the reclaim controller once");

        FeatType body;body.name="work-corpse";body.reclaimable=true;
        FeatType stone=body;stone.name="work-stone";
        FeatType frozen=body;frozen.name="work-frozen";
        repairWorld.setFeatureTypes({body,stone,frozen});
        damaged.corpse=body.name;
        repairWorld.mapCorpse(&damaged,0);
        for(int mode=0;mode<3;++mode) {
            const auto beforeCorpse=RetailReplayProbe::scriptStatics(repairWorld,worker);
            const int corpse=repairWorld.spawn(&damaged,220,200,0);
            // Reclaim works on the installed corpse feature.
            require(RetailReplayProbe::corpse(repairWorld,corpse,mode),"the body installs its corpse feature");
            repairWorld.unit(corpse)->corpseStatue=mode ? mode : -1;
            workCommand.kind=tak::net::Cmd::Reclaim;workCommand.targetId=-corpse;
            applyCommand(repairWorld,workRegistry,workCommand);
            for(int tick=0;tick<2;++tick)repairWorld.tick(1.f/30);
            require(repairWorld.unit(worker)->workScriptWorking &&
                RetailReplayProbe::scriptStatics(repairWorld,worker)[2]==beforeCorpse[2]+1,
                "corpse/stone/frozen Reclaim command starts the authored work controller");
            if(mode==1) {
                workCommand.kind=tak::net::Cmd::Stop;
                applyCommand(repairWorld,workRegistry,workCommand);
                require(repairWorld.unit(corpse)->corpseUntil!=0,
                    "stopping corpse reclaim preserves its unfinished target");
            } else {
                for(int tick=0;tick<60;++tick)repairWorld.tick(1.f/30);
                require(!repairWorld.unit(corpse) || repairWorld.unit(corpse)->corpseUntil==0,
                    "completed corpse reclaim retires its target");
            }
            require(!repairWorld.unit(worker)->workScriptWorking &&
                RetailReplayProbe::scriptStatics(repairWorld,worker)[3]==beforeCorpse[3]+1,
                "corpse reclaim completion or cancellation stops its controller once");
        }

        auto activationFile=std::make_shared<tak::cob::File>();
        activationFile->numStatics=1;
        activationFile->scripts={{"Activate",0},{"Deactivate",8}};
        // Count both transitions so duplicate command notifications are visible.
        activationFile->code={0x10021004,0,0x10021001,1,0x10031000,0x10023004,0,0x10065000,
                               0x10021004,0,0x10021001,10,0x10031000,0x10023004,0,0x10065000};
        UnitType fire;fire.maxHp=100;fire.onOffable=true;fire.activateWhenBuilt=false;
        fire.simulationScript=activationFile;
        World activationWorld;
        activationWorld.setTerrain(std::vector<uint8_t>(32*32,100),32,32,20);
        const int fireId=activationWorld.spawn(&fire,200,200);
        activationWorld.setActive(fireId,true);
        require(RetailReplayProbe::scriptStatics(activationWorld,fireId)[0]==1,
            "non-gate activation starts its authored controller immediately");
        activationWorld.setActive(fireId,true);
        require(RetailReplayProbe::scriptStatics(activationWorld,fireId)[0]==1,
            "repeated activation does not start a duplicate controller");
        activationWorld.setActive(fireId,false);
        activationWorld.setActive(fireId,false);
        require(RetailReplayProbe::scriptStatics(activationWorld,fireId)[0]==11,
            "non-gate deactivation reaches its controller exactly once");

        auto cloakFile=std::make_shared<tak::cob::File>();
        cloakFile->numStatics=1;cloakFile->scripts={{"StartCloaking",0},{"StopCloaking",5}};
        cloakFile->code={0x10021001,1,0x10023004,0,0x10065000,
                         0x10021001,2,0x10023004,0,0x10065000};
        UnitType cloaker;cloaker.maxHp=100;cloaker.canCloak=true;
        cloaker.cloakCost=100000;cloaker.minCloakDist=200;cloaker.simulationScript=cloakFile;
        UnitType enemy;enemy.maxHp=100;
        World cloakWorld;
        cloakWorld.setTerrain(std::vector<uint8_t>(32*32,100),32,32,20);
        const int spy=cloakWorld.spawn(&cloaker,200,200,{},0);
        const int nearby=cloakWorld.spawn(&enemy,240,200,{},1);
        cloakWorld.setCloak(spy,true);
        require(RetailReplayProbe::scriptStatics(cloakWorld,spy)[0]==0,
            "cloak command queues its pose callback without running it inline");
        cloakWorld.tick(1.f/30);
        require(!cloakWorld.unit(spy)->cloaked && RetailReplayProbe::scriptStatics(cloakWorld,spy)[0]==1,
            "StartCloaking reaches the script even when the unit cannot pay to turn invisible");
        cloakWorld.unit(nearby)->hp={};cloakWorld.player(0).mana=0;
        cloakWorld.tick(1.f/30);
        require(!cloakWorld.unit(spy)->cloaked && RetailReplayProbe::scriptStatics(cloakWorld,spy)[0]==1,
            "insufficient cloak mana does not restore the uncloaked script pose");
        cloakWorld.setCloak(spy,false);
        require(RetailReplayProbe::scriptStatics(cloakWorld,spy)[0]==1,
            "decloak callback is also deferred to the script phase");
        cloakWorld.tick(1.f/30);
        require(RetailReplayProbe::scriptStatics(cloakWorld,spy)[0]==2,
            "StopCloaking reaches the script when the requested mode is disabled");

        auto file=std::make_shared<tak::cob::File>();
        file->numStatics=5;file->scripts={{"ReadMovement",0}};
        const int ids[]={28,29,30,33,34};
        for(unsigned i=0;i<5;++i) {
            file->code.insert(file->code.end(),{0x10021001,uint32_t(ids[i]),0x10042000,0x10023004,i});
        }
        file->code.push_back(0x10065000);
        UnitType type;type.simulationScript=file;type.maxHp=100;type.maxVel=Fixed::fromInt(4);
        type.roadMult=Fixed::fromInt(2);type.waterMult=Fixed::fromFloat(0.5f);
        type.turnRate=3000;type.turnInPlaceRate=2000;
        World world;
        world.setTerrain(std::vector<uint8_t>(32*32,100),32,32,20);
        const int id=world.spawn(&type,200,200);auto* u=world.unit(id);
        u->baseSpeed=Fixed::fromInt(4);u->speed=Fixed::fromInt(2);u->heading=Bam(0);
        u->animationTurnBam=1200;
        require(RetailReplayProbe::movementQueries(world,id)==std::vector<uint32_t>{0,50,0,40,0},
            "simulation scripts read native horizontal speed and signed turn percentage");
        u->groundTerrainFlags=0x800;
        u->animationTurnBam=-1800;
        require(RetailReplayProbe::movementQueries(world,id)==std::vector<uint32_t>{0,25,0,uint32_t(-30),1},
            "road query and terrain-adjusted turn and speed reach the script host");
        u->groundTerrainFlags=0x1000;
        u->animationTurnBam=1200;
        require(RetailReplayProbe::movementQueries(world,id)==std::vector<uint32_t>{1,100,0,80,0},
            "water query and terrain-adjusted turn and speed reach the script host");
        u->bodyBlockStreak=2;
        require(RetailReplayProbe::movementQueries(world,id)[1]==0,"blocked mover reports stopped despite retained speed");
        type.canFly=true;u->groundTerrainFlags=0;u->flightVelocity={3*65536,-65536,0};
        u->animationTurnBam=-1500;
        require(RetailReplayProbe::movementQueries(world,id)==std::vector<uint32_t>{0,75,uint32_t(-50),uint32_t(-50),0},
            "flying scripts read horizontal, vertical and turn percentages");
        u->inTransport=99;
        require(RetailReplayProbe::movementQueries(world,id)==std::vector<uint32_t>{0,0,0,0,0},
            "transported scripts report no movement");
        u->inTransport=0;u->bodyBlockStreak=0;type.canFly=false;
        u->groundTerrainFlags=0;u->heading=Bam(0);u->animationTurnBam=0;
        const Bam before=u->heading;world.order(id,100,200,false);world.tick(1.f/30);
        const auto applied=std::bit_cast<int16_t>(uint16_t(u->heading.v-before.v));
        require(applied!=0 && u->animationTurnBam==applied,
            "world publishes the completed mover turn for next-tick GET 33");
        const auto turnMultiplier=u->groundTerrainFlags&0x800 ? type.roadMult :
            u->groundTerrainFlags&0x1000 ? type.waterMult : Fixed::fromInt(1);
        const auto afterTurn=RetailReplayProbe::movementQueries(world,id);
        const auto expectedTurn=uint32_t(retailTurnAnimationPercent(applied,
            uint16_t(type.turnRate),uint16_t(type.turnInPlaceRate),turnMultiplier.v,false));
        require(afterTurn[3]==expectedTurn,
            "the following COB update reads the same signed applied turn");
        const auto turnHash=world.stateHash();++u->animationTurnBam;
        require(world.stateHash()!=turnHash,"the sampled GET 33 input is lockstep state");
        --u->animationTurnBam;
        std::cout<<"PASS: authoritative movement speed, terrain and turn script queries\n";
    }
    {
        using namespace tak::sim;
        auto require=[](bool ok,const char* message) {if(!ok)throw std::runtime_error(message);};
        auto file=std::make_shared<tak::cob::File>();
        file->scripts={{"AimWeapon",0},{"FireWeapon",9},{"TargetCleared",18}};
        file->code={0x10021001,100,0x10013000,0x10021001,22,0x10021002,2,0x10082000,0x10065000,
                    0x10021001,100,0x10013000,0x10021001,23,0x10021002,0,0x10082000,0x10065000,
                    0x10022000,0x10021001,21,0x10021002,0,0x10082000,0x10065000};
        World world;world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType shooter,targetType;shooter.simulationScript=file;shooter.maxHp=targetType.maxHp=1000;
        shooter.maxMana=20;
        Weapon weapon;weapon.range=400;weapon.reload=10;weapon.damage=1;weapon.projVel=300;weapon.manaCost=3;
        shooter.weapons.push_back(weapon);shooter.weapon=weapon;
        const int sid=world.spawn(&shooter,200,200,{},0),tid=world.spawn(&targetType,300,200,{},1);
        world.unit(sid)->mana=20;   // a new unit's pool starts empty (511990)
        unsigned draws=0;world.setCrtRngObserver([&](const World::RngObservation& o){if(o.retailReturnAddress==0x530167)++draws;});
        world.attack(sid,tid,false);
        int callback=0,shot=0,reloadAtCallback=0;
        std::vector<tak::RetailWeaponAnimation::Kind> callbacks;
        for(int tick=1;tick<=20;++tick) {
            world.tick(1.f/30);const auto* u=world.unit(sid);
            for(unsigned i=0;i<u->weaponAnimations.count;++i) {
                const auto& event=u->weaponAnimations.at(i);
                require(event.slot==0,"display callback preserves the selected slot");
                callbacks.push_back(event.kind);
            }
            if(u->fireAnimations) {
                require(!callback,"reload prevents duplicate FireWeapon callbacks");callback=tick;reloadAtCallback=u->reloads[0];
                require(!u->justFired && u->mana==20,"callback precedes projectile and mana deduction");
            }
            if(u->justFired) {require(!shot,"SET 23 produces only one shot");shot=tick;}
        }
        require(callbacks==std::vector<tak::RetailWeaponAnimation::Kind>{tak::RetailWeaponAnimation::Aim,tak::RetailWeaponAnimation::Fire,tak::RetailWeaponAnimation::Aim},
            "display receives aim, fire and the native re-aim after projectile retirement");
        require(callback>1 && shot>callback,"live combat waits for aim acknowledgement then delayed fire acknowledgement");
        require(draws==1 && reloadAtCallback>=240 && reloadAtCallback<360,"callback consumes one native reload draw");
        require(world.unit(sid)->mana==17,"mana is spent when the script releases the projectile");
        {
            // Retail keeps SET 23 pending while the assigned target is just
            // outside weapon range, then releases the same shot when range is
            // admitted again. Keep the target at a controlled relative offset
            // so this covers the weapon gate rather than chase movement.
            UnitType mobileShooter=shooter;mobileShooter.canMove=true;
            mobileShooter.maxVel=Fixed::fromInt(1);mobileShooter.turnInPlaceRate=0;
            UnitType mobileTarget=targetType;mobileTarget.maxVel=Fixed::fromInt(1);
            World rangeLoss;
            rangeLoss.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int from=rangeLoss.spawn(&mobileShooter,200,200,{},0);
            rangeLoss.unit(from)->mana=mobileShooter.maxMana;   // a new unit's pool starts empty (511990)
            const int victim=rangeLoss.spawn(&mobileTarget,300,200,{},1);
            rangeLoss.attack(from,victim,false);
            bool fireCallback=false,pendingRelease=false;
            for(int tick=0;tick<20 && !pendingRelease;++tick) {
                if(fireCallback) {
                    const auto* archer=rangeLoss.unit(from);
                    auto* target=rangeLoss.unit(victim);
                    target->x=archer->x+Fixed::fromInt(401);target->z=archer->z;
                }
                rangeLoss.tick(1.f/30);
                const auto* archer=rangeLoss.unit(from);
                fireCallback=fireCallback || archer->fireAnimations!=0;
                pendingRelease=(archer->weaponAim[0].flags&16)!=0;
            }
            require(fireCallback && pendingRelease,"FireWeapon SET 23 becomes pending beyond weapon range");
            for(int tick=0;tick<8;++tick) {
                const auto* archer=rangeLoss.unit(from);auto* target=rangeLoss.unit(victim);
                target->x=archer->x+Fixed::fromInt(401);target->z=archer->z;
                rangeLoss.tick(1.f/30);
                archer=rangeLoss.unit(from);
                require((archer->weaponAim[0].flags&16) && !archer->justFired &&
                    rangeLoss.projectiles().empty() && archer->mana==20,
                    "out-of-range ticks retain SET 23 without releasing a shot or mana");
            }
            {
                const auto* archer=rangeLoss.unit(from);auto* target=rangeLoss.unit(victim);
                target->x=archer->x+Fixed::fromInt(400);target->z=archer->z;
            }
            rangeLoss.tick(1.f/30);
            const auto* archer=rangeLoss.unit(from);
            require(archer->justFired && !(archer->weaponAim[0].flags&16) &&
                rangeLoss.projectiles().size()==1 && archer->mana==17,
                "returning to the inclusive range boundary releases the one pending shot");
            std::cout<<"PASS: pending script release waits outside native weapon range and resumes at the range boundary\n";
        }
        {
            World edge;
            edge.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            UnitType stationary=shooter;stationary.canMove=false;
            UnitType mobileTarget=targetType;mobileTarget.maxVel=Fixed::fromInt(1);
            const int from=edge.spawn(&stationary,200,200,{},0);
            edge.unit(from)->mana=stationary.maxMana;   // a new unit's pool starts empty (511990)
            const int atRange=edge.spawn(&mobileTarget,600,200,{},1);
            edge.attack(from,atRange,false);edge.tick(1.f/30);
            const auto& inRangeEvents=edge.unit(from)->weaponAnimations;
            require(inRangeEvents.count>0 &&
                    inRangeEvents.events[0].kind==tak::RetailWeaponAnimation::Aim,
                "scripted combat starts AimWeapon at the native inclusive maximum range");

            World outside;
            outside.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int outsideFrom=outside.spawn(&stationary,200,200,{},0);
            outside.unit(outsideFrom)->mana=stationary.maxMana;   // a new unit's pool starts empty (511990)
            const int outsideTarget=outside.spawn(&mobileTarget,601,200,{},1);
            outside.attack(outsideFrom,outsideTarget,false);outside.tick(1.f/30);
            require(outside.unit(outsideFrom)->weaponAnimations.count==0,
                "scripted combat does not start AimWeapon beyond the native maximum range");
        }
        {
            World blocked;
            blocked.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            blocked.blockCells(15,12,1,1,true);
            require(!blocked.nav().losBetween(200,200,300,200,0,0),
                "the blocked-LOS weapon fixture has an actual terrain obstruction");
            UnitType stationary=shooter;stationary.canMove=false;
            const int from=blocked.spawn(&stationary,200,200,{},0);
            blocked.unit(from)->mana=stationary.maxMana;   // a new unit's pool starts empty (511990)
            const int victim=blocked.spawn(&targetType,300,200,{},1);
            blocked.attack(from,victim,false);
            for(int tick=0;tick<12;++tick)blocked.tick(1.f/30);
            require(blocked.unit(from)->weaponAnimations.count==0 &&
                    !blocked.unit(from)->fireAnimations && !blocked.unit(from)->justFired &&
                    blocked.projectiles().empty() && blocked.unit(from)->mana==20,
                "World holds AimWeapon/FireWeapon and projectile release while clear line is blocked");
            blocked.blockCells(15,12,1,1,false);
            bool aimed=false;
            for(int tick=0;tick<4 && !aimed;++tick) {
                blocked.tick(1.f/30);
                const auto& events=blocked.unit(from)->weaponAnimations;
                for(unsigned i=0;i<events.count;++i)
                    aimed|=events.at(i).kind==tak::RetailWeaponAnimation::Aim;
            }
            require(aimed,"clearing the terrain obstruction admits the pending AimWeapon callback");
            std::cout<<"PASS: blocked-LOS World gate suppresses callbacks and projectile until the wall clears\n";
        }
        const int next=world.spawn(&targetType,320,237,{},1);
        const auto expected=world.queryWeaponAim(sid,next,0);
        world.attack(sid,next,false);world.tick(1.f/30);
        const auto& clearEvents=world.unit(sid)->weaponAnimations;
        require(clearEvents.count==1 && clearEvents.events[0].kind==tak::RetailWeaponAnimation::Clear,
            "retargeting queues TargetCleared before a new aim can start");
        world.tick(1.f/30);
        const auto& aimEvents=world.unit(sid)->weaponAnimations;
        require(aimEvents.count==1 && aimEvents.events[0].kind==tak::RetailWeaponAnimation::Aim,
            "the regular script pass clears the old aim before starting the new AimWeapon callback");
        require(aimEvents.events[0].heading==(expected->heading&0xff00) &&
            aimEvents.events[0].pitch==(expected->pitch&0xff00),"display angles use native packet precision");
        world.tick(1.f/30);
        require(world.unit(sid)->weaponAnimations.count==0,"callback events expire on the next simulation update");
        std::cout<<"PASS: live script-timed aim, firing callback, reload and delayed projectile\n";
        for(const auto missing:std::array<std::array<bool,2>,3>{{{true,false},{false,true},{true,true}}}) {
            auto incomplete=std::make_shared<tak::cob::File>(*file);
            if(missing[0])incomplete->scripts[0]={"AbsentAim",0};
            if(missing[1])incomplete->scripts[1]={"AbsentFire",9};
            UnitType type=shooter;type.simulationScript=incomplete;
            World callbacksWorld;
            callbacksWorld.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int from=callbacksWorld.spawn(&type,200,200,{},0),target=callbacksWorld.spawn(&targetType,300,200,{},1);
            callbacksWorld.unit(from)->mana=type.maxMana;   // a new unit's pool starts empty (511990)
            callbacksWorld.attack(from,target,false);
            bool firedCallback=false;
            for(int tick=0;tick<60;++tick) {
                callbacksWorld.tick(1.f/30);
                const auto* unit=callbacksWorld.unit(from);
                firedCallback=firedCallback || unit->fireAnimations!=0;
                require(!unit->justFired && callbacksWorld.projectiles().empty(),
                    "missing weapon callback cannot bypass script acknowledgement and fire instantly");
            }
            require(firedCallback==!missing[0],"missing AimWeapon prevents readiness; missing FireWeapon still starts reload");
            require(callbacksWorld.unit(from)->mana==20,"missing callback does not spend projectile mana");
        }

        for(bool clearAcknowledges:{false,true})for(bool removed:{false,true}) {
            UnitType type=shooter;
            if(!clearAcknowledges) {
                auto absent=std::make_shared<tak::cob::File>(*file);
                absent->scripts[2]={"AbsentTargetCleared",18};type.simulationScript=absent;
            }
            World loss;
            loss.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int from=loss.spawn(&type,200,200,{},0),target=loss.spawn(&targetType,300,200,{},1);
            loss.unit(from)->mana=type.maxMana;   // a new unit's pool starts empty (511990)
            loss.setStance(from,2);loss.attack(from,target,false);loss.tick(1.f/30);
            require(loss.unit(from)->scriptAimTarget==target,"target loss fixture establishes script target");
            loss.unit(from)->weaponAim[0].set(23);
            loss.unit(target)->hp={};if(removed)loss.unit(target)->deadFor=0;
            require(!loss.queryWeaponAim(from,target,0),"aim query rejects dead and pending-death unit targets");
            loss.tick(1.f/30);
            const auto* unit=loss.unit(from);
            require(!unit->justFired && loss.projectiles().empty() && unit->mana==20,
                "target death prevents a pending release from spending mana or creating a shot");
            require(unit->weaponAnimations.count==1 && unit->weaponAnimations.events[0].kind==tak::RetailWeaponAnimation::Clear &&
                unit->scriptAimTarget==0,"dying target is cleared during the same combat update");
            require(unit->weaponAim[0].flags&16,
                "target retirement queues TargetCleared without executing it inline");
            loss.tick(1.f/30);
            require(bool(loss.unit(from)->weaponAim[0].flags&16)==!clearAcknowledges,
                "the regular script pass owns delayed-acknowledgement cancellation");
            require(loss.unit(from)->weaponAnimations.count==0,"target retirement does not repeat its clear callback next tick");
            const int next=loss.spawn(&targetType,320,200,{},1);
            loss.attack(from,next,false);loss.tick(1.f/30);
            require(loss.unit(from)->justFired==!clearAcknowledges,
                "a missing TargetCleared callback preserves the pending shot for the next admitted target");
        }
        {
            UnitType type=shooter;type.weaponSwitching=true;type.weapons.assign(3,weapon);
            World switching;
            switching.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int from=switching.spawn(&type,200,200,{},0);
            switching.unit(from)->mana=type.maxMana;   // a new unit's pool starts empty (511990)
            auto* unit=switching.unit(from);
            switching.setStance(from,2);
            unit->scriptAimTarget=123;unit->weaponAim[0].set(23);
            switching.setWeapon(from,1);
            require(unit->scriptAimTarget==0 && (unit->weaponAim[0].flags&16),
                "weapon switch retires the target but queues its script callback without running inline");
            for(int i=0;i<20;++i)switching.setWeapon(from,(i+2)%3);
            switching.tick(1.f/30);
            require(!(unit->weaponAim[0].flags&16),"next script update executes the old slot's TargetCleared");
            const auto& callbacks=unit->weaponAnimations;
            require(callbacks.count==22 && callbacks.at(0).kind==tak::RetailWeaponAnimation::Clear &&
                callbacks.at(0).slot==0,"command target-clear survives the tick boundary before switch callbacks");
            for(size_t i=1;i<callbacks.count;++i)
                require(callbacks.at(i).kind==tak::RetailWeaponAnimation::Switch && callbacks.at(i).slot==i%3,
                    "repeated weapon selections retain every display callback in order");
            switching.tick(1.f/30);
            require(unit->weaponAnimations.count==0,"command callbacks are published once");
        }
        for(int interruption=0;interruption<3;++interruption) {
            UnitType type=shooter;type.weaponSwitching=true;type.weapons.assign(2,weapon);
            World delayed;
            delayed.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int from=delayed.spawn(&type,200,200,{},0),target=delayed.spawn(&targetType,300,200,{},1);
            delayed.unit(from)->mana=type.maxMana;   // a new unit's pool starts empty (511990)
            delayed.setStance(from,2);delayed.unit(from)->reloads[1]=1000;
            delayed.attack(from,target,false);
            bool callback=false;
            for(int tick=0;tick<30 && !callback;++tick) {
                delayed.tick(1.f/30);callback=delayed.unit(from)->fireAnimations!=0;
            }
            require(callback && !delayed.unit(from)->justFired,"interruption fixture reaches delayed FireWeapon callback");
            if(interruption==0)delayed.stop(from);else delayed.setWeapon(from,1);
            for(int tick=0;tick<12;++tick) {
                delayed.tick(1.f/30);
                require(!delayed.unit(from)->justFired && delayed.projectiles().empty(),
                    "late acknowledgement cannot release while targetless or deselected");
            }
            require(delayed.unit(from)->weaponAim[0].flags&16,"late script acknowledgement remains pending during interruption");
            require(delayed.unit(from)->mana==20,"pending acknowledgement spends no projectile mana");
            delayed.setWeapon(from,0);
            if(interruption==2) {
                delayed.stop(from);delayed.tick(1.f/30);
                require(delayed.unit(from)->weaponAim[0].flags&16,
                    "Stop after switching back has no newly assigned target to retire");
                delayed.tick(1.f/30);
                require(delayed.unit(from)->weaponAim[0].flags&16,
                    "an already-retired target does not queue a second TargetCleared for a late acknowledgement");
            }
            if(interruption!=1)delayed.attack(from,target,false);
            delayed.tick(1.f/30);
            require(delayed.unit(from)->justFired,
                "retargeting or reselection releases SET 23 that arrived after the target-clear callback");
            require(delayed.unit(from)->fireAnimations==0,"resuming a pending release does not start another FireWeapon callback");
            require(delayed.unit(from)->mana==17,"resumed acknowledgement charges mana exactly once");
            require(!(delayed.unit(from)->weaponAim[0].flags&16),"pending release is retired or canceled");
            delayed.tick(1.f/30);
            require(!delayed.unit(from)->justFired,"resumed release is not repeated on the next update");
        }
        for(bool switching:{false,true}) {
            UnitType type=shooter;type.weaponSwitching=switching;type.weapons.assign(3,weapon);
            World clearing;
            clearing.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int from=clearing.spawn(&type,200,200,{},0),first=clearing.spawn(&targetType,300,200,{},1),
                next=clearing.spawn(&targetType,320,240,{},1);
                clearing.unit(from)->mana=type.maxMana;   // a new unit's pool starts empty (511990)
            clearing.setWeapon(from,1);clearing.attack(from,first,false);clearing.tick(1.f/30);
            if(switching) {
                clearing.unit(from)->weaponAim[0].flags=0xe8;
                clearing.unit(from)->weaponAim[2].flags=0xc8;
            }
            clearing.attack(from,next,false);clearing.tick(1.f/30);
            std::vector<int> cleared;
            const auto& events=clearing.unit(from)->weaponAnimations;
            for(unsigned i=0;i<events.count;++i)
                if(events.at(i).kind==tak::RetailWeaponAnimation::Clear)cleared.push_back(events.at(i).slot);
            require(cleared==(switching?std::vector<int>{1}:std::vector<int>{0,1,2}),
                "target-clear callback respects selected versus independent weapon slots");
            if(switching)require(clearing.unit(from)->weaponAim[0].flags==0xe8 &&
                clearing.unit(from)->weaponAim[2].flags==0xc8,
                "retargeting the selected weapon does not reset inactive aim acknowledgements");
        }
        for(bool switching:{false,true}) {
            UnitType type;type.maxHp=100;type.weaponSwitching=switching;
            Weapon gun;gun.range=400;gun.damage=1;gun.switchReloadTicks=13;
            type.weapons.assign(3,gun);type.weapon=gun;
            World timers;
            timers.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int id=timers.spawn(&type,200,200);auto* unit=timers.unit(id);
            unit->reloads[0]=4;unit->reloads[1]=5;unit->reloads[2]=6;
            timers.setWeapon(id,1);timers.tick(1.f/30);
            require(unit->reloads[0]==(switching?4:3) && unit->reloads[1]==(switching?12:4) &&
                unit->reloads[2]==(switching?6:5),"only selected reload advances unless all slots are independent");
            timers.setWeapon(id,2);timers.tick(1.f/30);
            require(unit->reloads[0]==(switching?4:2) && unit->reloads[1]==(switching?12:3) &&
                unit->reloads[2]==(switching?12:4),"switching starts the new weapon's switch reload clock");
            timers.setWeapon(id,0);timers.tick(1.f/30);
            require(unit->reloads[0]==(switching?12:1) && unit->reloads[1]==(switching?12:2) &&
                unit->reloads[2]==(switching?12:3),"returning to a weapon replaces its saved countdown with switch reload");
        }
        {
            // 52af72..52af9a (emulated, probe_mana_fallback.py): after a
            // release a switcher steps down from the fired slot while the
            // slot's cost (531ea0: manapershot / 5193c0) is above the mana
            // left, and selects what it lands on.
            struct Row {int selected;std::array<float,3> cost;double mana;int slot;float vet;bool noVet;};
            const Row rows[]={
                {2,{0,10,30},40,1,0,false},{2,{0,10,30},35,0,0,false},{2,{0,10,30},60,2,0,false},
                {2,{0,10,30},59.5,1,0,false},{2,{0,10,30},25,1,0,false},{2,{5,50,30},40,0,0,false},
                {2,{50,50,30},40,0,0,false},{1,{50,30,0},40,0,0,false},{0,{30,0,0},40,0,0,false},
                {2,{0,0,0},0,2,0,false},{2,{0,10,30},40,2,2.f,false},{2,{0,10,30},40,1,2.f,true}};
            for(const Row& row:rows) {
                UnitType type=shooter;type.weaponSwitching=true;type.maxMana=100;type.noVeteran=row.noVet;
                type.weapons.assign(3,weapon);
                for(int s=0;s<3;++s)type.weapons[size_t(s)].manaCost=row.cost[size_t(s)];
                World table;
                table.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
                const int id=table.spawn(&type,200,200,{},0);auto* unit=table.unit(id);
                unit->weaponSlot=row.selected;unit->vetOverride=row.vet;
                // The mana left after the release paid the fired slot.
                const float paid=RetailReplayProbe::manaShotCost(*unit,row.selected);
                unit->mana=double(paid)<=row.mana ? row.mana-paid : row.mana;
                RetailReplayProbe::manaFallback(table,*unit,row.selected);
                require(unit->weaponSlot==row.slot,"mana fallback lands where the native release does");
                require(row.slot==row.selected || unit->reloads[size_t(row.slot)]==type.weapons[size_t(row.slot)].switchReloadTicks,
                    "the fallback slot starts its switch reload");
            }
            // Live: the script's SET 23 releases on slot 2 and the unit ends on
            // slot 1. A release it cannot pay still leaves (mana 25 < 30: the
            // shot is made, nothing is taken) and still falls back.
            for(bool unpaid:{false,true}) {
                UnitType type=shooter;type.weaponSwitching=true;type.maxMana=100;
                type.weapons.assign(3,weapon);
                type.weapons[0].manaCost=0;type.weapons[1].manaCost=10;type.weapons[2].manaCost=30;
                World live;
                live.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
                const int from=live.spawn(&type,200,200,{},0),target=live.spawn(&targetType,300,200,{},1);
                live.setStance(from,2);live.setWeapon(from,2);
                auto* unit=live.unit(from);unit->mana=40;unit->reloads[2]=0;
                live.attack(from,target,false);
                bool callback=false,released=false;
                for(int tick=0;tick<40 && !released;++tick) {
                    live.tick(1.f/30);unit=live.unit(from);
                    if(unit->fireAnimations && !callback) {callback=true;if(unpaid)unit->mana=25;}
                    released=unit->justFired;
                }
                require(callback && released && live.projectiles().size()==1,"the switcher releases one shot");
                require(unit->mana==(unpaid?25:10),"the release pays only a cost the mana covers");
                require(unit->weaponSlot==1 && unit->reloads[1]==type.weapons[1].switchReloadTicks,
                    "the release falls back to the cheaper weapon in the same update");
                bool cleared=false,switched=false;
                for(unsigned i=0;i<unit->weaponAnimations.count;++i) {
                    cleared|=unit->weaponAnimations.at(i).kind==tak::RetailWeaponAnimation::Clear && unit->weaponAnimations.at(i).slot==2;
                    switched|=unit->weaponAnimations.at(i).kind==tak::RetailWeaponAnimation::Switch && unit->weaponAnimations.at(i).slot==1;
                }
                require(cleared && switched,"the fallback clears the fired slot and starts SwitchWeapon on the new one");
                require(!(unit->missionEvents&0x40u),"a walker's unit attack is not given the 0x40 event");
            }
            // A walker's unit attack (Attack_Chase) in reach: the switch's 0x40
            // wakes it in the same dispatch, and its fire stage gives the new
            // slot the target at once (51a700; E, probe_attack_chase_stop.py
            // --switch-tick): the new slot's aim starts in the release update.
            {
                UnitType type=shooter;type.weaponSwitching=true;type.maxMana=100;
                type.canMove=true;type.maxVel=Fixed::fromInt(1);type.turnInPlaceRate=0;
                type.weapons.assign(3,weapon);
                type.weapons[0].manaCost=0;type.weapons[1].manaCost=10;type.weapons[2].manaCost=30;
                World live;
                live.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
                const int from=live.spawn(&type,200,200,{},0),target=live.spawn(&targetType,300,200,{},1);
                live.setStance(from,2);live.setWeapon(from,2);
                auto* unit=live.unit(from);unit->mana=40;unit->reloads[2]=0;
                live.attack(from,target,false);
                bool released=false,aimed=false;
                for(int tick=0;tick<40 && !released;++tick) {
                    live.tick(1.f/30);unit=live.unit(from);
                    released=unit->justFired;
                    if(released)
                        for(unsigned i=0;i<unit->weaponAnimations.count;++i)
                            aimed|=unit->weaponAnimations.at(i).kind==tak::RetailWeaponAnimation::Aim &&
                                unit->weaponAnimations.at(i).slot==1;
                }
                require(released && unit->weaponSlot==1,"the walker's release falls back to the cheaper weapon");
                require(aimed,"a walker's attack aims the new slot in the update its switch happened (it waited a tick)");
            }
            // The 0x40 event reaches a flyer's unit attack (AirToGround's
            // and the other air heads' readers).
            {
                UnitType type=shooter;type.weaponSwitching=true;type.canFly=true;type.canMove=true;
                type.maxVel=Fixed::fromInt(2);type.weapons.assign(3,weapon);
                World air;
                air.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
                const int from=air.spawn(&type,200,200,{},0),target=air.spawn(&targetType,300,200,{},1);
                air.attack(from,target,false);
                auto* unit=air.unit(from);unit->missionEvents=0;
                require(RetailReplayProbe::switchWeapon(air,*unit,1) && (unit->missionEvents&0x40u),
                    "a flyer's unit attack hears the switch");
            }
            std::cout<<"PASS: a switcher short of mana after a release falls back to a cheaper weapon\n";
        }

    }
    {
        using namespace tak::sim;
        auto require=[](bool ok,const char* message) {if(!ok)throw std::runtime_error(message);};
        World world;
        UnitType shooter,targetType;shooter.maxHp=targetType.maxHp=100;
        targetType.maxVel=Fixed::fromInt(1);targetType.canFly=true;
        Weapon weapon;weapon.beam=true;weapon.projVel=30;weapon.noLead=true;
        shooter.weapons.push_back(weapon);shooter.weapon=weapon;
        const int sid=world.spawn(&shooter,200,200),tid=world.spawn(&targetType,300,200);
        world.unit(sid)->heading=Bam(32768);world.unit(sid)->groundY=Fixed::fromInt(50);
        world.unit(tid)->flightY=Fixed::fromInt(50);world.unit(tid)->flightVelocity.x=65536;
        auto aim=world.queryWeaponAim(sid,tid,0);
        require(aim && aim->heading==16384 && aim->pitch==0 && aim->target[0]==300*65536,
            "simulation direct aim uses world positions and respects no-lead");
        shooter.weapons[0].noLead=false;aim=world.queryWeaponAim(sid,tid,0);
        require(aim && aim->target[0]==300*65536+100*0xcccc && aim->heading==16384,
            "simulation aim leads actual flying target velocity with native fixed factor");
        targetType.maxVel=Fixed();aim=world.queryWeaponAim(sid,tid,0);
        require(aim && aim->target[0]==300*65536,"structure target suppresses aim lead");
        shooter.weapons[0].ballistic=true;shooter.weapons[0].projVel=300;
        shooter.weapons[0].lobPreferred=false;auto low=world.queryWeaponAim(sid,tid,0);
        shooter.weapons[0].lobPreferred=true;auto high=world.queryWeaponAim(sid,tid,0);
        require(low && high && low->pitch>0 && high->pitch>low->pitch && high->pitch<16384,
            "simulation ballistic aim chooses low and high native arcs");
        shooter.weapons[0].projVel=0.1f;aim=world.queryWeaponAim(sid,tid,0);
        require(aim && aim->pitch==0,"unreachable ballistic pitch uses native caller fallback");
        require(!world.queryWeaponAim(sid,tid,3) && !world.queryWeaponAim(sid,999,0),
            "aim query rejects absent weapon slots and targets");
        std::cout<<"PASS: simulation-owned direct, leading and ballistic weapon aim\n";
    }
    {
        using namespace tak::sim;
        auto require=[](bool ok,const char* message) {if(!ok)throw std::runtime_error(message);};
        auto file=std::make_shared<tak::cob::File>();
        file->pieces={"root","muzzle"};
        file->scripts={{"QueryWeapon",0},{"SweetSpot",0}};
        file->code={0x10021002,1,0x10023002,0,0x10065000};
        UnitType type;type.simulationScript=file;type.maxHp=100;
        type.productionModel={{{0,0,0},-1,0},{{32*65536,6*65536,12*65536},0,1}};
        type.scriptPieceVertices={{{10*65536,12*65536,14*65536}},{}};
        World world;
        const int id=world.spawn(&type,200,200);auto* unit=world.unit(id);
        unit->groundY=Fixed::fromInt(50);unit->heading=Bam(32768);
        require(world.queryUnitScriptPoint(id,false,1)==std::array<int32_t,3>{168*65536,56*65536,212*65536},
            "simulation QueryWeapon returns selected piece in world coordinates");
        // 4dd2a0 bounds the drawn buffer, so the body's attitude turns the
        // root piece's vertex: retail heading 0 leaves (10,12,14), and a quarter
        // turn moves it to (-14,12,10). Bounds include zero, so halve each.
        require(world.queryUnitScriptPoint(id,true)==std::array<int32_t,3>{205*65536,56*65536,207*65536},
            "simulation SweetSpot centres the drawn piece bounds at rest");
        unit->heading=Bam(49152);
        require(world.queryUnitScriptPoint(id,true)==std::array<int32_t,3>{193*65536,56*65536,205*65536},
            "simulation SweetSpot follows the body heading of the drawn piece");
        unit->groundPitch=1234;unit->groundRoll=5678;unit->heading=Bam(12345);
        const auto attitude=world.queryUnitScriptPoint(id,true);
        require(attitude!=std::array<int32_t,3>{205*65536,56*65536,207*65536} &&
                attitude!=std::array<int32_t,3>{193*65536,56*65536,205*65536},
            "simulation SweetSpot follows the body pitch and roll of the drawn piece");
        type.canFly=true;unit->flightY=Fixed::fromInt(80);
        const auto flying=world.queryUnitScriptPoint(id,true);
        require(flying==std::array<int32_t,3>{attitude[0],attitude[1]+30*65536,attitude[2]},
            "flying script query uses authoritative flight altitude");
        require(world.queryUnitScriptPoint(id,false,2)==std::array<int32_t,3>{200*65536,80*65536,200*65536},
            "invalid script piece falls back to unit position");
        std::cout<<"PASS: authoritative weapon and SweetSpot piece queries\n";
    }
    {
        using namespace tak::sim;
        auto require=[](bool ok,const char* message) {if(!ok)throw std::runtime_error(message);};
        auto file=std::make_shared<tak::cob::File>();
        file->numStatics=2;file->pieces={"root","emitjim"};
        file->scripts={{"QueryWeapon",0},{"MoveRate",5}};
        file->code={0x10021001,1,0x10023002,0,0x10065000,
            0x10021004,0,0x10021001,1,0x10031000,0x10023004,0,
            0x10021002,0,0x10023004,1,
            // Verharp sail/endsail's query-piece offsets, observed in native COB.
            0x10021001,0xfff56fef,0x1000b000,1,1,
            0x10021001,0xffdfc5d9,0x1000b000,1,2,0x10065000};
        UnitType type;type.simulationScript=file;type.maxHp=100;
        type.maxVel=Fixed::fromInt(4);type.accel=type.brake=Fixed::fromInt(1);
        type.turnRate=type.turnInPlaceRate=10000;
        type.animationMoveRate1=type.animationMoveRate2=Fixed::fromInt(100);
        type.productionModel={{{0,0,0},-1,0},{{0,0,0},0,1}};
        World world;
        world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        const int id=world.spawn(&type,200,200);
        world.order(id,800,200,false);world.tick(1.f/30);
        require(RetailReplayProbe::scriptStatics(world,id)==std::vector<uint32_t>{1,1},
            "ordinary mover immediately delivers its first MoveRate edge to simulation COB");
        const auto source=world.queryUnitScriptPoint(id,false);
        require(source[1]==world.unit(id)->groundY.v-692241,
            "movement animation updates authoritative QueryWeapon source before the next shot");
        for(int tick=0;tick<8;++tick)world.tick(1.f/30);
        require(RetailReplayProbe::scriptStatics(world,id)==std::vector<uint32_t>{1,1},
            "unchanged movement tier does not restart its authored controller");
        world.stop(id);
        for(int tick=0;tick<30;++tick)world.tick(1.f/30);
        require(RetailReplayProbe::scriptStatics(world,id)==std::vector<uint32_t>{2,0},
            "stopping emits exactly one MoveRate zero transition");
        std::cout<<"PASS: authoritative movement callback updates weapon source and preserves edge delivery\n";
    }
    {
        using namespace tak::sim;
        auto require=[](bool ok,const char* message) {if(!ok)throw std::runtime_error(message);};
        auto file=std::make_shared<tak::cob::File>();
        file->scripts={{"Create",0}};
        file->code={0x10021001,21,0x10021001,0,0x10082000,
                    0x10021001,22,0x10021001,1,0x10082000,
                    0x10021001,23,0x10021001,2,0x10082000,0x10065000};
        World world;
        UnitType type;type.simulationScript=file;type.maxHp=100;
        const int id=world.spawn(&type,100,100);
        auto* unit=world.unit(id);
        unit->weaponAim[0].flags=0xffff;
        world.tick(1.f/30);
        require(unit->weaponAim[0].flags==0xff07 && unit->weaponAim[1].flags==9 &&
                unit->weaponAim[2].flags==18,"simulation COB SET updates the addressed weapon handshake");
        require((unit->missionEvents&4)!=0,"weapon SET wakes the owning unit mission");
        const auto base=world.stateHash();
        for(auto& aim:unit->weaponAim) {
            ++aim.heading;require(world.stateHash()!=base,"weapon aim heading participates in lockstep hash");--aim.heading;
            ++aim.pitch;require(world.stateHash()!=base,"weapon aim pitch participates in lockstep hash");--aim.pitch;
            aim.flags^=8;require(world.stateHash()!=base,"weapon acknowledgement participates in lockstep hash");aim.flags^=8;
        }
        std::cout<<"PASS: authoritative weapon SET handshakes, owner wake and hashed aim state\n";
    }
    {
        using namespace tak::sim;
        auto script=std::make_shared<tak::cob::File>();
        script->scripts={{"Open",0},{"Close",6}};
        script->code={0x10021001,18,0x10021001,1,0x10082000,0x10065000,
                      0x10021001,18,0x10021001,0,0x10082000,0x10065000};
        for (char cell:std::string(".oOfwcCSyY")) {
            World world;
            world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
            UnitType factory;factory.id="yard";factory.maxVel=Fixed();
            factory.footX=factory.footZ=1;factory.yardMap=std::string(1,cell);
            factory.simulationScript=script;
            UnitType body;body.id="body";body.maxVel=Fixed::fromInt(1);body.footX=body.footZ=1;
            const int producer=world.spawn(&factory,128,128,0,0);
            const int occupant=world.spawn(&body,256,256,0,0);
            if (!RetailReplayProbe::yard(world,producer,"Open")) return 1;
            world.unit(occupant)->x=world.unit(producer)->x;
            world.unit(occupant)->z=world.unit(producer)->z;
            const bool blocksClosed=std::string("ofwcCS").find(cell)!=std::string::npos;
            if (RetailReplayProbe::yard(world,producer,"Close")!=blocksClosed) return 1;
            world.unit(occupant)->x=Fixed::fromInt(256);
            if (RetailReplayProbe::yard(world,producer,"Close")) return 1;
            world.unit(occupant)->x=world.unit(producer)->x;
            const bool blocksOpen=std::string("ofwSO").find(cell)!=std::string::npos;
            if (RetailReplayProbe::yard(world,producer,"Open")==blocksOpen) return 1;
            world.unit(occupant)->x=Fixed::fromInt(256);
            if (RetailReplayProbe::yard(world,producer,"Close")) return 1;
            world.unit(producer)->x=Fixed();
            world.unit(producer)->missionEvents=0;
            if (RetailReplayProbe::yard(world,producer,"Open") ||
                !(world.unit(producer)->missionEvents&4)) return 1;
        }
        std::cout<<"PASS: factory yard transitions wait for occupied blocking cells\n";
        {
            World world;
            world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
            UnitType factory;factory.id="yard-index";factory.maxHp=100;
            factory.footX=factory.footZ=1;factory.yardMap="c";factory.simulationScript=script;
            const int id=world.spawn(&factory,128,128);
            if (world.scriptYardOpen(-1) || world.scriptYardOpen(id+1000) || world.scriptYardOpen(id)) return 1;
            if (!RetailReplayProbe::yard(world,id,"Open") || !world.scriptYardOpen(id)) return 1;
            world.unit(id)->hp={};world.tick(1.f/30);
            if (world.scriptYardOpen(id)) return 1;
            world.resetForReplay();
            const int replacement=world.spawn(&factory,128,128);
            if (replacement!=id || world.scriptYardOpen(replacement)) return 1;
            if (!RetailReplayProbe::yard(world,replacement,"Open")) return 1;
            std::cout<<"PASS: indexed yard reads survive script retirement and replay ID reuse\n";
        }
        {
            World world;
            world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
            world.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
            UnitType gate;gate.id="gate";gate.maxVel=Fixed();gate.gate=true;
            gate.footX=gate.footZ=1;gate.yardMap="c";gate.simulationScript=script;
            UnitType body;body.footX=body.footZ=1;
            const int id=world.spawn(&gate,128,128),occupant=world.spawn(&body,256,256);
            RetailReplayProbe::cacheYard(world);
            auto grades=[&](int passage,int border) {
                return RetailReplayProbe::cachedYard(world,8,8)==passage &&
                       RetailReplayProbe::cachedYard(world,7,8)==border;
            };
            if (!grades(3,4)) return 1;
            if (!RetailReplayProbe::yard(world,id,"Open") || !grades(6,6)) return 1;
            world.unit(occupant)->x=world.unit(occupant)->z=Fixed::fromInt(128);
            if (!RetailReplayProbe::yard(world,id,"Close") || !grades(6,6)) return 1;
            world.unit(occupant)->x=Fixed::fromInt(256);
            if (RetailReplayProbe::yard(world,id,"Close") || !grades(3,4)) return 1;
            // Retail refreshes even an accepted assignment of the same state.
            world.unit(id)->x=Fixed::fromInt(144);
            if (RetailReplayProbe::yard(world,id,"Close") ||
                RetailReplayProbe::cachedYard(world,9,8)!=3) return 1;
            std::cout<<"PASS: gate yard changes refresh cached passages and clearance borders\n";
        }
        {
            World world;
            world.setTerrain(std::vector<uint8_t>(32*32,0),32,32,0);
            world.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
            auto gateScript=std::make_shared<tak::cob::File>(*script);
            gateScript->scripts={{"Activate",0},{"Deactivate",6}};
            UnitType gate;gate.id="command-gate";gate.maxVel=Fixed();gate.gate=true;
            gate.onOffable=true;gate.activateWhenBuilt=false;
            gate.footX=gate.footZ=1;gate.yardMap="c";gate.simulationScript=gateScript;
            const int id=world.spawn(&gate,128,128);
            RetailReplayProbe::tickYard(world,id);
            RetailReplayProbe::cacheYard(world);
            TypeRegistry registry;
            tak::net::Command command;command.kind=tak::net::Cmd::SetActive;
            command.unitId=id;command.targetId=1;command.player=1;
            applyCommand(world,registry,command);
            if (world.unit(id)->active || RetailReplayProbe::cachedYard(world,8,8)!=3) return 1;
            command.player=0;applyCommand(world,registry,command);
            if (!world.unit(id)->active || RetailReplayProbe::cachedYard(world,8,8)!=6) return 1;
            world.unit(id)->missionEvents=0;applyCommand(world,registry,command);
            if (world.unit(id)->missionEvents) return 1; // unchanged state sends no callback
            command.targetId=0;applyCommand(world,registry,command);
            if (world.unit(id)->active || RetailReplayProbe::cachedYard(world,8,8)!=3) return 1;
            world.unit(id)->missionEvents=0;applyCommand(world,registry,command);
            if (world.unit(id)->missionEvents) return 1;
            std::cout<<"PASS: owned gate activation commands drive scripts and path grades immediately\n";
        }
    }
    {
        tak::cob::File animation;animation.pieces.resize(70);
        tak::cob::RetailScriptState indexed(animation);
        std::vector<tak::cob::RetailPiece> reference(70);
        Host animationHost;
        tak::cob::RetailScriptState::Adapter<Host> adapter{indexed,animationHost};
        const std::array<uint32_t,6> operations={0x10001000,0x10002000,0x10003000,
                                               0x10004000,0x1000b000,0x1000c000};
        uint32_t seed=73;
        auto next=[&] {seed=seed*1664525u+1013904223u;return seed;};
        std::array<bool,70> seen{};
        for (int step=0;step<3000;++step) {
            const int piece=int((next()>>8)%70),axis=int((next()>>8)%3);
            const auto op=operations[(next()>>8)%operations.size()];
            const int target=int((next()>>8)%131072)-65536,speed=int((next()>>8)%60000);
            seen[size_t(piece)]=true;
            adapter.piece(op,piece,axis,target,speed);
            reference[size_t(piece)].command(op,axis,target,speed);
            if (step==1500) {
                // Native restoration marks every piece active for its first
                // update, including pieces above the compact index's limit.
                std::vector<uint8_t> bytes(0xa48+70*0x6c,0);
                auto put=[&](size_t offset,uint32_t value) {
                    for (unsigned byte=0;byte<4;++byte) bytes[offset+byte]=uint8_t(value>>(byte*8));
                };
                for (size_t i=0;i<reference.size();++i) {
                    auto& pieceState=reference[i];size_t offset=0xa48+i*0x6c;
                    for (const auto* values:{&pieceState.moveTarget,&pieceState.moveSpeed,
                            &pieceState.turnTarget,&pieceState.turnSpeed,&pieceState.spinTarget,
                            &pieceState.spinAcceleration,&pieceState.move,&pieceState.turn})
                        for (auto value:*values) {put(offset,uint32_t(value));offset+=4;}
                    put(offset,pieceState.visible);put(offset+4,pieceState.cached);put(offset+8,pieceState.shaded);
                    pieceState.active=true;
                }
                indexed.restore(animation,bytes);
            }
            const int elapsed=int((next()>>8)%6);
            indexed.tick(animation,elapsed,animationHost);
            for (auto& pieceState:reference) pieceState.tick(elapsed);
            for (size_t i=0;i<reference.size();++i) {
                const auto& a=indexed.pieces[i];const auto& b=reference[i];
                if (a.moveTarget!=b.moveTarget || a.moveSpeed!=b.moveSpeed ||
                    a.turnTarget!=b.turnTarget || a.turnSpeed!=b.turnSpeed ||
                    a.spinTarget!=b.spinTarget || a.spinAcceleration!=b.spinAcceleration ||
                    a.move!=b.move || a.turn!=b.turn || a.active!=b.active ||
                    a.visible!=b.visible || a.cached!=b.cached || a.shaded!=b.shaded || a.rendered!=b.rendered)
                    return 1;
            }
        }
        if (!std::all_of(seen.begin(),seen.end(),[](bool value){return value;})) return 1;
        std::cout<<"PASS: indexed animation matches full scan through commands and restoration\n";
    }
    tak::cob::File file;
    file.scripts={{"parent",0},{"child",7}};
    file.code={0x10062000,1,0,0x10021001,0,0x10065000,0,
               0x10021001,34,0x10013000,0x10021001,0,0x10065000};
    tak::cob::RetailVm vm;
    Host host;
    {
        tak::cob::RetailVm indexed,reference;
        Host indexedHost,referenceHost;
        for(unsigned step=0;step<300;++step) {
            if(step%41==0){indexed.clearThreads();reference.clearThreads();}
            if(step%11==0){indexed.finish(step%16);reference.finish(step%16);}
            if(step%3==0)for(int n=0;n<5;++n)
                if(indexed.start(file,int(step%2))!=reference.start(file,int(step%2)))return 1;
            indexedHost.events.clear();referenceHost.events.clear();
            indexed.tick(file,int(step%2),indexedHost);
            reference.tick(file,int(step%2),referenceHost,true);
            if(indexed.active!=reference.active || indexed.statics!=reference.statics ||
               indexedHost.seed!=referenceHost.seed || indexedHost.events!=referenceHost.events)return 1;
            for(size_t slot=0;slot<16;++slot)
                if(indexed.threads[slot].words!=reference.threads[slot].words)return 1;
        }
        std::cout<<"PASS: indexed thread scheduling matches full scan through slot reuse, saturation, calls, sleep, wakeup and reset\n";
    }
    if (vm.start(file,0)!=0) return 1;
    vm.tick(file,1,host);
    if (vm.active!=2 || vm.threads[0].flags()!=0x2800000 || vm.threads[1].words[3]!=1) return 1;
    {
        // Restoring a sleeping child and blocked parent must resume exactly as
        // uninterrupted execution, including the tick of the parent's wakeup.
        std::vector<uint8_t> bytes(0xa48,0);
        auto put=[&](size_t offset,uint32_t value) {
            for (unsigned byte=0;byte<4;++byte) bytes[offset+byte]=uint8_t(value>>(byte*8));
        };
        for (size_t slot=0;slot<vm.threads.size();++slot)
            for (size_t word=0;word<vm.threads[slot].words.size();++word)
                put(4+slot*0xa4+word*4,vm.threads[slot].words[word]);
        put(0xa44,vm.active);
        tak::cob::RetailScriptState restored(file);restored.restore(file,bytes);
        auto uninterrupted=vm;
        Host originalHost=host,restoredHost=host;
        for (int step=0;step<4;++step) {
            uninterrupted.tick(file,1,originalHost);restored.tick(file,1,restoredHost);
            if (uninterrupted.active!=restored.vm.active || originalHost.seed!=restoredHost.seed ||
                originalHost.events!=restoredHost.events) return 1;
            for (size_t slot=0;slot<vm.threads.size();++slot)
                if (uninterrupted.threads[slot].words!=restored.vm.threads[slot].words) return 1;
        }
        std::cout<<"PASS: restored sleeping threads preserve continuation and parent wakeup\n";
    }
    vm.tick(file,1,host);
    if (vm.active!=1 || vm.threads[0].flags()!=0x1000000) return 1;
    vm.tick(file,1,host);
    if (vm.active) return 1;
    std::cout<<"PASS: separate call slots, integer sleep and ordered parent wakeup\n";
}
