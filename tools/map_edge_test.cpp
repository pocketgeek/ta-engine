// Nothing leaves the coordinate range on the largest maps.
//
// Positions are 16.16 fixed point in an int32 (sim/fixed.h): the largest value is
// 32767.99998 px. A 2048-cell map is 32768 px a side, so its east and south edges
// sit at the top of the range with no room past them. Sums used to wrap there: a
// flyer that crossed the edge reappeared 65536 px to the west, a group move
// ordered at the edge sent members to the far side, and so on. Now a sum or a
// difference of positions saturates (satAdd32 / satSub32), so a point past such
// an edge is a point on it.
//
//   map_edge_test              the arithmetic and constructed 2048-cell worlds, synthetic units
//   map_edge_test <data dir>   Gryphons on attack runs at targets on the east and south edges
#include "hpi/hpi.h"
#include "net/lockstep.h"
#include "sim/legion.h"
#include "sim/matchsetup.h"
#include "sim/retailairattack.h"
#include "sim/retailanimationqueries.h"
#include "sim/retailconstruction.h"
#include "sim/retailflight.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace ta::sim { struct RetailReplayProbe {
    static int gridW(const World& w) { return w.gW_; }
    static int gridH(const World& w) { return w.gH_; }
    static void shoot(World& w,const Projectile& p) { w.projectiles_.push_back(p); }
}; }

namespace {
using namespace ta::sim;
constexpr int kCells=2048;                    // the largest map: 2048 cells of 16 px
constexpr float kMapPx=float(kCells)*16;      // 32768 px, one past the largest position
constexpr int32_t kPx=65536;                  // one pixel, raw
int failures=0;
void check(bool ok,const std::string& message) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",message.c_str());
    if(!ok)++failures;
}
double px(Fixed v) { return double(v.v)/65536.0; }

UnitType walker(const char* id,int foot=1) {
    UnitType t{};t.id=t.name=id;
    t.canMove=true;t.maxHp=1000;t.footX=t.footZ=foot;t.sight=256;t.maxVel=Fixed::fromInt(2);
    t.accel=t.brake=Fixed::fromInt(10);t.turnRate=t.turnInPlaceRate=2500;t.halfCellTicks=3;t.buildTime=1;
    return t;
}
UnitType flyer(const char* id) {
    UnitType t{};t.id=t.name=id;
    t.canFly=t.canMove=true;t.maxHp=1000;t.footX=t.footZ=2;t.sight=256;
    t.maxVel=Fixed::fromInt(4);t.accel=t.brake=Fixed::fromInt(1);t.turnRate=1200;t.cruiseAlt=80;t.buildTime=1;
    return t;
}
void flat(World& w) {
    w.setGameSeed(7);w.setSerialThreads(true);w.setPlayerCount(2);w.setTeam(0,0);w.setTeam(1,1);
    w.setTerrain(std::vector<uint8_t>(size_t(kCells)*kCells,100),kCells,kCells,20);
}

// What a flyer did over a run: the largest step in one tick, and how far it was from the map.
struct Track {
    int id=0;
    Fixed x,z;
    double step=0,low=1e9,travelled=0;
    void start(const World& w,int unit) { id=unit;x=w.unit(unit)->x;z=w.unit(unit)->z; }
    void tick(const World& w) {
        const Unit* u=w.unit(id);
        if(!u || !u->alive())return;
        const double dx=std::abs(double(u->x.v)-double(x.v))/65536,dz=std::abs(double(u->z.v)-double(z.v))/65536;
        step=std::max({step,dx,dz});
        travelled+=std::hypot(dx,dz);
        low=std::min({low,px(u->x),px(u->z)});
        x=u->x;z=u->z;
    }
};
// The spatial hash follows the units: map size plus the off-map margin, in its 32 px cells.
constexpr int kMarginPx=2048;
constexpr int kGridMax=(kCells*16+2*kMarginPx)/32+3;

