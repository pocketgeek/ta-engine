// transport_test -- what unloadAt has to get right about the approach.
//
// The transport used to route itself: unloadAt ran NavGrid::findPath (a synchronous A*
// on the sim thread) and pushed its waypoints ahead of the unload order. That A* was
// the only caller of findPath and resolved its grid with the same navFor() the async
// boundary tracer uses, so it was removed and unloadAt asks the tracer instead.
//
// The native surface-carrier order (TotalA.exe GROUND_UNLOAD 0x406900) calls
// TransportDrop first; out of reach it aims at the exact requested position with a
// circle of radius 1.5 x footprintz x 16 px for a hover, else 0 (0x406a23..0x406a5e).
// The navigator chooses a reachable, fitting endpoint in that circle; preselecting a
// nearby cell and issuing an ordinary move can choose a different point or
// disconnected water. Destinations already within reach unload at the first call.

// TWO: the shape of the queue. The tracer installs routes with replaceLeg(), which
// rebuilds the current leg -- the first order flagged `goal` -- out of route waypoints,
// carrying only the leg's movement flags across. `unload` is not one of them. So if the
// unload order were itself the leg, a route arriving for it would come back as plain
// waypoints, the flag would be gone, and the transport would sail to the drop point and
// sit there with its cargo aboard for ever, with an order list that looks valid.
//
// Neither hazard is visible to a "does it unload?" check on open ground, which is why
// the queue shape is asserted directly and why the coastal case below builds real water
// and runs the path service rather than reusing the flat map.

#include "sim/sim.h"
#include "sim/legion.h"
#include "sim/matchsetup.h"
#include "hpi/hpi.h"
#include "sim/retailtransport.h"
#include "net/protocol.h"
#include "tnt/tnt.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include <cmath>
#include <cstdio>
#include <vector>

using ta::sim::UnitType;
using ta::sim::World;
static ta::sim::PathfindingMode g_pathfinding = ta::sim::PathfindingMode::Retail;
static void setTestPathfinding(World& world) {
    if(g_pathfinding!=ta::sim::PathfindingMode::Retail)world.setPathfindingMode(g_pathfinding);
}

namespace ta::sim {
struct RetailReplayProbe {
    static bool waterFootprintPassable(const World& world,const ta::sim::Unit* unit) {
        if(!unit || !unit->type) return false;
        const int fx=unit->type->footX,fz=unit->type->footZ;
        const int x=ta::sim::footprintOrigin(unit->x,fx);
        const int z=ta::sim::footprintOrigin(unit->z,fz);
        const auto& nav=world.navFor(unit->type);
        const bool fits=nav.fits(x+fx/2,z+fz/2,std::max(fx,fz));
        if(!fits) return false;
        for(int cz=z;cz<z+fz;++cz)
            for(int cx=x;cx<x+fx;++cx)
                if(!world.passable(unit->type,cx,cz)) return false;
        return true;
    }

    static bool footprintPhysicallyPlaced(const World& world,const ta::sim::Unit* unit) {
        if(!unit || !unit->type) return false;
        return world.mobilePlacement(*unit,
            ta::sim::footprintOrigin(unit->x,unit->type->footX),
            ta::sim::footprintOrigin(unit->z,unit->type->footZ),false);
    }

    static void dumpSurfaceSearchGrades(World& world) {
        struct State { bool dumped=false; std::vector<int> initial; int changes=0; };
        auto* stream=std::getenv("TA_DUMP_GRADE_PLANE") ? stderr : nullptr;
        if (!stream) return;
        auto state=std::make_shared<State>();
        world.paths_.setGradeHost({
            [&world](int id,bool last) {
                world.prepareSearchGrade(id,last);
                if(!std::getenv("TA_DUMP_ATTEMPT_PLANES") || world.activeSearchGrade_<0)
                    return;
                const Unit* unit=world.unit(id);
                if(!unit || !unit->type) return;
                const auto& attempt=world.paths_.worker_.attempt;
                const int fx=unit->type->footX,fz=unit->type->footZ;
                const int startX=attempt.trace.start.x+fx/2;
                const int startZ=attempt.trace.start.z+fz/2;
                const PathCell start{startX,startZ};
                const auto& plane=world.searchGrades_[size_t(world.activeSearchGrade_)];
                const int width=plane.nav->width(),height=plane.nav->height();
                const auto costs=world.searchCosts(*unit);
                std::fprintf(stderr,"ATTEMPTPROFILE %u %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                    world.tickCounter_,id,unit->type->turnRate,fx,fz,unit->type->roadMult.v,
                    unit->type->waterMult.v,unit->groundTerrainFlags,
                    unit->type->transportDist,unit->type->maxWaterDepth,
                    unit->type->minWaterDepth,unit->type->halfCellTicks,
                    int(costs.heavyFloater),int(last));
                std::fprintf(stderr,"ATTEMPTPLANE %u %d %d %d %d %d %d %d %d %d %d %d\n",
                    world.tickCounter_,id,startX,startZ,attempt.retry,attempt.heading,
                    attempt.weight,attempt.trace.trafficRadius,width,height,fx,fz);
                for(int z=0;z<height;++z) {
                    for(int x=0;x<width;++x) {
                        const int grade=world.searchGrade(id,x+fx/2,z+fz/2,
                                                         attempt.retry,start);
                        std::fprintf(stderr,"%d%c",grade,x+1==width?'\n':' ');
                    }
                }
            },
            [&world,state,stream](int id,int cx,int cz,int retry,PathCell start) {
                const int result=world.searchGrade(id,cx,cz,retry,start);
                const Unit* unit=world.unit(id);
                if (!state->dumped) {
                    if (unit && unit->type && world.activeSearchGrade_>=0) {
                        const auto& plane=world.searchGrades_[size_t(world.activeSearchGrade_)];
                        const int width=plane.nav->width(),height=plane.nav->height();
                        const int fx=unit->type->footX,fz=unit->type->footZ;
                        const auto costs=world.searchCosts(*unit);
                        std::fprintf(stream,"COSTPROFILE %d %d %d %d %d %d %d %d\n",
                                     unit->type->turnRate,fx,fz,unit->type->roadMult.v,
                                     unit->type->waterMult.v,unit->groundTerrainFlags,
                                     int(costs.heavyFloater),int(portHeadingToRetail(unit->heading)));
                        std::fprintf(stream,"GRADEPLANE %d %d %d %d %d %d %d %u %d\n",
                                     width,height,fx,fz,retry,start.x,start.z,
                                     world.tickCounter_,int(portHeadingToRetail(unit->heading)));
                        state->initial.resize(size_t(width)*height);
                        for(int z=0;z<height;++z) {
                            for(int x=0;x<width;++x) {
                                const int grade=world.searchGrade(id,x+fx/2,z+fz/2,retry,start);
                                state->initial[size_t(z)*width+x]=grade;
                                std::fprintf(stream,"%d%c",grade,
                                             x+1==width?'\n':' ');
                            }
                        }
                        state->dumped=true;
                    }
                } else if (unit && unit->type && world.activeSearchGrade_>=0) {
                    const auto& plane=world.searchGrades_[size_t(world.activeSearchGrade_)];
                    const int x=cx-unit->type->footX/2,z=cz-unit->type->footZ/2;
                    if (x>=0 && z>=0 && x<plane.nav->width() && z<plane.nav->height() &&
                        result!=state->initial[size_t(z)*size_t(plane.nav->width())+size_t(x)] &&
                        state->changes<100) {
                        std::fprintf(stream,"GRADECHANGE %u %d %d %d %d %d %d\n",
                            world.tickCounter_,retry,x,z,start.x,start.z,result);
                        ++state->changes;
                    }
                }
                return result;
            },
            [&world](int id) { world.finishSearchGrade(id); }
        });
    }

    static void dumpCompletedPathAttempt(const World& world,int unitId) {
        const Unit* unit=world.unit(unitId);
        if(!unit || !unit->type) return;
        const auto& attempt=world.paths_.worker_.attempt;
        const int fx=unit->type->footX,fz=unit->type->footZ;
        const auto& costs=attempt.cost.costs;
        std::fprintf(stderr,
            "WORLDATTEMPT %u %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %zu\n",
            world.tickCounter_,unitId,
            attempt.trace.start.x+fx/2,attempt.trace.start.z+fz/2,
            attempt.trace.goal.x+fx/2,attempt.trace.goal.z+fz/2,
            attempt.heading,attempt.retry,attempt.weight,attempt.initialDistance,
            attempt.partialDistance,attempt.phase,attempt.cost.endpoint,
            attempt.route.flags,costs.ground,costs.road,costs.slope,costs.traffic,
            costs.shortTurn,costs.minStraight,int(costs.heavyFloater),
            attempt.route.points.size());
        for(size_t i=0;i<attempt.route.points.size();++i) {
            const auto point=attempt.route.points[i];
            std::fprintf(stderr,"WORLDRAW %zu %d %d\n",i,
                         point.x+fx/2,point.z+fz/2);
        }
        uint64_t searchPlaneHash=14695981039346656037ull;
        for(const auto& cell:attempt.cost.cells) {
            searchPlaneHash=(searchPlaneHash^cell.flags)*1099511628211ull;
            searchPlaneHash=(searchPlaneHash^cell.direction)*1099511628211ull;
        }
        std::fprintf(stderr,"WORLDSEARCH %u %d %d %d %d %d %016llx\n",
            world.tickCounter_,unitId,attempt.retry,attempt.cost.processed,
            attempt.cost.endpoint,attempt.cost.heuristicWeight,
            static_cast<unsigned long long>(searchPlaneHash));
    }
};
}

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  %-68s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}

// tickTransport's unload radius. Mirrored here rather than read from the class (it is
// private) -- if they ever disagree, the distance assertions below start failing, which
// is the point.
static constexpr float kRange = 150.0f;

// GROUND_UNLOAD's drop-point circle (retailUnloadCircleRadius).
static int unloadCircle(const UnitType& t) {
    return ta::sim::retailUnloadCircleRadius(t.canHover,int16_t(t.footZ));
}

static UnitType boatType() {
    UnitType t{};
    t.name = "boat";
    t.maxVel = ta::sim::Fixed::fromFloat(60.0f / 30.0f);              // maxVel > 0 => not a structure
    t.turnRate = 10000;
    t.canTransport = true;
    t.transportCap = 4;
    t.transportSizeCap = 16;
    t.maxTransportSize = 4;
    t.transportDist = kRange;
    t.maxHp = 100;
    // A hover carrier (armthovr / corthovr): GROUND_UNLOAD stops it on a circle
    // round the drop point instead of driving onto the point (0x406a23).
    t.canHover = true;
    t.footZ = 4;   // armthovr's: a 96 px drop-point circle (retailUnloadCircleRadius)
    return t;
}

static UnitType footType() {
    UnitType t{};
    t.name = "foot";
    t.upright = true;
    t.maxVel = ta::sim::Fixed::fromFloat(30.0f / 30.0f);
    t.turnRate = 10000;
    t.transportSize = 1;
    t.maxHp = 100;
    return t;
}

// Board directly rather than driving the load order: the pickup is a separate path and
// every case here is about what happens after it.
static void board(World& w, int tid, int cid) {
    w.unit(cid)->inTransport = tid;
    w.unit(tid)->cargo.push_back(cid);
}

