// Our levels' army orders, as a real ai::Controller's commands on constructed
// worlds: a fighter that makes no headway on its attack-move is stopped and sent
// again, one on the march or standing at a fight is not; a unit whose queue still
// holds the attack it was given is left alone; an attack the sim would refuse is
// not ordered; and the side stays the seat's when its Monarch is gone.
#include "ai/ai.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace ta;
static int fails=0;
static void check(bool ok,const char* label) {
    std::printf("[%s] %s\n",ok ? "PASS" : "FAIL",label);if (!ok) ++fails;
}
static sim::UnitType fighter() {
    sim::UnitType t;t.id="fighter";t.maxHp=100;t.maxVel=sim::Fixed::fromInt(2);
    t.canMove=true;t.footX=t.footZ=2;t.turnRate=t.turnInPlaceRate=1000;
    t.weapon.damage=10;t.weapon.range=120;t.weapons={t.weapon};
    t.defaultMove=1;t.defaultFire=2;t.buildTime=1;t.buildMovementCode=1;return t;
}
// A building that pays mana and never falls: what a strike goes for.
static sim::UnitType lodestone() {
    auto t=fighter();t.id="lode";t.maxVel={};t.canMove=false;t.onMana=true;t.buildMovementCode=0;
    t.weapon.damage=0;t.weapons.clear();t.buildCost=300;t.maxHp=30000;return t;
}
// A flat 4096 px map, FULL VISION: every unit is in sight, ticked or not.
static void terrain(sim::World& w) {
    w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    w.setRoomVision(true,true);
}
static void pastStart(sim::World& w) {w.resumeClocks(1,w.gameRngState());}
static std::vector<net::Command> think(ai::Controller& ai,sim::World& w,uint32_t tick) {
    std::vector<net::Command> out;ai.tick(w,tick,[&](const auto& c){out.push_back(c);});return out;
}
static int count(const std::vector<net::Command>& cs,net::Cmd kind) {
    return int(std::count_if(cs.begin(),cs.end(),[&](const auto& c){return c.kind==kind;}));
}
static void apply(sim::World& w,const sim::TypeRegistry& registry,const std::vector<net::Command>& cs) {
    for (const auto& c:cs) sim::applyCommand(w,registry,c);
}
static std::vector<int> army(sim::World& w,const sim::UnitType& soldier,int n,float x=400,float z=800) {
    std::vector<int> ids;
    for (int i=0;i<n;++i) ids.push_back(w.spawn(&soldier,x+float(i%7)*40,z+float(i/7)*40,0,0));
    return ids;
}

// A wave's fighters hold their attack-move until it ends, and nothing promises
// it does. One that has made no headway for a minute is stopped, and goes out
// again from the next think; each stop that does not free it doubles the wait.
static void stalledMarch(const sim::TypeRegistry& registry,const ai::Profile& profile) {
    const auto soldier=fighter(),lode=lodestone();
    for (auto level:{ai::Difficulty::Normal,ai::Difficulty::Absurd}) {
        const uint32_t period=level==ai::Difficulty::Normal ? 30 : 20;
        const char* name=level==ai::Difficulty::Normal ? "Normal" : "Absurd";
        char label[160];
        auto say=[&](const char* what) {std::snprintf(label,sizeof label,"%s: %s",name,what);return label;};
        sim::World w;terrain(w);w.player(0).income=100;
        const auto ids=army(w,soldier,35);
        w.spawn(&lode,3400,3400,0,1);
        pastStart(w);
        ai::Controller controller(0,registry,profile,1,level,{{3400,800}});
        auto cs=think(controller,w,0);
        check(count(cs,net::Cmd::AttackMove)==35,say("the push marches on the enemy's lodestone"));
        apply(w,registry,cs);
        // The world is not ticked: every fighter stands where it was with its
        // attack-move, as bodies that lock each other on the way do.
        size_t early=0;
        uint32_t tick=period;
        for (;tick<60*30;tick+=period) early+=think(controller,w,tick).size();
        check(early==0,say("a march that has stood for less than a minute is left alone"));
        cs=think(controller,w,tick);
        check(count(cs,net::Cmd::Stop)==35 && cs.size()==35,
              say("a fighter that has made no headway for a minute on its attack-move is stopped"));
        apply(w,registry,cs);
        const uint32_t stopped=tick;
        cs=think(controller,w,tick+=period);
        check(count(cs,net::Cmd::AttackMove)==35 && count(cs,net::Cmd::Stop)==0,
              say("... and is sent out again at the next think"));
        apply(w,registry,cs);
        const uint32_t resent=tick;
        uint32_t second=0;
        for (tick+=period;tick<stopped+10*60*30 && !second;tick+=period) {
            cs=think(controller,w,tick);
            if (count(cs,net::Cmd::Stop)) second=tick;
            apply(w,registry,cs);
        }
        check(second>=resent+2*60*30-period && second<=resent+2*60*30,
              say("a unit the fresh order did not free waits two minutes for the next stop"));
        // Headway ends the back-off: once it has come 64 px, the wait is a minute again.
        for (int id:ids) w.unit(id)->x=w.unit(id)->x+sim::Fixed::fromInt(80);
        cs=think(controller,w,tick);
        apply(w,registry,cs);
        const uint32_t moved=tick;
        uint32_t third=0;
        for (tick+=period;tick<moved+10*60*30 && !third;tick+=period)
            if (count(think(controller,w,tick),net::Cmd::Stop)) third=tick;
        check(third>=moved+60*30-period && third<=moved+60*30+2*period,
              say("after headway the wait is a minute again"));
    }
}

