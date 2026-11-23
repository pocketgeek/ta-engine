#include "crt/crt.h"
#include "hpi/hpi.h"
#include "sim/scenario.h"
#include "sim/sim.h"

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace tak;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
crt::RuleGroup onStart(std::initializer_list<crt::Rule> actions) {
    return {{{0,{}}}, actions};
}
std::unique_ptr<sim::World> makeWorld(int count) {
    auto world = std::make_unique<sim::World>();
    world->setPlayerCount(count);
    world->setTerrain(std::vector<uint8_t>(32*32,40),32,32,0);
    return world;
}
void attach(sim::World& world, const sim::TypeRegistry& registry,
            const crt::Scenario& scene, int viewer = -1, uint32_t participants = 0xff) {
    // Serialization is intentional: exercise bank ownership as saved by the editor.
    auto parsed = crt::parse(crt::write(scene));
    world.setScenario(std::make_unique<sim::ScenarioScript>(parsed, registry, viewer,
                       world.numPlayers(), 32, 32, participants));
}
void ticks(sim::World& world, int count) {
    for (int i=0;i<count;++i) world.tick(1.f/30);
}
}

int main() try {
    auto files = std::make_shared<hpi::Vfs::Files>();
    const std::string fbi = "[UNITINFO]{\nunitname=monarch;\nobjectname=monarch;\n"
        "maxdamage=100;\nfootprintx=1;\nfootprintz=1;\ncommander=1;\n}";
    (*files)["units/monarch.fbi"] = {fbi.begin(),fbi.end()};
    const std::string hutFbi = "[UNITINFO]{\nunitname=hut;\nobjectname=hut;\n"
        "maxdamage=100;\nfootprintx=1;\nfootprintz=1;\nbuildtime=100;\n}";
    (*files)["units/hut.fbi"] = {hutFbi.begin(),hutFbi.end()};
    hpi::Vfs vfs;vfs.setMapFiles(files);
    sim::TypeRegistry registry;registry.loadDir(vfs,"units");
    const auto* monarch=registry.find("monarch");
    check(monarch && monarch->commander,"synthetic monarch missing");

    // These recipient sets were also observed by calling retail's dispatcher.
    const std::array<int,6> opcodes{5,6,21,22,23,24};
    const std::array<std::array<int,3>,6> expected{{
        {1,0,0},{-1,0,0},{1,1,0},{-1,-1,0},{0,0,1},{0,0,-1}
    }};
    for (size_t i=0;i<opcodes.size();++i) {
        auto world=makeWorld(3);world->setTeam(1,0);
        crt::Scenario scene;scene.players.resize(9);
        scene.players[1].push_back(onStart({{opcodes[i],{}}}));
        attach(*world,registry,scene);world->updateOutcome();
        check(world->hasScenarioOutcomes(),"authored outcome mode not enabled");
        for(int p=0;p<3;++p) {
            check(world->scenarioOutcome(p)==expected[i][size_t(p)],"outcome action targets wrong recipient");
            check(world->player(p).defeated==(expected[i][size_t(p)]<0),"custom defeat flags disagree with outcomes");
        }
        check(world->scenarioOutcome(-1)==0,"spectator ended while participants still running");
        check(world->winningTeam()==-1,"incomplete custom outcome declared a winning team");
    }
    {
        // First result wins, without turning an individual win into opponent loss.
        auto world=makeWorld(2);crt::Scenario scene;scene.players.resize(3);
        scene.players[1].push_back(onStart({{5,{}},{6,{}}}));
        scene.players[2].push_back({{{1,{"0"}}},{{6,{}}}});
        attach(*world,registry,scene);world->updateOutcome();
        check(world->scenarioOutcome(0)==1 && world->scenarioOutcome(1)==0 &&
              !world->player(1).defeated,"individual victory fabricated an opponent defeat");
        check(world->player(0).defeated,"later explicit loss failed to mark gameplay defeat after a latched win");
        ticks(*world,30);
        check(world->scenarioOutcome(0)==1 && world->scenarioOutcome(1)==-1 &&
              world->scenarioOutcome(-1)==1,"completed mixed results did not aggregate");
        check(world->winningTeam()==0,"completed sole winning team missing");
    }
    {
        // Native loss marks the player immediately, before later players' conditions.
        auto world=makeWorld(2);crt::Scenario scene;scene.players.resize(3);
        scene.players[1].push_back(onStart({{6,{}}}));
        scene.players[2].push_back({{{21,{"1"}}},{{5,{}}}});
        attach(*world,registry,scene);
        check(world->player(0).defeated && world->scenarioOutcome(1)==1,
              "later player could not observe explicit defeat in the same rule pass");
    }
    {
        auto world=makeWorld(1);crt::Scenario scene;scene.players.resize(2);
        scene.players[1].push_back(onStart({{6,{}},{5,{}}}));
        attach(*world,registry,scene);world->updateOutcome();
        check(world->scenarioOutcome(0)==-1 && world->scenarioOutcome(-1)==-1,
              "later victory replaced first defeat");
    }
    {
        auto world=makeWorld(1);crt::Scenario scene;scene.players.resize(2);
        scene.players[1].push_back(onStart({{5,{}}}));
        attach(*world,registry,scene);
        check(world->scenarioOutcome(0)==1 && world->scenarioOutcome(-1)==1,
              "solo authored victory requires a second team");
    }
    {
        auto world=makeWorld(2);crt::Scenario scene;scene.players.resize(3);
        attach(*world,registry,scene);ticks(*world,60);
        check(!world->player(0).defeated && !world->player(1).defeated &&
              world->scenarioOutcome(-1)==0,"empty authored armies automatically defeated");
        const int id=world->spawn(monarch,128,128,0,0);ticks(*world,1);
        world->unit(id)->hp=sim::Fixed();ticks(*world,1);
        check(!world->player(0).defeated && world->scenarioOutcome(0)==0,
              "monarch loss overrode authored outcome rules");
        world->forceDefeat(0);ticks(*world,1);
        check(world->player(0).defeated && world->scenarioOutcome(0)==-1 &&
              world->scenarioOutcome(-1)==0,"explicit forfeit did not defeat just its participant");
        world->forceDefeat(1);ticks(*world,1);
        check(world->scenarioOutcome(-1)==-1,"all forfeits did not finish spectator result");
    }
    {
        auto world=makeWorld(2);
        const int first=world->spawn(monarch,128,128,0,0);
        const int second=world->spawn(monarch,192,128,0,0);
        const int enemy=world->spawn(monarch,320,320,0,1);
        world->unit(first)->lastHitBy=enemy;
        world->unit(first)->lastHitPlayer=1;
        world->unit(second)->underConstruction=true;
        world->unit(second)->selfDestructT=60;
        check(!world->canPlace(monarch,128,128,1),"fixture structure did not block placement");
        crt::Scenario scene;scene.players.resize(3);
        scene.players[1].push_back(onStart({{6,{}}}));
        scene.players[1].push_back({{{9,{"1","monarch"}}},{{13,{"All Players","removed"}}}});
        attach(*world,registry,scene);
        check(!world->unit(first)->alive() && !world->unit(second)->alive() && world->unit(enemy)->alive(),
              "explicit defeat did not retire exactly its owner's units immediately");
        check(world->unit(first)->deathType==10 && world->unit(first)->severity==0 &&
              world->unit(first)->corpseUntil==0 && !world->unit(first)->deathHasCorpse,
              "scenario removal used an ordinary animated or corpse-producing death");
        check(world->canPlace(monarch,128,128,1),"retired structure left blocked terrain");
        // 5131b0's kind 10 is no loss (512ae0, check_kill_credit.py losses),
        // and 512610 keeps the last hitter (cargo).
        check(world->player(0).unitCount==0 && world->player(0).losses==0 &&
              world->player(1).kills==0 && world->player(1).score==0,
              "scenario removal credited an earlier attacker, counted losses or kept the unit cap occupied");
        check(world->unit(first)->lastHitBy==enemy && world->unit(first)->lastHitPlayer==1,
              "scenario removal cleared the last hitter");
        const auto messages=world->scenario()->drainMessages();
        check(messages.size()==1 && messages[0].text=="removed",
              "later rules did not see removal loss counters in the same pass");
        world->forceDefeat(0);ticks(*world,60);
        check(world->player(0).losses==0 && world->unit(enemy)->alive(),
              "repeated defeat duplicated losses or removed another owner's unit");
    }
    {
        // A site left to unconjure away dies (429cea: 51a140(site, site, 30000,
        // 11, 0), check_kill_credit.py unconjure): its own last hitter, kind
        // 11, no loss, and its retirement reaches the scenario's lost tally.
        const auto* hut=registry.find("hut");check(hut,"synthetic hut missing");
        auto world=makeWorld(2);
        const int site=world->spawn(hut,128,128,0,0);world->spawn(monarch,320,320,0,1);
        world->unit(site)->underConstruction=true;world->unit(site)->beingBuilt=false;
        world->unit(site)->hp=sim::Fixed::fromFloat(0.5f);
        crt::Scenario scene;scene.players.resize(3);
        scene.players[1].push_back({{{9,{"0","hut"}}},{{13,{"All Players","decayed"}}}});
        attach(*world,registry,scene);
        // Never worked: its GetBuilt waits 300 + 30 ticks (check_site_decay.py),
        // then the first decay finds no work left to give back and removes it.
        for (int i=0;i<400 && world->unit(site) && world->unit(site)->alive();++i) ticks(*world,1);
        const auto* u=world->unit(site);
        check(u && !u->alive(),"the unattended site did not unconjure away");
        check(u->deathType==11 && u->lastHitBy==site && u->lastHitPlayer==0,
              "an unconjured site was not its own kind-11 hitter");
        check(world->player(0).losses==0,"an unconjured site counted as a loss");
        ticks(*world,30);   // the rules' next pass
        const auto messages=world->scenario()->drainMessages();
        check(messages.size()==1 && messages[0].text=="decayed","an unconjured site was not counted as lost");
    }
    {
        // Closed holes are not recipients or phantom opponents. Bank3 belongs to owner2.
        auto world=makeWorld(4);world->setTeam(1,0);
        crt::Scenario scene;scene.players.resize(9);
        scene.players[1].push_back(onStart({{21,{}}}));
        scene.players[3].push_back(onStart({{6,{}}}));
        scene.players[2].push_back(onStart({{24,{}}})); // Closed player's rules must not execute.
        attach(*world,registry,scene,-1,0b0101);world->updateOutcome();
        check(world->scenarioOutcome(0)==1 && world->scenarioOutcome(2)==-1 &&
              world->scenarioOutcome(1)==0 && world->scenarioOutcome(3)==0,
              "sparse participant mask remapped rules or included closed slots");
        check(world->scenarioOutcome(-1)==1,"closed slots prevented completed aggregation");
    }
    {
        // Global rules have a separate disabled copy for each player and precede local rules.
        auto world=makeWorld(2);crt::Scenario scene;scene.players.resize(3);
        scene.players[0].push_back({{{19,{}}},{{2,{"global","1"}},{13,{"All Players","global"}},{14,{}}}});
        scene.players[1].push_back({{{17,{"global","0"}}},{{13,{"All Players","owner0"}},{14,{}}}});
        scene.players[2].push_back({{{17,{"global","0"}}},{{13,{"All Players","owner1"}},{14,{}}}});
        attach(*world,registry,scene);
        const auto messages=world->scenario()->drainMessages();
        check(messages.size()==4 && messages[0].text=="global" && messages[1].text=="owner0" &&
              messages[2].text=="global" && messages[3].text=="owner1",
              "All Players order or independent rule enable state wrong");
        ticks(*world,90);
        check(world->scenario()->drainMessages().empty(),"disabled rule fired again");
    }
    {
        auto world=makeWorld(1);crt::Scenario scene;scene.players.resize(2);
        scene.players[1].push_back(onStart({{0,{"0","1"}},{13,{"All Players","start"}}}));
        scene.players[1].push_back({{{19,{}}},{{13,{"All Players","repeat"}}}});
        scene.players[1].push_back({{{4,{"0","0"}}},{{13,{"All Players","negative"}},{14,{}}}});
        scene.players[1].push_back({{},{{13,{"All Players","empty conditions"}},{14,{}}}});
        attach(*world,registry,scene);
        auto messages=world->scenario()->drainMessages();
        check(messages.size()==3 && messages[0].text=="start" && messages[1].text=="repeat" &&
              messages[2].text=="empty conditions" && messages[0].t==0,
              "initial evaluation or empty-condition execution wrong");
        ticks(*world,29);
        check(world->scenario()->drainMessages().empty(),"rules ran before one game second");
        ticks(*world,1);messages=world->scenario()->drainMessages();
        check(messages.size()==1 && messages[0].text=="repeat" && messages[0].t==30,
              "persistent true condition did not repeat at tick30");
        ticks(*world,30);messages=world->scenario()->drainMessages();
        check(messages.size()==2 && messages[0].text=="repeat" && messages[1].text=="negative" &&
              messages[1].t==60,"countdown was clamped or advanced at wrong cadence");
    }
    {
        crt::Scenario scene;scene.players.resize(3);
        scene.players[0].push_back(onStart({{13,{"Player 1","private0"}},{13,{"Player 2","private1"}}}));
        scene.players[1].push_back({{{1,{"0"}}},{{5,{}}}});
        scene.players[2].push_back({{{1,{"0"}}},{{6,{}}}});
        auto client=makeWorld(2),peer=makeWorld(2),referee=makeWorld(2);
        attach(*client,registry,scene,0);attach(*peer,registry,scene,1);attach(*referee,registry,scene,-1);
        check(client->scenario()->drainMessages().size()==2 && peer->scenario()->drainMessages().size()==2 &&
              referee->scenario()->drainMessages().size()==4,"viewer recipient filtering wrong");
        for(int i=0;i<90;++i) {
            check(client->stateHash()==peer->stateHash() && client->stateHash()==referee->stateHash(),
                  "viewer-dependent scenario results or rule state desync");
            ticks(*client,1);ticks(*peer,1);ticks(*referee,1);
        }
        check(client->scenarioOutcome(-1)==1 && peer->scenarioOutcome(-1)==1 &&
              referee->scenarioOutcome(-1)==1,"peer/referee terminal results differ");
    }
    std::cout << "PASS: authored result recipients, rule ownership, scheduler, solo/sparse outcomes and lockstep\n";
    return 0;
} catch(const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';return 1;
}
