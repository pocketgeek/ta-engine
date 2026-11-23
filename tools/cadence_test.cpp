// cadence_test -- retail's repath cadence ladder (icd 0x4e51f3-0x4e5491).
//
// Two behaviours the ladder defines, each porting a traced branch:
//
//   1. Repeated refusal bypasses the random cadence (0x4e51d4 -> 0x4e5486).
//      A blocked mover immediately asks again after a search finishes. Admission
//      still enforces the navigator's 15-tick minimum interval (4e54a0). The old
//      gentle-probing expectation overlooked that branch and was incorrect.
//
//   2. A healthy route on a long march re-asks on the same tail, which is how a
//      trip longer than one search's visit limit chains route by route.
//
// Synthetic world, synthetic types: the ladder reads halfCellTicks (default 6,
// a standard walker), floater and minwaterdepth.

#include "sim/sim.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace tak::sim {
struct RetailReplayProbe {
    static void boundary(World& world,uint32_t tick,uint32_t seed) {
        world.tickCounter_=tick;world.gameRng_=seed;
    }
    static uint32_t seed(const World& world) { return world.gameRng_; }
};
}
using namespace tak::sim;

static int g_fail = 0;
static void check(bool cond, const char* what, const std::string& detail = {}) {
    std::printf("  %-62s %s%s%s\n", what, cond ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!cond) ++g_fail;
}

static UnitType walkerType() {
    UnitType t{};
    t.name = "walker"; t.id = "walker";
    t.maxVel = Fixed::fromFloat(48.0f / 30.0f);
    t.turnRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.footX = 2; t.footZ = 2;
    return t;
}

static World* makeWorld(int W, int H) {
    World* w = new World();
    w->setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
    w->setPathService(true);
    return w;
}

