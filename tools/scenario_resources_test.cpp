// Map scenario resource actions 16-20 and conditions 24 / 25 on TA's economy
// (sim/taeconomy.h): the one scenario "resources" figure is metal and energy alike
// (World::applyScenarioResourceAction); the conditions read the scarcer store.
#include "sim/scenario.h"
#include "sim/sim.h"
#include "client/renderframe.h"
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace ta;
static void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
int main() try {
    sim::TypeRegistry registry;
    sim::UnitType source;source.id="source";source.maxHp=100;source.footX=source.footZ=1;
    source.energyMake=30;source.metalMake=3;
    auto make=[&] {
        auto w=std::make_unique<sim::World>();w->setPlayerCount(1);
        w->setTerrain(std::vector<uint8_t>(32*32,40),32,32,0);
        w->setStartResources(0,1000,1000);
        w->spawn(&source,128,128,0,0);return w;
    };
    const auto& books=[](const sim::World& w)->const sim::econ::PlayerBooks& {return w.player(0).books;};
    auto client=make(),referee=make();
    crt::Scenario scene;scene.players.resize(2);
    scene.players[1].push_back({{{0,{}}},{{17,{"200"}},{16,{"25"}},{12,{}}}});
    scene.players[1].push_back({{{1,{"0"}}},{{20,{}},{17,{"10"}},{14,{}}}});
    for(auto* w:{client.get(),referee.get()})
        w->setScenario(std::make_unique<sim::ScenarioScript>(scene,registry,w==client.get()?0:-1,1,32,32));
    check(client->scenario()->showClock() && referee->scenario()->showClock(),"Display gameclock action not latched");
    check(books(*client).stored[0]==200 && books(*client).stored[1]==200,
          "set resources did not write both stores (or clamped before the settle)");
    for(int tick=1;tick<=61;++tick) {
        client->tick(1.f/30);referee->tick(1.f/30);
        check(client->stateHash()==referee->stateHash(),"resource rules desynchronized viewers");
        if(tick<29) {
            check(books(*client).cap[0]==25 && books(*client).cap[1]==25,"resource limit did not cap both stores");
            check(books(*client).stored[0]==25 && books(*client).stored[1]==25,"capped stores not settled to the limit");
            PlayerR shown;shown.captureEconomy(client->player(0));
            check(shown.metalCap==25 && shown.energyCap==25,"HUD does not show the authored cap");
        }
    }
    check(client->player(0).scenarioResourceLimit==0 && books(*client).cap[0]==1000 && books(*client).cap[1]==1000,
          "Resources normal did not restore the storage");
    check(books(*client).stored[0]>10 && books(*client).stored[1]>10 && books(*client).stored[0]<=70 &&
          books(*client).stored[1]<=16,"set-to-10 then income did not land in both stores");
    {
        auto w=make();const auto before=w->stateHash();
        w->applyScenarioResourceAction(0,16,25);
        check(w->stateHash()!=before,"persistent resource override absent from hash");
        w->applyScenarioResourceAction(0,20,0);
        check(w->stateHash()==before,"resource reset left hashed override state");
        w->applyScenarioResourceAction(0,17,16777217);
        check(books(*w).stored[0]==16777216 && books(*w).stored[1]==16777216,"set resources did not round to float32");
        w->applyScenarioResourceAction(0,17,25);
        w->applyScenarioResourceAction(0,18,5);
        check(books(*w).stored[0]==30 && books(*w).stored[1]==30,"give resources");
        w->applyScenarioResourceAction(0,19,50);
        check(books(*w).stored[0]==0 && books(*w).stored[1]==0,"take resources went below zero");
        w->applyScenarioResourceAction(0,16,-1);w->tick(1.f/30);
        check(books(*w).cap[0]==1000 && books(*w).income[0]==30 && books(*w).income[1]==3,
              "nonpositive resource limit changed the economy");
        w->applyScenarioResourceAction(0,16,25);w->clearScenarioState();
        check(w->player(0).scenarioResourceLimit==0,"replay reset leaked resource override");
    }
    // Conditions 24 / 25 (Resources less / greater than v) on the scarcer store.
    auto outcome=[&](int opcode,const char* value,float energy,float metal) {
        auto w=make();
        w->applyScenarioResourceAction(0,16,5000);   // cap high: the settle keeps the figures
        auto& b=const_cast<sim::econ::PlayerBooks&>(books(*w));b.stored[0]=energy;b.stored[1]=metal;
        crt::Scenario rules;rules.players.resize(2);
        rules.players[1].push_back({{{opcode,{value}}},{{5,{}}}});   // -> Victory for me
        w->setScenario(std::make_unique<sim::ScenarioScript>(rules,registry,0,1,32,32));
        w->tick(1.f/30);
        return w->scenarioOutcome(0)==1;
    };
    check(outcome(24,"50",100,40) && outcome(24,"50",40,100),"less than: either store under v");
    check(!outcome(24,"50",60,60),"less than: both stores at or over v");
    check(outcome(25,"50",60,70) && !outcome(25,"50",100,40) && !outcome(25,"50",40,100),
          "greater than: both stores over v");
    std::cout<<"PASS: TA metal/energy resource limit/reset, set/give/take, conditions, HUD cap and peer parity\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
