// What other units hold on a unit ends when it changes owner.
//
// Retail's change of owner (514da0) makes the new owner a new object and kills
// the old one, and that death lets go of every mission and controller that
// named the object (512860 -> 519a60: event 8, target pointer zeroed), queued
// missions included; a weapon record's target goes at its next lookup
// (51a9a0). World keeps the unit's id, so World::changeOwner points every
// other unit's reference at no unit (World::retireReferences) and each holder
// loses it as it loses a target that died. Before, only automatic and
// mind-control attacks ended: an explicit Attack stayed on a unit the
// attacker's own side had just charmed (melee units killed it, Gryphons made
// runs over it for the rest of its life), a producer's Attack rally kept
// sending new units at it, a queued Attack leg came up later, and a guard
// went on escorting a ward an enemy had charmed.
//
// Without an argument this runs on synthetic types (a real charm from a
// synthetic mind-control caster, a gift, a scenario transfer); with a retail
// data root it also runs the shipped roster: a Harpy's and a Mind Mage's
// charm against Swordsman, Archer, Gryphon, Troll and Hunter holders, a Keep's
// rally, a guard, a third player's attacker, in both pathfinding modes; and
// under Legion, two attackers whose queued targets are both charmed (the
// references let go share one id, which must not join them in one group).
#include "hpi/hpi.h"
#include "net/lockstep.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace ta::sim;
using ta::net::Cmd;

int failures=0;
void check(bool ok,const std::string& what) {
    std::printf("[%s] %s\n",ok ? "PASS" : "FAIL",what.c_str());
    if (!ok) ++failures;
}

// Players 0 and 2 are allies; 1 and 3 are each hostile to everyone.
void sides(World& w) {
    w.setPlayerCount(4);
    w.setTeam(0,0);w.setTeam(1,1);w.setTeam(2,0);w.setTeam(3,3);
}

// Does `u` still hold anything on unit `id`: an order leg or one of its route
// corners, a rally step or a weapon slot.
bool names(const Unit& u,int id) {
    const auto leg=[&](const Order& o) { return o.targetId==id || o.repairTarget==id; };
    return std::any_of(u.orders.begin(),u.orders.end(),leg) || std::any_of(u.rally.begin(),u.rally.end(),leg) ||
           std::find(u.slotTarget.begin(),u.slotTarget.end(),id)!=u.slotTarget.end();
}
bool anyoneNames(const World& w,int id) {
    for (const Unit& u:w.units()) if (u.id!=id && u.alive() && names(u,id)) return true;
    return false;
}
// An order leg with a target unit of any kind (attack, guard, boarding).
bool targeted(const Unit& u) {
    return std::any_of(u.orders.begin(),u.orders.end(),[](const Order& o) { return o.targetId!=0; });
}
float distance(const Unit& u,float x,float z) {
    const float dx=u.x.toFloat()-x,dz=u.z.toFloat()-z;
    return std::sqrt(dx*dx+dz*dz);
}

// ---- synthetic types -------------------------------------------------------

UnitType body() {
    // bmcode 1: the charm refuses a body without a mover. Upright, so it
    // stands on the terrain and is seen (capture_combat_test's victim).
    UnitType t;t.id="body";t.maxHp=10000;t.healTime=0;t.canMove=true;t.buildMovementCode=1;t.upright=true;
    return t;
}
UnitType caster() {
    auto t=body();t.id="caster";
    Weapon w;w.damage=1;w.range=400;w.reload=0.1f;w.aimTol=65536;   // it fires on any bearing
    w.mindControl=true;w.kind=Weapon::Kind::Remote;
    w.remote=Weapon::RemoteKind::MindCtl;w.unitsOnly=true;w.aoe=32;w.buildUpTicks=8;w.decayTicks=8;
    t.weapon=w;t.weapons={w};return t;
}
UnitType fighter() {
    auto t=caster();t.id="fighter";
    t.weapons[0].mindControl=false;t.weapons[0].remote=Weapon::RemoteKind::Plain;
    t.weapons[0].damage=20;t.weapons[0].range=200;t.weapons[0].reload=0.5f;
    t.weapon=t.weapons[0];return t;
}

