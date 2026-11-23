// Who a hit and a kill credit, as retail's 51a140, 52a330, 512860, 512ae0,
// 513130 and 51e13f decide (tools/re/check_kill_credit.py runs them natively).
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/mission.h"
#include "sim/sim.h"
#include <cstdio>
#include <iostream>
#include <stdexcept>
using namespace tak::sim;
namespace tak::sim { struct RetailReplayProbe {
    static void hit(World& w,const Weapon& weapon,float x,float z,int player,int from,Unit* victim) {
        w.applyHit(weapon,x,z,player,from,victim);
    }
}; }
static void check(bool ok,const char* what) {if(!ok)throw std::runtime_error(what);}
static UnitType fighter(const char* id="fighter") {
    UnitType t;t.id=id;t.maxHp=2000;t.healTime=0;t.sight=600;t.experiencePoints=100;
    t.weapon.damage=100;t.weapon.range=100;t.weapon.melee=true;
    t.weapon.reload=1;t.weapon.aimTol=65536;t.weapons={t.weapon};return t;
}
static void terrain(World& w,int players=3) {
    w.setPlayerCount(players);
    w.setTerrain(std::vector<uint8_t>(64*64,50),64,64,0);
}
// A dead unit with no Dying retires on its death tick.
static void killNow(World& w,int id) {
    w.unit(id)->hp=Fixed();w.unit(id)->deathType=1;w.tick(1.f/30);
    check(!w.unit(id)->alive() && !w.unit(id)->deathPending,"synthetic unit did not die and retire");
}