// A column that queues through a gap in a wall, marches on and lays siege, under
// the real sim in both pathfinding modes: every unit of it makes headway or
// stands at the fight, and none is stopped while the target stands.
static void marchAndSiege(const sim::TypeRegistry& registry,const ai::Profile& profile) {
    auto soldier=fighter();soldier.maxVel=sim::Fixed::fromInt(1);
    const auto lode=lodestone();
    for (auto mode:{sim::PathfindingMode::Retail,sim::PathfindingMode::Legion}) {
        const char* name=mode==sim::PathfindingMode::Retail ? "Retail pathfinding" : "Legion pathfinding";
        char label[160];
        auto say=[&](const char* what) {std::snprintf(label,sizeof label,"%s: %s",name,what);return label;};
        sim::World w;
        // A wall of water across the map at x 1984-2048 with a ford 64 px wide.
        std::vector<uint8_t> h(256*256,100);
        for (int z=0;z<256;++z) for (int x=124;x<128;++x) if (z<52 || z>=56) h[size_t(z)*256+size_t(x)]=0;
        w.setTerrain(std::move(h),256,256,20);w.setRoomVision(true,true);
        w.setPathService(true);w.setPathfindingMode(mode);
        w.player(0).income=100;
        const auto ids=army(w,soldier,35);
        const int target=w.spawn(&lode,3400,900,0,1);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,900}});
        int stops=0,sent=0,queued=0,sieging=0;
        uint32_t tick=0;
        for (;tick<8000 && w.unit(target) && w.unit(target)->alive();++tick) {
            const auto cs=think(controller,w,tick);
            stops+=count(cs,net::Cmd::Stop);
            sent+=count(cs,net::Cmd::AttackMove);
            apply(w,registry,cs);
            w.tick(1.f/30.f);
            if (tick%300) continue;
            // Marchers held up before the ford, and standing behind the siege line.
            int before=0,behind=0;
            for (int id:ids) {
                const auto* u=w.unit(id);
                const bool marching=std::any_of(u->orders.begin(),u->orders.end(),[](const auto& o){return o.attackMove;}) &&
                    std::none_of(u->orders.begin(),u->orders.end(),[](const auto& o){return o.targetId!=0;});
                if (marching && u->x.toFloat()<1984 && tick>=1500) ++before;
                if (marching && u->x.toFloat()>3000) ++behind;
            }
            queued=std::max(queued,before);sieging=std::max(sieging,behind);
        }
        const bool through=std::all_of(ids.begin(),ids.end(),[&](int id){return w.unit(id)->x.toFloat()>2048;});
        const auto* t=w.unit(target);
        std::printf("  %s: %d still short of the ford after 50 s, %d at most standing behind the siege line, %u ticks\n",
                    name,queued,sieging,tick);
        check(sent>=35 && through,say("the whole column gets through the ford"));
        check(queued>=1,say("... queuing for it"));
        check(!t || !t->alive() || t->hp.toFloat()<25000,say("... and lays siege to the lodestone beyond"));
        check(sieging>=1,say("... some of it waiting behind the siege line"));
        check(stops==0,say("no fighter of a column on the march or at its siege is stopped"));
    }
}