struct Plain {
    World w;
    Plain() { sides(w);w.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20); }
    void run(int ticks) { for (int i=0;i<ticks;++i) w.tick(1.f/30); }
    // A caster on Hold Fire: it charms what it is ordered onto and nothing else.
    int spawnCaster(const UnitType& type,float x,float z,int player) {
        const int id=w.spawn(&type,x,z,0,player);
        w.unit(id)->fireState=0;
        return id;
    }
    // A real charm: the caster is ordered onto the unit and casts until it is
    // its player's. The tick the charm takes is the last one run.
    bool charm(int casterId,int victim,int limit=900) {
        const int owner=w.unit(casterId)->player;
        w.attack(casterId,victim,false);
        for (int t=0;t<limit && w.unit(victim)->alive() && w.unit(victim)->player!=owner;++t) run(1);
        return w.unit(victim)->alive() && w.unit(victim)->player==owner;
    }
};

void syntheticCases() {
    const UnitType vt=body(),ct=caster(),ft=fighter();
    // An explicit attack on a unit the attacker's own player, or an ally,
    // then charms; afterwards an attack given on the friendly unit still
    // stands (intentional friendly fire, as capture_combat_test has it).
    for (int charmer:{0,2}) {
        const std::string who=charmer==0 ? "own charm" : "ally's charm";
        Plain p;World& w=p.w;
        const int f=w.spawn(&ft,1000,1000,0,0),v=w.spawn(&vt,1000,1150,0,1),c=p.spawnCaster(ct,1150,1150,charmer);
        w.unit(f)->fireState=0;w.attack(f,v,false);
        p.run(20);
        check(names(*w.unit(f),v),"synthetic, "+who+": the attacker holds its explicit attack before the charm");
        check(p.charm(c,v),"synthetic, "+who+": the charm takes");
        check(!anyoneNames(w,v),"synthetic, "+who+": nobody holds an order or slot on the unit once it has changed owner");
        p.run(30);   // shots already on their way
        const Fixed hp=w.unit(v)->hp;
        bool fired=false;
        for (int t=0;t<120;++t) { p.run(1);fired|=w.unit(f)->justFired; }
        check(!fired && w.unit(f)->orders.empty() && w.unit(v)->hp==hp,
              "synthetic, "+who+": the attacker lets go and never fires at the charmed unit");
        w.attack(f,v,false);
        bool again=false;
        for (int t=0;t<30;++t) { p.run(1);again|=w.unit(f)->justFired; }
        check(again && names(*w.unit(f),v),
              "synthetic, "+who+": an explicit attack given on the friendly unit after the change is carried out");
    }
    // A queued attack leg on the charmed unit, behind an attack on another.
    {
        Plain p;World& w=p.w;
        const int f=w.spawn(&ft,1000,1000,0,0),e=w.spawn(&vt,900,1100,0,1),v=w.spawn(&vt,1000,1150,0,1);
        const int c=p.spawnCaster(ct,1150,1150,0);
        w.unit(f)->fireState=0;w.attack(f,e,false);w.attack(f,v,true);
        p.run(20);
        check(w.unit(f)->orders.size()==2 && names(*w.unit(f),v),"synthetic, queued: the second attack waits in the queue");
        check(p.charm(c,v),"synthetic, queued: the charm takes");
        check(!names(*w.unit(f),v) && !w.unit(f)->orders.empty() && w.unit(f)->orders.front().targetId==e,
              "synthetic, queued: the queued leg lets go and the current attack goes on");
        w.scenarioDestroy(e);
        const Fixed hp=w.unit(v)->hp;
        bool fired=false;
        for (int t=0;t<90;++t) { p.run(1);fired|=t>0 && w.unit(f)->justFired; }
        check(!fired && w.unit(f)->orders.empty() && w.unit(v)->hp==hp,
              "synthetic, queued: when its turn comes the leg ends without a shot at the charmed unit");
    }
    // A producer's Attack rally step on the unit.
    {
        auto keep=body();keep.id="keep";keep.isBuilder=true;keep.maxVel={};
        Plain p;World& w=p.w;
        const int k=w.spawn(&keep,1000,1000,0,0),v=w.spawn(&vt,1000,1150,0,1),c=p.spawnCaster(ct,1150,1150,0);
        w.attack(k,v,false);
        check(w.unit(k)->rally.size()==1 && w.unit(k)->rally.front().targetId==v,
              "synthetic, rally: an attack on an enemy is the producer's rally step");
        p.run(20);
        check(p.charm(c,v),"synthetic, rally: the charm takes");
        check(!names(*w.unit(k),v),"synthetic, rally: the rally step lets go of the charmed unit");
    }
    // A guard whose ward an enemy charms, and a WaitForAttack watching it.
    {
        auto mover=body();mover.id="mover";mover.maxVel=Fixed::fromInt(2);
        Plain p;World& w=p.w;
        const int v=w.spawn(&vt,1000,1150,0,1),g=w.spawn(&mover,1000,1050,0,1),s=w.spawn(&mover,900,1150,0,1);
        const int c=p.spawnCaster(ct,1150,1150,0);
        w.guard(g,v,false);w.orderWaitAttack(s,false,v);
        p.run(20);
        check(names(*w.unit(g),v) && w.unit(g)->orders.front().guard,"synthetic, guard: the guard holds before the charm");
        check(w.unit(s)->orders.size()==1 && w.unit(s)->orders.front().waitAttack,
              "synthetic, wait: the WaitForAttack holds before the charm");
        check(p.charm(c,v) && w.unit(g)->player==1 && w.unit(s)->player==1,"synthetic, guard: an enemy's charm takes the ward alone");
        check(!names(*w.unit(g),v),"synthetic, guard: the guard lets go of a ward an enemy charmed");
        p.run(3);
        check(w.unit(g)->orders.empty(),"synthetic, guard: the guard order has ended");
        check(w.unit(s)->orders.empty(),"synthetic, wait: a WaitForAttack on the unit ends with the old object");
    }
    // A third player's explicit attack ends too; still hostile to the new
    // owner, its unit takes the new object by its own targeting.
    {
        Plain p;World& w=p.w;
        const int h=w.spawn(&ft,1000,1000,0,3),v=w.spawn(&vt,1000,1150,0,1),c=p.spawnCaster(ct,1150,1150,0);
        w.attack(h,v,false);
        p.run(20);
        check(names(*w.unit(h),v),"synthetic, third player: its attacker holds an explicit attack before the charm");
        check(p.charm(c,v),"synthetic, third player: the charm takes");
        check(!anyoneNames(w,v),"synthetic, third player: its explicit attack lets go at the charm");
        bool again=false,hit=false;
        const Fixed hp=w.unit(v)->hp;
        for (int t=0;t<240 && w.unit(v) && w.unit(v)->alive();++t) {
            p.run(1);
            again|=names(*w.unit(h),v);
            if (const Unit* u=w.unit(v)) hit|=u->hp<hp && u->lastHitPlayer==3;
        }
        check(again && hit,"synthetic, third player: it takes the new owner's unit again on its own and hits it");
    }
    // A gift between allies and a scenario transfer are the same change of
    // owner: a guard, a queued repair and an enemy's attack all let go.
    for (int path=0;path<2;++path) {
        const std::string how=path==0 ? "gift" : "scenario transfer";
        auto mover=body();mover.id="mover";mover.maxVel=Fixed::fromInt(2);
        auto mason=mover;mason.id="mason";mason.isBuilder=true;mason.workerTime=100;mason.buildDist=400;
        Plain p;World& w=p.w;
        const int u=w.spawn(&vt,1000,1150,0,0),g=w.spawn(&mover,1000,1050,0,0),b=w.spawn(&mason,900,1150,0,0);
        const int h=w.spawn(&ft,1000,1000,0,1);
        w.spawn(&vt,300,300,0,2);   // the recipient is in the game
        w.player(0).mana=1e6;
        w.unit(u)->hp=Fixed::fromInt(5000);
        w.guard(g,u,false);w.orderWait(b,2.0f,false);w.repair(b,u,true);w.attack(h,u,false);
        p.run(10);
        check(names(*w.unit(g),u) && names(*w.unit(b),u) && names(*w.unit(h),u),
              "synthetic, "+how+": a guard, a queued repair and an enemy's attack hold before the change");
        check(path==0 ? w.giveUnit(u,0,2) : w.scenarioTransfer(u,2),"synthetic, "+how+": the unit changes owner");
        check(!anyoneNames(w,u),"synthetic, "+how+": nobody holds an order or slot on the unit after the change");
        bool mended=false,guarded=false;
        Fixed last=w.unit(u)->hp;
        for (int t=0;t<240 && w.unit(u) && w.unit(u)->alive();++t) {
            p.run(1);
            const Unit* now=w.unit(u);
            if (!now) break;
            mended|=now->hp>last;last=now->hp;
            if (const Unit* guard=w.unit(g);guard && guard->alive() && t>2)
                guarded|=std::any_of(guard->orders.begin(),guard->orders.end(),[](const Order& o) { return o.guard; });
        }
        const Unit* builder=w.unit(b);
        check(!guarded && !mended && builder && builder->alive() && builder->orders.empty(),
              "synthetic, "+how+": the guard and the queued repair have ended");
    }
}

