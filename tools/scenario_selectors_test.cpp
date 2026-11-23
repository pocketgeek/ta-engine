#include "sim/scenario.h"
#include "sim/sim.h"
#include "hpi/hpi.h"
#include <iostream>
#include <stdexcept>
using namespace tak;
static void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main()try {
    auto files=std::make_shared<hpi::Vfs::Files>();
    for(const char* id:{"small","large"}){
        std::string text=std::string("[UNITINFO]{\nunitname=")+id+";\nobjectname="+id+";\nmaxdamage=100;\nfootprintx="+(std::string(id)=="large"?"3":"1")+";\nfootprintz=1;\n}";
        (*files)[std::string("units/")+id+".fbi"]={text.begin(),text.end()};
    }
    hpi::Vfs vfs;vfs.setMapFiles(files);sim::TypeRegistry reg;reg.loadDir(vfs,"units");
    const auto* small=reg.find("small");const auto* large=reg.find("large");check(small&&large,"types missing");
    sim::World world;world.setPlayerCount(3);world.setTerrain(std::vector<uint8_t>(64*64,40),64,64,0);
    world.spawn(small,168,168,0,0);world.spawn(small,200,200,0,0);
    int unfinished=world.spawn(small,184,184,0,0);world.unit(unfinished)->underConstruction=true;
    int dying=world.spawn(small,184,184,0,0);world.unit(dying)->hp={};
    world.spawn(large,216,184,0,0); // origin cell12, centre cell13
    world.spawn(small,-8,168,0,0); // Anywhere includes native margin; namedregion does not
    world.spawn(small,168,168,0,1);
    auto source=[&](crt::Rule cond){crt::Scenario s;s.players.resize(9);s.regions={{"Hill",10,10,12,12},{"Reverse",12,12,10,10}};s.players[1].push_back({{cond},{{13,{"Player 1","yes"}}}});return s;};
    auto fires=[&](crt::Rule cond,uint32_t participants=3){auto s=source(cond);sim::ScenarioScript script(s,reg,0,3,64,64,participants);script.start(world);return !script.drainMessages().empty();};
    check(fires({15,{"2","Any Unit","hILL"}}),"inclusive footprint-origin region failed");
    check(!fires({15,{"3","Any Unit","Hill"}}),"unfinished/dead units counted");
    check(fires({15,{"3","Any Unit","Anywhere"}}),"Anywhere omitted off-map margin");
    check(!fires({15,{"0","Any Unit","missing"}}),"unknown region became wholemap");
    check(!fires({15,{"0","Any Unit",""}}),"empty region became wholemap");
    check(!fires({15,{"0","Any Unit","Reverse"}}),"inverted region silently normalized");
    check(fires({15,{"2","unknown","Hill"}}),"native unknown-type wildcard lost");
    check(fires({13,{"Any Unit","Hill"}}),"most control failed");
    check(!fires({14,{"large","missing"}}),"least control accepted tie");
    check(fires({14,{"large","missing"}},1),"solo least zero rejected");
    world.player(1).defeated=true;
    check(fires({14,{"Any Unit","Hill"}}),"defeated competitor included");world.player(1).defeated=false;
    check(!fires({18,{"unset","1"}}),"unset flag comparison became true");
    check(!fires({4,{"42","1"}}),"unset timer comparison became true");
    {
        crt::Scenario s;s.players.resize(9);
        s.players[1].push_back({{{0,{}}},{{3,{"8","missing"}},{25,{"Player 1","bad","missing",""}},
            {2,{"alpha","4"}},{3,{"3","apple"}},{4,{"2","a"}},{25,{"Player 1","","also",""}}}});
        sim::ScenarioScript script(s,reg,0,3,64,64);script.start(world);
        auto msg=script.drainMessages();check(msg.size()==1 && msg[0].text=="5","first-byte flags or unset add/display wrong");
    }
    {
        crt::Scenario s;s.players.resize(9);
        s.players[1].push_back({{{0,{}}},{{1,{"1","16777216"}},{14,{}}}});
        s.players[1].push_back({{{3,{"1","16777216"}}},{{13,{"Player 1","advanced"}},{14,{}}}});
        sim::ScenarioScript script(s,reg,0,3,64,64);script.start(world);check(script.drainMessages().empty(),"timer advanced at creation");
        for(int i=0;i<30;++i)script.step(world,1.f/30);
        check(script.drainMessages().size()==1,"large integer timer lost precision");
    }
    {
        crt::Scenario s;s.players.resize(9);
        s.players[1].push_back({{{6,{"2","Any Unit"}}},{{13,{"Player 1","first"}}}});
        s.players[1].push_back({{{5,{"1","large"}}},{{13,{"Player 1","specific"}}}});
        sim::ScenarioScript script(s,reg,0,3,64,64);
        int killer=world.spawn(small,400,400,0,0);
        world.unit(killer)->player=1; // capture after the damaging shot must not move its credit
        for(const auto* type:{small,large,large}){int id=world.spawn(type,420,420,0,1);world.unit(id)->lastHitBy=killer;world.unit(id)->lastHitPlayer=0;script.unitDied(world,id);world.unit(id)->deadFor=0;}
        script.start(world);check(script.drainMessages().size()==2,"native first-inserted wildcard death counter differs");
    }
    {
        auto mobile=*small;mobile.canMove=true;mobile.maxVel=sim::Fixed::fromInt(1);
        sim::World moves;moves.setPlayerCount(2);moves.setTerrain(std::vector<uint8_t>(64*64,40),64,64,0);
        int id=moves.spawn(&mobile,168,168,0,0);
        crt::Scenario s;s.players.resize(9);
        s.regions={{"reverse",20,30,3,13},{"extreme",2147483647,2147483647,2147483647,2147483647}};
        for(const char* location:{"reverse","extreme"}) {
            s.players[1]={{{{0,{}}},{{15,{"Any Unit","Anywhere",location}}}}};
            sim::ScenarioScript script(s,reg,0,2,64,64);script.start(moves);
            const auto* unit=moves.unit(id);check(!unit->orders.empty(),"extreme midpoint order missing");
            const auto& order=unit->orders.front();
            check(order.clickX.floorInt()==(std::string(location)=="reverse"?192:-16),"signed/wrapped midpoint x differs");
            check(order.clickZ.floorInt()==(std::string(location)=="reverse"?352:-16),"signed/wrapped midpoint z differs");
        }
    }
    std::cout<<"PASS: scenario selectors, regions, comparisons, flags and integer timers\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
