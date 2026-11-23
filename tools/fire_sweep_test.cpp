// The fire-at-will sweep's companions: deterministic per-player vision (icd
// 4f7210 / 4c6800 / 411700), AI weapon switching (40f411..40fb92) and the
// idle retaliation mission (409a50 part a, 4d7de0), the target score's
// terrain-scaled turn rate (412a77..412ae2), and enemies with construction
// left (411700, 412620, 40f330). One ctest per part:
//   fire_sweep_test vision | switch | retaliation | turnrate | sites
// `fire_sweep_test --picker` is the oracle tools/re/check_fire_sweep_picker.py
// compares with retail's target picker 412620; `--scan` is
// tools/re/check_mission_scan.py's (the mission scan 4d8370) and `--gate`
// tools/re/check_issue_gate.py's (the attack-issuing gate 4d7de0) and
// `--guard` tools/re/check_guard_nomove.py's (Guard_NoMove's slot release
// 401a90).
#include "sim/sim.h"
#include "sim/retailcost.h"
#include "sim/retailexploration.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <algorithm>
#include <cmath>
using namespace tak;
namespace {
int failures=0;
void check(bool ok,const char* label) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",label);failures+=!ok;
}
constexpr int kMap=128;
void setup(sim::World& w,int players) {
    w.setSerialThreads(true);w.setPathfindingMode(sim::PathfindingMode::Retail);
    w.setTerrain(std::vector<uint8_t>(kMap*kMap,100),kMap,kMap,20);
    w.setPathService(true);w.setPlayerCount(players);
}
void ticks(sim::World& w,int n) {for(int i=0;i<n;++i)w.tick(1.f/30);}
sim::UnitType gunner(int sight) {
    sim::UnitType t;t.id="gunner";t.canMove=true;t.upright=true;t.maxHp=500;t.footX=t.footZ=2;
    t.maxVel=sim::Fixed::fromInt(2);t.turnRate=t.turnInPlaceRate=600;t.sight=sight;t.sightHeight=24;
    sim::Weapon w;w.name="bolt";w.range=300;w.reload=0.3f;w.damage=10;w.projVel=900;w.aimTol=33000;
    t.weapon=w;t.weapons={w};return t;
}
sim::UnitType dummy() {
    sim::UnitType t;t.id="dummy";t.maxHp=5000;t.footX=t.footZ=2;t.maxVel=sim::Fixed::fromInt(1);
    t.canMove=true;t.upright=true;t.sight=40;t.sightHeight=24;return t;
}

// The view counters must equal a from-scratch sum of the active footprints.
bool countsMatchRebuild(const sim::World& w) {
    const int width=kMap/2,height=kMap/2;
    const auto heights=sim::retailExplorationHeights(kMap,kMap,20,[](int,int){return 100;});
    std::vector<uint16_t> expect(w.viewCounts().size(),0);
    if (expect.empty()) return false;
    const size_t cells=size_t(width)*height;
    for (const auto& u:w.units()) {
        if (!u.viewSight.active) continue;
        auto sight=u.viewSight;sight.active=false;
        sim::retailSightFootprint(sight,true,false,width,height,uint8_t(u.viewPlayer),
            [&](int x,int z){return heights[size_t(z)*width+x];},
            [&](int x,int z,int d,uint16_t){
                for (int p=0;p<16;++p)
                    if (u.viewPlanes>>p & 1) expect[size_t(p)*cells+size_t(z)*width+x]+=uint16_t(d);
            });
    }
    return expect==w.viewCounts();
}

void vision() {
    const auto g=gunner(40);const auto scout=gunner(160);const auto d=dummy();
    sim::World w;setup(w,2);
    const int shooter=w.spawn(&g,400,1000,0,0),foe=w.spawn(&d,650,1000,0,1);
    w.unit(shooter)->fireState=0;   // keep the order-driven acquisition out of it
    bool consistent=true;
    for(int i=0;i<12;++i){ticks(w,1);consistent=consistent && countsMatchRebuild(w);}
    check(!w.playerSees(0,*w.unit(foe)),"vision: an enemy beyond the player's sight is unseen");
    check(w.playerSees(1,*w.unit(foe)) && w.playerSees(0,*w.unit(shooter)),"vision: a player sees its own units");
    check(w.visibleEnemies(0).empty(),"vision: the unseen enemy is not on the visible-enemy list");
    check(w.retailSelectTarget(*w.unit(shooter),0,true)==0,"vision: the sweep does not select an enemy its player cannot see");
    // A scout walks up: the enemy becomes seen at once, and a target within
    // one 7-tick list rebuild.
    const int spotter=w.spawn(&scout,650,1070,0,0);
    w.unit(spotter)->fireState=0;
    ticks(w,1);consistent=consistent && countsMatchRebuild(w);
    check(w.playerSees(0,*w.unit(foe)),"vision: an enemy inside another own unit's footprint is seen");
    int picked=0,waited=0;
    for(;waited<8 && !picked;++waited){
        picked=w.retailSelectTarget(*w.unit(shooter),0,true);
        if(!picked){ticks(w,1);consistent=consistent && countsMatchRebuild(w);}
    }
    check(picked==foe,"vision: once seen, the enemy is selected within one list rebuild");
    // Cloak hides an enemy, never an own unit.
    w.unit(foe)->cloaked=true;w.unit(shooter)->cloaked=true;
    check(!w.playerSees(0,*w.unit(foe)) && w.playerSees(0,*w.unit(shooter)),"vision: cloak hides enemies, never own units");
    w.unit(foe)->cloaked=false;w.unit(shooter)->cloaked=false;
    // The scout leaves: the enemy is unseen again; the snapshot may hold it
    // for up to six more ticks, then it drops.
    w.order(spotter,200,1700,false);
    int lost=-1;
    for(int i=0;i<300;++i){ticks(w,1);consistent=consistent && countsMatchRebuild(w);
        if(lost<0 && !w.playerSees(0,*w.unit(foe)))lost=i;
        if(lost>=0 && i>=lost+8)break;}
    check(lost>=0 && w.visibleEnemies(0).empty(),"vision: an enemy that left sight drops off the list at the next rebuild");
    check(consistent,"vision: the view counters equal a rebuild from the footprints every tick");
}
// shootme: a human-owned shooter's sweep skips types without it; an AI-owned
// shooter picks them anyway (icd 412620).
void shootMe() {
    const auto g=gunner(400);auto d=dummy();d.shootMe=false;
    for (bool ai:{false,true}) {
        sim::World w;setup(w,2);w.player(0).automaticGates=ai;
        const int shooter=w.spawn(&g,400,1000,0,0),foe=w.spawn(&d,600,1000,0,1);
        w.unit(shooter)->fireState=0;
        int picked=0;
        for(int i=0;i<8 && !picked;++i){ticks(w,1);picked=w.retailSelectTarget(*w.unit(shooter),0,true);}
        check(ai ? picked==foe : picked==0,ai ? "shootme: an AI-owned shooter picks a type without shootme"
                                              : "shootme: a human-owned shooter does not pick a type without shootme");
    }
    {   // An explicit attack order is never gated by shootme.
        sim::World w;setup(w,2);
        const int shooter=w.spawn(&g,400,1000,0,0),foe=w.spawn(&d,600,1000,0,1);
        w.attack(shooter,foe,false);
        const auto hp=w.unit(foe)->hp;
        ticks(w,90);
        check(w.unit(foe)->hp<hp,"shootme: a human-ordered attack still hits a type without shootme");
    }
}
} // namespace