// ---- the shipped roster ------------------------------------------------------

void command(World& w,const TypeRegistry& reg,Cmd kind,int player,int unit,int target=0,uint8_t queue=0,
             float x=0,float z=0,const char* type=nullptr) {
    ta::net::Command c;c.kind=kind;c.player=uint8_t(player);c.unitId=unit;c.targetId=target;c.queue=queue;c.x=x;c.z=z;
    if (type) std::snprintf(c.type,sizeof c.type,"%s",type);
    applyCommand(w,reg,c);
}

struct Field {
    World w;
    const TypeRegistry& reg;
    std::string label;
    Field(const TypeRegistry& registry,PathfindingMode mode) : reg(registry),label(pathfindingModeName(mode)) {
        // The order setupMatch uses.
        w.setPathfindingMode(PathfindingMode::Retail);
        w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
        w.setMapPlacementFeatures(std::vector<uint16_t>(256*256,0xffff),{});
        w.buildNavClasses(reg);w.setPathService(true);
        w.setPathfindingMode(mode);
        sides(w);
        w.setPathBudgetClasses(~0u);
        w.setUnitCap(2000);
    }
    void run(int ticks) { for (int i=0;i<ticks;++i) w.tick(1.f/30); }
    int spawn(const char* type,float x,float z,int player) {
        const UnitType* t=reg.find(type);
        if (!t) { check(false,label+": shipped type "+type+" is missing");return 0; }
        return w.spawn(t,x,z,0,player);
    }
    // A real charm by a Harpy or Mind Mage with its Attack click on the unit.
    // The tick the charm takes is the last one run. `keep`: a unit whose hit
    // points are held up meanwhile (one the charm must find alive).
    bool charm(int casterId,int victim,int keep=0,int limit=1800) {
        if (!casterId) return false;
        const int player=w.unit(casterId)->player;
        command(w,reg,Cmd::Attack,player,casterId,victim);
        for (int t=0;t<limit && w.unit(victim)->alive() && w.unit(victim)->player!=player;++t) {
            if (Unit* k=w.unit(keep);k && k->alive()) k->hp=Fixed::fromInt(int(k->type->maxHp));
            run(1);
        }
        return w.unit(victim)->alive() && w.unit(victim)->player==player;
    }
    // ...by a caster that arrives beside the unit.
    bool charm(const char* casterType,int player,int victim) {
        const Unit& v=*w.unit(victim);
        return charm(spawn(casterType,v.x.toFloat()-220,v.z.toFloat()+60,player),victim);
    }
};

