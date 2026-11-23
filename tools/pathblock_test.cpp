// pathblock_test -- a unit must get PAST other units standing in its way.
//
// Reported as "they don't go around larger obstacles, they try their damndest to go
// over or through them first". Terrain was only half of it. The other half was other
// units, and it had two causes that had to be fixed together -- fixing either alone
// leaves the unit stuck, which is why this test exists rather than a comment.
//
// ONE: the SEARCH routed through them. cellScore grades a parked body kCellOccupied,
// which EQUALS kCellThreshold, so it is passable-but-costly -- retail's model, where
// every icd call site compares against 4. Our mover has no local avoidance and bodies
// are solid, so a route through a standing crowd is a route into a wall.
//
// TWO: the route SHORTCUT straightened it back through them. Once the search goes
// around, a shortcut that only checks terrain undoes the detour.
//
// Measured on the wall below before the fix: every thickness from 1 to 8 cells STUCK,
// and it stayed stuck with the shortcut disabled entirely -- which is how the first
// diagnosis (blame the shortcut) was shown to be wrong. After: every thickness passes,
// travelling 1262-1329 against a 1100 straight line.

#include "sim/sim.h"
#include "cartographer/blockers.h"
#include "tnt/tnt.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

using namespace tak::sim;

static int g_fail = 0;
static void check(bool cond, const std::string& what, const std::string& detail = {}) {
    std::printf("  %-56s %s%s%s\n", what.c_str(), cond ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!cond) ++g_fail;
}

static UnitType soldier() {
    UnitType t{};
    t.name = "sol"; t.id = "sol";
    t.maxVel = tak::sim::Fixed::fromFloat(70.0f / 30.0f); t.turnRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.footX = 2; t.footZ = 2;
    return t;
}

// A wall of PARKED bodies across the direct line, `thick` rows deep, with open ground
// round both ends. Returns the distance travelled, or -1 if the unit never arrived.
static float runWall(int thick) {
    const int W = 160, H = 160;
    World w;
    w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, /*seaLevel=*/20);
    w.setPathService(true);
    UnitType s = soldier();
    // Player 1's units, left standing still -- a neutral crowd, not a fight.
    for (int t = 0; t < thick; ++t)
        for (int k = 0; k < 40; ++k)
            w.spawn(&s, 600.0f + float(k) * 32.0f, 1000.0f + float(t) * 32.0f, 0, 1);
    for (auto& u : w.units()) u.speed = tak::sim::Fixed();
    const int id = w.spawn(&s, 900, 500, 0, 0);
    w.order(id, 900, 1600, /*queue=*/false);
    float px = w.unit(id)->x.toFloat(), pz = w.unit(id)->z.toFloat(), travelled = 0;
    for (int i = 0; i < 30 * 200; ++i) {
        w.tick(1.0f / 30.0f);
        const Unit* q = w.unit(id);
        travelled += std::sqrt((q->x.toFloat() - px) * (q->x.toFloat() - px) + (q->z.toFloat() - pz) * (q->z.toFloat() - pz));
        px = q->x.toFloat(); pz = q->z.toFloat();
        const float dx = q->x.toFloat() - 900.0f, dz = q->z.toFloat() - 1600.0f;
        if (std::sqrt(dx * dx + dz * dz) < 60.0f) return travelled;
    }
    return -1.0f;
}



// A wall of the game's hard-blocker marker (0xFFFC) painted with Cartographer's
// BLOCKED brush across a flat 96x64-cell map, through a TNT save and load:
// columns 40-41, every row, or rows 0-47 with a gap below. A 2x2 soldier is
// ordered from (320,160) to (960,160). `touched` reports a footprint on a wall
// cell (outside the gap); `travelled` is the distance to the goal, or -1.
struct BlockerRun {float travelled=-1;bool touched=false;};
static BlockerRun runBlockerWall(bool legion,bool gap) {
    const int W=96,H=64;
    tak::tnt::Map map;map.width=W;map.height=H;map.blocksX=W/2;map.blocksY=H/2;map.seaLevel=20;
    map.heights.assign(size_t(W)*H,100);map.features.assign(map.heights.size(),cart::kCellEmpty);
    map.tileKeys.assign(size_t(map.blocksX)*map.blocksY,0);map.tileCols.assign(map.tileKeys.size(),0);map.tileRows.assign(map.tileKeys.size(),0);
    cart::applyBlockers(map,40,0,41,gap?47:H-1,cart::BlockOp::Paint);
    const auto saved=tak::tnt::Map::load(map.save(),"blocker wall");
    World w;
    w.setTerrain(saved.heights,saved.width,saved.height,saved.seaLevel,&saved.features);
    w.setPathService(true);
    if (legion) w.setPathfindingMode(PathfindingMode::Legion);
    UnitType s=soldier();
    const int id=w.spawn(&s,320,160,0,0);
    w.order(id,960,160,false);
    BlockerRun r;float px=320,pz=160,travelled=0;
    for (int t=0;t<3000;++t) {
        w.tick(1.0f/30.0f);
        const Unit* u=w.unit(id);
        const float x=u->x.toFloat(),z=u->z.toFloat();
        travelled+=std::sqrt((x-px)*(x-px)+(z-pz)*(z-pz));px=x;pz=z;
        const int ox=footprintOrigin(u->x,2),oz=footprintOrigin(u->z,2);
        if (ox<=41 && ox+1>=40 && (!gap || oz<=47)) r.touched=true;
        if (std::sqrt((x-960)*(x-960)+(z-160)*(z-160))<60) {r.travelled=travelled;break;}
    }
    return r;
}

// A builder whose approach and site are blocked by another player's unit that
// never moves. Retail's mover bumps it, its step is refused, the navigator
// re-searches and, from the closest point, gets an empty route: the 0x200
// failed-approach event hands the order back to MobileBuild, which places
// (within reach) or drops it (out of reach). Legion used to hold the builder
// in front of the body for ever (the order never ended). Returns the tick
// the build order retired, or -1 if it never did within `limit`.
static int runBlockedApproach(bool legion,float blockerX,float blockerZ,int limit,bool verbose=false) {
    World w;
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    w.setPathService(true);
    if (legion) w.setPathfindingMode(PathfindingMode::Legion);
    UnitType builder=soldier(),blocker=soldier(),site=soldier();
    builder.id=builder.name="bld";builder.isBuilder=true;builder.buildDist=60;builder.turnInPlaceRate=1000;
    blocker.id=blocker.name="blk";
    site.id=site.name="site";site.maxVel=Fixed();site.buildTime=10000;site.buildCost=0;
    const int bid=w.spawn(&builder,160,512,0,0);
    // Ordered onto a clear site; the body walks in afterwards.
    w.queueBuild(bid,&site,512,512,false);
    if (w.unit(bid)->orders.empty()) return -2;
    w.spawn(&blocker,blockerX,blockerZ,0,1);
    for (int t=0;t<limit;++t) {
        w.tick(1.0f/30.0f);
        const Unit* b=w.unit(bid);
        if (verbose && t%30==0) std::printf("    t=%d pos=(%.1f,%.1f) orders=%zu site=%d\n",t,b->x.toFloat(),b->z.toFloat(),b->orders.size(),b->buildSiteId);
        if (b->orders.empty() || b->buildSiteId) return t;
    }
    return -1;
}