void weaponSwitch();
void retaliation();
void turnRate();
void sites();
int pickerOracle();
int scanOracle();
int gateOracle();
int guardOracle();

int main(int argc,char** argv) {
    const std::string part=argc>1 ? argv[1] : "all";
    if(part=="--picker")return pickerOracle();
    if(part=="--scan")return scanOracle();
    if(part=="--gate")return gateOracle();
    if(part=="--guard")return guardOracle();
    if(part=="vision" || part=="all")vision();
    if(part=="vision" || part=="all")shootMe();
    if(part=="switch" || part=="all")weaponSwitch();
    if(part=="retaliation" || part=="all")retaliation();
    if(part=="turnrate" || part=="all")turnRate();
    if(part=="sites" || part=="all")sites();
    return failures?1:0;
}
void weaponSwitch() {
    using sim::RetailWeaponSwitchInput;
    auto row=[](int e,int f,int rnd,int* draws){
        return std::tuple{[e](int){return e;},[f](int){return f;},[rnd,draws](int){++*draws;return rnd;}};
    };
    auto run=[&](const RetailWeaponSwitchInput& in,int e,int f,int rnd=5,int* drawsOut=nullptr){
        int draws=0;auto [E,F,R]=row(e,f,rnd,&draws);
        const int c=sim::retailWeaponSwitchChoice(in,E,F,R);
        if(drawsOut)*drawsOut=draws;
        return c;
    };
    RetailWeaponSwitchInput base;base.target=true;base.selected=0;base.targetHp=500;
    base.slots[0]={true,true,true,0,false,false,60};
    {   auto in=base;in.slots[1]={true,true,true,40,false,false,30};
        check(run(in,1,1)==1,"switch: an affordable aoe<=50 slot is taken outright");
        in.targetHp=121;check(run(in,1,1)==-1,"switch: the primary is kept when it does half the target's HP");
        in.targetHp=122;check(run(in,1,1)==1,"switch: ...and dropped just above that");
        in.slots[1].affordable=false;in.targetHp=500;check(run(in,1,1)==-1,"switch: an unaffordable slot is skipped");
    }
    {   auto in=base;in.slots[2]={true,true,true,250,false,true,0};
        check(run(in,2,0)==-1,"switch: aoe>=200 needs more than two enemies");
        check(run(in,3,0)==2,"switch: aoe>=200 with three enemies and no allies");
        check(run(in,3,1)==-1,"switch: aoe>=200 refuses unless allies are outnumbered 3:1");
        in.lowMonarch=true;check(run(in,3,1)==2,"switch: a low Monarch ignores its allies");
    }
    {   auto in=base;in.slots[1]={true,true,true,100,false,false,0};
        check(run(in,5,1)==-1 && run(in,6,1)==1,"switch: medium aoe needs allies outnumbered 5:1");
        in.slots[1].mindControl=true;in.slots[1].aoe=40;in.targetHp=10;
        check(run(in,1,1)==1,"switch: a mind-control slot overrides the primary-finishes-it rule");
    }
    {   auto in=base;in.selected=1;in.slots[1]={true,true,false,40,false,false,0};
        check(run(in,1,1)==0,"switch: from slot 1 it returns straight to the primary");
    }
    {   auto in=base;in.target=false;in.selected=2;in.slots[2]={true,true,true,250,false,true,0};
        int draws=0;
        check(run(in,0,0,0,&draws)==0 && draws==1,"switch: no target, costly slot: back to 0 on rand(10)==0");
        check(run(in,0,0,3,&draws)==-1 && draws==1,"switch: ...and kept otherwise (one draw)");
        in.slots[2].costlyOrSpecial=false;
        check(run(in,0,0,0,&draws)==-1 && draws==0,"switch: no draw for a cheap ordinary slot");
    }
    // World: an AI switcher facing a clump switches to its splash slot; a
    // human-owned one keeps the primary.
    auto caster=[]{
        auto t=gunner(400);t.id="caster";t.weaponSwitching=true;
        sim::Weapon a=t.weapons[0],b=a,c=a;
        b.name="ball";b.aoe=150;c.name="wave";c.aoe=250;
        t.weapons={a,b,c};t.weapon=a;return t;
    }();
    const auto d=dummy();
    for(bool ai:{true,false}) {
        sim::World w;setup(w,2);w.player(0).automaticGates=ai;
        const int id=w.spawn(&caster,400,1000,0,0);
        for(int i=0;i<4;++i)w.spawn(&d,600+40*(i%2),960+40*(i/2),0,1);
        int slot=0;
        for(int t=0;t<90 && !slot;++t){ticks(w,1);slot=w.unit(id)->weaponSlot;}
        if(ai)check(slot==2,"switch: an AI switcher next to an enemy clump takes its splash weapon");
        else check(slot==0,"switch: a human-owned switcher never auto-switches");
    }
}
void retaliation() {
    auto victimType=[]{auto t=gunner(160);t.id="victim";t.leash=300;return t;}();
    auto raider=[](int range){auto t=gunner(160);t.id="raider";t.maxVel=sim::Fixed::fromInt(4);
        t.weapons[0].range=range;t.weapon=t.weapons[0];return t;};
    const auto r1=raider(500),r2=raider(700);
    struct Hit { int victim=0,attacker=0; bool hit=false; sim::Fixed hitX,hitZ; };
    // Runs until the victim is first hit; returns where it stood.
    auto firstHit=[&](sim::World& w,Hit& h,int limit) {
        for(int t=0;t<limit && !h.hit;++t){
            const sim::Unit& v=*w.unit(h.victim);
            const sim::Fixed x=v.x,z=v.z,hp=v.hp;
            ticks(w,1);
            if(w.unit(h.victim)->hp<hp){h.hit=true;h.hitX=x;h.hitZ=z;}
        }
    };
    {   // 1: idle Offensive mobile, hit from 1.5x range inside 0.9 leash + range.
        sim::World w;setup(w,2);Hit h;
        h.victim=w.spawn(&victimType,400,1000,0,0);h.attacker=w.spawn(&r1,850,1000,0,1);
        auto& v=*w.unit(h.victim);v.moveState=1;v.fireState=2;
        w.attack(h.attacker,h.victim,false);
        firstHit(w,h,300);
        const auto& o=w.unit(h.victim)->orders;
        const bool attack=!o.empty() && o[0].targetId==h.attacker && o[0].autoTarget && o[0].chaseLeashed &&
            o[0].chaseLeash==300 && o[0].chaseOrigin.x==int16_t(w.unit(h.victim)->x.floorInt());
        // Behind the attack leg (its route waypoints share its target).
        const auto ret=std::find_if(o.begin(),o.end(),[](const sim::Order& x){return !x.targetId;});
        const bool back=ret!=o.end() && ret->groundMission && ret->groundResponse.mode==1;
        check(h.hit && attack && back,"retaliation: an idle Offensive unit takes a leashed attack with a return move");
        const int16_t originX=o.empty() ? 0 : o[0].chaseOrigin.x;
        // The attacker flees past the leash: the attack retires, the unit walks home.
        w.order(h.attacker,1900,1000,false);w.unit(h.attacker)->fireState=0;
        float farthest=0;bool retired=false,home=false;
        for(int t=0;t<1200;++t){
            ticks(w,1);
            const auto& u=*w.unit(h.victim);
            farthest=std::max(farthest,u.x.toFloat()-originX);
            if(!retired && (u.orders.empty() || u.orders[0].targetId!=h.attacker))retired=true;
            if(retired && u.orders.empty()){home=std::abs(u.x.toFloat()-originX)<48;break;}
        }
        check(retired && farthest>=250 && farthest<360,"retaliation: the chase retires at the leash");
        check(home,"retaliation: the unit then walks back to where it was hit");
    }
    {   // 2: idle Defensive: no mission.
        sim::World w;setup(w,2);Hit h;
        h.victim=w.spawn(&victimType,400,1000,0,0);h.attacker=w.spawn(&r1,850,1000,0,1);
        auto& v=*w.unit(h.victim);v.moveState=0;v.fireState=2;
        w.attack(h.attacker,h.victim,false);
        firstHit(w,h,300);
        check(h.hit && w.unit(h.victim)->orders.empty(),"retaliation: an idle Defensive unit gets no mission");
    }
    {   // 3: patrolling Defensive with fire at will: attack + return, then the patrol resumes.
        sim::World w;setup(w,2);Hit h;
        h.victim=w.spawn(&victimType,400,1000,0,0);h.attacker=w.spawn(&r1,880,1000,0,1);
        auto& v=*w.unit(h.victim);v.moveState=0;v.fireState=2;
        w.patrol(h.victim,400,1300);
        w.attack(h.attacker,h.victim,false);
        firstHit(w,h,300);
        const auto& o=w.unit(h.victim)->orders;
        const bool attack=!o.empty() && o[0].targetId==h.attacker && o[0].chaseLeashed;
        const auto ret=std::find_if(o.begin(),o.end(),[](const sim::Order& x){return !x.targetId;});
        const bool back=ret!=o.end() && ret->groundMission && ret->groundResponse.mode==1 && !ret->patrol;
        const bool patrolKept=std::any_of(o.begin(),o.end(),[](const sim::Order& x){return x.patrol;});
        check(h.hit && attack && back && patrolKept,"retaliation: a patrolling Defensive unit attacks with a return and keeps its patrol");
    }
    {   // 4: attacker beyond 0.9 leash + range: no mission.
        sim::World w;setup(w,2);Hit h;
        h.victim=w.spawn(&victimType,400,1000,0,0);h.attacker=w.spawn(&r2,1050,1000,0,1);
        auto& v=*w.unit(h.victim);v.moveState=1;v.fireState=2;
        w.attack(h.attacker,h.victim,false);
        firstHit(w,h,400);
        check(h.hit && w.unit(h.victim)->orders.empty(),"retaliation: an attacker beyond 0.9 x leash + range gets no mission");
    }
    {   // 5: 409b56..409b67: an armed building still under construction skips
        // both the retaliation mission (a) and the slot handoff (b); the same
        // building finished answers. The attacker stands outside the tower's
        // sight, so the sweep cannot pick it: only the hit response can.
        auto tower=gunner(160);tower.id="tower";tower.canMove=false;tower.maxVel=sim::Fixed();
        tower.buildTime=1000;   // an unattended site decays slowly instead of vanishing
        for (const bool site:{true,false}) {
            sim::World w;setup(w,2);Hit h;
            h.victim=w.spawn(&tower,400,1000,0,0);h.attacker=w.spawn(&r1,650,1000,0,1);
            auto& v=*w.unit(h.victim);v.fireState=2;v.underConstruction=site;
            w.attack(h.attacker,h.victim,false);
            // A site's HP moves without a hit (no builder): watch the hit itself.
            for (int t=0;t<300 && !h.hit && w.unit(h.victim);++t) {
                ticks(w,1);
                h.hit=w.unit(h.victim) && w.unit(h.victim)->lastHitBy==h.attacker;
            }
            if (!w.unit(h.victim)) {check(false,"retaliation: the building survives its first hit");continue;}
            const auto& u=*w.unit(h.victim);
            const bool answered=(!u.orders.empty() && u.orders[0].targetId==h.attacker) ||
                u.slotTarget[0]==h.attacker;
            if (site) check(h.hit && u.orders.empty() && u.slotTarget[0]==0,
                            "retaliation: an unfinished building under fire takes no target");
            else check(h.hit && answered,"retaliation: the finished building answers the same hit");
        }
    }
}

