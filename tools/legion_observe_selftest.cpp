// legion_observe_selftest -- the observer (tools/legion_observe.h) on hand-
// placed bodies with known answers. Nothing here ticks the world: each case
// spawns units on an open flat map, then writes their positions, headings,
// speeds and orders directly, tick by tick, and samples the observer. Every
// expected number is worked out in the comment beside it.
//
// The last case runs the crossings check with the selftest mutation on
// (Config::mutation = 1: the flip test turns strict), and passes only if that
// check FAILS -- a gate that cannot fail proves nothing.
#include "legion_observe.h"
#include "sim/sim.h"

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace tak::sim;
namespace obs=tak::legion_observe;

namespace {
int failures=0;

UnitType mover(int foot) {
    UnitType t{};t.id=t.name="observe-foot-"+std::to_string(foot);
    t.canMove=true;t.maxHp=100;t.footX=t.footZ=foot;t.sight=4096;t.maxVel=Fixed::raw(117964);   // 1.8 px/tick
    t.accel=t.brake=Fixed::fromInt(10);t.turnRate=t.turnInPlaceRate=2500;t.halfCellTicks=3;t.buildTime=1;
    return t;
}
UnitType flyer() {
    UnitType t=mover(2);t.id=t.name="observe-flyer";t.canFly=true;t.maxVel=Fixed::fromInt(4);t.cruiseAlt=80;
    return t;
}

struct Scene {
    World world;
    int W,H;
    explicit Scene(int w=96,int h=96):W(w),H(h) {
        world.setGameSeed(7);world.setSerialThreads(true);
        world.setPathfindingMode(PathfindingMode::Retail);
        world.setPlayerCount(2);world.setTeam(0,0);world.setTeam(1,1);
        world.setTerrain(std::vector<uint8_t>(size_t(w)*h,100),w,h,64);
        world.setMapPlacementFeatures(std::vector<uint16_t>(size_t(w)*h,0xffff),{});
    }
    // Spawns at a centre in px; returns the id.
    int spawn(const UnitType& t,int x,int z,int player=0) {
        const int id=world.spawn(&t,float(x),float(z),std::nullopt,player);
        if(id<=0)throw std::runtime_error("spawn failed");
        auto& u=u_(id);u.heading=Bam(0);u.speed=Fixed();u.baseSpeed=t.maxVel;u.orders.clear();
        return id;
    }
    Unit& u_(int id) {return *world.unit(id);}
    void at(int id,int x,int z) {auto& u=u_(id);u.x=Fixed::fromInt(x);u.z=Fixed::fromInt(z);}
    void atRaw(int id,int32_t x,int32_t z) {auto& u=u_(id);u.x=Fixed::raw(x);u.z=Fixed::raw(z);}
    void order(int id,int x,int z) {Order o;o.x=Fixed::fromInt(x);o.z=Fixed::fromInt(z);u_(id).orders.assign(1,o);}
    void done(int id) {u_(id).orders.clear();u_(id).speed=Fixed();}
};

void expect(const char* test,const obs::Keys& k,const char* key,int64_t want,bool quiet=false) {
    const int64_t got=obs::Observer::get(k,key);
    if(got!=want) {
        if(!quiet)std::printf("FAIL %s: %s = %lld, want %lld\n",test,key,(long long)got,(long long)want);
        ++failures;
    }
}

// ---- lanes: crossings over a strip, files abreast -------------------------
// A gate x 10..17 (8 cells), lateral z, end windows 2 cells deep. Three 2x2
// members cross it eastward together: a from z cell 20 to 22, b from 22 to
// 20 (their order flips by exactly 2 cells = one body width at both ends: a
// crossing), c stays at 30 (never flips with either). Pairs: ab ac bc = 3,
// crossings 1. A fourth member d crosses the same way 700 ticks later
// (beyond the 600-tick pair window): no pairs with it.
int crossingsCase(int mutation,bool quiet) {
    Scene s;const auto t=mover(2);
    const int a=s.spawn(t,8*16+8,20*16+8),b=s.spawn(t,8*16+8,22*16+8),c=s.spawn(t,8*16+8,30*16+8),d=s.spawn(t,8*16+8,40*16+8);
    obs::Config cfg;cfg.groups={{"g",{a,b,c,d},60*16,30*16}};
    obs::Gate g;g.name="strip";g.region={10,0,17,60};g.lateral=1;g.edge=2;cfg.gates={g};
    cfg.mutation=mutation;
    obs::Observer o(cfg);
    for(int id:{a,b,c,d})s.order(id,60*16,30*16);
    for(int tick=0;tick<900;++tick) {
        // a, b, c: x from cell 8 to 20 over ticks 0..191 (1 px per tick).
        const int x=8*16+8+std::min(tick,192);
        const bool late=x>=14*16;   // the lateral switch, mid strip
        s.at(a,x,(late?22:20)*16+8);s.at(b,x,(late?20:22)*16+8);s.at(c,x,30*16+8);
        // d: the same crossing, from tick 700.
        s.at(d,8*16+8+std::clamp(tick-700,0,192),40*16+8);
        o.sample(s.world,tick);
    }
    const auto k=o.report();
    const int before=failures;
    expect("crossings",k,"gate.strip.pairs",3,quiet);
    expect("crossings",k,"gate.strip.crossings",1,quiet);
    expect("crossings",k,"gate.strip.cross_none",1,quiet);   // W9 step 0: no navigator in this scene: every flip is unattributed
    expect("crossings",k,"gate.strip.cross_gate",0,quiet);
    return failures-before;
}

// Files: three members stand inside a gate at z cells 20, 21 and 26 (bands
// 10, 10, 13: 2 files, lateral spread 6 cells) for 50 ticks; a fourth is
// outside. A second gate needs 4 inside, so it never samples.
void filesCase() {
    Scene s;const auto t=mover(2);
    const int a=s.spawn(t,12*16+8,20*16+8),b=s.spawn(t,13*16+8,21*16+8),c=s.spawn(t,14*16+8,26*16+8),d=s.spawn(t,40*16+8,20*16+8);
    obs::Config cfg;cfg.groups={{"g",{a,b,c,d},0,0}};
    obs::Gate g;g.name="files";g.region={10,0,17,60};g.lateral=1;g.band=2;
    obs::Gate g4=g;g4.name="four";g4.minCount=4;cfg.gates={g,g4};
    obs::Observer o(cfg);
    for(int tick=0;tick<50;++tick)o.sample(s.world,tick);
    const auto k=o.report();
    expect("files",k,"gate.files.samples",50);
    expect("files",k,"gate.files.files_x100",200);
    expect("files",k,"gate.files.spread_x100",600);
    expect("files",k,"gate.four.samples",0);
}

// ---- lane-order swaps and hug carry-over (the MV-13 S-bend probe) ---------
// Line 1: cells x 20..21, lateral z, sign +1. Line 2: z 40..41, lateral x,
// sign -1 (x is recorded negated). Five members, all entering both lines on
// the same ticks (every pair is inside the 300-tick window: 10 pairs), at
// (z at line 1, x at line 2):
//   a (10,30)  b (14,26)  c (18,22)  e (16,34)  f (11,33)
// recorded as (z, -x). A pair swaps when its difference at line 1 and at
// line 2 have opposite signs, both at least 2 cells:
//   a-b -4/-4   a-c -8/-8   a-e -6/+4 swap   a-f -1/+3 (too small)
//   b-c -4/-4   b-e -2/+8 swap   b-f 3/7   c-e 2/12   c-f 7/11
//   e-f 5/-1 (too small)
// Swaps 2. Hug, tips z 10 and x 34 within 2 cells: vertex 1 a (10) and f
// (11); vertex 2 e (34) and f (33); carry (both vertices) f alone.
void laneCase() {
    Scene s;const auto t=mover(2);
    struct P {int z1,x2;};
    const std::vector<P> plan={{10,30},{14,26},{18,22},{16,34},{11,33}};
    std::vector<int> ids;
    for(size_t i=0;i<plan.size();++i)ids.push_back(s.spawn(t,(5+int(i)*3)*16+8,5*16+8));
    obs::Config cfg;cfg.groups={{"g",ids,0,0}};
    obs::LaneOrder l;l.name="s";l.before={{20,0,21,90},1,1};l.after={{0,40,90,41},0,-1};
    cfg.laneOrders={l};
    obs::Hug h;h.name="s";h.first=l.before;h.second=l.after;h.tipFirst=10;h.tipSecond=34;h.cells=2;cfg.hugs={h};
    obs::Observer o(cfg);
    for(int tick=0;tick<4;++tick) {
        for(size_t i=0;i<plan.size();++i) {
            if(tick==1)s.at(ids[i],20*16+8,plan[i].z1*16+8);   // into line 1
            if(tick==2)s.at(ids[i],60*16+8,60*16+8);            // between
            if(tick==3)s.at(ids[i],plan[i].x2*16+8,40*16+8);   // into line 2
        }
        o.sample(s.world,tick);
    }
    const auto k=o.report();
    expect("lanes",k,"lane.s.pairs",10);
    expect("lanes",k,"lane.s.swaps",2);
    expect("lanes",k,"hug.s.first",2);    // a (z 10), f (z 11)
    expect("lanes",k,"hug.s.second",2);   // e (x 34), f (x 33)
    expect("lanes",k,"hug.s.carry",1);    // f
}

// ---- spacing: contacts by footprint ring ---------------------------------
// 2x2 bodies (origin = centre - 16 px): A1 origin (10,10), A2 (12,10) beside
// it, B1 (10,12) below A1, S a settled ungrouped body at (8,10) left of A1, F
// a far settled body. Rings: A1 touches A2 (own), B1 (other), S (settled);
// A2 touches A1 (own) and B1 at (11,12) (other); B1 touches A1 and A2 (other)
// and S diagonally at (9,11) (settled). G, a settled member of A at (14,10),
// touches A2: its own command (the group, nothing noted), so not counted. 3
// samples per decision tick: own 2/3, other 3/3. contact_settled (W3-1) is
// per arrived unit, none arrived (read as 1): A1-S and B1-S on 2 decision
// ticks = 4000. Noting one selection {A1, A2, B1, S} makes S their own
// command and G (still its group's) foreign: A2-G twice = 2000.
// Commands key by convoy (ruling W3 round 4 (1)): {A1, A2, B1} and {S} noted
// as two selections of one convoy are one command, as the single selection
// (2000); in two convoys S is foreign again: A1-S, B1-S and A2-G = 6000. A
// group whose click joined a convoy of 12 bodies (discN) reads that disc.
// Pair proximity A x B (centre cells within 2): A1 (11,11)-B1 (11,13) and
// A2 (13,11)-B1: 2 contacts of 2 pairs.
void spacingCase() {
    Scene s;const auto t=mover(2);
    const int a1=s.spawn(t,10*16+16,10*16+16),a2=s.spawn(t,12*16+16,10*16+16),b1=s.spawn(t,10*16+16,12*16+16);
    const int st=s.spawn(t,8*16+16,10*16+16),far=s.spawn(t,60*16+16,60*16+16);(void)far;
    const int g=s.spawn(t,14*16+16,10*16+16);
    for(int id:{a1,a2,b1})s.order(id,80*16,80*16);
    obs::Config cfg;cfg.groups={{"A",{a1,a2,g},0,0},{"B",{b1},0,0}};cfg.pairs={{"A","B",2}};
    obs::Observer o(cfg),noted(cfg),oneConvoy(cfg),twoConvoys(cfg);
    noted.noteSelection({a1,a2,b1,st});
    oneConvoy.noteSelection({a1,a2,b1},7);oneConvoy.noteSelection({st},7);
    twoConvoys.noteSelection({a1,a2,b1},7);twoConvoys.noteSelection({st},8);
    auto disc=cfg;disc.groups[0].discN=12;
    obs::Observer wide(disc);
    for(int tick=0;tick<20;++tick) {   // decision ticks 0 and 10
        o.sample(s.world,tick);noted.sample(s.world,tick);oneConvoy.sample(s.world,tick);
        twoConvoys.sample(s.world,tick);wide.sample(s.world,tick);
    }
    const auto k=o.report();
    expect("spacing",oneConvoy.report(),"contact_settled_permille",2000);
    expect("spacing",twoConvoys.report(),"contact_settled_permille",6000);
    expect("spacing",k,"g.A.radius_px",58);           // 3 bodies, 2x2: 16*3*1.25*sqrt(3/pi)
    expect("spacing",wide.report(),"g.A.radius_px",117);   // the convoy's 12
    expect("spacing",k,"spacing_samples",6);
    expect("spacing",k,"contact_own_permille",667);
    expect("spacing",k,"contact_other_permille",1000);
    expect("spacing",k,"contact_settled_permille",4000);
    expect("spacing",noted.report(),"contact_settled_permille",2000);
    expect("spacing",k,"pair.A.B.pairs",4);
    expect("spacing",k,"pair.A.B.contacts",4);
    expect("spacing",k,"pair.A.B.permille_x100",100000);
}

// A pair given as comma lists is the union of its groups: C (a2) joins A (a1) against B, so the
// pairs and contacts equal spacingCase's A (a1, a2) against B, under the pair's own key.
void pairListCase() {
    Scene s;const auto t=mover(2);
    const int a1=s.spawn(t,10*16+16,10*16+16),a2=s.spawn(t,12*16+16,10*16+16),b1=s.spawn(t,10*16+16,12*16+16);
    for(int id:{a1,a2,b1})s.order(id,id==b1?10*16+16:80*16,80*16);
    obs::Config cfg;cfg.groups={{"A",{a1},0,0},{"C",{a2},0,0},{"B",{b1},0,0}};
    obs::Pair p;p.a="A,C";p.b="B";p.cells=2;p.name="ac";
    obs::Pair bad;bad.a="A,Z";bad.b="B";bad.name="bad";
    cfg.pairs={p,bad};
    obs::Observer o(cfg);
    for(int tick=0;tick<20;++tick)o.sample(s.world,tick);
    const auto k=o.report();
    expect("pairlist",k,"pair.ac.pairs",4);
    expect("pairlist",k,"pair.ac.contacts",4);
    expect("pairlist",k,"pair.bad.pairs",0);
}

// ---- walls: clearance and touch ------------------------------------------
// A wall column at x cell 30. 2x2 origins x 29 and 30 overlap it (distance
// 0); origin 28 is next to it (clearance 0: touching), 27 has one free cell
// (clearance 1). Two members walk south along origins 28 and 27, 1 px per
// tick, from origin z 30; a third at origin x 60 is far from the wall and
// the map's edges (which count as walls: out of the map is illegal), beyond
// the 10-cell 'near' band. Decision samples with a prior sample: ticks
// 10..90, 9 each.
void wallCase() {
    Scene s;const auto t=mover(2);
    const int a=s.spawn(t,28*16+16,30*16+16),b=s.spawn(t,27*16+16,30*16+16),c=s.spawn(t,60*16+16,30*16+16);
    const int d=s.spawn(t,28*16+16,10*16+16);   // W9 step 0: a body that stands still against the wall
    std::vector<uint8_t> blocked(size_t(s.W)*s.H,0);
    for(int z=0;z<s.H;++z)blocked[size_t(z)*s.W+30]=1;
    for(int id:{a,b,c,d})s.order(id,id==c?60*16+16:28*16,90*16);
    obs::Config cfg;cfg.groups={{"g",{a,b,c,d},0,0}};
    obs::Observer o(cfg);o.setStaticBlocked(blocked,s.W,s.H);
    for(int tick=0;tick<100;++tick) {
        s.at(a,28*16+16,30*16+16+tick);s.at(b,27*16+16,30*16+16+tick);s.at(c,60*16+16,30*16+16+tick);
        s.at(d,28*16+16,10*16+16);
        o.sample(s.world,tick);
    }
    const auto k=o.report();
    expect("walls",k,"wall_samples",27);
    expect("walls",k,"wall_touch_permille",333);
    expect("walls",k,"wall_near_samples",18);
    expect("walls",k,"wall_touch_near_permille",500);
    expect("walls",k,"clearance_mean_x100",50);
    expect("walls",k,"clearance_p10_x100",0);
    // W9 step 0 splits: the moving touches are all on a flat wall column; d stands still against it
    expect("walls",k,"wall_touch_flat_permille",333);
    expect("walls",k,"wall_touch_gate_permille",0);
    expect("walls",k,"wall_touch_ring_permille",0);
    expect("walls",k,"wall_touch_corner_permille",0);
    expect("walls",k,"wall_touch_near_flat_permille",500);
    expect("walls",k,"wall_still_samples",9);
    expect("walls",k,"wall_touch_still_permille",1000);
}

// ---- motion: spins, reversals, sideways/backward, statue, walk in place ---
void motionCase() {
    Scene s;const auto t=mover(2);
    const int spin=s.spawn(t,10*16+8,10*16+8),rev=s.spawn(t,20*16+8,10*16+8),side=s.spawn(t,30*16+8,10*16+8),
              backw=s.spawn(t,40*16+8,40*16+8),statue=s.spawn(t,50*16+8,10*16+8);
    for(int id:{rev,side,backw,statue})s.order(id,90*16,90*16);
    obs::Config cfg;cfg.groups={{"g",{spin,rev,side,backw,statue},0,0}};cfg.decisionEvery=1000;
    obs::Observer o(cfg);
    for(int tick=0;tick<20;++tick) {
        // spin: still, turning 100 BAM a tick for ticks 1..5: 5 spins.
        if(tick>=1&&tick<=5)s.u_(spin).heading=Bam(tick*100);
        // rev: cell 20 -> 21 (tick 3) -> 20 (tick 6): one return within 90 ticks.
        s.at(rev,(tick>=3&&tick<6?21:20)*16+8,10*16+8);
        // side: faces +z (heading 0), walks +x 2 px a tick: 90 degrees off,
        // sideways (> 45) but not backward (> 90): ticks 1..19 = 19.
        s.at(side,30*16+8+2*tick,10*16+8);
        // backw: faces +z, walks -z 2 px a tick: sideways and backward, 19 each.
        // (rev's two 16 px steps east are sideways too: 2 more.)
        s.at(backw,40*16+8,40*16+8-2*tick);
        // statue: speed 1, steps 0.125 px a tick (<= cap/8 = 0.225), never
        // turns. statue ticks 1..19 = 19. No headway (< baseSpeed/4) from
        // tick 0, so walk_in_place counts once the run reaches 6: ticks 5..19 = 15.
        s.atRaw(statue,(50*16+8)*65536+tick*8192,(10*16+8)*65536);s.u_(statue).speed=Fixed::fromInt(1);
        o.sample(s.world,tick);
    }
    const auto k=o.report();
    expect("motion",k,"spins",5);
    expect("motion",k,"reversals",1);
    expect("motion",k,"sideways",19+19+2);
    expect("motion",k,"backward",19);
    expect("motion",k,"statue_ticks",19);
    expect("motion",k,"walk_in_place",15);
    // rev: still with orders on ticks 1,2 (run 2), 4,5 (2), 7.. 19 (13 >= 10:
    // waiting ticks 16..19 = 4); spin has no orders; statue moves every tick.
    expect("motion",k,"waiting_no_progress",4);
}

// ---- progress and AR-08 completion ---------------------------------------
// Five 2x2 bodies round a click at (480,480) px: four 16 px off it, one 200
// px off (12 cells). Packed disc: 400*9*5*113/355 = 5729 px^2 (75 px). Their
// orders end at ticks 5,10,15,20,25. Arrived: 1,2,3 (t50 at 15: 3*2 >= 5),
// 4, then the outlier is outside: t90/done never, left_behind 1. Completion
// distances 1,1,1,1,12 cells: median 1, max 12, one outside. Read as five
// one-body groups of one click (ruling W3 final exit (f); each disc sized for
// the convoy's 5): click.c has the group's numbers, each group's own t90 is
// its body's (g0 at 5, g4 never) and click_n is 5.
void progressCase() {
    Scene s;const auto t=mover(2);
    const int cx=480,cz=480;
    const std::vector<std::pair<int,int>> off={{16,0},{0,16},{-16,0},{0,-16},{200,0}};
    std::vector<int> ids;
    for(auto [dx,dz]:off)ids.push_back(s.spawn(t,cx+dx,cz+dz));
    for(int id:ids)s.order(id,cx,cz);
    obs::Config cfg;cfg.groups={{"g",ids,cx,cz}};
    obs::Config one;
    for(size_t i=0;i<ids.size();++i) {
        obs::Group g{"g"+std::to_string(i),{ids[i]},cx,cz};g.discN=5;g.click="c";one.groups.push_back(g);
    }
    obs::Observer o(cfg),clicked(one);
    for(int tick=100;tick<140;++tick) {
        for(size_t i=0;i<ids.size();++i)if(tick-100==5*int(i+1))s.done(ids[i]);
        o.sample(s.world,tick);clicked.sample(s.world,tick);
    }
    const auto c=clicked.report();
    expect("progress",c,"click.c.n",5);
    expect("progress",c,"click.c.arrived",4);
    expect("progress",c,"click.c.t50",15);
    expect("progress",c,"click.c.t90",-1);
    expect("progress",c,"click.c.done",-1);
    expect("progress",c,"g.g0.t90",5);
    expect("progress",c,"g.g4.t90",-1);
    expect("progress",c,"g.g4.click_n",5);
    const auto k=o.report();
    expect("progress",k,"g.g.radius_px",75);
    expect("progress",k,"g.g.t50",15);
    expect("progress",k,"g.g.t90",-1);
    expect("progress",k,"g.g.done",-1);
    expect("progress",k,"g.g.arrived",4);
    expect("progress",k,"g.g.left_behind",1);
    expect("progress",k,"g.g.orders_done",25);
    expect("progress",k,"g.g.complete_n",5);
    expect("progress",k,"g.g.complete_outside_radius",1);
    expect("progress",k,"g.g.complete_dist_median",1);
    expect("progress",k,"g.g.complete_dist_max",12);
}

// ---- decision stability and follow chains --------------------------------
// Decision samples every 10 ticks. zig walks east 16 px a sample, alternating
// 16 px north and south: the way to its goal (far east) is left, right, left
// ... of each step: a reversal at every sample after the first sided one
// (samples 1..9 sided, 8 reversals). blink moves on odd samples and stands on
// even ones: its class flips at every sample after the first (samples 1..9:
// 8 flips). runner moves 20 px a sample (>= half its base speed, 9 px) for
// samples 1..34, then stops: 34 free samples, then a stop: one engagement.
// A chain of three movers 32 px apart heading east at the same speed (each
// within two body widths behind the next): depth 3. A fourth at another
// speed is not part of it.
void stabilityCase() {
    Scene s(160,96);const auto t=mover(2);
    const int zig=s.spawn(t,10*16,10*16),blink=s.spawn(t,10*16,30*16),runner=s.spawn(t,10*16,50*16);
    const int c1=s.spawn(t,10*16,70*16),c2=s.spawn(t,12*16,70*16),c3=s.spawn(t,14*16,70*16),c4=s.spawn(t,16*16,70*16);
    for(int id:{zig,blink,runner,c1,c2,c3,c4})s.order(id,150*16,s.u_(id).z.v>>16);
    for(int id:{c1,c2,c3})s.u_(id).speed=Fixed::fromInt(1);
    s.u_(c4).speed=Fixed::fromInt(2);
    obs::Config cfg;cfg.groups={{"g",{zig,blink,runner,c1,c2,c3,c4},0,0}};
    obs::Observer o(cfg);
    for(int tick=0;tick<400;++tick) {
        const int n=tick/10;   // samples so far at the decision tick
        if(tick%10==0) {
            if(n<10)s.at(zig,10*16+16*n,10*16+(n%2?16:0));
            s.at(blink,10*16+16*((n+1)/2),30*16);
            s.at(runner,10*16+20*std::min(n,34),50*16);
            for(int id:{c1,c2,c3,c4})s.at(id,(s.u_(id).x.v>>16)+10,70*16);
        }
        o.sample(s.world,tick);
    }
    const auto k=o.report();
    expect("stability",k,"aim_reversals",8);
    expect("stability",k,"engagement_on_flowing",1);
    expect("stability",k,"follow_chain_max",3);
    // flips: blink 38 (samples 1..39 alternate), zig: moves samples 1..9,
    // still 10..39 (1 flip), runner: 1 flip at 35, chain: none.
    expect("stability",k,"flips",38+1+1);
}

// ---- flyers ----------------------------------------------------------------
// One flyer F with a ground member G of its group (G keeps orders). F lands
// on top of G's footprint for ticks 10..14 (illegal overlap 5), takes off at
// 5 and 15 (2 takeoffs, the second after a landing: 1 relift), starts a
// landing at 20 and abandons it at 25 airborne (1 go-around), hovers still
// from tick 60 (the hover window opens at 60) to 79 (20 ticks), and its
// orders end at 80; it starts descending at 90 (land delay 10). Away: F sits
// 100 px from G from tick 150 to 199 (G's orders hold): away_max 100.
void flyerCase() {
    Scene s;const auto t=mover(2);const auto f=flyer();
    const int g=s.spawn(t,20*16+16,20*16+16),fl=s.spawn(f,40*16+16,20*16+16);
    s.order(g,80*16,80*16);s.order(fl,80*16,80*16);
    auto& u=s.u_(fl);u.flightGroundMode=1;
    obs::Config cfg;cfg.groups={{"g",{g,fl},0,0}};
    obs::Observer o(cfg);
    for(int tick=0;tick<200;++tick) {
        int mode=tick<5?1:tick<10?2:tick<15?1:2;
        if(tick>=95)mode=1;
        u.flightGroundMode=uint8_t(mode);
        if(tick>=10&&tick<15)s.at(fl,20*16+16,20*16+16); else if(tick<150)s.at(fl,40*16+16+tick,20*16+16);
        else s.at(fl,20*16+16+100,20*16+16);
        if(tick==20)u.landing.emplace();
        if(tick==25)u.landing.reset();
        if(tick>=60&&tick<80)s.at(fl,40*16+16+60,20*16+16);   // hovering still
        if(tick==80)u.orders.clear();
        if(tick==90)u.landing.emplace();
        if(tick==95)u.landing.reset();
        u.speed=Fixed();
        o.sample(s.world,tick);
    }
    const auto k=o.report();
    expect("flyers",k,"g.g.flyers",1);
    expect("flyers",k,"g.g.takeoffs",2);
    expect("flyers",k,"g.g.relifts",1);
    expect("flyers",k,"g.g.go_arounds",1);
    expect("flyers",k,"g.g.illegal_overlap_ticks",5);
    expect("flyers",k,"g.g.land_delay_max",10);
    expect("flyers",k,"g.g.away_max",100);
}

// ---- side lines, work ------------------------------------------------------
void sideWorkCase() {
    Scene s;const auto t=mover(2);
    std::vector<int> ids;for(int i=0;i<10;++i)ids.push_back(s.spawn(t,(5+i*3)*16+8,10*16+8));
    obs::Config cfg;cfg.groups={{"g",ids,0,0}};cfg.sides={{"east",0,40}};
    obs::Observer o(cfg);
    // One member crosses x cell 40 every 5 ticks from tick 5: 9 of 10 by 45.
    for(int tick=0;tick<60;++tick) {
        for(int i=0;i<10;++i)if(tick>=5*(i+1))s.at(ids[size_t(i)],41*16+8,(10+3*i)*16+8);
        o.work("a",uint64_t(tick));               // 0..59: total 1770, max 59, p99 59
        o.workCumulative("b",uint64_t(tick*2));   // per tick 2 (0 at the first)
        o.sample(s.world,tick);
    }
    const auto k=o.report();
    expect("side",k,"side.east.t90",45);
    expect("side",k,"side.east.high",10);
    expect("side",k,"work.a.total",1770);
    expect("side",k,"work.a.max",59);
    expect("side",k,"work.a.p99",59);
    expect("side",k,"work.b.total",118);
    expect("side",k,"work.b.max",2);
}
}

int main() {
    try {
        crossingsCase(0,false);
        filesCase();laneCase();spacingCase();pairListCase();wallCase();motionCase();progressCase();stabilityCase();flyerCase();sideWorkCase();
        const int real=failures;
        // The mutated metric must be caught by its sub-check.
        const int caught=crossingsCase(1,true);
        failures=real;
        if(caught==0) {std::printf("FAIL mutation: the strict flip test was not detected\n");++failures;}
        else std::printf("mutation detected (%d sub-check failures)\n",caught);
    } catch(const std::exception& e) {std::fprintf(stderr,"legion_observe_selftest: %s\n",e.what());return 1;}
    if(failures) {std::printf("%d failures\n",failures);return 1;}
    std::puts("ok");
    return 0;
}
