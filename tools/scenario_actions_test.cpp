#include "sim/scenario.h"
#include "sim/sim.h"
#include "hpi/hpi.h"
#include <iostream>
#include <stdexcept>
using namespace tak;
static void check(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
static void run(sim::World& w,const sim::TypeRegistry& reg,int opcode,std::initializer_list<const char*> slots) {
    crt::Scenario scene;scene.players.resize(9);
    scene.regions={{"source",2,3,4,5},{"destination",10,20,15,27}};
    crt::Rule action;action.opcode=opcode;int i=0;for(auto value:slots)action.slot[i++]=value;
    crt::RuleGroup group;group.actions.push_back(action);scene.players[1].push_back(group);
    sim::ScenarioScript script(scene,reg,0,2,64,64);script.start(w);
}
int main() try {
    auto files=std::make_shared<hpi::Vfs::Files>();
    for(const char* name:{"soldier","other"}) {
        const std::string text=std::string("[UNITINFO]{\nunitname=")+name+";\nobjectname="+name+
            ";\nmaxdamage=200;\nfootprintx=1;\nfootprintz=1;\ncanmove=1;\nmaxvelocity=1;\n}";
        (*files)[std::string("units/")+name+".fbi"]={text.begin(),text.end()};
    }
    hpi::Vfs vfs;vfs.setMapFiles(files);sim::TypeRegistry reg;reg.loadDir(vfs,"units");
    auto* soldier=reg.find("soldier");auto* other=reg.find("other");check(soldier&&other,"missing types");
    auto setup=[](sim::World& w){w.setPlayerCount(2);w.setTerrain(std::vector<uint8_t>(64*64,40),64,64,0);};
    {
        sim::World w;setup(w);int a=w.spawn(soldier,40,56,0,0),b=w.spawn(other,56,72,0,0),enemy=w.spawn(soldier,72,88,0,1);
        w.unit(a)->hp=sim::Fixed::fromInt(80);w.unit(b)->hp=sim::Fixed::fromInt(80);
        run(w,reg,10,{"Any unit","23","source"});
        check(w.unit(a)->hp.floorInt()==103&&w.unit(b)->hp.floorInt()==103,"heal must use raw HP and wildcard");
        check(w.unit(enemy)->hp.floorInt()==200,"heal affected another player");
        run(w,reg,11,{"soldier","23","source"});
        check(w.unit(a)->hp.floorInt()==80&&w.unit(b)->hp.floorInt()==103,"damage selector/raw HP wrong");
        run(w,reg,11,{"Any unit","65535","missing"});check(w.unit(a)->hp.floorInt()==80,"missing region became global");
        run(w,reg,10,{"Any unit","65535","source"});check(w.unit(a)->hp.floorInt()==200,"large HP addition overflowed");
        run(w,reg,11,{"Any unit","65535","source"});check(w.unit(a)->hp.floorInt()==201,"native HP word wrap differs");
        run(w,reg,11,{"Any unit","30000","source"});check(w.unit(a)->hp==sim::Fixed(),"large HP subtraction did not kill");
        run(w,reg,10,{"Any unit","23","source"});check(w.unit(a)->hp==sim::Fixed(),"heal resurrected dead unit");
    }
    {
        // Damage is 51a140(0, unit, amount, 13, 0) (4cb23b): no hitter, so the
        // last hitter stays (51a350), and an activated unit takes a damage
        // below 30000 through its damagemodifier before the 16-bit packet
        // (51a16e, 51a1d4). Heal (kind 12) is never scaled.
        auto shell=*soldier;shell.id="shell";shell.damageModifier=0x8000;
        sim::World w;setup(w);int a=w.spawn(&shell,40,56,0,0),enemy=w.spawn(soldier,72,88,0,1);
        w.unit(a)->lastHitBy=enemy;w.unit(a)->lastHitPlayer=1;
        run(w,reg,11,{"Any unit","20","source"});
        check(w.unit(a)->hp.floorInt()==180,"an inactive unit's damage was scaled");
        check(w.unit(a)->lastHitBy==enemy&&w.unit(a)->lastHitPlayer==1,"scripted damage cleared the last hitter");
        w.unit(a)->active=true;
        check(w.retailActivated(*w.unit(a)),"fixture: the unit is not activated");
        run(w,reg,11,{"Any unit","20","source"});
        check(w.unit(a)->hp.floorInt()==170,"an activated unit's damage ignored its damagemodifier");
        run(w,reg,11,{"Any unit","65556","source"});   // 30000 or more: unscaled, then its low word, 20
        check(w.unit(a)->hp.floorInt()==150,"the modifier was applied after the 16-bit truncation");
        run(w,reg,10,{"Any unit","20","source"});
        check(w.unit(a)->hp.floorInt()==170,"a heal was scaled");
        run(w,reg,11,{"Any unit","340","source"});w.tick(1.f/30);
        check(!w.unit(a)->alive()&&w.unit(a)->lastHitPlayer==1,"a scripted death lost its last hitter");
    }
    {
        sim::World w;setup(w);int a=w.spawn(soldier,40,56,0,0),b=w.spawn(other,56,72,0,0),enemy=w.spawn(soldier,72,88,0,1);
        run(w,reg,15,{"Any unit","source","destination"});
        for(int id:{a,b}) {check(w.unit(id)->orders.size()==1,"move wildcard missed unit");
            check(w.unit(id)->orders.front().clickX.floorInt()==192&&w.unit(id)->orders.front().clickZ.floorInt()==368,"move midpoint has half-cell offset");}
        check(w.unit(enemy)->orders.empty(),"move affected enemy");
        run(w,reg,8,{"Any unit","source"});check(w.unit(a)->hp==sim::Fixed()&&w.unit(b)->hp==sim::Fixed(),"destroy wildcard missed");
        check(w.unit(enemy)->hp.floorInt()==200,"destroy affected enemy");
    }
    {
        sim::World w;setup(w);int enemy=w.spawn(soldier,40,56,0,1),mine=w.spawn(soldier,56,56,0,0);
        w.unit(enemy)->hp=sim::Fixed::fromInt(23);w.unit(enemy)->squad=4;w.order(enemy,500,500,false);
        run(w,reg,9,{"Any unit","source"});
        check(w.unit(enemy)->player==0&&w.unit(enemy)->hp.floorInt()==23,"ownership changed health or failed");
        check(w.unit(enemy)->squad==0&&w.unit(enemy)->orders.empty(),"ownership kept former commands/groups");
        check(w.player(0).unitCount==2&&w.player(1).unitCount==0,"ownership counts stale");
        check(w.unit(mine)->player==0,"existing owned unit changed");
    }
    {
        sim::World w;setup(w);run(w,reg,7,{"soldier","source"});
        check(w.units().size()==1&&w.units()[0].x.floorInt()==40&&w.units()[0].z.floorInt()==56,"create must start at region first free cell");
        run(w,reg,7,{"soldier","source"});
        check(w.units().size()==2&&w.units()[1].x.floorInt()==56&&w.units()[1].z.floorInt()==56,"create did not skip occupied cell");
        run(w,reg,7,{"soldier","missing"});check(w.units().size()==2,"missing create region spawned");
    }
    {
        for (int mode:{1,2}) {
            sim::World w;setup(w);auto flying=*soldier;flying.canFly=true;
            int id=w.spawn(&flying,40,56,0,0);w.unit(id)->flightGroundMode=mode;
            const int made=w.scenarioCreate(soldier,0,2,3,4,5);
            check(w.unit(made)->x.floorInt()==(mode==1?56:40),"create primary occupancy must exclude airborne units");
        }
        sim::World w;setup(w);int id=w.spawn(soldier,40,56,0,0);w.unit(id)->underConstruction=true;
        int made=w.scenarioCreate(soldier,0,2,3,4,5);
        check(w.unit(made)->x.floorInt()==56,"unfinished unit lost primary occupancy");
        auto building=*soldier;building.canMove=false;building.maxVel=sim::Fixed();building.yardMap="y";
        sim::World yard;setup(yard);yard.spawn(&building,40,56,0,0);
        made=yard.scenarioCreate(soldier,0,2,3,4,5);
        check(yard.unit(made)->x.floorInt()==40,"open yard cell should not block scripted creation");
        building.yardMap="O";sim::World inverseYard;setup(inverseYard);inverseYard.spawn(&building,40,56,0,0);
        made=inverseYard.scenarioCreate(soldier,0,2,3,4,5);
        check(inverseYard.unit(made)->x.floorInt()==40,"uppercase O blocks only the opened yard state");
        sim::World outside;setup(outside);made=outside.scenarioCreate(soldier,0,80,80,90,90);
        check(outside.unit(made)->x.floorInt()==1160&&outside.unit(made)->z.floorInt()==1160,
              "off-map creation fallback changed from native midpoint");
    }
    {
        sim::World w;setup(w);w.setUnitCap(1);
        int own=w.spawn(soldier,40,56,0,0),enemy=w.spawn(other,56,56,0,1);
        check(w.scenarioCreate(soldier,0,2,3,4,5)==0,"script creation exceeded player pool cap");
        check(!w.scenarioTransfer(enemy,0)&&w.unit(enemy)->player==1,"script transfer exceeded recipient pool cap");
        check(w.unit(own)->alive()&&w.player(0).unitCount==1,"refused script actions damaged existing owner state");
    }
    {
        // Authoritative unfinished sites keep HP in their construction record.
        // Heal does not advance work; damage backs work up to the new HP ratio.
        sim::World w;setup(w);int site=w.spawn(soldier,40,56,0,0);
        auto* u=w.unit(site);u->underConstruction=true;u->hp=sim::Fixed::fromInt(80);
        u->retailSite.emplace();u->retailSite->type.maxHp=200;
        u->retailSite->progress.hp=80;u->retailSite->progress.remaining=.75f;
        run(w,reg,10,{"soldier","23","source"});
        check(u->hp.floorInt()==103&&u->retailSite->progress.hp==103&&u->retailSite->progress.remaining==.75f,
              "heal changed construction progress or failed to update authoritative HP");
        run(w,reg,11,{"soldier","23","source"});
        check(u->hp.floorInt()==80&&u->retailSite->progress.hp==80&&u->retailSite->progress.remaining==.6f,
              "damage did not synchronize unfinished HP/work");
        run(w,reg,11,{"soldier","200","source"});
        check(!u->alive()&&!u->retailSite&&w.player(0).unitCount==0,"lethal damage left immortal unfinished site");
        int second=w.spawn(soldier,40,56,0,0);w.unit(second)->underConstruction=true;w.unit(second)->retailSite.emplace();
        int builder=w.spawn(other,80,80,0,0);w.unit(builder)->buildSiteId=second;
        w.unit(builder)->retailBuild.emplace();w.unit(builder)->retailBuild->target=second;
        run(w,reg,8,{"soldier","source"});
        check(!w.unit(second)->alive()&&!w.unit(second)->retailSite&&!w.unit(builder)->retailBuild&&w.unit(builder)->buildSiteId==0,
              "destroy left an unfinished site or its worker alive");
        w.setUnitCap(2);check(w.scenarioCreate(soldier,0,2,3,4,5)!=0,"destroy failed to free player cap immediately");
    }
    {
        sim::World w;setup(w);int site=w.spawn(soldier,40,56,0,1),worker=w.spawn(other,80,80,0,1);
        w.unit(site)->underConstruction=true;w.unit(site)->buildBegun=false;
        w.unit(worker)->buildSiteId=site;
        check(w.scenarioTransfer(site,0)&&w.unit(site)->alive()&&w.unit(site)->underConstruction&&
              w.unit(worker)->buildSiteId==0,"transfer destroyed unfinished ghost or kept old worker attached");
    }
    {
        // A scripted removal has native death type zero and must not detonate
        // EXPLODEAS at nearby enemies. Ordinary damage death still detonates.
        auto bomb=*soldier; bomb.id="bomb"; bomb.hasExplodeAs=true;
        bomb.explodeAs.damage=150; bomb.explodeAs.aoe=200;
        for (int mode : {8,11}) {
            sim::World w;setup(w);int id=w.spawn(&bomb,40,56,0,0),victim=w.spawn(soldier,56,56,0,1);
            if (mode==8) run(w,reg,8,{"Any unit","source"});
            else run(w,reg,11,{"Any unit","200","source"});
            w.tick(1.f/30);
            check(w.unit(id)->deathType==(mode==8?0:13),"scripted death type wrong");
            check(mode==8 ? w.unit(victim)->hp.floorInt()==200 : w.unit(victim)->hp.floorInt()<200,
                  "scripted destruction and ordinary damage death must use distinct explosion behavior");
        }
    }
    std::cout<<"PASS: native scenario action HP, wildcard, creation, movement and transfer regressions\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