// 412a77..412ae2 on a synthetic unit (UnitDef +0x18e turnrate, +0x16e/+0x172
// water/road multiplier, mover +0x36 flags), emulated in the retail binary.
void turnRate() {
    struct Case { uint16_t turn; int32_t road,water; uint16_t flags; uint16_t want; const char* label; };
    const Case cases[]={
        {300,0x13333,0x10000,0,     300,"turnrate: off-road dry ground keeps the type's rate"},
        {300,0x13333,0x10000,0x800, 359,"turnrate: road bit scales by roadmultiplier, truncated"},
        {300,0x13333,0x8000, 0x1000,150,"turnrate: water bit scales by watermultiplier"},
        {300,0x13333,0x8000, 0x1800,359,"turnrate: road wins over water"},
        {455,0x18000,0x6666, 0x800, 682,"turnrate: 1.5 road"},
        {455,0x18000,0x6666, 0x1000,181,"turnrate: 0.4 water truncates toward zero"},
        {333,0x15555,0xaaaa, 0x1000,221,"turnrate: 2/3 water"},
        {999,0x10001,0,      0x800, 999,"turnrate: just over 1.0 road"},
        {1,  0x8000, 0,      0x800, 0,  "turnrate: a product under 1 truncates to 0"},
        {0,  0x13333,0,      0x800, 0,  "turnrate: zero stays zero"},
        {700,-0x8000,0x10000,0x800, 65186,"turnrate: a negative multiplier wraps in 16 bits"},
        {900,0x640000,0,     0x800, 24464,"turnrate: an overflowing product keeps the low 16 bits"},
        {65535,0x20000,0,    0x1000,0,"turnrate: a zero water multiplier gives 0"},
    };
    for(const Case& c:cases)
        check(sim::retailEffectiveTurnRate(c.turn,c.road,c.water,c.flags)==c.want,c.label);
    // In the sweep: a 600 turnrate pays the two penalty draws; on a 2.0 road
    // it turns at 1200 and pays none, in water at 0.5 it still pays them.
    auto draws=[](int32_t road,int32_t water,uint16_t flags) {
        auto g=gunner(400);g.roadMult=sim::Fixed::raw(road);g.waterMult=sim::Fixed::raw(water);
        const auto d=dummy();
        sim::World w;setup(w,2);
        const int shooter=w.spawn(&g,400,1000,0,0);w.spawn(&d,400,800,0,1);
        w.unit(shooter)->fireState=0;
        int picked=0;
        for(int i=0;i<8 && !picked;++i){ticks(w,1);picked=w.retailSelectTarget(*w.unit(shooter),0,true);}
        if(!picked)return -1;
        int n=0;w.setRngObserver([&](const sim::World::RngObservation&){++n;});
        w.unit(shooter)->groundTerrainFlags=flags;
        return w.retailSelectTarget(*w.unit(shooter),0,true) ? n : -1;
    };
    const int base=draws(0x20000,0x8000,0);
    check(base>0 && draws(0x20000,0x8000,0x800)==base-2,"turnrate: a 2.0 road lifts the sweep's rate past the penalty");
    check(base>0 && draws(0x20000,0x8000,0x1000)==base,"turnrate: 0.5 water keeps the penalty draws");
}