// A selection with a charm caster in it is given one Attack click on an enemy
// unit; each unit also has a move queued behind the attack. `casterOwner` 0:
// the player's own caster, 2: an ally's.
void selectionCase(const TypeRegistry& reg,PathfindingMode mode,const std::vector<const char*>& holders,
                   const char* victimType,const char* casterType,int casterOwner) {
    Field f(reg,mode);World& w=f.w;
    const std::string tag=f.label+", "+(casterOwner==0 ? "own " : "ally's ")+casterType+" on "+victimType;
    const int v=f.spawn(victimType,2600,2000,1);
    std::vector<int> ids;
    for (size_t i=0;i<holders.size();++i) ids.push_back(f.spawn(holders[i],1900,1800+80*float(i),0));
    const int lone=f.spawn("zongryp",1900,1700,0);   // its attack is its last order
    if (!v || !lone || std::find(ids.begin(),ids.end(),0)!=ids.end()) return;
    const float restX=2000,restZ=2150;   // a short walk from where the charm finds them
    for (int id:ids) {
        command(w,reg,Cmd::Attack,0,id,v);
        command(w,reg,Cmd::Move,0,id,0,ta::net::kQueueAppend,restX,restZ);
    }
    command(w,reg,Cmd::Attack,0,lone,v);
    const bool took=f.charm(casterType,casterOwner,v);
    check(took,tag+": the charm takes");
    if (!took) return;
    check(!anyoneNames(w,v),tag+": nobody holds an order, rally step or slot on the unit once it has changed owner");
    bool seek=false,fired=false,held=false,hurt=false;
    Fixed last=w.unit(v)->hp;
    for (int t=0;t<900;++t) {
        f.run(1);
        const Unit* victim=w.unit(v);
        if (!victim || !victim->alive()) break;
        held|=anyoneNames(w,v);
        // (By tick 90 the shots that were in the air at the charm have landed.)
        hurt|=t>90 && victim->hp<last;
        last=victim->hp;
        for (int id:ids) if (const Unit* u=w.unit(id);u && u->alive() && t>2) fired|=u->justFired;
        // (Promptly: on a unit left alive, not when its target has at last been killed.)
        if (const Unit* u=w.unit(lone);u && u->alive() && t<150)
            seek|=!u->orders.empty() && u->orders.front().seekAttack;
    }
    const Unit* victim=w.unit(v);
    check(victim && victim->alive() && victim->player==casterOwner && !held,
          tag+": the charmed unit lives and nobody takes it as a target again");
    check(victim && victim->alive() && !hurt && !fired,
          tag+": no holder fires after the charm and the unit takes no more damage");
    bool arrived=true;
    for (size_t i=0;i<ids.size();++i) {
        const Unit* u=w.unit(ids[i]);
        const bool there=u && u->alive() && !targeted(*u) && distance(*u,restX,restZ)<160;
        if (!there) std::printf("       %s: %zu orders, %.0f px from its queued move\n",holders[i],
                                u ? u->orders.size() : size_t(0),u ? double(distance(*u,restX,restZ)) : -1.0);
        arrived&=there;
    }
    check(arrived,tag+": each holder went on with the move queued behind its attack");
    check(seek,tag+": a Gryphon whose attack was its last order goes on as VTOL_SeekAttack (seekLostTarget)");
}