static bool runUntilUnloaded(World& w, int tid, int ticks = 4000) {
    for (int i = 0; i < ticks; ++i) {
        w.tick(1.0f / 30.0f);
        if (w.unit(tid)->cargo.empty()) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1. Open ground: the queue shape, in isolation.
// ---------------------------------------------------------------------------
// --legion: Legion (not the native mover) routes a ground passenger walking to
// its carrier, the ground carrier's unload approach and the released
// passenger's PARK ring; the missions themselves stay native.
static bool legionSaw(World& w,ta::sim::LegionMission kind) {
    const auto* legion=w.legionNavigator();bool seen=false;
    if(legion)for(const auto& u:w.units())seen|=legion->mission(u)==kind;
    return seen;
}
static void legionLogistics() {
    if(g_pathfinding!=ta::sim::PathfindingMode::Legion)return;
    std::printf("legion routes boarding, unload approach and parking:\n");
    World w;setTestPathfinding(w);w.setPathService(true);
    w.setTerrain(std::vector<uint8_t>(size_t(128)*128,100),128,128,20);
    UnitType carrier=boatType(),passenger=footType();
    carrier.footX=carrier.footZ;   // Legion routes square footprints off a placement plane
    const int tid=w.spawn(&carrier,300,600),cid=w.spawn(&passenger,1100,600);
    w.loadInto(cid,tid);
    bool load=false,boarded=false;
    for(int tick=0;tick<3000&&!boarded;++tick) {
        w.tick(1.f/30);load|=legionSaw(w,ta::sim::LegionMission::Load);
        boarded=w.unit(cid)->inTransport==tid;
    }
    check(load,"Legion routes the passenger's walk to its carrier");
    check(boarded,"the passenger boards after a Legion approach");
    w.unloadAt(tid,1500,1200);
    bool unload=false,released=false;
    for(int tick=0;tick<4000&&!released;++tick) {
        w.tick(1.f/30);unload|=legionSaw(w,ta::sim::LegionMission::Unload);
        released=w.unit(tid)->cargo.empty();
    }
    check(unload,"Legion routes the ground carrier's unload approach");
    check(released,"the carrier unloads after a Legion approach");
    bool park=false;
    for(int tick=0;tick<900;++tick) {w.tick(1.f/30);park|=legionSaw(w,ta::sim::LegionMission::Park);}
    check(park,"Legion routes the released passenger's PARK ring");
    check(w.unit(cid)->orders.empty() && !w.unit(cid)->embarked(),"the released passenger finishes parking");
}

static void openGround() {
    std::printf("open ground -- the order queue unloadAt builds:\n");
    const int W = 64, H = 64;
    World w;setTestPathfinding(w);
    w.setTerrain(std::vector<uint8_t>(size_t(W) * H, 100), W, H, /*seaLevel=*/20);

    UnitType boat = boatType(), foot = footType();
    const int tid = w.spawn(&boat, 100, 100);
    const int cid = w.spawn(&foot, 100, 100);
    board(w, tid, cid);

    const float dropX = 600, dropZ = 600;
    w.unloadAt(tid, dropX, dropZ);

    const ta::sim::Unit* t = w.unit(tid);
    check(t->orders.size() == 2, "an out-of-range drop queues an approach plus the unload");
    if (t->orders.size() == 2) {
        const ta::sim::Order& leg = t->orders[0];
        const ta::sim::Order& un = t->orders[1];
        // The leg replaceLeg() would rebuild is the FIRST order flagged `goal`. It must
        // be the approach, never the unload -- see hazard TWO in the header.
        check(leg.goal && !leg.unload, "the routable leg is the approach, not the unload");
        check(!un.goal && un.unload,
              "the unload sits BEHIND the leg, where replaceLeg preserves it");
        check(un.x.toFloat() == dropX && un.z.toFloat() == dropZ, "the unload keeps the drop point it was given");
        check(leg.groundMission, "surface approach uses the native circle navigator goal");
        check(leg.x.toFloat() == dropX && leg.z.toFloat() == dropZ,
              "the approach circle is centered on the exact selected drop point");
        check(int(leg.missionRadius + 4) == unloadCircle(boat),
              "the approach circle is GROUND_UNLOAD's drop-point circle");
    } else {
        g_fail += 4;
    }

    const bool done = runUntilUnloaded(w, tid);
    check(done, "the transport sails to the drop point and disembarks");
    if (done) {
        const ta::sim::Unit* c = w.unit(cid);
        check(c->inTransport == 0, "the cargo unit is off the transport");
        const float dx = c->x.toFloat() - dropX, dz = c->z.toFloat() - dropZ;
        check(std::sqrt(dx * dx + dz * dz) < 250,
              "the cargo is put down near the drop point, not where it boarded");
        const auto& tail=w.unit(tid)->orders;
        check(tail.size()==1 && tail.front().unload && tail.front().transportUnloadReleasePending,
              "the completed unload retains retail's one-tick idle mission");
        w.tick(1.f/30);
        check(w.unit(tid)->orders.empty(), "the idle unload mission retires after its one-tick wait");
    } else {
        g_fail += 3;
    }
}

// ---------------------------------------------------------------------------
// 2. Already in range: no approach leg at all.
// ---------------------------------------------------------------------------
static void alreadyInRange() {
    std::printf("already within unloading range:\n");
    const int W = 64, H = 64;
    World w;setTestPathfinding(w);
    w.setTerrain(std::vector<uint8_t>(size_t(W) * H, 100), W, H, /*seaLevel=*/20);

    UnitType boat = boatType(), foot = footType();
    const int tid = w.spawn(&boat, 500, 500);
    const int cid = w.spawn(&foot, 500, 500);
    board(w, tid, cid);

    // 100px away -- inside kRange, so there is nothing to approach.
    foot.footX = foot.footZ = 2;
    const int cid2 = w.spawn(&foot, 500, 500);
    board(w, tid, cid2);
    w.unloadAt(tid, 600, 500);
    const ta::sim::Unit* t = w.unit(tid);
    check(t->orders.size() == 2 && t->orders[0].transportUnloadApproach && t->orders[1].unload,
          "a drop point already in range still starts with GROUND_UNLOAD's call");

    // One passenger per order (TotalA.exe 0x4069f0..0x406a09): the first
    // unload ends with the second passenger still aboard.
    bool first = false;
    for (int i = 0; i < 120 && !first; ++i) {
        w.tick(1.0f / 30.0f);
        first = w.unit(tid)->cargo.size() == 1 && w.unit(tid)->orders.empty();
    }
    check(first, "one unload order puts down one passenger");
    check(w.unit(cid2)->inTransport == tid, "the passenger boarded last stays aboard");

    // Each passenger uses the same point. The first must walk clear before
    // the second passenger's drop is accepted.
    w.unloadAt(tid, 600, 500);
    bool quick = false;
    for (int i = 0; i < 900 && !quick; ++i) {
        w.tick(1.0f / 30.0f);
        quick = w.unit(tid)->cargo.empty();
    }
    check(quick, "a second unload puts down the second passenger at the same point");
    const auto& a = *w.unit(cid);
    const auto& b = *w.unit(cid2);
    check(std::abs(ta::sim::footprintOrigin(a.x, 2) - ta::sim::footprintOrigin(b.x, 2)) >= 2 ||
          std::abs(ta::sim::footprintOrigin(a.z, 2) - ta::sim::footprintOrigin(b.z, 2)) >= 2,
          "passengers reserve distinct whole footprints during one unload");
}

// A coastline: cells [0,shoreCell) are water, the rest land. Water units need
// minWaterDepth 13 (sea - height >= 13); ground units are blocked by maxWaterDepth 20.
// sea 40 with height 10 / 100 puts each side firmly in one.
static std::vector<uint8_t> coastHeights(int W, int H, int shoreCell) {
    std::vector<uint8_t> hts(size_t(W) * H, 100);
    for (int z = 0; z < H; ++z)
        for (int x = 0; x < shoreCell; ++x) hts[size_t(z) * W + x] = 10;
    return hts;
}

// ---------------------------------------------------------------------------
// 3. A real coastline, with the path service running.
//
// Water on the left, land on the right, drop point on the land just past the shore.
// This is the case the flat map cannot express: the navigator must find a reachable
// water endpoint inside the native circle around the land destination.
// ---------------------------------------------------------------------------
static void coastline() {
    std::printf("coastline, path service on:\n");
    const int W = 96, H = 96;
    const int shoreCell = 25;                 // cells [0,25) water, [25,..) land
    World w;setTestPathfinding(w);
    w.setTerrain(coastHeights(W, H, shoreCell), W, H, /*seaLevel=*/40);
    w.setPathService(true);                   // the async boundary tracer, as in a game

    UnitType boat = boatType(), foot = footType();
    boat.domain = UnitType::Domain::Water;    // navFor() hands it the water grid
    const int tid = w.spawn(&boat, 80, 700);  // out at sea, well down-map
    const int cid = w.spawn(&foot, 80, 700);
    board(w, tid, cid);

    const float shoreX = float(shoreCell) * 16;          // 400
    const float dropX = shoreX + 64, dropZ = 300;        // on land, 64px past the shore
    w.unloadAt(tid, dropX, dropZ);

    const ta::sim::Unit* t = w.unit(tid);
    check(t->orders.size() == 2, "a coastal drop still queues an approach plus the unload");
    if (t->orders.size() == 2) {
        const ta::sim::Order& leg = t->orders[0];
        check(leg.groundMission && leg.x.toFloat() == dropX && leg.z.toFloat() == dropZ,
              "the approach circle is centered on the exact land drop point");
        check(int(leg.missionRadius + 4) == unloadCircle(boat),
              "the circle uses GROUND_UNLOAD's drop-point radius");
        check(leg.goal && !leg.unload && t->orders[1].unload, "the queue shape still holds");
    } else {
        g_fail += 3;
    }

    // Drive it. The route arrives asynchronously and is spliced in by replaceLeg, so
    // this also covers the flag surviving a real route installation.
    const bool done = runUntilUnloaded(w, tid);
    check(done, "the transport crosses the water, reaches the shore and disembarks");
    if (done) {
        const ta::sim::Unit* c = w.unit(cid);
        const ta::sim::Unit* tr = w.unit(tid);
        check(c->inTransport == 0, "the cargo unit is off the transport");
        check(c->x.toFloat() >= shoreX, "the cargo is put down on LAND, not in the water");
        check(tr->x.toFloat() < shoreX, "the transport itself stayed in its own domain");
        const float dx = c->x.toFloat() - dropX, dz = c->z.toFloat() - dropZ;
        check(std::sqrt(dx * dx + dz * dz) < 250, "the cargo lands near the drop point");
    } else {
        g_fail += 4;
    }
}

// A moving body arrives at the exact landing cell while a transport is closing
// on a crowded shore. Exercise the interaction the flat blocker and empty-coast
// fixtures cover separately: real placement/terrain, the native retry wait,
// carrier movement during the retry, and passenger attachment/release. Air and
// surface carriers share the landing rules, while only the ship must remain on
// the water side of the shoreline.
static void crowdedCoastlineUnloadRetry() {
    for (bool air : {false, true}) {
        std::printf("crowded coastline unload retry (%s):\n", air ? "air" : "sea");
        constexpr int W=96,H=96,shoreCell=25;
        auto heights=coastHeights(W,H,shoreCell);
        World w;setTestPathfinding(w);w.setPathService(true);
        w.setTerrain(heights,W,H,40);
        w.setMapPlacementFeatures(std::vector<uint16_t>(size_t(W)*H,0xffff),{});

        UnitType carrier=boatType(),passenger=footType(),blockerType=footType();
        carrier.canFly=air;carrier.cruiseAlt=100;
        if(!air)carrier.domain=UnitType::Domain::Water;
        passenger.accel={};
        blockerType.accel=ta::sim::Fixed::fromInt(1);
        blockerType.maxVel=ta::sim::Fixed::fromInt(3);
        // A surface drop lands where the boom carried the point (retailHullPoint),
        // a few px off it while the hull brakes: a 2x2 body covers it.
        if(!air)blockerType.footX=blockerType.footZ=2;
        const int dropX=shoreCell*16+64,dropZ=300;
        const int tid=w.spawn(&carrier,80,700),cid=w.spawn(&passenger,80,700);
        board(w,tid,cid);
        w.unloadAt(tid,float(dropX),float(dropZ));

        int blocker=0;
        bool retrySeen=false,carrierMovedDuringRetry=false,passengerFollowedCarrier=true;
        bool heldThroughRetry=false,blockerClearedAtRelease=false;
        int retryTick=-1;ta::sim::Fixed retryX{},retryZ{};
        bool released=false;
        for(int tick=0;tick<4000 && !released;++tick) {
            const auto* before=w.unit(tid);
            const float dx=(ta::sim::Fixed::fromInt(dropX)-before->x).toFloat();
            const float dz=(ta::sim::Fixed::fromInt(dropZ)-before->z).toFloat();
            if(!blocker && dx*dx+dz*dz < (kRange+8)*(kRange+8)) {
                blocker=w.spawn(&blockerType,float(dropX),float(dropZ));
            }
            w.tick(1.f/30);
            auto* transport=w.unit(tid);
            auto mission=std::find_if(transport->orders.begin(),transport->orders.end(),
                [](const auto& order){return order.transportUnloadApproach;});
            // GROUND_UNLOAD has no retry stage: a refused DROP_UNIT returns the
            // order to its TransportDrop call (stage 0/1) with the cargo aboard.
            const bool surfaceRefused=!air && mission!=transport->orders.end() && mission->unload &&
                mission->transportMission.stage<=1 && transport->cargo.size()==1;
            if(mission!=transport->orders.end() && (air ? mission->transportMission.stage==3 : surfaceRefused) &&
               !retrySeen) {
                retrySeen=true;retryTick=int(w.tickCount());retryX=transport->x;retryZ=transport->z;
                heldThroughRetry=transport->cargo.size()==1 && w.unit(cid)->inTransport==tid;
                w.order(blocker,dropX+256,dropZ,false);
            }
            if(retrySeen && transport->cargo.size()==1 && w.unit(cid)->inTransport==tid &&
               int(w.tickCount())>=retryTick+4) {
                if(transport->x!=retryX || transport->z!=retryZ)
                    carrierMovedDuringRetry=true;
                passengerFollowedCarrier=passengerFollowedCarrier &&
                    w.unit(cid)->x==transport->x && w.unit(cid)->z==transport->z;
            }
            released=transport->cargo.empty();
            if(released && blocker) {
                const auto& cargo=*w.unit(cid);const auto& body=*w.unit(blocker);
                const int px=ta::sim::footprintOrigin(cargo.x,passenger.footX);
                const int pz=ta::sim::footprintOrigin(cargo.z,passenger.footZ);
                const int bx=ta::sim::footprintOrigin(body.x,blockerType.footX);
                const int bz=ta::sim::footprintOrigin(body.z,blockerType.footZ);
                const bool overlapX=bx<px+passenger.footX && px<bx+blockerType.footX;
                const bool overlapZ=bz<pz+passenger.footZ && pz<bz+blockerType.footZ;
                blockerClearedAtRelease=!overlapX || !overlapZ;
            }
        }
        if(!released || !heldThroughRetry) {
            const auto* transport=w.unit(tid);const auto* cargo=w.unit(cid);
            const auto* blockerUnit=blocker?w.unit(blocker):nullptr;
            std::fprintf(stderr,"crowded unload diagnostic air=%d tick=%u retry=%d moved=%d released=%d cargo=%zu attached=%d ship=(%.1f,%.1f) blocker=(%.1f,%.1f) orders=%zu pass=(%.1f,%.1f)\n",
                int(air),w.tickCount(),retryTick,int(carrierMovedDuringRetry),int(released),
                transport->cargo.size(),cargo->inTransport==tid,transport->x.toFloat(),transport->z.toFloat(),
                blockerUnit?blockerUnit->x.toFloat():-1,blockerUnit?blockerUnit->z.toFloat():-1,
                transport->orders.size(),cargo->x.toFloat(),cargo->z.toFloat());
        }
        check(blocker!=0,"a crowding unit enters the selected shoreline landing cell");
        check(retrySeen,"a landing-cell arrival interrupts the native unload transfer");
        check(heldThroughRetry,
              "cargo remains attached while the shoreline unload retries");
        if(air)
            check(carrierMovedDuringRetry,
                  "the carrier keeps moving while the crowded-site retry waits");
        check(passengerFollowedCarrier,
              "the attached passenger follows the carrier during the retry");
        check(released && !w.unit(cid)->embarked(),
              "the same passenger is released after the blocker clears");
        if(released) {
            check(blockerClearedAtRelease,
                  "the blocker leaves the passenger's footprint before release");
            // A surface hull carries the boom's point as it brakes (retailHullPoint).
            const float slack=air ? 0.0f : 32.0f;
            check(std::abs(w.unit(cid)->x.toFloat()-float(dropX))<=slack &&
                  std::abs(w.unit(cid)->z.toFloat()-float(dropZ))<=slack,
                  "retry retains the selected passenger landing point");
            if(!air)
                check(w.unit(tid)->x.toFloat()<shoreCell*16,
                      "the surface carrier stays in navigable water as cargo lands on shore");
        }
    }
}

// ---------------------------------------------------------------------------
// 4. Far inland: preserve the native circle request even when no fitting water cell
// is reachable inside it; navigator failure handling owns the result.
// ---------------------------------------------------------------------------
static void farInland() {
    std::printf("drop point far inland -- no reachable approach:\n");
    const int W = 96, H = 96;
    const int shoreCell = 25;
    World w;setTestPathfinding(w);
    w.setTerrain(coastHeights(W, H, shoreCell), W, H, /*seaLevel=*/40);
    w.setPathService(true);

    UnitType boat = boatType(), foot = footType();
    boat.domain = UnitType::Domain::Water;
    const int tid = w.spawn(&boat, 80, 700);
    const int cid = w.spawn(&foot, 80, 700);
    board(w, tid, cid);

    // 900px inland: no water cell is within the native approach circle.
    w.unloadAt(tid, 1300, 300);
    const ta::sim::Unit* t = w.unit(tid);
    check(t->orders.size() == 2 && t->orders[0].groundMission && t->orders[1].unload,
          "an unreachable destination still submits the native circle goal before unload");
    if (t->orders.size() == 2)
        check(t->orders[0].x.toFloat() == 1300 && t->orders[0].z.toFloat() == 300 &&
              int(t->orders[0].missionRadius + 4) == unloadCircle(boat),
              "far-inland request retains the exact point and native radius");

}

static void unloadApproachFailure() {
    std::printf("boxed surface unload approach failure:\n");
    auto heights=coastHeights(96,96,50);
    for(int z=5;z<=9;++z)for(int x=5;x<=9;++x)
        if(x!=7 || z!=7)heights[size_t(z)*96+x]=100;
    World w;setTestPathfinding(w);w.setTerrain(heights,96,96,40);w.setPathService(true);
    UnitType boat=boatType(),foot=footType();boat.domain=UnitType::Domain::Water;
    const int tid=w.spawn(&boat,120,120),cid=w.spawn(&foot,120,120);
    board(w,tid,cid);w.unloadAt(tid,1300,300);w.order(tid,140,120,true);
    bool failure=false;
    for(int tick=0;tick<100 && !failure;++tick) {
        w.tick(1.f/30);
        for(const auto& order:w.unit(tid)->orders)
            failure=failure || (order.transportUnloadApproach &&
                                (order.transportMission.pending&0x200));
    }
    check(failure,"boxed ship reports native navigator failure for its unload circle");
    // TotalA.exe 0x406a13 / 0x43b8e5: each failure is a wake and another
    // TransportDrop call; the third returns 9, which drops the unload for the
    // queued move.
    bool preserved=false;
    for(int tick=0;tick<600 && !preserved;++tick) {
        bool approach=false,unload=false,queuedGoal=false;
        for(const auto& order:w.unit(tid)->orders) {
            approach=approach || order.transportUnloadApproach;
            unload=unload || order.unload;
            queuedGoal=queuedGoal || (order.goal && !order.unload &&
                order.clickX.toFloat()==140 && order.clickZ.toFloat()==120);
        }
        preserved=!approach && !unload && queuedGoal;
        if(!preserved)w.tick(1.f/30);
    }
    check(preserved,"failed unload retires its approach and preserves the queued move");
    check(w.unit(cid)->inTransport==tid,"failed surface unload leaves its passenger aboard");
}

// ---------------------------------------------------------------------------
// 5. A route already in flight when the unload is issued.
//
// unloadAt clears the order list, but a search requested for the orders it just threw
// away is still queued in the PathService. When the new order list needs NO approach
// leg (cases 1 and 3) nothing re-requests, so that stale search survives -- and when it
// lands, replaceLeg() rebuilds the current leg from its waypoints. With a bare unload
// order that leg IS the unload: currentLeg() falls through to the last order when
// nothing carries `goal`, and replaceLeg carries only movement flags across. The unload
// flag is dropped, the transport drives off along a route to somewhere it was told to go
// before, and the cargo never leaves.
// ---------------------------------------------------------------------------
static void retargetWithPendingRoute() {
    std::printf("unload issued while a route is still in flight:\n");
    const int W = 96, H = 96, shoreCell = 25;
    World w;setTestPathfinding(w);
    w.setTerrain(coastHeights(W, H, shoreCell), W, H, /*seaLevel=*/40);
    w.setPathService(true);

    UnitType boat = boatType(), foot = footType();
    boat.domain = UnitType::Domain::Water;
    const int tid = w.spawn(&boat, 280, 200);
    const int cid = w.spawn(&foot, 280, 200);
    board(w, tid, cid);

    // A long move down the coast, so a search is genuinely outstanding...
    w.order(tid, 200, 1400, /*queue=*/false);
    // ...and the player changes their mind immediately: unload right here. Close enough
    // that no approach is needed, which is precisely the case that re-requests nothing.
    w.unloadAt(tid, 420, 200);

    const ta::sim::Unit* t = w.unit(tid);
    check(t->orders.size() == 2 && t->orders[0].transportUnloadApproach && t->orders[1].unload,
          "the new order list is GROUND_UNLOAD's call and its unload");

    // Let any in-flight search land. If it is allowed to, it rewrites the unload.
    bool stillUnloading = true;
    for (int i = 0; i < 120 && stillUnloading; ++i) {
        w.tick(1.0f / 30.0f);
        const ta::sim::Unit* u = w.unit(tid);
        if (u->cargo.empty()) break;                       // unloaded: fine
        stillUnloading = !u->orders.empty() && u->orders.back().unload;
    }
    check(stillUnloading, "a superseded route does not overwrite the unload order");
    check(w.unit(tid)->cargo.empty() || (!w.unit(tid)->orders.empty() && w.unit(tid)->orders.back().unload),
          "the transport is still on the unload job, not off following the old route");
}

// ---------------------------------------------------------------------------
// 6. An isolated pond nearer the drop point than the sea. The circle-goal search must
// choose a reachable water cell rather than stopping at a disconnected component.
// ---------------------------------------------------------------------------
static void isolatedPond() {
    std::printf("an isolated pond closer than the open sea:\n");
    const int W = 96, H = 96, shoreCell = 25;
    std::vector<uint8_t> hts = coastHeights(W, H, shoreCell);
    // A 3x3 landlocked pond at cells x 27..29, z 19..21 -- one cell of land between it
    // and the sea, so it is genuinely a separate body of water.
    for (int z = 19; z <= 21; ++z)
        for (int x = 27; x <= 29; ++x) hts[size_t(z) * W + x] = 10;

    World w;setTestPathfinding(w);
    w.setTerrain(hts, W, H, /*seaLevel=*/40);
    w.setPathService(true);

    UnitType boat = boatType(), foot = footType();
    boat.domain = UnitType::Domain::Water;
    const int tid = w.spawn(&boat, 80, 700);              // out at sea
    const int cid = w.spawn(&foot, 80, 700);
    board(w, tid, cid);

    // Cell (26,22) is dry across all four height samples; the disconnected pond is
    // closer to the destination than open sea.
    const float dropX = 26 * 16 + 8, dropZ = 22 * 16 + 8;
    const float shoreX = float(shoreCell) * 16;           // 400
    w.unloadAt(tid, dropX, dropZ);

    const ta::sim::Unit* t = w.unit(tid);
    check(t->orders.size() == 2, "an out-of-range unload queues its native circle approach");
    if (t->orders.size() == 2) {
        const ta::sim::Order& leg = t->orders[0];
        check(leg.groundMission && leg.x.toFloat() == dropX && leg.z.toFloat() == dropZ,
              "the approach stays centered on the requested point instead of preselecting water");
        check(int(leg.missionRadius + 4) == unloadCircle(boat),
              "the reachable endpoint set uses the retail approach radius");
    } else {
        g_fail += 2;
    }

    check(runUntilUnloaded(w, tid), "the navigator reaches connected sea and disembarks");
    check(w.unit(tid)->x.toFloat() < shoreX,
          "the carrier never routes through the closer disconnected pond");
}

static void boardingLimits() {
    std::printf("boarding eligibility and competing orders:\n");
    World w;setTestPathfinding(w);
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    UnitType carrier=boatType(), passenger=footType();
    carrier.transportCap=3;
    carrier.transportSizeCap=8;
    passenger.transportSize=4;
    const int tid=w.spawn(&carrier,400,400);
    std::vector<int> ids;
    for(int i=0;i<3;++i) ids.push_back(w.spawn(&passenger,420,400+i*16));
    check(w.canLoadInto(ids[0],tid),"eligible passenger can board");
    for(bool air:{false,true}) {
        World gated;setTestPathfinding(gated);
        gated.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        auto transport=boatType();transport.canFly=air;transport.cruiseAlt=100;
        const int t=gated.spawn(&transport,400,400),p=gated.spawn(&passenger,420,400);
        for(int disabled:{p,t}) {
            gated.unit(disabled)->paralyzedFor=90;
            check(!gated.canLoadInto(p,t),"paralyzed passenger or carrier cannot start boarding");
            gated.loadInto(p,t);
            check(gated.unit(p)->orders.empty() && gated.unit(t)->orders.empty(),
                  "rejected paralyzed boarding leaves both order queues unchanged");
            gated.unit(disabled)->paralyzedFor=0;
        }
        gated.loadInto(p,t);gated.tick(1.f/30);
        gated.unit(p)->paralyzedFor=90;
        for(int tick=0;tick<60;++tick)gated.tick(1.f/30);
        check(!gated.unit(p)->embarked() && gated.unit(t)->cargo.empty() &&
              std::none_of(gated.unit(t)->orders.begin(),gated.unit(t)->orders.end(),
                  [](const auto& order){return order.transportPickup;}),
              "paralysis after pickup starts prevents attachment and retires the carrier mission");
        gated.unit(p)->paralyzedFor=0;gated.loadInto(p,t);
        for(int tick=0;tick<180 && !gated.unit(p)->embarked();++tick)gated.tick(1.f/30);
        check(gated.unit(p)->embarked(),"recovered passenger can board after a new load command");
    }
    check(!w.canLoadInto(tid,tid),"carrier cannot board itself");
    passenger.transportSize=5;
    check(!w.canLoadInto(ids[0],tid),"individual size limit applies before boarding");
    passenger.transportSize=4;
    passenger.canFly=true;
    check(!w.canLoadInto(ids[0],tid),"flying passengers are rejected");
    passenger.canFly=false;
    passenger.cantBeTransported=true;
    check(!w.canLoadInto(ids[0],tid),"cantbetransported is respected");
    passenger.cantBeTransported=false;
    w.unit(ids[0])->player=1;
    check(!w.canLoadInto(ids[0],tid),"enemy passenger is rejected");
    w.unit(ids[0])->player=0;
    passenger.transportLandEligible=false;
    check(!w.canLoadInto(ids[0],tid),"surface carrier rejects water-only passenger");
    carrier.canFly=true;
    check(w.canLoadInto(ids[0],tid),"air carrier skips surface passenger restriction");
    carrier.canFly=false;
    passenger.transportLandEligible=true;
    const auto height=w.unit(ids[0])->groundY;
    w.unit(ids[0])->groundY=ta::sim::Fixed::fromInt(20);
    check(!w.canLoadInto(ids[0],tid),"fully submerged passenger cannot board");
    passenger.modelTop=1;
    check(w.canLoadInto(ids[0],tid),"passenger model top above sea level permits boarding");
    passenger.modelTop=0;w.unit(ids[0])->groundY=height;
    for(int id:ids) w.loadInto(id,tid);
    bool capacityHeld=true;
    for(int tick=0;tick<120;++tick) {
        w.tick(1.0f/30);
        capacityHeld=capacityHeld && w.unit(tid)->cargo.size()<=2;
    }
    check(capacityHeld && w.unit(tid)->cargo.size()==2,
          "queued boarding never exceeds total size budget");
    int remaining=0;
    for(int id:ids) if(!w.unit(id)->embarked()) remaining=id;
    check(remaining && !w.canLoadInto(remaining,tid),"remaining passenger cannot overfill carrier");
    carrier.transportSizeCap=16;
    carrier.transportCap=2;
    check(remaining && !w.canLoadInto(remaining,tid),"count limit applies independently of total size");
    carrier.transportCap=3;
    check(remaining && w.canLoadInto(remaining,tid),"passenger admitted when both limits permit it");
}

static void transferLifecycle() {
    World w;setTestPathfinding(w);
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    UnitType carrier=boatType(),passenger=footType();
    const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,430,400);
    w.loadInto(cid,tid);
    for(int tick=0;tick<15;++tick)w.tick(1.f/30);
    check(!w.unit(cid)->embarked(),"boarding waits fifteen retail ticks before attachment");
    check(w.unit(cid)->x.toFloat()==430,"boarding passenger stands still during transfer");
    w.stop(cid);w.tick(1.f/30);
    check(!w.unit(cid)->embarked(),"Stop interrupts an unfinished boarding transfer");
    w.loadInto(cid,tid);
    for(int tick=0;tick<18;++tick)w.tick(1.f/30);
    check(w.unit(cid)->inTransport==tid,"boarding completes after the transfer interval");
    w.unloadAt(tid,500,400);
    for(int tick=0;tick<15;++tick)w.tick(1.f/30);
    check(w.unit(cid)->embarked(),"unloading waits before releasing its passenger");
    // GROUND_UNLOAD's call (its approach handler), then the fifteen-tick drop.
    int releaseTick=15;
    while(releaseTick<40 && w.unit(cid)->embarked()) { w.tick(1.f/30);++releaseTick; }
    std::printf("  (released after %d ticks)\n",releaseTick);
    check(!w.unit(cid)->embarked() && releaseTick<=18,"unloading releases passenger after the transfer interval");
    check(!w.unit(cid)->orders.empty() && w.unit(cid)->orders.front().park.has_value(),
        "sea-unloaded passenger receives retail PARK to clear the landing point");
    w.loadInto(cid,tid);
    for(int tick=0;tick<120 && !w.unit(cid)->embarked();++tick)w.tick(1.f/30);
    check(w.unit(cid)->embarked(),"dispersing passenger finishes braking and reboards before carrier death");
    w.unit(tid)->deadFor=0;
    w.tick(1.f/30);
    check(!w.unit(cid)->alive(),"cargo is lost when its carrier dies");
}

// The released passenger's PARK (409154..409178): never permanent; behind the
// passenger's own orders, or in front of them when the carrier fails the
// selection predicate -- a mission script's transport (its selectable bit
// clear) puts it first. (A selectable passenger's PARK in front gives way to
// its next order at its first run.)
static void unloadParkPlace() {
    for(int scripted=0;scripted<2;++scripted) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType carrier=boatType(),passenger=footType();
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,430,400);
        w.loadInto(cid,tid);
        for(int tick=0;tick<40 && !w.unit(cid)->embarked();++tick)w.tick(1.f/30);
        // The passenger's own order, queued aboard, from its mission script (so
        // it is not selectable either: a PARK in front of it does not give way
        // at once, 407d1e, and stays to be seen).
        w.order(cid,700,700,true);
        w.unit(cid)->selectable=false;
        w.unit(tid)->selectable=scripted==0;
        w.unloadAt(tid,500,400);
        for(int tick=0;tick<40 && w.unit(cid)->embarked();++tick)w.tick(1.f/30);
        const auto& o=w.unit(cid)->orders;
        const bool front=!o.empty() && o.front().park.has_value();
        const bool back=!o.empty() && o.back().park.has_value();
        const auto* park=front ? &*o.front().park : back ? &*o.back().park : nullptr;
        check(!w.unit(cid)->embarked() && park && park->permanent==0,
              scripted ? "a scripted carrier's passenger gets a PARK that is not permanent"
                       : "a passenger gets a PARK that is not permanent");
        check(scripted ? front && !back : back && !front,
              scripted ? "in front of its own orders (the carrier is not selectable)"
                       : "behind its own orders (the carrier is selectable)");
    }
}

// Ordinary moves use the same VTOL mission for transports, builders and armed
// flyers: TotalA.exe's VTOL_Move (0x40fa20) has one point controller, radius 0
// at the snapped goal (0x40fa67..0x40faf6), and ends on its event -- no coarse
// circle first, so queued work starts at the point too.
static void ordinaryFlightMoves() {
    using namespace ta::sim;
    for (int role=0;role<3;++role) {
        UnitType air=boatType();air.canFly=true;air.cruiseAlt=100;air.footZ=1;
        air.accel=Fixed::raw(20000);air.brake=Fixed::raw(60000);
        air.isBuilder=role==1;
        if(role==2) air.weapons.emplace_back();
        for(bool queued:{false,true}) {
            World w;setTestPathfinding(w);
            w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int id=w.spawn(&air,160,160);
            w.unit(id)->baseSpeed=air.maxVel;
            w.unit(id)->flightY=Fixed::fromInt(200);
            w.order(id,800,800,false);
            if(queued)w.orderWait(id,300,true);
            bool coarse=false,precise=false,completed=false;
            for(int tick=0;tick<1200;++tick) {
                w.tick(1.f/30);
                const auto& u=*w.unit(id);
                if(!u.orders.empty() && u.orders.front().flightMoveMission) {
                    const auto& o=u.orders.front();
                    coarse|=o.flightGoal && (o.flightGoal->flags&0x10);
                    precise|=o.mission.stage==2 && o.flightGoal &&
                        o.flightGoal->flags==0x20 && o.flightGoal->radius==0;
                    continue;
                }
                const double distance=std::hypot(u.x.toFloat()-808,u.z.toFloat()-808);
                check(distance<4,queued ? "queued flight hands off at the precise point" :
                                          "terminal flight reaches precise destination");
                completed=true;break;
            }
            check(!coarse && completed && precise,
                  "all flying roles fly one radius-0 controller to the point");
        }
        // Retail has no fight-move: a flyer's fight-move leg flies VTOL_Move
        // (its air-traffic check included) and keeps its engage-en-route flag.
        World converted;setTestPathfinding(converted);const int id=converted.spawn(&air,160,160);
        converted.attackMove(id,800,800,false);
        check(converted.unit(id)->orders.front().flightMoveMission && converted.unit(id)->orders.front().attackMove,
              "fight conversion flies VTOL_Move and engages en route");
        converted.patrolTo(id,800,800,false);
        check(!converted.unit(id)->orders.front().flightMoveMission,
              "patrol conversion retains its looping handler");
    }
}

