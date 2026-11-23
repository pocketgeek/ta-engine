// Capture attacks must reject targets their charm impact cannot take, and a
// conversion keeps the unit's damage. Retail converts only with the
// mind-control weapon (52d8d0/52da10): cancapture is read by nothing in the
// simulation, so there is no capture by contact.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cstdio>

namespace tak::sim { struct RetailReplayProbe {
    static void impact(World& w,const Weapon& weapon,int from,int target) {
        const auto* source=w.unit(from);auto* victim=w.unit(target);
        w.applyHit(weapon,victim->x.toFloat(),victim->z.toFloat(),source->player,from,victim);
    }
}; }

namespace {
int failures=0;
void check(bool ok,const char* message) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",message);
    if(!ok)++failures;
}
tak::sim::UnitType victimType() {
    // bmcode 1: retail's charm (52da10) refuses a body without a mover.
    // Upright, so it stands on the terrain: the scans take only what the
    // player sees, and a body left at y 0 under the height-100 ground falls
    // on the wrong sight cells.
    tak::sim::UnitType t;t.id="victim";t.maxHp=10000;t.canMove=true;t.buildMovementCode=1;t.upright=true;
    return t;
}
tak::sim::UnitType casterType() {
    auto t=victimType();t.id="caster";
    tak::sim::Weapon w;w.damage=1;w.range=400;w.reload=0.1f;
    w.mindControl=true;w.kind=tak::sim::Weapon::Kind::Remote;
    w.remote=tak::sim::Weapon::RemoteKind::MindCtl;w.unitsOnly=true;w.aoe=32;w.buildUpTicks=8;w.decayTicks=8;
    t.weapon=w;t.weapons={w};return t;
}
void setup(tak::sim::World& w) {
    w.setPlayerCount(3);w.setTeam(2,0);
    w.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
}
bool targets(const tak::sim::Unit& unit,int id) {
    return std::any_of(unit.orders.begin(),unit.orders.end(),
                       [&](const auto& order){return order.targetId==id;});
}

const tak::sim::Weapon* charmOf(const tak::sim::UnitType& caster) {
    const auto it=std::find_if(caster.weapons.begin(),caster.weapons.end(),
                               [](const auto& weapon){return weapon.mindControl;});
    return it==caster.weapons.end()?nullptr:&*it;
}

void rejectedTargets(const tak::sim::UnitType& caster) {
    // 6: still being built. Retail's charm roll refuses a target whose
    // construction remaining (+0x108) is above zero.
    for(int reason=0;reason<7;++reason) {
        auto victim=victimType();
        victim.commander=reason==0;victim.cantBeCaptured=reason==1;
        victim.buildTime=1e6f;   // an unworked site un-conjures this slowly
        tak::sim::World w;setup(w);
        const int a=w.spawn(&caster,1000,1000,0,0);
        const int b=w.spawn(&victim,1000,1100,0,reason==5?2:1);
        if(reason==2)w.unit(b)->inTransport=a;
        if(reason==3)w.unit(b)->stonedFor=1000;
        if(reason==4)w.setUnitCap(1);
        if(reason==6)w.unit(b)->underConstruction=true;
        w.unit(a)->fireState=2;w.unit(a)->moveState=2;w.unit(a)->mana=1e6;
        w.attack(a,b,false);
        check(!targets(*w.unit(a),b),"explicit capture rejects an ineligible target");
        bool fired=false,aimed=false;
        for(int tick=0;tick<120;++tick) {
            w.tick(1.f/30);
            fired|=w.unit(a)->justFired;
            aimed|=targets(*w.unit(a),b) || w.unit(a)->scriptAimTarget==b;
        }
        check(!fired && !aimed,
              "automatic capture does not acquire or fire against immunity/capacity");
        check(w.unit(b)->player==(reason==5?2:1),"rejected capture preserves ownership");
        if(const auto* charm=charmOf(caster)) {
            for(int i=0;i<20;++i)tak::sim::RetailReplayProbe::impact(w,*charm,a,b);
            // 5: an ally's unit. The caster does not choose it, but the charm
            // spares only the caster's own player (52da10 compares +0xfd), so a
            // shot that strikes it takes it.
            if(reason==5)check(w.unit(b)->player==0,"a charm impact takes an ally's unit it strikes");
            else check(w.unit(b)->player==1,"a charm impact cannot take an ineligible target");
        }
    }
}