// ---- The arithmetic ------------------------------------------------------------------
void arithmetic() {
    // volatile: the plain int32 add this replaced was undefined on overflow, and a
    // compiler may fold it into anything.
    volatile int32_t top=INT32_MAX-kPx,bottom=INT32_MIN+kPx,step=4*kPx;
    const Fixed a=Fixed::raw(top),b=Fixed::raw(bottom),s=Fixed::raw(step);
    check((a+s).v==INT32_MAX,"a sum past the top of the range stops at the top");
    check((b-s).v==INT32_MIN,"a difference past the bottom of the range stops at the bottom");
    check((a-b).v==INT32_MAX && (b-a).v==INT32_MIN,"a difference of far-apart positions keeps its sign");
    Fixed c=a;c+=s;Fixed d=b;d-=s;
    check(c.v==INT32_MAX && d.v==INT32_MIN,"+= and -= stop at the ends of the range too");
    check((-Fixed::raw(bottom-kPx)).v==INT32_MAX,"negating the lowest value stops at the top");
    bool same=true;
    for(int32_t x:{0,1,-1,kPx,-kPx,1234567,-7654321,INT32_MAX-step,INT32_MIN+step})
        for(int32_t y:{0,1,-1,kPx,-kPx,222222,-step,step})
            same=same && (Fixed::raw(x)+Fixed::raw(y)).v==int32_t(int64_t(x)+y) &&
                 (Fixed::raw(x)-Fixed::raw(y)).v==int32_t(int64_t(x)-y);
    check(same,"in range the sum and the difference are the plain ones");
    volatile int edge=kCells*16;
    check(Fixed::fromInt(edge).v==INT32_MAX && Fixed::fromInt(-edge-1).v==INT32_MIN && Fixed::fromInt(edge-1).v==(edge-1)*kPx,
          "32768 whole pixels, the far edge of a 2048-cell map, is the top of the range");
}

// ---- The flight ports on raw position words ---------------------------------------------
void flightPorts() {
    const int32_t edge=INT32_MAX-10*kPx;   // 10 px inside the east edge of a 2048-cell map
    // 41c5b1: `distance` px from the point along -heading. Heading 0xc000 flies east.
    const auto run=retailAirRunPoint({edge,0,1000*kPx},0,0xc000,600);
    check(run.x>=edge,"an attack run's point past the east edge stays at the edge (x "+std::to_string(run.x/kPx)+")");
    const auto south=retailAirRunPoint({1000*kPx,0,edge},0,0x8000,600);
    check(south.z>=edge,"an attack run's point past the south edge stays at the edge (z "+std::to_string(south.z/kPx)+")");
    // A flyer 1000 px past the west edge sent to a point at the east edge: more than
    // 32768 px apart. It must be steered east, as one starting on the map is.
    const RetailFlightVector far{-1000*kPx,80*kPx,5000*kPx},near{0,80*kPx,5000*kPx},goal{32500*kPx,80*kPx,5000*kPx};
    const auto farNav=retailFlightNavigation(far,goal,goal,0,80*kPx,false,false,0);
    const auto nearNav=retailFlightNavigation(near,goal,goal,0,80*kPx,false,false,0);
    check(farNav.heading==nearNav.heading,"a flyer more than 32768 px from its goal heads toward it");
    const auto velocity=retailFlightVelocity({},far,goal,{},0,4*kPx,kPx,kPx,nearNav.heading);
    check(velocity.x>0,"...and accelerates toward it");
    // A patrol waypoint at the east edge: the point flown to lies past it (41a47e).
    uint32_t draw=0;
    const auto patrol=retailFlightPatrolGoal({edge-800*kPx,80*kPx,1000*kPx},{edge,80*kPx,1000*kPx},4*kPx,
        [&](int n) { return uint32_t(n-1)*(++draw%2); });
    check(patrol.point.x>=edge-200*kPx,"a patrol point past the east edge stays at the edge (x "+
          std::to_string(patrol.point.x/kPx)+")");
}

// ---- Whole-pixel distances read as signed 16-bit words -----------------------------------
void distances() {
    // 405756: a builder 42,000 px from its site is not within its 100 px reach.
    const Fixed bx=Fixed::fromInt(100),bz=Fixed::fromInt(100),sx=Fixed::fromInt(30000),sz=Fixed::fromInt(30000);
    check(!retailBuildWithinReach(bx,bz,sx,sz,2,2,2,2,100),"a site 42,000 px away is out of a builder's reach");
    check(retailBuildWithinReach(sx-Fixed::fromInt(40),sz,sx,sz,2,2,2,2,100) &&
          !retailBuildWithinReach(sx-Fixed::fromInt(400),sz,sx,sz,2,2,2,2,100),"the reach test is unchanged up close");
    // 52c4e0: a level aim at a point 42,000 px away has no pitch.
    const auto aim=retailDirectAim(30000*kPx,0,30000*kPx,0);
    const auto close=retailDirectAim(300*kPx,0,300*kPx,0);
    check(aim[1]==close[1],"a level aim 42,000 px away is level");
}