// Attack on one enemy, a queued Attack on a second; the second is charmed
// while the unit is still on the first.
void queuedCase(const TypeRegistry& reg,PathfindingMode mode,const char* holder,const char* casterType) {
    Field f(reg,mode);World& w=f.w;
    const std::string tag=f.label+", queued attack ("+holder+", "+casterType+")";
    const int a=f.spawn(holder,2000,2000,0),first=f.spawn("zontrain",2000,2200,1),second=f.spawn("zontrain",2420,2000,1);
    if (!a || !first || !second) return;
    command(w,reg,Cmd::Attack,0,a,first,ta::net::kQueueReplace);
    command(w,reg,Cmd::Attack,0,a,second,ta::net::kQueueAppend);
    f.run(30);
    check(names(*w.unit(a),first) && names(*w.unit(a),second),tag+": both legs are held before the charm");
    // The first target must outlast the charm.
    const bool took=f.charm(f.spawn(casterType,2620,2060,0),second,first);
    check(took,tag+": the charm takes the second target");
    if (!took) return;
    check(!anyoneNames(w,second) && names(*w.unit(a),first),
          tag+": the queued leg lets go at the charm and the current attack goes on");
    w.scenarioDestroy(first);
    f.run(2);
    const float x=w.unit(a)->x.toFloat(),z=w.unit(a)->z.toFloat();
    const Fixed hp=w.unit(second)->hp;
    bool fired=false,held=false;
    for (int t=0;t<450;++t) {
        f.run(1);
        fired|=w.unit(a)->justFired;held|=names(*w.unit(a),second);
    }
    check(!fired && !held && w.unit(second)->alive() && w.unit(second)->hp>=hp,
          tag+": when the first target is gone the unit does not turn on the charmed one");
    check(w.unit(a)->orders.empty() && distance(*w.unit(a),x,z)<48,
          tag+": the queue ends there, with no walk for the leg that let go");
}

