// Point missions must outlive partial routes and produced units must inherit
// real movement controllers, including their occupied-destination handling.
#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "hpi/hpi.h"
#include "tnt/mapgen.h"
#include "net/lockstep.h"
#include "net/protocol.h"
#include <algorithm>
#include <set>
#include <cstdio>
#include <stdexcept>
using namespace tak::sim;
namespace {
int failures=0;
// Retail by default; --legion selects Legion.
PathfindingMode selectedMode=PathfindingMode::Retail;
void check(bool ok,const char* message) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",message);failures+=!ok;
}
UnitType soldier() {
    UnitType t{};t.id=t.name="order-test";t.maxVel=Fixed::fromFloat(70.f/30);
    t.turnRate=10000;t.maxHp=100;t.canMove=true;t.footX=t.footZ=2;
    t.sight=4096;t.buildTime=1;return t;
}
void setup(World& w,PathfindingMode mode,bool serial=true) {
    w.setSerialThreads(serial);w.setPathfindingMode(mode);
    w.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);w.setPathService(true);
}
void ticks(World& w,int count) {for(int n=0;n<count;++n)w.tick(1.f/30);}
void partial(PathfindingMode mode,bool patrol,bool clipped) {
    World w;setup(w,mode);auto type=soldier();const int id=w.spawn(&type,160,320,0,0);
    if(patrol) {w.patrolTo(id,1600,320,false);w.patrolTo(id,160,320,true);}
    else {w.attackMove(id,1600,320,false);w.attackMove(id,1600,1600,true);}
    w.setPathService(false);
    auto& state=w.unit(id)->orders.front().mission;
    // Exercise the active mission without its stage-zero direct-segment reset.
    state.stage=patrol?3:2;state.waitMask=0x2700;state.deadline=0xffffffffu;
    std::vector<PathCell> route{{10,20},{30,20}};
    if(clipped) {route.clear();for(int x=10;x<80;++x)route.push_back({x,20});route.push_back({100,20});}
    w.deliverSearchRoute(id,route,Fixed::fromInt(1600),Fixed::fromInt(320),!clipped,false,!clipped,false);
    // The injected route's first point is a corner; the final installed point
    // is the controller's prefix endpoint. Stand there to expose false arrival.
    const auto end=w.unit(id)->orders[World::currentLeg(w.unit(id)->orders)];
    auto* u=w.unit(id);u->x=end.x;u->z=end.z;ticks(w,1);
    check(!u->orders.empty()&&u->orders[World::currentLeg(u->orders)].missionTarget==
        std::pair{Fixed::fromInt(1600),Fixed::fromInt(320)},
        patrol?"patrol retains its requested point at a partial/clipped endpoint":"fight-move retains its requested point at a partial/clipped endpoint");
    check(u->orders.size()>=2&&u->orders.back().missionTarget.value_or(std::pair{u->orders.back().x,u->orders.back().z})==
        (patrol?std::pair{Fixed::fromInt(160),Fixed::fromInt(320)}:
                 std::pair{Fixed::fromInt(1600),Fixed::fromInt(1600)}),"partial delivery preserves the next queued point");
}
uint64_t group(PathfindingMode mode,bool fight,bool serial) {
    World w;setup(w,mode,serial);auto type=soldier();std::vector<int> ids;
    for(int i=0;i<24;++i) {
        const int id=w.spawn(&type,float(160+i%4*48),float(400+i/4*48),0,0);ids.push_back(id);
        if(fight)w.attackMove(id,1600,640,false);else w.order(id,1600,640,false);
    }
    ticks(w,18000);int arrived=0;
    for(int id:ids) {const auto& u=*w.unit(id);arrived+=u.orders.empty();
        if(!u.orders.empty())std::printf("group mode=%d fight=%d id=%d at=%.1f,%.1f radius=%u orders=%zu\n",
            int(mode),fight,id,u.x.toFloat(),u.z.toFloat(),u.orders.back().missionRadius,u.orders.size());}
    check(arrived==int(ids.size()),"every member of a converging move/fight-move group finishes");
    return w.stateHash();
}
void patrolLaps(PathfindingMode mode) {
    World w;setup(w,mode);auto type=soldier();
    // A terrain detour must survive several rounds of route installation and
    // patrol rotation; endpoint coordinates must never become the next mission.
    w.nav().block(38,0,3,49,true);
    const int id=w.spawn(&type,320,640,0,0);w.patrol(id,1280,640);
    int laps=0;bool outbound=false;
    for(int n=0;n<18000;++n) {w.tick(1.f/30);const auto& u=*w.unit(id);
        if(u.x>Fixed::fromInt(1200))outbound=true;
        if(outbound&&u.x<Fixed::fromInt(400)){++laps;outbound=false;}
    }
    check(laps>=3&&!w.unit(id)->orders.empty(),"patrol completes repeated outbound/return laps around terrain");
    for(const auto& order:w.unit(id)->orders)if(order.goal)
        check(order.missionTarget==std::pair{Fixed::fromInt(320),Fixed::fromInt(640)}||
              order.missionTarget==std::pair{Fixed::fromInt(1280),Fixed::fromInt(640)},"patrol endpoints stay canonical after repeated laps");
}
uint64_t production(bool mobile,bool rally,bool serial) {
    World w;setup(w,selectedMode,serial);auto type=soldier(),producer=soldier();
    producer.id=producer.name="producer-test";producer.isBuilder=true;producer.workerTime=1000;
    producer.footX=6;producer.footZ=8;if(!mobile)producer.maxVel=Fixed();
    const int id=w.spawn(&producer,600,600,0,0);w.player(0).mana=1e9;
    w.setRepeat(id,&type);if(rally)w.order(id,1600,640,false);
    bool stopped=false,controllerSeen=false;
    std::map<int,std::pair<Fixed,Fixed>> births;std::set<int> parked;
    bool birthplaceClear=true;
    for(int n=0;n<18000;++n) {
        w.tick(1.f/30);
        for(const auto& u:w.units())if(u.id!=id&&!u.underConstruction) {
            // The exit step records where the output was finished. A factory
            // releases it with GetBuilt's PARK instead, whose ring lies clear
            // of the factory: an output born outside it (this unscripted
            // producer's) may park beside its own birthplace.
            // A rally-only release (any producer's) is first seen where born.
            if(!births.contains(u.id)) {
                for(const auto& order:u.orders) {
                    if(order.productionExit){births.emplace(u.id,*order.productionExit);break;}
                    if(order.park){births.emplace(u.id,std::pair{u.x,u.z});parked.insert(u.id);break;}
                }
                if(!births.contains(u.id) && rally)births.emplace(u.id,std::pair{u.x,u.z});
            }
            if(u.orders.empty()) {
                const auto origin=births.find(u.id);
                const auto& f=*w.unit(id);
                const int ux=footprintOrigin(u.x,type.footX),uz=footprintOrigin(u.z,type.footZ);
                const int fx=footprintOrigin(f.x,producer.footX),fz=footprintOrigin(f.z,producer.footZ);
                birthplaceClear&=origin!=births.end()&&(parked.contains(u.id)
                    ? ux>=fx+producer.footX||ux+type.footX<=fx||uz>=fz+producer.footZ||uz+type.footZ<=fz
                    : std::abs(ux-footprintOrigin(origin->second.first,type.footX))>=type.footX||
                      std::abs(uz-footprintOrigin(origin->second.second,type.footZ))>=type.footZ);
            }
        }
        for(const auto& u:w.units())if(u.id!=id&&!u.underConstruction&&!u.orders.empty()) {
            const auto& goal=u.orders.back();
            if(!rally||goal.x!=Fixed::fromInt(1600))continue;
            controllerSeen|=goal.groundMission&&goal.controller;
        }
        if(!stopped&&w.units().size()>=25) {w.stop(id);stopped=true;}
    }
    int built=0,idle=0;
    for(const auto& u:w.units())if(u.id!=id&&u.alive()&&!u.underConstruction){++built;idle+=u.orders.empty();
        if(!u.orders.empty())std::printf("production mobile=%d rally=%d id=%d at=%.1f,%.1f orders=%zu\n",mobile,rally,u.id,u.x.toFloat(),u.z.toFloat(),u.orders.size());}
    std::printf("production mobile=%d rally=%d serial=%d tick=%u built=%d hash=%016llx\n",mobile,rally,serial,w.tickCount(),built,(unsigned long long)w.stateHash());
    check(stopped&&built>=24,"infinite queue produced the requested observation cohort");
    check(!rally||controllerSeen,"produced rally points receive a ground controller");
    check(idle==built,"all completed outputs stop after their exit/rally move");
    if(selectedMode==PathfindingMode::Legion)
        check(birthplaceClear&&births.size()==size_t(built),"every produced output clears its full birthplace (a parked one, its factory) before stopping");
    return w.stateHash();
}
void mobileFightRally() {
    World w;setup(w,selectedMode);auto type=soldier(),builder=soldier();builder.isBuilder=true;
    const int id=w.spawn(&builder,320,640,0,0);w.setRepeat(id,&type);w.attackMove(id,1600,640,false);
    check(w.unit(id)->orders.empty()&&w.unit(id)->rally.size()==1&&w.unit(id)->rally.front().attackMove,
        "infinite mobile producer retains the fight-move rally flag");
}
// `queued`: the player Shift-queues a move onto the output during its exit.
void exitRally(PathfindingMode mode,bool queued) {
    World w;setup(w,mode);
    auto type=soldier(),producer=soldier();producer.isBuilder=true;producer.workerTime=1000;
    const int id=w.spawn(&producer,600,600,0,0);w.player(0).mana=1e9;
    w.setRepeat(id,&type);
    int child=0;
    for(int tick=0;tick<600&&!child;++tick) {
        w.tick(1.f/30);
        for(const auto& u:w.units())if(u.id!=id&&u.alive()&&!u.underConstruction){child=u.id;break;}
    }
    check(child!=0,"exit/rally fixture produced an output");if(!child)return;
    // A repeating mobile producer's output parks on the factory ring round it
    // (engine deviation), in either mode, instead of the exit step.
    check(w.unit(child)->orders[World::currentLeg(w.unit(child)->orders)].park.has_value(),
          "a repeating mobile producer's output parks round it");
    w.stop(id);
    if(queued)w.order(child,1600,640,true);
    auto* u=w.unit(child);u->x=Fixed::fromInt(1000);u->z=Fixed::fromInt(900);u->bodyBlockStreak=2;
    ticks(w,5);
    bool rallyActive=false;
    for(const auto& order:w.unit(child)->orders)
        if(order.goal){rallyActive=order.missionTarget==std::pair{Fixed::fromInt(1600),Fixed::fromInt(640)};break;}
    check(!rallyActive,"a blocked exit does not hand over to the order behind it early");
}
void combatResume(PathfindingMode mode,bool patrol) {
    World w;setup(w,mode);auto type=soldier(),enemy=soldier();enemy.maxVel=Fixed();
    // Upright, so both stand on the terrain: a patrol's scan (403600's
    // 4d8370) takes only an enemy its player sees, and a body left at y 0
    // under the height-100 ground falls on the wrong sight cells.
    type.upright=enemy.upright=true;
    // Ten shots to kill, so it outlives the first shot of the walker's
    // fire-at-will sweep and the patrol's own scan (a timer wake, 5 +
    // rand(10)) gets to put its attack in.
    enemy.maxHp=1000;
    Weapon weapon;weapon.name="order-test-weapon";weapon.range=120;weapon.damage=100;
    weapon.reload=0.1f;weapon.aimTol=32767;type.weapon=weapon;type.weapons.push_back(weapon);
    const int id=w.spawn(&type,320,640,0,0),target=w.spawn(&enemy,600,640,0,1);
    if(patrol)w.patrol(id,1400,640);else w.attackMove(id,1400,640,false);
    bool engaged=false,retained=true,arrived=false;
    for(int n=0;n<3000;++n) {
        w.tick(1.f/30);const auto& u=*w.unit(id);
        if(!u.orders.empty()&&u.orders.front().targetId==target) {
            engaged=true;bool point=false;
            for(const auto& o:u.orders)point|=o.groundMission&&o.missionTarget==
                std::pair{Fixed::fromInt(1400),Fixed::fromInt(640)};
            retained&=point;
        }
        arrived|=u.x>Fixed::fromInt(1300);
    }
    const auto* remaining=w.unit(target);
    check(engaged&&retained&&(!remaining||!remaining->alive())&&arrived,
        "fight-move/patrol engages an enemy and resumes its original destination");
}
// Legion routes these missions; the other modes leave them to their own
// navigation. Records whether the navigator ever served `kind` for `id`.
void sawLegion(World& w,int id,LegionMission kind,bool& seen) {
    if(auto* legion=w.legionNavigator();legion&&w.unit(id))seen|=legion->mission(*w.unit(id))==kind;
}
UnitType armed() {
    auto type=soldier();
    Weapon weapon;weapon.name="order-test-weapon";weapon.range=120;weapon.damage=100;
    weapon.reload=0.1f;weapon.aimTol=32767;type.weapon=weapon;type.weapons.push_back(weapon);
    return type;
}
// Attack approach around terrain, then a chase of a target that walks away
// behind the same wall: both must close to firing range and kill.
void attackChase(PathfindingMode mode,bool moving) {
    World w;setup(w,mode);auto type=armed(),enemy=soldier();
    if(!moving)enemy.maxVel=Fixed();else enemy.maxVel=Fixed::fromFloat(30.f/30);
    w.nav().block(38,0,3,49,true);
    const int id=w.spawn(&type,320,640,0,0),target=w.spawn(&enemy,900,640,0,1);
    if(moving)w.order(target,900,1500,false);
    w.attack(id,target,false);
    bool legion=false;int killedAt=-1;
    for(int n=0;n<6000&&killedAt<0;++n) {
        w.tick(1.f/30);sawLegion(w,id,LegionMission::Attack,legion);
        const auto* t=w.unit(target);if(!t||!t->alive())killedAt=n;
    }
    std::printf("attack mode=%d moving=%d killedAt=%d legion=%d\n",int(mode),moving,killedAt,legion);
    check(killedAt>=0,moving?"attack chases a target walking away behind terrain and kills it":
                             "attack approach routes around terrain into firing range and kills");
    if(mode==PathfindingMode::Legion)check(legion,"Legion routes the attack approach");
}
// Guard follows its charge around a wall and stays close; the order is kept.
void guardFollow(PathfindingMode mode) {
    World w;setup(w,mode);auto type=soldier();
    w.nav().block(38,0,3,49,true);
    const int charge=w.spawn(&type,320,640,0,0),escort=w.spawn(&type,240,560,0,0);
    w.guard(escort,charge,false);w.order(charge,1280,640,false);
    bool legion=false;
    for(int n=0;n<6000;++n){w.tick(1.f/30);sawLegion(w,escort,LegionMission::Guard,legion);}
    const auto& c=*w.unit(charge);const auto& e=*w.unit(escort);
    const float d=fxLen(c.x-e.x,c.z-e.z).toFloat();
    std::printf("guard mode=%d dist=%.1f orders=%zu legion=%d\n",int(mode),d,e.orders.size(),legion);
    // Follow_Ground's ring for a 2x2 guard of a 2x2 charge: 96..144 px (origin
    // cells, so up to a cell's rounding past either edge).
    check(c.orders.empty()&&d>=80.f&&d<=160.f&&!e.orders.empty()&&e.orders.front().guard,
          "guard follows its charge around terrain and stays beside it");
    if(mode==PathfindingMode::Legion)check(legion,"Legion routes the guard escort");
}
// Work approaches around the same wall: a builder repairs a damaged ally,
// then walks to and puts up a site. The work handlers keep reach, start and
// retirement; Legion (in --legion) only routes the approaches.
void workApproach(PathfindingMode mode) {
    World w;setup(w,mode);auto worker=soldier(),ally=soldier();
    worker.isBuilder=true;worker.workerTime=1000;worker.buildDist=48;
    ally.buildTime=1;ally.buildCost=0;ally.maxVel=Fixed();ally.canMove=false;
    UnitType site{};site.id=site.name="work-site";site.maxVel=Fixed();site.footX=site.footZ=4;
    site.maxHp=100;site.buildTime=1;site.buildCost=0;
    // A water wall on the placement plane (on the legacy nav-grid plane
    // Legion holds a body, a plain Move as well, at a blocked wall's end:
    // docs/legion-pathfinding.md "Scope").
    std::vector<uint8_t> heights(size_t(128)*128,100);
    for(int z=0;z<49;++z)for(int x=38;x<41;++x)heights[size_t(z)*128+x]=0;
    w.setTerrain(heights,128,128,40);w.setMapPlacementFeatures(std::vector<uint16_t>(size_t(128)*128,0xffff),{});
    w.player(0).mana=1e9;
    const int builder=w.spawn(&worker,240,560,0,0),hurt=w.spawn(&ally,1100,400,0,0);
    w.unit(hurt)->hp=Fixed::fromInt(10);
    w.repair(builder,hurt,false);w.queueBuild(builder,&site,1100,700,true);
    bool repaired=false,legionRepair=false,legionBuild=false;int built=0;
    for(int n=0;n<9000&&!(built&&!w.unit(built)->underConstruction);++n) {
        w.tick(1.f/30);
        sawLegion(w,builder,LegionMission::Repair,legionRepair);sawLegion(w,builder,LegionMission::Build,legionBuild);
        repaired|=w.unit(hurt)->hp>=Fixed::fromFloat(ally.maxHp);
        for(const auto& u:w.units())if(u.type==&site&&u.alive())built=u.id;
    }
    std::printf("work mode=%d repaired=%d built=%d tick=%u legion=%d/%d\n",int(mode),repaired,built,w.tickCount(),legionRepair,legionBuild);
    if(auto* legion=w.legionNavigator())
        check(!legion->staticLegal(*w.unit(builder),39,20),"Legion plans around the wall (not through it)");
    check(repaired,"builder walks around terrain and repairs its ally");
    check(built&&!w.unit(built)->underConstruction,"builder walks around terrain and finishes its queued site");
    if(mode==PathfindingMode::Legion)check(legionRepair&&legionBuild,"Legion routes the repair and build approaches");
}
// A group patrol: twelve bodies loop between their starts and one shared
// point; every one keeps lapping. (Open ground: patrolLaps covers a single
// patrol around terrain. On this legacy nav-grid world a 12-body column
// jams at the wall's end in Legion for plain queued moves as well.)
uint64_t groupPatrol(PathfindingMode mode,bool serial) {
    World w;setup(w,mode,serial);auto type=soldier();
    std::vector<int> ids;
    for(int i=0;i<12;++i) {const int id=w.spawn(&type,float(240+i%3*48),float(560+i/3*48),0,0);ids.push_back(id);w.patrol(id,1280,640);}
    std::vector<int> laps(ids.size(),0);std::vector<bool> outbound(ids.size(),false);bool legion=false;
    for(int n=0;n<18000;++n) {
        w.tick(1.f/30);
        for(size_t k=0;k<ids.size();++k) {
            const auto& u=*w.unit(ids[k]);sawLegion(w,ids[k],LegionMission::Patrol,legion);
            if(u.x>Fixed::fromInt(1150))outbound[k]=true;
            if(outbound[k]&&u.x<Fixed::fromInt(450)){++laps[k];outbound[k]=false;}
        }
    }
    const int least=*std::min_element(laps.begin(),laps.end());
    std::printf("group patrol mode=%d least laps=%d legion=%d\n",int(mode),least,legion);
    check(least>=2,"every member of a group patrol keeps lapping around terrain");
    if(mode==PathfindingMode::Legion)check(legion,"Legion routes the group patrol");
    return w.stateHash();
}
// ---- Shift-queued player orders ----------------------------------------
// Each order arrives through applyCommand exactly as the client sends it
// (queue=1 is Shift), and must run after everything queued before it.
void send(World& w,tak::net::Cmd kind,int id,float x,float z,bool queue,int target=0) {
    static const TypeRegistry none;
    tak::net::Command c;c.kind=kind;c.player=uint8_t(w.unit(id)->player);c.unitId=id;
    c.x=x;c.z=z;c.queue=queue;c.targetId=target;
    // Over the wire first: the Shift flag must survive the command encoding.
    tak::net::Writer out;out.cmd(c);tak::net::Reader in(out.b.data(),out.b.size());
    const auto wire=in.cmd();
    if(!in.ok||wire.kind!=kind||wire.queue!=uint8_t(queue)||wire.targetId!=target) {
        check(false,"a queued order survives the command encoding");return;
    }
    applyCommand(w,none,wire);
}
bool near(const Unit& u,float x,float z,float r=40) {
    return fxLen(u.x-Fixed::fromFloat(x),u.z-Fixed::fromFloat(z))<=Fixed::fromFloat(r);
}
// Visits `points` in order (each within 40px), ticking at most `limit`.
// Returns how many were reached in sequence.
int visits(World& w,int id,const std::vector<std::pair<float,float>>& points,int limit) {
    size_t next=0;
    for(int n=0;n<limit&&next<points.size();++n) {
        w.tick(1.f/30);
        if(near(*w.unit(id),points[next].first,points[next].second))++next;
    }
    return int(next);
}
// The sequence of `points` the unit stands at (within 40px), each new
// entry recorded once on arrival, until `count` are seen or `limit` ticks.
std::vector<int> route(World& w,int id,const std::vector<std::pair<float,float>>& points,size_t count,int limit) {
    std::vector<int> seen;int at=-1;
    for(int n=0;n<limit&&seen.size()<count;++n) {
        w.tick(1.f/30);int here=-1;
        for(size_t k=0;k<points.size();++k)if(near(*w.unit(id),points[k].first,points[k].second))here=int(k);
        if(here>=0&&here!=at)seen.push_back(here);
        if(here>=0)at=here;
    }
    return seen;
}
void queuedMoves(PathfindingMode mode) {
    using tak::net::Cmd;
    {   // Move, then two Shift-moves: three legs in order, then idle at the last.
        World w;setup(w,mode);auto type=soldier();const int id=w.spawn(&type,320,320,0,0);
        send(w,Cmd::Move,id,1200,320,false);send(w,Cmd::Move,id,1200,1200,true);send(w,Cmd::Move,id,320,1200,true);
        check(visits(w,id,{{1200,320},{1200,1200},{320,1200}},9000)==3,"shift-moves run in the order they were queued");
        ticks(w,300);check(w.unit(id)->orders.empty()&&near(*w.unit(id),320,1200),"the last queued move finishes and the unit idles there");
    }
    // Points: 0 start, 1 east, 2 south-east, 3 south.
    const std::vector<std::pair<float,float>> square{{320,320},{1200,320},{1200,1200},{320,1200}};
    {   // Move, then Shift-patrol: walk out, then lap between the move's end and the patrol point.
        World w;setup(w,mode);auto type=soldier();const int id=w.spawn(&type,320,320,0,0);
        send(w,Cmd::Move,id,1200,320,false);send(w,Cmd::Patrol,id,1200,1200,true);
        check(route(w,id,square,7,15000)==std::vector<int>{0,1,2,1,2,1,2},
              "a Shift-patrol after a move laps between the move's end and the patrol point");
    }
    {   // Shift-patrols from idle: the loop is start, B, C (retail's return point).
        World w;setup(w,mode);auto type=soldier();const int id=w.spawn(&type,320,320,0,0);
        send(w,Cmd::Patrol,id,1200,320,true);send(w,Cmd::Patrol,id,1200,1200,true);
        check(route(w,id,square,7,18000)==std::vector<int>{0,1,2,0,1,2,0},
              "Shift-patrols from idle lap start, B, C in order");
    }
    {   // Move, then two Shift-patrols: lap the move's end, B, C.
        World w;setup(w,mode);auto type=soldier();const int id=w.spawn(&type,320,320,0,0);
        send(w,Cmd::Move,id,1200,320,false);send(w,Cmd::Patrol,id,1200,1200,true);send(w,Cmd::Patrol,id,320,1200,true);
        check(route(w,id,square,8,18000)==std::vector<int>{0,1,2,3,1,2,3,1},
              "Shift-patrols after a move lap the move's end and every patrol point in order");
    }
    {   // Patrol, then Shift-patrol: appended to the running loop (start, B, then C).
        World w;setup(w,mode);auto type=soldier();const int id=w.spawn(&type,320,320,0,0);
        send(w,Cmd::Patrol,id,1200,320,false);send(w,Cmd::Patrol,id,1200,1200,true);
        check(route(w,id,square,7,18000)==std::vector<int>{0,1,0,2,1,0,2},
              "Shift-patrol joins a running patrol loop at its tail");
    }
    {   // Attack, then Shift-move: kill the target first, then walk on.
        World w;setup(w,mode);auto type=armed(),enemy=soldier();enemy.maxVel=Fixed();
        const int id=w.spawn(&type,320,640,0,0),target=w.spawn(&enemy,800,640,0,1);
        send(w,Cmd::Attack,id,0,0,false,target);send(w,Cmd::Move,id,800,1200,true);
        int killed=-1,arrived=-1;
        for(int n=0;n<9000&&arrived<0;++n) {
            w.tick(1.f/30);const auto* t=w.unit(target);
            if(killed<0&&(!t||!t->alive()))killed=n;
            if(near(*w.unit(id),800,1200))arrived=n;
        }
        check(killed>=0&&arrived>killed,"attack then Shift-move: the target dies, then the queued move runs");
    }
    {   // Fight-move, then Shift-patrol: engage en route, reach the fight point, then patrol from there.
        World w;setup(w,mode);auto type=armed(),enemy=soldier();enemy.maxVel=Fixed();
        // Upright, as combatResume's: the fight-move's scan (Move response
        // mode -1, 4d8370) takes only an enemy its player sees.
        type.upright=enemy.upright=true;
        const int id=w.spawn(&type,320,640,0,0),target=w.spawn(&enemy,700,640,0,1);
        send(w,Cmd::AttackMove,id,1200,640,false);send(w,Cmd::Patrol,id,1200,1400,true);
        const int reached=visits(w,id,{{1200,640},{1200,1400},{1200,640},{1200,1400}},12000);
        const auto* t=w.unit(target);
        check(reached==4&&(!t||!t->alive()),"fight-move then Shift-patrol: engages en route, then laps from the fight point");
    }
    {   // Move, then Shift-guard: the guard takes over once the move is done.
        World w;setup(w,mode);auto type=armed();
        const int id=w.spawn(&type,320,320,0,0),charge=w.spawn(&type,1200,1200,0,0);
        send(w,Cmd::Move,id,1200,320,false);send(w,Cmd::Guard,id,0,0,true,charge);
        const bool moved=visits(w,id,{{1200,320}},6000)==1;
        ticks(w,1200);
        const auto& u=*w.unit(id);
        check(moved&&!u.orders.empty()&&u.orders.front().guard&&near(u,1200,1200,160),"move then Shift-guard: walks out, then guards");
    }
    {   // Move, Shift-repair, Shift-move: walk out, mend the ally, walk on.
        World w;setup(w,mode);auto worker=soldier(),ally=soldier();
        worker.isBuilder=true;worker.workerTime=1000;worker.buildDist=48;
        ally.maxVel=Fixed();ally.canMove=false;w.player(0).mana=1e9;
        const int id=w.spawn(&worker,320,320,0,0),hurt=w.spawn(&ally,1200,640,0,0);
        w.unit(hurt)->hp=Fixed::fromInt(10);
        send(w,Cmd::Move,id,1200,320,false);send(w,Cmd::Repair,id,0,0,true,hurt);send(w,Cmd::Move,id,320,1200,true);
        int out=-1,mended=-1,back=-1;
        for(int n=0;n<9000&&back<0;++n) {
            w.tick(1.f/30);const auto& u=*w.unit(id);
            if(out<0&&near(u,1200,320))out=n;
            if(mended<0&&w.unit(hurt)->hp>=Fixed::fromFloat(ally.maxHp))mended=n;
            if(near(u,320,1200))back=n;
        }
        check(out>=0&&mended>out&&back>mended,"move, Shift-repair, Shift-move run in order");
    }
    {   // Move, Shift-reclaim, Shift-move: the feature goes only after the first move.
        World w;setup(w,mode);auto worker=soldier();
        worker.isBuilder=worker.canReclaim=true;worker.workerTime=1000;worker.buildDist=48;
        const int id=w.spawn(&worker,320,320,0,0);w.addFeature(31,1200,700,1,1,1,false);
        send(w,Cmd::Move,id,1200,320,false);send(w,Cmd::Reclaim,id,0,0,true,31);send(w,Cmd::Move,id,320,1200,true);
        int out=-1,gone=-1,back=-1;
        for(int n=0;n<9000&&back<0;++n) {
            w.tick(1.f/30);const auto& u=*w.unit(id);const auto* f=w.feature(31);
            if(out<0&&near(u,1200,320))out=n;
            if(gone<0&&(!f||!f->alive))gone=n;
            if(near(u,320,1200))back=n;
        }
        check(out>=0&&gone>out&&back>gone,"move, Shift-reclaim, Shift-move run in order");
    }
    {   // Fight-move, then Shift-move: queued attack-move legs complete like moves.
        World w;setup(w,mode);auto type=armed();const int id=w.spawn(&type,320,320,0,0);
        send(w,Cmd::AttackMove,id,1200,320,false);send(w,Cmd::AttackMove,id,1200,1200,true);send(w,Cmd::Move,id,320,1200,true);
        check(visits(w,id,{{1200,320},{1200,1200},{320,1200}},9000)==3,"fight-move legs queue with moves in order");
    }
}
// Shift-queued patrols for aircraft, the same routes as the ground cases in
// queuedMoves. A patrol leg that starts with no loop behind it adds a return
// point where the flyer then stands. Armed and builder flyers (ordinary
// VTOL_Move, not the unarmed patrol handler) used to fly to a lone queued
// point and hover there for good. A flight point is accepted well short of
// its centre, so arrival here is within 200px.
UnitType aircraft(bool weapon,bool builder) {
    UnitType t=weapon?armed():soldier();t.id=t.name="order-test-flyer";
    t.canFly=t.canMove=true;t.maxVel=Fixed::fromInt(4);t.accel=t.brake=Fixed::fromInt(1);
    t.turnRate=1200;t.cruiseAlt=80;
    if(builder) {t.isBuilder=true;t.workerTime=100;t.buildDist=48;}
    return t;
}
std::vector<int> flyRoute(World& w,int id,const std::vector<std::pair<float,float>>& points,size_t count,int limit) {
    std::vector<int> seen;int at=-1;
    for(int n=0;n<limit&&seen.size()<count;++n) {
        w.tick(1.f/30);int here=-1;
        for(size_t k=0;k<points.size();++k)if(near(*w.unit(id),points[k].first,points[k].second,200))here=int(k);
        if(here>=0&&here!=at)seen.push_back(here);
        if(here>=0)at=here;
    }
    return seen;
}
void queuedFlyerPatrols(PathfindingMode mode) {
    using tak::net::Cmd;
    const std::vector<std::pair<float,float>> square{{320,320},{1200,320},{1200,1200},{320,1200}};
    const struct {bool weapon,builder;const char* name;} kinds[]{
        {false,false,"unarmed flyer"},{true,false,"armed flyer"},{false,true,"flying builder"}};
    for(const auto& kind:kinds) {
        char label[160];
        auto report=[&](const std::vector<int>& got,const char* what) {
            std::printf("  %s %s:",kind.name,what);for(int k:got)std::printf(" %d",k);std::printf("\n");
        };
        {   World w;setup(w,mode);auto type=aircraft(kind.weapon,kind.builder);const int id=w.spawn(&type,320,320,0,0);
            send(w,Cmd::Move,id,1200,320,false);send(w,Cmd::Patrol,id,1200,1200,true);
            const auto got=flyRoute(w,id,square,7,15000);report(got,"move+shift-patrol");
            std::snprintf(label,sizeof label,"%s: a Shift-patrol after a move laps between the move's end and the patrol point",kind.name);
            check(got==std::vector<int>{0,1,2,1,2,1,2},label);
        }
        {   World w;setup(w,mode);auto type=aircraft(kind.weapon,kind.builder);const int id=w.spawn(&type,320,320,0,0);
            send(w,Cmd::Patrol,id,1200,1200,true);
            const auto got=flyRoute(w,id,square,5,15000);report(got,"lone shift-patrol");
            std::snprintf(label,sizeof label,"%s: a lone Shift-patrol from idle laps between its start and the patrol point",kind.name);
            check(got==std::vector<int>{0,2,0,2,0},label);
        }
        {   World w;setup(w,mode);auto type=aircraft(kind.weapon,kind.builder);const int id=w.spawn(&type,320,320,0,0);
            send(w,Cmd::Patrol,id,1200,320,true);send(w,Cmd::Patrol,id,1200,1200,true);
            const auto got=flyRoute(w,id,square,7,18000);report(got,"two shift-patrols");
            std::snprintf(label,sizeof label,"%s: Shift-patrols from idle lap start, B, C in order",kind.name);
            check(got==std::vector<int>{0,1,2,0,1,2,0},label);
        }
        // Every flyer's patrol is VTOL_Patrol: a running (non-queued) patrol's
        // return leg is its anchor, so a Shift-patrol joins the loop at its tail.
        {   World w;setup(w,mode);auto type=aircraft(kind.weapon,kind.builder);const int id=w.spawn(&type,320,320,0,0);
            send(w,Cmd::Patrol,id,1200,320,false);send(w,Cmd::Patrol,id,1200,1200,true);
            const auto got=flyRoute(w,id,square,7,18000);report(got,"patrol+shift-patrol");
            std::snprintf(label,sizeof label,"%s: a Shift-patrol joins a running patrol loop at its tail",kind.name);
            check(got==std::vector<int>{0,1,0,2,1,0,2},label);
        }
    }
}
// A selection's Shift-queued group moves: every member is sent the SAME
// point per click (Legion packs it into arrival slots), and every leg is
// a group leg when it becomes current.
uint64_t queuedGroupMoves(PathfindingMode mode,bool serial) {
    using tak::net::Cmd;
    World w;setup(w,mode,serial);auto type=soldier();std::vector<int> ids;
    for(int i=0;i<12;++i)ids.push_back(w.spawn(&type,float(240+i%3*48),float(320+i/3*48),0,0));
    const bool shared=mode==PathfindingMode::Legion;
    auto click=[&](float x,float z,bool queue) {
        float cx=0,cz=0;for(int id:ids){cx+=w.unit(id)->x.toFloat();cz+=w.unit(id)->z.toFloat();}
        cx/=float(ids.size());cz/=float(ids.size());
        for(int id:ids) {const auto& u=*w.unit(id);
            send(w,Cmd::Move,id,shared?x:x+std::clamp(u.x.toFloat()-cx,-60.f,60.f),
                 shared?z:z+std::clamp(u.z.toFloat()-cz,-60.f,60.f),queue);}
    };
    click(1300,400,false);click(1300,1400,true);click(400,1400,true);
    std::vector<int> stage(ids.size(),0);bool legionSecond=false;
    const std::pair<float,float> goals[3]={{1300,400},{1300,1400},{400,1400}};
    for(int n=0;n<24000;++n) {
        w.tick(1.f/30);bool all=true;
        for(size_t k=0;k<ids.size();++k) {
            const auto& u=*w.unit(ids[k]);
            if(stage[k]<3&&near(u,goals[stage[k]].first,goals[stage[k]].second,200))++stage[k];
            if(stage[k]==1)sawLegion(w,ids[k],LegionMission::Move,legionSecond);
            all&=stage[k]==3&&u.orders.empty();
        }
        if(all)break;
    }
    const int least=*std::min_element(stage.begin(),stage.end());int idle=0;
    for(int id:ids)idle+=w.unit(id)->orders.empty();
    std::printf("queued group mode=%d least=%d idle=%d legion=%d tick=%u\n",int(mode),least,idle,legionSecond,w.tickCount());
    check(least==3&&idle==int(ids.size()),"every member runs a selection's queued group moves in order and stops");
    if(mode==PathfindingMode::Legion)check(legionSecond,"Legion routes a queued group move when it becomes current");
    return w.stateHash();
}
void mazeProduction(const char* data) {
    auto vfs=tak::hpi::mountRetailRoot(data);
    TypeRegistry registry;setupRegistry(registry,vfs,true);
    constexpr auto recipe="~gen1~0a00f0a981b8b476a2cb010008000808ffffff67ff03000000004a616e6b204d617a65207631";
    auto generated=tak::mapgen::generate(tak::mapgen::decodeMapId(recipe),vfs);
    auto files=std::make_shared<tak::hpi::Vfs::Files>();(*files)["maps/orders-maze.tnt"]=generated.map.save();vfs.setMapFiles(files);
    for(bool mobile:{false,true})for(bool rally:{false,true}) {
        World w;w.setSerialThreads(true);
        MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath="maps/orders-maze.tnt";cfg.loadCrt=false;
        cfg.pathfindingMode=selectedMode;cfg.slots.resize(1);setupMatch(w,registry,cfg);
        auto hunter=*registry.find("zonter");hunter.weapons.clear();hunter.weapon.damage=0;
        // Keep authored Hunter movement/footprints/scripts; accelerate production
        // so this fixture measures exit/rally behavior rather than the economy.
        hunter.buildTime=1;hunter.buildCost=0;
        UnitType producer{};producer.id="maze-producer";producer.footX=6;producer.footZ=8;
        producer.maxHp=10000;producer.isBuilder=true;producer.workerTime=1000;
        // A UnitType's default maxVel is mobile: a building must clear it.
        producer.maxVel=mobile?hunter.maxVel:Fixed();producer.canMove=mobile;
        const int id=w.spawn(&producer,8448,24320,0,0);w.setRepeat(id,&hunter);
        if(rally)w.order(id,4864,16640,false);
        w.updateNavigationExploration();auto& known=w.editNavigationExploration();
        std::fill(known.begin(),known.end(),0xffff);
        bool stopped=false;int built=0,idle=0;
        for(int tick=0;tick<120000;++tick) {
            w.tick(1.f/30);
            if(!stopped&&w.units().size()>=17){w.stop(id);stopped=true;}
            built=idle=0;
            for(const auto& u:w.units())if(u.id!=id&&u.alive()&&!u.underConstruction){++built;idle+=u.orders.empty();}
            if(stopped&&built==idle)break;
        }
        std::printf("maze mobile=%d rally=%d tick=%u built=%d idle=%d hash=%016llx\n",
            mobile,rally,w.tickCount(),built,idle,(unsigned long long)w.stateHash());
        check(stopped&&built==16&&idle==built,"all Hunters finish production exit/rally moves on the actual 64x64 maze");
    }
}
uint64_t trollRally(const TypeRegistry& registry,bool serial,const tak::hpi::Vfs* maze=nullptr,int count=100) {
    World w;w.setSerialThreads(serial);
    if(maze) {
        MatchConfig cfg;cfg.vfs=maze;cfg.mapPath="maps/orders-troll-maze.tnt";cfg.loadCrt=false;
        cfg.pathfindingMode=selectedMode;cfg.slots.resize(1);setupMatch(w,registry,cfg);
    } else {
        w.setPathfindingMode(selectedMode);
        w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
        w.buildNavClasses(registry);w.setPathService(true);
    }
    auto producer=*registry.find("zonhand"),troll=*registry.find("zontroll");
    // Preserve authored movement, footprint and scripts. Speed up only the
    // economy to expose continued arrivals beyond the earlier 16-unit tests.
    producer.workerTime=1000;producer.storage=10000000;
    troll.buildTime=1;troll.buildCost=0;troll.weapons.clear();troll.weapon.damage=0;
    const int id=w.spawn(&producer,maze?8448:600,maze?24320:600,0,0);
    w.player(0).mana=10000000;w.setRepeat(id,&troll);
    w.order(id,maze?4864:1600,maze?16640:640,false);
    w.updateNavigationExploration();auto& known=w.editNavigationExploration();
    std::fill(known.begin(),known.end(),0xffff);
    bool stopped=false,legionExit=false;int built=0,idle=0;
    for(int tick=0;tick<90000;++tick) {
        w.tick(1.f/30);built=idle=0;
        for(const auto& u:w.units())if(u.id!=id&&u.alive()&&!u.underConstruction){++built;idle+=u.orders.empty();
            sawLegion(w,u.id,LegionMission::Exit,legionExit);}
        if(!stopped&&built>=count){w.stop(id);stopped=true;}
        if(stopped&&built==idle)break;
    }
    std::printf("Troll rally maze=%d serial=%d tick=%u built=%d idle=%d hash=%016llx\n",
        maze!=nullptr,serial,w.tickCount(),built,idle,(unsigned long long)w.stateHash());
    for(const auto& u:w.units())if(u.id!=id&&u.alive()&&!u.underConstruction&&!u.orders.empty())
        std::printf("unsettled Troll id=%d at=%.1f,%.1f orders=%zu legion=%d\n",u.id,u.x.toFloat(),u.z.toFloat(),u.orders.size(),
            w.legionNavigator()?int(w.legionNavigator()->mission(u)):-1);
    check(stopped&&built==count&&idle==built,"all Beast Handler Trolls finish their shared rally orders");
    if(selectedMode==PathfindingMode::Legion)check(!legionExit,"no Troll takes a production exit ahead of its rally");
    if(!maze)for(const auto& u:w.units())if(u.id!=id&&u.alive()&&!u.underConstruction) {
        const int64_t dx=u.x.floorInt()-1600,dz=u.z.floorInt()-640;
        const int64_t radius=int64_t(count)*48+32;
        check(dx*dx+dz*dz<=radius*radius,"Troll stopped outside its shared rally formation");
    }
    return w.stateHash();
}
// `open`: only the open-ground cohorts. `maze`: only the generated-maze
// cohort, serial and threaded.
void trollProduction(const char* data,bool open=false,bool mazeOnly=false) {
    auto vfs=tak::hpi::mountRetailRoot(data);
    for(bool crusades:{false,true}) {
        TypeRegistry registry;setupRegistry(registry,vfs,crusades);
        if(mazeOnly&&!crusades)continue;
        if(!mazeOnly) {
            const auto serial=trollRally(registry,true);
            check(serial==trollRally(registry,false),"authored Troll arrivals match serial and threaded preparation");
        }
        if(crusades) {
            if(!mazeOnly) {
                const auto large=trollRally(registry,true,nullptr,256);
                check(large==trollRally(registry,false,nullptr,256),"256-Troll arrivals match serial and threaded preparation");
            }
            constexpr auto recipe="~gen1~0a00f0a981b8b476a2cb010008000808ffffff67ff03000000004a616e6b204d617a65207631";
            auto generated=tak::mapgen::generate(tak::mapgen::decodeMapId(recipe),vfs);
            auto files=std::make_shared<tak::hpi::Vfs::Files>();(*files)["maps/orders-troll-maze.tnt"]=generated.map.save();
            if(!open) {
                vfs.setMapFiles(files);
                const auto serial=trollRally(registry,true,&vfs);
                if(mazeOnly)check(serial==trollRally(registry,false,&vfs),"maze Troll arrivals match serial and threaded preparation");
            }
        }
    }
}
}
// A fast member of a Legion formation walks at its slowest member's pace,
// steadily: its headway window measures against that pace, so its scripts see
// it walking (legionStill false, GET 29 above 0). Measured against its own top
// speed, a member held to under a quarter of it read still and slid along with
// its legs standing (an Aramon Horseman with a Rolling Tower).
void formationGait(PathfindingMode mode) {
    World w;setup(w,mode);
    auto fast=soldier(),slow=soldier();fast.id="fast";slow.id="slow";
    slow.maxVel=Fixed::fromFloat(14.f/30);   // a fifth of the soldier's 70/30
    const int a=w.spawn(&fast,400,640,0,0),b=w.spawn(&slow,400,720,0,0);
    w.setSquad(a,-1);w.setSquad(b,-1);
    ticks(w,10);
    w.order(a,2400,640,false);w.order(b,2400,720,false);
    int moving=0,still=0;
    for(int t=0;t<600;++t) {
        const auto x=w.unit(a)->x;w.tick(1.f/30);
        if(t<60||w.unit(a)->x==x)continue;
        ++moving;still+=w.unit(a)->legionStill;
    }
    std::printf("formation gait mode=%d moving=%d still=%d\n",int(mode),moving,still);
    check(moving>300&&still*10<moving,"a formation's fast member held to its slowest member's pace walks, not stands");
}
int main(int argc,char** argv) {
    if(argc>1&&std::string(argv[1])=="--legion") {
        selectedMode=PathfindingMode::Legion;
        --argc;++argv;
    }
    if((argc==3||(argc==4&&(std::string(argv[3])=="open"||std::string(argv[3])=="maze")))&&std::string(argv[1])=="--trolls") {
        try {trollProduction(argv[2],argc==4&&std::string(argv[3])=="open",argc==4&&std::string(argv[3])=="maze");return failures?1:0;}
        catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
    }
    if(argc==2) {
        try {mazeProduction(argv[1]);return failures?1:0;}
        catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
    }
    if(argc!=1)return 2;
    const std::vector<PathfindingMode> modes{selectedMode};
    for(auto mode:modes) {
        for(bool patrol:{false,true})for(bool clipped:{false,true})partial(mode,patrol,clipped);
        group(mode,false,true);group(mode,true,true);patrolLaps(mode);
        combatResume(mode,false);combatResume(mode,true);
        // Mission-goal routing (attack, chase, guard, group patrol): the
        // default run covers Retail, --legion covers Legion.
        attackChase(mode,false);attackChase(mode,true);guardFollow(mode);workApproach(mode);
        const auto patrolHash=groupPatrol(mode,true);
        check(patrolHash==groupPatrol(mode,false),"group patrol hashes match serial and threaded preparation");
        queuedMoves(mode);queuedFlyerPatrols(mode);formationGait(mode);
        const auto queuedHash=queuedGroupMoves(mode,true);
        check(queuedHash==queuedGroupMoves(mode,false),"queued group move hashes match serial and threaded preparation");
    }
    const auto serial=group(selectedMode,false,true);
    check(serial==group(selectedMode,false,false),"shared-navigation group hashes match serial and threaded preparation");
    for(bool mobile:{false,true})for(bool rally:{false,true}) {
        const auto hash=production(mobile,rally,true);
        check(hash==production(mobile,rally,false),"production hashes match serial and threaded preparation");
    }
    for(auto mode:modes)
        for(bool far:{false,true})exitRally(mode,far);
    mobileFightRally();std::printf("movement orders: %d failures\n",failures);return failures?1:0;
}