// Enemies with construction left (a site still being conjured, a unit still
// being built at a factory door). Retail lists them on +5 (411700 drops only
// the scenario flag 0x2000), its picker offers them with the score x10
// (412620, 412b52..412b77), its sweep skips its own ones for free (40f3a1),
// and its AI weapon switch takes no stone, freeze or charm slot against one
// (40f50e..40f5ea). tools/re/probe_site_targets.py and
// tools/re/check_fire_sweep_picker.py hold the emulated evidence.
sim::UnitType siteType() {
    sim::UnitType t;t.id="keep";t.maxHp=3000;t.footX=t.footZ=4;t.sight=40;t.sightHeight=24;
    t.buildTime=100000;   // an unattended site decays slowly instead of vanishing
    return t;
}
void sites() {
    const auto keep=siteType();const auto d=dummy();
    auto site=[&](sim::World& w,float x,float z){
        const int id=w.spawn(&keep,x,z,0,1);w.unit(id)->underConstruction=true;return id;
    };
    {   // The list and the sweep: a site in sight is listed, and alone in range
        // the sweep picks it.
        const auto g=gunner(400);
        sim::World w;setup(w,2);
        const int shooter=w.spawn(&g,400,1000,0,0);
        const int s=site(w,600,1000);
        w.unit(shooter)->fireState=0;   // keep the order-driven acquisition out of it
        ticks(w,8);
        const auto ve=w.visibleEnemies(0);
        check(std::find(ve.begin(),ve.end(),s)!=ve.end(),"sites: an enemy site in sight is on the visible-enemy list");
        check(w.retailSelectTarget(*w.unit(shooter),0,true)==s,"sites: the sweep picks a lone site in range");
    }
    // A tower: no turn penalty in the score, so only distance and the x10 count.
    auto tower=gunner(400);tower.id="tower";tower.canMove=false;tower.maxVel=sim::Fixed();
    {   // The x10: a finished enemy twice as far as a site is still taken about
        // nine times in ten (one in fifty with no x10).
        sim::World w;setup(w,2);
        const int shooter=w.spawn(&tower,400,1000,0,0);
        const int s=site(w,500,1000),foe=w.spawn(&d,400,1200,0,1);
        w.unit(shooter)->fireState=0;
        ticks(w,8);
        int siteN=0,foeN=0;
        for (int i=0;i<400;++i) {
            const int picked=w.retailSelectTarget(*w.unit(shooter),0,true);
            siteN+=picked==s;foeN+=picked==foe;
        }
        std::printf("  site %d finished %d of 400\n",siteN,foeN);
        check(siteN>=10 && siteN<=100 && siteN+foeN==400,"sites: the picker takes a site behind a finished target (x10), not never");
    }
    {   // Paralysis is no x10: a paralyzed enemy and a healthy one at the same
        // distance are taken about equally often.
        sim::World w;setup(w,2);
        const int shooter=w.spawn(&tower,400,1000,0,0);
        const int still=w.spawn(&d,600,1000,0,1),foe=w.spawn(&d,400,1200,0,1);
        w.unit(shooter)->fireState=0;
        ticks(w,8);
        w.unit(still)->paralyzedFor=3000;
        int stillN=0,foeN=0;
        for (int i=0;i<400;++i) {
            const int picked=w.retailSelectTarget(*w.unit(shooter),0,true);
            stillN+=picked==still;foeN+=picked==foe;
        }
        std::printf("  paralyzed %d healthy %d of 400\n",stillN,foeN);
        check(stillN>=140 && foeN>=140 && stillN+foeN==400,"sites: a paralyzed target is scored like a healthy one");
    }
    {   // Stone, freeze and charm classes never take one (4127eb..4128db).
        for (int c=0;c<3;++c) {
            auto g=gunner(400);
            g.weapons[0].status=c==0 ? sim::Weapon::Status::Stoned : c==1 ? sim::Weapon::Status::Frozen :
                                                                            sim::Weapon::Status::None;
            g.weapons[0].mindControl=c==2;g.weapon=g.weapons[0];
            sim::World w;setup(w,2);
            const int shooter=w.spawn(&g,400,1000,0,0);
            site(w,600,1000);
            w.unit(shooter)->fireState=0;
            ticks(w,8);
            check(w.retailSelectTarget(*w.unit(shooter),0,true)==0,
                  c==0 ? "sites: a stone class does not pick a site" : c==1 ? "sites: a freeze class does not pick a site"
                                                                         : "sites: a charm class does not pick a site");
        }
    }
    {   // Idle (Standby's scan, 4d8370, through the engine's acquisition): an
        // idle armed unit beside an enemy site attacks it.
        const auto g=gunner(300);
        sim::World w;setup(w,2);
        const int u=w.spawn(&g,400,1000,0,0);
        const int s=site(w,650,1000);
        bool attacked=false,hit=false;
        for (int t=0;t<240 && !hit;++t) {
            ticks(w,1);
            const auto& o=w.unit(u)->orders;
            attacked=attacked || (!o.empty() && o[0].targetId==s && o[0].autoTarget);
            hit=w.unit(s) && w.unit(s)->lastHitBy==u;
        }
        check(attacked && hit,"sites: an idle armed unit attacks an enemy site in reach");
    }
    {   // Behind finished targets: at the same distance the idle unit takes the
        // finished one; with the site four times nearer (16 > 10) the site is
        // the likelier pick of Standby's scan (4d8370: 412620's sampled score,
        // not nearest first), and the finished enemy still comes up. A turn
        // rate of 1000 leaves out the turn penalty (412a77), which is added
        // before the x10 and would put the site, off the gunner's bearing,
        // behind the enemy ahead of it.
        for (const bool near:{false,true}) {
            auto g=gunner(300);
            if (near) g.turnRate=1000;
            sim::World w;setup(w,2);
            const int u=w.spawn(&g,400,1000,0,0);
            const int s=site(w,near ? 450 : 600,1000),foe=w.spawn(&d,400,1200,0,1);
            int first=0;
            for (int t=0;t<120 && !first;++t) {
                ticks(w,1);
                const auto& o=w.unit(u)->orders;
                if (!o.empty() && o[0].targetId) first=o[0].targetId;
            }
            if (near) {
                int siteN=0,foeN=0;
                for (int i=0;i<400;++i) {
                    const int picked=w.retailMissionScan(*w.unit(u));
                    siteN+=picked==s;foeN+=picked==foe;
                }
                std::printf("  near site %d finished %d of 400\n",siteN,foeN);
                check(first!=0 && siteN>foeN && foeN>0 && siteN+foeN==400,
                      "sites: the scan takes a site four times nearer than a finished enemy more often");
            } else check(first==foe,"sites: an idle unit takes a finished enemy before a site as near");
        }
    }
    {   // A guard runs no scan of its own (Follow_Ground): it keeps its ward and
        // shoots a site in range from its auto slots.
        const auto g=gunner(300);
        sim::World w;setup(w,2);
        const int ward=w.spawn(&d,400,1000,0,0),u=w.spawn(&g,400,1060,0,0);
        const int s=site(w,600,1060);
        w.guard(u,ward,false);
        bool left=false,hit=false;
        for (int t=0;t<240;++t) {
            ticks(w,1);
            const auto& o=w.unit(u)->orders;
            left=left || o.empty() || o[0].targetId==s;
            hit=hit || (w.unit(s) && w.unit(s)->lastHitBy==u);
        }
        check(!left && hit,"sites: a guard keeps its ward and its auto slots shoot the site");
    }
    {   // A stone-class unit does not go for one either (its scanning slot).
        auto g=gunner(300);g.weapons[0].status=sim::Weapon::Status::Stoned;g.weapon=g.weapons[0];
        sim::World w;setup(w,2);
        const int u=w.spawn(&g,400,1000,0,0);
        site(w,650,1000);
        ticks(w,120);
        check(w.unit(u)->orders.empty(),"sites: an idle stone-class unit does not attack a site");
    }
    {   // The sweep's budget (40f3a1..40f3c9): with a cap of 30 it is two units
        // a call; an own unit with construction left costs nothing, a paralyzed
        // one costs one.
        sim::World w;setup(w,2);w.setUnitCap(30);
        const int a=w.spawn(&d,200,200,0,0);
        const int s1=w.spawn(&keep,300,200,0,0),s2=w.spawn(&keep,400,200,0,0);
        const int b=w.spawn(&d,500,200,0,0),p=w.spawn(&d,600,200,0,0),c=w.spawn(&d,700,200,0,0);
        w.spawn(&d,1500,1500,0,1);
        for (const int id:{s1,s2}) w.unit(id)->underConstruction=true;
        w.unit(p)->paralyzedFor=3000;
        ticks(w,1);
        const int first=w.player(0).sweepCursor;
        ticks(w,1);
        const int second=w.player(0).sweepCursor;
        std::printf("  cursor %d then %d (a %d b %d p %d c %d)\n",first,second,a,b,p,c);
        check(first==b,"sites: the sweep passes own sites at no cost");
        check(second==c,"sites: a paralyzed unit costs the sweep one");
    }
    {   // The AI weapon switch takes no stone slot against a site (an AI
        // switcher ordered onto one keeps its primary).
        auto caster=gunner(400);caster.id="caster";caster.weaponSwitching=true;
        sim::Weapon stone=caster.weapons[0];stone.name="stone";stone.status=sim::Weapon::Status::Stoned;
        caster.weapons={caster.weapons[0],stone};caster.weapon=caster.weapons[0];
        sim::World w;setup(w,2);w.player(0).automaticGates=true;
        const int u=w.spawn(&caster,400,1000,0,0);
        const int s=site(w,600,1000);
        w.attack(u,s,false);
        int slot=0;bool hit=false;
        for (int t=0;t<150;++t) {
            ticks(w,1);
            slot=std::max(slot,w.unit(u)->weaponSlot);
            hit=hit || (w.unit(s) && w.unit(s)->lastHitBy==u);
        }
        check(slot==0 && hit,"sites: an AI switcher on a site keeps its primary, no stone slot");
    }
}