static void loadOrderQueueing() {
    World w;setTestPathfinding(w);
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    UnitType carrier=boatType(),passenger=footType();carrier.footZ=1;
    const int tid=w.spawn(&carrier,200,440),cid=w.spawn(&passenger,430,400);
    w.order(cid,460,400,false);
    w.order(tid,460,440,false);

    ta::net::Command command;command.kind=ta::net::Cmd::Load;
    command.unitId=cid;command.targetId=tid;command.queue=1;
    ta::net::Writer writer;writer.cmd(command);
    ta::net::Reader reader(writer.b.data(),writer.b.size());
    const auto delivered=reader.cmd();
    ta::sim::TypeRegistry registry;
    ta::sim::applyCommand(w,registry,delivered);

    const auto* passengerUnit=w.unit(cid);
    const auto* carrierUnit=w.unit(tid);
    check(reader.ok && delivered.queue==1,
          "queued-load flag survives command serialization");
    check(passengerUnit->orders.size()==2 &&
          passengerUnit->orders.front().groundMission &&
          passengerUnit->orders.back().load && passengerUnit->orders.back().goal,
          "shift-load appends boarding behind the passenger's current order");
    check(carrierUnit->orders.size()==2 &&
          carrierUnit->orders.front().groundMission &&
          carrierUnit->orders.back().transportPickup &&
          carrierUnit->orders.back().targetId==cid,
          "shift-load appends the matching pickup behind the carrier's current order");
    bool boarded=false;
    for(int tick=0;tick<300 && !boarded;++tick) {
        w.tick(1.f/30);
        boarded=w.unit(cid)->inTransport==tid;
    }
    check(boarded,"queued passenger and carrier routes advance into boarding");

    World replace;setTestPathfinding(replace);
    replace.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    const int replaceCarrier=replace.spawn(&carrier,200,440);
    const int replacePassenger=replace.spawn(&passenger,430,400);
    replace.order(replacePassenger,460,400,false);
    replace.order(replaceCarrier,460,440,false);
    replace.loadInto(replacePassenger,replaceCarrier);
    check(replace.unit(replacePassenger)->orders.size()==1 &&
          replace.unit(replacePassenger)->orders.front().load &&
          replace.unit(replaceCarrier)->orders.size()==1 &&
          replace.unit(replaceCarrier)->orders.front().transportPickup,
          "ordinary load still replaces prior passenger and carrier orders");

    World unloading;setTestPathfinding(unloading);
    unloading.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    const int unloadCarrier=unloading.spawn(&carrier,200,440);
    const int unloadPassenger=unloading.spawn(&passenger,200,440);
    board(unloading,unloadCarrier,unloadPassenger);
    unloading.order(unloadCarrier,460,440,false);
    ta::net::Command unload;unload.kind=ta::net::Cmd::Unload;
    unload.unitId=unloadCarrier;unload.x=480;unload.z=440;unload.queue=1;
    ta::net::Writer unloadWriter;unloadWriter.cmd(unload);
    ta::net::Reader unloadReader(unloadWriter.b.data(),unloadWriter.b.size());
    const auto queuedUnload=unloadReader.cmd();
    ta::sim::applyCommand(unloading,registry,queuedUnload);
    const auto& unloadOrders=unloading.unit(unloadCarrier)->orders;
    check(unloadReader.ok && queuedUnload.queue==1 && unloadOrders.size()==3 &&
          unloadOrders[0].groundMission && !unloadOrders[0].unload &&
          unloadOrders[1].transportUnloadApproach && !unloadOrders[1].unload &&
          unloadOrders[2].unload,
          "shift-unload appends its route and transfer behind the carrier's current order");
    bool released=false;
    for(int tick=0;tick<400 && !released;++tick) {
        unloading.tick(1.f/30);
        released=unloading.unit(unloadCarrier)->cargo.empty();
    }
    check(released && !unloading.unit(unloadPassenger)->embarked(),
          "queued carrier route completes before passengers disembark");

    World returning;setTestPathfinding(returning);returning.setPathService(true);
    returning.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    const int returningCarrier=returning.spawn(&carrier,200,440);
    const int returningPassenger=returning.spawn(&passenger,200,440);
    board(returning,returningCarrier,returningPassenger);
    returning.order(returningCarrier,700,440,false);
    returning.unloadAt(returningCarrier,240,440,{},true);
    check(returning.unit(returningCarrier)->orders.size()==3 &&
          returning.unit(returningCarrier)->orders[1].transportUnloadApproach &&
          int(returning.unit(returningCarrier)->orders[1].missionRadius+4)==unloadCircle(carrier),
          "queued unload retains the native circle approach despite being in range at issue time");
    returning.order(returningCarrier,800,700,true);
    bool reachedPriorMove=false,returned=false;
    for(int tick=0;tick<2500 && !returned;++tick) {
        returning.tick(1.f/30);
        reachedPriorMove|=returning.unit(returningCarrier)->x.toFloat()>650;
        returned=returning.unit(returningCarrier)->cargo.empty();
    }
    check(reachedPriorMove && returned &&
          returning.unit(returningPassenger)->x.toFloat()<400,
          "queued nearby unload approaches its site after an earlier move takes the carrier away");
    bool reachedNextMove=false;
    for(int tick=0;tick<2500 && !reachedNextMove;++tick) {
        returning.tick(1.f/30);
        reachedNextMove=returning.unit(returningCarrier)->x.toFloat()>750 &&
            returning.unit(returningCarrier)->z.toFloat()>650;
    }
    check(reachedNextMove,"move queued after unload survives route completion and transfer");
}

static void queueUnloadDuringPickup() {
    for(bool air:{false,true}) {
        World w;setTestPathfinding(w);w.setPathService(true);
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType carrier=boatType(),passenger=footType();
        carrier.canFly=air;carrier.cruiseAlt=100;
        const int tid=w.spawn(&carrier,200,440),cid=w.spawn(&passenger,240,440);
        w.loadInto(cid,tid);
        ta::net::Command unload;unload.kind=ta::net::Cmd::Unload;
        unload.unitId=tid;unload.x=650;unload.z=440;unload.queue=1;
        ta::sim::TypeRegistry registry;ta::sim::applyCommand(w,registry,unload);
        bool boarded=false,released=false;
        for(int tick=0;tick<3000 && !released;++tick) {
            w.tick(1.f/30);
            boarded|=w.unit(cid)->inTransport==tid;
            released=boarded && !w.unit(cid)->embarked();
        }
        check(boarded && released && w.unit(cid)->x.toFloat()>600,
              air ? "air pickup followed by queued unload boards and delivers the passenger" :
                    "surface pickup followed by queued unload boards and delivers the passenger");
    }
}

static void transportEffectEvents() {
    for(bool air:{false,true}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,430,400);
        w.loadInto(cid,tid);
        std::vector<World::TransportFx> captured;
        for(int tick=0;tick<120 && !w.unit(cid)->embarked();++tick) {
            w.tick(1.f/30);
            for(const auto& event:w.transportEffects()) {
                check(event.tick==w.tickCount(),"pickup effect retains callback tick");
                captured.push_back(event);
            }
        }
        check(w.unit(cid)->embarked() && captured.size()==1,
              "pickup emits exactly one effect event before attachment");
        if(captured.empty())continue;
        const auto original=captured.front();
        w.unloadAt(tid,500,400,ta::sim::Fixed::raw(1234567));
        for(int tick=0;tick<240 && w.unit(cid)->embarked();++tick) {
            w.tick(1.f/30);
            for(const auto& event:w.transportEffects()) {
                check(event.tick==w.tickCount(),"unload effect retains callback tick");
                check(event.passenger==std::array<int32_t,3>{500*65536,1234567,400*65536},
                      "unload effect captures complete destination position");
                captured.push_back(event);
            }
        }
        check(!w.unit(cid)->embarked() && captured.size()==2,
              "unload emits exactly one effect event before release");
        check(captured.front().passenger==original.passenger &&
              captured.front().carrier==original.carrier && captured.front().tick==original.tick,
              "queued effect values survive later movement and mission completion");
        w.tick(1.f/30);
        check(w.transportEffects().empty(),"transport effects clear on the following simulation tick");
    }
}

static void unloadDestinationRetention() {
    for(bool air:{false,true})for(int32_t height:{-65537,0,20*65536+32768}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,430,400);
        w.loadInto(cid,tid);
        for(int tick=0;tick<120 && !w.unit(cid)->embarked();++tick)w.tick(1.f/30);
        ta::net::Command command;command.kind=ta::net::Cmd::Unload;
        command.unitId=tid;command.x=900;command.z=400;command.targetId=height;
        ta::net::Writer writer;writer.cmd(command);
        ta::net::Reader reader(writer.b.data(),writer.b.size());
        const auto delivered=reader.cmd();
        ta::sim::TypeRegistry registry;ta::sim::applyCommand(w,registry,delivered);
        check(reader.ok && delivered.targetId==height,"unload command wire preserves signed fractional height");
        check(!w.unit(tid)->orders.empty() && w.unit(tid)->orders.back().unload &&
              w.unit(tid)->orders.back().transportY.v==height,
              "command dispatch stores unload destination height in the queued mission");
        if(!w.unit(tid)->orders.empty()) {
            const auto hash=w.stateHash();
            w.unit(tid)->orders.back().transportY.v^=1;
            check(w.stateHash()!=hash,"retained unload height participates in the lockstep checksum");
            w.unit(tid)->orders.back().transportY.v^=1;
        }
        bool retained=true;
        for(int tick=0;tick<30;++tick) {
            for(const auto& order:w.unit(tid)->orders)
                if(order.unload)retained=retained && order.transportY.v==height;
            w.tick(1.f/30);
        }
        check(retained,"unload destination height survives the approach queue");
        w.unloadAt(tid,500,400);
        check(w.unit(tid)->orders.back().transportY.v==0,
              "direct coordinate-only unload defaults to zero mission height");
    }
}

static void unloadTransferInterruption() {
    for(bool air:{false,true})for(int elapsed:{1,7,14}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,430,400);
        w.loadInto(cid,tid);
        for(int tick=0;tick<120 && !w.unit(cid)->embarked();++tick)w.tick(1.f/30);
        check(w.unit(cid)->inTransport==tid,"unload interruption fixture boards passenger");
        w.unloadAt(tid,500,400);
        for(int tick=0;tick<elapsed;++tick)w.tick(1.f/30);
        check(w.unit(cid)->embarked(),"unload interruption precedes passenger release");
        w.stop(tid);
        for(int tick=0;tick<30;++tick)w.tick(1.f/30);
        check(w.unit(cid)->inTransport==tid && w.unit(tid)->cargo.size()==1,
              "stopped unload retains cargo after the original transfer deadline");
        w.unloadAt(tid,500,450);
        for(int tick=0;tick<240 && w.unit(cid)->embarked();++tick)w.tick(1.f/30);
        check(!w.unit(cid)->embarked() && w.unit(tid)->cargo.empty(),
              "replacement unload releases cargo exactly once");
        check(w.unit(cid)->z.toFloat()>420,
              "replacement unload uses its new destination instead of cancelled coordinates");
    }
}

static void pickupTransferInterruption() {
    for(bool air:{false,true})for(bool death:{false,true}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,450,400);
        w.loadInto(cid,tid);
        bool transferring=false;
        for(int tick=0;tick<60 && !transferring;++tick) {
            w.tick(1.f/30);
            for(const auto& order:w.unit(tid)->orders)
                transferring=transferring || (order.transportPickup && order.transportMission.stage==2);
        }
        check(transferring,"interruption fixture reaches pickup transfer before attachment");
        if(death)w.unit(cid)->deadFor=0;
        else w.stop(cid);
        for(int tick=0;tick<30;++tick)w.tick(1.f/30);
        check(w.unit(tid)->cargo.empty() && !w.unit(cid)->embarked(),
              "cancelled or dead passenger never attaches after transfer interruption");
        const int replacement=w.spawn(&passenger,450,430);
        w.loadInto(replacement,tid);
        for(int tick=0;tick<180 && !w.unit(replacement)->embarked();++tick)w.tick(1.f/30);
        check(w.unit(replacement)->inTransport==tid,
              "carrier accepts a new pickup after interrupted transfer");
    }
}

static void pickupRangeBoundary() {
    for(bool air:{false,true}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,550,400.5f);
        w.loadInto(cid,tid);w.tick(1.f/30);w.tick(1.f/30);w.tick(1.f/30);
        const auto* cargo=w.unit(cid);
        check(!w.unit(tid)->orders.empty() && w.unit(tid)->orders.front().transportMission.stage==2 &&
              (!air || (cargo->x==ta::sim::Fixed::fromInt(550) && cargo->z==ta::sim::Fixed::fromFloat(400.5f))),
              air ? "air pickup accepts native fractional range boundary without approach" :
                    "surface carrier starts transfer while its passenger approaches the inner circle");
    }
}

static void pickupBraking() {
    World w;setTestPathfinding(w);
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    UnitType carrier=boatType(),passenger=footType();
    passenger.brake=ta::sim::Fixed::fromFloat(0.1f);
    const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,430,400);
    w.unit(cid)->speed=ta::sim::Fixed::fromInt(1);
    w.loadInto(cid,tid);w.tick(1.f/30);w.tick(1.f/30);w.tick(1.f/30);
    check(w.unit(cid)->speed>ta::sim::Fixed() && w.unit(tid)->orders.front().transportTicks==0 &&
          w.unit(tid)->orders.front().transportApproachAttempts==1,
          "pickup brakes a moving passenger and delays the beam by six ticks");
    for(int tick=0;tick<16;++tick)w.tick(1.f/30);
    check(!w.unit(cid)->embarked(),"moving passenger does not use the stationary pickup deadline");
    for(int tick=0;tick<30 && !w.unit(cid)->embarked();++tick)w.tick(1.f/30);
    check(w.unit(cid)->inTransport==tid,"stopped passenger completes pickup after its beam interval");
}

// Retail's sight batch 4f6a60 has no attachment test, and 4dad30 commits the
// host attach point (4dd250, piece -1 for every pickup) as the cargo's own
// position, height included. A short-sighted flying carrier therefore reveals
// what its long-sighted passenger sees from the carrier's altitude.
static void cargoSight() {
    std::printf("embarked cargo sight:\n");
    auto explored=[](const World& w) {
        size_t count=0;
        for(uint16_t cell:w.navigationExploration()) count+=cell!=0;
        return count;
    };
    UnitType carrier=boatType(),passenger=footType();
    carrier.canFly=true;carrier.cruiseAlt=100;carrier.sight=40;passenger.sight=400;
    auto run=[&](bool withCargo,int& cargoId,int& carrierId) {
        auto w=std::make_unique<World>();setTestPathfinding(*w);
        w->setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
        carrierId=w->spawn(&carrier,1000,1000);
        cargoId=withCargo ? w->spawn(&passenger,1000,1000) : 0;
        if(withCargo) board(*w,carrierId,cargoId);
        w->order(carrierId,1400.f,1000.f,false);
        for(int tick=0;tick<60;++tick) w->tick(1.f/30);
        return w;
    };
    int cargoId=0,carrierId=0,unusedCargo=0,emptyCarrier=0;
    auto loaded=run(true,cargoId,carrierId);
    auto empty=run(false,unusedCargo,emptyCarrier);
    const auto* cargo=loaded->unit(cargoId);
    const auto* host=loaded->unit(carrierId);
    check(cargo && host && cargo->embarked(),"the passenger stays aboard the moving carrier");
    if(cargo && host) {
        check(cargo->x==host->x && cargo->z==host->z,
              "the cargo sits on the carrier (attach piece -1, 4dd250)");
        check(cargo->groundY==host->flightY && host->flightY.floorInt()>100,
              "the cargo height is the carrier's flight height (4dad30 -> 51b3b0)");
        check(cargo->sightFootprint.distance==400 && cargo->sightFootprint.x>=0,
              "the cargo keeps its own live sight footprint");
    } else {
        g_fail+=3;
    }
    check(explored(*loaded)>explored(*empty)+100,
          "the passenger's own sight explores beyond the carrier's");
    // The floater clamp in 4dad3e..4dadd2 wraps its byte arithmetic to sea-waterline.
    const auto low=ta::sim::retailAttachedPosition({1,3<<16,2},true,5,20);
    check(low[1]==15<<16 && low[0]==1 && low[2]==2,"a floater cargo is held at sea minus waterline");
    check(ta::sim::retailAttachedPosition({1,3<<16,2},false,5,20)[1]==3<<16,
          "a non-floater cargo takes the attach height unchanged");
    check(ta::sim::retailAttachedPosition({0,-400*65536,0},true,200,10)[1]==-190*65536,
          "the floater clamp wraps (waterline*0xffff+sea)<<16 to (sea-waterline)<<16");
    check(ta::sim::retailAttachPoint({7,8,9},{},{},-1,0)==std::array<int32_t,3>{7,8,9},
          "attach piece -1 is the host position");
}

static void airPickupTransfer() {
    World w;setTestPathfinding(w);
    w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
    UnitType carrier=boatType(),passenger=footType();carrier.canFly=true;carrier.cruiseAlt=100;
    const int tid=w.spawn(&carrier,400,400),first=w.spawn(&passenger,430,400),second=w.spawn(&passenger,430,430);
    w.loadInto(first,tid);w.loadInto(second,tid);
    w.tick(1.f/30);
    const auto* before=w.unit(tid);
    const auto departure=ta::sim::retailUnloadStepOut({before->x.v,before->flightY.v,before->z.v},
        ta::sim::portHeadingToRetail(before->heading),uint16_t(carrier.transportDist));
    w.tick(1.f/30);w.tick(1.f/30);
    const auto* transport=w.unit(tid);
    check(!transport->orders.empty() && transport->orders.front().flightGoal &&
          transport->orders.front().flightGoal->radius==16 && transport->flightGroundMode==2,
          "air pickup starts the retail departure controller during transfer");
    const auto& goal=transport->orders.front().flightGoal;
    check(goal && goal->point.x==departure.point.x && goal->point.z==departure.point.z,
          "air pickup retains the departure point through flight updates");
    check(w.unit(second)->orders.front().transportTicks==0,
          "air carrier transfers only its assigned passenger at a time");
    // Once the beam is committed, the carrier's continuing motion must not
    // reset it when the pair move outside the initial pickup radius.
    w.unit(tid)->x=ta::sim::Fixed::fromInt(1000);
    for(int tick=0;tick<20 && !w.unit(first)->embarked();++tick)w.tick(1.f/30);
    check(w.unit(first)->inTransport==tid,"committed air pickup finishes beyond the approach radius");
    w.stop(tid);
    for(int tick=0;tick<30;++tick)w.tick(1.f/30);
    check(!w.unit(second)->embarked() && w.unit(second)->orders.empty(),
          "canceling carrier pickup also ends its waiting passenger request");
}

static void surfacePickupQueue(bool water) {
    World w;setTestPathfinding(w);
    w.setTerrain(water ? coastHeights(96,96,25) : std::vector<uint8_t>(96*96,100),96,96,40);
    w.setPathService(true);
    UnitType carrier=boatType(),passenger=footType();
    if(water)carrier.domain=UnitType::Domain::Water;
    const int tid=w.spawn(&carrier,360,400),first=w.spawn(&passenger,430,400),
        second=w.spawn(&passenger,430,430),third=w.spawn(&passenger,430,460);
    w.loadInto(first,tid);w.loadInto(second,tid);w.loadInto(third,tid);
    for(int tick=0;tick<3;++tick)w.tick(1.f/30);
    check(w.unit(tid)->orders.front().targetId==first && w.unit(tid)->orders.front().transportTicks>0 &&
          w.unit(second)->orders.front().transportTicks==0 &&
          w.unit(third)->orders.front().transportTicks==0,
          "surface carrier transfers only its active passenger");
    for(int tick=0;tick<30 && !w.unit(first)->embarked();++tick)w.tick(1.f/30);
    check(w.unit(first)->inTransport==tid && !w.unit(second)->embarked(),
          "surface passengers do not board simultaneously");
    for(int tick=0;tick<30 && !w.unit(second)->embarked();++tick)w.tick(1.f/30);
    check(w.unit(second)->inTransport==tid && !w.unit(third)->embarked(),
          "surface carrier advances to the next waiting passenger");
    w.stop(tid);
    for(int tick=0;tick<30;++tick)w.tick(1.f/30);
    check(!w.unit(third)->embarked() && w.unit(third)->orders.empty(),
          "canceling surface pickup ends the remaining passenger request");
}

static void pickupMissionOwnership() {
    for(bool air:{false,true})for(bool passengerFirst:{false,true}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;carrier.cruiseAlt=100;
        int tid,cid;
        if(passengerFirst) {cid=w.spawn(&passenger,430,400);tid=w.spawn(&carrier,400,400);}
        else {tid=w.spawn(&carrier,400,400);cid=w.spawn(&passenger,430,400);}
        w.loadInto(cid,tid);
        for(int tick=0;tick<3;++tick)w.tick(1.f/30);
        check(w.unit(tid)->orders.front().transportMission.stage==2 &&
              w.unit(tid)->orders.front().transportTicks==1 &&
              w.unit(cid)->orders.front().transportMission.stage==2 &&
              w.unit(cid)->orders.front().transportApproachAttempts==1 &&
              w.unit(cid)->orders.front().transportTicks==0,
              "carrier owns the transfer state regardless of unit update order");
        for(int tick=3;tick<17;++tick)w.tick(1.f/30);
        check(!w.unit(cid)->embarked(),"carrier retains passenger until all fifteen beam waits elapse");
        w.tick(1.f/30);
        check(w.unit(cid)->inTransport==tid && w.unit(tid)->orders.empty(),
              "carrier attaches passenger and removes its completed pickup mission");
    }
    for(bool air:{false,true}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;carrier.cruiseAlt=100;
        passenger.brake=ta::sim::Fixed();
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,430,400);
        w.unit(cid)->speed=ta::sim::Fixed::fromInt(1);
        w.loadInto(cid,tid);
        for(int tick=0;tick<3;++tick)w.tick(1.f/30);
        check(w.unit(tid)->orders.front().transportApproachAttempts==1 &&
              w.unit(cid)->orders.front().transportApproachAttempts==1,
              "carrier beam retries and passenger attachment waits have independent counters");
        for(int tick=3;tick<210;++tick)w.tick(1.f/30);
        check(!w.unit(cid)->embarked() && w.unit(cid)->orders.empty() && w.unit(tid)->orders.empty(),
              "retry exhaustion removes pickup without leaving an orphaned request");
    }
}

static void pickupApproachPolling() {
    for(bool air:{false,true})for(bool wakeEarly:{false,true}) {
        World w;setTestPathfinding(w);w.setGameSeed(71);
        w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;carrier.cruiseAlt=100;
        const int tid=w.spawn(&carrier,400,400),far=w.spawn(&passenger,1000,400),
            near=w.spawn(&passenger,440,440);
        w.loadInto(far,tid);w.tick(1.f/30);
        const auto& seek=w.unit(far)->orders.back();
        check(seek.transportMission.stage==1 && seek.transportMission.deadline==31 &&
              seek.transportMission.waitMask==(air ? 0x89u : 0x789u) &&
              (air ? !seek.controller : seek.controller && seek.missionRadius+4==134),
              "live passenger initializes native air wait or surface approach controller");
        w.tick(1.f/30);
        const auto& pending=w.unit(tid)->orders.front().transportMission;
        const uint32_t deadline=pending.deadline;
        check(pending.waitMask==(air ? 0x729u : 0x709u) &&
              (air ? deadline>=8 && deadline<=13 : deadline==17),
              "pickup approach retains the native event mask and polling interval");
        const auto x=w.unit(tid)->x,z=w.unit(tid)->z;
        w.loadInto(near,tid);w.tick(1.f/30);
        check(w.unit(tid)->orders.front().targetId==far &&
              w.unit(tid)->orders.front().transportMission.deadline==deadline,
              "waiting pickup does not rescan passengers on every movement tick");
        check(w.unit(tid)->x!=x || w.unit(tid)->z!=z,
              "carrier movement continues while the approach mission sleeps");
        if(wakeEarly)w.unit(tid)->missionEvents|=0x100;
        const uint32_t wakeTick=wakeEarly ? 4 : deadline;
        for(uint32_t tick=4;tick<=wakeTick;++tick)w.tick(1.f/30);
        check(w.unit(tid)->orders.front().targetId==near && !(w.unit(tid)->missionEvents&0x100),
              wakeEarly ? "movement event wakes pickup and is consumed before selection" :
                          "pickup deadline wakes selection without a movement event");
    }
}

