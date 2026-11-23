// A change of owner as retail's 514da0 makes it (tools/re/check_change_owner.py
// runs it natively): the old object retires with death kind 6 and the new
// owner gets a new object (512060 -> 511990) that keeps only HP, the
// construction left, the angles, the individual speed and the activation
// byte. World keeps the unit's id and resets the rest in place
// (World::changeOwner), the same for the charm, ShareUnits, the CRT transfer
// and the mission Capture. Also: a new unit's HP samples start at 0
// (511990), and deadly water hurts a carried unit at its carrier's
// attach point (51e0d5 reads the +0x6e that 4dad30 committed).
#include "cob/cob.h"
#include "hpi/hpi.h"
#include "crt/crt.h"
#include "sim/scenario.h"
#include "sim/sim.h"
#include <iostream>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>
using namespace tak::sim;
namespace tak::sim { struct RetailReplayProbe {
    static void charm(World& w,int id,int player) { w.captureUnit(*w.unit(id),player); }
    static World::UnitScript* script(World& w,int id) { return w.unitScript(id); }
}; }
static void check(bool ok,const char* what) {if(!ok)throw std::runtime_error(what);}
static UnitType caster() {
    UnitType t;t.id="caster";t.maxHp=500;t.healTime=0;t.sight=300;t.maxMana=100;t.manaRegen=0;
    t.maxVel=Fixed::fromInt(2);t.buildAngle=4096;t.canSetStance=true;t.canCloak=true;
    t.defaultStandingOrder=2;t.defaultMove=1;t.defaultFire=2;
    t.weapon.damage=10;t.weapon.range=100;t.weapon.melee=true;t.weapon.reload=1;t.weapon.aimTol=65536;
    t.weapons={t.weapon};return t;
}
static void terrain(World& w,int players=3,uint8_t height=50,int sea=0) {
    w.setPlayerCount(players);
    w.setTerrain(std::vector<uint8_t>(64*64,height),64,64,sea);
}

// tools/re/check_change_owner.py: one change of owner per row,
//   same dying capped limited activatewhenbuilt buildangle moving activation
// answered as: changed, the creation draws' bounds, then the new object's
// activation (1 active, 4 cloaked) and whether every field 511990 starts
// fresh is fresh while HP and the construction left were kept.
static int native() {
    auto file=std::make_shared<tak::cob::File>();
    file->code={0x10021001u,0u,0x10065000u};
    for (const char* name:{"Create","Activate","Deactivate","SetMaxReloadTime"}) file->scripts.push_back({name,0});
    const UnitType t0=caster();
    int same,dying,capped,limited,flag,spread,moving,act;
    while (std::cin>>same>>dying>>capped>>limited>>flag>>spread>>moving>>act) {
        UnitType type=caster();type.buildAngle=uint32_t(spread);type.maxVel=moving ? Fixed::fromInt(2) : Fixed();
        type.activateWhenBuilt=flag!=0;type.totalAllowed=limited ? 1 : 0;type.simulationScript=file;
        World w;terrain(w);
        const int id=w.spawn(&type,400,400,0,0);
        if (limited) w.spawn(&type,800,800,0,1);
        if (capped) { w.spawn(&t0,800,600,0,1); w.setUnitCap(1); }
        Unit& u=*w.unit(id);
        u.hp=Fixed::fromInt(77);u.mana=40;u.healCarry=5;u.hpPct1s=u.hpPctCur=50;u.cloakBlockedUntil=9;
        u.lastHitBy=3;u.lastHitPlayer=2;u.airTraffic=2;u.cloaked=(act&4)!=0;u.scenarioName="Ragnar";
        if (auto* script=RetailReplayProbe::script(w,id)) script->activated=(act&1)!=0;
        if (dying) u.hp=Fixed();
        std::vector<int32_t> draws;   // the CRT's as -1
        w.setRngObserver([&](const World::RngObservation& o){draws.push_back(o.bound);});
        w.setCrtRngObserver([&](const World::RngObservation&){draws.push_back(-1);});
        const bool changed=w.scenarioTransfer(id,same ? 0 : 1,nullptr);
        w.setRngObserver({});w.setCrtRngObserver({});
        const Unit& n=*w.unit(id);
        std::cout<<(changed ? 1 : 0);
        for (int32_t d:draws) std::cout<<' '<<d;
        if (changed) {
            const auto* script=RetailReplayProbe::script(w,id);
            const bool fresh=n.mana==0 && n.healCarry==0 && n.hpPct1s==0 && n.hpPctCur==0 && n.cloakBlockedUntil==0 &&
                n.lastHitBy==0 && n.lastHitPlayer==-1 && n.airTraffic==0 && n.hp==Fixed::fromInt(77) &&
                n.scenarioName.empty();
            std::cout<<" | "<<((script && script->activated) ? 1 : 0)+(n.cloaked ? 4 : 0)<<' '<<(fresh ? 1 : 0);
        }
        std::cout<<'\n';
    }
    return 0;
}