// A Keep's Attack rally on an enemy unit that the Keep's player then charms:
// the Swordsman it trains afterwards is not sent at it.
void rallyCase(const TypeRegistry& reg,PathfindingMode mode) {
    const UnitType* keepType=reg.find("arakeep");
    if (!keepType) { check(false,"rally: shipped type arakeep is missing");return; }
    auto keep=*keepType;keep.storage=1000000;   // economy only: the pool can hold the price
    Field f(reg,mode);World& w=f.w;
    const std::string tag=f.label+", rally (arakeep, arasword)";
    const float kx=float(112*16+keep.footX%2*8),kz=float(112*16+keep.footZ%2*8);
    const int k=w.spawn(&keep,kx,kz,0,0),v=f.spawn("zontrain",kx+500,kz+350,1);
    if (!v) return;
    w.player(0).mana=1000000;
    command(w,reg,Cmd::Attack,0,k,v);   // a producer selected, right-click on the enemy unit
    check(w.unit(k)->rally.size()==1 && w.unit(k)->rally.front().targetId==v,tag+": the click is the Keep's rally step");
    f.run(30);
    const bool took=f.charm("zonharp",0,v);
    check(took,tag+": the charm takes");
    if (!took) return;
    check(!names(*w.unit(k),v),tag+": the rally step lets go of the charmed unit");
    command(w,reg,Cmd::Train,0,k,1,0,0,0,"arasword");
    int out=0;
    for (int t=0;t<3000 && !out;++t) {
        f.run(1);
        for (const Unit& u:w.units())
            if (u.alive() && u.player==0 && !u.underConstruction && u.type && u.type->id=="arasword") out=u.id;
    }
    check(out!=0,tag+": the Keep trains a Swordsman after the charm");
    if (!out) return;
    check(!names(*w.unit(out),v) && !targeted(*w.unit(out)),tag+": the new unit is not handed an attack on the charmed unit");
    const Fixed hp=w.unit(v)->hp;
    bool held=false;
    for (int t=0;t<300;++t) { f.run(1);held|=anyoneNames(w,v); }
    check(!held && w.unit(v)->alive() && w.unit(v)->hp>=hp,tag+": nothing attacks the charmed unit afterwards");
}

// A walking and a flying guard whose ward an enemy charms.
void guardCase(const TypeRegistry& reg,PathfindingMode mode) {
    Field f(reg,mode);World& w=f.w;
    const std::string tag=f.label+", guard";
    const int v=f.spawn("zontrain",2400,2000,1),walker=f.spawn("zonter",2300,2100,1),flyer=f.spawn("zongryp",2500,2100,1);
    if (!v || !walker || !flyer) return;
    // Passive: after the guard ends they must not hunt the new enemy, so the
    // check below sees the guard order alone.
    for (int id:{walker,flyer}) { command(w,reg,Cmd::Stance,1,id,2);command(w,reg,Cmd::Guard,1,id,v); }
    f.run(60);
    check(names(*w.unit(walker),v) && names(*w.unit(flyer),v),tag+": both guards hold before the charm");
    const bool took=f.charm("zonharp",0,v);
    check(took,tag+": an enemy's charm takes the ward");
    if (!took) return;
    check(!anyoneNames(w,v),tag+": the guards let go of a ward an enemy charmed");
    bool seekGuard=false,held=false;
    for (int t=0;t<120;++t) {
        f.run(1);
        held|=names(*w.unit(walker),v) || names(*w.unit(flyer),v);
        if (const Unit* u=w.unit(flyer);u && u->alive()) seekGuard|=!u->orders.empty() && u->orders.front().seekGuard;
    }
    const Unit* g=w.unit(walker);
    check(!held && g && g->alive() && std::none_of(g->orders.begin(),g->orders.end(),[](const Order& o) { return o.guard; }),
          tag+": the walking guard's order has ended and it no longer follows the unit");
    check(seekGuard,tag+": the flying guard goes on as VTOL_SeekGuard (seekLostWard)");
}

