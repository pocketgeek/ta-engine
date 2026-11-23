// conjure_site_test -- a conjure order the game takes is carried out.
//
// Zhon's builders conjure mobile units on the ground beside them (retail's
// MobileBuild with a mobile type). The order used to be taken on a test of the
// clicked point and placed on a different test of the snapped site: a circle
// on unit centres, 8*max(foot)+12 px, that counted the conjurer itself, which
// its approach leaves standing beside the site and so inside the circle. The
// site was refused 31 times, 10 ticks apart, and the order was dropped with no
// message. The same circle let a site go down over a larger body whose centre
// lay outside it.
//
// Flat ground, the shipped types, every order through sim::applyCommand (the
// path a client's click and a computer player's command both take):
//  - an idle Beast Lord (3x3 Jungle Orc, 6x6 Roc), Beast Handler, Beast Tamer
//    and flying monarch, one order for every site position around them: the
//    order is taken, the site goes down in a few seconds, and it shares no
//    cell with the conjurer or anyone else;
//  - a second conjurer at work beside the spot: the order is refused when it
//    is given exactly when the site would lie on the worker or its site, and
//    every order that is taken is placed, also when the worker only comes to
//    stand in the first conjurer's way after the order (under Legion both
//    used to hold the conjurer there for good);
//  - a standing unit beside the spot, the player's own (it steps aside) or
//    another player's (it does not): the finished unit shares no cell with it;
//  - the conjurer among its player's idle units, ordered to conjure where it
//    stands: it walks off its own site, round them or after they made room;
//  - a conjurer that is not idle (walking past, or at work with the order
//    queued behind its job), ordered to conjure where it stands: the same;
//  - the conjurer hemmed in on a site of its own size by its player's idle
//    units, none of them on the site: one makes room and the unit is conjured,
//    also with another player's unit on the side the conjurer would take; by
//    another player's units all round: the order still ends;
//  - the computer player's own ring pick (ai::Controller), for the conjurer
//    standing at different places inside a cell: every order is placed.
// Each order is also compared with the build cursor's test, called as the
// client calls it (canPlaceOrder with the builder the order is for).
//
//   conjure_site_test <retail-install-dir> [--legion]
#include "ai/ai.h"
#include "hpi/hpi.h"
#include "sim/footprint.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ta;
namespace {
int failures=0,cases=0;
void check(bool ok,const std::string& label,const std::string& detail={}) {
    std::printf("[%s] %s%s%s\n",ok?"PASS":"FAIL",label.c_str(),detail.empty()?"":": ",detail.c_str());
    ++cases;if(!ok)++failures;
}

constexpr int kCells=96;
// A site goes down this soon after its order: the longest clean run is the
// Beast Lord walking out from under a 6x6 Roc. The 31 tries of a blocked site
// end at tick 301.
constexpr int kPlaceLimit=150;

struct Rect {int x,z,w,h;};
Rect footprint(const sim::Unit& u) {
    return {sim::footprintOrigin(u.x,u.type->footX),sim::footprintOrigin(u.z,u.type->footZ),u.type->footX,u.type->footZ};
}
// The site of `type` for a click at (x,z), as World::makeBuildOrder snaps it.
Rect siteRect(const sim::UnitType& type,float x,float z) {
    return {sim::footprintOrigin(x,type.footX),sim::footprintOrigin(z,type.footZ),type.footX,type.footZ};
}
bool intersects(const Rect& a,const Rect& b) {return a.x<b.x+b.w && b.x<a.x+a.w && a.z<b.z+b.h && b.z<a.z+a.h;}
// A body on the ground: a flyer in the air holds no cell.
bool grounded(const sim::Unit& u) {return u.alive() && u.type && !u.embarked() && (!u.type->canFly || u.flightGroundMode==1);}

// The first unit sharing a cell with `id`, or 0.
int sharesCells(const sim::World& w,int id) {
    const auto* a=w.unit(id);
    if(!a || !grounded(*a))return 0;
    for(const auto& b:w.units())
        if(b.id!=id && grounded(b) && intersects(footprint(*a),footprint(b)))return b.id;
    return 0;
}

void setup(sim::World& w,const sim::TypeRegistry& registry,bool legion) {
    w.setSerialThreads(true);
    if(legion)w.setPathfindingMode(sim::PathfindingMode::Legion);
    w.setTerrain(std::vector<uint8_t>(kCells*kCells,100),kCells,kCells,20);
    w.setMapPlacementFeatures(std::vector<uint16_t>(kCells*kCells,0xffff),{});
    w.buildNavClasses(registry);w.setPathService(true);
    w.player(0).mana=1000000;
}

int cursorDiffers=0;   // orders the build cursor's test answered differently
std::string cursorFirst;   // the first of them
bool build(sim::World& w,const sim::TypeRegistry& registry,int builder,const sim::UnitType& type,float x,float z,
           bool queue=false) {
    // The client's call (GameView::canPlaceLocked and canPlacePreview): the
    // builder the order is for goes in, as World::queueBuild passes its own.
    const bool cursor=w.canPlaceOrder(&type,x,z,0,builder);
    net::Command c{};
    c.kind=net::Cmd::Build;c.player=0;c.unitId=builder;c.x=x;c.z=z;c.queue=queue;
    std::snprintf(c.type,sizeof c.type,"%s",type.id.c_str());
    sim::applyCommand(w,registry,c);
    const auto* b=w.unit(builder);
    const bool taken=std::any_of(b->orders.begin(),b->orders.end(),[&](const sim::Order& o){return o.buildType==&type;});
    if(taken!=cursor && !cursorDiffers++)
        cursorFirst=type.id+" by "+b->type->id+" at ("+std::to_string(int(x))+","+std::to_string(int(z))+"): the cursor "+
            (cursor?"takes":"refuses")+" it, the order is "+(taken?"taken":"refused");
    return taken;
}

struct Outcome {
    bool accepted=false;
    int placed=-1,dropped=-1,shared=0,site=0;
};

// Ticks the world until `builder`'s order for `type` puts its site down or
// is given up.
Outcome follow(sim::World& w,int builder,const sim::UnitType& type,int limit) {
    Outcome o;o.accepted=true;
    for(int tick=1;tick<=limit;++tick) {
        w.player(0).mana=1000000;w.tick(1.f/30);
        const auto* b=w.unit(builder);
        const auto* site=b->buildSiteId ? w.unit(b->buildSiteId) : nullptr;
        if(site && site->type==&type) {o.placed=tick;o.site=site->id;o.shared=sharesCells(w,site->id);return o;}
        if(std::none_of(b->orders.begin(),b->orders.end(),[&](const sim::Order& q){return q.buildType==&type;})) {o.dropped=tick;return o;}
    }
    return o;
}

struct Tally {
    int clicks=0,refused=0,placed=0,dropped=0,waiting=0,shared=0,slowest=0,wrongRefusal=0,wrongAccept=0;
    std::string first;   // the first failing click
    void note(const char* what,float x,float z) {
        if(first.empty())first=std::string(what)+" at ("+std::to_string(int(x))+","+std::to_string(int(z))+")";
    }
    void add(const Outcome& o,float x,float z) {
        ++clicks;
        if(!o.accepted) {++refused;return;}
        if(o.placed>=0) {++placed;slowest=std::max(slowest,o.placed);}
        else if(o.dropped>=0) {++dropped;note("dropped",x,z);}
        else {++waiting;note("not placed",x,z);}
        if(o.shared) {++shared;note("shares cells",x,z);}
    }
    std::string text() const {
        return std::to_string(clicks)+" orders, "+std::to_string(refused)+" refused, "+std::to_string(placed)+" placed (slowest "+
            std::to_string(slowest)+" ticks), "+std::to_string(dropped)+" dropped, "+std::to_string(waiting)+" not placed, "+
            std::to_string(shared)+" on another body"+(first.empty()?"":"; first: "+first);
    }
};

// One order for every site position within `reach` cells of an idle conjurer
// standing at (bx,bz), each in a world of its own.
void idleConjurer(const sim::TypeRegistry& registry,bool legion,const char* builderName,const char* typeName,
                  float bx,float bz,int reach) {
    const auto* builder=registry.find(builderName);const auto* type=registry.find(typeName);
    if(!builder || !type) {check(false,std::string("shipped types ")+builderName+" and "+typeName);return;}
    Tally t;
    for(int dz=-reach;dz<=reach;++dz)for(int dx=-reach;dx<=reach;++dx) {
        sim::World w;setup(w,registry,legion);
        const int id=w.spawn(builder,bx,bz,std::nullopt,0);
        for(int tick=0;tick<3;++tick)w.tick(1.f/30);
        // Off the snapped centre, as a click is.
        const float x=bx+float(dx*16)+3,z=bz+float(dz*16)-5;
        Outcome o;
        if(build(w,registry,id,*type,x,z))o=follow(w,id,*type,400);
        t.add(o,x,z);
    }
    char where[64];std::snprintf(where,sizeof where," at (%.2f,%.2f)",bx,bz);
    check(t.refused==0 && t.placed==t.clicks && t.shared==0 && t.slowest<=kPlaceLimit,
          std::string(builderName)+where+" conjures "+typeName+" on every site around it",t.text());
}

// A second conjurer of the player works on a site of its own beside the
// spot. `x0..x1`, `z0..z1` (world px, `step` apart) are the clicks.
void secondConjurer(const sim::TypeRegistry& registry,bool legion,const char* builderName,const char* typeName,
                    int x0,int x1,int z0,int z1,int step) {
    const auto* builder=registry.find(builderName);const auto* type=registry.find(typeName);
    const auto* worker=registry.find("zonhand");const auto* made=registry.find("zontrain");
    if(!builder || !type || !worker || !made) {check(false,"shipped types of the second-conjurer case");return;}
    Tally t;
    for(int z=z0;z<=z1;z+=step)for(int x=x0;x<=x1;x+=step) {
        sim::World w;setup(w,registry,legion);
        const int id=w.spawn(builder,768,768,std::nullopt,0);
        const int second=w.spawn(worker,888,768,std::nullopt,0);
        if(!build(w,registry,second,*made,936,768)) {check(false,"the second conjurer takes its own order");return;}
        for(int tick=0;tick<200 && !w.unit(second)->buildSiteId;++tick)w.tick(1.f/30);
        const int secondSite=w.unit(second)->buildSiteId;
        if(!secondSite) {check(false,"the second conjurer starts its own site");return;}
        for(int tick=0;tick<30;++tick)w.tick(1.f/30);
        // Neither the worker nor its unfinished unit moves away: a site on
        // either is refused when the order is given, any other is taken.
        const Rect site=siteRect(*type,float(x),float(z));
        const bool blocked=intersects(site,footprint(*w.unit(second))) || intersects(site,footprint(*w.unit(secondSite)));
        Outcome o;
        if(build(w,registry,id,*type,float(x),float(z)))o=follow(w,id,*type,700);
        if(blocked && o.accepted) {++t.wrongAccept;t.note("taken on the worker",float(x),float(z));}
        if(!blocked && !o.accepted) {++t.wrongRefusal;t.note("refused on clear ground",float(x),float(z));}
        t.add(o,float(x),float(z));
    }
    check(t.refused>0 && t.placed>0 && t.placed+t.refused==t.clicks && t.shared==0 && !t.wrongAccept && !t.wrongRefusal,
          std::string(builderName)+" conjures "+typeName+" beside a conjurer at work: refused at the order or placed",
          t.text()+", "+std::to_string(t.wrongAccept)+" taken on the worker, "+std::to_string(t.wrongRefusal)+" refused on clear ground");
}

// The second conjurer comes to its work after the order: it walks up and
// stops on the very cell beside the site that the first conjurer, still on
// its way from far off, is heading for.
void lateWorker(const sim::TypeRegistry& registry,bool legion) {
    const auto* builder=registry.find("zonlord");const auto* type=registry.find("zonorc");
    const auto* worker=registry.find("zonhand");const auto* made=registry.find("zontrain");
    if(!builder || !type || !worker || !made) {check(false,"shipped types of the late-worker case");return;}
    sim::World w;setup(w,registry,legion);
    const int id=w.spawn(builder,500,768,std::nullopt,0);
    const int second=w.spawn(worker,800,768,std::nullopt,0);
    for(int tick=0;tick<3;++tick)w.tick(1.f/30);
    Outcome o;
    const bool taken=build(w,registry,id,*type,928,704);
    const bool working=build(w,registry,second,*made,936,768);
    if(taken)o=follow(w,id,*type,1500);
    const auto* s=w.unit(second);
    check(taken && working && s->buildSiteId && o.placed>=0 && !o.shared,
          "zonlord conjures zonorc beside a conjurer that came to work on its way there",
          !taken?"refused":o.placed>=0?"placed at tick "+std::to_string(o.placed):
          o.dropped>=0?"given up at tick "+std::to_string(o.dropped):"still held after 1500 ticks");
}

// A unit stands beside the spot. The player's own idle unit steps aside for
// the site (World::clearBuildSite); another player's stays, and a site on it
// is refused when the order is given. Either way the conjured unit is
// finished sharing no cell with it.
void standingUnit(const sim::TypeRegistry& registry,bool legion,const char* builderName,const char* typeName,
                  const char* standingName,bool own) {
    const auto* builder=registry.find(builderName);const auto* type=registry.find(typeName);
    const auto* standing=registry.find(standingName);
    if(!builder || !type || !standing) {check(false,"shipped types of the standing-unit case");return;}
    Tally t;int finished=0,finishedShared=0;
    for(int dz=-4;dz<=4;++dz)for(int dx=-4;dx<=4;++dx) {
        sim::World w;setup(w,registry,legion);
        w.setTeam(1,0);   // an ally: it draws no fire and never moves
        const int id=w.spawn(builder,768,768,std::nullopt,0);
        const int other=w.spawn(standing,896,768,std::nullopt,own?0:1);
        for(int tick=0;tick<3;++tick)w.tick(1.f/30);
        const float x=896+float(dx*16)+3,z=768+float(dz*16)-5;
        const bool blocked=!own && intersects(siteRect(*type,x,z),footprint(*w.unit(other)));
        Outcome o;
        if(build(w,registry,id,*type,x,z))o=follow(w,id,*type,400);
        if(blocked && o.accepted) {++t.wrongAccept;t.note("taken on the standing unit",x,z);}
        if(!blocked && !o.accepted) {++t.wrongRefusal;t.note("refused on clear ground",x,z);}
        t.add(o,x,z);
        if(o.placed<0)continue;
        for(int tick=0;tick<3000 && w.unit(o.site)->alive() && w.unit(o.site)->underConstruction;++tick) {
            w.player(0).mana=1000000;w.tick(1.f/30);
        }
        const auto* unit=w.unit(o.site);
        if(unit->alive() && !unit->underConstruction) {
            ++finished;
            if(sharesCells(w,o.site)) {++finishedShared;t.note("finished on another body",x,z);}
        }
    }
    check(t.placed>0 && t.placed+t.refused==t.clicks && (own ? t.refused==0 : t.refused>0) && t.shared==0 &&
          finished==t.placed && finishedShared==0 && !t.wrongAccept && !t.wrongRefusal,
          std::string(builderName)+" conjures "+typeName+" beside "+(own?"its player's idle ":"another player's ")+standingName+
          ": no unit is finished on another",
          t.text()+", "+std::to_string(finished)+" finished, "+std::to_string(finishedShared)+" of them on another body, "+
          std::to_string(t.wrongAccept)+" taken on the standing unit, "+std::to_string(t.wrongRefusal)+" refused on clear ground");
}

// The eight places round a 2x2 conjurer at (768,768), a body apart.
constexpr int kRound[8][2]={{-32,0},{32,0},{0,-32},{0,32},{-32,-32},{32,-32},{-32,32},{32,32}};

// The conjurer among its player's idle 2x2 units (`mask`: which of kRound),
// ordered to conjure a 3x3 unit on each of the nine sites centred on or
// beside itself. Every site holds the conjurer; the idle units on it step
// aside, and the conjurer walks off it, past a neighbour on the side it
// would take first, or once the units that hemmed it in have moved.
void crowdedConjurer(const sim::TypeRegistry& registry,bool legion) {
    const auto* builder=registry.find("zonlord");const auto* type=registry.find("zonorc");
    const auto* idle=registry.find("zontroll");
    if(!builder || !type || !idle) {check(false,"shipped types of the crowded case");return;}
    Tally t;
    for(const int mask : {1,2,4,8,3,12,15,255})for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
        sim::World w;setup(w,registry,legion);
        const int id=w.spawn(builder,768,768,std::nullopt,0);
        for(int k=0;k<8;++k)if(mask>>k&1)w.spawn(idle,float(768+kRound[k][0]),float(768+kRound[k][1]),std::nullopt,0);
        for(int tick=0;tick<3;++tick)w.tick(1.f/30);
        const float x=768+float(dx*16)+3,z=768+float(dz*16)-5;
        Outcome o;
        if(build(w,registry,id,*type,x,z))o=follow(w,id,*type,600);
        t.add(o,x,z);
    }
    check(t.refused==0 && t.placed==t.clicks && t.shared==0,
          "zonlord among its player's idle units conjures zonorc where it stands",t.text());
}

