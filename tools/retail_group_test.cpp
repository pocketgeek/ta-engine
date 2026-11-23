// Retail mode group pacing (51b890 / 4d95f0 / 402880): slowest type speed,
// straggler slowdown, and re-forming through Move_Ground_Formation.
#include "navigation_test_common.h"
#include "sim/retailairseek.h"
#include "sim/retailairtoair.h"
#include <cstdlib>

using namespace navigation_test;
namespace {
bool formationMission(const Unit& u) {
    return std::any_of(u.orders.begin(),u.orders.end(),[](const Order& o){return o.formationLevel!=0;});
}

// A formation (-N, retail "CreateSquad N f") paces at the slowest member's
// type speed.
void slowest() {
    World world;setup(world,PathfindingMode::Retail);auto slow=mover(1),fast=mover(0);
    const int a=world.spawn(&slow,640,2048,std::nullopt,0),b=world.spawn(&fast,640,2096,std::nullopt,0);
    for(int id:{a,b}){world.setSquad(id,-1);world.order(id,3200,2072,false);}
    known(world);Fixed maximum;
    int reform=0;
    for(int tick=0;tick<600;++tick){const auto x=world.unit(b)->x,z=world.unit(b)->z;
        // A member re-forming (Move_Ground_Formation) is not paced: retail's
        // formation mission lacks the 0x2000000 flag the cap keys on.
        const bool paced=!formationMission(*world.unit(b));world.tick(1.f/30);
        reform+=!paced||formationMission(*world.unit(b));
        (void)x;(void)z;
        if(!paced||formationMission(*world.unit(b)))continue;
        // The mover's speed, not the displacement: the Q13 heading table
        // rounds a diagonal step up to ~1% longer than the speed.
        const Fixed step=world.unit(b)->speed;maximum=fxMax(maximum,step);
// Retail scales the group speed by the member's own/type maximum
        // (individual speed variation), as 4d95f0 does.
        const auto& fu=*world.unit(b);
        const Fixed cap=Fixed::raw(retailGroupSpeedCap(slow.maxVel.v,fu.baseSpeed.v,fu.type->maxVel.v,fu.type->maxVel.v));
        require(step<=cap,"grouped fast member exceeded the slowest type speed");}
    std::printf("slowest: reform ticks=%d\n",reform);
    const auto* record=world.retailGroupRecord(b);
    require(record&&record->active&&record->groundSpeed==slow.maxVel.v,"group speed is not the slowest type maximum");
    require(maximum>slow.maxVel-Fixed::fromFloat(0.05f)&&maximum<slow.maxVel+Fixed::fromFloat(0.05f),"grouped fast member did not reach the group speed");
    // Ungrouped, the same fast unit keeps its own maximum.
    World alone;setup(alone,PathfindingMode::Retail);
    const int c=alone.spawn(&fast,640,2096,std::nullopt,0);alone.order(c,3200,2072,false);known(alone);
    Fixed free;
    for(int tick=0;tick<300;++tick){const auto x=alone.unit(c)->x;alone.tick(1.f/30);free=fxMax(free,alone.unit(c)->x-x);}
    require(free>slow.maxVel+Fixed::fromFloat(0.5f),"ungrouped unit was paced");
    std::printf("scenario=slowest grouped_max=%.4f ungrouped_max=%.4f\n",maximum.toFloat(),free.toFloat());
}

// A member far behind slows the group by 0xaa7e/65536 instead of sprinting,
// then re-forms through a Move_Ground_Formation mission.
void straggler(PathfindingMode selected) {
    World world;setup(world,selected);auto type=mover(1);std::vector<int> ids;
    for(int i=0;i<3;++i)ids.push_back(world.spawn(&type,float(1200),float(2000+i*48),std::nullopt,0));
    ids.push_back(world.spawn(&type,600,2048,std::nullopt,0));
    for(int id:ids){world.setSquad(id,-2);world.order(id,3600,2048,false);}
    known(world);world.tick(1.f/30);
    const auto* record=world.retailGroupRecord(ids[0]);
    if(selected!=PathfindingMode::Retail) {
        require(!record,"non-Retail mode exposed a native group record");
    } else {
        require(record&&record->active,"Retail group record inactive");
        require(record->groundSpeed==retailGroupStragglerSpeed(type.maxVel.v,type.maxVel.v),
                "one straggler did not slow the group by 0xaa7e/65536");
        require(record->groundSpeed==0xaa7e,"straggler slowdown is not the native factor");
    }
    bool reformed=false,retired=false,fast=false;
    for(int tick=0;tick<12000&&!finished(world,ids);++tick) {
        world.tick(1.f/30);
        const auto& u=*world.unit(ids[3]);
        if(formationMission(u))reformed=true;else if(reformed)retired=true;
        if(u.speed>u.baseSpeed)fast=true;
        for(int id:ids)require(selected==PathfindingMode::Retail||!formationMission(*world.unit(id)),
            "non-Retail mode pushed a formation mission");
        if(tick%30==0)legal(world,ids);
    }
    for(int id:ids){const auto& u=*world.unit(id);std::printf("member id=%d x=%.1f z=%.1f orders=%zu formation=%d\n",id,u.x.toFloat(),u.z.toFloat(),u.orders.size(),formationMission(u));
}
    require(!fast,"straggler sprinted past its own maximum");
    require(finished(world,ids),"group did not complete its move");
    if(selected==PathfindingMode::Retail) {
        require(reformed,"straggler never received Move_Ground_Formation");
        require(retired,"Move_Ground_Formation never retired");
    }
    std::printf("scenario=straggler mode=%d reformed=%d retired=%d tick=%u\n",int(selected),reformed,retired,world.tickCount());
}

// A Ctrl group (+N) is retail's plain group ("CreateSquad N", record +8
// clear): 51b890 never aggregates it, so it is never active, never capped,
// and never re-forms. The slow and fast members and the straggler from the
// scenes above all walk at their own speeds.
void plain() {
    World world;setup(world,PathfindingMode::Retail);auto slow=mover(1),fast=mover(0);
    std::vector<int> ids{world.spawn(&slow,640,2048,std::nullopt,0),world.spawn(&fast,640,2096,std::nullopt,0),
                         world.spawn(&fast,200,2096,std::nullopt,0)};
    for(int id:ids){world.setSquad(id,1);world.order(id,3200,2072,false);}
    known(world);
    Fixed fastest;bool reformed=false;
    for(int tick=0;tick<300;++tick){
        world.tick(1.f/30);
        fastest=fxMax(fastest,world.unit(ids[1])->speed);
        for(int id:ids)reformed|=formationMission(*world.unit(id));
        const auto* record=world.retailGroupRecord(ids[0]);
        require(record&&!record->active&&record->all[0].count==0,"a Ctrl group's record was aggregated");
    }
    require(!reformed,"a Ctrl group member re-formed");
    require(fastest>slow.maxVel+Fixed::fromFloat(0.5f),"a Ctrl group was paced at its slowest member");
    std::printf("scenario=plain fast_max=%.4f slow_type=%.4f\n",fastest.toFloat(),slow.maxVel.toFloat());
}

// A computer player's AI squads (Cmd::AiGroup): a strike or VTOL squad is
// its own record, a formation only while the AI flags it (50b700). Its
// members are aggregated, get the group checks and take the speed cap: a
// computer player is owner kind 2 on the machine that runs it (4d978f caps
// kinds 1 and 2; only a remote player, kind 3, is left to its own machine).
// A human player cannot use it.
void aiSquads() {
    World world;setup(world,PathfindingMode::Retail);world.setPlayerCount(2);
    world.player(0).automaticGates=true;
    auto slow=mover(1),fast=mover(0);
    const int a=world.spawn(&slow,640,2048,std::nullopt,0),b=world.spawn(&fast,640,2096,std::nullopt,0);
    const int h=world.spawn(&fast,640,1500,std::nullopt,1);
    for(int id:{a,b}){world.setAiGroup(0,id,23,false);world.order(id,3200,2072,false);}
    world.setAiGroup(1,h,23,false);world.setAiGroup(1,0,23,true);
    require(world.unit(a)->aiGroup==23 && world.unit(b)->aiGroup==23,"AI squad membership");
    require(world.unit(h)->aiGroup==0 && !world.aiFormation(1,23),"a human player took an AI squad");
    known(world);
    world.tick(1.f/30);
    const auto* record=world.retailGroupRecord(a);
    require(record && !record->active,"an unflagged AI squad was aggregated");
    world.setAiGroup(0,0,23,true);
    require(world.aiFormation(0,23),"AI squad formation flag");
    Fixed fastest;
    for(int tick=0;tick<120;++tick){
        world.tick(1.f/30);
        const auto& fu=*world.unit(b);
        if(formationMission(fu))continue;   // re-forming is not paced (no 0x2000000)
        fastest=fxMax(fastest,fu.speed);
        const Fixed cap=Fixed::raw(retailGroupSpeedCap(slow.maxVel.v,fu.baseSpeed.v,fu.type->maxVel.v,fu.type->maxVel.v));
        require(fu.speed<=cap,"a computer player's formation did not take the speed cap");
    }
    record=world.retailGroupRecord(a);
    require(record && record->active && record->all[0].count==2 && record->groundSpeed==slow.maxVel.v,
            "a flagged AI squad is not a paced formation");
    require(fastest>Fixed() && fastest<slow.maxVel+Fixed::fromFloat(0.5f),
            "a computer player's formation did not take the speed cap");
    const auto before=world.stateHash();
    world.setAiGroup(0,0,23,false);
    require(world.stateHash()!=before,"the AI formation flag is not hashed");
    world.setAiGroup(0,0,23,true);
    require(world.stateHash()==before,"the AI formation flag hash");
    world.setAiGroup(0,a,0,false);
    require(world.unit(a)->aiGroup==0 && world.stateHash()!=before,"leaving an AI squad");
    std::printf("scenario=aiSquads fast_max=%.4f\n",fastest.toFloat());
}

// A rejoin replays the match from tick 0 into a reset World (resetForReplay):
// the AI squads' formation flags (hashed) and the group records must not
// survive the reset, or the rejoiner's sim paces squads the referee's does
// not yet and its hash differs from the first tick.
void replayReset() {
    const auto fresh=[](World& world) {
        setup(world,PathfindingMode::Retail);world.setPlayerCount(2);world.player(0).automaticGates=true;
    };
    World clean;fresh(clean);
    World world;fresh(world);auto t=mover(0);
    const int a=world.spawn(&t,640,2048,std::nullopt,0),b=world.spawn(&t,640,2096,std::nullopt,0);
    for(int id:{a,b}){world.setAiGroup(0,id,7,false);world.order(id,3200,2072,false);}
    world.setAiGroup(0,0,7,true);
    known(world);for(int tick=0;tick<30;++tick)world.tick(1.f/30);
    require(world.retailGroupRecord(a) && world.retailGroupRecord(a)->active,"the AI squad was not a formation");
    world.resetForReplay();fresh(world);
    require(!world.aiFormation(0,7),"an AI squad's formation flag survived resetForReplay");
    require(world.stateHash()==clean.stateHash(),"a World reset for a replay does not hash as a fresh one");
    const int c=world.spawn(&t,640,2048,std::nullopt,0);world.setAiGroup(0,c,7,false);
    const auto* record=world.retailGroupRecord(c);
    require(record && !record->active && record->all[0].count==0,"a group record survived resetForReplay");
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
    {Group g;RetailMissionState m;m.flags=0x1000600u;RetailFlightVector point{};uint16_t angle=0;RetailAirSeekGuard in;
     const int result=retailVtolSeekGuard(m,point,angle,0,0,in,g);
     expect("VTOL_SeekGuard",result,m.flags,g,0x1000600u);}
    // In the slot every one runs its stages; a guard flagged and out of the
    // slot again lets the member go on rand(4) == 0 (2).
    {Group g;g.out=false;RetailMissionState m;m.flags=0x1000200u;int32_t count=0;RetailAirDuel duel;
     retailAirToAir(m,count,0,0,duel,g);
     require(g.outCalls==1 && !g.formations && m.flags==0x1000200u,"AirToAir in the slot runs its stages");}
    {bool let=false;
     for (uint32_t seed=1;seed<64 && !let;++seed) {
         Group g;g.seed=seed;RetailMissionState m;m.flags=0x9000600u;RetailFlightVector point{};uint16_t angle=0;
         RetailAirSeekGuard in;
         const int result=retailVtolSeekGuard(m,point,angle,0,0,in,g);
         if (result==2) { require(g.cancels==1 && !g.formations,"a guard let go: 50c480 and 4d6a50, no formation");let=true; }
         else require(result==0 && g.formations==1,"a flagged guard out of the slot: formation again");
     }
     require(let,"a flagged guard out of the slot is let go on rand(4) == 0");}
}

// A flyer for the World scenes: the plain mover made a VTOL that stands by
// in the air.
UnitType flyer(int kind=0) {
    UnitType t=mover(kind);t.id=t.name="navigation-flyer-"+std::to_string(kind);
    t.canFly=true;t.vtolStandby=true;t.cruiseAlt=64;t.footX=t.footZ=1;
    return t;
}

// A flyer's move is VTOL_Move (row 0x5ebcd3: 0x1000402), not Move_Ground's
// 0x3000400 (tools/re/check_air_point_mission.py): no navigator formation
// bit (0x2000000), so a flyer in a formation is never paced.
void flyerMove() {
    World world;setup(world,PathfindingMode::Retail);auto t=flyer();
    const int a=world.spawn(&t,640,2048,std::nullopt,0);
    world.setSquad(a,-1);world.order(a,1600,2048,false);
    const auto& o=world.unit(a)->orders;
    require(o.size()==1 && o[0].mission.flags==0x1000402u,"a flyer's move does not carry VTOL_Move's flags");
    known(world);world.tick(1.f/30);world.tick(1.f/30);
    const auto* record=world.retailGroupRecord(a);
    require(record && record->active && record->moving[size_t(RetailGroupClass::Flyer)].count==1,
            "a flyer's move is not a group mover");
}

// A formation's patrolling flyer left behind (VTOL_Patrol 419f9b..41a200,
// the same two checks as VTOL_Move): VTOL_Move_Formation goes ahead of its
// leg, and when it retires the patrol goes on along the same leg, with no
// second anchor (4d6b80 finds the leg flagged 0x4000).
void flyerPatrol() {
    World world;setup(world,PathfindingMode::Retail);auto t=flyer();
    std::vector<int> ids;
    for (int i=0;i<3;++i) ids.push_back(world.spawn(&t,640,1900+float(i)*48,std::nullopt,0));
    const int outlier=world.spawn(&t,2600,3400,std::nullopt,0);ids.push_back(outlier);
    for (int id:ids) world.setSquad(id,-1);
    known(world);
    for (int id:ids) world.patrol(id,1600,1960);
    const auto legs=[&] {
        std::vector<std::pair<Fixed,Fixed>> v;
        for (const auto& o:world.unit(outlier)->orders) if (!o.formationLevel) v.emplace_back(o.x,o.z);
        return v;
    };
    const auto before=legs();
    bool pushed=false,retired=false,ahead=false;
    std::vector<std::pair<Fixed,Fixed>> during;
    for (int tick=0;tick<1800 && !retired;++tick) {
        world.tick(1.f/30);
        const auto& o=world.unit(outlier)->orders;
        const bool lead=!o.empty() && o[0].formationLevel!=0;
        if (lead && !pushed) { pushed=true;during=legs();ahead=o.size()>1 && o[1].patrol; }
        if (pushed && !lead) retired=true;
    }
    require(pushed,"a patrolling flyer out of its slot got no VTOL_Move_Formation");
    require(ahead,"the formation did not go ahead of the patrol leg");
    require(during==before,"the formation changed the patrol's legs");
    require(retired,"the patrol's formation never retired");
    const auto after=legs();
    require(after.size()==before.size() && after.front()==before.front(),
            "the patrol did not resume its leg once the formation retired");
    std::printf("scenario=flyerPatrol legs=%zu\n",after.size());
}

// A formation's patrolling walker left behind (Patrol 403600: the same two
// checks as Move_Ground's 402b00 at 40361c / 4037e9, E: check_retail_group.py
// patrol): Move_Ground_Formation goes ahead of its leg, and when it retires
// the patrol goes on along the same leg.
void groundPatrol() {
    World world;setup(world,PathfindingMode::Retail);auto t=mover(0);
    std::vector<int> ids;
    for (int i=0;i<3;++i) ids.push_back(world.spawn(&t,640,1900+float(i)*48,std::nullopt,0));
    const int outlier=world.spawn(&t,2600,3400,std::nullopt,0);ids.push_back(outlier);
    for (int id:ids) world.setSquad(id,-1);
    known(world);
    for (int id:ids) world.patrol(id,1600,1960);
    const auto legs=[&] {
        std::vector<std::pair<Fixed,Fixed>> v;
        for (const auto& o:world.unit(outlier)->orders) if (!o.formationLevel) v.emplace_back(o.x,o.z);
        return v;
    };
    const auto before=legs();
    bool pushed=false,retired=false,ahead=false;
    std::vector<std::pair<Fixed,Fixed>> during;
    for (int tick=0;tick<3000 && !retired;++tick) {
        world.tick(1.f/30);
        const auto& o=world.unit(outlier)->orders;
        const bool lead=!o.empty() && o[0].formationLevel!=0;
        if (lead && !pushed) { pushed=true;during=legs();ahead=o.size()>1 && o[1].patrol; }
        if (pushed && !lead) retired=true;
    }
    require(pushed,"a patrolling walker out of its slot got no Move_Ground_Formation");
    require(ahead,"the formation did not go ahead of the ground patrol leg");
    require(during==before,"the formation changed the ground patrol's legs");
    require(retired,"the ground patrol's formation never retired");
    const auto after=legs();
    require(!after.empty() && after.size()==before.size() && after.front()==before.front(),
            "the ground patrol did not resume its leg once the formation retired");
    std::printf("scenario=groundPatrol legs=%zu\n",after.size());
}

// The standbys' group check (Standby 407779, VTOL_Standby 417527): an idle
// formation member out of its slot (51d1e0(unit, 1, 2)) gets the
// FormationStandby (Move_Ground_FormationStandby with response mode -1:
// 0x15000000; VTOL_Move_FormationStandby: 0x5000002) at level -1, goes back
// to the group, and stands by again once it retires.
void standbys(bool air) {
    World world;setup(world,PathfindingMode::Retail);auto t=air ? flyer() : mover(0);
    std::vector<int> ids;
    for (int i=0;i<3;++i) ids.push_back(world.spawn(&t,640,1900+float(i)*64,std::nullopt,0));
    const int outlier=world.spawn(&t,1800,2700,std::nullopt,0);ids.push_back(outlier);
    for (int id:ids) world.setSquad(id,-1);
    known(world);
    bool pushed=false,retired=false;uint32_t flags=0;int level=0;
    for (int tick=0;tick<3000 && !retired;++tick) {
        world.tick(1.f/30);
        const auto& u=*world.unit(outlier);
        const bool lead=!u.orders.empty() && u.orders.front().formationLevel!=0;
        if (lead && !pushed) { pushed=true;flags=u.orders.front().mission.flags;level=u.orders.front().formationLevel; }
        if (pushed && !lead) retired=true;
    }
    std::printf("scenario=standby air=%d pushed=%d flags=%#x level=%d retired=%d\n",int(air),int(pushed),flags,level,
                int(retired));
    require(pushed,"an idle formation member out of its slot got no FormationStandby");
    require(flags==(air ? 0x5000002u : 0x15000000u) && level==-1,"the FormationStandby's flags or level");
    require(retired,"the FormationStandby never retired");
    const auto& u=*world.unit(outlier);
    require(u.orders.empty() && u.standbyActive,"the member does not stand by after its FormationStandby");
}
}