// The oracle for tools/re/check_fire_sweep_picker.py. One case per stdin line:
//   seed sweep fawr melee cls ai sight range damage sx sz k para
//   {x z unfinished paralyzed bad immune pimmune}*k
// cls 0 plain, 1 stone, 2 freeze, 3 charm; para: the paralyzer damage type;
// pimmune: immunetoparalyzer. The shooter is a structure of
// player 0 (no turn penalty) and every candidate an enemy unit, listed on
// player 0's visible-enemy list in id order, in range of the shooter.
// Prints the picked candidate's index (-1 none) and the game RNG state after
// retailSelectTarget(shooter, slot 0, sweep).
int pickerOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        uint32_t seed=0;int sweep=0,fawr=0,melee=0,cls=0,ai=0,sight=0,range=0,damage=0,sx=0,sz=0,k=0,para=0;
        in>>seed>>sweep>>fawr>>melee>>cls>>ai>>sight>>range>>damage>>sx>>sz>>k>>para;
        sim::UnitType shooter;shooter.id="shooter";shooter.maxHp=500;shooter.footX=shooter.footZ=2;
        shooter.sight=sight;shooter.sightHeight=24;shooter.fireAtWillRandom=fawr!=0;
        shooter.badTargetCats[0]={"BAD"};
        sim::Weapon wp;wp.name="w";wp.range=range;wp.damage=damage;wp.reload=1;wp.melee=melee!=0;
        wp.status=cls==1 ? sim::Weapon::Status::Stoned : cls==2 ? sim::Weapon::Status::Frozen : sim::Weapon::Status::None;
        wp.mindControl=cls==3;wp.dmgType=para ? 4 : 1;
        shooter.weapon=wp;shooter.weapons={wp};
        // The eight candidate kinds: bad target or not, immune to the class
        // or not, immune to paralyzers or not.
        std::array<sim::UnitType,8> kinds;
        for (int b=0;b<8;++b) {
            // Mobile: a structure would block the engine's 2D clear-shot line
            // to the candidates behind it (retail has no such test).
            auto& t=kinds[size_t(b)];t=dummy();t.id="cand"+std::to_string(b);
            if (b&1) t.categories={"BAD"};
            t.cantBeStoned=t.cantBeFrozen=t.cantBeCaptured=(b&2)!=0;
            t.immuneToParalyzer=(b&4)!=0;
        }
        sim::World w;setup(w,2);w.player(0).automaticGates=ai!=0;
        const int shooterId=w.spawn(&shooter,float(sx),float(sz),0,0);
        struct Cand { int id,unfinished,paralyzed; };
        std::vector<Cand> cands;
        for (int i=0;i<k;++i) {
            int x=0,z=0,unfinished=0,paralyzed=0,bad=0,immune=0,pimmune=0;
            in>>x>>z>>unfinished>>paralyzed>>bad>>immune>>pimmune;
            cands.push_back({w.spawn(&kinds[size_t(bad+2*immune+4*pimmune)],float(x),float(z),0,1),unfinished,paralyzed});
        }
        w.unit(shooterId)->fireState=0;
        ticks(w,1);   // the spatial grid
        std::vector<int> listed;
        for (const auto& c:cands) {
            auto& u=*w.unit(c.id);
            u.underConstruction=c.unfinished!=0;u.paralyzedFor=c.paralyzed ? 300 : 0;
            listed.push_back(c.id);
        }
        std::sort(listed.begin(),listed.end());
        w.player(0).visibleEnemies=listed;
        w.setGameSeed(seed^0x66e29572u);   // retailSeed: the state is `seed` (odd)
        const int picked=w.retailSelectTarget(*w.unit(shooterId),0,sweep!=0);
        int index=-1;
        for (int i=0;i<k;++i) if (cands[size_t(i)].id==picked) index=i;
        std::printf("%d %u\n",index,w.gameRngState());
    }
    return 0;
}

