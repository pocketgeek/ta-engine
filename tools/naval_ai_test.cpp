// Use the shipped build trees, scripts and movement profiles. A naval planner
// that merely emits Train can still leave every ship stuck inside its factory.
#include "ai/ai.h"
#include "ai/builtinprofile.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cstdio>
#include <set>

using namespace tak;

static bool waterFighter(const sim::UnitType* t) {
    return t && !t->isStructure() && !t->canFly && !t->isBuilder &&
        t->domain==sim::UnitType::Domain::Water;
}

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=hpi::mountRetailRoot(argv[1]);
    const auto& retailProfile=ai::builtinProfile(ai::Difficulty::Normal);
    int failures=0,cases=0;
    auto check=[&](bool ok,const char* label) {
        std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);
        ++cases;if (!ok) ++failures;
    };
    for (bool crusades:{false,true}) {
        sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,crusades);
        {
            sim::World world;
            world.setTerrain(std::vector<uint8_t>(256*256,0),256,256,32);
            world.buildNavClasses(registry);world.setPathService(true);
            const auto* ship=registry.find("cresubm");
            const int yard=world.spawn(registry.find("crenavy"),2000,2000,0,0);
            world.tick(1.f/30.f);
            sim::Fixed x,y,z;world.productionPosition(yard,ship,x,y,z);
            // An ally's ship: the yard's door clearing moves only its owner's
            // idle ships (World::clearYardDoor), so this one stays put.
            world.setTeam(1,0);
            const int blocker=world.spawn(registry.find("arawar"),x.toFloat(),z.toFloat(),0,1);
            world.train(yard,ship,1);
            for (int tick=0;tick<300;++tick) {world.player(0).mana=100000;world.tick(1.f/30.f);}
            check(world.unit(yard)->productionSiteId==0,"another player's ship occupying the open pad still blocks production");
            world.order(blocker,2000,2600,false);
            for (int tick=0;tick<9000 && !world.unit(yard)->buildQueue.empty();++tick) {
                world.player(0).mana=100000;world.tick(1.f/30.f);
            }
            check(world.unit(yard)->buildQueue.empty() && std::any_of(world.units().begin(),world.units().end(),[&](const auto& u) {
                return u.alive() && u.type==ship && !u.underConstruction;
            }),"shipyard resumes and completes its ship after the pad is cleared");
        }
        {
            // The owner's own idle ship on the pad is moved aside and the
            // yard builds (an engine deviation: retail moves no idle body).
            sim::World world;
            world.setTerrain(std::vector<uint8_t>(256*256,0),256,256,32);
            world.buildNavClasses(registry);world.setPathService(true);
            const auto* ship=registry.find("cresubm");
            const int yard=world.spawn(registry.find("crenavy"),2000,2000,0,0);
            world.tick(1.f/30.f);
            sim::Fixed x,y,z;world.productionPosition(yard,ship,x,y,z);
            world.spawn(registry.find("arawar"),x.toFloat(),z.toFloat(),0,0);
            world.train(yard,ship,1);
            for (int tick=0;tick<9000 && !world.unit(yard)->buildQueue.empty();++tick) {
                world.player(0).mana=100000;world.tick(1.f/30.f);
            }
            check(world.unit(yard)->buildQueue.empty() && std::any_of(world.units().begin(),world.units().end(),[&](const auto& u) {
                return u.alive() && u.type==ship && !u.underConstruction;
            }),"the owner's idle ship on the pad makes way and the yard completes its ship");
        }
        // Every AI-enabled water unit and its actual producer. This includes
        // the mobile Aramon/Zhon chains, not just Veruna's conventional yard.
        for (const auto& [parentId,parent]:registry.types()) {
            for (const auto& child:registry.buildable(parentId)) {
                const auto* ship=registry.find(child);
                const auto weight=retailProfile.weight.find(child),limit=retailProfile.limit.find(child);
                if (!ship || ship->isStructure() || ship->canFly || ship->domain==sim::UnitType::Domain::Ground ||
                    (ship->domain==sim::UnitType::Domain::Hover && ship->isBuilder) ||
                    weight==retailProfile.weight.end() || weight->second<=0 ||
                    (limit!=retailProfile.limit.end() && limit->second==0)) continue;
              for (bool defensive:{false,true}) {
                sim::World world;
                std::vector<uint8_t> heights(256*256,40);
                for (int z=0;z<256;++z) for (int x=96;x<192;++x) heights[size_t(z)*256+x]=0;
                for (int z=0;z<256;++z) for (int x=0;x<4;++x) {
                    heights[size_t(z)*256+96+x]=uint8_t(28-x*8);
                    heights[size_t(z)*256+191-x]=uint8_t(28-x*8);
                }
                world.setTerrain(std::move(heights),256,256,32);
                world.buildNavClasses(registry);world.setPathService(true);
                world.player(0).defensiveAi=defensive;
                const float px=parent.isStructure() ?
                    (ship->domain==sim::UnitType::Domain::Water ? 2000.f : 1200.f) : 1400.f;
                world.spawn(&parent,px,800,std::nullopt,0);
                ai::Profile profile;profile.weight[child]=100;
                ai::Controller controller(0,registry,profile,1,
                    defensive?ai::Difficulty::Defensive:ai::Difficulty::Normal,{{3280,800}});
                int completed=0,attackOrders=0;
                for (uint32_t tick=0;tick<18000 && !completed;++tick) {
                    world.player(0).mana=100000;world.player(0).income=100;
                    controller.tick(world,tick,[&](const auto& c) {
                        if (c.kind==net::Cmd::AttackMove) ++attackOrders;
                        sim::applyCommand(world,registry,c);
                    });
                    world.tick(1.f/30.f);
                    for (const auto& u:world.units())
                        if (u.alive() && u.type==ship && !u.underConstruction) completed=u.id;
                }
                std::printf("balance=%d defensive=%d %s -> %s: ",crusades,defensive,parentId.c_str(),child.c_str());
                check(completed!=0,"AI finishes its water-unit build through normal commands");
                if (completed && !ship->isBuilder) {
                    for (uint32_t tick=18000;tick<18120 && !attackOrders;++tick) {
                        controller.tick(world,tick,[&](const auto& c) {
                            if (c.unitId==completed && c.kind==net::Cmd::AttackMove) ++attackOrders;
                            sim::applyCommand(world,registry,c);
                        });
                        world.tick(1.f/30.f);
                    }
                    check(defensive ? attackOrders==0 : attackOrders>0,
                        "naval fighters deploy for Normal and remain defensive for Defensive AI");
                }
              }
            }
        }

        // Natural opening: no free builders, factories, income, mana or altered
        // profile. All factions use the same inland Varro Passage starting seat.
        for (int faction=0;faction<5;++faction) {
            sim::World world;
            sim::MatchConfig config;config.vfs=&vfs;config.mapPath=hpi::findMap(vfs,"Varro Passage");
            config.roomMapRevealed=true;   // FOG OF WAR: EXPLORED, the room's default
            config.slots={{true,0,0,1.f},{true,faction,1,1.f,true,false}};
            const auto starts=sim::setupMatch(world,registry,config);
            ai::Controller controller(1,registry,retailProfile,0x1234,ai::Difficulty::Normal,{starts[0]});
            std::set<int> completed;int attacks=0;
            for (uint32_t tick=0;tick<36000;++tick) {
                controller.tick(world,tick,[&](const auto& c) {
                    const auto* u=world.unit(c.unitId);
                    const bool water=u && u->type && (faction==1 ?
                        (!u->type->canFly && u->type->domain==sim::UnitType::Domain::Hover) : waterFighter(u->type));
                    if (water && (c.kind==net::Cmd::AttackMove || c.kind==net::Cmd::Attack)) ++attacks;
                    sim::applyCommand(world,registry,c);
                });
                world.tick(1.f/30.f);
                if (tick%30==0) for (const auto& u:world.units()) {
                    if (u.player!=1 || !u.alive() || u.underConstruction || !u.type || u.type->isBuilder) continue;
                    if (faction==1 ? (!u.type->canFly && u.type->domain==sim::UnitType::Domain::Hover) : waterFighter(u.type))
                        completed.insert(u.id);
                }
            }
            std::printf("balance=%d faction=%d completed=%zu attacks=%d: ",crusades,faction,completed.size(),attacks);
            check(!completed.empty() && attacks>0,"natural AI opening produces and deploys its faction's water-capable force");
        }
    }
    std::printf("naval AI: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