// MobileBuild placement at the site (retail 405824..405911). The builder
// approaches from the west and stands outside the 2x2 site at (512,512).
// `what`: 0 another player's unit stands on the site's east column and never
// moves; 1 the same, but it walks away after 100 ticks; 2 a non-reclaimable
// feature appears on the site. A move is queued behind the build.
struct PlacementRun {int firstFailure=-1,dropped=-1,built=-1,maxFailures=0;bool heldStill=true;};
static PlacementRun runSitePlacement(bool legion,int what,bool fly=false) {
    PlacementRun r;
    World w;
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    w.setPathService(true);
    if (legion) w.setPathfindingMode(PathfindingMode::Legion);
    FeatType rock;rock.reclaimable=false;
    w.setFeatureTypes({rock});
    UnitType builder=soldier(),blocker=soldier(),site=soldier();
    builder.id=builder.name="bld";builder.isBuilder=true;builder.buildDist=60;builder.turnInPlaceRate=1000;
    builder.accel=Fixed::fromFloat(0.1f);builder.brake=Fixed::fromFloat(0.5f);
    builder.canFly=fly;
    blocker.id=blocker.name="blk";
    site.id=site.name="site";site.maxVel=Fixed();site.buildTime=10000;site.buildCost=0;
    const int bid=w.spawn(&builder,160,512,0,0);
    w.queueBuild(bid,&site,512,512,false);
    w.order(bid,160,800,true);
    int kid=0;
    if (what==2) w.addFeature(4242,512,512,0,1,1,true,0);
    else kid=w.spawn(&blocker,528,512,0,1);
    Fixed sx,sz;
    for (int t=0;t<1200;++t) {
        if (what==1 && t==r.firstFailure+100 && r.firstFailure>=0) w.order(kid,900,900,false);
        w.tick(1.0f/30.0f);
        const Unit* b=w.unit(bid);
        if (b->buildSiteId) {r.built=t;break;}
        const bool building=!b->orders.empty() && b->orders.front().buildType;
        const int failures=building ? b->orders.front().placementFailures : 0;
        // Braking settles within a few ticks of the first refusal.
        if (failures>0 && r.firstFailure<0) r.firstFailure=t;
        if (failures>0 && t==r.firstFailure+10) {sx=b->x;sz=b->z;}
        if (failures>0 && t>r.firstFailure+10 && (b->x!=sx || b->z!=sz)) r.heldStill=false;
        r.maxFailures=std::max(r.maxFailures,failures);
        bool pending=false;
        for (const auto& o:b->orders) pending|=o.buildType!=nullptr;
        if (!pending) {r.dropped=t;break;}
    }
    return r;
}

// The player's own units in a build site step aside (engine deviation,
// user-requested: retail 405824..405911 only waits for them and drops the
// order after 31 tries). The builder approaches a 4x4 site at (512,512) from
// the west, standing next to it. `who`: 0 an idle unit of the builder's player, 1 one guarding the
// builder, 2 an idle one on Hold Position, each standing inside the site;
// 3 an own unit that walks onto the clear site after the order is given; 4
// a crowd: the guard and `extraGuards` more guards of the builder in the site, where
// each exit point must skip one another mover took or a body stands on.
// `built` is the tick the site went down; `guarding` whether the guard still
// guards the builder, `guardNear` whether it ended up back beside it.
struct ClearRun {int built=-1,dropped=-1,maxFailures=0;bool guarding=false,guardNear=false;};
static ClearRun runSiteClear(bool legion,int who,int extraGuards=0) {
    ClearRun r;
    World w;
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    w.setPathService(true);
    if (legion) w.setPathfindingMode(PathfindingMode::Legion);
    UnitType builder=soldier(),blocker=soldier(),site=soldier();
    builder.id=builder.name="bld";builder.isBuilder=true;builder.buildDist=60;builder.turnInPlaceRate=1000;
    blocker.id=blocker.name="blk";
    site.id=site.name="site";site.maxVel=Fixed();site.buildTime=10000;site.buildCost=0;
    site.footX=4;site.footZ=4;
    // The builder starts beside the site and the blocker within a guard's
    // 70 px of it, so a guard stands still on the site when the order comes.
    const int bid=w.spawn(&builder,430,512,0,0);
    const int kid=w.spawn(&blocker,495,who==3 ? 560 : 512,0,0);
    if (who==1 || who==4) w.guard(kid,bid,false);
    std::vector<int> extra;
    if (who==4)
        for (int e=0;e<extraGuards;++e) {   // body-disjoint cells of the site
            extra.push_back(w.spawn(&blocker,e==0 ? 496 : 528,e==2 ? 512 : 480,0,0));
            w.guard(extra.back(),bid,false);
        }
    if (who==2) w.unit(kid)->moveState=0;
    w.queueBuild(bid,&site,512,512,false);
    if (who==3) w.order(kid,495,512,false);   // walks on after the order
    for (int t=0;t<1500;++t) {
        w.tick(1.0f/30.0f);
        const Unit* b=w.unit(bid);
        if (r.built<0 && b->buildSiteId) r.built=t;
        if (!b->orders.empty() && b->orders.front().buildType)
            r.maxFailures=std::max(r.maxFailures,int(b->orders.front().placementFailures));
        if (std::getenv("SITECLEAR_VERBOSE") && t%20==0) {const Unit* k=w.unit(kid);
            std::printf("    t=%d b=(%.1f,%.1f) k=(%.1f,%.1f) korders=%zu fails=%d\n",t,b->x.toFloat(),b->z.toFloat(),
                k->x.toFloat(),k->z.toFloat(),k->orders.size(),b->orders.empty()?-1:b->orders.front().placementFailures);}
        bool pending=false;
        for (const auto& o:b->orders) pending|=o.buildType!=nullptr;
        if (r.built<0 && !pending) {r.dropped=t;break;}
    }
    const Unit* k=w.unit(kid);const Unit* b=w.unit(bid);
    for (const auto& o:k->orders) r.guarding|=o.guard && o.targetId==bid;
    const float dx=(k->x-b->x).toFloat(),dz=(k->z-b->z).toFloat();
    // Back on its guard ring (World::guardRing: from (max builder foot + guard footX + 2)
    // x 16 px, three cells deep), with a cell's slack for the footprint origins.
    const float outer=float((std::max(b->type->footX,b->type->footZ)+k->type->footX+2)*16+48+16);
    r.guardNear=dx*dx+dz*dz<=outer*outer;
    for (int e:extra) {
        bool guards=false;
        for (const auto& o:w.unit(e)->orders) guards|=o.guard && o.targetId==bid;
        r.guarding&=guards;
    }
    return r;
}