// The oracle for tools/re/check_mission_scan.py. One case per stdin line:
//   seed fire armed switcher sel fawr ai kamikaze sight damage para sx sz
//   nslots {range melee}*nslots k {x z listed flyer shootme paralyzed pimmune}*k
// The shooter is a structure of player 0 (no turn penalty) with `nslots`
// weapons (none when unarmed); every candidate an enemy unit of player 1,
// on player 0's visible-enemy list when `listed`. Prints the picked
// candidate's index (-1 none) and the game RNG state after
// retailMissionScan(shooter).
int scanOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        uint32_t seed=0;
        int fire=0,armed=0,switcher=0,sel=0,fawr=0,ai=0,kamikaze=0,sight=0,damage=0,para=0,sx=0,sz=0,nslots=0,k=0;
        in>>seed>>fire>>armed>>switcher>>sel>>fawr>>ai>>kamikaze>>sight>>damage>>para>>sx>>sz>>nslots;
        sim::UnitType shooter;shooter.id="shooter";shooter.maxHp=500;shooter.footX=shooter.footZ=2;
        shooter.canMove=false;shooter.maxVel=sim::Fixed();
        shooter.sight=sight;shooter.sightHeight=24;shooter.fireAtWillRandom=fawr!=0;
        shooter.kamikaze=kamikaze!=0;shooter.weaponSwitching=switcher!=0;
        for (int s=0;s<nslots;++s) {
            int range=0,melee=0;in>>range>>melee;
            sim::Weapon wp;wp.name="w"+std::to_string(s);wp.range=range;wp.damage=uint16_t(damage);wp.reload=1;
            wp.melee=melee!=0;wp.dmgType=para ? 4 : 1;
            if (armed) shooter.weapons.push_back(wp);
        }
        if (!shooter.weapons.empty()) shooter.weapon=shooter.weapons[0];
        in>>k;
        struct Cand { int listed,flyer,paralyzed; };
        std::vector<Cand> spec;
        std::vector<std::pair<int,int>> at;
        std::vector<sim::UnitType> kinds;kinds.reserve(size_t(k));
        for (int i=0;i<k;++i) {
            int x=0,z=0,listed=0,flyer=0,shootme=0,paralyzed=0,pimmune=0;
            in>>x>>z>>listed>>flyer>>shootme>>paralyzed>>pimmune;
            auto t=dummy();t.id="cand"+std::to_string(i);t.shootMe=shootme!=0;t.immuneToParalyzer=pimmune!=0;
            kinds.push_back(t);spec.push_back({listed,flyer,paralyzed});at.push_back({x,z});
        }
        sim::World w;setup(w,2);w.player(0).automaticGates=ai!=0;
        const int shooterId=w.spawn(&shooter,float(sx),float(sz),0,0);
        std::vector<int> ids;
        for (int i=0;i<k;++i) ids.push_back(w.spawn(&kinds[size_t(i)],float(at[size_t(i)].first),float(at[size_t(i)].second),0,1));
        w.unit(shooterId)->fireState=0;
        ticks(w,1);   // the spatial grid
        auto& u=*w.unit(shooterId);
        u.fireState=fire;
        if (switcher) u.weaponSlot=sel;
        std::vector<int> listed;
        for (int i=0;i<k;++i) {
            auto& c=*w.unit(ids[size_t(i)]);
            c.flightGroundMode=spec[size_t(i)].flyer ? 2 : 1;
            c.paralyzedFor=spec[size_t(i)].paralyzed ? 300 : 0;
            if (spec[size_t(i)].listed) listed.push_back(c.id);
        }
        std::sort(listed.begin(),listed.end());
        w.player(0).visibleEnemies=listed;
        w.setGameSeed(seed^0x66e29572u);   // retailSeed: the state is `seed` (odd)
        const int picked=w.retailMissionScan(u);
        int index=-1;
        for (int i=0;i<k;++i) if (ids[size_t(i)]==picked) index=i;
        std::printf("%d %u\n",index,w.gameRngState());
    }
    return 0;
}

