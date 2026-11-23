#include "sim/mission.h"
#include "sim/sim.h"
#include "hpi/hpi.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace tak;
namespace tak::sim { struct RetailReplayProbe {
    // A charm shot's impact on the victim, as World's weapon code makes it.
    static void impact(World& w,const Weapon& weapon,int from,int target) {
        const auto* source=w.unit(from);auto* victim=w.unit(target);
        w.applyHit(weapon,victim->x.toFloat(),victim->z.toFloat(),source->player,from,victim);
    }
}; }
int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="--native") {
        char mode;
        while(std::cin>>mode) {
            if(mode=='A') {int cell,line;std::cin>>cell>>line;std::cout<<sim::retailCampaignAxis(cell,line)<<'\n';}
            if(mode=='R') {int32_t x,z,cx,cz,r;std::cin>>x>>z>>cx>>cz>>r;std::cout<<sim::retailCampaignRadius(x,z,cx,cz,r)<<'\n';}
            if(mode=='P') {int which,owner,live,dying,selectable,finished,transferring,carried,holder;
                std::cin>>which>>owner>>live>>dying>>selectable>>finished>>transferring>>carried>>holder;
                const bool ready=sim::retailCampaignReady(live,dying,selectable,finished,transferring,carried,holder);
                std::cout<<(which ? int(!ready) : int(owner==0 && ready))<<'\n';}
            if(mode=='D') {int kind,owner,match,commander,mobile,survivor,met;sim::RetailCampaignConditionState c;
                std::cin>>kind>>owner>>match>>commander>>mobile>>survivor>>met>>c.remaining;c.met=met;
                sim::retailCampaignDeath(sim::RetailCampaignConditionState::Kind(kind),c,owner,match,commander,mobile,survivor);
                std::cout<<c.met<<' '<<c.remaining<<'\n';}
        }
        return 0;
    }
    int failed=0;
    auto check=[&](bool ok,const char* label){std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n';failed+=!ok;};
    const auto root=std::filesystem::temp_directory_path()/
        ("tak-campaign-conditions-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}} cleanup{root};
    std::filesystem::create_directories(root/"units");
    for(const char* name:{"soldier","monarch","pretender","building","thrall","dyer","wide"}) {
        std::ofstream f(root/"units"/(std::string(name)+".fbi"));
        const bool wide=std::string(name)=="wide";   // a 3x2 footprint (GET 35 / 36)
        f<<"[UNITINFO]{\nunitname="<<name<<";\nobjectname="<<name<<";\nmaxdamage=100;\nfootprintx="<<(wide?3:1)
         <<";\nfootprintz="<<(wide?2:1)<<";\n";
        if(std::string(name)=="thrall")f<<"bmcode=1;\nupright=1;\n";   // a body the charm can take (52da10)
        if(std::string(name)!="building")f<<"canmove=1;\nmaxvelocity=1;\n";
        if(std::string(name)=="monarch" || std::string(name)=="pretender")f<<"commander=1;\n";
        f<<"}";
    }
    {
        // dyer's script: a Dying that ends its death animation (SET 31), so
        // the unit retires some 35 ticks after it dies.
        const std::vector<uint32_t> code={0x10021001,31,0x10021001,0,0x10082000,0x10021001,0,0x10065000};
        std::vector<uint8_t> cob(256);
        auto put=[&](size_t offset,uint32_t value){std::memcpy(cob.data()+offset,&value,4);};
        put(0,4);put(4,1);put(12,uint32_t(code.size()));put(24,52);put(28,56);put(32,60);put(36,128);
        put(52,0);put(56,72);std::memcpy(cob.data()+72,"Dying",6);
        for(size_t i=0;i<code.size();++i)put(128+i*4,code[i]);
        std::filesystem::create_directories(root/"scripts");
        std::ofstream(root/"scripts"/"dyer.cob",std::ios::binary).write(reinterpret_cast<const char*>(cob.data()),
                                                                      std::streamsize(cob.size()));
    }
    hpi::Vfs vfs;vfs.addLayer(hpi::MountSet(root));sim::TypeRegistry registry;registry.loadDir(vfs,"units");
    const auto* soldier=registry.find("soldier");const auto* monarch=registry.find("monarch");const auto* building=registry.find("building");
    if(!soldier || !monarch || !building)return 2;
    auto make=[&](std::string rules){for(size_t i=0;(i=rules.find(';',i))!=std::string::npos;i+=2)rules.insert(i+1,"\n");auto mission=std::make_unique<sim::MissionScript>(std::vector<uint8_t>{},
        tdf::parseText("[GlobalHeader]{"+rules+"}").children.at("globalheader"),registry,0,"conditions fixture");mission->setPlayerCommanders({monarch,monarch,monarch});return mission;};
    auto dead=[&](sim::World& w,sim::MissionScript& m,int id){w.unit(id)->deadFor=0;m.unitDied(w,id);};
    {
        sim::World w;w.spawn(soldier,500,500,0,0);int one=w.spawn(monarch,500,500,0,1),two=w.spawn(monarch,500,500,0,1);
        auto m=make("KillEnemyCommander=1;");m->start(w);dead(w,*m,one);m->step(w,1.f/30);
        check(m->outcome()==1 && w.unit(two)->alive(),"enemy commander death is an event even if another commander survives");
    }
    {
        sim::World w;w.spawn(soldier,500,500,0,0);int a=w.spawn(soldier,500,500,0,1),b=w.spawn(soldier,500,500,0,1);
        auto m=make("KillUnitType=soldier,2;VictoryTimerRunsOut=1;");m->start(w);
        dead(w,*m,a);m->step(w,1.f/30);check(m->outcome()==0,"kill counter waits for required enemy deaths");
        dead(w,*m,b);m->step(w,1.f/30);check(m->outcome()==0,"all victory conditions must hold");
        for(int i=2;i<30;++i)m->step(w,1.f/30);
        check(m->outcome()==1,"latched kill quota combines with timer at exactly30ticks");
    }
    {
        sim::World w;int human=w.spawn(monarch,500,500,0,0);w.spawn(soldier,500,500,0,0);w.spawn(soldier,500,500,0,1);
        auto m=make("VictoryTimerRunsOut=100;CommanderKilled=1;DeathTimerRunsOut=100;");m->start(w);dead(w,*m,human);m->step(w,1.f/30);
        check(m->outcome()==-1,"any defeat condition suffices without losing every soldier");
    }
    {
        sim::World w;w.spawn(soldier,500,500,0,1);int escort=w.spawn(soldier,10*16,10*16,0,0);
        auto m=make("UnitTypePassesZ=soldier,10;");m->start(w);m->step(w,1.f/30);
        check(m->outcome()==1,"axis condition already within two cells wins immediately");
        auto n=make("UnitTypePassesZ=soldier,10;");w.unit(escort)->z=sim::Fixed::fromInt(7*16);n->start(w);n->step(w,1.f/30);
        check(n->outcome()==0,"axis condition outside tolerance does not infer crossing side");
        w.unit(escort)->z=sim::Fixed::fromInt(12*16);n->step(w,1.f/30);check(n->outcome()==1,"axis tolerance includes exactly two cells");
    }
    {
        sim::World w;w.spawn(soldier,320,320,0,0);int enemy=w.spawn(soldier,160,160,0,1);
        auto m=make("MoveUnitToRadius=soldier,10,10,1;");m->start(w);m->step(w,1.f/30);
        check(m->outcome()==0,"enemy unit cannot satisfy human radius objective");
        w.unit(enemy)->player=0;m->step(w,1.f/30);check(m->outcome()==1,"radius uses authored origin without extra half cell");
    }
    {
        sim::World w;w.spawn(building,100,100,0,0);w.spawn(soldier,100,100,0,1);
        auto m=make("KillEnemyCommander=0;VictoryTimerRunsOut=0;AllUnitsKilled=1;");m->start(w);m->step(w,1.f/30);
        check(m->outcome()==0,"disabled boolean/timer conditions do not complete; own building prevents all-units defeat");
    }
    {
        // 523eb0 / 5243f0 count a unit only by retail's selection predicate:
        // one whose mission orders still run (selectable clear) neither
        // reaches the radius nor keeps its owner alive for AllUnitsKilled
        // (E: check_campaign_conditions.py, the P rows).
        sim::World w;int walker=w.spawn(soldier,160,160,0,0);const int home=w.spawn(soldier,900,100,0,0);
        w.spawn(soldier,900,900,0,1);w.unit(walker)->selectable=false;
        auto m=make("MoveUnitToRadius=soldier,10,10,1;");m->start(w);m->step(w,1.f/30);
        check(m->outcome()==0,"a unit still on its mission orders does not reach the radius");
        w.unit(walker)->selectable=true;m->step(w,1.f/30);
        check(m->outcome()==1,"it does once its orders are done");
        w.unit(walker)->selectable=false;w.unit(home)->selectable=false;
        auto n=make("VictoryTimerRunsOut=100;AllUnitsKilled=1;");n->start(w);n->step(w,1.f/30);
        check(n->outcome()==-1,"a side left only with units on mission orders meets AllUnitsKilled");
    }
    {
        sim::World w;w.spawn(soldier,100,100,0,0);int enemy=w.spawn(soldier,100,100,0,1);
        auto m=make("CaptureUnitType=soldier;");m->start(w);m->unitCaptured(w,enemy);w.unit(enemy)->player=0;m->step(w,1.f/30);
        check(m->outcome()==1,"capture condition latches matching enemy conversion");
    }
    {
        sim::World w;w.spawn(soldier,100,100,0,0);w.spawn(soldier,100,100,0,1);int target=w.spawn(building,100,100,0,0);
        w.unit(target)->underConstruction=true;auto m=make("BuildUnitType=building;");m->start(w);m->step(w,1.f/30);
        check(m->outcome()==0,"unfinished building does not satisfy build condition");
        w.unit(target)->underConstruction=false;m->step(w,1.f/30);check(m->outcome()==1,"completed owned building satisfies build condition");
    }
    {
        sim::World w;w.spawn(soldier,100,100,0,0);int enemy=w.spawn(registry.find("pretender"),100,100,0,1);
        auto m=make("KillEnemyCommander=1;");m->start(w);dead(w,*m,enemy);m->step(w,1.f/30);
        check(m->outcome()==0,"commander flag alone cannot substitute for authored faction monarch");
    }
    {
        sim::World w;w.spawn(soldier,100,100,0,0);w.spawn(soldier,100,100,0,1);
        auto m=make("VictoryTimerRunsOut=1;DeathTimerRunsOut=1;");m->start(w);
        for(int i=0;i<30;++i)m->step(w,1.f/30);
        check(m->outcome()==1,"simultaneous normal campaign outcome resolves victory first");
    }
    {
        sim::World w;w.spawn(soldier,100,100,0,0);
        auto m=make("");m->start(w);m->step(w,1.f/30);
        check(m->outcome()==0,"empty campaign victory list waits for a scripted outcome even without enemies");
    }
    {
        sim::World w;w.spawn(soldier,100,100,0,0);w.spawn(soldier,0,100,0,1);
        auto m=make("AnyUnitPassesX=0;");m->start(w);m->step(w,1.f/30);
        check(m->outcome()==-1,"enemy axis defeat accepts authored zero line");
    }
    // A Start script that runs one MAP_COMMAND `verb`(a0, a1).
    auto commandScript=[](const char* verb,uint32_t a0,uint32_t a1) {
        const std::vector<uint32_t> code={0x10021001,a0,0x10021001,a1,
            0x10073000,0,2,0x10024000,0x10021001,0,0x10065000};
        std::vector<uint8_t> cob(512);
        auto put=[&](size_t offset,uint32_t value){std::memcpy(cob.data()+offset,&value,4);};
        put(0,6);put(4,1);put(12,uint32_t(code.size()));put(24,52);put(28,56);
        put(36,128);put(44,60);put(48,1);put(52,0);put(56,72);put(60,80);
        std::memcpy(cob.data()+72,"Start",6);std::memcpy(cob.data()+80,verb,std::strlen(verb)+1);
        for(size_t i=0;i<code.size();++i)put(128+i*4,code[i]);
        return cob;
    };
    for(int next:{1,0}) {
        sim::World w;w.spawn(soldier,100,100,0,0);int enemy=w.spawn(soldier,100,100,0,1);
        w.unit(enemy)->xp=5*soldier->experiencePoints;w.unit(enemy)->veteran=w.unit(enemy)->veteranLevel();
        tdf::Node header;header.values["captureunittype"]="soldier";
        sim::MissionScript m(commandScript("Capture",uint32_t(enemy),uint32_t(next)),header,registry,0,"capture fixture");
        // Start runs at once (4d2dc0's run-now flag), so its capture is
        // judged by the first tick's outcome check.
        m.start(w);m.step(w,1.f/30);
        check(m.outcome()==(next==0?1:0) && w.unit(enemy)->player==next,
            next==0?"scripted ownership transfer satisfies capture objective":"same-owner scripted Capture is a no-op");
        // Retail's Capture goes through 514da0: the new object has no experience.
        check(next==0 ? w.unit(enemy)->xp==0 && w.unit(enemy)->veteran==0 : w.unit(enemy)->veteran==5,
            next==0?"scripted Capture kept the unit's experience":"same-owner scripted Capture changed experience");
    }
    for(int cap:{0,1}) {
        // 514da0 kills the old object of a change of owner (kind 6, retired at
        // once): the conditions hear the death of an enemy soldier (523510),
        // so a capture counts toward KillUnitType. The capture event (523570)
        // comes before the new object is made, so it holds even when the new
        // owner's unit cap refuses it, and then nothing else changes.
        sim::World w;w.setTerrain(std::vector<uint8_t>(64*64,50),64,64,0);
        w.spawn(soldier,100,100,0,0);int enemy=w.spawn(soldier,300,100,0,1);
        if(cap)w.setUnitCap(1);
        tdf::Node header;header.values["killunittype"]="soldier,1";
        auto m=std::make_unique<sim::MissionScript>(commandScript("Capture",uint32_t(enemy),0),header,registry,0,"capture death fixture");
        auto* mission=m.get();w.setMission(std::move(m));
        for(int i=0;i<3;++i)w.tick(1.f/30);
        check(cap ? mission->outcome()==0 && w.unit(enemy)->player==1 :
                    mission->outcome()==1 && w.unit(enemy)->player==0 && w.unit(enemy)->alive(),
            cap?"a capture refused at the unit cap killed nothing":"a capture was not a death for KillUnitType (514da0 kind 6)");
        tdf::Node held;held.values["captureunittype"]="soldier";
        sim::World v;v.setTerrain(std::vector<uint8_t>(64*64,50),64,64,0);
        v.spawn(soldier,100,100,0,0);int other=v.spawn(soldier,300,100,0,1);
        if(cap)v.setUnitCap(1);
        auto n=std::make_unique<sim::MissionScript>(commandScript("Capture",uint32_t(other),0),held,registry,0,"capture event fixture");
        auto* event=n.get();v.setMission(std::move(n));
        for(int i=0;i<3;++i)v.tick(1.f/30);
        check(event->outcome()==1 && v.unit(other)->player==(cap?1:0),
            cap?"the capture event waited for the new object (523570 comes first)":"capture objective after a change of owner");
    }
    for(int cap:{0,1}) {
        // The charm (52da10) hands a unit its roll took to 514da0 with no
        // check of the new owner's room (S; E: probe_los_status.py at the
        // unit cap), and 514da0 sends the capture event (523570) before the
        // cap refuses the new object (check_change_owner.py). So a charm at
        // the unit cap meets CaptureUnitType though the unit stays.
        const auto* thrall=registry.find("thrall");
        sim::UnitType caster=*soldier;caster.id="caster";
        sim::Weapon charm;charm.damage=1;charm.range=400;charm.reload=0.1f;charm.mindControl=true;
        charm.kind=sim::Weapon::Kind::Remote;charm.remote=sim::Weapon::RemoteKind::MindCtl;charm.unitsOnly=true;charm.aoe=32;
        caster.weapon=charm;caster.weapons={charm};
        sim::World w;w.setTerrain(std::vector<uint8_t>(64*64,50),64,64,0);
        const int a=w.spawn(&caster,300,300,0,0),b=w.spawn(thrall,340,300,0,1);
        if(cap)w.setUnitCap(1);
        auto m=make("CaptureUnitType=thrall;");auto* event=m.get();w.setMission(std::move(m));
        for(int i=0;i<20 && w.unit(b)->player==1;++i)sim::RetailReplayProbe::impact(w,charm,a,b);
        for(int i=0;i<3;++i)w.tick(1.f/30);
        check(event->outcome()==1 && w.unit(b)->player==(cap?1:0),
            cap?"a charm at the unit cap sent no capture event (52da10 calls 514da0 regardless)"
               :"a charm did not meet CaptureUnitType");
    }
    {
        // Retail's Kill sends death kind 10 (513130 -> 512610): no kill credit.
        sim::World w;int mine=w.spawn(soldier,100,100,0,0);int enemy=w.spawn(soldier,300,100,0,1);
        w.unit(enemy)->lastHitBy=mine;w.unit(enemy)->lastHitPlayer=0;
        sim::MissionScript m(commandScript("Kill",uint32_t(enemy),0),tdf::Node{},registry,0,"kill fixture");
        m.start(w);m.step(w,1.f/30);
        for(int i=0;i<3;++i)w.tick(1.f/30);
        check(!w.unit(enemy)->alive() && w.unit(mine)->xp==0,"scripted Kill credited the last attacker");
    }
    {
        // 523510 (the conditions' death event) and the scenario's +0x18
        // (UnitDestroyed) are retirement's (512ae0), not the death's: a unit
        // whose Dying runs is heard of when its Dying ends.
        const auto* dyer=registry.find("dyer");
        check(dyer && dyer->script(),"the dyer fixture has its script");
        if(!dyer)return 1;
        sim::World w;w.setTerrain(std::vector<uint8_t>(64*64,50),64,64,0);
        w.spawn(soldier,100,100,0,0);const int enemy=w.spawn(dyer,300,100,0,1);
        tdf::Node header;header.values["killunittype"]="dyer,1";
        auto m=std::make_unique<sim::MissionScript>(std::vector<uint8_t>{},header,registry,0,"retirement fixture");
        auto* mission=m.get();w.setMission(std::move(m));
        w.tick(1.f/30);
        w.unit(enemy)->hp=sim::Fixed();w.unit(enemy)->overkill=sim::Fixed::fromInt(50);w.unit(enemy)->deathType=1;
        int ticks=0;bool early=false;
        while(w.unit(enemy)->deathPending || w.unit(enemy)->alive()) {
            w.tick(1.f/30);++ticks;
            if(ticks>200)break;
            if(w.unit(enemy)->deathPending)early|=mission->outcome()!=0;
        }
        check(ticks>30 && ticks<=200,"the dyer's Dying ran to its end");
        check(!early,"a death was not heard before its Dying ended");
        w.tick(1.f/30);
        check(mission->outcome()==1,"a retired unit's death reached the conditions");
    }
    {
        // The mission GET table, 4d4060 (E: tools/re/check_mission_get.py).
        // Its player argument is the 0-based record index Create takes:
        // GET(5, 1) is .ota Player2's population word (+0x24ec), every object
        // from its creation to its retirement, a unit being built and one
        // whose Dying runs too. GET_UNIT_VALUE(7), the same routine, is the
        // unit cap; 31 is the owner byte (10 for none); 35/36 are the
        // footprint-origin cell (+0x74/+0x76); 40 a record's score.
        sim::World w;w.setUnitCap(200);
        const auto* wide=registry.find("wide");
        for(int i=0;i<3;++i)w.spawn(soldier,100,100,0,0);
        int enemy[5];for(int& id:enemy)id=w.spawn(soldier,300,100,0,1);
        w.unit(enemy[1])->underConstruction=true;
        w.unit(enemy[2])->hp=sim::Fixed();w.unit(enemy[2])->deadFor=0;w.unit(enemy[2])->deathPending=true;  // Dying runs
        w.unit(enemy[3])->hp=sim::Fixed();w.unit(enemy[3])->deadFor=0;                                       // retired
        const int big=w.spawn(wide,165,170,0,1);
        w.player(1).score=1234;
        sim::MissionScript m(std::vector<uint8_t>{},tdf::Node{},registry,0,"get fixture",{-1,0,1});
        check(m.get(w,5,{1,0,0,0})==5,"GET(5, 1) is Player2's population, a unit being built and a dying one included");
        check(m.get(w,5,{0,0,0,0})==3,"GET(5, 0) is Player1's");
        check(m.get(w,7,{})==200 && m.get(w,7,{1,0,0,0})==200,"GET_UNIT_VALUE(7) is the unit cap");
        check(m.get(w,31,{enemy[0],0,0,0})==1 && m.get(w,31,{0,0,0,0})==10,"GET(31) is the owner byte, 10 for no unit");
        check(m.get(w,35,{big,0,0,0})==9 && m.get(w,36,{big,0,0,0})==10,"GET(35/36) is the footprint-origin cell");
        check(m.get(w,40,{1,0,0,0})==1234,"GET(40, 1) is Player2's score");
    }
    {
        // UnitDestroyed starts with the run-now flag (4d2d90 -> 56c680), so its
        // first slice runs inside the retirement (512ae0), before 512e52 takes
        // the unit off its owner's population word: GET(5) there counts the
        // retiring unit and those retiring after it in the same tick.
        // UnitDestroyed: WriteValue pop, GET(5, 1).
        const std::vector<uint32_t> code={0x10021001,5,0x10021001,1,0x10021001,0,0x10021001,0,0x10021001,0,
            0x10043000,0x10073000,0,1,0x10024000,0x10021001,0,0x10065000};
        std::vector<uint8_t> cob(512);
        auto put=[&](size_t offset,uint32_t value){std::memcpy(cob.data()+offset,&value,4);};
        put(0,6);put(4,1);put(12,uint32_t(code.size()));put(24,52);put(28,56);
        put(36,160);put(44,60);put(48,1);put(52,0);put(56,72);put(60,96);
        std::memcpy(cob.data()+72,"UnitDestroyed",14);std::memcpy(cob.data()+96,"WriteValue pop",15);
        for(size_t i=0;i<code.size();++i)put(160+i*4,code[i]);
        sim::World w;w.setTerrain(std::vector<uint8_t>(64*64,50),64,64,0);
        w.spawn(soldier,100,100,0,0);
        int enemy[3];for(int& id:enemy)id=w.spawn(soldier,300,100,0,1);
        auto m=std::make_unique<sim::MissionScript>(cob,tdf::Node{},registry,0,"population fixture");
        auto* mission=m.get();w.setMission(std::move(m));
        w.tick(1.f/30);
        auto kill=[&](int id){w.unit(id)->hp=sim::Fixed();w.unit(id)->overkill=sim::Fixed::fromInt(50);w.unit(id)->deathType=1;};
        kill(enemy[0]);
        for(int i=0;i<3;++i)w.tick(1.f/30);
        check(mission->value("pop")==3,"a UnitDestroyed's GET(5) still counts the retiring unit");
        kill(enemy[1]);kill(enemy[2]);
        for(int i=0;i<3;++i)w.tick(1.f/30);
        check(mission->value("pop")==1,"and those retiring after it in the same tick, not those before");
    }
    return failed?1:0;
}