// ---- Flyers at the east and south edges, synthetic ---------------------------------------
// A flyer's guard orbits its ward 320 px out (VTOL_Follow), and a flyer's patrol
// overshoots its waypoint: both fly past an edge the ward or the waypoint stands at.
void flyersAtTheEdge() {
    World w;flat(w);
    const UnitType ward=walker("ward"),air=flyer("air");
    const int eastWard=w.spawn(&ward,kMapPx-24,16000,std::nullopt,0);
    const int southWard=w.spawn(&ward,16000,kMapPx-24,std::nullopt,0);
    std::vector<Track> tracks(5);
    const int eastGuard=w.spawn(&air,kMapPx-600,16000,std::nullopt,0);
    const int southGuard=w.spawn(&air,16000,kMapPx-600,std::nullopt,0);
    const int eastPatrol=w.spawn(&air,kMapPx-900,20000,std::nullopt,0);
    const int southPatrol=w.spawn(&air,20000,kMapPx-900,std::nullopt,0);
    const int corner=w.spawn(&air,kMapPx-700,kMapPx-700,std::nullopt,0);
    w.guard(eastGuard,eastWard,false);
    w.guard(southGuard,southWard,false);
    w.patrol(eastPatrol,kMapPx-8,20000);
    w.patrol(southPatrol,20000,kMapPx-8);
    w.order(corner,kMapPx-1,kMapPx-1,false);
    const int ids[]={eastGuard,southGuard,eastPatrol,southPatrol,corner};
    for(size_t i=0;i<tracks.size();++i)tracks[i].start(w,ids[i]);
    int gridW=0,gridH=0;
    std::vector<double> late(tracks.size(),0);
    for(int tick=0;tick<3000;++tick) {
        w.tick(1.f/30);
        for(size_t i=0;i<tracks.size();++i) {
            const double before=tracks[i].travelled;
            tracks[i].tick(w);
            if(tick>=2000)late[i]+=tracks[i].travelled-before;
        }
        gridW=std::max(gridW,RetailReplayProbe::gridW(w));gridH=std::max(gridH,RetailReplayProbe::gridH(w));
    }
    const char* names[]={"guard at the east edge","guard at the south edge","patrol to the east edge",
                         "patrol to the south edge","move to the south-east corner"};
    for(size_t i=0;i<tracks.size();++i) {
        const auto& t=tracks[i];
        std::printf("  %s: largest step %.2f px, lowest coordinate %.1f, travelled %.0f px (%.0f in the last 1000 ticks)\n",
                    names[i],t.step,t.low,t.travelled,late[i]);
        check(t.step<=px(air.maxVel)+1.0,std::string(names[i])+": never moves more than its speed in a tick");
        check(t.low>-double(kMarginPx),std::string(names[i])+": never appears on the far side of the map");
        // Guards and patrols keep flying; the mover to the corner arrives and stays.
        if(i<4)check(late[i]>1000,std::string(names[i])+": still flying its rounds after 2000 ticks");
    }
    const Unit* c=w.unit(corner);
    check(c && px(c->x)>kMapPx-40 && px(c->z)>kMapPx-40,"the flyer sent to the corner is at the corner");
    std::printf("  spatial grid at most %d x %d cells (limit %d)\n",gridW,gridH,kGridMax);
    check(gridW<=kGridMax && gridH<=kGridMax,"the spatial grid stays within the map plus its margin");
}

// ---- A group move ordered at the edge (Cmd::Order, the right-click's offsets) -------------
void groupMoveAtTheEdge() {
    for(const bool south:{false,true}) {
        World w;flat(w);
        const UnitType foot=walker("foot");
        TypeRegistry registry;
        std::vector<int> ids;
        // Three in a row 50 px apart, along the axis the edge closes.
        for(int i=-1;i<=1;++i)
            ids.push_back(south ? w.spawn(&foot,16000,kMapPx-600+float(i)*50,std::nullopt,0)
                                : w.spawn(&foot,kMapPx-600+float(i)*50,16000,std::nullopt,0));
        auto order=std::make_shared<ta::net::SelectionOrder>();
        order->serial=1;order->shape=ta::net::OrderShape::Offsets;
        order->tmpl.kind=ta::net::Cmd::Move;
        order->tmpl.x=south ? 16000.f : kMapPx-10;order->tmpl.z=south ? kMapPx-10 : 16000.f;
        order->members=ids;std::sort(order->members.begin(),order->members.end());
        ta::net::Command c;c.kind=ta::net::Cmd::Order;c.player=0;c.order=order;
        applyCommand(w,registry,c);
        double low=1e9;
        for(int id:ids) {
            const Unit* u=w.unit(id);
            if(!u || u->orders.empty()) {low=-1e9;continue;}
            low=std::min(low,px(south ? u->orders.back().z : u->orders.back().x));
        }
        std::printf("  %s edge: lowest member goal %.1f px along the closed axis\n",south ? "south" : "east",low);
        check(low>kMapPx-100,std::string("a group move ordered 10 px from the ")+(south ? "south" : "east")+
              " edge keeps every member's goal at that edge");
    }
}