// A third player's explicit attack on a unit that someone else charms: the
// order ends, and its unit takes the new owner's unit again by itself.
void thirdPlayerCase(const TypeRegistry& reg,PathfindingMode mode) {
    Field f(reg,mode);World& w=f.w;
    const std::string tag=f.label+", third player (araarch)";
    // Well inside the Archer's sight: automatic targeting takes only what its
    // player sees, where an order needs no sight.
    const int v=f.spawn("zontrain",2400,2000,1),h=f.spawn("araarch",2400,1880,3);
    if (!v || !h) return;
    command(w,reg,Cmd::Attack,3,h,v);
    f.run(30);
    check(names(*w.unit(h),v),tag+": its Archer holds an explicit attack before the charm");
    // The beast must outlast the Archer's arrows until the charm takes.
    const int casterId=f.spawn("zonharp",2180,1900,0);
    const bool took=f.charm(casterId,v,v);
    check(took,tag+": the charm takes");
    if (!took) return;
    check(!anyoneNames(w,v),tag+": its explicit attack lets go at the charm");
    // Leave the charmed unit as the only enemy the Archer can see.
    w.scenarioDestroy(casterId);
    const Fixed hp=w.unit(v)->hp;
    bool again=false,automatic=false,hit=false;
    for (int t=0;t<450 && w.unit(v) && w.unit(v)->alive();++t) {
        f.run(1);
        const Unit* u=w.unit(h);
        if (!u || !u->alive()) break;
        again|=names(*u,v);
        for (const Order& o:u->orders) automatic|=o.targetId==v && o.autoTarget;
        automatic|=std::find(u->slotTarget.begin(),u->slotTarget.end(),v)!=u->slotTarget.end();
        if (const Unit* victim=w.unit(v)) hit|=victim->hp<hp && victim->lastHitPlayer==3;
    }
    check(again && automatic,tag+": still hostile, it takes the new owner's unit again by automatic targeting");
    check(hit,tag+": and hits it");
}

// Unchanged: an explicit Attack given on an own unit after the change.
void friendlyFireCase(const TypeRegistry& reg,PathfindingMode mode) {
    Field f(reg,mode);World& w=f.w;
    const std::string tag=f.label+", explicit attack on an own unit";
    const int v=f.spawn("zontrain",2400,2000,1),a=f.spawn("araarch",2400,2150,0);
    if (!v || !a) return;
    command(w,reg,Cmd::Stance,0,a,2);   // Passive: only an order makes it shoot
    const bool took=f.charm("zonharp",0,v);
    check(took,tag+": the charm takes");
    if (!took) return;
    command(w,reg,Cmd::Attack,0,a,v);
    bool fired=false,held=true;
    for (int t=0;t<150;++t) { f.run(1);fired|=w.unit(a)->justFired;held&=names(*w.unit(a),v); }
    check(fired && held,tag+": given after the change it is kept and carried out (friendly fire)");
}