static void surfacePickupNavigation() {
    {
        World w;setTestPathfinding(w);
        std::vector<uint8_t> heights(96*96,100);
        for(int z=0;z<45;++z)for(int x=35;x<45;++x)heights[size_t(z)*96+x]=0;
        w.setTerrain(heights,96,96,40);w.setPathService(true);
        // Legion plans on the placement plane: on the legacy nav-grid plane a
        // body (a plain Move as well) holds at the water's corner, the known
        // gap in docs/legion-pathfinding.md "Scope".
        const bool legion=g_pathfinding==ta::sim::PathfindingMode::Legion;
        if(legion)w.setMapPlacementFeatures(std::vector<uint16_t>(size_t(96)*96,0xffff),{});
        UnitType carrier=boatType(),passenger=footType();
        carrier.maxVel=ta::sim::Fixed::raw(1);
        passenger.brake=ta::sim::Fixed::fromFloat(0.1f);
        const int tid=w.spawn(&carrier,1000,400),cid=w.spawn(&passenger,200,400);
        w.loadInto(cid,tid);
        bool sawRoute=false,preserved=true;
        for(int tick=0;tick<4000 && !w.unit(cid)->embarked();++tick) {
            w.tick(1.f/30);
            const auto& orders=w.unit(cid)->orders;
            size_t end=0;while(end<orders.size() && !orders[end].goal)++end;
            // Legion installs no native waypoints: the route is Legion's
            // while it serves this pickup leg.
            if(legion ? w.legionNavigator() && w.legionNavigator()->mission(*w.unit(cid))==ta::sim::LegionMission::Load &&
                        end<orders.size() && orders[end].controller :
                        end>0 && end<orders.size()) {
                sawRoute=true;
                for(size_t i=0;i<=end;++i)
                    preserved=preserved && orders[i].load && !orders[i].transportPickup &&
                        orders[i].targetId==tid;
                preserved=preserved && orders[end].transportMission.stage<=1;
            }
        }
        check(sawRoute && preserved,"passenger routes around water while retaining its pickup mission");
        check(w.unit(cid)->inTransport==tid,"routed passenger reaches stationary carrier and boards");
    }
    {
        World w;setTestPathfinding(w);
        auto heights=coastHeights(96,96,50);
        // A peninsula blocks the straight line from the ship to the passengers.
        for(int z=28;z<35;++z)for(int x=25;x<50;++x)heights[size_t(z)*96+x]=100;
        w.setTerrain(heights,96,96,40);w.setPathService(true);
        UnitType carrier=boatType(),passenger=footType();carrier.domain=UnitType::Domain::Water;
        passenger.maxVel=ta::sim::Fixed::raw(1);
        const int tid=w.spawn(&carrier,200,300),first=w.spawn(&passenger,850,800),
            second=w.spawn(&passenger,850,850);
        w.loadInto(first,tid);w.loadInto(second,tid);
        bool sawRoute=false,preserved=true,sawRadius=false;
        for(int tick=0;tick<4000 && !w.unit(second)->embarked();++tick) {
            w.tick(1.f/30);
            const auto& orders=w.unit(tid)->orders;
            size_t end=0;while(end<orders.size() && !orders[end].goal)++end;
            if(end<orders.size() && orders[end].controller) {
                sawRadius=sawRadius || orders[end].missionRadius+4==134;
                if(end>0) {
                    sawRoute=true;
                    for(size_t i=0;i<=end;++i)
                        preserved=preserved && orders[i].load && orders[i].transportPickup &&
                            orders[i].targetId==orders[end].targetId;
                }
            }
        }
        check(sawRoute && preserved,"surface pickup preserves its mission through routed waypoints");
        check(sawRadius,"surface pickup installs transportdistance minus 16 as its navigator radius");
        check(w.unit(first)->inTransport==tid && w.unit(second)->inTransport==tid,
              "ship routes around a peninsula and boards both queued passengers");
    }
    {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);w.setPathService(true);
        UnitType carrier=boatType(),passenger=footType();
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,1000,400);
        w.loadInto(cid,tid);w.tick(1.f/30);w.tick(1.f/30);
        w.unit(tid)->x=ta::sim::Fixed::fromInt(875);
        w.tick(1.f/30);
        const auto& goal=w.unit(tid)->orders.front();
        check((goal.transportMission.pending&0x500)==0x500 && goal.controller==0,
              "surface navigator generates arrival and detachment for pickup");
        w.tick(1.f/30);
        check(w.unit(tid)->orders.front().transportMission.stage==2,
              "surface pickup consumes navigator arrival before the polling deadline");
    }
    {
        World w;setTestPathfinding(w);
        auto heights=coastHeights(96,96,50);
        for(int z=5;z<=9;++z)for(int x=5;x<=9;++x)
            if(x!=7 || z!=7)heights[size_t(z)*96+x]=100;
        w.setTerrain(heights,96,96,40);w.setPathService(true);
        UnitType carrier=boatType(),passenger=footType();carrier.domain=UnitType::Domain::Water;
        const int tid=w.spawn(&carrier,120,120),cid=w.spawn(&passenger,850,800);
        w.loadInto(cid,tid);bool failed=false;
        for(int tick=0;tick<100 && !failed;++tick) {
            w.tick(1.f/30);
            for(const auto& order:w.unit(tid)->orders)
                failed=failed || (order.transportPickup && (order.transportMission.pending&0x200));
        }
        check(failed,"boxed ship generates a native navigator failure for its pickup mission");
        if(failed) {
            w.unit(cid)->speed=ta::sim::Fixed();
            w.tick(1.f/30);
            check(w.unit(tid)->orders.empty() && !w.unit(cid)->embarked(),
                  "navigator failure aborts pickup when its passenger has stopped");
            for(int tick=0;tick<35 && !w.unit(cid)->orders.empty();++tick)w.tick(1.f/30);
            check(w.unit(cid)->orders.empty(),"failed pickup passenger retires on its own scheduled wake");
        }
    }
}

static void pickupApproachFailure() {
    for(bool air:{false,true})for(bool moving:{false,true})for(bool inRange:{false,true}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;carrier.cruiseAlt=100;
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,1000,400);
        w.loadInto(cid,tid);w.tick(1.f/30);w.tick(1.f/30);
        if(inRange)w.unit(cid)->x=ta::sim::Fixed::fromInt(430);
        w.unit(cid)->speed=moving ? ta::sim::Fixed::fromInt(1) : ta::sim::Fixed();
        w.unit(tid)->missionEvents|=0x200;
        w.tick(1.f/30);
        const auto& orders=w.unit(tid)->orders;
        const bool abort=!air && !moving && !inRange;
        check(orders.empty()==abort && !(w.unit(tid)->missionEvents&0x200),
              "pickup failure aborts only an out-of-range stationary surface passenger");
        if(inRange)check(!orders.empty() && orders.front().transportMission.stage==2,
              "in-range pickup enters transfer despite an approach failure event");
        if(abort) {
            check(!w.unit(cid)->orders.empty(),"carrier abort preserves the sleeping passenger request");
            for(int tick=0;tick<35 && !w.unit(cid)->orders.empty();++tick)w.tick(1.f/30);
            check(w.unit(cid)->orders.empty() && !w.unit(cid)->embarked(),
                  "failed surface pickup passenger cleans up when its scheduler wakes");
        }
    }
}

static void airPickupArrivalEvent() {
    World w;setTestPathfinding(w);w.setGameSeed(71);
    w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
    UnitType carrier=boatType(),passenger=footType();carrier.canFly=true;carrier.cruiseAlt=100;
    const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,1000,400);
    w.loadInto(cid,tid);w.tick(1.f/30);w.tick(1.f/30);
    const auto& initial=w.unit(tid)->orders.front();
    const uint32_t deadline=initial.transportMission.deadline;
    check(initial.flightGoal && initial.flightGoal->flags==0x11 && initial.flightGoal->radius==149 &&
          initial.flightGoal->point.y==w.unit(cid)->groundY.v,
          "air pickup pursues passenger height with the native arrival radius");
    // Put the controller inside its radius before its polling deadline. The
    // actual movement update must generate the event; do not inject it here.
    w.unit(tid)->x=ta::sim::Fixed::fromInt(852);
    w.tick(1.f/30);
    check(w.unit(tid)->orders.front().transportMission.stage==1 &&
          (w.unit(tid)->orders.front().transportMission.pending&0x100) && deadline>4,
          "air pursuit generates an arrival event before the mission deadline");
    check(w.unit(tid)->flightNavigation.destination.y==w.unit(cid)->groundY.v,
          "air pickup navigation uses passenger height when within 160 units");
    w.tick(1.f/30);
    check(w.unit(tid)->orders.front().transportMission.stage==2 &&
          !(w.unit(tid)->orders.front().transportMission.pending&0x100),
          "air pickup consumes controller arrival and enters transfer on the next tick");
    for(int tick=0;tick<20 && !w.unit(cid)->embarked();++tick)w.tick(1.f/30);
    check(w.unit(cid)->inTransport==tid,"arrival-driven air pickup completes boarding");
}

static void pickupNearbySelection(bool air,bool slowBrake=false) {
    World w;setTestPathfinding(w);w.setGameSeed(173);w.setPathService(true);
    w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
    UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;carrier.cruiseAlt=100;
    if(!slowBrake)passenger.brake=ta::sim::Fixed::fromFloat(0.125f);
    const int tid=w.spawn(&carrier,400,400),far=w.spawn(&passenger,1000,400);
    const int first=w.spawn(&passenger,430,400),second=w.spawn(&passenger,430,430),
        canceled=w.spawn(&passenger,400,430);
    w.loadInto(far,tid);w.loadInto(second,tid);w.loadInto(canceled,tid);w.loadInto(first,tid);
    w.stop(canceled);
    int bound=0,choice=-1;
    w.setRngObserver([&](const World::RngObservation& observation) {
        if(std::strstr(observation.caller.function_name(),"tickTransport")) {
            bound=observation.bound;choice=int(observation.result);
        }
    });
    w.tick(1.f/30);w.tick(1.f/30);
    const auto& orders=w.unit(tid)->orders;
    const int expected=choice==0 ? first : second;
    check(bound==2 && choice>=0 && choice<2 && !orders.empty() && orders.front().targetId==expected,
          air ? "air pickup randomly selects eligible nearby passengers in unit order" :
                "surface pickup randomly selects eligible nearby passengers in unit order");
    check(orders.size()==4 && orders[1].targetId==far,
          "nearby pickup preserves the interrupted distant request");
    check(orders[1].transportMission.stage==0 && orders[1].transportMission.waitMask==0,
          "temporary pickup resets the interrupted mission stage and wait mask as retail does");
    for(int tick=0;tick<30 && !w.unit(expected)->embarked();++tick)w.tick(1.f/30);
    check(w.unit(expected)->inTransport==tid && !w.unit(far)->embarked(),
          "nearby passenger boards before the distant queued target");
    w.stop(expected==first ? second : first);
    bool resumedTransfer=false;
    for(int tick=0;tick<2000 && !w.unit(far)->embarked();++tick) {
        w.tick(1.f/30);
        for(const auto& order:w.unit(tid)->orders)
            if(order.transportPickup && order.targetId==far && order.transportMission.stage==2)
                resumedTransfer=true;
    }
    check(resumedTransfer,"interrupted distant pickup resumes through its transfer stage");
    if(slowBrake) {
        check(!w.unit(far)->embarked() && w.unit(tid)->orders.empty() && w.unit(far)->orders.empty(),
              "surface pickup exhausts retail retries when the passenger cannot brake in time");
        check(w.unit(expected)->inTransport==tid,"failed resumed pickup preserves existing cargo");
        w.loadInto(far,tid);
        for(int tick=0;tick<300 && !w.unit(far)->embarked();++tick)w.tick(1.f/30);
        check(w.unit(far)->inTransport==tid && w.unit(expected)->inTransport==tid,
              "a fresh pickup succeeds after the failed passenger has stopped");
    } else {
        check(w.unit(far)->inTransport==tid && w.unit(expected)->inTransport==tid,
              air ? "air carrier resumes and completes the interrupted distant pickup" :
                    "surface carrier resumes and completes the interrupted distant pickup");
    }
}

// TotalA.exe 0x41121a..0x411226: VTOL_PICKUP fails while the carrier holds
// anyone, so an air transport takes one passenger whatever its capacity; a
// surface carrier fills to transportcapacity (0x489aec).
static void airCarriesOne() {
    for(bool air:{false,true}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;carrier.cruiseAlt=100;
        const int tid=w.spawn(&carrier,400,400),first=w.spawn(&passenger,430,400),
            second=w.spawn(&passenger,430,430);
        board(w,tid,first);
        check(w.canLoadInto(second,tid),"the load order counts only against transportcapacity");
        w.loadInto(second,tid);
        for(int tick=0;tick<300 && !w.unit(second)->embarked();++tick)w.tick(1.f/30);
        check(w.unit(second)->embarked()==!air,
              air ? "an air carrier with a passenger fails a second pickup" :
                    "a surface carrier with a passenger takes a second");
        check(w.unit(first)->inTransport==tid,"the first passenger stays aboard");
    }
}

static void retailRoster(const char* root) {
    auto vfs=ta::hpi::mountRetailRoot(root);
    {
        ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
        for(bool air:{false,true}) {
            World w;setTestPathfinding(w);
            w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
            auto carrier=boatType(),target=footType();carrier.canFly=air;carrier.cruiseAlt=100;
            target.maxHp=10000;
            const auto* archer=registry.find("armpw");
            if(!archer)continue;
            const int tid=w.spawn(&carrier,400,400),cid=w.spawn(archer,430,400),
                enemy=w.spawn(&target,430,600,0,1);
            w.attack(cid,enemy,false);
            for(int tick=0;tick<120 && !w.unit(cid)->scriptAimTarget;++tick)w.tick(1.f/30);
            check(w.unit(cid)->scriptAimTarget==enemy,"armed passenger has an active scripted aim before pickup");
            w.loadInto(cid,tid);
            w.unit(cid)->reloads[0]=3;
            int clears=0;
            for(int tick=0;tick<180 && !w.unit(cid)->embarked();++tick) {
                w.tick(1.f/30);
                if(tick==0)check(w.unit(cid)->reloads[0]==2,
                    "passenger reload advances while executing pickup");
                const auto& events=w.unit(cid)->weaponAnimations;
                for(size_t i=0;i<events.count;++i)
                    clears+=events.at(i).kind==ta::RetailWeaponAnimation::Clear;
            }
            check(w.unit(cid)->embarked(),"armed passenger boards through its real load order");
            check(!w.unit(cid)->scriptAimTarget && clears==1,
                  "boarding clears the old aim target and emits one TargetCleared callback");
            w.unit(cid)->reloads[0]=3;
            for(int tick=0;tick<5;++tick)w.tick(1.f/30);
            check(w.unit(cid)->embarked() && w.unit(cid)->reloads[0]==0,
                  "passenger reload finishes while aboard without underflow");
            w.unloadAt(tid,430,450);
            check(runUntilUnloaded(w,tid),"armed passenger can unload after clearing its old aim");
            w.attack(cid,enemy,false);
            bool fired=false;
            for(int tick=0;tick<300 && !fired;++tick) {
                w.tick(1.f/30);fired=w.unit(cid)->firedWeapons!=0;
            }
            check(fired,"unloaded passenger can acquire aim and fire again");
        }
        // (Kingdoms' conjurers trained units while mobile and had their
        // production cancelled on boarding; no TA mobile unit trains.)
        int carriers=0;
        for(const auto& [name,type]:registry.types()) {
            if(!type.canTransport) continue;
            ++carriers;
            std::printf("carrier=%s count=%d budget=%d maximum=%d distance=%d\n",
                name.c_str(),type.transportCap,type.transportSizeCap,type.maxTransportSize,type.transportDist);
            // TA's two limits (docs/retail-data.md 2.1, +0x22a/+0x22b):
            // transportcapacity, and transportsize, the largest passenger.
            check(type.transportCap>0 && type.maxTransportSize>0,"retail carrier has nonzero capacity limits");
            World w;setTestPathfinding(w);
            w.setTerrain(coastHeights(128,128,50),128,128,40);
            w.buildNavClasses(registry);w.setPathService(true);
            const auto* foot=registry.find("armpw");
            check(foot && foot->transportSize==foot->footX,
                "passenger size is its final movement footprint x (TotalA.exe 0x489b0b)");
            if(!foot) continue;
            const int tid=w.spawn(&type,type.domain==UnitType::Domain::Ground && !type.canFly ? 900 : 720,700);
            const int cid=w.spawn(foot,832,700);
            check(w.canLoadInto(cid,tid),"real transport accepts a shore passenger");
            w.loadInto(cid,tid);
            for(int tick=0;tick<120 && !w.unit(cid)->embarked();++tick) w.tick(1.f/30);
            check(w.unit(cid)->inTransport==tid,"real passenger boards through a load order");
            w.unloadAt(tid,864,1000);
            const bool unloaded=runUntilUnloaded(w,tid);
            check(unloaded && !w.unit(cid)->embarked(),"real carrier travels and unloads passenger");
            check(w.unit(cid)->x.toFloat()>=800,"passenger unloads on land");
            // Reuse the same pair: stale attachment, mission or effect state can
            // leave the first trip green while preventing the next pickup.
            if (unloaded && !w.unit(cid)->embarked()) {
                w.loadInto(cid,tid);
                for(int tick=0;tick<600 && !w.unit(cid)->embarked();++tick) w.tick(1.f/30);
                check(w.unit(cid)->inTransport==tid,"real carrier can reboard its released passenger");
                if (w.unit(cid)->inTransport==tid) {
                    w.unloadAt(tid,864,700);
                    check(runUntilUnloaded(w,tid) && !w.unit(cid)->embarked(),
                          "real carrier completes a second coastal trip");
                    check(w.unit(tid)->cargo.empty() && w.unit(cid)->x.toFloat()>=800,
                          "second trip leaves empty cargo and a passenger on land");
                }
            }
        }
        // Atlas, Valkyrie, both transport ships and both Core Contingency hovers.
        check(carriers==6,"all retail carriers covered");
    }
}

// After the last passenger leaves, every shipped carrier stays where it dropped
// its cargo. VTOL_UNLOAD retires with its step-out controller still attached --
// aimed 2*(transportdistance+400)px ahead -- and retail's dispatcher gives the
// emptied list its default mission in that same call (4d856a); VTOL_Standby
// installs VTOL_LANDIFCAN two ticks later, whose 32px brake point replaces the
// controller. World let the controller run on instead: a Kingdoms Roc (replay
// game-1-1791860558, Legion) flew 1400px past its drop point, and other
// carriers did the same. Each TA carrier,
// in both pathfinding modes, with one passenger and with a full load, unloading
// in range and after an approach: once released it comes to rest within its own
// stopping distance of where it let go (flyers: plus the landing search's reach).
static void postUnloadRoster(const char* root) {
    using ta::sim::Fixed;
    using ta::sim::PathfindingMode;
    std::printf("every carrier stays at its drop point after unloading:\n");
    auto vfs=ta::hpi::mountRetailRoot(root);
    int carriers=0,cases=0,drifted=0;
    {
        ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
        const auto* foot=registry.find("armpw");
        check(foot!=nullptr,"post-unload roster passenger type is available");
        if(!foot)return;
        for(const auto& [name,type]:registry.types()) {
            if(!type.canTransport)continue;
            ++carriers;
            const bool landCarrier=type.canFly || type.domain==UnitType::Domain::Ground;
            const int fits=type.canFly ? 1 : std::min(type.transportCap,5);
            for(PathfindingMode mode:{PathfindingMode::Retail,PathfindingMode::Legion})
            for(int pass=0;pass<2;++pass) {
                const int count=pass ? fits : 1;   // one passenger, then a full load
                if(pass && fits==1)continue;
                for(bool near:{true,false}) {
                    World w;
                    // Ships need the coast; flyers and hovers drop on open land,
                    // where the landing mission need not search past the water.
                    w.setTerrain(landCarrier ? std::vector<uint8_t>(128*128,100) : coastHeights(128,128,50),128,128,40);
                    w.buildNavClasses(registry);w.setPathService(true);w.setPathfindingMode(mode);
                    const int tid=w.spawn(&type,landCarrier ? 900 : 720,700);
                    std::vector<int> cargo;
                    for(int i=0;i<count;++i) {
                        cargo.push_back(w.spawn(foot,832+40*(i%3),620+40*(i/3)));
                        w.loadInto(cargo.back(),tid);
                    }
                    auto aboard=[&] {
                        return std::all_of(cargo.begin(),cargo.end(),
                            [&](int id){return w.unit(id)->inTransport==tid;});
                    };
                    for(int tick=0;tick<3000 && !aboard();++tick)w.tick(1.f/30);
                    char what[160];
                    std::snprintf(what,sizeof what,"%s %s x%d %s: boards",name.c_str(),mode==PathfindingMode::Retail?"retail":"legion",
                        count,near?"near":"far");
                    check(aboard(),what);
                    if(!aboard())continue;
                    ta::sim::Unit* t=w.unit(tid);
                    const float dropX=near ? std::max(880.f,t->x.toFloat()-60) : 900.f;
                    const float dropZ=near ? t->z.toFloat()+80 : std::min(t->z.toFloat()+800,1900.f);
                    const bool inRange=ta::sim::retailTransportInRange((Fixed::fromFloat(dropX)-t->x).v,
                        (Fixed::fromFloat(dropZ)-t->z).v,uint16_t(type.transportDist));
                    // One passenger per GROUND_UNLOAD (TotalA.exe 0x4069f0..0x406a09):
                    // a full load takes one (shift-)queued unload per passenger.
                    // An unload whose drop is refused three times while another
                    // order waits is dropped (0x43b8e5); the player clicks again.
                    for(int i=0;i<(type.canFly ? 1 : count);++i)w.unloadAt(tid,dropX,dropZ,{},i>0);
                    bool released=false;
                    for(int tick=0;tick<6000 && !released;++tick) {
                        w.tick(1.f/30);
                        released=w.unit(tid)->cargo.empty();
                        if(!released && w.unit(tid)->orders.empty())w.unloadAt(tid,dropX,dropZ);
                    }
                    std::snprintf(what,sizeof what,"%s %s x%d %s: unloads",name.c_str(),mode==PathfindingMode::Retail?"retail":"legion",
                        count,near?"near":"far");
                    check(released,what);
                    if(!released) {
                        std::printf("    left aboard %zu, orders %zu\n",w.unit(tid)->cargo.size(),w.unit(tid)->orders.size());
                        continue;
                    }
                    t=w.unit(tid);
                    const float x0=t->x.toFloat(),z0=t->z.toFloat(),v0=t->speed.toFloat();
                    const float decel=(type.canFly ? type.accel : type.brake).toFloat();
                    // Stopping distance from the release speed, the ticks before the
                    // default mission takes over, and (flyers) VTOL_LANDIFCAN's 32px
                    // brake point plus its candidate search, +-240px a side
                    // (retailLandingCandidates). The step-out point is 2*(range+400)px.
                    const float allowance=v0*v0/(2*std::max(decel,1e-3f))+4*v0+96+(type.canFly ? 32+340 : 0);
                    float drift=0;bool defaulted=!type.canFly;
                    for(int tick=0;tick<900;++tick) {
                        w.tick(1.f/30);
                        const auto& c=*w.unit(tid);
                        drift=std::max(drift,std::hypot(c.x.toFloat()-x0,c.z.toFloat()-z0));
                        // 4d856a + VTOL_Standby: standing by at once, landing two ticks on.
                        // (A ship's GROUND_UNLOAD tail first lets its route coast out.)
                        if(type.canFly && tick<4)defaulted|=c.landing.has_value();
                    }
                    t=w.unit(tid);
                    const bool atRest=t->speed==Fixed() && t->orders.empty() && t->cargo.empty() &&
                        t->standbyActive && (!type.canFly || t->flightGroundMode==1);
                    ++cases;drifted+=drift>allowance;
                    std::printf("    post-unload %-9s %-6s x%d %-4s inRange=%d v0=%.2f default=%d drift=%.0f "
                                "allowance=%.0f drop=%.0f rest=%d\n",name.c_str(),
                                mode==PathfindingMode::Retail?"retail":"legion",count,near?"near":"far",
                                int(inRange),v0,int(defaulted),drift,allowance,
                                std::hypot(t->x.toFloat()-dropX,t->z.toFloat()-dropZ),int(atRest));
                    std::snprintf(what,sizeof what,"%s %s x%d %s: stays at its drop point",name.c_str(),mode==PathfindingMode::Retail?"retail":"legion",
                        count,near?"near":"far");
                    check(defaulted && drift<=allowance && atRest,what);
                }
            }
        }
    }
    check(carriers==6,"post-unload roster covers all 6 carriers");
    std::printf("  post-unload: %d carriers, %d cases, %d drifted\n",carriers,cases,drifted);
}