int main() {
    for (bool legion:{false,true}) {
        const std::string mode=legion?"Legion: ":"Retail: ";
        static const char* names[]={"an idle own unit","a unit guarding the builder","an own unit on Hold Position",
                                    "an own unit arriving","three guards of the builder"};
        for (int who=0;who<5;++who) {
            const auto run=runSiteClear(legion,who,2);
            check(run.built>=0 && run.dropped<0 && run.maxFailures>0,mode+names[who]+" on the site steps aside and the site goes down",
                  "built "+std::to_string(run.built)+", dropped "+std::to_string(run.dropped)+", failures "+std::to_string(run.maxFailures));
            if (who==1 || who==4)
                check(run.guarding && run.guardNear,mode+names[who]+": the guards keep their guard orders, the first back beside the builder");
        }
    }

    {
        World w;
        w.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
        w.setPathService(true);
        auto s=soldier();
        const int id=w.spawn(&s,320,640,0,0);
        w.patrol(id,960,640);
        bool outbound=false,returned=false,secondOutbound=false;
        for (int tick=0;tick<1500;++tick) {
            w.tick(1.f/30);
            const float x=w.unit(id)->x.toFloat();
            if (x>880) { if (returned) secondOutbound=true; outbound=true; }
            if (outbound && x<400) returned=true;
        }
        check(outbound && returned && secondOutbound,
              "ground patrol travels out, returns, and repeats");
    }
    std::printf("pathblock_test\n");
    // The first blocking body by ID wins, even when spatial cell order is
    // reversed. A mobile blocker returns 2 while a structure returns 0.
    for (bool structureFirst : {false,true}) {
        World w;
        w.setTerrain(std::vector<uint8_t>(32*32,100),32,32,20);
        UnitType mover=soldier(),mobile=soldier(),structure=soldier();
        mobile.footX=mobile.footZ=structure.footX=structure.footZ=1;
        structure.maxVel=Fixed();
        w.spawn(structureFirst ? &structure : &mobile,168,168);
        w.spawn(structureFirst ? &mobile : &structure,152,152);
        const int self=w.spawn(&mover,64,64);
        check(w.cellScore(&mover,10,10,self)==(structureFirst ? 0 : 2),
              "mixed blockers retain ascending-ID precedence over cell order");
    }
    // Retail's ground word +0 (tools/re/check_ground_owner.py replays random
    // event sequences against the original routines). A body placed over a
    // live holder leaves it the cell (5066f0): of two identical bodies stamped
    // 1 then 2, retail's 507d10 lets 1 step east and refuses 2.
    {
        auto flat=[](World& w) {
            w.setTerrain(std::vector<uint8_t>(32*32,100),32,32,20);
            w.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
        };
        UnitType body=soldier();
        {
            World w;flat(w);
            const int a=w.spawn(&body,96,96),b=w.spawn(&body,96,96);
            check(w.mobilePlacement(*w.unit(a),6,5,false) && !w.mobilePlacement(*w.unit(b),6,5,false),
                  "the first body stamped keeps an overlapped cell (5066f0/507d10)");
        }
        // A holder that leaves hands its cells to the bodies it refused
        // (506aa0 -> 5097f0 -> 5062d0): within one sector the newest link
        // first, here the later of two bodies stacked on it.
        {
            World w;flat(w);
            const int holder=w.spawn(&body,176,176),first=w.spawn(&body,176,176),second=w.spawn(&body,176,176);
            w.commitGroundStep(*w.unit(holder),Fixed::fromInt(32),Fixed());
            check(footprintOrigin(w.unit(holder)->x,2)==12 && w.mobilePlacement(*w.unit(second),10,10,false) &&
                  !w.mobilePlacement(*w.unit(first),10,10,false),
                  "a leaving holder's cell goes to the newest-linked refused body");
        }
        // Across sectors 5097f0 walks column by column: the body in sector
        // (0,1) claims the shared cell before the later-spawned one in (1,0).
        {
            World w;flat(w);
            const int holder=w.spawn(&body,128,128);   // cells 7-8 x 7-8
            const int west=w.spawn(&body,112,128);     // 6-7 x 7-8, sector (0,1)
            const int north=w.spawn(&body,128,112);    // 7-8 x 6-7, sector (1,0)
            w.commitGroundStep(*w.unit(holder),Fixed::fromInt(32),Fixed());
            check(footprintOrigin(w.unit(holder)->x,2)==9 && w.mobilePlacement(*w.unit(west),6,7,false) &&
                  !w.mobilePlacement(*w.unit(north),7,6,false),
                  "refused bodies are restamped in sector order");
        }
        // Search grades read the same holder: a parked mobile body stamped
        // before a structure over it grades the cell as the mobile (2).
        {
            World w;flat(w);
            UnitType mover=soldier(),mobile=soldier(),structure=soldier();
            mobile.footX=mobile.footZ=structure.footX=structure.footZ=1;
            structure.maxVel=Fixed();
            w.spawn(&mobile,168,168);
            w.spawn(&structure,168,168);
            const int self=w.spawn(&mover,64,64);
            check(w.cellScore(&mover,10,10,self)==2,"search grades take the ground word's holder");
        }
    }
    // A plane's first build takes box minima instead of rating every cell's
    // footprint and ring one by one. It must give the per-cell reference's
    // cells and checksum exactly: slopes, water, features, edges, structures,
    // gates and mobile bodies, for square and long footprints.
    for (auto [fx,fz] : {std::pair{1,1},{2,2},{3,3},{2,6},{6,2},{4,4}}) {
        World w;w.setPathService(true);
        std::vector<uint8_t> heights(size_t(64)*64,100);
        for (int z=0;z<64;++z) for (int x=0;x<64;++x) {
            if (x>=40 && x<46) heights[size_t(z)*64+x]=uint8_t(x%2 ? 120 : 100);   // rough: graded 4
            if (z>=50 && x<20) heights[size_t(z)*64+x]=uint8_t(z>=55 ? 5 : 15);  // shore and deep water
        }
        w.setTerrain(heights,64,64,20);
        w.setMapPlacementFeatures(std::vector<uint16_t>(size_t(64)*64,0xffff),{});
        int feature=1;
        for (auto [x,z,size,blocks] : {std::tuple{100,100,1,true},{300,120,2,true},{500,80,1,false},
                                       {8,8,1,true},{1000,1000,3,true},{200,700,2,false}})
            w.addFeature(feature++,float(x),float(z),0,size,size,blocks);
        w.blockCells(30,10,2,12,true);
        UnitType body=soldier(),big=soldier(),house=soldier(),gate=soldier(),mover=soldier();
        body.footX=body.footZ=1;big.footX=3;big.footZ=2;
        house.footX=house.footZ=2;house.maxVel=Fixed();
        gate.footX=3;gate.footZ=1;gate.maxVel=Fixed();gate.gate=true;gate.yardMap="oco";
        mover.footX=fx;mover.footZ=fz;
        for (auto [x,z] : {std::pair{200,200},{216,200},{24,500},{1010,300},{600,1010},{8,1016}})
            w.spawn(&body,float(x),float(z),0,0);
        w.spawn(&big,420.f,260.f,0,1);
        w.spawn(&house,520.f,520.f,0,0);
        w.spawn(&gate,328.f,600.f,0,0);
        // A yard with open cells, and bodies stacked on structures in both
        // orders: the cell keeps the body stamped first, whatever its grade.
        UnitType yard=house;yard.yardMap="o..o";
        w.spawn(&yard,712.f,520.f,0,0);
        w.spawn(&body,712.f,520.f,0,0);
        w.spawn(&body,808.f,424.f,0,0);
        w.spawn(&house,808.f,424.f,0,0);
        const int id=w.spawn(&mover,160,160,0,0);
        for (int tick=0;tick<14;++tick) w.tick(1.f/30.f);
        w.order(id,900,900,false);
        for (int tick=0;tick<4;++tick) w.tick(1.f/30.f);
        check(w.pathStats().completions()+w.pathStats().failures()>0 && w.searchGradeBuildMatchesReference(),
              "box-minimum plane build matches the per-cell build "+std::to_string(fx)+"x"+std::to_string(fz));
        // A window refresh (a unit's step, a structure, an overlay edit) takes
        // box minima over the cells its footprints and rings read. Every window
        // position, inside, across and off each map edge, must give the
        // per-cell refresh's cells and checksum.
        bool windows=true;
        for (auto [ww,wh] : {std::pair{1,1},{2,2},{3,1},{1,4},{6,2},{9,7},{64,64}})
            for (int z=-8;z<=66 && windows;z+=ww==64 ? 37 : 3)
                for (int x=-8;x<=66 && windows;x+=ww==64 ? 37 : 3)
                    windows=w.searchGradeRefreshMatchesReference(x,z,ww,wh);
        check(windows,"box-minimum window refresh matches the per-cell refresh "+std::to_string(fx)+"x"+std::to_string(fz));
    }
    // Stress flyers exposed this through a landed body below the map. Aging
    // its clipped footprint read unrelated heap bytes into the cached checksum.
    for (auto [bx,bz] : {std::pair{-5,32},{69,32},{32,-5},{32,69},
                        {-1,32},{64,32},{32,-1},{32,64}}) {
        World w;w.setPathService(true);
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType body=soldier(),mover=soldier();mover.footX=mover.footZ=4;
        w.spawn(&body,float(bx*16+16),float(bz*16+16),0,0);
        const int id=w.spawn(&mover,160,160,0,0);
        for (int tick=0;tick<14;++tick) w.tick(1.f/30.f);
        w.order(id,640,640,false);
        bool consistent=true;
        for (int tick=0;tick<60;++tick) {
            w.tick(1.f/30.f);
            consistent &= w.searchGradeChecksumsValid();
        }
        check(w.pathStats().completions()+w.pathStats().failures()>0,
              "off-map body fixture exercises the search cache");
        check(consistent,"off-map footprint aging preserves the grade checksum invariant");
    }
    // Native move-command validation/submission preserves blocked destinations.
    // A nearest-walkable-cell scan changes both the active and queued missions.
    for (bool boat : {false,true}) for (auto [fx,fz] : {std::pair{1,1},{2,2},{2,6},{6,2}}) {
        World w;w.setPathService(true);
        w.setTerrain(std::vector<uint8_t>(32*32,0),32,32,boat?32:0);
        UnitType type=soldier();type.footX=fx;type.footZ=fz;
        type.floater=boat;type.minWaterDepth=boat?13:-10000;type.maxWaterDepth=boat?10000:20;
        type.domain=boat?UnitType::Domain::Water:UnitType::Domain::Ground;
        const int id=w.spawn(&type,6*16+fx*8,6*16+fz*8);
        w.blockCells(12,12,12,12,true);
        w.order(id,270.25f,340.5f,false);
        w.order(id,290.5f,330.25f,true);
        const auto& orders=w.unit(id)->orders;
        check(orders.size()==2 && orders[0].missionTarget==std::pair{Fixed::fromFloat(270.25f),Fixed::fromFloat(340.5f)} &&
              orders[1].x==Fixed::fromFloat(290.5f) &&
              orders[1].z==Fixed::fromFloat(330.25f) && w.pathStats().pending(id),
              "blocked active/queued destinations retained",
              std::string(boat?"boat ":"land ")+std::to_string(fx)+"x"+std::to_string(fz));
        check(orders[0].x==footprintWaypoint(footprintCell(270.25f,fx),fx) &&
              orders[0].z==footprintWaypoint(footprintCell(340.5f,fz),fz),
              "initial steering endpoint uses independent footprint axes");
    }
    {
        World w;
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        w.setPathService(true);
        UnitType builder=soldier(), site=soldier();
        builder.isBuilder=true;
        builder.turnInPlaceRate=1000;
        site.maxVel=Fixed(); site.footX=site.footZ=2;
        site.buildTime=10000;
        for (int tick=0;tick<14;++tick) w.tick(1.0f/30.0f);
        const int id=w.spawn(&builder,128,128);
        w.queueBuild(id,&site,31*16,9*16,false);
        const Order approach=w.unit(id)->orders.front();
        check(approach.buildRectangle==RetailRectGoal{28,32,6,10},
              "normal build commands use the verified rectangular approach bounds");
        w.tick(1.0f/30.0f);
        check(w.units().size()==1 && !w.unit(id)->buildSiteId,
              "rectangular builder approach does not create a site before arrival");
        check(w.pathStats().completions()>0 && w.unit(id)->orders.back().buildType==&site &&
              w.unit(id)->orders.back().buildRectangle==approach.buildRectangle,
              "route delivery retains the owning builder order and rectangular goal");
        auto* u=w.unit(id);
        u->x=Fixed::fromInt(29*16); u->z=Fixed::fromInt(8*16);
        u->heading=Bam(0);
        const auto x=u->x,z=u->z;
        w.tick(1.0f/30.0f);
        check(w.units().size()==1 && u->x==x && u->z==z && (u->orders.back().mission.pending&0x100),
              "rectangle arrival holds position and signals its builder mission");
        w.tick(1.0f/30.0f);
        check(w.unit(id)->x==x && w.unit(id)->z==z,
              "deferred builder placement cannot add a movement step into the site");
        check(w.unit(id)->buildSiteId && w.unit(id)->heading==Bam(1000),
              "builder begins its pivot on the placement dispatch");
        w.tick(1.0f/30.0f);
        check(w.unit(id)->heading==Bam(2000) && w.unit(id)->x==x && w.unit(id)->z==z,
              "working builder turns exactly once and skips the ordinary mover");
    }
    {
        World w;
        w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        w.setPathService(true);
        UnitType builder=soldier(),site=soldier();
        builder.isBuilder=true; builder.workerTime=1000;
        site.maxVel=Fixed(); site.buildTime=1; site.buildCost=0;
        const int id=w.spawn(&builder,128,128);
        w.queueBuild(id,&site,320,128,false);
        w.queueBuild(id,&site,128,320,true);
        w.tick(1.0f/30.0f);
        check(w.units().size()==1 && w.unit(id)->orders.back().mission.stage==0,
              "queued build remains inactive while the builder approaches its predecessor");
        for (int t=0;t<1200 && (w.units().size()<3 || w.unit(id)->buildSiteId);++t)
            w.tick(1.0f/30.0f);
        check(w.units().size()==3 && !w.unit(id)->buildSiteId && w.unit(id)->orders.empty(),
              "builder completes successive rectangular approaches in queue order");
    }
    // Replacing a movement order must retire its in-flight search. Otherwise
    // delivery rewrites the replacement command as a route to the old goal.
    for (bool ambush : {false, true}) {
        World w;
        w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        w.setPathService(true);
        UnitType s = soldier();
        const int id = w.spawn(&s, 128, 128);
        w.order(id, 800, 128, false);
        check(w.pathStats().pending(id), "move has an outstanding search before replacement");
        if (ambush) w.orderWaitAttack(id, true);
        else w.orderWait(id, 10, true);
        check(w.pathStats().pending(id), "queued wait preserves current movement search");
        if (ambush) w.orderWaitAttack(id, false);
        else w.orderWait(id, 10, false);
        // A later queued destination must not be absorbed by stale delivery.
        w.order(id, 128, 800, true);
        check(!w.pathStats().pending(id),
              ambush ? "ambush replacement cancels old movement search"
                     : "wait replacement cancels old movement search");
        for (int tick = 0; tick < 30; ++tick) w.tick(1.0f / 30.0f);
        const Unit& u = *w.unit(id);
        check(u.x == Fixed::fromInt(128) && u.z == Fixed::fromInt(128) &&
                  u.orders.size() == 2 &&
                  (ambush ? u.orders.front().waitAttack : u.orders.front().wait == 270) &&
                  u.orders.back().x == Fixed::fromInt(128) &&
                  u.orders.back().z == Fixed::fromInt(800),
              ambush ? "stale delivery cannot erase ambush and queued move"
                     : "stale delivery cannot erase wait and queued move");
    }
    {
        World w;
        w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        w.setPathService(true);
        w.setPathBudget(1); // A blocked goal must survive an unfinished search.
        w.blockCells(16, 0, 2, 64, true);
        UnitType s = soldier();
        const int id = w.spawn(&s, 128, 128);
        w.order(id, 800, 128, false);
        w.order(id, 128, 800, true);
        w.unit(id)->speed = Fixed();
        w.unit(id)->stuckFor = 30;
        w.unit(id)->stuckX = w.unit(id)->x;
        w.unit(id)->stuckZ = w.unit(id)->z;
        w.tick(1.0f / 30.0f);
        const Unit& u = *w.unit(id);
        check(u.orders.size() == 2 && u.orders.front().x == Fixed::fromInt(800) &&
                  u.orders.front().z == Fixed::fromInt(128) &&
                  u.orders.back().x == Fixed::fromInt(128) &&
                  u.orders.back().z == Fixed::fromInt(800),
              "blocked goal and queued move survive unfinished search");
        check(w.pathStats().pending(id), "blocked goal retains ownership of pending search");
        w.tick(1.0f / 30.0f);
        check(w.pathStats().pending(id) && u.orders.size() == 2 &&
                  u.orders.front().x == Fixed::fromInt(800),
              "blocked movement does not silently advance to queued command");
    }
    // Captured stage-2 failure must cancel before the search can deliver,
    // reanchoring the direct navigator segment and retaining the queued goal.
    {
        World w;
        w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        w.setPathService(true);
        UnitType s = soldier();
        const int id = w.spawn(&s, 128, 128);
        w.order(id, 800, 128, false);
        w.order(id, 128, 800, true);
        auto& u = *w.unit(id);
        auto& goal = u.orders.front();
        goal.mission.stage = 2;
        goal.mission.waitMask = 0x2701;
        goal.mission.pending = 0x2000;
        goal.mission.deadline = 100;
        u.routeStamp = 0;
        const auto controller = goal.controller;
        goal.segmentX=Fixed::fromInt(32);
        const auto anchor = Fixed::fromInt(u.x.floorInt());
        w.tick(1.0f / 30.0f);
        check(w.pathStats().pending(id) && w.pathStats().completions() == 0,
              "ground failure replaces the search while retaining the admission delay");
        check(u.orders.size() == 2 && u.orders.front().segmentX == anchor &&
              u.orders.front().controller != controller &&
              u.orders.front().missionRadius == 64 && u.orders.front().mission.stage == 2 &&
              u.orders.front().mission.waitMask == 0x2701 &&
              u.orders.front().mission.deadline >= 6 && u.orders.front().mission.deadline <= 10,
              "ground reset reanchors direct segment, preserves queue and schedules polling");
        const auto hash = w.stateHash();
        ++u.orders.front().mission.deadline;
        check(hash != w.stateHash(), "ground dispatcher state participates in lockstep hash");
    }
    {
        World w;
        w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        w.setPathService(true);
        UnitType s = soldier();
        const int id = w.spawn(&s, 128, 128);
        w.order(id, 128, 128, false);
        w.order(id, 800, 128, true);
        w.unit(id)->speed=s.brake*Fixed::fromInt(2);
        w.tick(1.0f / 30.0f);
        check(w.unit(id)->orders.front().mission.pending & 0x100,
              "ground arrival reports an event to its owning mission");
        check(w.unit(id)->speed==s.brake && w.unit(id)->x==Fixed::fromInt(128),
              "ground arrival applies inactive braking without another movement step");
        w.tick(1.0f / 30.0f);
        const auto& u = *w.unit(id);
        check(!u.orders.empty() && u.orders.back().x == Fixed::fromInt(800) &&
              u.orders.back().mission.stage == 2,
              "arrival dispatch advances and initializes the queued ground goal");
    }
    {
        World w;
        w.setTerrain(std::vector<uint8_t>(20 * 20, 100), 20, 20, 20);
        w.setPathService(true);
        // The smallest budget that still runs: every Retail player is in the
        // 5x budget class (0x4f6379), so a budget below 5 has a zero quantum.
        w.setPathBudget(5);
        w.blockCells(10, 0, 1, 20, true);
        UnitType s = soldier();
        // Failure notification requires knowing that the wall cuts off the goal.
        s.sight = 1024; s.upright = true;
        s.footX = s.footZ = 1;
        s.maxVel = Fixed::fromFloat(0.001f);
        const int id = w.spawn(&s, 24, 168);
        w.order(id, 296, 168, false);
        auto& u = *w.unit(id);
        u.orders.front().mission = {2, 0x2701, 0xffffffffu, 0, 0};
        u.routeStamp = 0;
        // Keep the separate terrain watchdog out of this dispatcher fixture.
        u.stuckFor = -10000;
        u.stuckX = u.x; u.stuckZ = u.z;
        const auto controller = u.orders.front().controller;
        bool notified = false;
        for (int tick = 0; tick < 1000 && !u.orders.empty(); ++tick) {
            w.tick(1.0f / 30.0f);
            if (!u.orders.empty() && (u.orders.back().mission.pending & 0x2000)) {
                notified = true;
                break;
            }
        }
        check(notified && w.pathStats().pending(id) && w.pathStats().failures() == 0,
              "World receives early failure while cost search remains unfinished");
        w.tick(1.0f / 30.0f);
        check(!u.orders.empty() && u.orders.back().controller != controller &&
              u.orders.back().missionRadius == 32 && w.pathStats().pending(id) &&
              w.pathStats().failures() == 0,
              "next World dispatch replaces early-notifying search before stale delivery");
    }
    // A newly completed route becomes visible after this tick's movement.
    // Compare with the same mover whose background search is disabled.
    {
        World searched, direct;
        UnitType s = soldier();
        for (World* w : {&searched, &direct}) {
            w->setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
            w->blockCells(20, 0, 2, 32, true);
            for (int tick=0;tick<14;++tick) w->tick(1.0f/30.0f);
            const int id = w->spawn(&s, 135.5f, 128);
            w->setPathService(w == &searched);
            w->order(id, 640, 384, false);
            w->unit(id)->heading = Bam(8192);
            w->unit(id)->speed = s.maxVel;
            w->tick(1.0f / 30.0f);
        }
        const auto& a = searched.units().front();
        const auto& b = direct.units().front();
        check(searched.pathStats().completions() + searched.pathStats().failures() > 0,
              "timing fixture completes its search on the first eligible tick");
        check(a.groundScanTick==0 && b.groundScanTick>0,
              "route delivery invalidates the next local obstacle scan");
        check(a.x == b.x && a.z == b.z && a.heading == b.heading && a.speed == b.speed,
              "route completion does not change movement earlier in the same tick");
        check(footprintCell(a.x, 2) != footprintCell(Fixed::fromFloat(135.5f), 2) &&
              !a.orders.empty() &&
              a.orders.front().segmentX == footprintWaypoint(footprintCell(a.x, 2), 2) &&
              a.orders.front().segmentZ == footprintWaypoint(footprintCell(a.z, 2), 2),
              "new search anchors its route in the cell reached by this tick's move");
    }
    // Repeated destination clicks while a 2x2 group passes terrain. Check every
    // committed footprint, including moments when an in-flight route is replaced.
    {
        World w;
        w.setTerrain(std::vector<uint8_t>(80 * 80, 100), 80, 80, 20);
        w.setPathService(true);
        UnitType s = soldier();
        w.blockCells(24, 12, 5, 24, true);
        std::vector<int> group;
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
                group.push_back(w.spawn(&s, 128 + 48 * i, 272 + 48 * j));
        bool overlap = false, terrain = false;
        std::vector<uint32_t> radii(group.size(), 0);
        for (int tick = 0; tick < 3600; ++tick) {
            if (tick == 0 || tick == 300 || tick == 600)
                for (size_t i = 0; i < group.size(); ++i)
                    w.order(group[i], 800 + 48 * int(i % 4),
                            (tick == 300 ? 720 : 320) + 48 * int(i / 4), false);
            w.tick(1.0f / 30.0f);
            for (size_t a = 0; a < group.size(); ++a) {
                const Unit& u = *w.unit(group[a]);
                for (const auto& goal : u.orders)
                    if (goal.groundMission) { radii[a] = goal.missionRadius; break; }
                const int x = footprintOrigin(u.x, 2), z = footprintOrigin(u.z, 2);
                terrain |= !w.nav().fits(x + 1, z + 1, 2);
                for (size_t b = a + 1; b < group.size(); ++b) {
                    const Unit& v = *w.unit(group[b]);
                    overlap |= std::abs(x - footprintOrigin(v.x, 2)) < 2 &&
                               std::abs(z - footprintOrigin(v.z, 2)) < 2;
                }
            }
        }
        int arrived = 0;
        for (size_t i = 0; i < group.size(); ++i) {
            const Unit& u = *w.unit(group[i]);
            float dx = u.x.toFloat() - (800 + 48 * int(i % 4));
            float dz = u.z.toFloat() - (320 + 48 * int(i / 4));
            // Mission failure grows the accepted cell circle. Include one
            // cell for the difference between cell origin and subcell position.
            const float accepted = float(radii[i] + 4) + 23.0f;
            arrived += u.orders.empty() && dx * dx + dz * dz <= accepted * accepted;
            if (!u.orders.empty() || dx * dx + dz * dz > accepted * accepted) {
                std::printf("    unfinished id=%d pos=(%.3f,%.3f) speed=%.3f mode=%u orders=%zu\n",
                            u.id, u.x.toFloat(), u.z.toFloat(), u.speed.toFloat(),
                            unsigned(u.groundMovementMode), u.orders.size());
                for (const auto& o : u.orders)
                    std::printf("      point=(%.3f,%.3f) segment=(%.3f,%.3f) goal=%d\n",
                                o.x.toFloat(), o.z.toFloat(), o.segmentX.toFloat(), o.segmentZ.toFloat(), int(o.goal));
            }
        }
        check(!overlap && !terrain, "repeated group orders never overlap bodies or terrain");
        check(arrived == 16, "all 16 units complete within their mission radius after repeated clicks",
              std::to_string(arrived) + "/16");
    }
    // A nearby goal still needs a route when the intervening cell is blocked.
    // The old <3-cell request shortcut cancelled every retry, forever.
    for (bool body : {false, true}) {
        World w;
        w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        w.setPathService(true);
        UnitType s = soldier();
        s.footX = s.footZ = 1;
        if (body) w.spawn(&s, 168, 168, 0, 1);
        else w.blockCells(10, 10, 1, 1, true);
        const int id = w.spawn(&s, 152, 168, 0, 0);
        w.order(id, 184, 168, false);
        for (int i = 0; i < 30 * 30; ++i) w.tick(1.0f / 30.0f);
        const Unit* u = w.unit(id);
        const float dx = u->x.toFloat() - 184, dz = u->z.toFloat() - 168;
        check(dx * dx + dz * dz < 16 * 16 && u->orders.empty(),
              body ? "short move detours around a parked unit"
                   : "short move detours around terrain",
              "x=" + std::to_string(u->x.toFloat()));
    }

    // The leader is pressing into a wall at less than 75% of cruising speed.
    // Being slightly faster than its follower does not make it passable traffic.
    // Exercise the actual mover as well as the route scorer.
    {
        World w;
        w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        UnitType s = soldier();
        s.footX = s.footZ = 1;
        s.maxVel = Fixed::fromInt(2);
        w.blockCells(11, 10, 1, 1, true);
        int leader = w.spawn(&s, 175.5f, 168, 0, 0);
        int follower = w.spawn(&s, 159.5f, 168, 0, 0);
        w.order(leader, 400, 168, false);
        w.order(follower, 400, 168, false);
        w.unit(leader)->heading = w.unit(follower)->heading = Bam(16384);
        w.unit(leader)->speed = Fixed::fromFloat(0.9f);
        w.unit(follower)->speed = Fixed::fromFloat(0.8f);
        w.tick(1.0f / 30.0f);
        check(w.unit(follower)->x < Fixed::fromInt(160),
              "a slowed follower cannot step into a stalled leader");
        check(w.cellScore(&s, 10, 10, follower) == kCellUnitBlocked,
              "the route scorer also refuses stalled same-way traffic");
        w.unit(leader)->speed = w.unit(follower)->baseSpeed * Fixed::raw(0xc000);
        check(w.cellScore(&s, 10, 10, follower) == kCellGround,
              "traffic at the retail 75% speed floor preserves the terrain grade");
        w.unit(leader)->heading = retailHeadingToPort(65535);
        w.unit(follower)->heading = retailHeadingToPort(0);
        check(w.cellScore(&s, 10, 10, follower) == kCellUnitBlocked,
              "traffic heading comparison preserves retail's unsigned wrap boundary");
    }

    // Two movers attempt to claim the same empty cell during one tick. A
    // snapshot-only occupancy grid lets both succeed even with hard collision.
    {
        World w;
        w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        UnitType s = soldier();
        s.footX = s.footZ = 1;
        s.maxVel = Fixed::fromInt(2);
        const int a = w.spawn(&s, 159, 168, 0, 0);
        const int b = w.spawn(&s, 177, 168, 0, 0);
        w.order(a, 400, 168, false);
        w.order(b, 100, 168, false);
        w.unit(a)->heading = Bam(16384);
        w.unit(b)->heading = Bam(49152);
        w.unit(a)->speed = w.unit(b)->speed = s.maxVel;
        w.tick(1.0f / 30.0f);
        check(w.unit(a)->x.floorInt() / 16 != w.unit(b)->x.floorInt() / 16,
              "two movers cannot claim the same cell in one tick");
    }

    // A prospective step must reserve the entire body, even when the centre
    // itself is clear. The route scorer must see that same blocked footprint.
    {
        World w;
        w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        UnitType s = soldier();
        s.maxVel = Fixed::fromInt(2);
        w.blockCells(11, 9, 1, 1, true);
        const int id = w.spawn(&s, 167, 160, 0, 0);
        w.order(id, 400, 160, false);
        w.unit(id)->heading = Bam(16384);
        w.unit(id)->speed = s.maxVel;
        w.tick(1.0f / 30.0f);
        check(w.unit(id)->x >= Fixed::fromInt(167) && w.unit(id)->x < Fixed::fromInt(168),
              "terrain under the footprint edge blocks the step");
        check(w.cellScore(&s, 11, 10, id) < kCellThreshold,
              "the route scorer also sees terrain under the footprint edge");
        w.tick(1.0f / 30.0f);
        check(w.unit(id)->bodyBlockStreak == 2 &&
                  w.unit(id)->speed <= Fixed::fromFloat(w.unit(id)->baseSpeed.toFloat() * 0.2f),
              "terrain refusals advance the same slowdown state as bodies");
    }

    for (bool guard : {false, true}) {
        World w;
        w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        w.setPathService(true);
        UnitType s = soldier();
        s.footX = s.footZ = 1;
        s.weapon.damage = 1;
        s.weapon.range = 24;
        s.weapons = {s.weapon};
        UnitType victim = s;
        victim.weapon.damage = 0;
        victim.weapons.clear();
        victim.maxHp = 10000;
        w.blockCells(10, 5, 1, 11, true);
        const int target = w.spawn(&victim, 408, 168, 0, guard ? 0 : 1);
        const int id = w.spawn(&s, 104, 168, 0, 0);
        if (guard) w.guard(id, target, false);
        else w.attack(id, target, false);
        w.order(id, 408, 408, true);
        for (int i = 0; i < 30 * 45; ++i) w.tick(1.0f / 30.0f);
        const Unit* u = w.unit(id);
        const float dx = u->x.toFloat() - 408, dz = u->z.toFloat() - 168;
        // A guard stands on Follow_Ground's ring (1x1 guard and charge:
        // 64..112 px, give or take a cell's rounding); an attacker in range.
        const float d2 = dx * dx + dz * dz;
        check(guard ? d2 >= 48 * 48 && d2 <= 128 * 128 : d2 < 70 * 70,
              guard ? "guard routes around terrain to its target"
                    : "attack routes around terrain to its target");
        check(!u->orders.empty() && u->orders.front().targetId == target &&
                  u->orders.back().z == Fixed::fromInt(408),
              "routing preserves the target and queued move");
        w.unit(target)->hp = Fixed();
        for (int i = 0; i < 30 * 30; ++i) w.tick(1.0f / 30.0f);
        u = w.unit(id);
        const float qx = u->x.toFloat() - 408, qz = u->z.toFloat() - 408;
        check(qx * qx + qz * qz < 16 * 16 && u->orders.empty(),
              "losing the target retires the route and resumes the queued move");
    }

    std::printf("a unit routes around a wall of parked bodies:\n");
    for (int thick = 1; thick <= 8; ++thick) {
        const float travelled = runWall(thick);
        check(travelled > 0,
              "wall " + std::to_string(thick) + " rows deep is passed",
              travelled > 0 ? std::to_string(int(travelled)) + "px travelled"
                            : "STUCK against it");
        // ...and it goes AROUND rather than wandering: the detour is bounded. The
        // straight line is 1100px; a sane way round the end of the wall is well under
        // three times that. Before the fix a stuck unit racked up 2515-3636px going
        // nowhere, so this also catches "arrives eventually, by accident".
        if (travelled > 0)
            check(travelled < 3300.0f,
                  "  and does not wander to get there",
                  std::to_string(int(travelled)) + "px for a 1100px straight line");
    }

    // The goal itself being occupied must still work: ordering a unit to where
    // something already stands is ordinary, and the search's goal exemption is what
    // keeps that from failing to route at all.
    std::printf("the goal can be occupied:\n");
    {
        const int W = 120, H = 120;
        World w;
        w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
        w.setPathService(true);
        UnitType s = soldier();
        for (int k = 0; k < 6; ++k) w.spawn(&s, 1200.0f + float(k) * 24.0f, 1200.0f, 0, 1);
        for (auto& u : w.units()) u.speed = tak::sim::Fixed();
        const int id = w.spawn(&s, 400, 400, 0, 0);
        w.order(id, 1200, 1200, false);
        bool close = false;
        for (int i = 0; i < 30 * 120 && !close; ++i) {
            w.tick(1.0f / 30.0f);
            const Unit* q = w.unit(id);
            const float dx = q->x.toFloat() - 1200.0f, dz = q->z.toFloat() - 1200.0f;
            close = std::sqrt(dx * dx + dz * dz) < 140.0f;
        }
        check(close, "a unit still reaches a destination others are standing on");
    }

    for (bool legion:{false,true}) {
        const bool verbose=std::getenv("PATHBLOCK_VERBOSE")!=nullptr;
        const int done=runBlockedApproach(legion,496,512,1800,verbose);
        check(done>=0,std::string(legion?"Legion":"Retail")+": a builder blocked by a still body ends its approach",
              "retired at tick "+std::to_string(done));
    }

    for (bool legion:{false,true}) {
        const std::string mode=legion?"Legion: ":"Retail: ";
        const auto wait=runSitePlacement(legion,0);
        check(wait.firstFailure>=0 && wait.dropped>=0 && wait.maxFailures==30 && wait.heldStill &&
              wait.dropped-wait.firstFailure>=295 && wait.dropped-wait.firstFailure<=305,
              mode+"a unit on the site: retry every 10 ticks, drop after 31",
              "first failure "+std::to_string(wait.firstFailure)+", dropped "+std::to_string(wait.dropped)+
              ", failures "+std::to_string(wait.maxFailures));
        const auto clears=runSitePlacement(legion,1);
        check(clears.firstFailure>=0 && clears.built>=0 && clears.dropped<0 &&
              clears.built-clears.firstFailure>100 && clears.built-clears.firstFailure<300,
              mode+"the site clears during the wait: the builder places",
              "first failure "+std::to_string(clears.firstFailure)+", built "+std::to_string(clears.built));
        const auto rock=runSitePlacement(legion,2);
        check(rock.dropped>=0 && rock.maxFailures==0 && rock.built<0,
              mode+"a non-reclaimable feature on the site: drop at once",
              "dropped "+std::to_string(rock.dropped)+", failures "+std::to_string(rock.maxFailures));
    }

    {
        // FlyingBuild (41f301..41f3e5) runs the same placement sequence; the
        // flyer hovers where it arrived for the retries.
        const auto wait=runSitePlacement(false,0,true);
        check(wait.firstFailure>=0 && wait.dropped>=0 && wait.maxFailures==30 && wait.heldStill &&
              wait.dropped-wait.firstFailure>=295 && wait.dropped-wait.firstFailure<=305,
              "Flyer: a unit on the site: hover, retry every 10 ticks, drop after 31",
              "first failure "+std::to_string(wait.firstFailure)+", dropped "+std::to_string(wait.dropped)+
              ", failures "+std::to_string(wait.maxFailures)+", held "+std::to_string(wait.heldStill));
        const auto rock=runSitePlacement(false,2,true);
        check(rock.dropped>=0 && rock.maxFailures==0 && rock.built<0,
              "Flyer: a non-reclaimable feature on the site: drop at once",
              "dropped "+std::to_string(rock.dropped)+", failures "+std::to_string(rock.maxFailures));
    }

    // What Cartographer's BLOCKED brush paints is what the game blocks: no unit
    // enters a painted wall in either pathfinding mode, and with a gap left in it
    // they go round through the gap. The straight line is 640px; through the gap
    // it is about 1400px.
    std::printf("a wall painted with Cartographer's BLOCKED brush:\n");
    for (bool legion:{false,true}) {
        const std::string mode=legion?"Legion: ":"Retail: ";
        const auto closed=runBlockerWall(legion,false);
        check(!closed.touched && closed.travelled<0,mode+"no unit enters a closed blocker wall");
        const auto open=runBlockerWall(legion,true);
        check(open.travelled>1200 && !open.touched,mode+"a blocker wall with a gap is passed through the gap",
              open.travelled>0 ? std::to_string(int(open.travelled))+"px travelled"+(open.touched?", crossed the wall":"") : "never arrived");
    }

    std::printf(g_fail ? "pathblock_test: %d FAILURE(S)\n" : "pathblock_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