int main(int argc,char** argv) {
    if (argc==2 && std::strcmp(argv[1],"--oracle")==0) {
        uint32_t tick,stamp,seed;unsigned outcome,mode,speedMode,streak,boat,scale;
        int minimumDepth,maximumDepth,inactive,count;
        while (std::scanf("%u %u %u %u %u %u %u %u %u %d %d %d %d",&tick,&stamp,&seed,&outcome,
                          &mode,&speedMode,&streak,&boat,&scale,&minimumDepth,&maximumDepth,&inactive,&count)==13) {
            World world;world.setPathService(true);
            world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
            UnitType type=walkerType();type.floater=boat!=0;
            type.minWaterDepth=minimumDepth;type.maxWaterDepth=maximumDepth;
            type.halfCellTicks=int32_t(scale);
            const int id=world.spawn(&type,128,128);auto& u=*world.unit(id);
            Order goal;goal.x=goal.z=Fixed::fromInt(800);goal.goal=true;
            goal.segmentX=goal.segmentZ=Fixed::fromInt(128);goal.hasSegment=true;
            goal.navigationExhausted=inactive;goal.navigationConsumed=count<2;u.orders={goal};
            u.routeStamp=int32_t(stamp);u.routeCrowded=outcome&1;u.routeTraffic=outcome&2;
            u.routeFailed=outcome&4;u.routeDetour=outcome&8;
            u.groundMovementMode=uint8_t(mode);u.groundSpeedMode=uint8_t(speedMode);
            u.bodyBlockStreak=int32_t(streak);u.groundScanTick=UINT32_MAX;
            RetailReplayProbe::boundary(world,tick,seed);
            std::vector<int> bounds;
            world.setRngObserver([&](const World::RngObservation& d){bounds.push_back(d.bound);});
            world.tickNavigationMovement(u,u.baseSpeed);
            std::printf("%u %u %u",unsigned(world.pathStats().pending(id)),RetailReplayProbe::seed(world),
                        unsigned(u.routeCrowded|u.routeTraffic<<1|u.routeFailed<<2|u.routeDetour<<3));
            for (int bound:bounds) std::printf(" %d",bound);
            std::printf("\n");
        }
        return 0;
    }
    std::printf("cadence_test\n");

    for (bool consumed : {false,true}) for (bool refused : {false,true}) {
        World w;w.setPathService(true);
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType type=walkerType();const int id=w.spawn(&type,128,128);auto& u=*w.unit(id);
        Order goal;goal.goal=goal.groundMission=true;goal.controller=1;
        goal.x=goal.z=Fixed::fromInt(800);goal.hasSegment=true;
        goal.segmentX=u.x;goal.segmentZ=u.z;goal.navigationExhausted=true;
        goal.navigationConsumed=consumed;goal.missionTarget=std::pair{goal.x,goal.z};
        u.orders={goal};u.routeStamp=0;u.bodyBlockStreak=refused?2:0;
        u.routeCrowded=u.routeTraffic=u.routeFailed=u.routeDetour=true;
        RetailReplayProbe::boundary(w,1,123);
        w.tickNavigationMovement(u,u.baseSpeed);
        check(w.pathStats().pending(id)==(consumed||refused),
              "inactive route retries immediately only after consumption or repeated refusal");
        check(u.routeFailed==!(consumed||refused) && u.routeCrowded==u.routeFailed &&
              u.routeTraffic==u.routeFailed && u.routeDetour==u.routeFailed,
              "new pending request clears old route outcomes");
    }

    // A partial route ending at the mover is not a completed mission.
    {
        World w;w.setPathService(true);
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType type=walkerType();type.brake=Fixed::fromFloat(.125f);
        const int id=w.spawn(&type,128,128);auto& u=*w.unit(id);
        Order goal;goal.x=u.x;goal.z=u.z;goal.goal=goal.groundMission=true;
        goal.controller=1;goal.hasSegment=true;goal.segmentX=u.x;goal.segmentZ=u.z;
        goal.missionTarget=std::pair{Fixed::fromInt(800),Fixed::fromInt(800)};
        u.orders={goal};u.speed=Fixed::fromInt(1);u.routeStamp=0;
        const Fixed x=u.x,z=u.z;
        w.tickNavigationMovement(u,u.baseSpeed);
        check(u.orders.size()==1 && u.orders.front().navigationExhausted && u.orders.front().navigationConsumed &&
              !(u.orders.front().mission.pending&0x100),
              "partial endpoint exhausts navigator without completing mission");
        check(w.pathStats().pending(id),"exhausted route requests another path to mission target");
        check(u.speed>Fixed() && u.speed<Fixed::fromInt(1) && (u.x!=x || u.z!=z),
              "exhausted navigator coasts while braking");
        w.order(id,128,800,false);
        check(!u.orders.empty() && !u.orders.back().navigationExhausted,
              "replacement order starts with an active navigator");
    }

    // ---- 1. walled off by parked bodies: immediate repeated-refusal retry ----
    {
        World& w = *makeWorld(160, 160);
        UnitType wt = walkerType();
        // This cadence fixture assumes the enclosing wall is already known.
        wt.sight = 4096; wt.upright = true;
        std::vector<int> wall;
        for (int d = -3; d <= 3; ++d) {
            for (int ring = 0; ring < 2; ++ring) {
                const float r = 96.0f + 32.0f * float(ring);
                wall.push_back(w.spawn(&wt, 1600.0f + float(d) * 32.0f, 1600.0f - r, 0, 1));
                wall.push_back(w.spawn(&wt, 1600.0f + float(d) * 32.0f, 1600.0f + r, 0, 1));
                wall.push_back(w.spawn(&wt, 1600.0f - r, 1600.0f + float(d) * 32.0f, 0, 1));
                wall.push_back(w.spawn(&wt, 1600.0f + r, 1600.0f + float(d) * 32.0f, 0, 1));
            }
        }
        const int u = w.spawn(&wt, 600, 1600, 0, 0);
        for (int i = 0; i < 60; ++i) w.tick(1.0f / 30.0f);
        w.order(u, 1600, 1600, false);
        // Isolate the navigator ladder. The ground mission now expands its
        // acceptance radius on failure and can complete this order at the wall.
        w.unit(u)->orders.back().groundMission = false;
        for (int i = 0; i < 30 * 30; ++i) w.tick(1.0f / 30.0f);   // walk in, settle
        const Unit* p = w.unit(u);
        const float rx = p->x.toFloat(), rz = p->z.toFloat();
        const uint64_t asksAtRest = w.pathStats().requests();
        unsigned randomRetries = 0;
        w.setRngObserver([&](const World::RngObservation& draw) {
            if (draw.bound == 120) ++randomRetries;
        });
        for (int i = 0; i < 30 * 60; ++i) w.tick(1.0f / 30.0f);   // a minute at the wall
        const uint64_t probes = w.pathStats().requests() - asksAtRest;
        // This small failed search completes in one frame. A retained repeat
        // flag requests again without consuming a random-tail draw, while
        // the admission gate limits actual searches to two per second.
        check(probes == 120 && randomRetries == 0,
              "repeated refusal respects admission spacing without random delay",
              std::to_string(probes) + " asks in 60s");
        check(p && !p->orders.empty(),
              "...retains the navigator-only test goal",
              p ? std::to_string(p->orders.size()) + " orders" : "dead");
        const float drift = std::max(std::fabs(p->x.toFloat() - rx),
                                     std::fabs(p->z.toFloat() - rz));
        check(drift < 24.0f,
              "...and stands its ground between probes",
              "drifted " + std::to_string(int(drift)) + "px in 60s");
        delete &w;
    }

    // ---- 2. a long healthy march re-anchors on the same tail ----
    {
        World& w = *makeWorld(160, 160);
        UnitType wt = walkerType();
        const int u = w.spawn(&wt, 300, 300, 0, 0);
        for (int i = 0; i < 60; ++i) w.tick(1.0f / 30.0f);
        const uint64_t asks0 = w.pathStats().requests();
        w.order(u, 2300, 2300, false);   // ~2800px, ~58s at this speed
        for (int i = 0; i < 30 * 40; ++i) w.tick(1.0f / 30.0f);
        const uint64_t asks = w.pathStats().requests() - asks0;
        check(asks >= 3,
              "a long healthy march re-asks on the 1-in-120 tail",
              std::to_string(asks) + " asks in 40s");
        delete &w;
    }

    // Partial and long-detour outcomes suppress the plain retry draw itself.
    for (bool detour:{false,true}) {
        World& w=*makeWorld(160,160);UnitType wt=walkerType();
        const int id=w.spawn(&wt,300,300,0,0);
        w.order(id,2300,2300,false);
        w.unit(id)->orders.back().groundMission=false;
        for (int i=0;i<1000 && w.pathStats().pending(id);++i) w.tick(1.0f/30.0f);
        check(!w.pathStats().pending(id),"cadence fixture has a completed route");
        auto* u=w.unit(id);u->routeStamp=0;u->routeCrowded=u->routeTraffic=false;
        u->routeFailed=!detour;u->routeDetour=detour;
        unsigned draws=0;
        w.setRngObserver([&](const World::RngObservation& draw) { if (draw.bound==120) ++draws; });
        for (int i=0;i<150;++i) w.tick(1.0f/30.0f);
        check(draws==0,detour ? "long detour suppresses the plain retry draw" : "partial route suppresses the plain retry draw");
        delete &w;
    }

    std::printf(g_fail ? "cadence_test: %d FAILURE(S)\n" : "cadence_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