// Standing by after the drop must not swallow queued work: with a move or a pickup
// queued behind the unload, each air carrier takes it up instead of landing.
static void postUnloadQueued(const char* root) {
    using ta::sim::PathfindingMode;
    std::printf("queued orders after an air unload:\n");
    auto vfs=ta::hpi::mountRetailRoot(root);
    {
        ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
        const auto* foot=registry.find("armpw");
        if(!foot)return;
        for(const char* id:{"armatlas","corvalk"}) {
            const auto* type=registry.find(id);
            if(!type)continue;
            for(PathfindingMode mode:{PathfindingMode::Retail,PathfindingMode::Legion})
            for(int kind=0;kind<2;++kind) {
                World w;
                w.setTerrain(std::vector<uint8_t>(128*128,100),128,128,40);
                w.buildNavClasses(registry);w.setPathService(true);w.setPathfindingMode(mode);
                const int tid=w.spawn(type,900,700);
                const int a=w.spawn(foot,832,620),b=w.spawn(foot,832,1500);
                w.loadInto(a,tid);
                for(int tick=0;tick<3000 && w.unit(a)->inTransport!=tid;++tick)w.tick(1.f/30);
                w.unloadAt(tid,880,780);
                // TotalA.exe 0x489aec: the LOAD order counts the passengers aboard
                // when it is given, so a full Valkyrie (transportcapacity 1)
                // refuses a pickup queued behind its unload.
                if(kind==1 && !w.canLoadInto(b,tid)) {
                    check(type->transportCap<=1,"only a full carrier refuses the queued pickup");
                    continue;
                }
                if(kind==0)w.order(tid,1500,1500,true);
                else w.loadInto(b,tid,true);
                bool done=false;
                for(int tick=0;tick<4000 && !done;++tick) {
                    w.tick(1.f/30);
                    const auto& t=*w.unit(tid);
                    done=kind ? w.unit(b)->inTransport==tid
                              : t.orders.empty() && std::hypot(t.x.toFloat()-1500,t.z.toFloat()-1500)<48;
                }
                const auto& t=*w.unit(tid);
                char what[160];
                std::snprintf(what,sizeof what,"%s %s: %s after unload (at %.0f,%.0f)",id,
                    mode==PathfindingMode::Retail?"retail":"legion",kind ? "queued pickup boards" : "queued move arrives",
                    t.x.toFloat(),t.z.toFloat());
                check(done && w.unit(a)->inTransport==0,what);
            }
        }
    }
}

static void airAcrossWater() {
    World w;setTestPathfinding(w);
    auto heights=coastHeights(96,96,60);
    // Small launch island, disconnected from the destination land.
    for(int z=5;z<15;++z) for(int x=5;x<15;++x) heights[z*96+x]=100;
    w.setTerrain(heights,96,96,40);w.setPathService(true);
    UnitType air=boatType(),foot=footType();
    air.canFly=true;air.cruiseAlt=100;air.transportDist=154;
    const auto footAcceleration=foot.accel;
    const int tid=w.spawn(&air,160,160),cid=w.spawn(&foot,180,160);
    w.loadInto(cid,tid);
    for(int tick=0;tick<30;++tick) w.tick(1.f/30);
    check(w.unit(cid)->embarked(),"air carrier boards on launch island");
    w.unloadAt(tid,180,160);
    check(runUntilUnloaded(w,tid),"air carrier unloads its launch-island passenger before the route test");
    // A second passenger on the far bank cannot walk to the launch island.
    const int remote=w.spawn(&foot,1200,1000);
    w.loadInto(remote,tid);
    for(int tick=0;tick<4000 && !w.unit(remote)->embarked();++tick)w.tick(1.f/30);
    check(w.unit(remote)->inTransport==tid,"air carrier flies across water to collect a distant passenger");
    // Hold released units still for the exact-site assertion; restore their
    // normal movement before checking that PARK clears the landing footprint.
    foot.accel={};
    const auto unloadStartX=w.unit(tid)->x,unloadStartZ=w.unit(tid)->z;
    // Start more than transportdistance away. Retail installs an exact-site
    // VTOL approach controller with radius transportdistance-34 before the
    // step-out controller, so the mission must own this active flight order.
    w.unloadAt(tid,1480,1200);
    const auto* approach=w.unit(tid);
    check(approach->orders.size()==1 && approach->orders.front().unload &&
          approach->orders.front().transportUnloadApproach,
        "distant air unload keeps its approach controller on the unload mission");
    w.tick(1.f/30);
    approach=w.unit(tid);
    const auto* flightGoal=approach->orders.empty()?nullptr:(
        approach->orders.front().flightGoal ? &*approach->orders.front().flightGoal : nullptr);
    check(flightGoal && flightGoal->point.x==ta::sim::Fixed::fromInt(1480).v &&
          flightGoal->point.z==ta::sim::Fixed::fromInt(1200).v &&
          flightGoal->flags==0x30 &&
          flightGoal->radius==int16_t(air.transportDist-34),
        "distant air unload first flies to the native transportdistance-34 circle");
    const bool unloaded=runUntilUnloaded(w,tid);
    check(unloaded,"air carrier crosses water and unloads on destination land");
    foot.accel=footAcceleration;
    const auto* released=w.unit(remote);
    const float unloadTravel=std::hypot((w.unit(tid)->x-unloadStartX).toFloat(),
                                        (w.unit(tid)->z-unloadStartZ).toFloat());
    check(unloadTravel>150.0f,"air carrier physically flies to the distant unload point");
    check(std::abs((released->x-ta::sim::Fixed::fromInt(1480)).toFloat())<0.01f &&
          std::abs((released->z-ta::sim::Fixed::fromInt(1200)).toFloat())<0.01f,
          "air carrier releases cargo at the selected landing point");
    const auto parkOrder=std::find_if(released->orders.begin(),released->orders.end(),
        [](const auto& order){return order.park.has_value();});
    check(parkOrder!=released->orders.end(),
        "air-unloaded passenger receives retail PARK to clear the landing point");
    if (parkOrder!=released->orders.end()) {
        const auto& park=*parkOrder->park;
        check(park.permanent==0 && park.target==0 && park.padding>=16 && park.padding<=80,
            "last air passenger has a temporary, untargeted retail dispersion radius");
    }
    const auto dropX=released->x,dropZ=released->z;
    auto clearedDrop=[&] {
        return std::abs((w.unit(remote)->x-dropX).toFloat())>=foot.footX*16 ||
               std::abs((w.unit(remote)->z-dropZ).toFloat())>=foot.footZ*16;
    };
    for (int tick=0;tick<600 && !clearedDrop();++tick)
        w.tick(1.f/30);
    check(clearedDrop(),"air-unloaded passenger clears its entire original footprint using the existing navigator");
    w.stop(remote);
    check(w.unit(remote)->orders.empty(),"Stop cancels passenger dispersion normally");
}

static void exactLandingSites() {
    for (bool flying:{false,true}) {
        UnitType carrier=boatType(),passenger=footType(),blocker=footType();
        carrier.canFly=flying;carrier.cruiseAlt=100;
        passenger.accel={}; // observe the exact release point before PARK moves it
        auto terrain=[](World& w) {
            w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            w.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
        };
        {
            World w;setTestPathfinding(w);terrain(w);
            const int tid=w.spawn(&carrier,400,520),cid=w.spawn(&passenger,430,520);
            board(w,tid,cid);w.unloadAt(tid,512.25f,520.5f);
            check(runUntilUnloaded(w,tid),"exact-site unloading succeeds on a clear native placement plane");
            check(w.unit(cid)->x.toFloat()==512.25f && w.unit(cid)->z.toFloat()==520.5f,
                "unloading preserves the selected position without snapping or spiralling");
        }
        {
            World w;setTestPathfinding(w);terrain(w);UnitType building=blocker;building.maxVel={};
            const int tid=w.spawn(&carrier,400,520),cid=w.spawn(&passenger,430,520);
            board(w,tid,cid);w.spawn(&building,512.25f,520.5f);
            w.unloadAt(tid,512.25f,520.5f);
            if (!flying) {
                // GROUND_UNLOAD: DROP_UNIT refuses the occupied cell, the passenger
                // stays aboard, and after the third call (0x406a13) the lone order
                // calls again every 30 + rand(30) ticks (0x43b94f) -- it never ends.
                for (int tick=0;tick<600;++tick) w.tick(1.f/30);
                const auto& orders=w.unit(tid)->orders;
                check(w.unit(cid)->embarked() && !orders.empty() && orders.front().transportApproachAttempts>=3,
                    "a permanent landing obstruction keeps the unload calling with its cargo aboard");
            } else {
            for (int tick=0;tick<2;++tick) w.tick(1.f/30); // air controller starts before its placement check
            check(w.unit(cid)->embarked() && w.unit(tid)->orders.empty(),
                "a permanent landing obstruction ends the order and keeps cargo aboard");
            }
            w.unloadAt(tid,600.25f,600.5f);
            check(runUntilUnloaded(w,tid),
                "a fresh clear-site unload succeeds after permanent obstruction failure");
            // A surface hull still braking carries the boom's point with it
            // (retailHullPoint); an air drop lands on the point.
            const float slack=flying ? 0.0f : 32.0f;
            check(w.unit(cid)->inTransport==0 && std::abs(w.unit(cid)->x.toFloat()-600.25f)<=slack &&
                  std::abs(w.unit(cid)->z.toFloat()-600.5f)<=slack,
                "failed unload does not poison cargo attachment or the next exact destination");
        }
        {
            World w;setTestPathfinding(w);terrain(w);
            const int tid=w.spawn(&carrier,400,520),cid=w.spawn(&passenger,430,520);
            board(w,tid,cid);w.unloadAt(tid,512.25f,520.5f);
            for (int tick=0;tick<8;++tick) w.tick(1.f/30);
            const int bid=w.spawn(&blocker,512.25f,520.5f);
            if (!flying) {
                // GROUND_UNLOAD: DROP_UNIT tests the cell once, at release; a refusal
                // leaves the passenger aboard and the order calls TransportDrop again.
                bool refused=false;
                for (int tick=0;tick<20 && !refused;++tick) {
                    w.tick(1.f/30);
                    const auto& orders=w.unit(tid)->orders;
                    refused=!orders.empty() && orders.front().transportMission.stage<=1;
                }
                check(refused && w.unit(cid)->embarked(),
                    "an occupied landing cell refuses the drop and the passenger stays aboard");
                w.order(bid,700,520,false);
                const bool unloaded=runUntilUnloaded(w,tid);
                // The boom carries the point with a hull still coasting onto its
                // circle (retailHullPoint): near the point, not on it.
                check(unloaded && std::abs(w.unit(cid)->x.toFloat()-512.25f)<32 &&
                      std::abs(w.unit(cid)->z.toFloat()-520.5f)<32,
                    "a later call drops the passenger at the point once the blocker leaves");
            } else {
            w.tick(1.f/30);
            check(w.unit(cid)->embarked() && !w.unit(tid)->orders.empty() &&
                    w.unit(tid)->orders.front().transportMission.stage==3,
                "a unit entering the landing point interrupts transfer and enters retry");
            w.tick(1.f/30);
            check(w.unit(tid)->orders.front().transportTicks==0 &&
                    w.unit(tid)->orders.front().transportMission.stage==1,
                "retry resets transfer progress and waits before another attempt");
            w.order(bid,700,520,false);
            bool restarted=false;
            for (int tick=0;tick<600 && !restarted;++tick) {
                w.tick(1.f/30);
                const auto& orders=w.unit(tid)->orders;
                restarted=!orders.empty() && orders.front().transportTicks==1;
            }
            check(restarted,"transfer restarts at the same point after the blocker walks away");
            if (restarted) {
                for (int tick=0;tick<14;++tick) w.tick(1.f/30);
                check(w.unit(cid)->embarked(),"a retried transfer waits the entire effect interval");
                w.tick(1.f/30);
                check(!w.unit(cid)->embarked(),"the retried transfer releases its passenger normally");
            }
            }
        }
        {
            World w;setTestPathfinding(w);terrain(w);w.setPathService(true);
            const int tid=w.spawn(&carrier,400,520),cid=w.spawn(&passenger,430,520);
            board(w,tid,cid);w.unloadAt(tid,512.25f,520.5f);
            for (int tick=0;tick<8;++tick) w.tick(1.f/30);
            const int bid=w.spawn(&blocker,512.25f,520.5f);
            if (!flying) {
                for (int tick=0;tick<20;++tick) w.tick(1.f/30);
                check(w.unit(cid)->embarked() && !w.unit(tid)->orders.empty() &&
                        w.unit(tid)->orders.front().transportMission.stage<=1,
                    "a refused drop returns the unload to its call before cancellation");
            } else {
            w.tick(1.f/30);
            check(w.unit(cid)->embarked() && !w.unit(tid)->orders.empty() &&
                    w.unit(tid)->orders.front().transportMission.stage==3,
                "blocked unload reaches its retry stage before cancellation");
            }
            w.stop(tid);
            check(w.unit(cid)->inTransport==tid && w.unit(tid)->cargo.size()==1 &&
                    w.unit(tid)->orders.empty(),
                "Stop removes the blocked retry mission without releasing or losing cargo");
            w.order(bid,700,520,false);
            w.unloadAt(tid,440.25f,600.5f);
            check(runUntilUnloaded(w,tid),
                "a fresh unload succeeds after canceling a blocked retry");
            const float slack=flying ? 0.0f : 32.0f;
            check(w.unit(cid)->inTransport==0 && w.unit(tid)->cargo.empty() &&
                    std::abs(w.unit(cid)->x.toFloat()-440.25f)<=slack &&
                    std::abs(w.unit(cid)->z.toFloat()-600.5f)<=slack,
                "blocked retry state and old destination do not leak into the replacement unload");
        }
    }
    {
        World w;setTestPathfinding(w);w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        w.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
        auto carrier=boatType(),passenger=footType();carrier.canFly=true;carrier.cruiseAlt=100;
        const int tid=w.spawn(&carrier,512.25f,520.5f),cid=w.spawn(&passenger,430,520);
        board(w,tid,cid);w.unloadAt(tid,512.25f,520.5f);
        check(runUntilUnloaded(w,tid),"a landed air carrier takes off and unloads at its own starting position");
        check(w.unit(tid)->flightGroundMode==2 && w.unit(tid)->flightY.toFloat()>100,
            "air transfer advances the flight controller instead of freezing at ground level");
    }
}

static void passengerCancellationWait() {
    for(bool air:{false,true})for(bool wake:{false,true}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,1000,400);
        w.loadInto(cid,tid);w.tick(1.f/30);
        w.stop(tid);
        // An unrelated navigator event does not wake an air passenger's 0x89 mask.
        if(air)w.unit(cid)->missionEvents|=0x100;
        w.tick(1.f/30);
        check(!w.unit(cid)->orders.empty(),"stopping carrier preserves passenger until a subscribed wake");
        if(wake) {
            w.unit(cid)->missionEvents|=8;w.tick(1.f/30);
            check(w.unit(cid)->orders.empty(),"subscribed event wakes cancellation before passenger deadline");
        } else {
            for(int tick=3;tick<=30;++tick)w.tick(1.f/30);
            check(!w.unit(cid)->orders.empty(),"sleeping passenger retains request before tick-31 deadline");
            w.tick(1.f/30);
            check(w.unit(cid)->orders.empty(),"passenger discovers canceled carrier pickup at its deadline");
        }
    }
    // The reciprocal carrier has the same dispatcher gate: changing the
    // passenger's orders does not bypass a sleeping pickup mission.
    for(bool air:{false,true})for(bool wake:{false,true}) {
        World w;setTestPathfinding(w);w.setGameSeed(71);
        w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;carrier.cruiseAlt=100;
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,1000,400);
        w.loadInto(cid,tid);w.tick(1.f/30);w.tick(1.f/30);
        const auto deadline=w.unit(tid)->orders.front().transportMission.deadline;
        w.stop(cid);w.tick(1.f/30);
        check(!w.unit(tid)->orders.empty(),"sleeping carrier retains pickup after passenger Stop");
        if(wake) {
            w.unit(tid)->missionEvents|=8;w.tick(1.f/30);
            check(w.unit(tid)->orders.empty() && !(w.unit(tid)->missionEvents&8),
                  "carrier cancellation consumes its subscribed wake event");
        } else {
            for(uint32_t now=4;now<deadline;++now)w.tick(1.f/30);
            check(!w.unit(tid)->orders.empty(),"carrier retains canceled passenger until pickup deadline");
            w.tick(1.f/30);
            check(w.unit(tid)->orders.empty(),"carrier cancels invalid pickup on its deadline");
        }
    }
    for(bool air:{false,true}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;carrier.cruiseAlt=100;
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,1000,400);
        w.loadInto(cid,tid);w.tick(1.f/30);w.tick(1.f/30);
        w.unit(tid)->missionEvents|=8;w.tick(1.f/30);
        check(w.unit(tid)->orders.empty(),"pickup target-loss event aborts even with a valid passenger record");
    }

    // Reference invalidation on death posts event 8, unlike changing orders.
    for(bool air:{false,true})for(bool passengerDies:{false,true}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
        UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;carrier.cruiseAlt=100;
        const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,1000,400);
        w.loadInto(cid,tid);w.tick(1.f/30);w.tick(1.f/30);
        const int waiting=passengerDies ? tid : cid;
        check(!w.unit(waiting)->orders.empty(),"reciprocal pickup is waiting before target death");
        w.unit(passengerDies ? cid : tid)->deadFor=0;
        w.tick(1.f/30);
        check(w.unit(waiting)->orders.empty(),"target death wakes reciprocal pickup before its polling deadline");
    }

}

static void passengerMissionSchedule() {
    using namespace ta::sim;
    struct Host {
        RetailMissionState mission;
        uint32_t now=0,retries=0;
        bool active=true,surface=true;
        int approaches=0,detaches=0;
        bool enabled()const{return active;}
        RetailMissionState* head(){return active ? &mission : nullptr;}
        bool hasNext(const RetailMissionState&)const{return false;}
        int handle(RetailMissionState& m,uint32_t events) {
            return retailPassengerPickup(m,retries,now,events,surface,
                [&]{++approaches;},[&]{++detaches;});
        }
        uint32_t random(uint32_t){return 0;}
        void remove(RetailMissionState&){active=false;}
        void rotate(RetailMissionState&){active=false;}
        void clear(){active=false;}
        void idle(){}
    };
    for(bool surface:{false,true}) {
        Host h;h.surface=surface;uint32_t events=0;
        retailDispatchMissions(0,events,h);
        check(h.mission.stage==1 && h.mission.deadline==30 &&
            h.approaches==int(surface) && h.detaches==int(!surface),
            "passenger initialization installs surface goal or stops for air pickup");
        h.now=30;retailDispatchMissions(h.now,events,h);
        check(h.mission.stage==0 && h.mission.deadline==35,
            "passenger without carrier stop event retries approach after five ticks");
        h.now=35;retailDispatchMissions(h.now,events,h);
        check(h.mission.stage==1 && h.mission.deadline==65,
            "passenger retry reinstalls approach and thirty-tick wait");
        events=0x80;h.now=36;retailDispatchMissions(h.now,events,h);
        check(h.mission.stage==2 && h.retries==1 && h.mission.deadline==66 && events==0,
            "carrier stop event wakes passenger and begins bounded attachment wait");
        for(uint32_t tick=66;tick<=156;tick+=30) {
            h.now=tick;retailDispatchMissions(tick,events,h);
        }
        check(h.active && h.retries==5 && h.mission.deadline==186,
            "passenger remains available through five attachment waits");
        h.now=186;retailDispatchMissions(h.now,events,h);
        check(!h.active && h.retries==6,
            "passenger aborts when sixth attachment wait expires");
    }
}

static void airUnloadArrivalEvent() {
    World w;setTestPathfinding(w);w.setGameSeed(71);
    w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
    UnitType carrier=boatType(),passenger=footType();carrier.canFly=true;carrier.cruiseAlt=100;
    const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,430,400);
    w.loadInto(cid,tid);
    for(int tick=0;tick<120 && !w.unit(cid)->embarked();++tick)w.tick(1.f/30);
    check(w.unit(cid)->embarked(),"air unload arrival fixture boards passenger");
    w.unloadAt(tid,500,400);
    w.unit(tid)->x=ta::sim::Fixed::fromInt(100);w.tick(1.f/30);
    const auto deadline=w.unit(tid)->orders.front().transportMission.deadline;
    w.unit(tid)->x=ta::sim::Fixed::fromInt(400);w.tick(1.f/30);
    check((w.unit(tid)->orders.front().transportMission.pending&0x500)==0x500 &&
          !w.unit(tid)->orders.front().flightGoal,
          "air unload controller posts arrival and detaches before its polling deadline");
    w.tick(1.f/30);
    const auto& mission=w.unit(tid)->orders.front().transportMission;
    check(mission.stage==2 && !(mission.pending&0x100) && mission.deadline<deadline,
          "air unload consumes arrival and enters transfer before the old polling deadline");
    auto* flying=w.unit(tid);
    flying->orders.front().flightGoal.reset();
    flying->orders.front().transportMission.pending|=0x400;
    flying->flightNavigation.destination={flying->x.v+8*65536,flying->flightY.v,flying->z.v};
    flying->flightNavigation.velocity={};
    flying->flightVelocity={65536,0,0};flying->speed=ta::sim::Fixed::fromInt(1);
    const auto retained=flying->flightNavigation.destination;
    const auto oldX=flying->x,oldZ=flying->z;
    w.tick(1.f/30);
    check(!flying->orders.front().flightGoal &&
          flying->flightNavigation.destination.x==retained.x &&
          flying->flightNavigation.destination.y==retained.y &&
          flying->flightNavigation.destination.z==retained.z &&
          (flying->x!=oldX || flying->z!=oldZ),
          "detached transport keeps integrating motion from retained navigation without recreating a controller");
}

static void airUnloadControllerPendingEvents() {
    World w;setTestPathfinding(w);w.setGameSeed(71);
    w.setTerrain(std::vector<uint8_t>(96*96,100),96,96,20);
    UnitType carrier=boatType(),passenger=footType();carrier.canFly=true;carrier.cruiseAlt=100;
    const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,430,400);
    w.loadInto(cid,tid);
    for(int tick=0;tick<120 && !w.unit(cid)->embarked();++tick)w.tick(1.f/30);
    check(w.unit(cid)->embarked(),"air unload event fixture boards its passenger");
    w.unloadAt(tid,500,400);
    // Drift outside range after the command, exercising the unload handler's
    // own approach rather than the separate command-side movement prefix.
    w.unit(tid)->x=ta::sim::Fixed::fromInt(100);w.tick(1.f/30);
    for(bool inside:{false,true}) {
        auto& mission=w.unit(tid)->orders.front().transportMission;
        mission.deadline=0;mission.pending=0x403700;
        if(inside)w.unit(tid)->x=ta::sim::Fixed::fromInt(450);
        w.tick(1.f/30);
        const auto& after=w.unit(tid)->orders.front().transportMission;
        check((after.pending&0x403700)==0x400000,
              inside ? "air unload departure controller clears stale movement events" :
                       "air unload approach controller clears stale movement events");
        check(after.stage==(inside?2:1),"air unload event fixture reaches the intended controller stage");
    }
}