// A conjurer that is not idle, so that no test would have it step aside:
// walking past, or at work on a unit beside it with the new order queued
// behind that job. It is ordered to conjure on each of the nine sites centred
// on or beside itself, all of which hold its own body. The cursor's test and
// the order's both leave the order's own conjurer out: the order is taken
// (but for a site on the unit it is still making), it steps off the site and
// the unit is conjured.
void busyConjurer(const sim::TypeRegistry& registry,bool legion,bool atWork) {
    const auto* builder=registry.find("zonhand");const auto* type=registry.find("zontroll");
    const auto* made=registry.find("zontrain");
    if(!builder || !type || !made) {check(false,"shipped types of the busy-conjurer case");return;}
    Tally t;
    for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
        sim::World w;setup(w,registry,legion);
        const int id=w.spawn(builder,768,768,std::nullopt,0);
        for(int tick=0;tick<3;++tick)w.tick(1.f/30);
        int first=0;
        if(atWork) {
            if(!build(w,registry,id,*made,848,768)) {check(false,"the conjurer takes its first order");return;}
            for(int tick=0;tick<200 && !w.unit(id)->buildSiteId;++tick)w.tick(1.f/30);
            first=w.unit(id)->buildSiteId;
            if(!first) {check(false,"the conjurer starts its first site");return;}
        } else {
            net::Command walk{};
            walk.kind=net::Cmd::Move;walk.player=0;walk.unitId=id;walk.x=1100;walk.z=900;
            sim::applyCommand(w,registry,walk);
            for(int tick=0;tick<30;++tick)w.tick(1.f/30);
        }
        const auto* b=w.unit(id);
        const Rect body=footprint(*b);   // wherever its walk has taken it
        const float x=float((body.x+dx)*16+body.w*8)+3,z=float((body.z+dz)*16+body.h*8)-5;
        const Rect site=siteRect(*type,x,z);
        if((atWork ? b->buildSiteId!=first : b->orders.empty()) || !intersects(site,body)) {
            check(false,"the busy conjurer stands on the site it is given");return;
        }
        const bool blocked=first && intersects(site,footprint(*w.unit(first)));
        Outcome o;
        if(build(w,registry,id,*type,x,z,atWork))o=follow(w,id,*type,atWork?3000:400);
        if(blocked && o.accepted) {++t.wrongAccept;t.note("taken on the unit it is making",x,z);}
        if(!blocked && !o.accepted) {++t.wrongRefusal;t.note("refused on clear ground",x,z);}
        t.add(o,x,z);
    }
    check(t.placed>0 && t.placed+t.refused==t.clicks && t.shared==0 && !t.wrongAccept && !t.wrongRefusal,
          std::string("zonhand ")+(atWork?"at work, the order queued":"walking past")+", conjures zontroll where it stands",
          t.text()+", "+std::to_string(t.wrongAccept)+" taken on the unit it is making, "+
          std::to_string(t.wrongRefusal)+" refused on clear ground");
}