// ---- A plain (2D) shot whose last step passes the edge -----------------------------------
// The shot flies whole ticks, so its last step can pass its target by up to one
// tick of travel. For a target 8 px from the edge that step ends past the edge;
// wrapped, the step's segment pointed the wrong way and missed the target.
void shotAtTheEdge() {
    for(const bool south:{false,true}) {
        World w;flat(w);
        UnitType victim=walker("victim");victim.maxVel={};victim.canMove=false;victim.healTime=0;
        UnitType shooter=walker("shooter");
        Weapon weapon;weapon.damage=100;weapon.range=400;weapon.projVel=1500;
        const float edge=kMapPx-8,from=edge-170;
        const int target=south ? w.spawn(&victim,16000,edge,std::nullopt,1) : w.spawn(&victim,edge,16000,std::nullopt,1);
        const int source=south ? w.spawn(&shooter,16000,from,std::nullopt,0) : w.spawn(&shooter,from,16000,std::nullopt,0);
        w.tick(1.f/30);
        const Fixed hp=w.unit(target)->hp;
        Projectile p;
        p.x=Fixed::fromFloat(south ? 16000 : from);p.z=Fixed::fromFloat(south ? from : 16000);
        (south ? p.vz : p.vx)=Fixed::fromInt(50);   // 50 px a tick: 4 ticks cover 200 px, 30 past the target
        p.life=4;p.flight=4;p.targetId=target;p.fromId=source;p.fromPlayer=0;p.wsrc=&weapon;
        RetailReplayProbe::shoot(w,p);
        double low=1e9;
        for(int tick=0;tick<6;++tick) {
            w.tick(1.f/30);
            for(const auto& shot:w.projectiles())low=std::min({low,px(shot.x),px(shot.z)});
        }
        check(low>0,std::string("a shot along the ")+(south ? "south" : "east")+" edge stays on the map");
        check(w.unit(target) && w.unit(target)->hp<hp,std::string("a shot whose last step passes the ")+
              (south ? "south" : "east")+" edge still hits its target standing there");
    }
}

// ---- Legion: a goal off the map costs no whole-map scan ----------------------------------
// The order point of a Move is kept as given (retail). Legion resolves it to the
// nearest origin the unit can stand on: square rings outward from the goal's cell.
// Every ring that lay wholly off the map was walked cell by cell.
void legionGoalOffTheMap() {
    for(const float goalX:{-30000.f,-32768.f}) {
        World w;
        w.setGameSeed(7);w.setSerialThreads(true);w.setPathService(true);
        w.setPathfindingMode(PathfindingMode::Legion);
        w.setPlayerCount(2);w.setTeam(0,0);w.setTeam(1,1);
        w.setTerrain(std::vector<uint8_t>(size_t(kCells)*kCells,100),kCells,kCells,20);
        w.setMapPlacementFeatures(std::vector<uint16_t>(size_t(kCells)*kCells,0xffff),{{"wall",1,1,true,true,false,0}});
        const UnitType foot=walker("foot",2);
        const int id=w.spawn(&foot,2000,16000,std::nullopt,0);
        w.tick(1.f/30);
        w.updateNavigationExploration();
        auto& explored=w.editNavigationExploration();
        std::fill(explored.begin(),explored.end(),0xffff);
        const auto registration=[&] {
            return w.legionNavigator() ? w.legionNavigator()->accounting().total[size_t(NavWork::Registration)] : ~uint64_t(0);
        };
        const uint64_t before=registration();
        const Fixed startX=w.unit(id)->x;
        w.order(id,goalX,16000,false);
        for(int tick=0;tick<120;++tick)w.tick(1.f/30);
        const uint64_t spent=registration()-before;
        std::printf("  goal x %.0f: registration work %llu units, unit moved %.1f px west\n",double(goalX),
                    static_cast<unsigned long long>(spent),px(startX)-px(w.unit(id)->x));
        // One ring of the map's own size is 8 x 2048 units; the whole-map scan was 4 x 2048 x 2049.
        check(spent<200000,"a Move to x "+std::to_string(int(goalX))+" resolves its goal without scanning the map");
        if(goalX>-32000)   // within reach of the ring walk: the nearest origin is at the west edge
            check(px(w.unit(id)->x)<px(startX)-50,"...and the unit walks toward the west edge");
    }
}