static void airUnloadWorldTimeline() {
    using ta::sim::Fixed;
    World w;setTestPathfinding(w);w.setGameSeed(71);
    w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    UnitType carrier=boatType(),passenger=footType();
    carrier.canFly=true;carrier.cruiseAlt=100;carrier.transportDist=150;
    const int tid=w.spawn(&carrier,3000,3000),cid=w.spawn(&passenger,3000,3000);
    board(w,tid,cid);
    w.unloadAt(tid,3000,3000,Fixed::fromInt(100));
    for(uint32_t tick=1;tick<=24 && w.unit(cid)->inTransport;++tick) {
        if(tick==2) {
            const auto& orders=w.unit(tid)->orders;
            if(!orders.empty() && orders.front().flightGoal) {
                const auto goal=orders.front().flightGoal->point;
                auto* unit=w.unit(tid);
                // The paired native probe advances only the navigator's current
                // point to the controller target; mirror that controlled arrival.
                unit->x=Fixed::raw(goal.x);unit->z=Fixed::raw(goal.z);
                unit->flightY=Fixed::raw(goal.y);unit->flightVelocity={};
                unit->flightNavigation.velocity={};
                unit->flightNavigation.destination=goal;
            }
        }
        w.tick(1.f/30);
        const auto& unit=*w.unit(tid);
        const auto* order=unit.orders.empty()?nullptr:&unit.orders.front();
        const ta::sim::RetailMissionState emptyMission{};
        const auto& mission=order ? order->transportMission : emptyMission;
        const auto* goal=order && order->flightGoal ? &*order->flightGoal : nullptr;
        const auto& passengerUnit=*w.unit(cid);
        const bool parked=!passengerUnit.orders.empty() && passengerUnit.orders.front().park.has_value();
        std::printf("%u %u %u %u %u %u %u %d %d %d %u %u %u %u %u %u\n",tick,
            order?unsigned(mission.stage):0,order?mission.waitMask:0,
            order?mission.deadline:0,order?mission.pending:0,
            order?unsigned(order->transportTicks):0,unsigned(goal!=nullptr),
            goal?goal->point.x:0,goal?goal->point.y:0,goal?goal->point.z:0,
            goal?unsigned(goal->flags):0,goal?unsigned(uint16_t(goal->radius)):0,
            unsigned(unit.cargo.size()),unsigned(passengerUnit.embarked()),
            unsigned(w.transportEffects().empty()?0:2),unsigned(parked));
    }
}

static void surfaceUnloadWorldTimeline() {
    using ta::sim::Fixed;
    World w;setTestPathfinding(w);w.setPathService(false);w.setGameSeed(71);
    w.setTerrain(coastHeights(64,64,25),64,64,40);
    UnitType carrier=boatType(),passenger=footType();
    carrier.domain=UnitType::Domain::Water;carrier.floater=true;
    carrier.minWaterDepth=13;carrier.maxWaterDepth=10000;
    const int tid=w.spawn(&carrier,300,500),cid=w.spawn(&passenger,500,500);
    board(w,tid,cid);
    w.unloadAt(tid,500,500);
    for(uint32_t tick=1;tick<=24;++tick) {
        if(tick==2) {
            auto* unit=w.unit(tid);
            unit->x=Fixed::fromInt(450);unit->z=Fixed::fromInt(500);
            auto& approach=unit->orders.front();
            approach.transportMission.pending|=0x100;
            approach.controller=0;approach.navigationExhausted=true;
        }
        w.tick(1.f/30);
        const auto& unit=*w.unit(tid);
        const auto* order=unit.orders.empty()?nullptr:&unit.orders.front();
        const ta::sim::RetailMissionState emptyMission{};
        const auto& mission=order ? order->transportMission : emptyMission;
        const auto center=order && order->missionTarget ? *order->missionTarget :
            std::pair<Fixed,Fixed>{};
        const bool controller=order && order->controller;
        const int goalX=controller ? int(center.first.floorInt()/16) : 0;
        const int goalZ=controller ? int(center.second.floorInt()/16) : 0;
        const int radius=controller ? int(order->missionRadius+4) : 0;
        const auto& passengerUnit=*w.unit(cid);
        const bool parked=!passengerUnit.orders.empty() && passengerUnit.orders.front().park.has_value();
        std::printf("%u %u %u %u %u %u %u %d %d %d %u %u %u %u %u\n",tick,
            unsigned(order!=nullptr),order?unsigned(mission.stage):0,order?mission.waitMask:0,
            order?mission.deadline:0,order?mission.pending:0,
            order?unsigned(order->transportTicks):0,unsigned(controller),goalX,goalZ,
            unsigned(radius),unsigned(!unit.cargo.empty()),unsigned(passengerUnit.embarked()),
            unsigned(w.transportEffects().empty()?0:2),unsigned(parked));
        if(!order && !unit.cargo.size())break;
    }
}

static void pickupControllerPendingEvents() {
    for(bool air:{false,true}) {
        World w;setTestPathfinding(w);
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType carrier=boatType(),passenger=footType();
        carrier.canFly=air;carrier.cruiseAlt=100;
        const int tid=w.spawn(&carrier,100,100),cid=w.spawn(&passenger,800,800);
        w.loadInto(cid,tid);w.tick(1.f/30);
        auto& mission=w.unit(tid)->orders.front().transportMission;
        mission.pending=0x403700;
        w.tick(1.f/30);
        check((w.unit(tid)->orders.front().transportMission.pending&0x403700)==0x400000,
              air ? "air pickup controller installation clears only stale movement events" :
                    "surface pickup controller installation clears only stale movement events");
        if(air) {
            w.unit(tid)->orders.front().transportMission.pending|=0x3000;
            w.tick(1.f/30);
            check((w.unit(tid)->orders.front().transportMission.pending&0x403000)==0x403000,
                  "air pursuit position refresh preserves pending events between mission polls");
            w.unit(tid)->x=w.unit(cid)->x-ta::sim::Fixed::fromInt(100);
            w.unit(tid)->z=w.unit(cid)->z;
            auto& approach=w.unit(tid)->orders.front().transportMission;
            approach.pending=0x403700;approach.deadline=0;
            w.tick(1.f/30);
            const auto& transfer=w.unit(tid)->orders.front().transportMission;
            check(transfer.stage==2 && (transfer.pending&0x403700)==0x400000,
                  "air pickup departure controller clears stale movement events");
        }
    }
}

// Small deterministic comparison fixture for the native sea-unload route probe.
// It emits the production World's water-domain grades and its first routed leg;
// the Python probe feeds that same grade plane to retail's original search worker.
static void surfaceUnloadRouteFixture(int variant) {
    const int W=variant==4 ? 128 : 64,H=W,sea=40;
    std::vector<uint8_t> source(size_t(W)*H,100);
    auto water=[&](int x0,int z0,int x1,int z1) {
        for(int z=z0;z<z1;++z) for(int x=x0;x<x1;++x)
            source[size_t(z)*W+x]=10;
    };
    if(variant==4) {
        water(4,4,101,13);
        water(96,8,111,112);
    } else if(variant==7) {
        water(4,4,21,21);
        water(40,40,57,57);
    } else {
        water(4,4,37,13);
        water(32,8,47,47);
    }
    // Keep the exact passenger drop cell dry while leaving enough open water
    // around it for the boat's unload-range circle and footprint.
    const int unloadCell=variant==4 ? 104 : variant==8 ? 31 : 40;
    for(int z=unloadCell;z<=unloadCell+1;++z)
        for(int x=unloadCell;x<=unloadCell+1;++x)
            source[size_t(z)*W+x]=100;
    auto cell=[&](int x,int z) {
        switch(variant) {
        case 1:return std::pair{W-1-x,z};
        case 2:return std::pair{x,H-1-z};
        case 3:return std::pair{W-1-z,x};
        default:return std::pair{x,z};
        }
    };
    auto pixel=[&](int x,int z) {
        switch(variant) {
        case 1:return std::pair{(W-1)*16-x,z};
        case 2:return std::pair{x,(H-1)*16-z};
        case 3:return std::pair{(W-1)*16-z,x};
        default:return std::pair{x,z};
        }
    };
    std::vector<uint8_t> heights(size_t(W)*H,100);
    for(int z=0;z<H;++z) for(int x=0;x<W;++x) {
        const auto [tx,tz]=cell(x,z);
        heights[size_t(tz)*W+tx]=source[size_t(z)*W+x];
    }

    World w;setTestPathfinding(w);w.setTerrain(heights,W,H,sea);w.setPathService(true);
    ta::sim::RetailReplayProbe::dumpSurfaceSearchGrades(w);
    UnitType carrier=boatType(),passenger=footType();
    carrier.domain=UnitType::Domain::Water;carrier.floater=true;
    carrier.minWaterDepth=13;carrier.maxWaterDepth=10000;
    const int footprint=variant==5 ? 3 : variant==6 ? 2 : 4;
    carrier.footX=carrier.footZ=int16_t(footprint);carrier.transportDist=150;
    const int requestedTarget=variant==4 ? 1664 : variant==8 ? 500 : 640;
    const auto [startX,startZ]=pixel(variant==8 ? 400 : 160,160);
    const auto [goalX,goalZ]=pixel(requestedTarget,requestedTarget);
    const int tid=w.spawn(&carrier,startX,startZ),cid=w.spawn(&passenger,startX,startZ);
    board(w,tid,cid);w.unloadAt(tid,float(goalX),float(goalZ));

    std::printf("PLANE %d %d %d %d\n",W,H,tid,sea);
    for(int z=0;z<H;++z) for(int x=0;x<W;++x)
        std::printf("%d %d %d\n",x,z,w.cellScore(&carrier,x,z,tid));

    for(uint32_t tick=1;tick<=5000;++tick) {
        w.tick(1.f/30);
        const auto* u=w.unit(tid);
        if(!u || u->orders.empty()) break;
        if(std::getenv("TA_ROUTE_TRACE") && (tick<=2 || (tick>=14 && tick<=15))) {
            const auto& o=u->orders.front();
            std::printf("TRACE %u pos=%d,%d heading=%d orders=%zu front=%d,%d segment=%d,%d has=%d mission=%d radius=%u target=%d,%d consumed=%d exhausted=%d routeStamp=%d pending=%d completed=%llu failed=%llu\n",
                tick,u->x.v,u->z.v,u->heading.v,u->orders.size(),o.x.v,o.z.v,o.segmentX.v,o.segmentZ.v,
                int(o.hasSegment),int(o.transportUnloadApproach),o.missionRadius,
                o.missionTarget ? o.missionTarget->first.v : 0,o.missionTarget ? o.missionTarget->second.v : 0,
                int(o.navigationConsumed),int(o.navigationExhausted),u->routeStamp,
                int(w.pathStats().pending(tid)),static_cast<unsigned long long>(w.pathStats().completions()),
                static_cast<unsigned long long>(w.pathStats().failures()));
        }
        // The mission dispatch installs a direct approach segment before the
        // asynchronous path worker runs.  Do not mistake that initial segment
        // for the delivered route: this boundary is the first completed search.
        if(w.pathStats().completions()+w.pathStats().failures()==0) continue;
        const auto& orders=u->orders;
        size_t end=0;
        while(end<orders.size() && !orders[end].unload) ++end;
        std::printf("ROUTE %u %d %d %d %d %zu %llu %llu\n",tick,u->x.v,u->z.v,
            orders.front().segmentX.v,orders.front().segmentZ.v,end,
            static_cast<unsigned long long>(w.pathStats().completions()),
            static_cast<unsigned long long>(w.pathStats().failures()));
        for(size_t i=0;i<end;++i)
            std::printf("%d %d\n",orders[i].x.v,orders[i].z.v);
        if(std::getenv("TA_SURFACE_STEP")) {
            auto* stepped=w.unit(tid);
            stepped->x=orders.front().segmentX;stepped->z=orders.front().segmentZ;
            stepped->speed=ta::sim::Fixed();
            stepped->heading=ta::sim::retailHeadingToPort(
                ta::sim::portHeadingToRetail(stepped->heading));
            stepped->turnReqBam=0;stepped->groundMoveTick=0;stepped->groundScanTick=tick+1000;
            stepped->groundMovementMode=0;stepped->groundSpeedMode=0;stepped->groundTerrainFlags=0x1000;
            const size_t activeGoal=World::currentLeg(stepped->orders);
            // Align a native approach poll with the routed unload boundary so
            // this trace covers retry re-anchoring, circle arrival and transfer.
            stepped->orders[activeGoal].transportMission.deadline=tick+474;
            w.setPathService(false);
            std::printf("WORLDSEED %d %d %d %d %d %d %d\n",stepped->x.v,stepped->groundY.v,
                stepped->z.v,int(ta::sim::portHeadingToRetail(stepped->heading)),
                stepped->speed.v,stepped->baseSpeed.v,stepped->groundTerrainFlags);
            const auto& initialMission=stepped->orders[activeGoal].transportMission;
            std::printf("WORLDMISSION %u %u %u %u %u %u %u %u %d %d\n",
                unsigned(initialMission.stage),initialMission.waitMask,initialMission.deadline,
                initialMission.pending,initialMission.flags,stepped->orders[activeGoal].transportApproachAttempts,
                unsigned(stepped->cargo.size()),unsigned(stepped->cargo.empty()?0:stepped->cargo.front()),
                stepped->orders[activeGoal].missionTarget ? stepped->orders[activeGoal].missionTarget->first.v : 0,
                stepped->orders[activeGoal].missionTarget ? stepped->orders[activeGoal].missionTarget->second.v : 0);
            // Cover the retry, unload transfer, and initial post-transfer coast.
            unsigned stepLimit=32;
            if(const char* text=std::getenv("TA_SURFACE_STEPS"))
                stepLimit=unsigned(std::clamp(std::atoi(text),1,1000));
            for(unsigned step=1;step<=stepLimit;++step) {
                w.tick(1.f/30);
                const auto* after=w.unit(tid);
                std::printf("WORLDSTEP %u %d %d %d %d %d %u %u %u %d\n",step,
                    after->x.v,after->groundY.v,after->z.v,
                    int(ta::sim::portHeadingToRetail(after->heading)),after->speed.v,
                    unsigned(after->groundMovementMode),unsigned(after->groundSpeedMode),
                    unsigned(after->groundTerrainFlags),after->turnReqBam);
                const auto& orders=after->orders;
                size_t pathCount=0;
                while(pathCount<orders.size() &&
                      (!orders[pathCount].unload || orders[pathCount].transportUnloadApproach))
                    ++pathCount;
                const size_t goalIndex=orders.empty()?0:World::currentLeg(orders);
                const auto* current=pathCount ? &orders.front() : nullptr;
                const auto* goal=goalIndex<orders.size() ? &orders[goalIndex] : nullptr;
                std::printf("WORLDNAV %u %zu %u %d %u %u %u %u %d %d %d %d %d %d %u %u %u %u\n",
                    step,pathCount,after->missionEvents,after->routeStamp,
                    unsigned(goal && goal->controller),goal?goal->transportMission.pending:0,
                    goal?goal->transportMission.waitMask:0,goal?unsigned(goal->transportMission.stage):0,
                    int(current?current->x.floorInt():0),int(current?current->z.floorInt():0),
                    int(current?current->segmentX.floorInt():0),int(current?current->segmentZ.floorInt():0),
                    int(goal?int(goal->transportUnloadApproach):0),int(goal?int(goal->goal):0),
                    unsigned(current?current->hasSegment:0),unsigned(after->orders.size()),
                    unsigned(current?current->navigationConsumed:0),
                    unsigned(current?current->navigationExhausted:0));
                const size_t missionIndex=orders.empty()?0:World::currentLeg(orders);
                const auto* missionOrder=orders.empty()?nullptr:&orders[missionIndex];
                const auto* passengerUnit=w.unit(cid);
                const auto& mission=missionOrder ? missionOrder->transportMission : ta::sim::RetailMissionState{};
                std::printf("WORLDTRANS %u %u %u %u %u %u %u %u %u %u %u %u\n",step,
                    unsigned(orders.size()),unsigned(after->cargo.size()),unsigned(mission.stage),
                    mission.waitMask,mission.deadline,mission.pending,mission.flags,
                    unsigned(missionOrder?missionOrder->transportApproachAttempts:0),
                    unsigned(missionOrder?missionOrder->transportTicks:0),
                    unsigned(passengerUnit && passengerUnit->embarked()),
                    unsigned(w.transportEffects().size()));
                for(size_t i=0;i<pathCount;++i)
                    std::printf("WORLDPOINT %u %zu %d %d\n",step,i,
                        orders[i].x.floorInt(),orders[i].z.floorInt());
            }
        }
        return;
    }
    std::printf("NO_ROUTE\n");
}

// Route fixture backed by an installed map instead of a hand-made coastline.  The
// same production map parser, terrain grid, feature placement plane and boat-domain
// cache used in a match feed World; the native oracle later consumes the effective
// search grades exported at the real unload search boundary.
static void surfaceUnloadMapRouteFixture(const char* retailRoot,const char* mapName,
                                         int startCellX,int startCellZ,
                                         int goalCellX,int goalCellZ,int footprint,
                                         const char* carrierName=nullptr,
                                         const char* passengerName=nullptr) {
    auto vfs=ta::hpi::mountRetailRoot(retailRoot);
    const std::string mapPath=ta::hpi::findMap(vfs,mapName);
    if(mapPath.empty()) throw std::runtime_error(std::string("map not found: ")+mapName);
    const auto map=ta::tnt::Map::load(vfs.read(mapPath),mapPath);
    if((!carrierName && (footprint<1 || footprint>16)) || startCellX<0 || startCellZ<0 ||
       goalCellX<0 || goalCellZ<0 || startCellX>=map.width || startCellZ>=map.height ||
       goalCellX>=map.width || goalCellZ>=map.height)
        throw std::runtime_error("surface-unload map route coordinates outside the map");

    std::unique_ptr<ta::sim::TypeRegistry> registry;
    UnitType syntheticCarrier{},syntheticPassenger{};
    const UnitType* carrierType=nullptr;
    const UnitType* passengerType=nullptr;
    if(carrierName || passengerName) {
        if(!carrierName || !passengerName)
            throw std::runtime_error("both retail carrier and passenger names are required");
        registry=std::make_unique<ta::sim::TypeRegistry>();
        ta::sim::setupRegistry(*registry,vfs);
        carrierType=registry->find(carrierName);
        passengerType=registry->find(passengerName);
        if(!carrierType || !passengerType)
            throw std::runtime_error("retail carrier or passenger unit type not found");
        // Sensitivity fixture for the cargo sight check: a passenger that sees
        // farther than its carrier reveals cells only its own sight reaches.
        if(const char* sight=std::getenv("TA_MAP_SURFACE_CARGO_SIGHT");sight && *sight)
            const_cast<UnitType*>(passengerType)->sight=std::atoi(sight);
        if(!carrierType->canTransport || carrierType->domain!=UnitType::Domain::Water ||
           !passengerType->canMove || passengerType->domain==UnitType::Domain::Water)
            throw std::runtime_error("retail unit profiles are not a surface carrier and land passenger");
    } else {
        syntheticCarrier=boatType();syntheticPassenger=footType();
        syntheticCarrier.domain=UnitType::Domain::Water;syntheticCarrier.floater=true;
        syntheticCarrier.minWaterDepth=13;syntheticCarrier.maxWaterDepth=10000;
        syntheticCarrier.footX=syntheticCarrier.footZ=int16_t(footprint);
        carrierType=&syntheticCarrier;passengerType=&syntheticPassenger;
    }

    World w;setTestPathfinding(w);
    w.setTerrain(map.heights,map.width,map.height,map.seaLevel,&map.features);
    ta::sim::registerMapFeatures(w,map,vfs);
    if(registry) w.buildNavClasses(*registry);
    w.setPathService(true);
    const int fx=carrierType->footX,fz=carrierType->footZ;
    if(fx<1 || fz<1 || fx>16 || fz>16)
        throw std::runtime_error("surface carrier footprint is outside supported range");
    const int startX=startCellX*16,startZ=startCellZ*16;
    const int goalX=goalCellX*16+8,goalZ=goalCellZ*16+8;
    const int tid=w.spawn(carrierType,startX,startZ),cid=w.spawn(passengerType,startX,startZ);
    if(carrierName) {
        std::fprintf(stderr,"TRANSPORTPROFILE %d %d %d %d\n",carrierType->transportDist,
                     carrierType->maxWaterDepth,carrierType->minWaterDepth,
                     int(carrierType->floater && carrierType->minWaterDepth>0));
    }
    board(w,tid,cid);w.unloadAt(tid,float(goalX),float(goalZ));
    ta::sim::RetailReplayProbe::dumpSurfaceSearchGrades(w);
    std::printf("MAPROUTE %d %d %d %d %d %d %d %d %d\n",map.width,map.height,
        map.seaLevel,fx,startX,startZ,goalX,goalZ,tid);

    for(uint32_t tick=1;tick<=20000;++tick) {
        w.tick(1.f/30);
        const auto* u=w.unit(tid);
        if(!u || u->orders.empty()) break;
        if(w.pathStats().completions()+w.pathStats().failures()==0) continue;
        const auto& orders=u->orders;
        if(std::getenv("TA_DUMP_ROUTE_ATTEMPT"))
            ta::sim::RetailReplayProbe::dumpCompletedPathAttempt(w,tid);
        size_t end=0;
        while(end<orders.size() && !orders[end].unload) ++end;
        std::printf("ROUTE %u %d %d %d %d %zu %llu %llu\n",tick,u->x.v,u->z.v,
            orders.front().segmentX.v,orders.front().segmentZ.v,end+1,
            static_cast<unsigned long long>(w.pathStats().completions()),
            static_cast<unsigned long long>(w.pathStats().failures()));
        for(size_t i=0;i<end;++i)
            std::printf("%d %d\n",orders[i].x.v,orders[i].z.v);
        if(const char* text=std::getenv("TA_MAP_SURFACE_STEPS")) {
            const unsigned stepLimit=unsigned(std::clamp(std::atoi(text),1,10000));
            auto* stepped=w.unit(tid);
            stepped->x=orders.front().segmentX;stepped->z=orders.front().segmentZ;
            stepped->speed=ta::sim::Fixed();
            stepped->heading=ta::sim::retailHeadingToPort(
                ta::sim::portHeadingToRetail(stepped->heading));
            stepped->turnReqBam=0;stepped->groundMoveTick=0;
            unsigned scanAfter=stepLimit+1000;
            if(const char* scan=std::getenv("TA_MAP_SURFACE_SCAN_AFTER"))
                scanAfter=unsigned(std::clamp(std::atoi(scan),0,2500));
            stepped->groundScanTick=tick+scanAfter;
            stepped->groundMovementMode=0;stepped->groundSpeedMode=0;
            stepped->groundTerrainFlags=0x1000;
            const size_t activeGoal=World::currentLeg(stepped->orders);
            stepped->orders[activeGoal].transportMission.deadline=tick+stepLimit+1000;
            int shoreBlocker=0;
            bool shoreBlockerCleared=false;
            int routeBlocker=0;
            unsigned routeCompletions=w.pathStats().completions();
            bool routeBlockerCleared=false;
            bool routeSearchEnabled=false;
            const char* routeAlwaysEnabled=std::getenv("TA_MAP_SURFACE_ROUTE_ALWAYS_ON");
            const bool alwaysEnabledRouteSearch=routeAlwaysEnabled && *routeAlwaysEnabled &&
                *routeAlwaysEnabled!='0';
            const char* routeBlockerEnabled=std::getenv("TA_MAP_SURFACE_ROUTE_BLOCKER");
            const bool liveRouteBlocker=routeBlockerEnabled && *routeBlockerEnabled &&
                *routeBlockerEnabled!='0';
            std::vector<std::pair<int,int>> initialWaypoints;
            size_t initialEnd=0;
            while(initialEnd<stepped->orders.size() && !stepped->orders[initialEnd].unload)
                ++initialEnd;
            for(size_t i=0;i<initialEnd;++i)
                initialWaypoints.emplace_back(stepped->orders[i].x.v,stepped->orders[i].z.v);
            if(liveRouteBlocker) {
                const auto target=stepped->orders[activeGoal].missionTarget.value_or(
                    std::pair{stepped->orders[activeGoal].x,stepped->orders[activeGoal].z});
                const float bx=(stepped->x.toFloat()+target.first.toFloat())*0.5f;
                const float bz=(stepped->z.toFloat()+target.second.toFloat())*0.5f;
                routeBlocker=w.spawn(carrierType,bx,bz);
                std::printf("WORLD_ROUTE_BLOCKER %d %d %d %d %d %d\n",routeBlocker,
                    w.unit(routeBlocker)->x.v,w.unit(routeBlocker)->z.v,
                    int(initialWaypoints.size()),carrierType->footX,carrierType->footZ);
            }
            if(const char* enabled=std::getenv("TA_MAP_SURFACE_BLOCK_SHORE")) {
                if(*enabled && *enabled!='0') {
                    if(!passengerType || !passengerType->canMove || passengerType->canFly)
                        throw std::runtime_error("shore blocker requires a ground passenger profile");
                    shoreBlocker=w.spawn(passengerType,float(goalX),float(goalZ));
                    const auto* body=w.unit(shoreBlocker);
                    std::printf("WORLD_BLOCKER 0 %d %d %d %d 0 %zu 0\n",body->x.v,
                        body->z.v,body->speed.v,
                        int(ta::sim::portHeadingToRetail(body->heading)),
                        w.unit(tid)->cargo.size());
                }
            }
            stepped=w.unit(tid); // spawning the blocker may reallocate World's unit vector
            if(!alwaysEnabledRouteSearch) w.setPathService(false);
            std::printf("WORLDSEED %d %d %d %d %d %d %d\n",stepped->x.v,
                stepped->groundY.v,stepped->z.v,
                int(ta::sim::portHeadingToRetail(stepped->heading)),
                stepped->speed.v,stepped->baseSpeed.v,stepped->groundTerrainFlags);
            std::printf("WORLDTYPE %d %d %d %d %d\n",stepped->type->maxVel.v,
                stepped->type->accel.v,stepped->type->brake.v,stepped->type->turnRate,
                stepped->type->waterline);
            std::printf("WORLDSCAN %u %u %u\n",w.tickCount(),stepped->groundScanTick,
                unsigned(stepped->type->halfCellTicks));
            std::vector<uint16_t> previousExploration;
            const int explorationWidth=map.width/2;
            const int explorationHeight=map.height/2;
            // Exploration deltas for native sight-producer comparisons; the
            // route-blocker trace always needs them.
            const char* explorationTrace=std::getenv("TA_MAP_SURFACE_EXPLORATION");
            const bool traceExploration=liveRouteBlocker ||
                (explorationTrace && *explorationTrace && *explorationTrace!='0');
            if(traceExploration)
                std::printf("WORLDSIGHT %d %u %d %d %d %d %d %d\n",w.sightDistance(*stepped->type),
                    unsigned(stepped->type->sightHeight),int(stepped->player),
                    int(stepped->cargo.size()),int(stepped->sightFootprint.x),
                    int(stepped->sightFootprint.z),int(stepped->sightFootprint.eyeHeight),
                    int(stepped->sightFootprint.active));
            // 4f6a60 also runs each attached passenger's sight from its attach
            // point; publish the first passenger's cached footprint too.
            if(traceExploration && !stepped->cargo.empty())
                if(const auto* cargo=w.unit(stepped->cargo.front()))
                    std::printf("WORLDCARGOSIGHT %d %u %d %d %d %d %d %d\n",w.sightDistance(*cargo->type),
                        unsigned(cargo->type->sightHeight),cargo->groundY.v,int(cargo->sightFootprint.x),
                        int(cargo->sightFootprint.z),int(cargo->sightFootprint.eyeHeight),
                        int(cargo->sightFootprint.active),int(cargo->type->waterline));
            if(traceExploration) {
                const auto& exploration=w.navigationExploration();
                if(exploration.size()!=size_t(explorationWidth)*size_t(explorationHeight))
                    throw std::runtime_error("surface route trace exploration dimensions do not match map");
                previousExploration=exploration;
                for(size_t i=0;i<previousExploration.size();++i) if(previousExploration[i])
                    std::printf("WORLDEXP_INIT %d %d %u\n",int(i%size_t(explorationWidth)),
                        int(i/size_t(explorationWidth)),unsigned(previousExploration[i]));
            }
            bool routeReleaseReported=false;
            bool routeAttemptReported=false;
            for(unsigned step=1;step<=stepLimit;++step) {
                w.tick(1.f/30);
                if(traceExploration) {
                    const auto& exploration=w.navigationExploration();
                    for(size_t i=0;i<exploration.size();++i)
                        if(exploration[i]!=previousExploration[i]) {
                            std::printf("WORLDEXP_DELTA %u %d %d %u\n",step,
                                int(i%size_t(explorationWidth)),
                                int(i/size_t(explorationWidth)),unsigned(exploration[i]));
                            previousExploration[i]=exploration[i];
                        }
                }
                const auto* after=w.unit(tid);
                if(routeBlocker && !routeReleaseReported && w.unit(tid)->cargo.empty()) {
                    const int64_t dx=int64_t(after->x.v)-goalX*65536ll;
                    const int64_t dz=int64_t(after->z.v)-goalZ*65536ll;
                    const int64_t radius=int64_t(carrierType->transportDist-34)*65536ll;
                    const bool inUnloadCircle=dx*dx+dz*dz<=radius*radius;
                    std::printf("WORLD_ROUTE_RELEASE %u %d %d %d %d %d %d %zu %d\n",step,
                        after->x.v,after->z.v,w.unit(cid)->x.v,w.unit(cid)->z.v,
                        int(ta::sim::RetailReplayProbe::waterFootprintPassable(w,after)),
                        int(inUnloadCircle),after->cargo.size(),
                        int(w.unit(cid)->inTransport==tid));
                    routeReleaseReported=true;
                }
                if(routeBlocker && !routeSearchEnabled && after->bodyBlockStreak>=2) {
                    if(!alwaysEnabledRouteSearch) w.setPathService(true);
                    routeSearchEnabled=true;
                    std::printf(alwaysEnabledRouteSearch ?
                        "WORLD_ROUTE_BLOCK_THRESHOLD %u %d %u\n" :
                        "WORLD_ROUTE_SEARCH_ENABLED %u %d %u\n",step,
                        after->bodyBlockStreak,after->groundScanTick);
                }
                if(routeBlocker && w.pathStats().completions()>routeCompletions) {
                    routeCompletions=w.pathStats().completions();
                    const auto& orders=after->orders;
                    size_t pathEnd=0;
                    while(pathEnd<orders.size() && !orders[pathEnd].unload) ++pathEnd;
                    std::vector<std::pair<int,int>> points;
                    for(size_t i=0;i<pathEnd;++i)
                        points.emplace_back(orders[i].x.v,orders[i].z.v);
                    const bool changed=points!=initialWaypoints;
                    std::printf("WORLD_REPATH %u %llu %llu %d %d %d %zu %d %zu %d\n",step,
                        static_cast<unsigned long long>(w.pathStats().completions()),
                        static_cast<unsigned long long>(w.pathStats().failures()),
                        int(changed),after->x.v,after->z.v,pathEnd,
                        int(after->bodyBlockStreak),after->cargo.size(),
                        int(w.unit(cid)->inTransport==tid));
                    if(changed && !routeAttemptReported &&
                       std::getenv("TA_DUMP_ROUTE_ATTEMPT")) {
                        ta::sim::RetailReplayProbe::dumpCompletedPathAttempt(w,tid);
                        routeAttemptReported=true;
                    }
                    if(!routeBlockerCleared && changed) {
                        w.order(routeBlocker,float(goalX+256),float(goalZ+256),false);
                        routeBlockerCleared=true;
                    }
                }
                if(routeBlocker) {
                    const auto* body=w.unit(routeBlocker);
                    std::printf("WORLD_ROUTE_BODY %u %d %d %d %d %u\n",step,
                        body->x.v,body->z.v,body->speed.v,
                        int(ta::sim::portHeadingToRetail(body->heading)),
                        unsigned(routeBlockerCleared));
                }
                std::printf("WORLDSTEP %u %d %d %d %d %d %u %u %u %d %u\n",step,
                    after->x.v,after->groundY.v,after->z.v,
                    int(ta::sim::portHeadingToRetail(after->heading)),after->speed.v,
                    unsigned(after->groundMovementMode),unsigned(after->groundSpeedMode),
                    unsigned(after->groundTerrainFlags),after->turnReqBam,after->groundScanTick);
                if(traceExploration)
                    if(const auto* cargo=w.unit(cid);cargo && cargo->inTransport==tid)
                        std::printf("WORLDCARGO %u %d %d %d %d\n",step,cargo->x.v,cargo->groundY.v,
                            cargo->z.v,int(cargo<w.unit(tid)));
                if(shoreBlocker) {
                    const auto& orders=w.unit(tid)->orders;
                    auto mission=std::find_if(orders.begin(),orders.end(),
                        [](const auto& order){return order.transportUnloadApproach;});
                    const unsigned stage=mission==orders.end()?255u:
                        unsigned(mission->transportMission.stage);
                    const size_t cargo=w.unit(tid)->cargo.size();
                    if(!shoreBlockerCleared && stage==3 && cargo) {
                        w.order(shoreBlocker,goalX+256.f,float(goalZ),false);
                        shoreBlockerCleared=true;
                    }
                    const auto* body=w.unit(shoreBlocker);
                    std::printf("WORLD_BLOCKER %u %d %d %d %d %u %zu %u\n",step,
                        body->x.v,body->z.v,body->speed.v,
                        int(ta::sim::portHeadingToRetail(body->heading)),stage,cargo,
                        unsigned(shoreBlockerCleared));
                }
            }
        }
        return;
    }
    std::printf("NO_ROUTE\n");
}