// Fighters waiting behind a siege line stand with their attack-move: a fight
// around them, or around the point they march on, gives them four minutes
// where another standing march gets one. Not for ever: the bodies that lock
// each other beside a long fight are stopped and sent again too.
static void siegeQueue(const sim::TypeRegistry& registry,const ai::Profile& profile) {
    const auto soldier=fighter(),lode=lodestone();
    {
        sim::World w;terrain(w);w.player(0).income=100;
        army(w,soldier,35);
        const auto front=army(w,soldier,3,3300,3300);   // within reach of the lodestone
        const int target=w.spawn(&lode,3400,3400,0,1);
        pastStart(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        auto cs=think(controller,w,0);
        check(count(cs,net::Cmd::AttackMove)==35 && count(cs,net::Cmd::Attack)==3,
              "the push marches on the lodestone and the three beside it attack it");
        apply(w,registry,cs);
        int stops=0;
        uint32_t tick=30;
        for (;tick<4*60*30;tick+=30) stops+=count(think(controller,w,tick),net::Cmd::Stop);
        check(stops==0,"fighters standing behind a siege line are left with their orders for four minutes");
        cs=think(controller,w,tick);
        check(count(cs,net::Cmd::Stop)==35 && cs.size()==35,"... then stopped");
        apply(w,registry,cs);
        cs=think(controller,w,tick+=30);
        check(count(cs,net::Cmd::AttackMove)==35,"... and sent again at the next think");
        apply(w,registry,cs);
        // Two minutes on the siege ends: the lodestone falls and its attackers
        // have no order. Away from a fight the second wait is two minutes, not
        // eight, and the march has stood that long.
        const uint32_t over=tick+2*60*30;
        for (tick+=30;tick<over;tick+=30) stops+=count(think(controller,w,tick),net::Cmd::Stop);
        check(stops==0,"while the siege lasts they are not stopped again");
        w.unit(target)->deadFor=0;
        for (int id:front) w.stop(id);
        check(count(think(controller,w,over),net::Cmd::Stop)==35,
              "once the fight is over, a march that has stood its time is stopped at once");
    }
    {
        // A fight near home excuses the fighters standing around it, not one
        // standing far from it whose march goes elsewhere.
        sim::World w;terrain(w);w.player(0).income=100;
        const auto ids=army(w,soldier,35);
        const int far=w.spawn(&soldier,400,3000,0,0);
        w.spawn(&lode,3400,3400,0,1);
        pastStart(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        auto cs=think(controller,w,0);
        check(count(cs,net::Cmd::AttackMove)==36,"the push marches, the fighter standing apart with it");
        apply(w,registry,cs);
        auto post=lodestone();post.id="post";post.onMana=false;
        const int enemy=w.spawn(&post,800,700,0,1);
        for (int i=0;i<3;++i) w.attack(ids[size_t(i)],enemy,false);
        std::vector<net::Command> stops;
        for (uint32_t tick=30;tick<=60*30+30;tick+=30)
            for (const auto& c:think(controller,w,tick)) if (c.kind==net::Cmd::Stop) stops.push_back(c);
        check(stops.size()==1 && stops.front().unitId==far,
              "a fight elsewhere does not excuse a fighter standing far from it");
    }
}

// The sim puts legs of its own in front of an attack: the walk out of a weapon's
// minimum range, or to a clear shot (World::repositionForShot inserts a plain
// move at the head of the queue). The attack behind it is still the unit's
// order, and the AI does not send it again.
static void heldAttack(const sim::TypeRegistry& registry,const ai::Profile& profile) {
    const auto soldier=fighter(),lode=lodestone();
    sim::World w;terrain(w);w.player(0).income=100;
    const int id=w.spawn(&soldier,1000,1000,0,0);
    const int target=w.spawn(&lode,1100,1000,0,1);
    pastStart(w);
    ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
    auto cs=think(controller,w,0);
    check(cs.size()==1 && cs.front().kind==net::Cmd::Attack && cs.front().unitId==id && cs.front().targetId==target,
          "a fighter beside the enemy's lodestone is ordered to attack it");
    apply(w,registry,cs);
    // The back-off leg, as repositionForShot builds it.
    sim::Order step{sim::Fixed::fromInt(900),sim::Fixed::fromInt(1000),0};
    step.goal=true;step.groundMission=true;
    auto& orders=w.unit(id)->orders;
    orders.insert(orders.begin(),step);
    int again=0;
    for (uint32_t tick=30;tick<=300;tick+=30) again+=int(think(controller,w,tick).size());
    check(again==0,"a unit walking to a firing spot in front of the attack it was given is not ordered again");
    // The walk done, the attack leads again: still nothing to add.
    orders.erase(orders.begin());
    check(think(controller,w,330).empty(),"... nor while it attacks");
    // A unit on a plain move alone is free, as before.
    w.stop(id);
    w.order(id,900,1000,false);
    cs=think(controller,w,360);
    check(cs.size()==1 && cs.front().kind==net::Cmd::Attack && cs.front().unitId==id,
          "a striker on a plain move with no attack behind it is free for its next order");
    {
        // The Monarch fights back by the same reading: it is ordered onto the
        // armed enemy beside it once, not again on each think of its walk.
        auto king=fighter();king.id="king";king.commander=true;king.isBuilder=true;
        sim::World v;terrain(v);
        const int monarch=v.spawn(&king,1000,1000,0,0);
        const int enemy=v.spawn(&soldier,1100,1000,0,1);
        pastStart(v);
        ai::Controller ai(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        cs=think(ai,v,0);
        check(cs.size()==1 && cs.front().kind==net::Cmd::Attack && cs.front().unitId==monarch && cs.front().targetId==enemy,
              "a Monarch with an armed enemy beside it is ordered to attack it");
        apply(v,registry,cs);
        auto& queue=v.unit(monarch)->orders;
        queue.insert(queue.begin(),step);
        again=0;
        for (uint32_t tick=30;tick<=300;tick+=30) again+=int(think(ai,v,tick).size());
        check(again==0,"... and not again while it walks to a firing spot in front of that attack");
    }
}

// An Attack is ordered only where the sim would take it (World::canAttackTarget):
// a charm takes a finished mobile unit that is not immune, with its side below
// the unit cap; a stone weapon one that can be stoned. Otherwise the unit
// fight-moves to the site and takes what it can there.
static void refusedAttacks(const sim::TypeRegistry& registry,const ai::Profile& profile) {
    auto harpy=fighter();harpy.id="harpy";harpy.buildCost=1000;
    harpy.weapon.damage=1;harpy.weapon.mindControl=true;harpy.weapons={harpy.weapon};
    auto basilisk=fighter();basilisk.id="basilisk";basilisk.buildCost=1000;
    basilisk.weapon.status=sim::Weapon::Status::Stoned;basilisk.weapons={basilisk.weapon};
    auto troll=fighter();troll.id="troll";troll.weapon.damage=0;troll.weapons.clear();troll.buildCost=100;
    auto immune=troll;immune.id="immune";immune.cantBeCaptured=true;immune.cantBeStoned=true;
    const auto lode=lodestone();
    // The one order `caster` gets with `victim` 100 px from it.
    auto order=[&](const sim::UnitType& caster,const sim::UnitType& victim,int unitCap) {
        sim::World w;terrain(w);w.player(0).income=100;
        if (unitCap) w.setUnitCap(unitCap);
        const int id=w.spawn(&caster,1000,1000,0,0);
        const int target=w.spawn(&victim,1100,1000,0,1);
        pastStart(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        const auto cs=think(controller,w,0);
        if (cs.size()!=1 || cs.front().unitId!=id) return 0;
        return cs.front().kind==net::Cmd::Attack && cs.front().targetId==target ? 1 :
               cs.front().kind==net::Cmd::AttackMove ? 2 : 0;
    };
    check(order(harpy,troll,0)==1,"a Harpy is ordered to charm a mobile unit beside it");
    check(order(harpy,lode,0)==2,"a Harpy beside a lodestone fight-moves there: a charm takes no building");
    check(order(harpy,immune,0)==2,"... nor a unit that cannot be captured");
    check(order(harpy,troll,1)==2,"... nor anything while its side is at the unit cap");
    check(order(basilisk,troll,0)==1,"a Basilisk is ordered to stone a mobile unit beside it");
    check(order(basilisk,lode,0)==2,"a Basilisk beside a lodestone fight-moves there: nothing stones a building");
    check(order(basilisk,immune,0)==2,"... nor a unit that cannot be stoned");
    check(order(fighter(),lode,0)==1 && order(fighter(),immune,1)==1,
          "a unit whose weapon does damage attacks either, at the unit cap too");
    {
        // Standing where that fight-move ends, the Harpy is left there: the
        // same order on every think would end as it began.
        sim::World w;terrain(w);w.player(0).income=100;
        const int id=w.spawn(&harpy,1000,1000,0,0);
        w.spawn(&lode,1100,1000,0,1);
        pastStart(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        auto cs=think(controller,w,0);
        const bool sent=cs.size()==1 && cs.front().kind==net::Cmd::AttackMove;
        if (sent) { w.unit(id)->x=sim::Fixed::fromFloat(cs.front().x);w.unit(id)->z=sim::Fixed::fromFloat(cs.front().z); }
        size_t again=0;
        for (uint32_t tick=30;tick<=90;tick+=30) again+=think(controller,w,tick).size();
        check(sent && again==0,"a Harpy standing at the lodestone it cannot take is not sent there again on every think");
    }
}

// The seat's faction is its Monarch's, for the whole game. With the Monarch dead
// it used to be read off the lowest-numbered mobile builder -- which a captured
// one can be: a Zhon seat then counted as a side with factory buildings, and
// stopped replacing the producers that are its factories.
struct Roster {
    std::shared_ptr<hpi::Vfs::Files> files=std::make_shared<hpi::Vfs::Files>();
    hpi::Vfs vfs;
    sim::TypeRegistry reg;
    ai::Profile profile;
    void file(const std::string& path,const std::string& text) {
        std::string lines;
        for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
        (*files)[path]={lines.begin(),lines.end()};
    }
    void type(const std::string& id,const std::string& fields) {
        file("units/"+id+".fbi","[UNITINFO]{unitname="+id+";objectname="+id+
            ";maxdamage=100;footprintx=2;footprintz=2;buildtime=30;buildcost=10;sightdistance=600;"+fields+"}");
    }
    const sim::UnitType* operator()(const char* id) const {return reg.find(id);}
    Roster() {
        const std::string mobile="canmove=1;maxvelocity=1;";
        // ARA has a factory building; ZON has none: its handlers and tamers are its factories.
        type("arakeep","side=ARA;builder=1;workertime=30;");
        type("aramason","side=ARA;builder=1;workertime=30;"+mobile);
        type("arasword","side=ARA;"+mobile);
        file("canbuild/arakeep/arasword.tdf","[CANBUILD]{sortbias=0;}");
        file("canbuild/aramason/arakeep.tdf","[CANBUILD]{sortbias=0;}");
        type("zonking","side=ZON;builder=1;commander=1;workertime=30;"+mobile);
        type("zongod","side=ZON;"+mobile);
        type("zonhand","side=ZON;builder=1;workertime=30;"+mobile);
        type("zontame","side=ZON;builder=1;workertime=30;"+mobile);
        type("zongob","side=ZON;"+mobile);
        file("canbuild/zonking/zonhand.tdf","[CANBUILD]{sortbias=0;}");
        file("canbuild/zonhand/zontame.tdf","[CANBUILD]{sortbias=0;}");
        file("canbuild/zonhand/zongob.tdf","[CANBUILD]{sortbias=1;}");
        file("canbuild/zontame/zongob.tdf","[CANBUILD]{sortbias=0;}");
        vfs.setMapFiles(files);
        reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
        for (const char* id : {"arasword","zongob"}) {
            auto* t=const_cast<sim::UnitType*>(reg.find(id));
            t->weapon.damage=10;t->weapon.range=120;t->weapons={t->weapon};
        }
        for (const char* id : {"arakeep","aramason","arasword","zonhand","zontame","zongob"}) profile.weight[id]=10;
    }
};
static void seatSide() {
    Roster r;
    // Orders to conjure a producer: the Monarch's Beast Handler, a handler's Tamer.
    auto producers=[](const std::vector<net::Command>& cs) {
        return int(std::count_if(cs.begin(),cs.end(),[](const auto& c) {
            return c.kind==net::Cmd::Build && (std::string(c.type)=="zonhand" || std::string(c.type)=="zontame");
        }));
    };
    // A Zhon seat on 200 income wants six producers and has four. `captured`: an
    // Aramon constructor with a lower unit id than any of them is the seat's too.
    struct Seat { sim::World w; int monarch=0; };
    auto seat=[&](Seat& s,bool captured,bool monarch) {
        terrain(s.w);s.w.player(0).income=200;s.w.player(0).mana=5000;
        if (captured) s.w.spawn(r("aramason"),700,1000,0,0);
        if (monarch) s.monarch=s.w.spawn(r("zonking"),1000,1000,0,0);
        for (int i=0;i<4;++i) s.w.spawn(r("zonhand"),1000+float(i)*200,1400,0,0);
        for (int i=0;i<14;++i) s.w.spawn(r("zongob"),1000+float(i)*40,1800,0,0);
        pastStart(s.w);
    };
    {
        Seat s;seat(s,true,true);
        ai::Controller controller(0,r.reg,r.profile,1,ai::Difficulty::Normal);
        check(producers(think(controller,s.w,0))>0,"a Zhon seat short of producers conjures more");
        s.w.unit(s.monarch)->deadFor=0;
        check(producers(think(controller,s.w,30))>0,
              "with its Monarch dead and a captured constructor its lowest-numbered builder, it still does: the side is the seat's");
    }
    {
        // Controls: no captured builder; and nothing to tell the seat by (no
        // Monarch seen, no god), where the builders' side is read as before.
        Seat s;seat(s,false,true);
        ai::Controller controller(0,r.reg,r.profile,1,ai::Difficulty::Normal);
        think(controller,s.w,0);
        s.w.unit(s.monarch)->deadFor=0;
        check(producers(think(controller,s.w,30))>0,"a Zhon seat with no captured builder conjures producers after its Monarch as before");
        Seat t;seat(t,true,false);
        ai::Controller unknown(0,r.reg,r.profile,1,ai::Difficulty::Normal);
        check(producers(think(unknown,t.w,0))==0,"with neither Monarch nor god to tell the seat by, the lowest-numbered builder's side is read");
    }
}

// The audited defect itself, on the shipped data (Crusades balance): a Veruna
// Catapult ordered onto a unit inside its minimum range walks out to fire. The
// AI used to send the Attack again on every think of the walk, the sim's three
// walks were spent in two seconds and every later order was dropped: no shot.
static void catapultBackOff(const char* dataRoot) {
    auto vfs=hpi::mountRetailRoot(dataRoot);
    sim::TypeRegistry reg;sim::setupRegistry(reg,vfs,true);
    const auto* catapult=reg.find("verpult");
    const auto* tamer=reg.find("zontrain");
    if (!catapult || !tamer || catapult->weapons.empty() || catapult->weapons.front().minRange<=0) {
        check(false,"the Crusades roster has a Veruna Catapult with a minimum range");
        return;
    }
    // A Beast Tamer that neither moves nor fights nor falls.
    auto victim=*tamer;victim.maxVel={};victim.weapon.damage=0;victim.weapons.clear();victim.maxHp=30000;
    for (auto mode:{sim::PathfindingMode::Retail,sim::PathfindingMode::Legion}) {
        const char* name=mode==sim::PathfindingMode::Retail ? "Retail pathfinding" : "Legion pathfinding";
        char label[200];
        auto say=[&](const char* what) {std::snprintf(label,sizeof label,"%s: %s",name,what);return label;};
        sim::World w;
        w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
        w.setMapPlacementFeatures(std::vector<uint16_t>(256*256,0xffff),{});
        w.buildNavClasses(reg);w.setPathService(true);w.setPathfindingMode(mode);
        w.setRoomVision(true,true);
        w.player(0).income=100;
        const int id=w.spawn(catapult,2000,2000,0,0);
        for (int i=0;i<30;++i) w.tick(1.f/30.f);
        const int target=w.spawn(&victim,2000+float(catapult->weapons.front().minRange)/2,2000,0,1);
        ai::Profile profile;
        ai::Controller controller(0,reg,profile,1,ai::Difficulty::Normal,{{3400,800}});
        int attacks=0,shots=0,walks=0;
        for (uint32_t tick=30;tick<30+1500;++tick) {
            const auto cs=think(controller,w,tick);
            for (const auto& c:cs) if (c.unitId==id && c.kind==net::Cmd::Attack && c.targetId==target) ++attacks;
            apply(w,reg,cs);
            w.tick(1.f/30.f);
            const auto* u=w.unit(id);
            if (u->justFired) ++shots;
            walks=std::max(walks,int(u->repositions));
        }
        std::printf("  %s: %d Attack orders, %d walks, %d shots, target %.0f of 30000 hp\n",name,attacks,walks,shots,
                    w.unit(target)->hp.toFloat());
        check(attacks==1,say("the Catapult is ordered onto the unit inside its minimum range once"));
        check(walks==1 && shots>0 && w.unit(target)->hp.toFloat()<30000,say("it backs off once and fires"));
    }
}

int main(int argc,char** argv) {
    if (argc>1) {
        catapultBackOff(argv[1]);
        std::printf("%d failed\n",fails);
        return fails ? 1 : 0;
    }
    sim::TypeRegistry empty;ai::Profile profile;
    stalledMarch(empty,profile);
    marchAndSiege(empty,profile);
    siegeQueue(empty,profile);
    heldAttack(empty,profile);
    refusedAttacks(empty,profile);
    seatSide();
    std::printf("%d failed\n",fails);
    return fails ? 1 : 0;
}
