// Groups in Retail mode. TotalA.exe keeps no group records: its mission rows
// (0x4fc6e8 ground, 0x4fca18 air) carry none of Kingdoms' group bits, it has
// no formation or step-out mission, and its moves, patrols, standbys and
// guards (Move_Ground 0x4031d0, Patrol 0x4033a0, Standby 0x405fe0,
// Follow_Ground 0x406300, VTOL_Move 0x40fa20, VTOL_Patrol 0x410e70,
// VTOL_Standby 0x40f7d0, VTOL_Follow 0x40fbe0) run no group check. A
// formation (-N), a Ctrl group (+N) or a computer player's squad is therefore
// never paced, never re-forms and has no record (World::retailGroupOf). The
// Kingdoms group helpers (sim/retailgroup.h, the attacks' and flyer missions'
// head checks) are still checked on their own below; nothing in TA calls them.
#include "navigation_test_common.h"
#include "sim/retailairseek.h"
#include "sim/retailairtoair.h"
#include <cstdlib>

using namespace navigation_test;
namespace {
// navigation_test::setup with sea level 0. mover() has no model and so no
// ground support: its groundY stays 0, below setup's sea level 64, and TA
// halves the speed cap of a ground unit under water (0x43cc20). These
// scenarios move units on dry land.
void dry(World& world,PathfindingMode selected,int cells=256) {
    setup(world,selected,true,false,cells);
    world.setTerrain(std::vector<uint8_t>(size_t(cells)*cells,100),cells,cells,0);
    world.setMapPlacementFeatures(std::vector<uint16_t>(size_t(cells)*cells,0xffff),{});
}
bool formationMission(const Unit& u) {
    return std::any_of(u.orders.begin(),u.orders.end(),[](const Order& o){return o.formationLevel!=0;});
}
UnitType flyer(int kind=0) {
    UnitType t=mover(kind);t.id=t.name="navigation-flyer-"+std::to_string(kind);
    t.canFly=true;t.vtolStandby=true;t.cruiseAlt=64;t.footX=t.footZ=1;
    return t;
}

// A formation (-N), a Ctrl group (+N) and a flagged computer squad: the fast
// member walks at its own maximum, past the slow type's, and nobody re-forms.
void unpaced() {
    for (int kind=0;kind<3;++kind) {
        World world;dry(world,PathfindingMode::Retail);world.setPlayerCount(2);world.player(0).automaticGates=true;
        auto slow=mover(1),fast=mover(0);
        const int a=world.spawn(&slow,640,2048,std::nullopt,0),b=world.spawn(&fast,640,2096,std::nullopt,0);
        for (int id:{a,b}) {
            if (kind==0) world.setSquad(id,-1);
            else if (kind==1) world.setSquad(id,1);
            else world.setAiGroup(0,id,23,false);
            world.order(id,3200,2072,false);
        }
        if (kind==2) world.setAiGroup(0,0,23,true);
        known(world);
        Fixed fastest;
        for (int tick=0;tick<300;++tick) {
            world.tick(1.f/30);
            fastest=fxMax(fastest,world.unit(b)->speed);
            for (int id:{a,b}) {
                require(!formationMission(*world.unit(id)),"a group member re-formed (TA has no formation mission)");
                require(!world.retailGroupRecord(id) && !world.retailGroupPaced(id),"TA has no group record");
            }
        }
        require(fastest>slow.maxVel+Fixed::fromFloat(0.5f),"a group was paced at its slowest member");
        std::printf("scenario=unpaced kind=%d fast_max=%.4f slow_type=%.4f\n",kind,fastest.toFloat(),slow.maxVel.toFloat());
    }
}

// A member far behind its formation keeps its own speed and its own order,
// in both modes (Legion has formations of its own, but not this one's).
void straggler(PathfindingMode selected) {
    World world;dry(world,selected);auto type=mover(1);std::vector<int> ids;
    for(int i=0;i<3;++i)ids.push_back(world.spawn(&type,float(1200),float(2000+i*48),std::nullopt,0));
    ids.push_back(world.spawn(&type,600,2048,std::nullopt,0));
    for(int id:ids){world.setSquad(id,-2);world.order(id,3600,2048,false);}
    known(world);
    bool fast=false;
    for(int tick=0;tick<12000&&!finished(world,ids);++tick) {
        world.tick(1.f/30);
        const auto& u=*world.unit(ids[3]);
        if(u.speed>u.baseSpeed)fast=true;
        for(int id:ids)require(!formationMission(*world.unit(id)),"a formation mission was pushed");
        if(tick%30==0)legal(world,ids);
    }
    require(!fast,"straggler sprinted past its own maximum");
    require(finished(world,ids),"group did not complete its move");
    std::printf("scenario=straggler mode=%d tick=%u\n",int(selected),world.tickCount());
}

// A formation member far from the others on a patrol (air or ground), at
// standby (air or ground) or on a guard: no formation goes ahead of it.
void outliers() {
    for (int scene=0;scene<5;++scene) {
        World world;dry(world,PathfindingMode::Retail);world.setPlayerCount(2);
        const bool air=scene==0 || scene==3;
        auto t=air ? flyer() : mover(0);
        std::vector<int> ids;
        for (int i=0;i<3;++i) ids.push_back(world.spawn(&t,640,1900+float(i)*48,std::nullopt,0));
        const int outlier=world.spawn(&t,2600,3400,std::nullopt,0);ids.push_back(outlier);
        const int ward=world.spawn(&t,2600,3600,std::nullopt,0);
        for (int id:ids) world.setSquad(id,-1);
        known(world);
        if (scene<2) for (int id:ids) world.patrol(id,1600,1960);
        if (scene==4) world.guard(outlier,ward,false);
        for (int tick=0;tick<900;++tick) {
            world.tick(1.f/30);
            require(!formationMission(*world.unit(outlier)),"an outlier got a formation mission");
        }
        const auto& u=*world.unit(outlier);
        if (scene<2) require(!u.orders.empty(),"the outlier's patrol did not stand");
        if (scene==4) require(!u.orders.empty() && u.orders.back().guard,"the outlier's guard did not stand");
        std::printf("scenario=outlier %d orders=%zu\n",scene,u.orders.size());
    }
}

// A rejoin replays the match from tick 0 into a reset World (resetForReplay):
// the AI squads' formation flags (hashed) must not survive the reset.
void replayReset() {
    const auto fresh=[](World& world) {
        dry(world,PathfindingMode::Retail);world.setPlayerCount(2);world.player(0).automaticGates=true;
    };
    World clean;fresh(clean);
    World world;fresh(world);auto t=mover(0);
    const int a=world.spawn(&t,640,2048,std::nullopt,0),b=world.spawn(&t,640,2096,std::nullopt,0);
    for(int id:{a,b}){world.setAiGroup(0,id,7,false);world.order(id,3200,2072,false);}
    world.setAiGroup(0,0,7,true);
    const auto before=world.stateHash();
    world.setAiGroup(0,0,7,false);
    require(world.stateHash()!=before,"the AI formation flag is not hashed");
    world.setAiGroup(0,0,7,true);
    known(world);for(int tick=0;tick<30;++tick)world.tick(1.f/30);
    world.resetForReplay();fresh(world);
    require(!world.aiFormation(0,7),"an AI squad's formation flag survived resetForReplay");
    require(world.stateHash()==clean.stateHash(),"a World reset for a replay does not hash as a fresh one");
    std::puts("scenario=replayReset");
}

// Native slot geometry spot checks (51c700/51ce40/51d1e0).
void geometry() {
    RetailGroupRecord g;g.active=true;
    auto& all=g.all[0];all.count=4;all.area=16;all.centre={100,0,200};
    auto& moving=g.moving[0];moving.count=3;moving.area=12;moving.centre={120,0,200};
    RetailGroupMember u;u.area=4;u.position={300,0,200};
    require(retailOctDistance(-30,40)==47,"octagonal distance");
    const auto c=retailGroupCentre(g,u,true,true);
    require(c.x==(3*120+300)/4 && c.y==c.x && c.z==200,"self-inclusive ground centre");
    require(retailGroupRadius(g,u,true,true)==16 && retailGroupRadius(g,u,false,false)==16,"radius");
    // distance 180 px -> 11 cells; 121/4=30 > 12*2 at level 4 (moving, not self).
    require(retailGroupOutOfSlot(g,u,true,4,false),"level-4 straggler test");
    u.position={150,0,200};
    require(!retailGroupOutOfSlot(g,u,true,4,false),"member inside its slot");
    g.moving[0].count=0;   // empty moving subset relaxes the level by one
    require(!retailGroupOutOfSlot(g,u,true,5,false),"level 5 relaxed to 6 is never out of slot");
    require(retailGroupSpeedCap(65536,117964,117964,117964)==65536,"cap at group speed");
    require(retailGroupSpeedCap(10000,117964,117964,117964)==int32_t((int64_t(65536)*29491)>>16),"25% floor");
    require(retailGroupSpeedCap(200000,117964,117964,117964)==0,"no cap above own maximum");
}

// The attacks' group check (retailAttackGroupCheck: SUPPRESS 404e12, the
// flyer attacks 41c19a / 41b7a4 / 41dafa): its three outcomes.
void attackHead() {
    struct Calls { std::vector<int> bounds; int leaves=0,formations=0; };
    const auto run=[](uint32_t& flags,bool out,uint32_t next,int draw,Calls& c) {
        return retailAttackGroupCheck(flags,out,next,3,15,[&](int n){c.bounds.push_back(n);return draw;},
            [&]{++c.leaves;},[&]{++c.formations;return true;});
    };
    // Out of its slot, not yet flagged: no draw, the flag set, the formation
    // pushed, 0 (the attack starts again at stage 0).
    {uint32_t f=0x1000410u;Calls c;
     require(run(f,true,0,0,c)==0 && f==0x9000410u && c.formations==1 && c.bounds.empty() && !c.leaves,
             "out of slot unflagged: formation, flag, 0");}
    // Flagged and out again: rand(3) == 0 leaves (8); 50c480 unless the next
    // mission moves with the group.
    for (const uint32_t next:{0u,0x5012000u,0x15000400u,0x3000400u,0x11000000u}) {
        uint32_t f=0x9000410u;Calls c;
        const bool mover=(next&0x1000000u) && !(next&0x4000000u);
        require(run(f,true,next,0,c)==8 && c.bounds==std::vector<int>{3} && c.leaves==int(!mover) && !c.formations,
                "flagged out of slot, rand(3) == 0: leave (50c480 by the next mission), 8");
    }
    {uint32_t f=0x9000410u;Calls c;
     require(run(f,true,0,2,c)==0 && c.formations==1 && c.bounds==std::vector<int>{3} && !c.leaves,
             "flagged out of slot, rand(3) != 0: formation again");}
    // In its slot: the stages run (-1); a flagged mission clears on rand(15) == 0.
    {uint32_t f=0x9000410u;Calls c;
     require(run(f,false,0,0,c)==-1 && f==0x1000410u && c.bounds==std::vector<int>{15},
             "in slot flagged, rand(15) == 0: cleared");}
    {uint32_t f=0x9000410u;Calls c;
     require(run(f,false,0,7,c)==-1 && f==0x9000410u,"in slot flagged, rand(15) != 0: kept");}
    {uint32_t f=0x1000410u;Calls c;
     require(run(f,false,0,0,c)==-1 && c.bounds.empty(),"in slot unflagged: no draw");}
    // A formation that cannot be built ends the list (7), the flag already set.
    {uint32_t f=0x1000410u;
     require(retailAttackGroupCheck(f,true,0,2,25,[](int){return 0;},[]{},[]{return false;})==7 &&
             (f&0x8000000u),"an unbuilt formation: 7");}
}
// The flyer missions' single group check, at their head (tools/re:
// check_air_to_air.py, check_air_seek_attack.py, check_air_traffic.py):
// AirToAir (41ccde), VTOL_Evade (41e55a) and VTOL_SeekAttack (4194f9) as
// AirToGround's (51d1e0(unit, 1, 5), VTOL_Move_Formation at level -4), and
// VTOL_SeekGuard (4199b2) as VTOL_Follow's (51d1e0(unit, 1, 4), level -3).
// Out of the slot, not yet flagged: the flag set, the formation pushed, 0.
void airHeads() {
    struct Group {
        uint32_t seed=1;bool out=true;unsigned outCalls=0,leaves=0,formations=0,cancels=0;
        uint32_t random(int n) { return retailRandom(seed,n); }
        bool outOfSlot() { ++outCalls;return out; }
        uint32_t nextFlags() const { return 0; }
        void leave() { ++leaves; }
        void cancel() { ++cancels; }
        bool formation() { ++formations;return true; }
        void install(const RetailFlightGoal&) {}
        void install(const RetailInterceptGoal&) {}
        void beginFlight() {}
        void slotsAuto() {}
        void aim() {}
        RetailFlightVector lead() const { return {}; }
        bool retarget() { return false; }
        void release() {}
        bool evade() { return false; }
        bool attack() { return false; }
        bool stepOut(int32_t) { return true; }
        bool stepOut(RetailStepOut,int32_t) { return true; }
        void stopBuilding() {}
        bool found() { return false; }
        bool guard() { return false; }
    };
    const auto expect=[](const char* name,int result,uint32_t flags,const Group& g,uint32_t before) {
        if (result!=0 || flags!=(before|0x8000000u) || g.formations!=1 || g.outCalls!=1) {
            std::printf("%s: result %d flags %#x formations %u checks %u\n",name,result,flags,g.formations,g.outCalls);
            require(false,"a flyer mission's group check out of the slot: formation, flag, 0");
        }
    };
    {Group g;RetailMissionState m;m.flags=0x1000200u;int32_t count=0;RetailAirDuel duel;duel.maxSpeed=65536;
     const int result=retailAirToAir(m,count,0,0,duel,g);expect("AirToAir",result,m.flags,g,0x1000200u);}
    {Group g;RetailAirEvade e;e.mission.flags=0x1000200u;RetailAirEscape in;in.typeSpeed=in.targetTypeSpeed=65536;
     const int result=retailAirEvade(e,0,0,in,g);expect("VTOL_Evade",result,e.mission.flags,g,0x1000200u);}
    {Group g;RetailMissionState m;m.flags=0x1000600u;RetailFlightVector point{};uint16_t angle=0;RetailAirSeek in;
     const int result=retailVtolSeekAttack(m,point,angle,0,0,in,g);
     expect("VTOL_SeekAttack",result,m.flags,g,0x1000600u);}
    // TotalA.exe's VTOL_SeekGuard (0x410850) has no group check: out of the
    // slot, flagged or not, it runs its stages.
    for (const uint32_t flags:{0x1000600u,0x9000600u}) {
        Group g;RetailMissionState m;m.flags=flags;RetailFlightVector point{};uint16_t angle=0;RetailAirSeekGuard in;
        const int result=retailVtolSeekGuard(m,point,angle,0,0,in,g);
        require(result==1 && !g.outCalls && !g.formations && !g.cancels && m.flags==flags,
                "TA's VTOL_SeekGuard has no group check");
    }
    // In the slot every one runs its stages.
    {Group g;g.out=false;RetailMissionState m;m.flags=0x1000200u;int32_t count=0;RetailAirDuel duel;
     retailAirToAir(m,count,0,0,duel,g);
     require(g.outCalls==1 && !g.formations && m.flags==0x1000200u,"AirToAir in the slot runs its stages");}
}

}

int main() {
    try {
        geometry();attackHead();airHeads();unpaced();replayReset();outliers();
        straggler(PathfindingMode::Retail);straggler(PathfindingMode::Legion);
    } catch(const std::exception& error) {std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
    std::puts("retail groups (none in TA): PASS");
    return 0;
}