// Asset-backed end-to-end surface unload on a shipped map. Unlike the route
// fixture above, this keeps the World and carrier alive through physical
// movement, placement checking, transfer, and passenger release.
static bool surfaceUnloadMapTravelFixture(const char* retailRoot,const char* mapName,
                                          int startCellX,int startCellZ,
                                          int goalCellX,int goalCellZ,
                                          const char* carrierName=nullptr,
                                          const char* passengerName=nullptr,
                                          bool loadOrder=false) {
    auto vfs=ta::hpi::mountRetailRoot(retailRoot);
    const std::string mapPath=ta::hpi::findMap(vfs,mapName);
    if(mapPath.empty()) throw std::runtime_error(std::string("map not found: ")+mapName);
    const auto map=ta::tnt::Map::load(vfs.read(mapPath),mapPath);
    if(startCellX<0 || startCellZ<0 || goalCellX<0 || goalCellZ<0 ||
       startCellX>=map.width || startCellZ>=map.height ||
       goalCellX>=map.width || goalCellZ>=map.height)
        throw std::runtime_error("surface-unload map travel coordinates outside the map");

    std::unique_ptr<ta::sim::TypeRegistry> registry;
    UnitType syntheticCarrier{},syntheticPassenger{};
    const UnitType* carrierType=nullptr;
    const UnitType* passengerType=nullptr;
    if(carrierName || passengerName) {
        if(!carrierName || !passengerName)
            throw std::runtime_error("both retail carrier and passenger names are required");
        registry=std::make_unique<ta::sim::TypeRegistry>();
        ta::sim::setupRegistry(*registry,vfs);
        carrierType=registry->find(carrierName);
        passengerType=registry->find(passengerName);
        if(!carrierType || !passengerType)
            throw std::runtime_error("retail carrier or passenger unit type not found");
        if(!carrierType->canTransport || carrierType->domain!=UnitType::Domain::Water ||
           !passengerType->canMove || passengerType->domain==UnitType::Domain::Water)
            throw std::runtime_error("retail unit profiles are not a surface carrier and land passenger");
    } else {
        syntheticCarrier=boatType();syntheticPassenger=footType();
        syntheticCarrier.domain=UnitType::Domain::Water;syntheticCarrier.floater=true;
        syntheticCarrier.minWaterDepth=13;syntheticCarrier.maxWaterDepth=10000;
        syntheticCarrier.footX=syntheticCarrier.footZ=4;
        syntheticPassenger.accel={}; // keep the release point observable after transfer
        carrierType=&syntheticCarrier;passengerType=&syntheticPassenger;
    }

    World world;setTestPathfinding(world);
    world.setTerrain(map.heights,map.width,map.height,map.seaLevel,&map.features);
    ta::sim::registerMapFeatures(world,map,vfs);
    if(registry) world.buildNavClasses(*registry);
    world.setPathService(true);
    const int startX=startCellX*16,startZ=startCellZ*16;
    const int goalX=goalCellX*16+8,goalZ=goalCellZ*16+8;
    const int carrierId=world.spawn(carrierType,startX,startZ);
    const int passengerId=world.spawn(passengerType,loadOrder?goalX:startX,
        loadOrder?goalZ:startZ);
    uint32_t pickupTick=0;
    if(loadOrder) {
        if(!carrierName || !world.canLoadInto(passengerId,carrierId)) {
            std::printf("FAIL: retail profiles do not admit this pickup\n");
            return false;
        }
        world.loadInto(passengerId,carrierId);
        for(uint32_t tick=1;tick<=20000;++tick) {
            world.tick(1.0f/30.0f);
            const auto* passenger=world.unit(passengerId);
            if(!passenger || passenger->inTransport!=carrierId) continue;
            pickupTick=tick;break;
        }
        std::printf("%s: %s at tick %u\n",pickupTick?"PASS":"FAIL",
            pickupTick?"retail passenger boarded through the issued load order":"retail pickup did not board the passenger",
            pickupTick);
        if(!pickupTick) return false;
    } else board(world,carrierId,passengerId);
    world.unloadAt(carrierId,float(goalX),float(goalZ));

    bool unloaded=false;
    uint32_t unloadTick=0,finalTick=0;
    int releasedX=0,releasedZ=0;
    bool carrierPassableAtRelease=false,carrierPassableAtEnd=false;
    bool passengerPhysicallyPlacedAtRelease=false;
    for(uint32_t tick=1;tick<=20000;++tick) {
        world.tick(1.0f/30.0f);
        const auto* carrierUnit=world.unit(carrierId);
        const auto* passengerUnit=world.unit(passengerId);
        if(!carrierUnit || !passengerUnit) break;
        if(carrierUnit->cargo.empty() && !passengerUnit->embarked()) {
            if(!unloaded) {
                unloaded=true;unloadTick=tick;
                releasedX=passengerUnit->x.v;releasedZ=passengerUnit->z.v;
                carrierPassableAtRelease=ta::sim::RetailReplayProbe::footprintPhysicallyPlaced(
                    world,carrierUnit);
                passengerPhysicallyPlacedAtRelease=
                    ta::sim::RetailReplayProbe::footprintPhysicallyPlaced(world,passengerUnit);
            }
            carrierPassableAtEnd=ta::sim::RetailReplayProbe::footprintPhysicallyPlaced(
                world,carrierUnit);
            finalTick=tick;
            if(tick>=unloadTick+60) break;
        }
    }
    const auto* carrierUnit=world.unit(carrierId);
    const auto* passengerUnit=world.unit(passengerId);
    std::printf("MAPTRAVEL %s %d %d %d %d %u %d %d %d %u\n",mapName,
        startX,startZ,goalX,goalZ,unloadTick,
        carrierUnit?int(carrierUnit->x.floorInt()):0,
        carrierUnit?int(carrierUnit->z.floorInt()):0,
        passengerUnit?int(passengerUnit->x.floorInt()):0,
        passengerUnit?unsigned(passengerUnit->inTransport):0);
    if(loadOrder) std::printf("PICKUP_TICK %u\n",pickupTick);
    if(passengerUnit)
        std::printf("LANDING %.4f %.4f final=%.4f,%.4f speed=%.4f orders=%zu\n",
            releasedX/65536.0f,releasedZ/65536.0f,
            passengerUnit->x.toFloat(),passengerUnit->z.toFloat(),passengerUnit->speed.toFloat(),
            passengerUnit->orders.size());
    if(carrierUnit && !carrierUnit->orders.empty()) {
        const auto& order=carrierUnit->orders[World::currentLeg(carrierUnit->orders)];
        const auto& mission=order.transportMission;
        std::printf("MISSION %zu %u %u %u %u %u %u %d %d %d %d %d %d %llu %llu\n",
            carrierUnit->orders.size(),unsigned(mission.stage),mission.waitMask,
            mission.deadline,mission.pending,mission.flags,order.transportTicks,
            order.x.floorInt(),order.z.floorInt(),order.missionTarget?order.missionTarget->first.floorInt():0,
            order.missionTarget?order.missionTarget->second.floorInt():0,
            carrierUnit->groundMovementMode,carrierUnit->groundSpeedMode,
            static_cast<unsigned long long>(world.pathStats().completions()),
            static_cast<unsigned long long>(world.pathStats().failures()));
    } else if(carrierUnit) std::printf("MISSION 0\n");
    const bool exactRelease=unloaded && releasedX==ta::sim::Fixed::fromInt(goalX).v &&
        releasedZ==ta::sim::Fixed::fromInt(goalZ).v;
    const int64_t landingDx=unloaded ? int64_t(releasedX)-ta::sim::Fixed::fromInt(goalX).v : INT64_MAX;
    const int64_t landingDz=unloaded ? int64_t(releasedZ)-ta::sim::Fixed::fromInt(goalZ).v : INT64_MAX;
    constexpr int64_t kRetailPostReleaseTolerance=8ll*65536;
    const bool retailLanding=carrierName && unloaded && landingDx*landingDx+landingDz*landingDz<=
        kRetailPostReleaseTolerance*kRetailPostReleaseTolerance;
    const bool landedAtSite=carrierName ? retailLanding : exactRelease;
    const bool emptyCargo=carrierUnit && carrierUnit->cargo.empty();
    const bool carrierInWater=carrierPassableAtEnd;
    const bool carrierRoutePassable=ta::sim::RetailReplayProbe::waterFootprintPassable(
        world,carrierUnit);
    const bool passengerPlaced=ta::sim::RetailReplayProbe::footprintPhysicallyPlaced(
        world,passengerUnit);
    std::printf("%s: %s\n",unloaded?"PASS":"FAIL",
        unloaded?"boat completed routed shore unload on the shipped map":"boat did not release its cargo");
    std::printf("%s: %s\n",landedAtSite?"PASS":"FAIL",
        landedAtSite?(carrierName?"retail passenger remains within 8px of the selected legal landing point":"passenger occupies the selected legal landing point"):
            "passenger did not occupy the requested landing point");
    std::printf("%s: %s\n",emptyCargo?"PASS":"FAIL",
        emptyCargo?"carrier cargo is empty after release":"carrier still owns cargo after release");
    if(carrierName) {
        std::printf("%s: %s\n",carrierPassableAtRelease?"PASS":"FAIL",
            carrierPassableAtRelease?"retail carrier footprint is passable at release":"retail carrier footprint is blocked at release");
        std::printf("%s: %s\n",passengerPhysicallyPlacedAtRelease?"PASS":"FAIL",
            passengerPhysicallyPlacedAtRelease?"retail passenger footprint is placeable at release":"retail passenger footprint is blocked at release");
        std::printf("COAST %u %d %d %d\n",finalTick-unloadTick,
            carrierUnit?carrierUnit->x.floorInt():0,carrierUnit?carrierUnit->z.floorInt():0,
            int(carrierPassableAtEnd));
        std::printf("%s: %s\n",carrierRoutePassable?"PASS":"FAIL",
            carrierRoutePassable?"carrier footprint remains passable in its water navigation grid":
                "carrier footprint is blocked in its water navigation grid");
    }
    std::printf("%s: %s\n",carrierInWater?"PASS":"FAIL",
        carrierInWater?"surface carrier footprint remains physically valid through the 60-tick post-release coast":"surface carrier footprint is invalid after the 60-tick post-release coast");
    std::printf("%s: %s\n",passengerPlaced?"PASS":"FAIL",
        passengerPlaced?"released passenger remains physically placeable after the coast":"released passenger ends on a blocked or invalid footprint");
    return unloaded && landedAtSite && emptyCargo && carrierInWater &&
        (!carrierName || carrierRoutePassable) &&
        (!carrierName || passengerPhysicallyPlacedAtRelease) && passengerPlaced;
}

// Paired with the native 0x4dc800 flight-mover probe.  This isolates the
// long-lived point-controller update used by air transports after their
// dispatcher has installed a destination, so the comparison can distinguish
// flight integration from pickup/unload timing.
static void airFlightTraceFixture(unsigned steps) {
    World w;setTestPathfinding(w);
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,40);w.setPathService(false);
    UnitType air=boatType();
    air.name="flight trace";air.canFly=true;air.cruiseAlt=100;
    air.maxVel=ta::sim::Fixed::raw(120000);
    // The native flight kernel uses FBI brakerate (+0x166) as its lateral
    // velocity limit, while acceleration (+0x16a) limits the full 3D change.
    air.accel=ta::sim::Fixed::raw(20000);air.brake=ta::sim::Fixed::raw(60000);
    air.turnRate=10000;
    const int id=w.spawn(&air,160,160);
    auto* u=w.unit(id);
    u->baseSpeed=air.maxVel;u->speed=ta::sim::Fixed();
    u->heading=ta::sim::retailHeadingToPort(0);
    u->flightY=ta::sim::Fixed::fromInt(100);
    u->flightVelocity={};
    u->flightNavigation={{160<<16,100<<16,160<<16},{},0};
    w.order(id,800,800,false);
    // This fixture starts after controller installation, matching the native
    // direct 0x4e40e0 call rather than dispatching an ordinary VTOL_Move.
    u->orders.front().flightMoveMission=false;
    u->orders.front().mission={};
    for(unsigned step=1;step<=steps;++step) {
        w.tick(1.f/30);
        u=w.unit(id);
        const auto& n=u->flightNavigation;
        const auto& v=u->flightVelocity;
        std::printf("%u %d %d %d %u %d %d %d %d %d %d %d %d %d %d %u\n",
            step,u->x.v,u->flightY.v,u->z.v,
            unsigned(ta::sim::portHeadingToRetail(u->heading)),u->speed.v,
            v.x,v.y,v.z,n.destination.x,n.destination.y,n.destination.z,
            n.velocity.x,n.velocity.y,n.velocity.z,unsigned(n.heading));
    }
}

// The distant VTOL unload case starts in the carrier's mission dispatcher and
// exercises the native transportdistance-34 circle before its transfer/step-out
// phase. The Python side pairs this World trace with the retail handler's goal
// and full 0x4dc800 mover.
struct AirUnloadFlightTraceProfile {
    int32_t maxVelocityRaw=120000,accelerationRaw=20000,brakeRaw=60000;
    int32_t turnRate=10000,cruiseAlt=100,transportDistance=150;
    int32_t footprintX=1,footprintZ=1;
    int32_t waterMultiplierRaw=0x10000,roadMultiplierRaw=0x13333;
    int32_t halfCellTicks=70;
};

static void airUnloadFlightTraceFixture(unsigned steps,bool heightStep=false,
        const AirUnloadFlightTraceProfile* profile=nullptr) {
    World w;setTestPathfinding(w);
    std::vector<uint8_t> terrain(256*256,100);
    // The bounded height-step trace puts a tall ridge across the direct flight
    // goal so sector-height refresh is exercised without a long flat replay.
    if(heightStep)for(int z=0;z<256;++z)for(int x=220;x<256;++x)
        terrain[size_t(z)*256+x]=220;
    w.setTerrain(terrain,256,256,40);w.setPathService(false);
    UnitType air=boatType(),passenger=footType();
    air.name="flight unload trace";air.canFly=true;air.cruiseAlt=100;
    air.maxVel=ta::sim::Fixed::raw(120000);
    air.accel=ta::sim::Fixed::raw(20000);air.brake=ta::sim::Fixed::raw(60000);
    air.turnRate=10000;
    if(profile) {
        air.maxVel=ta::sim::Fixed::raw(profile->maxVelocityRaw);
        air.accel=ta::sim::Fixed::raw(profile->accelerationRaw);
        air.brake=ta::sim::Fixed::raw(profile->brakeRaw);
        air.turnRate=profile->turnRate;
        air.cruiseAlt=profile->cruiseAlt;
        air.transportDist=profile->transportDistance;
        air.footX=profile->footprintX;
        air.footZ=profile->footprintZ;
        air.waterMult=ta::sim::Fixed::raw(profile->waterMultiplierRaw);
        air.roadMult=ta::sim::Fixed::raw(profile->roadMultiplierRaw);
        air.halfCellTicks=profile->halfCellTicks;
    }
    const int carrier=w.spawn(&air,3000,3000),cargo=w.spawn(&passenger,3000,3000);
    board(w,carrier,cargo);
    auto* u=w.unit(carrier);
    u->baseSpeed=air.maxVel;u->speed=ta::sim::Fixed();
    u->heading=ta::sim::retailHeadingToPort(0);
    u->flightY=ta::sim::Fixed::fromInt(100);
    u->flightVelocity={};
    u->flightNavigation={{3000<<16,100<<16,3000<<16},{},0};
    w.unloadAt(carrier,4000,3000);
    for(unsigned step=1;step<=steps;++step) {
        w.tick(1.f/30);
        u=w.unit(carrier);
        const auto& n=u->flightNavigation;
        const auto& v=u->flightVelocity;
        const auto* order=u->orders.empty()?nullptr:&u->orders.front();
        const ta::sim::RetailMissionState emptyMission{};
        const auto& mission=order?order->transportMission:emptyMission;
        const auto* goal=order && order->flightGoal ? &*order->flightGoal :
            (u->retainedFlightGoal ? &*u->retainedFlightGoal : nullptr);
        const bool goalActive=order && order->flightGoal ? true : u->retainedFlightControllerActive;
        const auto* activeGoal=goalActive ? goal : nullptr;
        const auto* passengerUnit=w.unit(cargo);
        const bool passengerParked=passengerUnit && !passengerUnit->orders.empty() &&
            passengerUnit->orders.front().park.has_value();
        std::printf("%u %d %d %d %u %d %d %d %d %d %d %d %d %d %d %u ",
            step,u->x.v,u->flightY.v,u->z.v,
            unsigned(ta::sim::portHeadingToRetail(u->heading)),u->speed.v,
            v.x,v.y,v.z,n.destination.x,n.destination.y,n.destination.z,
            n.velocity.x,n.velocity.y,n.velocity.z,unsigned(n.heading));
        std::printf("%u %u %u %u %u %u %u %d %d %d %u %u %u %u %u %u\n",
            unsigned(order!=nullptr),unsigned(mission.stage),order?mission.waitMask:0,
            order?mission.deadline:0,order?mission.pending:0,order?unsigned(order->transportTicks):0,
            unsigned(goalActive),activeGoal?activeGoal->point.x:0,activeGoal?activeGoal->point.y:0,
            activeGoal?activeGoal->point.z:0,activeGoal?unsigned(activeGoal->flags):0,
            activeGoal?unsigned(uint16_t(activeGoal->radius)):0,unsigned(u->cargo.size()),
            unsigned(passengerUnit && passengerUnit->embarked()),
            unsigned(w.transportEffects().size()*2),
            unsigned(passengerParked));
    }
}

