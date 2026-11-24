// Use the shipped build trees, scripts and movement profiles. A naval planner
// that merely emits Train can still leave every ship stuck inside its factory.
#include "ai/ai.h"
#include "ai/builtinprofile.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cstdio>
#include <set>

using namespace ta;

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
    {
        sim::TypeRegistry registry;sim::setupRegistry(registry,vfs);
        {
            sim::World world;
            world.setTerrain(std::vector<uint8_t>(256*256,0),256,256,32);
            world.buildNavClasses(registry);world.setPathService(true);
            const auto* ship=registry.find("armpt");
            const int yard=world.spawn(registry.find("armsy"),2000,2000,0,0);
            world.tick(1.f/30.f);
            sim::Fixed x,y,z;world.productionPosition(yard,ship,x,y,z);
            // An ally's ship: the yard's door clearing moves only its owner's
            // idle ships (World::clearYardDoor), so this one stays put.
            world.setTeam(1,0);
            const int blocker=world.spawn(registry.find("corpt"),x.toFloat(),z.toFloat(),0,1);
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
            const auto* ship=registry.find("armpt");
            const int yard=world.spawn(registry.find("armsy"),2000,2000,0,0);
            world.tick(1.f/30.f);
            sim::Fixed x,y,z;world.productionPosition(yard,ship,x,y,z);
            world.spawn(registry.find("armpt"),x.toFloat(),z.toFloat(),0,0);
            world.train(yard,ship,1);
            for (int tick=0;tick<9000 && !world.unit(yard)->buildQueue.empty();++tick) {
                world.player(0).mana=100000;world.tick(1.f/30.f);
            }
            check(world.unit(yard)->buildQueue.empty() && std::any_of(world.units().begin(),world.units().end(),[&](const auto& u) {
                return u.alive() && u.type==ship && !u.underConstruction;
            }),"the owner's idle ship on the pad makes way and the yard completes its ship");
        }
        // Every shipyard that makes water units, alone on a coast and run by
        // TA's own AI (ai/taretail.h, which picks from the yard's menu by its
        // own profile tables): it must finish a water unit through normal
        // commands.
        for (const auto& [parentId,parent]:registry.types()) {
            if (!parent.isStructure()) continue;
            std::set<const sim::UnitType*> water;
            for (const auto& child:registry.buildable(parentId))
                if (const auto* t=registry.find(child); t && !t->isStructure() && !t->canFly &&
                    t->domain==sim::UnitType::Domain::Water) water.insert(t);
            if (water.empty()) continue;
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
                world.spawn(&parent,2000.f,800,std::nullopt,0);
                world.setStartResources(0,100000.f,100000.f);   // TA's books: the AI scores by them
                ai::Controller controller(0,registry,retailProfile,1,
                    defensive?ai::Difficulty::Defensive:ai::Difficulty::Normal,{{3280,800}});
                int completed=0,attackOrders=0;
                const sim::UnitType* ship=nullptr;
                for (uint32_t tick=0;tick<18000 && !completed;++tick) {
                    // A going economy: the build score (taretail score) wants
                    // metal or energy first while income is short.
                    auto& books=world.player(0).books;
                    books.income[0]=1000;books.income[1]=50;
                    controller.tick(world,tick,[&](const auto& c) {
                        if (c.kind==net::Cmd::AttackMove) ++attackOrders;
                        sim::applyCommand(world,registry,c);
                    });
                    world.tick(1.f/30.f);
                    for (const auto& u:world.units())
                        if (u.alive() && water.count(u.type) && !u.underConstruction) { completed=u.id;ship=u.type; }
                }
                std::printf("defensive=%d %s -> %s: ",defensive,parentId.c_str(),ship?ship->id.c_str():"-");
                check(completed!=0,"AI finishes a water unit from its yard through normal commands");
                // A lone fighter is not sent out: retail's naval strike group
                // (task F, 0x4077e0) launches at six (docs/retail-ai.md, the
                // squad table), which the natural opening below reaches. A
                // Defensive AI never launches one.
                if (defensive) check(attackOrders==0,"a Defensive AI keeps its ships home");
              }
        }

        // Natural opening: no free builders, factories, income or altered
        // profile. Both sides take the same Seven Islands seat, where TA's AI
        // goes to sea; in 30 minutes it has gathered and launched a naval
        // strike group (six ships, docs/retail-ai.md).
        for (int faction=0;faction<2;++faction) {
            sim::World world;
            sim::MatchConfig config;config.vfs=&vfs;config.mapPath=hpi::findMap(vfs,"Seven Islands");
            config.roomMapRevealed=true;   // FOG OF WAR: EXPLORED, the room's default
            config.slots={{true,0,0,1.f},{true,faction,1,1.f,true,false}};
            const auto starts=sim::setupMatch(world,registry,config);
            ai::Controller controller(1,registry,retailProfile,0x1234,ai::Difficulty::Normal,{starts[0]});
            std::set<int> completed;int attacks=0;
            for (uint32_t tick=0;tick<54000;++tick) {
                controller.tick(world,tick,[&](const auto& c) {
                    const auto* u=world.unit(c.unitId);
                    const bool water=u && waterFighter(u->type);
                    if (water && (c.kind==net::Cmd::AttackMove || c.kind==net::Cmd::Attack)) ++attacks;
                    sim::applyCommand(world,registry,c);
                });
                world.tick(1.f/30.f);
                if (tick%30==0) for (const auto& u:world.units()) {
                    if (u.player!=1 || !u.alive() || u.underConstruction || !u.type || u.type->isBuilder) continue;
                    if (waterFighter(u.type)) completed.insert(u.id);
                }
            }
            std::printf("faction=%d completed=%zu attacks=%d: ",faction,completed.size(),attacks);
            check(!completed.empty() && attacks>0,"natural AI opening produces and deploys its faction's water-capable force");
        }
    }
    std::printf("naval AI: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
