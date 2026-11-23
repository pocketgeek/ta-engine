#include "sim/scenario.h"
#include "sim/sim.h"
#include "client/renderframe.h"
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace tak;
static void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
int main() try {
    sim::TypeRegistry registry;
    sim::UnitType source;source.id="source";source.maxHp=100;source.footX=source.footZ=1;
    source.income=30;source.storage=200;
    auto make=[&] {
        auto w=std::make_unique<sim::World>();w->setPlayerCount(1);
        w->setTerrain(std::vector<uint8_t>(32*32,40),32,32,0);
        w->spawn(&source,128,128,0,0);return w;
    };
    auto client=make(),referee=make();
    crt::Scenario scene;scene.players.resize(2);
    scene.players[1].push_back({{{0,{}}},{{17,{"200"}},{16,{"25"}},{12,{}}}});
    scene.players[1].push_back({{{1,{"0"}}},{{20,{}},{17,{"10"}},{14,{}}}});
    for(auto* w:{client.get(),referee.get()})
        w->setScenario(std::make_unique<sim::ScenarioScript>(scene,registry,w==client.get()?0:-1,1,32,32));
    check(client->scenario()->showClock() && referee->scenario()->showClock(),"Display gameclock action not latched");
    check(client->player(0).mana==200,"resource set unexpectedly clamped immediately");
    for(int tick=1;tick<=31;++tick) {
        client->tick(1.f/30);referee->tick(1.f/30);
        check(client->stateHash()==referee->stateHash(),"resource rules desynchronized viewers");
        if(tick<30) {
            check(client->player(0).mana==25 && client->player(0).income==0,
                  "positive resource limit did not persist or suppress natural income");
            PlayerR shown;shown.captureEconomy(client->player(0));
            check(shown.storage==25,"HUD replaced authored cap with minimum100");
        }
    }
    check(client->player(0).scenarioResourceLimit==0 && client->player(0).storage==200 &&
          client->player(0).mana>10.99 && client->player(0).mana<11.01,
          "Resources normal did not restore building income and storage");
    {
        auto w=make();const auto before=w->stateHash();
        w->applyScenarioResourceAction(0,16,25);
        check(w->stateHash()!=before,"persistent resource override absent from hash");
        w->applyScenarioResourceAction(0,20,0);
        check(w->stateHash()==before,"resource reset left hashed override state");
        w->applyScenarioResourceAction(0,17,16777217);
        check(w->player(0).mana==16777216,"set resources did not round to retail float");
        w->applyScenarioResourceAction(0,17,25);
        w->applyScenarioResourceAction(0,19,50);
        check(w->player(0).mana==-25,"resource subtraction clamped before native tick boundary");
        crt::Scenario empty;w->setScenario(std::make_unique<sim::ScenarioScript>(empty,registry,-1,1,32,32));
        w->tick(1.f/30);
        check(w->player(0).mana==0,"negative scripted resources not clamped at tick-end");
        w->applyScenarioResourceAction(0,16,-1);w->tick(1.f/30);
        check(w->player(0).income==30 && w->player(0).storage==200,
              "nonpositive resource limit suppressed normal economy");
        w->applyScenarioResourceAction(0,16,25);w->clearScenarioState();
        check(w->player(0).scenarioResourceLimit==0,"replay reset leaked resource override");
    }
    {
        auto w=make();auto& p=w->player(0);p.retailResources.emplace();
        p.mana=p.retailResources->stored=123.25f;p.retailResources->totalProduced=1000.5;
        w->applyScenarioResourceAction(0,18,16777217);
        check(p.mana==16777340 && p.retailResources->stored==p.mana &&
              p.retailResources->totalProduced==16778217.5,
              "native resource object was not synchronized or add accounting differs");
        check(p.displayResources.income==0 && p.retailResources->produced==0,
              "scripted add fabricated per-second natural income");
        w->applyScenarioResourceAction(0,16,25);w->tick(1.f/30);
        check(p.mana==25 && p.retailResources->capacityOverride==25,"native resource override not applied");
        w->applyScenarioResourceAction(0,20,0);w->tick(1.f/30);
        check(p.retailResources->capacityOverride==0 && p.storage==200 && p.mana>25,
              "native resource reset did not restore natural economy");
    }
    std::cout<<"PASS: resource limit/reset persistence, HUD cap, native rounding/accounting and peer parity\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