int main(int argc,char** argv) try {
    const auto t=fighter();
    {
        // 51e07c..51e0cf: the once-a-second HP sample is HP x 100 over
        // maxdamage in whole numbers (53 of 100 is 53). 51e36c: a unit's
        // update ends with its death, after the sample, so one that dies on a
        // sample tick reads the sample of a second before as its severity's
        // "HP a second ago" (the one just moved into +0x111).
        UnitType hundred=fighter("hundred");hundred.maxHp=100;
        World w;terrain(w);
        const int v=w.spawn(&hundred,400,400,0,1);
        w.unit(v)->hp=Fixed::fromInt(53);
        int ticks=0;
        while (w.unit(v)->hpPctCur==0 && ticks<40) { w.tick(1.f/30);++ticks; }
        check(w.unit(v)->hpPctCur==53,"a 53 of 100 HP sample did not read 53");
        w.unit(v)->hp=Fixed::fromInt(80);
        for (int i=0;i<29;++i) w.tick(1.f/30);
        check(w.unit(v)->hpPctCur==53 && w.unit(v)->alive(),"a sample came early");
        w.unit(v)->hp=Fixed::fromInt(-1);w.unit(v)->overkill=Fixed::fromInt(1);w.unit(v)->deathType=1;
        w.tick(1.f/30);
        check(!w.unit(v)->alive(),"the unit did not die");
        // (1% overkill + the 53 sampled a second before) / 2.
        check(w.unit(v)->severity==27,"a death on a sample tick read the sample of two seconds before");
    }
    {
        // 51a16e: a hit on an activated unit goes through its type's
        // damagemodifier (16.16); an inactive one takes it whole.
        UnitType shell=fighter("shell");shell.damageModifier=0x8000;
        auto drop=[&](bool active) {
            World w;terrain(w);
            const int s=w.spawn(&t,400,400,0,0),v=w.spawn(&shell,440,400,0,1);
            w.unit(v)->active=active;
            const int before=w.unit(v)->hp.floorInt();
            RetailReplayProbe::hit(w,t.weapon,440,400,0,s,w.unit(v));
            return before-w.unit(v)->hp.floorInt();
        };
        const int whole=drop(false),scaled=drop(true);
        check(whole>=85 && whole<=115,"an inactive unit's hit was scaled");
        check(scaled==whole/2,"an activated unit's hit ignored its damagemodifier");
    }
    {
        // 512c6b: a credited kill pays the killer's player the victim type's
        // mogriumbounty into its pool, uncapped; an uncredited one nothing.
        UnitType prize=fighter("prize");prize.bounty=25.5f;
        for (const bool credited:{true,false}) {
            World w;terrain(w);
            const int k=w.spawn(&t,400,400,0,0),v=w.spawn(&prize,1200,1200,0,1);
            w.tick(1.f/30);
            const double before=w.player(0).mana;
            w.unit(v)->lastHitBy=k;w.unit(v)->lastHitPlayer=credited ? 0 : 1;
            w.unit(v)->hp=Fixed();w.unit(v)->deathType=1;
            w.tick(1.f/30);
            check(!w.unit(v)->alive() && !w.unit(v)->deathPending,"the prize did not retire");
            const double gained=w.player(0).mana-before;
            check(credited ? gained>=25.5 && gained<25.6 : gained<0.1,
                  credited ? "a credited kill did not pay its bounty" : "an uncredited kill paid a bounty");
        }
    }
    {
        // 51a35a: a hit names its hitter only while the hitter's object lives
        // and is the one that fired; otherwise +0xf8/+0xfc keep the old one.
        World w;terrain(w);
        const int s=w.spawn(&t,400,400,0,0),old=w.spawn(&t,400,800,0,2),v=w.spawn(&t,440,400,0,1);
        auto reset=[&]{w.unit(v)->lastHitBy=old;w.unit(v)->lastHitPlayer=2;};
        reset();
        RetailReplayProbe::hit(w,t.weapon,440,400,0,s,w.unit(v));
        check(w.unit(v)->lastHitBy==s && w.unit(v)->lastHitPlayer==0,"a live hitter was not named");
        reset();
        check(w.scenarioTransfer(s,1),"transfer refused");
        RetailReplayProbe::hit(w,t.weapon,440,400,0,s,w.unit(v));      // a shot fired before the change of sides
        check(w.unit(v)->lastHitBy==old && w.unit(v)->lastHitPlayer==2,"a converted shooter's shot took the credit");
        killNow(w,s);
        RetailReplayProbe::hit(w,t.weapon,440,400,1,s,w.unit(v));
        check(w.unit(v)->lastHitBy==old && w.unit(v)->lastHitPlayer==2,"a retired shooter's shot took the credit");
        RetailReplayProbe::hit(w,t.weapon,440,400,1,0,w.unit(v));
        check(w.unit(v)->lastHitBy==old && w.unit(v)->lastHitPlayer==2,"a hit with no shooter took the credit");
    }
    {
        // 52a35c: the shooter's attack multiplier only while it lives and is
        // not dying. A level 10 veteran doubles; its shot after death does not.
        auto hit=[&](bool dead) {
            World w;terrain(w);
            const int s=w.spawn(&t,400,400,0,0),v=w.spawn(&t,440,400,0,1);
            w.unit(s)->xp=10*t.experiencePoints;w.unit(s)->veteran=w.unit(s)->veteranLevel();
            check(w.unit(s)->vetMul()==2.0f,"synthetic veteran is not level 10");
            if (dead) killNow(w,s);
            const int before=w.unit(v)->hp.floorInt();
            RetailReplayProbe::hit(w,t.weapon,440,400,0,s,w.unit(v));
            return before-w.unit(v)->hp.floorInt();
        };
        const int live=hit(false),dead=hit(true);
        check(live>=170 && live<=230,"a live veteran's hit was not doubled");
        check(dead>=85 && dead<=115,"a dead veteran's shot kept its multiplier");
    }
    {
        // 512b71/512bd5: the F4 kill count and the score sit in the credit
        // kinds' branch and go to the player recorded at the hit (+0xfc) when
        // it is not the victim's owner -- no alliance test, and the killer's
        // later fate or owner does not matter.
        World w;terrain(w);w.setTeam(0,1);w.setTeam(1,1);
        const int k=w.spawn(&t,400,400,0,0);
        auto kill=[&](int victimPlayer,uint8_t kind) {
            const int v=w.spawn(&t,1200,1200,0,victimPlayer);
            w.unit(v)->lastHitBy=k;w.unit(v)->lastHitPlayer=0;
            w.scenarioDestroy(v,kind);
        };
        kill(2,5);kill(2,4);kill(2,10);kill(2,13);kill(2,0);
        check(w.player(0).kills==0 && w.player(0).score==0,"a death kind outside the credit branch counted a kill");
        kill(2,1);kill(2,14);
        check(w.player(0).kills==2 && w.player(0).score==200,"credit kinds did not count kills and score");
        kill(1,3);
        check(w.player(0).kills==3,"an ally's unit killed was not counted (retail tests owners, not alliance)");
        kill(0,1);
        check(w.player(0).kills==3,"the owner's own unit counted as a kill");
        check(w.scenarioTransfer(k,2),"transfer refused");
        kill(2,1);
        check(w.player(0).kills==4 && w.player(2).kills==0,"the kill followed the killer's new owner");
    }
    {
        // 4cd590: a new object's multipliers are 1.0 then the scenario type's;
        // a change of owner (514da0) makes one, so per-placement ones go.
        World w;terrain(w);
        const int a=w.spawn(&t,400,400,0,0),b=w.spawn(&t,600,400,0,0);
        w.unit(a)->scenarioArmor=0.5f;w.unit(a)->scenarioWeapon=3.0f;
        check(w.scenarioTransfer(a,1) && w.unit(a)->scenarioArmor==1.0f && w.unit(a)->scenarioWeapon==1.0f,
              "a transferred unit kept its placement's percentages");
        w.setScenarioTypeStats(t.id,200,150,0);
        w.unit(b)->scenarioArmor=0.5f;
        check(w.scenarioTransfer(b,1) && w.unit(b)->scenarioArmor==2.0f && w.unit(b)->scenarioWeapon==1.5f,
              "a transferred unit did not take the type's scenario percentages");
    }
    {
        // 52ac00: the death weapon has no shooter, fires at retirement only for
        // a finished unit, and is not scaled by the dying unit's veterancy.
        auto bomb=fighter("bomb");bomb.weapons.clear();bomb.hasExplodeAs=true;
        bomb.explodeAs.damage=100;bomb.explodeAs.aoe=200;bomb.explodeAs.edge=1;
        for (bool unfinished:{false,true}) {
            World w;terrain(w);
            const int b=w.spawn(&bomb,400,400,0,0),old=w.spawn(&t,400,900,0,2),v=w.spawn(&t,420,400,0,1);
            w.unit(b)->xp=10*bomb.experiencePoints;w.unit(b)->veteran=w.unit(b)->veteranLevel();
            w.unit(b)->underConstruction=unfinished;
            w.unit(v)->lastHitBy=old;w.unit(v)->lastHitPlayer=2;
            w.unit(b)->hp=Fixed();w.unit(b)->deathType=1;
            w.tick(1.f/30);
            const int lost=t.maxHp-w.unit(v)->hp.floorInt();
            if (unfinished) { check(lost==0,"an unfinished unit fired its death weapon"); continue; }
            check(lost>=85 && lost<=115,"the death weapon was missing or scaled by the dying unit's veterancy");
            check(w.unit(v)->lastHitBy==old && w.unit(v)->lastHitPlayer==2,"the death weapon named a hitter");
        }
        // 529c10 (529c4c..529c84): on a nosealeveltrigger map an impact that
        // struck no unit, in a cell whose highest corner is below sea level,
        // does nothing; a death blast is such an impact. Land, an ordinary
        // map and a struck unit keep their damage.
        auto blast=[&](bool trigger,bool water,bool direct) {
            World w;w.setPlayerCount(3);
            std::vector<uint8_t> heights(64*64,50);
            if (water) for (int z=20;z<30;++z) for (int x=20;x<30;++x) heights[size_t(z)*64+x]=10;
            w.setTerrain(heights,64,64,30);
            w.setNoSeaLevelTrigger(trigger);
            const int v=w.spawn(&t,420,400,0,1);
            if (direct) {
                RetailReplayProbe::hit(w,bomb.explodeAs,420,400,0,0,w.unit(v));
            } else {
                const int b=w.spawn(&bomb,400,400,0,0);
                w.unit(b)->hp=Fixed();w.unit(b)->deathType=1;
                w.tick(1.f/30);
            }
            return t.maxHp-w.unit(v)->hp.floorInt();
        };
        check(blast(true,true,false)==0,"a death blast in water hurt a unit on a nosealeveltrigger map");
        check(blast(false,true,false)>0,"a death blast in water did nothing on an ordinary map");
        check(blast(true,false,false)>0,"a death blast on land did nothing on a nosealeveltrigger map");
        check(blast(true,true,true)>0,"a shot that struck a unit in water did nothing on a nosealeveltrigger map");
    }
    {
        // 513130: Kill ignores its arguments and kills every unit, kind 10,
        // crediting nobody; their shots in flight go.
        World w;terrain(w);
        const int a=w.spawn(&t,400,400,0,0),b=w.spawn(&t,600,400,0,1),c=w.spawn(&t,800,400,0,2);
        w.unit(b)->lastHitBy=a;w.unit(b)->lastHitPlayer=0;
        TypeRegistry registry;
        MissionScript mission(std::vector<uint8_t>{},tak::tdf::Node{},registry,0,"kill fixture");
        mission.command(w,"Kill",{a});   // names one unit; retail ignores it
        w.tick(1.f/30);
        for (int id:{a,b,c}) check(!w.unit(id)->alive() && w.unit(id)->deathType==10,"Kill left a unit alive");
        check(w.player(0).kills==0 && w.player(0).score==0,"Kill credited a kill");
    }
    {
        // 51e13f: deadly water, once a second, whole HP, at or below sea level
        // whatever the unit is, except canhover.
        auto ship=fighter("ship");ship.maxWaterDepth=1000;
        auto hover=fighter("hover");hover.canHover=true;
        World w;w.setPlayerCount(2);w.setTerrain(std::vector<uint8_t>(64*64,10),64,64,50);
        w.setWaterDamage(100);
        const int s=w.spawn(&ship,400,400,0,0),h=w.spawn(&hover,600,400,0,0);
        int hits=0,last=w.unit(s)->hp.floorInt();
        for (int i=0;i<90;++i) {
            w.tick(1.f/30);
            const int hp=w.unit(s)->hp.floorInt();
            if (hp!=last) { check(last-hp==100,"deadly water did not take whole waterdamage"); ++hits; }
            last=hp;
        }
        check(hits==3,"deadly water did not hurt once a second");
        check(w.unit(h)->hp.floorInt()==hover.maxHp,"deadly water hurt a canhover unit");
        // 51e36c: the update whose water hit is lethal ends with 512610, so
        // the unit dies (here, with no Dying, retires: a loss) on that tick.
        World d;d.setPlayerCount(2);d.setTerrain(std::vector<uint8_t>(64*64,10),64,64,50);
        d.setWaterDamage(100);
        const int v=d.spawn(&ship,400,400,0,0);
        d.unit(v)->hp=Fixed::fromInt(100);
        int tick=0;
        while (d.unit(v)->hp>Fixed() && tick<60) { d.tick(1.f/30);++tick; }
        check(d.unit(v)->hp<=Fixed() && d.unit(v)->deathType==13,"deadly water did not kill");
        check(d.player(0).losses==1 && !d.unit(v)->deathPending,
              "a deadly-water death waited for the next tick's sweep");
    }
    {
        // Self-destruct (4017e0 in the background list, check_kill_credit.py
        // self-destruct): the countdown's runs every 30 ticks from the first
        // update, rand(15) on the run that finds it spent, the hit that many
        // ticks later: 51a140(u, u, 30000, 5, 0), the unit its own last
        // hitter, and the death in the same update (51e36c).
        int waited[15]={};
        for (uint32_t seed=1;seed<=40;++seed) {
            World w;terrain(w,2);w.setGameSeed(seed);
            const int u=w.spawn(&t,400,400,0,0);
            int drawTick=-1,draw=-1,update=0;
            w.setRngObserver([&](const World::RngObservation& o) {
                if (o.bound==15) { check(drawTick<0,"self-destruct drew rand(15) twice");drawTick=update;draw=int(o.result); }
            });
            w.destroy(u);
            for (;update<120 && w.unit(u)->alive();++update) {
                check(w.unit(u)->hp>Fixed() || !w.unit(u)->alive(),"self-destruct left a unit at zero hit points");
                w.tick(1.f/30);
            }
            const Unit& d=*w.unit(u);
            check(!d.alive() && w.player(0).losses==1 && !d.deathPending,"the self-destruct did not die in its update");
            check(drawTick==60,"the countdown's last run did not draw rand(15) two seconds in");
            check(update-1==60+draw,"the self-destruct did not hit rand(15) ticks after its countdown");
            check(d.lastHitBy==u && d.lastHitPlayer==0 && d.deathType==Unit::kDeathSelfDestruct,
                  "the self-destructing unit is not its own last hitter");
            check(w.player(0).kills==0,"a self-destruct credited a kill");
            ++waited[draw];
        }
        int distinct=0;
        for (int n:waited) distinct+=n>0;
        check(distinct>=5,"the self-destruct wait did not vary with the draw");
        // A countdown of 0 hits on its first run, with no draw.
        auto quick=fighter("quick");quick.selfDestructCountdown=0;
        World w;terrain(w,2);
        const int q=w.spawn(&quick,400,400,0,0);
        bool drew=false;
        w.setRngObserver([&](const World::RngObservation& o) { drew|=o.bound==15; });
        w.destroy(q);w.tick(1.f/30);
        check(!w.unit(q)->alive() && !drew,"a countdown of 0 did not hit on its first run");
        // SetMission "d" (SelfDestructFG, +0x4e 1): the same hit when the
        // mission comes up, and the death in that update.
        World s;terrain(s,2);
        const int f=s.spawn(&t,400,400,0,0);
        s.orderSelfDestruct(f,false);s.tick(1.f/30);
        check(!s.unit(f)->alive() && s.player(0).losses==1 && !s.unit(f)->deathPending,
              "a scripted self-destruct waited for the next tick's sweep");
        check(s.unit(f)->lastHitBy==f && s.unit(f)->lastHitPlayer==0,"a scripted self-destruct named no hitter");
    }
    {
        // 512610's commander rule (check_kill_credit.py commander): with the
        // Monarch not expendable, its death, or the death of a unit carrying
        // it, ends with 5131b0 for that unit's player: every unit of that
        // player dies at once, kind 10. Another player's units are untouched.
        auto monarch=fighter("monarch");monarch.commander=true;
        auto boat=fighter("boat");boat.canTransport=true;boat.transportCap=4;boat.transportSizeCap=16;
        for (const int carried:{0,1}) for (const int expendable:{0,1}) {
            World w;terrain(w);w.setMonarchExpendable(expendable!=0);
            const int m=w.spawn(&monarch,400,400,0,0),a=w.spawn(&t,500,400,0,0),b=w.spawn(&t,600,400,0,0);
            const int e=w.spawn(&t,900,900,0,1);
            const int ship=carried ? w.spawn(&boat,420,400,0,0) : 0;
            if (carried) check(w.embarkInto(m,ship),"the monarch did not board");
            const int victim=carried ? ship : m;
            // b was last hit by the other player: 512610 keeps that (512860).
            w.unit(b)->lastHitBy=e;w.unit(b)->lastHitPlayer=1;
            w.unit(victim)->hp=Fixed();w.unit(victim)->deathType=1;
            w.tick(1.f/30);
            const bool swept=!w.unit(a)->alive() && !w.unit(b)->alive();
            if (expendable) check(!swept,"an expendable monarch's death took its player's units");
            else {
                check(swept && w.unit(a)->deathType==10 && !w.unit(a)->deathPending,
                      carried ? "a carrier's death with the monarch aboard did not end its player's units"
                              : "the monarch's death did not end its player's units at once");
                check(w.player(0).kills==0 && w.player(1).kills==0,"the commander rule credited a kill");
                check(w.unit(b)->lastHitBy==e && w.unit(b)->lastHitPlayer==1,
                      "the commander rule cleared a unit's last hitter");
                // 512ae0 counts no loss for kind 10 (check_kill_credit.py
                // losses): only the commander killed by kind 1, or its
                // carrier and, as its passenger, the commander (kind 8).
                check(w.player(0).losses==(carried ? 2 : 1),"the commander rule's kind-10 deaths counted as losses");
            }
            check(w.unit(e)->alive(),"the commander rule took another player's unit");
        }
    }
    {
        // 512860 (check_kill_credit.py cargo): a carrier's death takes its
        // passengers, whoever owns them, at once with death kind 8 and the
        // carrier's hitter, who is credited for each at retirement.
        auto boat=fighter("boat");boat.canTransport=true;boat.transportCap=4;boat.transportSizeCap=16;
        for (const bool shooterLives:{true,false}) {
            World w;terrain(w);
            const int ship=w.spawn(&boat,400,400,0,0),a=w.spawn(&t,410,400,0,0),b=w.spawn(&t,420,400,0,2);
            const int s=w.spawn(&t,800,800,0,1);
            check(w.embarkInto(a,ship) && w.embarkInto(b,ship),"the passengers did not board");
            if (!shooterLives) killNow(w,s);
            w.unit(ship)->hp=Fixed();w.unit(ship)->deathType=1;
            w.unit(ship)->lastHitBy=s;w.unit(ship)->lastHitPlayer=1;
            w.tick(1.f/30);
            for (const int p:{a,b}) {
                const Unit& c=*w.unit(p);
                check(!c.alive() && !c.deathPending && c.deathType==8 && !c.embarked(),
                      "a passenger did not die at once with its carrier");
                check(c.lastHitPlayer==1 && c.lastHitBy==(shooterLives ? s : 0),
                      "a passenger did not take its carrier's hitter");
            }
            check(w.player(1).kills==3 && w.player(1).score==300,"the carrier's killer was not credited for its passengers");
            if (shooterLives) check(w.unit(s)->xp==300,"the carrier's killer did not earn its passengers' experience");
            // 512ccd: its own kill count (+0xfe) counts each under the same gate.
            if (shooterLives) check(w.unit(s)->kills==3,"the carrier's killer did not count its kills");
            if (shooterLives) {
                // 511c16: a new object, a change of owner's too, starts at 0.
                check(w.scenarioTransfer(s,2) && w.unit(s)->kills==0,"a unit that changed sides kept its kill count");
            }
            check(w.unit(ship)->cargo.empty(),"the dead carrier kept its passengers");
        }
        // A carrier changing sides: its old object's death packet takes the
        // passengers too (514da0 -> 512610 kind 6 -> 512860).
        World w;terrain(w);
        const int ship=w.spawn(&boat,400,400,0,0),a=w.spawn(&t,410,400,0,0),s=w.spawn(&t,800,800,0,1);
        check(w.embarkInto(a,ship),"the passenger did not board");
        w.unit(ship)->lastHitBy=s;w.unit(ship)->lastHitPlayer=1;
        check(w.scenarioTransfer(ship,1),"the transfer was refused");
        check(!w.unit(a)->alive() && w.unit(a)->deathType==8 && w.unit(a)->lastHitBy==s &&
              w.unit(ship)->cargo.empty() && w.unit(ship)->alive() && w.unit(ship)->player==1,
              "a carrier changing sides kept its passengers");
    }
    if (argc==2) {
        // The death weapon at retirement: the Bomb Sprinkler's Dying runs for
        // a while before 512ae0 fires its [EXPLODEAS].
        const auto vfs=tak::hpi::mountRetailRoot(argv[1]);
        TypeRegistry registry;setupRegistry(registry,vfs,false);
        const UnitType* bomb=registry.find("crebomb");
        check(bomb && bomb->hasExplodeAs,"crebomb has no death weapon");
        World w;w.setPlayerCount(2);w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        const int b=w.spawn(bomb,512,512,std::nullopt,0),v=w.spawn(&t,530,512,0,1);
        w.tick(1.f/30);
        w.unit(b)->hp=Fixed();w.unit(b)->deathType=1;
        int at=-1;
        for (int i=0;i<400 && at<0;++i) {
            w.tick(1.f/30);
            if (w.unit(v)->hp.floorInt()<t.maxHp) at=i;
        }
        const int handoff=w.unit(b)->corpseAnimationTicks();
        std::printf("crebomb: handoff %d, blast on tick %d after death\n",handoff,at);
        check(handoff>1 && at==handoff,"the death weapon did not wait for retirement");
        // 512ae0 fires the death weapon (512d97) before it places the corpse
        // (512ee0, check_kill_credit.py): the blast never reaches its own
        // corpse. Shipped corpses mostly take no weapon damage (no damage=),
        // so shipped walkers whose Killed leaves one get a destructible
        // corpse and a death weapon here; killed alone on open ground, the
        // corpse has taken no damage.
        World definitions;registerMapFeatures(definitions,tak::tnt::Map{},vfs,&registry);
        int reached=0;
        for (const auto& [id,shipped]:registry.types()) {
            if (reached>=4) break;
            const int ct=definitions.corpseTypeOf(&shipped);
            if (shipped.canFly || shipped.isStructure() || ct<0 || !shipped.script()) continue;
            UnitType type=shipped;type.hasExplodeAs=true;type.explodeAs=Weapon{};
            type.explodeAs.damage=100;type.explodeAs.aoe=96;type.explodeAs.edge=1;
            auto feats=definitions.featureTypes();
            feats[size_t(ct)].hp=1000;feats[size_t(ct)].indestructible=false;
            World d;d.setPlayerCount(2);d.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            d.setFeatureTypes(feats);d.mapCorpse(&type,ct);
            const int u=d.spawn(&type,512,512,std::nullopt,0);
            d.tick(1.f/30);
            d.unit(u)->hp=Fixed();d.unit(u)->deathType=1;
            for (int i=0;i<600 && (d.unit(u)->deathPending || d.unit(u)->alive());++i) d.tick(1.f/30);
            const int32_t damage=d.corpseDamage(u);
            if (damage<0 || !d.unit(u)->severity) continue;   // no corpse, or no blast
            ++reached;
            if (damage!=0) throw std::runtime_error(id+": its death blast reached its own corpse");
        }
        std::printf("own corpse spared: %d shipped walkers\n",reached);
        check(reached>0,"no shipped walker leaves a corpse to check");

    }
    std::cout<<"PASS: kill credit, death weapon, mission Kill and deadly water\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