// Asset-backed VTOL unload trace for a real TNT map and shipped unit profiles.
// The native Python peer feeds these exact coordinates/map records to retail
// 0x50e740 and the live 0x4d8450/0x4dc800 unload path.
static void airUnloadMapFlightTraceFixture(const char* retailRoot,const char* mapName,
        const char* carrierName,const char* passengerName,int startCellX,int startCellZ,
        int targetCellX,int targetCellZ,unsigned steps) {
    auto vfs=ta::hpi::mountRetailRoot(retailRoot);
    const std::string mapPath=ta::hpi::findMap(vfs,mapName);
    if(mapPath.empty()) throw std::runtime_error(std::string("map not found: ")+mapName);
    const auto map=ta::tnt::Map::load(vfs.read(mapPath),mapPath);
    if(startCellX<0 || startCellZ<0 || targetCellX<0 || targetCellZ<0 ||
       startCellX>=map.width || startCellZ>=map.height ||
       targetCellX>=map.width || targetCellZ>=map.height)
        throw std::runtime_error("air-unload map trace coordinates outside the map");

    ta::sim::TypeRegistry registry;
    ta::sim::setupRegistry(registry,vfs);
    const auto* carrierType=registry.find(carrierName);
    const auto* passengerType=registry.find(passengerName);
    if(!carrierType || !passengerType)
        throw std::runtime_error("retail air carrier or passenger type not found");
    if(!carrierType->canFly || !carrierType->canTransport || !passengerType->canMove ||
       passengerType->canFly)
        throw std::runtime_error("retail unit profiles are not an aerial carrier and ground passenger");

    World w;setTestPathfinding(w);
    w.setTerrain(map.heights,map.width,map.height,map.seaLevel,&map.features);
    ta::sim::registerMapFeatures(w,map,vfs);
    w.buildNavClasses(registry);
    w.setPathService(false);
    const int startX=startCellX*16+8,startZ=startCellZ*16+8;
    const int targetX=targetCellX*16+8,targetZ=targetCellZ*16+8;
    const int carrierId=w.spawn(carrierType,startX,startZ);
    const int passengerId=w.spawn(passengerType,startX,startZ);
    board(w,carrierId,passengerId);
    auto* carrier=w.unit(carrierId);
    carrier->baseSpeed=carrierType->maxVel;
    carrier->speed=ta::sim::Fixed();
    carrier->heading=ta::sim::retailHeadingToPort(0);
    carrier->flightNavigation={{carrier->x.v,carrier->flightY.v,carrier->z.v},{},0};
    const auto terrainAt=[&](int x,int z) {
        return int(map.heights[size_t(z)*size_t(map.width)+size_t(x)]);
    };
    w.unloadAt(carrierId,float(targetX),float(targetZ),
               ta::sim::Fixed::fromInt(terrainAt(targetCellX,targetCellZ)));

    std::printf("MAPTRACE %d %d %d %s %s %d %d %d %d %d %d %d\n",
        map.width,map.height,map.seaLevel,carrierName,passengerName,
        startCellX,startCellZ,targetCellX,targetCellZ,terrainAt(startCellX,startCellZ),
        terrainAt(targetCellX,targetCellZ),carrierType->cruiseAlt);
    for(unsigned step=1;step<=steps;++step) {
        w.tick(1.f/30);
        carrier=w.unit(carrierId);
        const auto& n=carrier->flightNavigation;
        const auto& v=carrier->flightVelocity;
        const auto* order=carrier->orders.empty()?nullptr:&carrier->orders.front();
        const ta::sim::RetailMissionState emptyMission{};
        const auto& mission=order?order->transportMission:emptyMission;
        const auto* goal=order && order->flightGoal ? &*order->flightGoal :
            (carrier->retainedFlightGoal ? &*carrier->retainedFlightGoal : nullptr);
        const bool goalActive=order && order->flightGoal ? true :
            carrier->retainedFlightControllerActive;
        const auto* activeGoal=goalActive ? goal : nullptr;
        const auto* passenger=w.unit(passengerId);
        const bool passengerParked=passenger && !passenger->orders.empty() &&
            passenger->orders.front().park.has_value();
        std::printf("%u %d %d %d %u %d %d %d %d %d %d %d %d %d %d %u ",
            step,carrier->x.v,carrier->flightY.v,carrier->z.v,
            unsigned(ta::sim::portHeadingToRetail(carrier->heading)),carrier->speed.v,
            v.x,v.y,v.z,n.destination.x,n.destination.y,n.destination.z,
            n.velocity.x,n.velocity.y,n.velocity.z,unsigned(n.heading));
        std::printf("%u %u %u %u %u %u %u %d %d %d %u %u %u %u %u %u\n",
            unsigned(order!=nullptr),unsigned(mission.stage),mission.waitMask,
            mission.deadline,mission.pending,unsigned(order?order->transportTicks:0),
            unsigned(goalActive),activeGoal?activeGoal->point.x:0,
            activeGoal?activeGoal->point.y:0,activeGoal?activeGoal->point.z:0,
            activeGoal?unsigned(activeGoal->flags):0,
            activeGoal?unsigned(uint16_t(activeGoal->radius)):0,
            unsigned(carrier->cargo.size()),unsigned(passenger && passenger->embarked()),
            unsigned(w.transportEffects().size()*2),unsigned(passengerParked));
    }
}

int main(int argc,char** argv) {
    // Optional backend run reuses the same placement/mission assertions. The
    // default and every retail trace command retain their existing backend.
    if(argc>1 && !std::strcmp(argv[1],"--legion")) {
        g_pathfinding=ta::sim::PathfindingMode::Legion;--argc;++argv;
    }
    if(argc==2 && !std::strcmp(argv[1],"--attach-position")) {
        // check_cargo_attach_position.py: host x y z, floater, waterline, sea, piece.
        long long hx,hy,hz;int floater,waterline,sea,piece;
        while(std::scanf("%lld %lld %lld %d %d %d %d",&hx,&hy,&hz,&floater,&waterline,&sea,&piece)==7) {
            const auto point=ta::sim::retailAttachedPosition(
                ta::sim::retailAttachPoint({int32_t(hx),int32_t(hy),int32_t(hz)},{},{},piece,0),
                floater!=0,uint8_t(waterline),uint8_t(sea));
            std::printf("%d %d %d\n",point[0],point[1],point[2]);
        }
        return 0;
    }
    if(argc==3 && !std::strcmp(argv[1],"--air-flight-trace")) {
        airFlightTraceFixture(unsigned(std::clamp(std::atoi(argv[2]),1,10000)));
        return 0;
    }
    if(argc==3 && !std::strcmp(argv[1],"--air-unload-flight-trace")) {
        airUnloadFlightTraceFixture(unsigned(std::clamp(std::atoi(argv[2]),1,10000)));
        return 0;
    }
    if(argc==3 && !std::strcmp(argv[1],"--air-unload-heightstep-trace")) {
        airUnloadFlightTraceFixture(unsigned(std::clamp(std::atoi(argv[2]),1,10000)),true);
        return 0;
    }
    if(argc==13 && !std::strcmp(argv[1],"--air-unload-heightstep-profile")) {
        AirUnloadFlightTraceProfile profile;
        profile.maxVelocityRaw=std::atoi(argv[3]);
        profile.accelerationRaw=std::atoi(argv[4]);
        profile.brakeRaw=std::atoi(argv[5]);
        profile.turnRate=std::atoi(argv[6]);
        profile.cruiseAlt=std::atoi(argv[7]);
        profile.transportDistance=std::atoi(argv[8]);
        profile.footprintX=std::atoi(argv[9]);
        profile.footprintZ=std::atoi(argv[10]);
        profile.waterMultiplierRaw=std::atoi(argv[11]);
        profile.roadMultiplierRaw=std::atoi(argv[12]);
        const int64_t bestSpeed=(int64_t(std::max(profile.waterMultiplierRaw,
            std::max(profile.roadMultiplierRaw,0x10000)))*profile.maxVelocityRaw)>>16;
        profile.halfCellTicks=bestSpeed<=0?255:int32_t(std::clamp<int64_t>(
            (int64_t(8)<<16)/bestSpeed,1,255));
        if(profile.maxVelocityRaw<=0 || profile.accelerationRaw<0 ||
           profile.brakeRaw<0 || profile.turnRate<0 || profile.cruiseAlt<0 ||
           profile.transportDistance<=34 || profile.footprintX<1 || profile.footprintZ<1) {
            std::fprintf(stderr,"invalid air-unload carrier profile\n");
            return 2;
        }
        airUnloadFlightTraceFixture(unsigned(std::clamp(std::atoi(argv[2]),1,10000)),
                                    true,&profile);
        return 0;
    }
    if(argc==11 && !std::strcmp(argv[1],"--air-unload-map-flight-trace")) {
        airUnloadMapFlightTraceFixture(argv[2],argv[3],argv[4],argv[5],
            std::atoi(argv[6]),std::atoi(argv[7]),std::atoi(argv[8]),std::atoi(argv[9]),
            unsigned(std::clamp(std::atoi(argv[10]),1,10000)));
        return 0;
    }
    if(argc==10 && !std::strcmp(argv[1],"--surface-unload-map-route-type")) {
        surfaceUnloadMapRouteFixture(argv[2],argv[3],std::atoi(argv[6]),std::atoi(argv[7]),
                                     std::atoi(argv[8]),std::atoi(argv[9]),0,
                                     argv[4],argv[5]);
        return 0;
    }
    if(argc==8 && !std::strcmp(argv[1],"--surface-unload-map-route")) {
        surfaceUnloadMapRouteFixture(argv[2],argv[3],std::atoi(argv[4]),std::atoi(argv[5]),
                                     std::atoi(argv[6]),std::atoi(argv[7]),4);
        return 0;
    }
    if(argc==9 && !std::strcmp(argv[1],"--surface-unload-map-route")) {
        surfaceUnloadMapRouteFixture(argv[2],argv[3],std::atoi(argv[4]),std::atoi(argv[5]),
                                     std::atoi(argv[6]),std::atoi(argv[7]),std::atoi(argv[8]));
        return 0;
    }
    if(argc==8 && !std::strcmp(argv[1],"--surface-unload-map-travel")) {
        return surfaceUnloadMapTravelFixture(argv[2],argv[3],std::atoi(argv[4]),
            std::atoi(argv[5]),std::atoi(argv[6]),std::atoi(argv[7])) ? 0 : 1;
    }
    if(argc==10 && !std::strcmp(argv[1],"--surface-unload-map-travel-type")) {
        return surfaceUnloadMapTravelFixture(argv[2],argv[3],std::atoi(argv[6]),
            std::atoi(argv[7]),std::atoi(argv[8]),std::atoi(argv[9]),argv[4],argv[5]) ? 0 : 1;
    }
    if(argc==10 && !std::strcmp(argv[1],"--surface-transport-map-roundtrip-type")) {
        return surfaceUnloadMapTravelFixture(argv[2],argv[3],std::atoi(argv[6]),
            std::atoi(argv[7]),std::atoi(argv[8]),std::atoi(argv[9]),argv[4],argv[5],true) ? 0 : 1;
    }
    if((argc==2 || argc==3) && !std::strcmp(argv[1],"--surface-unload-route-fixture")) {
        const int variant=argc==3 ? std::atoi(argv[2]) : 0;
        if(variant<0 || variant>8) return 2;
        surfaceUnloadRouteFixture(variant);return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--passenger-pickup")) {
        unsigned stage,retries,now,events,surface,mask,deadline;
        while(std::scanf("%u %u %u %u %u %u %u",&stage,&retries,&now,&events,&surface,&mask,&deadline)==7) {
            ta::sim::RetailMissionState m{uint8_t(stage),mask,deadline};
            int approaches=0,detaches=0;
            const int result=ta::sim::retailPassengerPickup(m,retries,now,events,surface!=0,
                [&]{++approaches;},[&]{++detaches;});
            std::printf("%d %u %u %u %u %d %d\n",result,unsigned(m.stage),retries,
                m.waitMask,m.deadline,approaches,detaches);
        }
        return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--pickup-approach-abort")) {
        unsigned air,mover,events;int speed;
        while(std::scanf("%u %u %u %d",&air,&mover,&events,&speed)==4)
            std::printf("%d\n",int(ta::sim::retailPickupApproachAborted(air!=0,mover!=0,events,speed)));
        return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--pickup-pursuit")) {
        int x,y,z,tx,ty,tz;unsigned range;
        while(std::scanf("%d %d %d %d %d %d %u",&x,&y,&z,&tx,&ty,&tz,&range)==7) {
            const auto goal=ta::sim::retailPickupPursuitGoal({tx,ty,tz},uint16_t(range));
            std::printf("%d %d %d %u %d %d\n",goal.point.x,goal.point.y,goal.point.z,
                unsigned(goal.flags),int(goal.radius),int(goal.accepts({x,y,z})));
        }
        return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--pickup-approach-wait")) {
        unsigned air,draw;
        while(std::scanf("%u %u",&air,&draw)==2) {
            ta::sim::RetailMissionState mission;unsigned bound=0;
            ta::sim::retailPickupApproachWait(mission,100,air!=0,[&](unsigned n){bound=n;return draw;});
            std::printf("%u %u %u\n",mission.waitMask,mission.deadline,bound);
        }
        return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--pickup-timeline")) {
        for(bool air:{false,true}) {
            World w;setTestPathfinding(w);
            w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            UnitType carrier=boatType(),passenger=footType();carrier.canFly=air;carrier.cruiseAlt=100;
            const int tid=w.spawn(&carrier,400,400),cid=w.spawn(&passenger,430,400);
            w.loadInto(cid,tid);
            for(int tick=1;tick<=18;++tick) {
                w.tick(1.f/30);
                const auto& orders=w.unit(tid)->orders;
                const auto* pickup=orders.empty() ? nullptr : &orders.front();
                std::printf("%d %d %d %d %u %d\n",int(air),tick,
                    pickup ? int(pickup->transportMission.stage) : -1,
                    pickup ? pickup->transportTicks : 0,
                    pickup ? pickup->transportApproachAttempts : 0,int(w.unit(cid)->embarked()));
            }
        }
        return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--air-unload-world-timeline")) {
        airUnloadWorldTimeline();
        return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--surface-unload-world-timeline")) {
        surfaceUnloadWorldTimeline();
        return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--pickup-transfer")) {
        unsigned air,attempts;int ticks,speed;
        while(std::scanf("%u %u %d %d",&air,&attempts,&ticks,&speed)==4) {
            ta::sim::RetailMissionState mission;int effects=0;
            const int result=ta::sim::retailPickupTransfer(mission,ticks,attempts,100,air!=0,speed,[&]{++effects;});
            std::printf("%d %u %d %u %d\n",result,attempts,ticks,
                mission.waitMask&1 ? mission.deadline-100:0,effects);
        }
        return 0;
    }
    if (argc==2 && !std::strcmp(argv[1],"--transport-range")) {
        int dx,dz;unsigned radius;
        while (std::scanf("%d %d %u",&dx,&dz,&radius)==3)
            std::printf("%d\n",ta::sim::retailTransportInRange(dx,dz,uint16_t(radius)));
        return 0;
    }
    if (argc==2 && !std::strcmp(argv[1],"--unload-stepout")) {
        int x,y,z;unsigned heading,range;
        while (std::scanf("%d %d %d %u %u",&x,&y,&z,&heading,&range)==5) {
            const auto goal=ta::sim::retailUnloadStepOut({x,y,z},uint16_t(heading),uint16_t(range));
            std::printf("%d %d %d %u %d\n",goal.point.x,goal.point.y,goal.point.z,goal.flags,goal.radius);
        }
        return 0;
    }
    if (argc==2 && !std::strcmp(argv[1],"--unload-transfer")) {
        unsigned stage,attempts;int ticks,valid,strict,relaxed;
        while (std::scanf("%u %d %u %d %d %d",&stage,&ticks,&attempts,&valid,&strict,&relaxed)==6) {
            ta::sim::RetailMissionState m{uint8_t(stage)};int effects=0,blocked=0,queries=0;
            const int result=ta::sim::retailUnloadTransfer(m,ticks,attempts,100,valid,
                [&](bool allow){++queries;return allow ? relaxed : strict;},
                [&]{++effects;},[&]{++blocked;});
            std::printf("%d %u %d %u %d %d %d\n",result,unsigned(m.stage),ticks,
                m.waitMask&1 ? m.deadline-100 : 0,effects,blocked,queries);
        }
        return 0;
    }
    if (argc==2 && !std::strcmp(argv[1],"--unload-approach")) {
        unsigned stage,attempts,next,inRange,mover;
        while (std::scanf("%u %u %u %u %u",&stage,&attempts,&next,&inRange,&mover)==5) {
            ta::sim::RetailMissionState m{uint8_t(stage)};unsigned approaches=0;
            const int result=ta::sim::retailTransportUnloadApproach(m,attempts,100,0,
                inRange!=0,mover!=0,next!=0,false,[&]{++approaches;},[](int){return 0u;});
            const unsigned delay=(m.waitMask&1u) ? m.deadline-100u : 0u;
            std::printf("%d %u %u %u %u %u\n",result,unsigned(m.stage),attempts,
                m.waitMask,delay,approaches);
        }
        return 0;
    }
    if (argc==2 && !std::strcmp(argv[1],"--unload-approach-timeline")) {
        using namespace ta::sim;
        struct Host {
            RetailMissionState mission{1};
            uint32_t now=0,attempts=0,requests=0;
            bool inRange=false,hasMover=true,active=true;
            bool enabled() const { return active; }
            RetailMissionState* head() { return active && mission.stage<=1 ? &mission : nullptr; }
            bool hasNext(const RetailMissionState&) const { return false; }
            uint32_t random(uint32_t) { return 0; }
            void idle() { active=false; }
            void remove(RetailMissionState&) { active=false; }
            void rotate(RetailMissionState&) { active=false; }
            void clear() { active=false; }
            int handle(RetailMissionState& m,uint32_t events) {
                return retailTransportUnloadApproach(m,attempts,now,events,inRange,hasMover,false,false,
                    [&]{++requests;},[](int){return 0u;});
            }
        } host;
        uint32_t unitEvents=0,tick; unsigned raised,inRange,hasMover;
        while (std::scanf("%u %u %u %u",&tick,&raised,&inRange,&hasMover)==4) {
            host.now=tick;host.inRange=inRange!=0;host.hasMover=hasMover!=0;
            unitEvents|=raised;
            const uint32_t before=host.requests;
            retailDispatchMissions(tick,unitEvents,host);
            std::printf("%u %u %u %u %u %u %u %u %u\n",tick,
                host.requests-before,unsigned(host.active),unsigned(host.mission.stage),
                host.mission.waitMask,host.mission.deadline,host.mission.pending,
                host.attempts,unitEvents);
        }
        return 0;
    }
    if (argc==2 && !std::strcmp(argv[1],"--unload-park")) {
        unsigned first,second,remaining;int footX,footZ;
        while (std::scanf("%u %u %u %d %d",&first,&second,&remaining,&footX,&footZ)==5)
            std::printf("%u\n",ta::sim::retailUnloadParkPadding(first,second,remaining,int16_t(footX),int16_t(footZ)));
        return 0;
    }
    if (argc==2 && !std::strcmp(argv[1],"--unload-park-order")) {
        // tools/re/check_unload_park_order.py: "flags left held carried holder"
        // per line -> 1 when the released passenger's PARK goes in front.
        unsigned flags,held,carried,holder;float left;
        while (std::scanf("%x %f %u %u %u",&flags,&left,&held,&carried,&holder)==5) {
            ta::sim::Unit c;
            c.deadFor=(flags&0x1000000u) ? -1 : 0;
            c.hp=(flags&0x1000u) ? ta::sim::Fixed() : ta::sim::Fixed::fromInt(100);
            c.selectable=(flags&0x20u)!=0;
            c.underConstruction=left>0.0f;
            c.inTransport=carried ? 99 : 0;
            std::printf("%d\n",int(World::unloadParkAhead(c)));
        }
        return 0;
    }
    if(argc==3 && !std::strcmp(argv[1],"--post-unload")) {
        postUnloadRoster(argv[2]);
        postUnloadQueued(argv[2]);
        std::printf(g_fail ? "transport_test: %d FAILURE(S)\n" : "transport_test: all passed\n",g_fail);
        return g_fail ? 1 : 0;
    }
    if (argc==2 && !std::strcmp(argv[1],"--capacity")) {
        unsigned size,maximum,count,limit,used,budget;
        while (std::scanf("%u %u %u %u %u %u",&size,&maximum,&count,&limit,&used,&budget)==6)
            std::printf("%d\n",ta::sim::retailTransportCapacity(size,maximum,count,limit,used,budget));
        return 0;
    }
    std::printf("transport_test\n");
    passengerMissionSchedule();
    passengerCancellationWait();
    if(argc==2) retailRoster(argv[1]);
    boardingLimits();
    transferLifecycle();
    unloadParkPlace();
    ordinaryFlightMoves();
    loadOrderQueueing();
    queueUnloadDuringPickup();
    pickupTransferInterruption();
    unloadTransferInterruption();
    transportEffectEvents();
    unloadDestinationRetention();
    pickupRangeBoundary();
    pickupBraking();
    airPickupTransfer();
    cargoSight();
    surfacePickupQueue(false);
    surfacePickupQueue(true);
    pickupMissionOwnership();
    pickupApproachPolling();
    pickupControllerPendingEvents();
    airUnloadControllerPendingEvents();
    airUnloadArrivalEvent();
    surfacePickupNavigation();
    pickupApproachFailure();
    airPickupArrivalEvent();
    pickupNearbySelection(false);pickupNearbySelection(false,true);
    airCarriesOne();
    exactLandingSites();
    airAcrossWater();
    legionLogistics();
    openGround();
    alreadyInRange();
    coastline();
    crowdedCoastlineUnloadRetry();
    farInland();
    unloadApproachFailure();
    retargetWithPendingRoute();
    isolatedPond();
    std::printf(g_fail ? "transport_test: %d FAILURE(S)\n" : "transport_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
