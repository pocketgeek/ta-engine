// Mind control, stone and freeze land when their shot arrives. Retail's
// Line-of-Sight status classes launch and draw like the ordinary straight shot
// (52c540 / 52c8c0) and fly with their own update (52d8d0 / 52db40 / 52dda0):
// the unit the shot strikes goes to the class's callback (52da10 / 52dca0 /
// 52df00), which replaces the damage with gates and a roll. The Area Mind
// Control and Area Freeze use the same callbacks. tools/re/probe_los_status.py
// runs the updates and callbacks natively.
//
//   los_status_flight_test             the callbacks, on synthetic units
// No Total Annihilation weapon carries a status class, so the shipped-caster
// flight cases (Kingdoms' Mind Mage and Basilisk) went with their data.
#include "sim/matchsetup.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ta::sim { struct RetailReplayProbe {
    static void capture(World& w,int id,int owner) {w.captureUnit(*w.unit(id),owner);}
    // A shot of `weapon` striking `target`, as a projectile's collision reports it.
    static void impact(World& w,const Weapon& weapon,int from,int player,int target) {
        auto* victim=w.unit(target);
        w.applyHit(weapon,victim->x.toFloat(),victim->z.toFloat(),player,from,victim);
    }
}; }

namespace {
using ta::sim::Fixed;
using ta::sim::Unit;
using ta::sim::UnitType;
using ta::sim::Weapon;
using ta::sim::World;
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
        for(int i=0;i<40 && !taken(w,b,kind);++i)ta::sim::RetailReplayProbe::impact(w,weapon,a,0,b);
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
            ta::sim::RetailReplayProbe::impact(w,weapon,a,0,b);
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
        ta::sim::RetailReplayProbe::impact(w,weapon,a,0,b);++strikes;
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
    ta::sim::RetailReplayProbe::capture(w,a,1);
    for(int i=0;i<40 && !taken(w,b,kind);++i)ta::sim::RetailReplayProbe::impact(w,weapon,a,0,b);
    check(taken(w,b,kind) && w.unit(b)->lastHitBy!=a,
          name+" from a caster that changed sides lands and credits no hitter (51a140)");
}

}

int main(int argc,char**) {
    if(argc>1)return 2;
    for(LosStatus kind:{LosStatus::MindControl,LosStatus::Stone,LosStatus::Frozen}) {
        callbackGates(kind);
        callbackRoll(kind);
        callbackPresentation(kind);
        callbackCasterSwitched(kind);
    }
    return failures?1:0;
}
