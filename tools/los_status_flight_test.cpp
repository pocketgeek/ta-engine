// Mind control, stone and freeze land when their shot arrives. Retail's
// Line-of-Sight status classes launch and draw like the ordinary straight shot
// (52c540 / 52c8c0) and fly with their own update (52d8d0 / 52db40 / 52dda0):
// the unit the shot strikes goes to the class's callback (52da10 / 52dca0 /
// 52df00), which replaces the damage with gates and a roll. The Area Mind
// Control and Area Freeze use the same callbacks. tools/re/probe_los_status.py
// runs the updates and callbacks natively.
//
//   los_status_flight_test             the callbacks, on synthetic units
//   los_status_flight_test <data dir>  the shipped casters, in flight
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tak::sim { struct RetailReplayProbe {
    static void capture(World& w,int id,int owner) {w.captureUnit(*w.unit(id),owner);}
    // A shot of `weapon` striking `target`, as a projectile's collision reports it.
    static void impact(World& w,const Weapon& weapon,int from,int player,int target) {
        auto* victim=w.unit(target);
        w.applyHit(weapon,victim->x.toFloat(),victim->z.toFloat(),player,from,victim);
    }
}; }

namespace {
using tak::sim::Fixed;
using tak::sim::Unit;
using tak::sim::UnitType;
using tak::sim::Weapon;
using tak::sim::World;
using LosStatus=Weapon::LosStatus;
int failures=0;
void check(bool ok,const std::string& message) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",message.c_str());
    if(!ok)++failures;
}
const char* kindName(LosStatus kind) {
    return kind==LosStatus::MindControl ? "charm" : kind==LosStatus::Stone ? "stone" : "freeze";
}
void setup(World& w) {
    w.setPlayerCount(3);w.setTeam(2,0);   // player 2 is player 0's ally
    w.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
}
// Did the status weapon take this unit (for the caster's player 0)?
bool taken(const World& w,int id,LosStatus kind) {
    const Unit* u=w.unit(id);
    if(!u)return false;
    if(kind==LosStatus::MindControl)return u->player==0;
    return u->hp<=Fixed() && u->deathType==(kind==LosStatus::Stone ? 14 : 15);
}

// ---- The callbacks, on synthetic units ---------------------------------------
UnitType body(const char* id) {
    UnitType t;t.id=id;t.maxHp=1000;t.canMove=true;t.buildMovementCode=1;t.maxVel=Fixed::fromInt(1);
    return t;
}
Weapon statusWeapon(LosStatus kind) {
    // The Crusades freeze weapons carry 450 damage; retail never deals it.
    Weapon w;w.damage=450;w.range=400;w.beam=true;w.projVel=500;w.losStatus=kind;
    if(kind==LosStatus::MindControl)w.mindControl=true;
    else w.status=kind==LosStatus::Stone ? Weapon::Status::Stoned : Weapon::Status::Frozen;
    return w;
}