int main(int argc,char** argv) try {
    if (argc==2 && std::string(argv[1])=="--native") return native();
    const auto t=caster();
    {
        // 511990 (E): the HP samples +0x110/+0x111 start at 0. Ours started
        // them at 100, so a unit killed in its first second took a higher
        // severity than retail's.
        World w;terrain(w);
        const int id=w.spawn(&t,400,400,0,0);
        check(w.unit(id)->hpPct1s==0 && w.unit(id)->hpPctCur==0,"a new unit's HP samples are not 0");
    }
    {
        // 4c033e: activatewhenbuilt is UnitDef +0x260 & 0x40000, default 0,
        // and only it activates a new unit (512301 finished, 429990 at
        // completion; 511990 zeroes +0x114). The four gates, onoffable
        // without the key, start closed. Ours read a missing key as 1.
        auto files=std::make_shared<tak::hpi::Vfs::Files>();
        const std::string gate="[UNITINFO]{\nunitname=gate;\nobjectname=gate;\nmaxdamage=100;\nonoffable=1;\n}";
        const std::string fire="[UNITINFO]{\nunitname=fire;\nobjectname=fire;\nmaxdamage=100;\nonoffable=1;\n"
                               "activatewhenbuilt=1;\n}";
        (*files)["units/gate.fbi"]={gate.begin(),gate.end()};
        (*files)["units/fire.fbi"]={fire.begin(),fire.end()};
        tak::hpi::Vfs vfs;vfs.setMapFiles(files);
        TypeRegistry registry;registry.loadDir(vfs,"units");
        const UnitType* g=registry.find("gate");const UnitType* f=registry.find("fire");
        check(g && f,"synthetic on/off types missing");
        World w;terrain(w);
        check(!g->activateWhenBuilt && !w.unit(w.spawn(g,400,400,0,0))->active,
              "an onoffable type without activatewhenbuilt starts active");
        check(f->activateWhenBuilt && w.unit(w.spawn(f,600,400,0,0))->active,
              "activatewhenbuilt=1 does not start active");
    }
    for (int path=0;path<4;++path) {
        // The four ways a unit changes sides all make the same new object.
        World w;terrain(w);w.setTeam(0,1);w.setTeam(1,1);
        const int id=w.spawn(&t,400,400,0,0);
        w.spawn(&t,800,800,0,1);
        Unit& u=*w.unit(id);
        u.hp=Fixed::fromInt(321);u.mana=80;u.paralyzedFor=200;u.cloakOn=true;u.cloakBlockedUntil=999;
        u.standingOrder=0;u.moveState=0;u.fireState=0;u.stance=2;u.reloads[0]=17;u.healCarry=9;
        u.hpPct1s=40;u.hpPctCur=60;u.airTraffic=3;u.lastHitBy=77;u.lastHitPlayer=2;u.squad=4;
        u.xp=5000;u.veteran=u.veteranLevel();u.selfDestructT=40;
        u.homeX=Fixed::fromInt(10);u.homeZ=Fixed::fromInt(10);
        const Bam heading=u.heading;const Fixed speed=u.baseSpeed;
        std::vector<int32_t> draws;
        w.setRngObserver([&](const World::RngObservation& o){draws.push_back(o.bound);});
        bool changed=false;
        switch (path) {
            case 0: changed=w.scenarioTransfer(id,1);break;
            case 1: changed=w.giveUnit(id,0,1);break;
            case 2: RetailReplayProbe::charm(w,id,1);changed=w.unit(id)->player==1;break;
            default: changed=w.scenarioTransfer(id,1,nullptr);break;
        }
        w.setRngObserver({});
        const Unit& n=*w.unit(id);
        check(changed && n.player==1,"the change of owner was refused");
        // Kept (514da0 copies them): HP raw, the angles, +0x12b.
        check(n.hp==Fixed::fromInt(321) && n.heading==heading && n.baseSpeed==speed,
              "HP, heading or the individual speed did not survive the change");
        // 512060's creation draws, in order: rand(buildangle), rand(65536)
        // for +0x82, rand(201) for the speed (511baa, 511cae, 512439).
        check(draws==std::vector<int32_t>({4096,65536,201}),"the new object's creation draws were not made");
        // Fresh (511990, 52aae0, the missions).
        check(n.mana==0,"the new object kept the old one's mana");
        check(n.paralyzedFor==0,"the new object kept the old one's paralysis");
        check(!n.cloakOn && n.cloakBlockedUntil==0,"the new object kept the cloak order or its delay");
        check(n.standingOrder==t.defaultStandingOrder && n.moveState==t.defaultMove && n.fireState==t.defaultFire,
              "the new object kept the old one's standing orders");
        check(n.reloads[0]==0 && n.healCarry==0 && n.airTraffic==0 && n.hpPct1s==0 && n.hpPctCur==0,
              "the new object kept reload, self-repair carry, air traffic or HP samples");
        check(n.lastHitBy==0 && n.lastHitPlayer==-1 && n.squad==0 && n.xp==0 && n.selfDestructT<0,
              "the new object kept its hitter, squad, experience or self-destruct");
        check(n.homeX==n.x && n.homeZ==n.z,"the new object's post is not where it stands");
    }
    {
        // 5120f6: the new owner's totalallowed refuses the new object, and
        // then nothing changes.
        auto capped=t;capped.totalAllowed=1;
        World w;terrain(w);
        const int id=w.spawn(&capped,400,400,0,0);w.spawn(&capped,800,800,0,1);
        w.unit(id)->mana=50;
        check(!w.scenarioTransfer(id,1) && w.unit(id)->player==0 && w.unit(id)->mana==50,
              "a change of owner past totalallowed was made");
    }
    {
        // The old object retires with kind 6 under its old owner (512ae0 ->
        // 4cd600): the scenario counts it lost by that owner, and killed by
        // the player of its last hit (+0xfc) when that is another player.
        World w;terrain(w);
        tak::crt::Scenario scene;scene.players.resize(9);TypeRegistry registry;
        auto rules=std::make_unique<ScenarioScript>(scene,registry,-1,w.numPlayers(),64,64);
        ScenarioScript* scenario=rules.get();
        w.setScenario(std::move(rules));
        const int id=w.spawn(&t,400,400,0,0);w.spawn(&t,800,800,0,1);w.spawn(&t,800,400,0,2);
        w.unit(id)->lastHitBy=0;w.unit(id)->lastHitPlayer=2;
        check(w.scenarioTransfer(id,1),"transfer refused");
        w.tick(1.f/30);
        check(scenario->lostCount(0,"caster")==1 && scenario->killedCount(2,"caster")==1 &&
              scenario->lostCount(1,"caster")==0,"the old object's retirement was not tallied");
        check(w.unit(id)->alive() && w.unit(id)->player==1,"the new object did not stand");
    }
    {
        // 51e0d5 hurts whatever stands at or below sea level, a carried unit
        // too: its +0x6e is the attach point its mover committed (4dad30). A
        // ship's cargo at sea is hurt, a flyer's aloft is not. Ours skipped
        // every carried unit.
        auto ship=t;ship.id="ship";ship.maxWaterDepth=1000;ship.transportCap=4;
        auto wagon=t;wagon.id="wagon";wagon.transportCap=4;
        auto rider=t;rider.id="rider";rider.maxMana=0;
        // Cells x < 40 lie under the sea (10 against sea level 50), the rest
        // is a plateau at 100. Each rider starts over the other ground.
        World w;w.setPlayerCount(2);
        std::vector<uint8_t> ground(64*64,10);
        for (int z=0;z<64;++z) for (int x=40;x<64;++x) ground[size_t(z)*64+size_t(x)]=100;
        w.setTerrain(ground,64,64,50);
        w.setWaterDamage(100);
        const int s=w.spawn(&ship,400,400,0,0),g=w.spawn(&wagon,800,400,0,0);
        const int low=w.spawn(&rider,800,600,0,0),high=w.spawn(&rider,400,600,0,0);
        // These synthetic movers have no ground support, so their height is
        // whatever is set: the ship's under the sea, the wagon's on the
        // plateau, each rider's its own ground before it is carried.
        w.unit(s)->groundY=Fixed::fromInt(10);w.unit(g)->groundY=Fixed::fromInt(100);
        w.unit(low)->groundY=Fixed::fromInt(100);w.unit(high)->groundY=Fixed::fromInt(10);
        for (auto [c,host]:{std::pair{low,s},std::pair{high,g}}) {
            w.unit(c)->inTransport=host;w.unit(host)->cargo.push_back(c);
        }
        for (int i=0;i<61;++i) w.tick(1.f/30);
        check(w.unit(low)->embarked() && w.unit(low)->hp.floorInt()==rider.maxHp-200,
              "deadly water spared the cargo of a ship at sea level");
        check(w.unit(high)->embarked() && w.unit(high)->hp.floorInt()==rider.maxHp,
              "deadly water hurt the cargo of a wagon on high ground");
    }
    std::cout<<"PASS: change of owner, new objects' pools and samples, carried units in deadly water\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