// A converted unit keeps its damage (514f8a copies HP raw) and, as retail's
// new object, starts without experience (511990 zeroes +0xf4).
void conversionKeepsDamage(const tak::sim::UnitType& caster,const tak::sim::UnitType& victim,bool data) {
    tak::sim::World w;setup(w);
    const int a=w.spawn(&caster,1000,1000,0,0),b=w.spawn(&victim,1000,1100,0,1);
    const auto hp=tak::sim::Fixed::fromFloat(float(victim.maxHp)*0.1f);
    w.unit(b)->hp=hp;w.unit(b)->veteran=5;w.unit(b)->xp=5;
    w.unit(a)->mana=1e6;w.unit(a)->fireState=0;w.attack(a,b,false);
    for(int tick=0;tick<600 && w.unit(b)->player!=0;++tick)w.tick(1.f/30);
    check(w.unit(b)->player==0,data?"shipped caster converts a damaged enemy":"caster converts a damaged enemy");
    check(w.unit(b)->hp==hp,"conversion keeps the unit's damage (no half-health floor)");
    check(w.unit(b)->veteran==0 && w.unit(b)->xp==0,"a converted unit starts without experience");
}
}

int main(int argc,char** argv) {
    if(argc>2)return 2;
    {
        auto caster=casterType();rejectedTargets(caster);
        conversionKeepsDamage(caster,victimType(),false);
        // A nearer immune enemy must not starve a farther eligible target.
        {
            auto immune=victimType(),eligible=victimType();immune.commander=true;
            tak::sim::World w;setup(w);
            const int a=w.spawn(&caster,1000,1000,0,0);
            const int blocked=w.spawn(&immune,1000,1100,0,1);
            const int b=w.spawn(&eligible,1000,1200,0,1);
            w.unit(a)->fireState=2;w.unit(a)->moveState=2;
            bool wrong=false;
            for(int tick=0;tick<300 && w.unit(b)->player!=0;++tick) {
                w.tick(1.f/30);wrong|=targets(*w.unit(a),blocked);
            }
            check(!wrong && w.unit(b)->player==0,"capture skips nearer immunity and converts an eligible enemy");
            check(w.unit(blocked)->player==1,"capture does not steal a monarch");
        }
        // Eligibility changes after an attack was accepted. Preserve its queued
        // destination while clearing aim/channel state before another release.
        for(bool capacity:{false,true}) {
            auto victim=victimType();tak::sim::World w;setup(w);
            const int a=w.spawn(&caster,1000,1000,0,0);
            const int b=w.spawn(&victim,1000,1100,0,1);
            w.unit(a)->fireState=0;w.attack(a,b,false);w.tick(1.f/30);
            check(targets(*w.unit(a),b),"eligible attack remains active before eligibility changes");
            w.order(a,1500,1500,true);
            if(capacity)w.setUnitCap(1);else victim.cantBeCaptured=true;
            w.tick(1.f/30);
            check(!targets(*w.unit(a),b) && !w.unit(a)->scriptAimTarget && !w.unit(a)->justFired,
                  "new immunity or full capacity retires an active capture attack");
            check(!w.unit(a)->orders.empty(),"capture retirement retains queued movement");
        }
    }
    // The active spell's category table determines eligibility, even if a
    // different selectable spell could capture this unit.
    {
        auto caster=casterType(),victim=victimType();victim.damageCategory="immune";
        caster.weaponSwitching=true;caster.weapons.push_back(caster.weapon);
        caster.weapons[0].dmgVs["immune"]=0;
        tak::sim::World w;setup(w);
        const int a=w.spawn(&caster,1000,1000,0,0),b=w.spawn(&victim,1000,1100,0,1);
        w.unit(a)->fireState=2;w.attack(a,b,false);
        for(int tick=0;tick<30;++tick)w.tick(1.f/30);
        check(!targets(*w.unit(a),b) && w.unit(b)->player==1,"inactive spell cannot make a category-immune target eligible");
        w.setWeapon(a,1);w.attack(a,b,false);
        for(int tick=0;tick<300 && w.unit(b)->player!=0;++tick)w.tick(1.f/30);
        check(w.unit(b)->player==0,"switching to an eligible capture spell works");
    }
    // Ordinary weapons retain explicit friendly fire.
    {
        auto caster=casterType(),victim=victimType();
        caster.weapons[0].mindControl=false;caster.weapons[0].remote=tak::sim::Weapon::RemoteKind::Plain;
        caster.weapon=caster.weapons[0];tak::sim::World w;setup(w);
        const int a=w.spawn(&caster,1000,1000,0,0),b=w.spawn(&victim,1000,1100,0,0);
        w.unit(a)->fireState=0;w.attack(a,b,false);w.tick(1.f/30);
        check(w.unit(a)->justFired,"ordinary explicit attacks still allow friendly fire");
    }
    if(argc==2) {
        auto vfs=tak::hpi::mountRetailRoot(argv[1]);
        for(bool crusades:{false,true}) {
            tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,crusades);
            int count=0;
            for(const auto& [name,type]:registry.types()) {
                if(!charmOf(type))continue;
                std::printf("capture roster balance=%d unit=%s\n",crusades,name.c_str());
                rejectedTargets(type);++count;
            }
            check(count>=3,"shipped capture roster includes Harpy, Mind Mage and campaign caster");
            const auto* harpy=registry.find("zonharp");
            check(harpy && charmOf(*harpy),"the Harpy carries a mind-control weapon");
            if(!harpy || !charmOf(*harpy))continue;
            auto victim=*registry.find("arasword");victim.maxVel={};
            victim.weapon.damage=0;victim.weapons.clear();victim.healTime=0;
            conversionKeepsDamage(*harpy,victim,true);
            // No capture by contact: a Harpy with no mana for its charm stays
            // beside a worn-down enemy without converting it.
            for(bool automatic:{false,true}) {
                tak::sim::World w;setup(w);
                const int a=w.spawn(harpy,1000,1000,0,0),b=w.spawn(&victim,1000,1060,0,1);
                w.unit(b)->hp=tak::sim::Fixed::fromFloat(float(victim.maxHp)*0.2f);
                w.unit(a)->fireState=2;w.unit(a)->moveState=2;w.unit(a)->mana=0;
                if(!automatic)w.attack(a,b,false);
                for(int tick=0;tick<150;++tick)w.tick(1.f/30);
                check(w.unit(b)->player==1,automatic?"an idle Harpy without mana does not convert by contact":
                      "an attacking Harpy without mana does not convert by contact");
            }
            // With mana the Harpy converts on its own, by its weapon: the charm
            // spends the shot's mana.
            {
                tak::sim::World w;setup(w);
                const int a=w.spawn(harpy,1000,1000,0,0),b=w.spawn(&victim,1000,1200,0,1);
                w.unit(a)->fireState=2;w.unit(a)->moveState=2;w.unit(a)->mana=harpy->maxMana;
                bool fired=false;
                for(int tick=0;tick<600 && w.unit(b)->player!=0;++tick) {w.tick(1.f/30);fired|=w.unit(a)->justFired;}
                check(w.unit(b)->player==0 && fired,"an idle Harpy converts an enemy with its weapon");
                check(w.unit(a)->mana<double(harpy->maxMana),"the Harpy's conversion spent mana");
            }
        }
    }
    return failures?1:0;
}