// Two attackers, each with a queued Attack on a different enemy unit; their
// player charms both units while they are still on a first target. Every
// reference let go carries one id, the id of no unit, and Legion keys an
// attack or guard leg, and joins its group, by the unit chased: when the legs'
// turn comes it must not read the two holders as chasing one unit.
void sharedIdCase(const TypeRegistry& reg) {
    Field f(reg,PathfindingMode::Legion);World& w=f.w;
    const std::string tag=f.label+", two charmed units (arasword, zonharp)";
    const int a=f.spawn("arasword",2000,2000,0),b=f.spawn("arasword",2000,2060,0),first=f.spawn("zontrain",2000,2200,1);
    const int v1=f.spawn("zontrain",2420,1960,1),v2=f.spawn("zontrain",2420,2060,1);
    const int c1=f.spawn("zonharp",2620,1960,0),c2=f.spawn("zonharp",2620,2060,0);
    if (!a || !b || !first || !v1 || !v2 || !c1 || !c2) return;
    for (int id:{a,b}) command(w,reg,Cmd::Attack,0,id,first,ta::net::kQueueReplace);
    command(w,reg,Cmd::Attack,0,a,v1,ta::net::kQueueAppend);
    command(w,reg,Cmd::Attack,0,b,v2,ta::net::kQueueAppend);
    f.run(30);
    check(names(*w.unit(a),first) && names(*w.unit(a),v1) && names(*w.unit(b),first) && names(*w.unit(b),v2),
          tag+": each attacker holds a queued attack on its own unit before the charms");
    // Hold Fire: each Harpy charms the unit it is ordered onto and nothing
    // else. The first target and both units must outlast the charms.
    w.unit(c1)->fireState=0;w.unit(c2)->fireState=0;
    command(w,reg,Cmd::Attack,0,c1,v1);command(w,reg,Cmd::Attack,0,c2,v2);
    const auto ours=[&](int id) { const Unit* u=w.unit(id);return u && u->alive() && u->player==0; };
    for (int t=0;t<1800 && !(ours(v1) && ours(v2));++t) {
        for (int id:{first,v1,v2}) if (Unit* k=w.unit(id);k && k->alive()) k->hp=Fixed::fromInt(int(k->type->maxHp));
        f.run(1);
    }
    const bool took=ours(v1) && ours(v2);
    check(took,tag+": both charms take");
    if (!took) return;
    check(!anyoneNames(w,v1) && !anyoneNames(w,v2) && names(*w.unit(a),first) && names(*w.unit(b),first) &&
          w.unit(a)->orders.size()==2 && w.unit(b)->orders.size()==2,
          tag+": both queued legs let go at the charms and the current attacks go on");
    const LegionNavigator* legion=w.legionNavigator();
    const auto chasing=[&](int id) {
        const LegionMission m=legion->mission(*w.unit(id));
        return m==LegionMission::Attack || m==LegionMission::Guard;
    };
    check(legion && chasing(a) && chasing(b),tag+": the attacks on the first target are Legion's attack legs");
    if (!legion) return;
    w.scenarioDestroy(first);
    bool chase=false,shared=false;
    for (int t=0;t<60;++t) {
        f.run(1);
        chase|=chasing(a) || chasing(b);
        const int ga=legion->unitGroup(a),gb=legion->unitGroup(b);
        if (ga && ga==gb) std::printf("       tick %d: both attackers are in Legion group %d\n",t,ga);
        shared|=ga && ga==gb;
    }
    check(!chase,tag+": a leg let go at a change of owner is no attack leg of Legion's when its turn comes");
    check(!shared,tag+": the holders of two different charmed units share no Legion group");
    check(w.unit(a)->orders.empty() && w.unit(b)->orders.empty() && ours(v1) && ours(v2),
          tag+": both queues end and the charmed units live");
}

void rosterCases(const TypeRegistry& reg) {
    for (PathfindingMode mode:{PathfindingMode::Retail,PathfindingMode::Legion}) {
        selectionCase(reg,mode,{"arasword","araarch","zongryp"},"zontrain","zonharp",0);
        selectionCase(reg,mode,{"arasword","araarch"},"araknigh","zonharp",2);
        selectionCase(reg,mode,{"zontroll","zonter","zongryp"},"araknigh","zonharp",0);   // one click on a Zhon army
        queuedCase(reg,mode,"arasword","tarmind");
        queuedCase(reg,mode,"zontroll","zonharp");
        rallyCase(reg,mode);
        guardCase(reg,mode);
        thirdPlayerCase(reg,mode);
        friendlyFireCase(reg,mode);
    }
    sharedIdCase(reg);
}
}

int main(int argc,char** argv) {
    if (argc>2) return 2;
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    syntheticCases();
    if (argc==2) {
        auto vfs=ta::hpi::mountRetailRoot(argv[1]);
        TypeRegistry registry;setupRegistry(registry,vfs);
        rosterCases(registry);
    }
    std::printf("%s: %d failure(s)\n",failures ? "FAIL" : "PASS",failures);
    return failures ? 1 : 0;
}