// The oracle for tools/re/check_issue_gate.py. One call per stdin line:
//   mover point stance fire explicit force x z leash
// A mover with a point is a walker (the ground missions' calls), a mover
// without one a flyer (the VTOL calls), a non-mover a building; the target
// an enemy unit. Prints the result, then the missions in the order the gate
// builds them: "return:<response mode>:<goal x>:<goal z>" and
// "attack:<auto>:<leash>:<origin x>:<origin z>" (leash and origin 0 when
// the attack is not leashed).
int gateOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        int mover=0,point=0,stance=0,fire=0,isExplicit=0,force=0,x=0,z=0,leash=0;
        in>>mover>>point>>stance>>fire>>isExplicit>>force>>x>>z>>leash;
        auto type=gunner(300);type.id="unit";type.leash=leash;
        if (!mover) { type.canMove=false;type.maxVel=sim::Fixed(); }
        else if (!point) { type.canFly=true;type.vtolStandby=true; }
        const auto d=dummy();
        sim::World w;setup(w,2);
        const int id=w.spawn(&type,float(x),float(z),0,0);
        const int foe=w.spawn(&d,float(x+200),float(z),0,1);
        auto& u=*w.unit(id);
        u.orders.clear();u.moveState=stance;u.fireState=fire;
        using MA=sim::World::MissionAttack;
        const MA how=isExplicit ? MA::Forced : point ? (force ? MA::Patrol : MA::Standby)
                                                     : (force ? MA::VtolPatrol : MA::Vtol);
        const bool issued=w.retailMissionAttack(u,foe,how);
        std::string out=issued ? "1" : "0";
        if (issued) {
            const auto& o=u.orders;
            if (o.size()>1 && o[1].targetId==0) {
                const int mode=o[1].groundMission ? o[1].groundResponse.mode : 0;
                out+=" return:"+std::to_string(mode)+":"+std::to_string(o[1].x.floorInt())+":"+
                     std::to_string(o[1].z.floorInt());
            }
            const auto& a=o.front();
            out+=" attack:"+std::to_string(a.autoTarget ? 1 : 0)+":"+std::to_string(a.chaseLeashed ? a.chaseLeash : 0)+
                 ":"+std::to_string(a.chaseLeashed ? a.chaseOrigin.x : 0)+":"+std::to_string(a.chaseLeashed ? a.chaseOrigin.z : 0);
        }
        std::printf("%s\n",out.c_str());
    }
    return 0;
}