// Units stand all round the conjurer (`foreign`: which of kRound are another
// player's), which is ordered to conjure a unit of its own size where it
// stands: nothing but the conjurer is on the site, so the order is taken, and
// the conjurer cannot leave the site. Its player's idle units are not on the
// site, so the site never asks them to leave: one makes room on a cell the
// conjurer can step to and the unit is conjured, also when another player's
// unit stands on the cell it would take first (kRound[0], on its left).
// Another player's do not move: with them all round it the order must still
// end, given up after the 31 tries, not held.
void hemmedConjurer(const sim::TypeRegistry& registry,bool legion,int foreign) {
    const auto* builder=registry.find("zonhand");const auto* type=registry.find("zontroll");
    if(!builder || !type) {check(false,"shipped types of the hemmed-in case");return;}
    sim::World w;setup(w,registry,legion);
    w.setTeam(1,0);
    const int id=w.spawn(builder,768,768,std::nullopt,0);
    for(int k=0;k<8;++k)w.spawn(type,float(768+kRound[k][0]),float(768+kRound[k][1]),std::nullopt,foreign>>k&1);
    for(int tick=0;tick<3;++tick)w.tick(1.f/30);
    Outcome o;
    if(build(w,registry,id,*type,768,768))o=follow(w,id,*type,1500);
    const bool room=foreign!=255;
    check(o.accepted && (room ? o.placed>=0 : o.placed>=0 || o.dropped>=0) && !o.shared,
          std::string("a conjurer hemmed in on its own site by ")+
          (foreign==0?"its player's idle units":room?"its player's idle units and one of another player's":"another player's units")+
          (room?": one makes room and the unit is conjured":": the order ends"),
          !o.accepted?"refused":o.placed>=0?"placed at tick "+std::to_string(o.placed):
          o.dropped>=0?"given up at tick "+std::to_string(o.dropped):"still held after 1500 ticks");
}