// A walker's guard and attack in a formation (rows 0x5eb89f Follow_Ground
// 0x3000200, 0x5eb8d1 Attack_Chase 0x1000280): both are group movers, so
// 51b890 counts them in the moving aggregate. A guard out of its slot runs
// Follow_Ground's group check (40800c..40810e, VTOL_Follow's:
// tools/re/check_retail_group.py follow): Move_Ground_Formation at level -3
// goes ahead of the guard, and the guard goes on when it retires.
void groundGuard() {
    World world;setup(world,PathfindingMode::Retail);world.setPlayerCount(2);auto t=mover(0),armed=mover(0);
    armed.weapon.damage=10;armed.weapon.range=120;armed.weapons={armed.weapon};
    std::vector<int> ids;
    for (int i=0;i<2;++i) ids.push_back(world.spawn(&t,640,1900+float(i)*48,std::nullopt,0));
    const int ward=world.spawn(&t,2600,3400,std::nullopt,0);
    const int guard=world.spawn(&t,2600,3460,std::nullopt,0);
    const int attacker=world.spawn(&armed,700,1700,std::nullopt,0);
    const int enemy=world.spawn(&t,1400,1700,std::nullopt,1);
    for (int id:ids) world.setSquad(id,-1);
    world.setSquad(guard,-1);world.setSquad(attacker,-1);
    known(world);
    world.guard(guard,ward,false);world.attack(attacker,enemy,false);
    world.tick(1.f/30);world.tick(1.f/30);
    const auto* record=world.retailGroupRecord(guard);
    require(record && record->active && record->moving[size_t(RetailGroupClass::Ground)].count==2,
            "a walker's guard and attack are not the group's movers");
    {   // In its slot (the ward beside the group) the guard is paced.
        World near;setup(near,PathfindingMode::Retail);
        const int a=near.spawn(&t,640,1900,std::nullopt,0),b=near.spawn(&t,640,1948,std::nullopt,0);
        const int w=near.spawn(&t,700,1924,std::nullopt,0);
        near.setSquad(a,-1);near.setSquad(b,-1);known(near);near.guard(b,w,false);
        near.tick(1.f/30);near.tick(1.f/30);
        require(!formationMission(*near.unit(b)),"a guard in its slot re-formed");
        require(near.retailGroupPaced(b),"a walker's guard (0x2000000) is not paced");
    }
    // Back in the slot the formation retires and the guard goes on (to its
    // far ward, so out again: flagged, rand(4) == 0 then drops every order,
    // as in retail).
    bool pushed=false,ahead=false,resumed=false,dropped=false;
    for (int tick=0;tick<3000 && !resumed;++tick) {
        const bool led=[&]{const auto& o=world.unit(guard)->orders;return !o.empty() && o[0].formationLevel!=0;}();
        world.tick(1.f/30);
        const auto& o=world.unit(guard)->orders;
        const bool lead=!o.empty() && o[0].formationLevel!=0;
        if (lead && !led && !pushed) {
            pushed=true;
            ahead=o[0].formationLevel==-3 && o[0].mission.flags==0x11000000u && o.size()>1 && o.back().guard &&
                  o.back().targetId==ward;
        }
        if (led && !lead) {
            if (!o.empty() && o.front().guard && o.front().targetId==ward) resumed=true;
            else if (o.empty()) dropped=true;
        }
    }
    require(pushed,"a guard out of its slot got no Move_Ground_Formation");
    require(ahead,"the guard's formation is not Move_Ground_Formation at -3 ahead of the guard");
    require(resumed,"the guard did not go on after its formation retired");
    std::printf("scenario=groundGuard tick=%u dropped=%d\n",world.tickCount(),int(dropped));
}

