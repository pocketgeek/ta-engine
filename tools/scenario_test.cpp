#include "sim/scenario.h"
#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "tnt/ota.h"
#include "hpi/hpi.h"
#include "util/scenariotrace.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace tak;
static void check(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
int main() try {
    {
        crt::Scenario source;source.units.push_back({"soldier","named"});
        source.players.resize(9);source.players[0].push_back({{{19,{}}},{{13,{"All Players","message"}}}});
        source.regions.push_back({"Region",1,2,3,4});source.customTypes.push_back({"soldier"});
        const auto bytes=crt::write(source);
        check(crt::parse(bytes).version==1,"valid CRT rejected");
        for(size_t n=0;n<bytes.size();++n) {
            const auto truncated=crt::parse({bytes.begin(),bytes.begin()+n});
            check(truncated.version==0 && truncated.units.empty() && truncated.players.empty() &&
                  truncated.regions.empty() && truncated.customTypes.empty(),"truncated CRT accepted or partial state leaked");
        }
        for(const uint32_t version : {0x7fc00000u,0x7f800000u,0x40000000u}) {
            auto invalid=bytes;for(int i=0;i<4;++i)invalid[i]=uint8_t(version>>(i*8));
            check(crt::parse(invalid).version==0,"invalid CRT version accepted");
        }
        auto residue=bytes;residue[4+4+256-1]=0x80; // Retail raw records may retain bytes after a NUL.
        check(crt::parse(residue).version==1,"retail record residue rejected");
    }
    auto files=std::make_shared<hpi::Vfs::Files>();
    for(const char* name:{"soldier","builder","araking","tarnecro"}) {
        const auto text=std::string("[UNITINFO]{\nunitname=")+name+";\nobjectname="+name+
            ";\nname=Display "+name+";\nmaxdamage=100;\nfootprintx=1;\nfootprintz=1;\ncanmove=1;\nmaxvelocity=1;\n}";
        (*files)[std::string("units/")+name+".fbi"]={text.begin(),text.end()};
    }
    hpi::Vfs vfs;vfs.setMapFiles(files);sim::TypeRegistry registry;registry.loadDir(vfs,"units");
    const auto* soldier=registry.find("soldier");const auto* builder=registry.find("builder");
    check(soldier && builder,"synthetic types missing");
    sim::World world;world.setPlayerCount(2);
    world.setTerrain(std::vector<uint8_t>(64*64,40),64,64,0);
    world.spawn(soldier,168,168,0,0);
    world.spawn(builder,184,168,0,0);
    world.spawn(soldier,600,600,0,0); // Outside the named region.
    world.spawn(soldier,168,184,0,1); // Another owner's unit does not count.
    int dead=world.spawn(soldier,184,184,0,0);world.unit(dead)->hp=sim::Fixed();world.unit(dead)->deadFor=0;
    auto fires=[&](int opcode,std::string first,std::string second,std::string third="") {
        crt::Scenario scenario;scenario.players.resize(9);
        scenario.regions.push_back({"Hill",10,10,12,12});
        crt::RuleGroup group;group.conditions.push_back({opcode,{first,second,third}});
        group.actions.push_back({13,{"Player 1","fired"}});scenario.players[1].push_back(group);
        // Exercise the actual serialized operand order used by Cartographer.
        scenario=crt::parse(crt::write(scenario));
        sim::ScenarioScript script(scenario,registry,0,2,64,64);script.start(world);script.step(world,1.f/30);
        auto messages=script.drainMessages();
        script.step(world,1.f/30);check(script.drainMessages().empty(),"condition repeated before the next game second");
        return !messages.empty();
    };
    check(fires(15,"0","soldier","Hill"),"control greater-than uses count then type");
    check(!fires(15,"1","soldier","Hill"),"greater-than excludes equality, dead units and other owners");
    check(fires(16,"2","soldier","Hill"),"control less-than uses count then type");
    check(!fires(16,"1","soldier","Hill"),"less-than excludes equality");
    check(fires(15,"1","aNy UnIt","Hill"),"Any Unit counts multiple types");
    check(!fires(15,"2","Any Unit","Hill"),"Any Unit respects region bounds");
    check(fires(15,"2","Any Unit","Anywhere"),"Anywhere counts units outside named regions");
    check(fires(15,"0","missing","Hill"),"native unknown type resolves to wildcard");
    check(fires(13,"Any Unit","Hill"),"control-most supports Any Unit");
    check(!fires(14,"Any Unit","Hill"),"control-least supports Any Unit");
    {
        crt::Scenario scenario;scenario.players.resize(9);
        crt::RuleGroup group;group.conditions.push_back({0,{}});
        group.actions.push_back({2,{"progress","42"}});
        group.actions.push_back({25,{"Player 1","Collected ","progress"," items"}});
        group.actions.push_back({25,{"Player 2","Remaining ","unset"," items"}});
        scenario.players[1].push_back(group);scenario=crt::parse(crt::write(scenario));
        sim::ScenarioScript human(scenario,registry,0,2,64,64),server(scenario,registry,-1,2,64,64);
        std::vector<int> actions;
        human.setTraceSink([&](int32_t tick,int player,int group,int action,const crt::Rule* rule) {
            check(tick==0 && player==0 && group==0,"trace group/tick differs from execution");
            actions.push_back(rule?rule->opcode:-1);
            check(action==int(actions.size())-2,"trace action order differs");
        });
        human.start(world);server.start(world);human.step(world,1.f/30);server.step(world,1.f/30);
        const auto visible=human.drainMessages();const auto all=server.drainMessages();
        check(visible.size()==1 && visible[0].text=="Collected 42 items","flag message interpolation or recipient filtering");
        check(all.size()==1 && all[0].text=="Collected 42 items","unset flag message should be skipped");
        uint64_t clientHash=0,serverHash=0;human.foldHash(clientHash);server.foldHash(serverHash);
        check(clientHash==serverHash,"message filtering changed deterministic rule state");
        check(actions==std::vector<int>({-1,2,25,25}),"trace did not record group and each attempted action");
        sim::ScenarioScript failed(scenario,registry,0,2,64,64);
        int calls=0;failed.setTraceSink([&](auto...){++calls;throw std::runtime_error("diagnostic failure");});
        failed.start(world);failed.step(world,1.f/30);uint64_t failedHash=0;failed.foldHash(failedHash);
        check(calls==1 && failedHash==serverHash,"failed trace sink changed execution or was not disabled");
        human.step(world,1.f/30);check(actions.size()==4,"initial-only rule repeated");
        const auto root=std::filesystem::temp_directory_path()/("tak-trigger-log-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto path=tak::scenarioTracePath(root,root/"test-source"/"snapshot.kmp");
        {
            tak::ScenarioTraceLog log(path,200);
            crt::Rule rule{13,{"Player 1","line\nbreak\r\t\"quote\\"}};
            for(int i=0;i<20;++i)log.append(i,0,1,0,&rule);
        }
        std::ifstream stream(path);const std::string log{std::istreambuf_iterator<char>(stream),{}};
        check(log.find("tick=0 player=1 group=2 action=1 opcode=13")!=std::string::npos,"readable trigger coordinates missing");
        check(log.find("line\\x0abreak\\x0d\\x09\\\"quote\\\\")!=std::string::npos,"trace operands can break log lines");
        check(log.find("LOG LIMIT REACHED")!=std::string::npos && log.find("tick=19")==std::string::npos,"trigger log is unbounded");
        stream.close();std::filesystem::remove_all(root);
    }
    {
        crt::Scenario scene;scene.players.resize(9);
        scene.players[1].push_back({{{5,{"0","SOLDIER"}}},{{13,{"Player 1","kill"}}}});
        scene.players[2].push_back({{{9,{"0","SOLDIER"}}},{{13,{"Player 2","loss"}}}});
        sim::ScenarioScript script(scene,registry,-1,2,64,64);script.start(world);
        const int target=world.units()[3].id;
        world.unit(target)->lastHitBy=world.units()[0].id;world.unit(target)->lastHitPlayer=0;
        script.unitDied(world,target);for(int tick=0;tick<30;++tick)script.step(world,1.f/30);
        check(script.drainMessages().size()==2,"kill/loss conditions used localized display name instead of unit identifier");
    }
    {
        tnt::Map map;map.width=map.height=64;map.blocksX=map.blocksY=32;
        map.heights.assign(4096,40);map.features.assign(4096,0xffff);
        map.tileKeys.assign(1024,0);map.tileCols.assign(1024,0);map.tileRows.assign(1024,0);
        (*files)["kmap/authored.tnt"]=map.save();
        tnt::Scenario metadata;metadata.hasScenario=true;
        const auto ota=metadata.write();(*files)["kmap/authored.ota"]={ota.begin(),ota.end()};
        crt::Scenario scene;scene.players.resize(9);
        scene.units.push_back({"SOLDIER","first",10,12,200,0,75,100,100,180,3});
        scene.units.push_back({"builder","second",30,32,200,1});
        scene.players[1].push_back({{{0,{}}},{{13,{"Player 1","Authored world"}},{2,{"begun","1"}}}});
        (*files)["kmap/authored.crt"]=crt::write(scene);
        sim::MatchConfig config;config.vfs=&vfs;config.mapPath="kmap/authored.tnt";
        config.slots={{true,0,0},{true,1,1}};config.scenarioViewPlayer=0;
        {
            sim::World ordinary;sim::setupMatch(ordinary,registry,config);
            check(!ordinary.scenario() && ordinary.units().size()==2 && ordinary.units()[0].type==registry.find("araking"),
                  "ordinary retail CRT map changed its skirmish setup");
        }
        auto hasMonarch=[&](const sim::World& w) {
            return std::any_of(w.units().begin(),w.units().end(),[&](const sim::Unit& u) {
                return u.type==registry.find("araking") || u.type==registry.find("tarnecro"); });
        };
        {
            // MAP SCENARIO (protocol 259) plays the declared scenario of an UNMARKED map:
            // the room's option, not a Test Map copy's opt-in, turns it on.
            auto on=config;on.mapScenario=true;
            sim::World client,server;sim::setupMatch(client,registry,on);
            on.scenarioViewPlayer=-1;sim::setupMatch(server,registry,on);
            check(client.scenario() && client.units().size()==2 && client.units()[0].type==soldier &&
                  client.units()[1].type==builder && !hasMonarch(client),
                  "MAP SCENARIO did not run the declared scenario of an unmarked map");
            for(int tick=0;tick<60;++tick) {
                client.tick(1.f/30);server.tick(1.f/30);
                check(client.stateHash()==server.stateHash(),"MAP SCENARIO setup diverged between peers");
            }
            // Stress tests and benchmarks are their own setups: never a map scenario.
            for(int mode=0;mode<2;++mode) {
                auto load=config;load.mapScenario=true;load.stressTest=mode==0;load.benchmark=mode;
                sim::World w;sim::setupMatch(w,registry,load);
                check(!w.scenario() && hasMonarch(w),"a stress or benchmark room ran the map scenario");
            }
        }
        {
            // Data the .ota does not declare (hasscenario=0) is a leftover retail never ran:
            // the option leaves it alone -- Monarchs, no runner, no Use Only list.
            tnt::Scenario undeclared;undeclared.useOnlyUnits="authored.tdf";
            const auto text=undeclared.write();(*files)["kmap/authored.ota"]={text.begin(),text.end()};
            const std::string list="[SOLDIER]{}\n";(*files)["kmap/authored.tdf"]={list.begin(),list.end()};
            auto on=config;on.mapScenario=true;
            sim::World w;sim::setupMatch(w,registry,on);
            check(!w.scenario() && w.units().size()==2 && hasMonarch(w) && w.buildAllowed(builder),
                  "MAP SCENARIO ran a scenario the map does not declare");
            files->erase("kmap/authored.tdf");
            (*files)["kmap/authored.ota"]={ota.begin(),ota.end()};
        }
        const std::string optIn=ota+"\n[TAKPlaytest]{\nauthoredscenario=1;\n}\n";
        (*files)["kmap/authored.ota"]={optIn.begin(),optIn.end()};
        sim::World client,server;
        const auto positions=sim::setupMatch(client,registry,config);
        config.scenarioViewPlayer=-1;sim::setupMatch(server,registry,config);
        check(client.units().size()==2 && client.units()[0].player==0 && client.units()[1].player==1,"authored placements lost or default monarchs added");
        check(positions[0]==std::pair<float,float>{168,200},"camera does not start at authored units");
        check(client.units()[0].hp.toFloat()==75 && client.units()[0].veteran==3,"authored health/veterancy lost");
        // setLevel 4cd335: the placed level's experience, (3 + 0.5) x experiencepoints.
        check(client.units()[0].xp==int32_t(3.5*client.units()[0].type->experiencePoints),
              "authored veterancy did not set the level's experience");
        for(int tick=0;tick<60;++tick) {
            client.tick(1.f/30);server.tick(1.f/30);
            check(client.stateHash()==server.stateHash(),"authored setup or triggers diverged between peers");
        }
        check(client.scenario() && client.scenario()->drainMessages().size()==1,"normal match setup did not attach authored rules");
        {
            auto rulesOnly=scene;rulesOnly.units.clear();(*files)["kmap/authored.crt"]=crt::write(rulesOnly);
            sim::World w;sim::setupMatch(w,registry,config);
            check(w.scenario() && w.units().size()==2 && w.units()[0].type==registry.find("araking"),
                  "rules-only map lost its default monarchs");
        }
        {
            // An empty rule group does nothing, so its bank needs no seat (it used to
            // demand one: "Seat player 8" on a two-seat game).
            auto emptyBank=scene;emptyBank.players[8].push_back({});
            (*files)["kmap/authored.crt"]=crt::write(emptyBank);
            sim::World w;std::string refused;
            try {sim::setupMatch(w,registry,config);}catch(const std::exception& e) {refused=e.what();}
            check(refused.empty() && w.scenario() && w.units().size()==2,
                  ("an empty rule bank demanded a seat: "+refused).c_str());
        }
        auto refuses=[&](const crt::Scenario& invalid,const char* message) {
            (*files)["kmap/authored.crt"]=crt::write(invalid);sim::World w;bool rejected=false;
            try {sim::setupMatch(w,registry,config);}catch(const std::exception&) {rejected=true;}
            check(rejected,message);check(w.units().empty(),"failed scenario left partial placements");
        };
        {
            auto authored=scene; authored.units[1].player=8;
            authored.units[0].armor=50; authored.units[0].weapon=150;
            authored.customTypes.push_back({"soldier",{200,200,50,7}});
            (*files)["kmap/authored.crt"]=crt::write(authored);
            sim::World w,peer;sim::setupMatch(w,registry,config);sim::setupMatch(peer,registry,config);
            check(w.numPlayers()==2 && w.isNeutralPlayer(8) && w.units()[1].player==8,
                  "neutral owner became a participant or was reassigned");
            check(w.player(8).unitCount==1 && !w.allied(0,8),"neutral ownership has wrong counters or alliance");
            const auto& u=w.units()[0];
            check(u.hp.toFloat()==75 && u.type->maxHp==100 && u.veteran==3 &&
                  u.xp==int32_t(3.5*u.type->experiencePoints),
                  "placement health/veteran failed to replace type defaults");
            check(u.scenarioArmor==1.f && u.scenarioWeapon==.75f,"type and placement combat percentages did not multiply");
            for(int tick=0;tick<60;++tick) {w.tick(1.f/30);peer.tick(1.f/30);check(w.stateHash()==peer.stateHash(),"neutral/stat world diverged");}
            int id=w.spawn(soldier,400,400,0,0);
            check(w.unit(id)->scenarioArmor==2.f && w.unit(id)->scenarioWeapon==.5f && w.unit(id)->veteran==7,
                  "later spawn lost scenario type defaults");
            w.setPlayerCount(2);id=w.spawn(soldier,420,420,0,0);
            check(!w.isNeutralPlayer(8) && w.unit(id)->scenarioArmor==1.f && w.unit(id)->veteran==0,
                  "new match retained scenario defaults");
        }
        auto invalid=scene;invalid.units[0].objectName="missing";refuses(invalid,"unknown placed unit silently dropped");
        {
            // The refusal names the type, for the room's CRUSADES wording.
            (*files)["kmap/authored.crt"]=crt::write(invalid);sim::World w;std::string type;
            try {sim::setupMatch(w,registry,config);}catch(const sim::ScenarioTypeMissing& e) {type=e.type;}
            check(type=="missing","unknown placed unit refused without its type");
        }
        {
            // Retail skips a custom type no unit answers to (0x4cd243 stores type 0, which
            // no unit has): the scenario starts and the known type keeps its stats.
            auto skipped=scene;skipped.customTypes.push_back({"missing",{200,100,100,0}});
            skipped.customTypes.push_back({"soldier",{200,200,50,7}});
            (*files)["kmap/authored.crt"]=crt::write(skipped);sim::World w,peer;std::string refused;
            try {sim::setupMatch(w,registry,config);sim::setupMatch(peer,registry,config);}
            catch(const std::exception& e) {refused=e.what();}
            check(refused.empty() && w.scenario(),("missing custom type refused the scenario: "+refused).c_str());
            check(w.stateHash()==peer.stateHash(),"skipped custom type set peers up differently");
            const int id=w.spawn(soldier,400,400,0,0);
            check(w.unit(id)->scenarioArmor==2.f && w.unit(id)->veteran==7,"known custom type lost its stats beside a skipped one");
        }
        invalid=scene;invalid.units[0].x=64;refuses(invalid,"off-map placement accepted");
        config.slots.resize(1);refuses(scene,"unseated player's placements silently reassigned");
    }
    std::cout<<"PASS: CRT control operands, wildcard counts, region/owner filters and flag messages\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