void callbackGates(LosStatus kind) {
    const Weapon weapon=statusWeapon(kind);
    const std::string name=kindName(kind);
    enum Case { Eligible, OwnPlayer, Ally, Building, Monarch, Immune, Unfinished, Carried, Count };
    const char* labels[]={"an eligible enemy","the caster's own unit","an ally's unit","a building",
                          "a Monarch","an immune unit","an unfinished unit","a carried unit"};
    for(int c=Eligible;c<Count;++c) {
        UnitType caster=body("caster"),victim=body("victim");
        if(c==Building) {victim.buildMovementCode=0;victim.canMove=false;victim.maxVel={};}
        if(c==Monarch)victim.commander=true;
        if(c==Immune) {
            if(kind==LosStatus::MindControl)victim.cantBeCaptured=true;
            else if(kind==LosStatus::Stone)victim.cantBeStoned=true;
            else victim.cantBeFrozen=true;
        }
        World w;setup(w);
        const int a=w.spawn(&caster,1000,1000,0,0);
        const int b=w.spawn(&victim,1000,1200,0,c==OwnPlayer ? 0 : c==Ally ? 2 : 1);
        const int carrier=w.spawn(&caster,1100,1200,0,1);
        if(c==Unfinished)w.unit(b)->underConstruction=true;
        if(c==Carried)w.unit(b)->inTransport=carrier;
        const Fixed hp=w.unit(b)->hp;
        // Forty strikes: an eligible unit falls to one of them (80% each).
        for(int i=0;i<40 && !taken(w,b,kind);++i)tak::sim::RetailReplayProbe::impact(w,weapon,a,0,b);
        const bool want=c==Eligible || c==Ally ||
            (c==Carried && kind!=LosStatus::MindControl);   // stone/freeze have no transport gate
        if(c==OwnPlayer && kind==LosStatus::MindControl) {
            check(w.unit(b)->player==0 && w.unit(b)->hp==hp,name+" leaves the caster's own unit alone");
            continue;
        }
        check(taken(w,b,kind)==want,name+(want ? " takes " : " refuses ")+labels[c]);
        if(!want)check(w.unit(b)->hp==hp,name+" deals no damage to "+labels[c]+" (weapon damage 450)");
    }
}

void callbackRoll(LosStatus kind) {
    const Weapon weapon=statusWeapon(kind);
    const std::string name=kindName(kind);
    for(int level:{0,10}) {
        UnitType caster=body("caster"),victim=body("victim");
        World w;setup(w);
        const int a=w.spawn(&caster,200,200,0,0);
        int landed=0,hurt=0;
        const int n=600;
        for(int i=0;i<n;++i) {
            const int b=w.spawn(&victim,float(300+(i%40)*40),float(300+(i/40)*40),0,1);
            w.unit(b)->veteran=level;
            const Fixed hp=w.unit(b)->hp;
            tak::sim::RetailReplayProbe::impact(w,weapon,a,0,b);
            if(taken(w,b,kind))++landed;
            else if(w.unit(b)->hp!=hp)++hurt;
        }
        std::printf("  %s level %d: %d of %d landed\n",name.c_str(),level,landed,n);
        // min(99, (level + 16) * 5): 80 for a green unit, 99 for level 4 and up.
        if(level==0)check(landed>=430 && landed<=530,name+" lands on about 80% of green units");
        else check(landed>=580,name+" lands on about 99% of veterans");
        check(hurt==0,name+" that fails its roll does no damage");
    }
}

void callbackPresentation(LosStatus kind) {
    const Weapon weapon=statusWeapon(kind);
    const std::string name=kindName(kind);
    UnitType caster=body("caster"),victim=body("victim");
    World w;setup(w);
    const int a=w.spawn(&caster,1000,1000,0,0);
    const int b=w.spawn(&victim,1000,1200,0,1);
    w.unit(b)->veteran=10;
    int strikes=0;
    while(!taken(w,b,kind) && strikes<40) {
        w.tick(1.f/30);   // a fresh hit list
        tak::sim::RetailReplayProbe::impact(w,weapon,a,0,b);++strikes;
    }
    const auto& hits=w.hits();
    const auto impact=std::find_if(hits.begin(),hits.end(),[&](const auto& h){return h.weapon==&weapon && h.sound;});
    const auto effect=std::find_if(hits.begin(),hits.end(),[&](const auto& h){return h.weapon==&weapon && !h.sound;});
    check(impact!=hits.end() && !impact->explosion,name+" impact plays its sound without an explosion (529c10)");
    check(effect!=hits.end() && effect->explosion && effect->victimId==b,
          name+" that lands puts its explosion on the victim (492c80)");
}