// The computer player picks the spot (ai::Controller's ring search round the
// conjurer), for the conjurer at `phases` x `phases` places inside a cell.
void aiRing(const sim::TypeRegistry& registry,bool legion,const char* typeName,int phases) {
    const auto* builder=registry.find("zonlord");const auto* type=registry.find(typeName);
    if(!builder || !type) {check(false,"shipped types of the computer player's case");return;}
    Tally t;int silent=0;
    for(int pz=0;pz<phases;++pz)for(int px=0;px<phases;++px) {
        sim::World w;setup(w,registry,legion);
        const float bx=768+float(px*16)/float(phases)+0.37f,bz=768+float(pz*16)/float(phases)+0.61f;
        const int id=w.spawn(builder,bx,bz,std::nullopt,0);
        ai::Profile profile;profile.weight[type->id]=100;
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{1400,768}});
        bool ordered=false;float x=0,z=0;
        Outcome o;
        for(uint32_t tick=0;tick<600 && !ordered;++tick) {
            w.player(0).mana=1000000;w.player(0).income=100;
            controller.tick(w,tick,[&](const net::Command& c) {
                if(ordered)return;
                sim::applyCommand(w,registry,c);
                if(c.kind!=net::Cmd::Build || c.unitId!=id || type->id!=c.type)return;
                ordered=true;x=c.x;z=c.z;
                const auto* b=w.unit(id);
                o.accepted=std::any_of(b->orders.begin(),b->orders.end(),[&](const sim::Order& q){return q.buildType==type;});
            });
            if(!ordered)w.tick(1.f/30);
        }
        if(!ordered) {++silent;continue;}
        if(o.accepted)o=follow(w,id,*type,400);
        t.add(o,x,z);
    }
    check(silent==0 && t.refused==0 && t.placed==t.clicks && t.shared==0 && t.slowest<=kPlaceLimit,
          std::string("the computer player's Beast Lord conjures ")+typeName+" where it picked",
          t.text()+", "+std::to_string(silent)+" gave no order");
}
}