// The transport missions are group movers (rows' +0x11, read statically):
// Move_Seek_Pickup and Ground_Pickup 0x1000200, Ground_Unload 0x1000400. A
// formation's passenger walking to its wagon, the wagon on its way to fetch
// it, and a wagon on its way to unload count in the moving aggregate (51b890:
// 0x1000000). World gave these orders no flags.
void transports() {
    World world;setup(world,PathfindingMode::Retail);
    auto t=mover(0),wagon=mover(0);
    t.transportLandEligible=true;t.transportSize=1;t.modelTop=100<<16;   // above the 64 sea level from a groundY of 0
    wagon.canTransport=true;wagon.maxTransportSize=4;wagon.transportCap=4;wagon.transportSizeCap=8;
    wagon.transportDist=60;
    const int passenger=world.spawn(&t,640,1900,std::nullopt,0);
    const int carrier=world.spawn(&wagon,2600,3400,std::nullopt,0);
    const int idle=world.spawn(&t,640,1948,std::nullopt,0);
    for (int id:{passenger,carrier,idle}) world.setSquad(id,-1);
    known(world);
    world.loadInto(passenger,carrier,false);
    require(!world.unit(passenger)->orders.empty() && world.unit(passenger)->orders.front().load &&
            !world.unit(carrier)->orders.empty() && world.unit(carrier)->orders.front().transportPickup,
            "the pickup orders were not given");
    world.tick(1.f/30);world.tick(1.f/30);
    const auto* record=world.retailGroupRecord(passenger);
    require(record && record->active && record->moving[size_t(RetailGroupClass::Ground)].count==2,
            "a passenger's Move_Seek_Pickup and its carrier's Ground_Pickup are not the group's movers");
    // A loaded wagon sent to unload far off moves with its formation too.
    World drop;setup(drop,PathfindingMode::Retail);
    const int a=drop.spawn(&t,640,1900,std::nullopt,0),w=drop.spawn(&wagon,700,1900,std::nullopt,0);
    const int b=drop.spawn(&t,640,1948,std::nullopt,0);
    for (int id:{w,b}) drop.setSquad(id,-1);
    known(drop);
    require(drop.embarkInto(a,w),"the soldier did not board the wagon");
    drop.unloadAt(w,2600,3400,Fixed(),false);
    drop.tick(1.f/30);drop.tick(1.f/30);
    const auto* dropRecord=drop.retailGroupRecord(w);
    require(dropRecord && dropRecord->active && dropRecord->moving[size_t(RetailGroupClass::Ground)].count==1,
            "a carrier's Ground_Unload is not a group mover");
    std::printf("scenario=transports\n");
}