// ---- Gryphons on attack runs at the east and south edges (shipped units) ------------------
int gryphons(const char* data) {
    auto vfs=ta::hpi::mountRetailRoot(data);
    TypeRegistry registry;setupRegistry(registry,vfs,false);
    const UnitType* gryphon=registry.find("zongryp");
    const UnitType* soldier=registry.find("arasword");
    if(!gryphon || !soldier) { std::printf("FAIL: the Gryphon or the Swordsman is missing from %s\n",data);return 1; }
    uint64_t hashes[2]={0,0};
    for(int run=0;run<2;++run) {
        World w;flat(w);w.buildNavClasses(registry);
        // Targets that stand and last: 24 px inside the edge, four on each.
        UnitType victim=*soldier;victim.maxHp=30000;victim.maxVel={};victim.healTime=0;victim.weapon.damage=0;victim.weapons.clear();
        std::vector<int> flyers,targets;
        for(int i=0;i<4;++i) {
            targets.push_back(w.spawn(&victim,kMapPx-24,16000+float(i)*200,std::nullopt,1));
            flyers.push_back(w.spawn(gryphon,kMapPx-900,16030+float(i)*200,std::nullopt,0));
            w.attack(flyers.back(),targets.back(),false);
        }
        for(int i=0;i<4;++i) {
            targets.push_back(w.spawn(&victim,16000+float(i)*200,kMapPx-24,std::nullopt,1));
            flyers.push_back(w.spawn(gryphon,16030+float(i)*200,kMapPx-900,std::nullopt,0));
            w.attack(flyers.back(),targets.back(),false);
        }
        std::vector<Track> tracks(flyers.size());
        for(size_t i=0;i<flyers.size();++i)tracks[i].start(w,flyers[i]);
        std::vector<Fixed> half(targets.size());
        int gridW=0,gridH=0;
        for(int tick=0;tick<3000;++tick) {
            w.tick(1.f/30);
            for(auto& t:tracks)t.tick(w);
            gridW=std::max(gridW,RetailReplayProbe::gridW(w));gridH=std::max(gridH,RetailReplayProbe::gridH(w));
            if(tick==1499)for(size_t i=0;i<targets.size();++i)half[i]=w.unit(targets[i])->hp;
        }
        hashes[run]=w.stateHash();
        if(run)break;
        double step=0,low=1e9;
        for(const auto& t:tracks) {step=std::max(step,t.step);low=std::min(low,t.low);}
        std::printf("  Gryphons: largest step %.2f px (speed %.2f), lowest coordinate %.1f, grid at most %d x %d\n",
                    step,px(gryphon->maxVel),low,gridW,gridH);
        check(step<=px(gryphon->maxVel)+1.0,"a Gryphon on attack runs at the edge never moves more than its speed in a tick");
        check(low>-double(kMarginPx),"a Gryphon on attack runs at the edge never appears on the far side of the map");
        check(gridW<=kGridMax && gridH<=kGridMax,"the spatial grid stays within the map plus its margin");
        int hurtEarly=0,hurtLate=0;
        for(size_t i=0;i<targets.size();++i) {
            const Unit* t=w.unit(targets[i]);
            const Fixed full=Fixed::fromInt(victim.maxHp);
            hurtEarly+=half[i]<full;
            hurtLate+=t && t->hp<half[i];
            std::printf("  target %zu (%s edge): %.0f damage in ticks 0-1499, %.0f in 1500-2999\n",i,i<4 ? "east" : "south",
                        px(full)-px(half[i]),t ? px(half[i])-px(t->hp) : 0.0);
        }
        // The run turns back at the edge and comes round again: every target is hit in both halves.
        check(hurtEarly==int(targets.size()) && hurtLate==int(targets.size()),
              "every Gryphon keeps hitting its target at the edge (both halves of the run)");
    }
    check(hashes[0]==hashes[1],"the run is reproducible (state hash equal run to run)");
    return 0;
}
}   // namespace

int main(int argc,char** argv) {
    if(argc>2) { std::fprintf(stderr,"usage: map_edge_test [data dir]\n");return 2; }
    if(argc==2) {
        if(gryphons(argv[1]))return 1;
    } else {
        arithmetic();
        flightPorts();
        distances();
        flyersAtTheEdge();
        groupMoveAtTheEdge();
        shotAtTheEdge();
        legionGoalOffTheMap();
    }
    std::printf("%s: %d failure(s)\n",failures ? "FAILED" : "OK",failures);
    return failures ? 1 : 0;
}