// A stone or freeze shot lands after its caster changed sides, but retail's
// ownership change (514da0) made the caster a new object: 51a140 names no
// hitter, so the old object gets no kill credit.
void callbackCasterSwitched(LosStatus kind) {
    if(kind==LosStatus::MindControl)return;   // a charm in flight dies with its caster
    const Weapon weapon=statusWeapon(kind);
    const std::string name=kindName(kind);
    UnitType caster=body("caster"),victim=body("victim");
    World w;setup(w);
    const int a=w.spawn(&caster,1000,1000,0,0);
    const int b=w.spawn(&victim,1000,1200,0,1);
    w.unit(b)->veteran=10;
    tak::sim::RetailReplayProbe::capture(w,a,1);
    for(int i=0;i<40 && !taken(w,b,kind);++i)tak::sim::RetailReplayProbe::impact(w,weapon,a,0,b);
    check(taken(w,b,kind) && w.unit(b)->lastHitBy!=a,
          name+" from a caster that changed sides lands and credits no hitter (51a140)");
}

// ---- The shipped casters, in flight ------------------------------------------
struct Caster { const UnitType* type; int slot; LosStatus kind; };

std::vector<Caster> roster(const tak::sim::TypeRegistry& registry) {
    std::vector<Caster> out;
    for(const auto& [id,type]:registry.types())
        for(size_t s=0;s<type.weapons.size() && s<3;++s)
            if(type.weapons[s].losStatus!=LosStatus::None)out.push_back({&type,int(s),type.weapons[s].losStatus});
    return out;
}

// A still enemy the weapon may target and take: mobile, on the ground, with a
// collision quad, not a Monarch, not immune.
const UnitType* victimFor(const tak::sim::TypeRegistry& registry,const Weapon& weapon) {
    for(const char* id:{"araarch","tararch","verarch","zonhunt"}) {
        const UnitType* t=registry.find(id);
        if(t && t->projectileQuad && !t->canFly && !t->commander && t->buildMovementCode==1 &&
           !t->cantBeCaptured && !t->cantBeStoned && !t->cantBeFrozen && weapon.damageVs(t)>0)return t;
    }
    return nullptr;
}

UnitType still(const UnitType& t) {
    UnitType v=t;v.maxVel={};v.weapons.clear();v.weapon=Weapon{};v.healTime=0;
    return v;
}

float distance3(const Unit& a,const Unit& b) {
    const float dx=(a.x-b.x).toFloat(),dz=(a.z-b.z).toFloat();
    const float dy=((a.type->canFly ? a.flightY : a.groundY)-(b.type->canFly ? b.flightY : b.groundY)).toFloat();
    return std::sqrt(dx*dx+dy*dy+dz*dz);
}

bool inFlight(const World& w,int from,const Weapon& weapon) {
    return std::any_of(w.projectiles().begin(),w.projectiles().end(),[&](const auto& p) {
        return p.fromId==from && p.wsrc==&weapon && p.straight && p.life>0;
    });
}

// The caster fires at a still enemy; the effect must land on the tick its
// shot strikes, one flight after the release, never on the release tick.
void shotLandsOnArrival(const Caster& c,const UnitType& victimType,bool crusades) {
    const Weapon& weapon=c.type->weapons[size_t(c.slot)];
    const std::string label=c.type->id+std::string(crusades ? " (Crusades) " : " ")+weapon.name;
    World w;setup(w);
    const int a=w.spawn(c.type,1000,1000,0,0);
    const float d=std::min(220.f,float(weapon.range)-60.f);
    const int b=w.spawn(&victimType,1000,1000+d,0,1);
    w.unit(b)->veteran=10;   // 99 in 100
    if(c.type->weaponSwitching)w.setWeapon(a,c.slot);
    w.attack(a,b,false);
    int fired=-1,landed=-1;float fireDistance=0;bool flew=false,struckThen=false;
    for(int tick=0;tick<900 && landed<0;++tick) {
        if(c.type->maxMana>0)w.unit(a)->mana=c.type->maxMana;
        w.tick(1.f/30);
        const int now=int(w.tickCount());
        const Unit* victim=w.unit(b);
        if(w.unit(a)->justFired && victim && victim->alive() && !taken(w,b,c.kind)) {
            fired=now;fireDistance=distance3(*w.unit(a),*w.unit(b));
        }
        flew|=inFlight(w,a,weapon);
        if(taken(w,b,c.kind)) {
            landed=now;
            struckThen=std::any_of(w.hits().begin(),w.hits().end(),[&](const auto& h) {
                return h.weapon==&weapon && h.victimId==b && h.sound;
            });
        }
    }
    const float perTick=weapon.projVel/30.f;
    const int earliest=std::max(2,int((fireDistance-64.f)/perTick));
    const int latest=int(std::ceil((fireDistance+64.f)/perTick))+3;
    std::printf("  %s: released tick %d at %.0f px, landed tick %d (window %d..%d)\n",label.c_str(),
                fired,fireDistance,landed,fired+earliest,fired+latest);
    check(fired>=0 && landed>=0,label+": the shot is released and lands");
    check(flew,label+": a real shot flies between release and impact");
    check(fired>=0 && landed-fired>=earliest && landed-fired<=latest,
          label+": the effect lands one flight after the release");
    check(struckThen,label+": the effect lands on the tick the shot strikes the unit");
}