// A passenger stays in its group (51b890's member filter, 51ba44..51ba8f,
// never reads the carrier +0xa8; check_retail_group.py carried): it counts
// in the group's aggregate at its +0x68, the attach point that follows the
// carrier, as a member that is not moving -- its mission aboard is BeCarried
// (row flags 0x24), which runs no group check, so it never re-forms.
void carried() {
    World world;setup(world,PathfindingMode::Retail);
    auto t=mover(0),wagon=mover(0);
    t.transportLandEligible=true;t.transportSize=1;
    wagon.canTransport=true;wagon.maxTransportSize=4;wagon.transportCap=4;wagon.transportSizeCap=8;
    wagon.transportDist=60;
    const int a=world.spawn(&t,640,1900,std::nullopt,0),w=world.spawn(&wagon,1600,1900,std::nullopt,0);
    const int b=world.spawn(&t,640,1948,std::nullopt,0);
    for (int id:{a,w,b}) world.setSquad(id,-1);
    known(world);
    require(world.embarkInto(a,w),"the soldier did not board the wagon");
    world.order(w,3000,2400,false);
    bool reformed=false;
    for (int tick=0;tick<240;++tick) {
        world.tick(1.f/30);
        reformed|=!world.unit(a)->orders.empty();
        const auto* record=world.retailGroupRecord(b);
        require(record && record->active,"the group is not active");
        const auto& all=record->all[size_t(RetailGroupClass::Ground)];
        require(all.count==3 && all.area==2*t.footX*t.footZ+wagon.footX*wagon.footZ,
                "a passenger is not counted in its group");
        require(record->moving[size_t(RetailGroupClass::Ground)].count<=1,"a passenger counts as moving");
        require(!world.retailGroupPaced(a),"a passenger is paced");
    }
    require(!reformed,"a passenger re-formed while carried");
    // The centre is taken before the units move, the passenger where its
    // wagon carried it.
    const auto ax=world.unit(a)->x.floorInt(),az=world.unit(a)->z.floorInt();
    const auto wx=world.unit(w)->x.floorInt(),wz=world.unit(w)->z.floorInt();
    const auto bx=world.unit(b)->x.floorInt(),bz=world.unit(b)->z.floorInt();
    require(std::abs(ax-wx)<8 && std::abs(az-wz)<8 && (ax!=640 || az!=1900),"the passenger did not ride along");
    world.tick(1.f/30);
    const auto& centre=world.retailGroupRecord(b)->all[size_t(RetailGroupClass::Ground)].centre;
    require(centre.x==(ax+wx+bx)/3 && centre.z==(az+wz+bz)/3,"a passenger is not in its group's centre");
    std::printf("scenario=carried centre=%d,%d\n",centre.x,centre.z);
}

int main() {
    try {
        geometry();attackHead();airHeads();slowest();plain();aiSquads();replayReset();
        flyerMove();flyerPatrol();groundPatrol();groundGuard();transports();carried();standbys(false);standbys(true);
        straggler(PathfindingMode::Retail);
    } catch(const std::exception& error) {std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
    std::puts("retail group pacing: PASS");
    return 0;
}
