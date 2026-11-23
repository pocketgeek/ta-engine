// Authored stat effects and the separate nonparticipant scenery owner.
#include "sim/sim.h"
#include <iostream>
#include <stdexcept>
using namespace tak::sim;
static void check(bool ok,const char* what) {if(!ok)throw std::runtime_error(what);}
static UnitType fighter() {
    UnitType t;t.id="fighter";t.maxHp=2000;t.healTime=0;t.sight=600;
    t.weapon.damage=40;t.weapon.range=100;t.weapon.melee=true;
    t.weapon.reload=1;t.weapon.aimTol=65536;t.weapons={t.weapon};return t;
}
static void terrain(World& w) {
    w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(64*64,50),64,64,0);
}
static int oneHit(float attack,float armor) {
    auto t=fighter(),victim=t;victim.id="victim";victim.weapon.damage=0;victim.weapons.clear();
    World w;terrain(w);int a=w.spawn(&t,400,400,0,0),b=w.spawn(&victim,440,400,0,1);
    w.unit(a)->scenarioWeapon=attack;w.unit(b)->scenarioArmor=armor;
    w.attack(a,b,false);
    bool fired=false;
    for(int i=0;i<120 && !fired;++i) {w.tick(1.f/30);fired=w.unit(a)->justFired;}
    check(fired,"synthetic attacker never fired");
    check(w.unit(a)->scenarioWeapon==attack && w.unit(b)->scenarioArmor==armor,"aura relaxation erased authored factors");
    return victim.maxHp-w.unit(b)->hp.floorInt();
}
int main() try {
    const int base=oneHit(1,1),boost=oneHit(1.5f,1),armored=oneHit(1,2);
    check(base>0 && boost>base && armored<base && armored>0,"authored attack/armor factors do not affect damage");
    check(oneHit(0,1)==0,"zero authored weapon percentage still dealt damage");
    check(oneHit(1.5f,1.5f)==base,"equal attack and armor factors did not cancel");
    {
        auto t=fighter();World w;terrain(w);w.enableScenarioNeutralPlayer();
        int a=w.spawn(&t,400,400,0,0),n=w.spawn(&t,440,400,0,8);
        for(int i=0;i<120;++i)w.tick(1.f/30);
        check(w.unit(a)->hp.floorInt()==t.maxHp && w.unit(n)->hp.floorInt()==t.maxHp,
              "neutral units or nearby players auto-acquired each other");
        check(w.numPlayers()==2 && w.player(0).unitCount==1 && w.player(8).unitCount==1,
              "neutral units contaminated a participant's count");
        w.attack(a,n,false);for(int i=0;i<120 && w.unit(n)->hp.floorInt()==t.maxHp;++i)w.tick(1.f/30);
        check(w.unit(n)->hp.floorInt()<t.maxHp,"explicit attack on a neutral unit failed");
    }
    {
        auto t=fighter(),victim=t;t.weapon.mindControl=true;t.weapons={t.weapon};
        victim.id="victim";victim.weapon.damage=0;victim.weapons.clear();
        victim.buildMovementCode=1;   // a mobile body: the charm (52da10) refuses one without a mover
        World w;terrain(w);w.enableScenarioNeutralPlayer();
        int a=w.spawn(&t,400,400,0,0),n=w.spawn(&victim,440,400,0,8);
        w.attack(a,n,false);
        for(int i=0;i<600 && w.unit(n)->player==8;++i)w.tick(1.f/30);
        check(w.unit(n)->player==0 && w.player(8).unitCount==0 && w.player(0).unitCount==2,
              "neutral capture failed or left stale ownership counts");
    }
    {
        auto t=fighter();World a,b;terrain(a);terrain(b);
        a.setScenarioTypeStats(t.id,200,150,7);b.setScenarioTypeStats(t.id,200,150,7);
        const int id=a.spawn(&t,400,400,0,0);b.spawn(&t,400,400,0,0);
        // setLevel 519370: (level + 0.5) x the type's experiencepoints, truncated.
        check(a.unit(id)->veteran==7 && a.unit(id)->xp==int32_t(7.5*t.experiencePoints),
              "authored veteran did not initialize experience");
        check(a.stateHash()==b.stateHash(),"same stat configuration diverged");
        b.unit(id)->scenarioWeapon=.5f;check(a.stateHash()!=b.stateHash(),"per-unit stat factor absent from lockstep hash");
    }
    {
        auto t=fighter(),builder=t; t.buildTime=1;t.buildCost=0;
        builder.id="builder";builder.isBuilder=true;builder.canMove=true;builder.maxVel=Fixed::fromInt(1);builder.buildDist=200;
        builder.weapon.damage=0;builder.weapons.clear();
        World w;terrain(w);w.setScenarioTypeStats(t.id,200,150,7);
        int a=w.spawn(&builder,400,400,0,0),b=w.spawn(&t,440,400,0,0);
        w.unit(b)->hp=Fixed::fromInt(100);w.repair(a,b,false);
        for(int i=0;i<300 && w.unit(b)->hp.floorInt()<t.maxHp;++i)w.tick(1.f/30);
        check(w.unit(b)->hp.floorInt()==t.maxHp && w.unit(b)->maximumHp()==t.maxHp,
              "repair did not use original type maximum HP");
        check(w.unit(b)->scenarioArmor==2.f && w.unit(b)->scenarioWeapon==1.5f && w.unit(b)->veteran==7,
              "repair or aura tick erased authored stats");
    }
    {
        auto t=fighter();t.noVeteran=true;World w;terrain(w);
        w.setScenarioTypeStats(t.id,100,100,7);
        const int id=w.spawn(&t,400,400,0,0);
        check(w.unit(id)->veteran==0 && w.unit(id)->xp==0,"noveteran unit accepted scenario veteran default");
    }
    {
        // 512ce0: the killer gains the VICTIM type's experiencepoints, and its
        // level is that over its OWN type's (519310), capped at 10. Nothing of
        // the killer is tested: a building gains it too.
        auto t=fighter(),victim=fighter(),tower=fighter();t.experiencePoints=10;t.canMove=true;
        victim.id="victim";victim.weapon.damage=0;victim.weapons.clear();victim.maxHp=40;victim.experiencePoints=25;
        tower.id="tower";tower.experiencePoints=10;tower.canMove=false;tower.maxVel=Fixed();
        tower.weapon.melee=false;tower.weapon.range=200;tower.weapons={tower.weapon};
        for (const UnitType* killerType:{&t,&tower}) {
            World w;terrain(w);
            const int a=w.spawn(killerType,400,400,0,0),b=w.spawn(&victim,440,400,0,1);
            w.attack(a,b,false);
            for(int i=0;i<300 && w.unit(b)->alive();++i)w.tick(1.f/30);
            check(!w.unit(b)->alive(),"synthetic victim survived");
            check(w.unit(a)->xp==25 && w.unit(a)->veteran==2,"kill did not credit the victim's experiencepoints");
        }
        World w;terrain(w);
        const int a=w.spawn(&t,400,400,0,0),b=w.spawn(&victim,440,400,0,1);
        w.unit(a)->xp=95;w.unit(a)->veteran=w.unit(a)->veteranLevel();
        w.attack(a,b,false);
        for(int i=0;i<300 && w.unit(b)->alive();++i)w.tick(1.f/30);
        check(w.unit(a)->xp==120 && w.unit(a)->veteran==10,"level is not capped at 10");
        // An ally's kill counts (only the owner's own does not); a unit given
        // away is a new object without experience (514da0 via 511990).
        World allies;terrain(allies);allies.setTeam(0,1);allies.setTeam(1,1);
        const int c=allies.spawn(&t,400,400,0,0),d=allies.spawn(&victim,440,400,0,1);
        allies.spawn(&victim,900,900,0,1);   // player 1 stays in the game to be given a unit
        allies.attack(c,d,false);
        for(int i=0;i<300 && allies.unit(d)->alive();++i)allies.tick(1.f/30);
        check(!allies.unit(d)->alive() && allies.unit(c)->xp==25,"an ally's kill did not credit experience");
        const bool gave=allies.giveUnit(c,0,1);
        check(gave && allies.unit(c)->xp==0 && allies.unit(c)->veteran==0,
              "a given unit kept its experience");
    }
    {
        // The credit gate of 512ae0, through a scripted death of a chosen kind:
        // credit for kinds 1,2,3,7,8,14,15 only, a finished victim, and a victim
        // owner other than the last hitter's player.
        auto killerType=fighter(),cheap=fighter(),proud=fighter();
        killerType.experiencePoints=100;cheap.id="cheap";cheap.experiencePoints=25;
        proud.id="proud";proud.experiencePoints=100;proud.noVeteran=true;
        World w;w.setPlayerCount(3);w.setTerrain(std::vector<uint8_t>(64*64,50),64,64,0);
        const int k=w.spawn(&killerType,400,400,0,0),nv=w.spawn(&proud,400,600,0,0);
        auto kill=[&](int killer,int victimPlayer,uint8_t kind,bool unfinished=false) {
            const int v=w.spawn(&cheap,1200,1200,0,victimPlayer);
            w.unit(v)->underConstruction=unfinished;
            w.unit(v)->lastHitBy=killer;w.unit(v)->lastHitPlayer=w.unit(killer)->player;
            w.scenarioDestroy(v,kind);
        };
        // A valuable unit killing cheap ones: 25 a kill, a level every 100.
        for (int i=0;i<3;++i) kill(k,1,1);
        check(w.unit(k)->xp==75 && w.unit(k)->veteran==0,"cheap kills did not add a fraction of a level");
        kill(k,1,3);
        check(w.unit(k)->xp==100 && w.unit(k)->veteran==1,"four cheap kills did not reach level 1");
        kill(k,1,5);kill(k,1,4);kill(k,1,0);kill(k,1,13);
        check(w.unit(k)->xp==100,"self-destruct, paralyzer, scripted or site death credited experience");
        kill(k,1,14);kill(k,2,15);
        check(w.unit(k)->xp==150,"stone or freeze kill did not credit experience");
        kill(k,1,1,true);
        check(w.unit(k)->xp==150,"an unfinished victim credited experience");
        kill(k,0,1);
        check(w.unit(k)->xp==150,"killing the owner's own unit credited experience");
        // noveteran collects experience but stays at level 0 (getLevel 519310).
        for (int i=0;i<8;++i) kill(nv,1,1);
        check(w.unit(nv)->xp==200 && w.unit(nv)->veteran==0 && w.unit(nv)->vetMul()==1.0f,
              "a noveteran killer was not held at level 0");
        // `v F`: level 10 while set, kills still add, cleared the level is xp's.
        w.setVeterancyOverride(k,1.5f);
        check(w.unit(k)->veteran==10 && w.unit(k)->vetMul()==1.5f,"v F did not hold level 10");
        for (int i=0;i<2;++i) kill(k,1,1);
        check(w.unit(k)->xp==200 && w.unit(k)->veteran==10,"kills under v F did not add experience");
        w.setVeterancyOverride(k,0.0f);
        check(w.unit(k)->veteran==2,"cleared v F did not return the level from experience");
        // A killer that died first gains nothing: its object is gone.
        const int gone=w.spawn(&killerType,400,800,0,0);
        w.unit(gone)->hp=Fixed();w.tick(1.f/30);
        check(!w.unit(gone)->alive(),"synthetic killer did not die");
        kill(gone,1,1);
        check(w.unit(gone)->xp==0,"a dead killer gained experience");
        // A CRT transfer is a new object too (514da0 at 4cb0c4).
        check(w.scenarioTransfer(k,2) && w.unit(k)->xp==0 && w.unit(k)->veteran==0,
              "a transferred unit kept its experience");
    }
    {
        // Deadly water hurts with no attacker and damage kind 13 (51e13f), the
        // death kind 512610 then gets: the last hitter is kept but not credited.
        auto killerType=fighter(),victim=fighter();
        killerType.experiencePoints=10;killerType.canHover=true;   // only canhover is spared
        victim.id="victim";victim.weapon.damage=0;victim.weapons.clear();victim.maxHp=40;victim.experiencePoints=25;
        World w;w.setPlayerCount(2);w.setTerrain(std::vector<uint8_t>(64*64,10),64,64,50);
        w.setWaterDamage(1000);
        const int k=w.spawn(&killerType,400,400,0,0),v=w.spawn(&victim,440,400,0,1);
        w.unit(v)->lastHitBy=k;w.unit(v)->lastHitPlayer=0;
        for(int i=0;i<60 && w.unit(v)->alive();++i)w.tick(1.f/30);
        check(!w.unit(v)->alive() && w.unit(v)->deathType==13,"deadly water did not kill with kind 13");
        check(w.unit(k)->alive() && w.unit(k)->xp==0,"a drowning credited its last attacker");
    }
    std::cout<<"PASS: authored combat percentages, neutral targeting/capture and state hashing\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