int main(int argc,char** argv) {
    if(argc<2) {std::fprintf(stderr,"usage: conjure_site_test <retail-install-dir> [--legion]\n");return 2;}
    const bool legion=argc>2 && std::strcmp(argv[2],"--legion")==0;
    auto vfs=hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,false);

    // A cell-centred conjurer and one off the grid.
    for(const auto [bx,bz] : {std::pair{768.f,768.f},std::pair{774.5f,763.25f}}) {
        idleConjurer(registry,legion,"zonlord","zonorc",bx,bz,5);    // Beast Lord, 3x3 Jungle Orc
        idleConjurer(registry,legion,"zonlord","zonroc",bx,bz,6);    // 6x6 Roc
        idleConjurer(registry,legion,"zonhand","zontroll",bx,bz,4);  // Beast Handler, 2x2 Troll
        idleConjurer(registry,legion,"zontrain","zongryp",bx,bz,5);  // Beast Tamer, 4x4 Gryphon
        idleConjurer(registry,legion,"zonhunt","zonhand",bx,bz,4);   // the flying monarch
    }
    // Round the worker at (888,768) and its site at (936,768).
    secondConjurer(registry,legion,"zonlord","zonorc",832,992,704,832,8);
    secondConjurer(registry,legion,"zontrain","zonharp",848,976,720,816,8);
    lateWorker(registry,legion);
    standingUnit(registry,legion,"zonhand","zontroll","zongiant",true);
    standingUnit(registry,legion,"zonhand","zontroll","zongiant",false);
    standingUnit(registry,legion,"zonlord","zonorc","zongiant",false);
    crowdedConjurer(registry,legion);
    busyConjurer(registry,legion,false);
    busyConjurer(registry,legion,true);
    hemmedConjurer(registry,legion,0);
    hemmedConjurer(registry,legion,1);
    hemmedConjurer(registry,legion,255);
    aiRing(registry,legion,"zonorc",8);
    aiRing(registry,legion,"zonroc",8);
    check(cursorDiffers==0,"the build cursor's test and the order agree on every click",
          std::to_string(cursorDiffers)+" differ"+(cursorFirst.empty()?"":"; first: "+cursorFirst));

    std::printf("conjure_site_test%s: %d checks, %d failed\n",legion?" --legion":"",cases,failures);
    return failures?1:0;
}