// One row per line: slots switcher selected bits draw, then per slot its
// automatic flag and target state (0 none, 1 inside the minimum range, 2 out
// of reach, 3 good). Answers per slot the final automatic flag and whether
// its target was cleared, then the event word's slot bits left.
int guardOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        int slots=0,switcher=0,selected=0,draw=0;uint32_t bits=0;
        in>>slots>>switcher>>selected>>bits>>draw;
        sim::UnitType tower;tower.id="tower";tower.maxHp=500;tower.footX=tower.footZ=2;
        tower.canMove=false;tower.maxVel=sim::Fixed();tower.sight=400;tower.sightHeight=24;
        tower.weaponSwitching=switcher!=0;
        for (int s=0;s<slots;++s) {
            sim::Weapon wp;wp.name="w"+std::to_string(s);wp.range=300;wp.minRange=100;wp.damage=10;wp.reload=1;
            tower.weapons.push_back(wp);
        }
        if (!tower.weapons.empty()) tower.weapon=tower.weapons[0];
        const auto d=dummy();
        sim::World w;setup(w,2);
        const int id=w.spawn(&tower,1000,1000,0,0);
        static constexpr float kDist[4]={0,50,450,200};
        static constexpr float kDir[3][2]={{1,0},{0,1},{-1,0}};
        std::array<int,3> before{};
        for (int s=0;s<slots;++s) {
            int automatic=0,state=0;in>>automatic>>state;
            auto& u=*w.unit(id);
            u.slotAuto=automatic ? uint8_t(u.slotAuto|1u<<s) : uint8_t(u.slotAuto&~(1u<<s));
            if (state) {
                const int foe=w.spawn(&d,1000+kDir[s][0]*kDist[state],1000+kDir[s][1]*kDist[state],0,1);
                w.unit(id)->slotTarget[size_t(s)]=foe;before[size_t(s)]=foe;
            } else w.unit(id)->slotTarget[size_t(s)]=0;
        }
        auto& u=*w.unit(id);
        u.weaponSlot=selected;u.missionEvents=bits;
        w.retailGuardNoMoveSlots(u,draw);
        std::string out;
        for (int s=0;s<slots;++s)
            out+=std::to_string(u.slotAuto>>s&1)+":"+std::to_string(before[size_t(s)] && !u.slotTarget[size_t(s)] ? 1 : 0)+" ";
        out+=std::to_string(u.missionEvents&0xe00000u);
        std::printf("%s\n",out.c_str());
    }
    return 0;
}