// Mind Mage (charm) and Basilisk (stone): what happens to a shot in flight.
void flightCases(const tak::sim::TypeRegistry& registry) {
    const UnitType* mage=registry.find("tarmind");
    const UnitType* basilisk=registry.find("zonbasil");
    check(mage && basilisk,"the Mind Mage and the Basilisk exist");
    if(!mage || !basilisk)return;
    const auto slotOf=[](const UnitType& t) {
        for(size_t s=0;s<t.weapons.size();++s)if(t.weapons[s].losStatus!=LosStatus::None)return int(s);
        return -1;
    };
    for(const UnitType* casterType:{mage,basilisk}) {
        const int slot=slotOf(*casterType);
        const Weapon& weapon=casterType->weapons[size_t(slot)];
        const LosStatus kind=weapon.losStatus;
        const UnitType* pick=victimFor(registry,weapon);
        check(pick!=nullptr,"an eligible victim for "+casterType->id);
        if(!pick)return;
        const UnitType victimType=still(*pick);
        const std::string name=casterType->id;
        enum Event { TargetDies, TargetTurned, CasterDies };
        for(Event event:{TargetDies,TargetTurned,CasterDies}) {
            if(event==TargetTurned && kind!=LosStatus::MindControl)continue;
            World w;setup(w);
            const int a=w.spawn(casterType,1000,1000,0,0);
            const int first=w.spawn(&victimType,1000,1160,0,1);
            const int behind=w.spawn(&victimType,1000,1250,0,1);
            w.unit(first)->veteran=w.unit(behind)->veteran=10;
            if(casterType->weaponSwitching)w.setWeapon(a,slot);
            w.attack(a,first,false);
            int fired=-1,released=0;bool firstTaken=false,behindTaken=false;
            for(int tick=0;tick<240;++tick) {
                if(Unit* u=w.unit(a))u->mana=casterType->maxMana;
                w.tick(1.f/30);
                const int now=int(w.tickCount());
                const Unit* shooter=w.unit(a);
                if(fired<0 && shooter && shooter->justFired) {
                    fired=now;++released;
                    firstTaken=taken(w,first,kind);   // the old engine's instant effect
                    if(event==TargetDies)w.scenarioDestroy(first);
                    if(event==TargetTurned)tak::sim::RetailReplayProbe::capture(w,first,0);
                    // One shot only: no second one may take the unit behind.
                    w.stop(a);w.unit(a)->fireState=0;
                } else if(fired>=0 && w.unit(a) && w.unit(a)->justFired)++released;
                if(event==CasterDies && fired>=0 && now==fired+3)w.scenarioDestroy(a);
                if(event==TargetTurned)firstTaken|=w.unit(first) && w.unit(first)->player!=0 && taken(w,first,kind);
                behindTaken|=taken(w,behind,kind);
                if(event==CasterDies)firstTaken|=taken(w,first,kind);
            }
            check(fired>=0 && released==1,name+": one shot is released");
            if(event==TargetDies) {
                check(!firstTaken,name+": nothing lands at the release on a target that dies in flight");
                check(behindTaken,name+": its shot flies on and the unit behind takes it");
            } else if(event==TargetTurned) {
                check(w.unit(first) && w.unit(first)->player==0,
                      name+": a target turned to the caster's side mid-flight stays turned");
                check(behindTaken,name+": the shot passes the caster's own unit and takes the one behind");
            } else if(kind==LosStatus::MindControl) {
                check(!firstTaken,name+": a charm in flight dies with its caster (52d8d0)");
            } else {
                check(firstTaken,name+": a stone shot in flight still lands after its caster dies (52db40)");
            }
        }
        if(kind!=LosStatus::Stone)continue;
        // A Monarch and a building: the stone would do nothing (the commander
        // and mover gates of 52dca0), so the Basilisk refuses the order
        // (engine deviation, World::statusCanTake; retail spends its shots).
        // Every shipped Monarch is cantbestoned as well; the copy here is
        // not, so only the commander gate stands between it and the stone.
        const UnitType* monarch=nullptr;const UnitType* building=nullptr;
        for(const auto& [id,t]:registry.types()) {
            if(!monarch && t.commander && t.projectileQuad && !t.canFly && weapon.damageVs(&t)>0)monarch=&t;
            if(!building && t.isStructure() && t.buildMovementCode!=1 && t.projectileQuad && !t.cantBeStoned &&
               t.footX<=3 && t.footZ<=3 && weapon.damageVs(&t)>0 && t.weapons.empty())building=&t;
        }
        check(monarch && building,"a Monarch and a building to petrify");
        for(const UnitType* target:{monarch,building}) {
            if(!target)continue;
            UnitType targetType=target->isStructure() ? *target : still(*target);
            targetType.cantBeStoned=targetType.cantBeFrozen=false;
            World w;setup(w);
            const int a=w.spawn(casterType,1000,1000,0,0);
            w.unit(a)->mana=casterType->maxMana;   // a new unit's pool starts empty (511990)
            const int b=w.spawn(&targetType,1000,1200,0,1);
            const Fixed hp=w.unit(b)->hp;
            w.attack(a,b,false);
            bool fired=false,struck=false;
            for(int tick=0;tick<240;++tick) {
                w.tick(1.f/30);
                fired|=w.unit(a)->justFired;
                struck|=std::any_of(w.hits().begin(),w.hits().end(),[&](const auto& h){return h.weapon==&weapon && h.victimId==b;});
            }
            check(!fired && !struck,name+" refuses "+target->id);
            const Unit* struckUnit=w.unit(b);
            check(struckUnit && struckUnit->alive() && struckUnit->hp==hp,
                  name+": "+target->id+" is not petrified and takes no damage");
        }
    }
}
}

int main(int argc,char** argv) {
    if(argc>2)return 2;
    for(LosStatus kind:{LosStatus::MindControl,LosStatus::Stone,LosStatus::Frozen}) {
        callbackGates(kind);
        callbackRoll(kind);
        callbackPresentation(kind);
        callbackCasterSwitched(kind);
    }
    if(argc==2) {
        auto vfs=tak::hpi::mountRetailRoot(argv[1]);
        for(bool crusades:{false,true}) {
            tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,crusades);
            const auto casters=roster(registry);
            std::printf("balance=%d: %zu status casters\n",int(crusades),casters.size());
            check(casters.size()==7,"seven casters carry a Line-of-Sight status weapon");
            for(const auto& c:casters) {
                const Weapon& weapon=c.type->weapons[size_t(c.slot)];
                const UnitType* pick=victimFor(registry,weapon);
                check(pick!=nullptr,std::string("an eligible victim for ")+c.type->id);
                if(!pick)continue;
                shotLandsOnArrival(c,still(*pick),crusades);
            }
            if(!crusades)flightCases(registry);
        }
    }
    return failures?1:0;
}
