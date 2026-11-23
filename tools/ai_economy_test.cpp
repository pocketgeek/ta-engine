// Our levels' economy and army make-up (Hard's and Absurd's most of all, and
// the factory rule Normal shares with them), as decisions of a real
// ai::Controller on constructed bases: what its producers are ordered to build,
// and where, in one think.
#include "ai/ai.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>
using namespace ta;
static int fails=0;
static void check(bool ok,const char* label) {
    std::printf("[%s] %s\n",ok ? "PASS" : "FAIL",label);if (!ok) ++fails;
}
// A flat 4096 px map under the room's default FOG OF WAR, EXPLORED: the whole
// map's ground (its deposits too) is known from the start, units only by sight.
static void terrain(sim::World& w) {
    w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    w.setRoomVision(true,false);
}
// The sim's per-player sight for the units as they stand, on a world past its
// first tick (before it our levels see nothing: Controller::updateKnowledge).
static void sight(sim::World& w) {w.updateNavigationExploration();w.resumeClocks(1,w.gameRngState());}
static std::vector<net::Command> think(ai::Controller& ai,sim::World& w,uint32_t tick=0) {
    std::vector<net::Command> out;ai.tick(w,tick,[&](const auto& c){out.push_back(c);});
    return out;
}
static int orders(const std::vector<net::Command>& cs,net::Cmd kind,const char* type) {
    return int(std::count_if(cs.begin(),cs.end(),[&](const auto& c){return c.kind==kind && std::string(c.type)==type;}));
}
// The first Build order for `type` (a copy: the commands may be a temporary).
static std::optional<net::Command> build(const std::vector<net::Command>& cs,const char* type) {
    const auto it=std::find_if(cs.begin(),cs.end(),[&](const auto& c){return c.kind==net::Cmd::Build && std::string(c.type)==type;});
    return it==cs.end() ? std::nullopt : std::optional<net::Command>(*it);
}
// A producer that already has work, which the AI leaves alone: a factory with
// a unit queued, a mobile builder with a build order.
static void busy(sim::World& w,int id,const sim::UnitType* making) {
    auto& u=*w.unit(id);
    if (u.type->isStructure()) { u.buildQueue.push_back(making); return; }
    sim::Order o;o.buildType=making;u.orders.push_back(o);
}
// Short of mana as retail's trigger reads it: over the last second more mana
// was asked for than came in (Player::displayResources; per tick: earned, asked).
static void shortOfMana(sim::World& w,int player,float mana) {
    auto& me=w.player(player);
    me.mana=mana;
    for (auto& sample:me.displayResources.samples) sample={me.income/30,me.income/30+10};
}

// The roster: ARA has a factory building (keep), a constructor (mason) and a
// miner whose whole menu is the lodestone; ZON has no factory building, so its
// handlers are its factories and its lodestone builders at once.
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
        type("arakeep","side=ARA;builder=1;workertime=30;");
        type("aramason","side=ARA;builder=1;workertime=30;"+mobile);
        type("araminer","side=ARA;builder=1;workertime=30;"+mobile);
        type("aralode","side=ARA;yardmap=SSSS;mogriumincome=10;mogriumstorage=1000;");
        type("arasword","side=ARA;"+mobile);
        type("arabow","side=ARA;"+mobile);
        type("arahawk","side=ARA;"+mobile);
        file("canbuild/arakeep/arasword.tdf","[CANBUILD]{sortbias=0;}");
        file("canbuild/aramason/arakeep.tdf","[CANBUILD]{sortbias=0;}");
        file("canbuild/araminer/aralode.tdf","[CANBUILD]{sortbias=0;}");
        // A second keep that trains only scouts, and one that trains only archers.
        type("araeyrie","side=ARA;builder=1;workertime=30;");
        type("ararange","side=ARA;builder=1;workertime=30;");
        file("canbuild/araeyrie/arahawk.tdf","[CANBUILD]{sortbias=0;}");
        file("canbuild/ararange/arabow.tdf","[CANBUILD]{sortbias=0;}");
        type("zonking","side=ZON;builder=1;commander=1;workertime=30;"+mobile);
        file("canbuild/zonking/zonhand.tdf","[CANBUILD]{sortbias=0;}");
        type("zonhand","side=ZON;builder=1;workertime=30;"+mobile);
        type("zonlode","side=ZON;yardmap=SSSS;mogriumincome=10;mogriumstorage=1000;");
        type("zongob","side=ZON;"+mobile);
        type("zontroll","side=ZON;"+mobile);
        for (const char* made : {"zonlode","zongob","zontroll"})
            file(std::string("canbuild/zonhand/")+made+".tdf","[CANBUILD]{sortbias=0;}");
        vfs.setMapFiles(files);
        reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
        // Fighters: a primary weapon that does damage (the wave pool's test).
        for (const char* id : {"arasword","arabow","zongob","zontroll"}) {
            auto* t=const_cast<sim::UnitType*>(reg.find(id));
            t->weapon.damage=10;t->weapon.range=120;t->weapons={t->weapon};
        }
        for (const char* id : {"arakeep","araeyrie","ararange","aramason","araminer","aralode","arasword","arabow",
                               "zonhand","zonlode","zongob","zontroll"}) profile.weight[id]=10;
        // The scout and the archer as ai/default.txt has them: a limit of 4 each.
        profile.weight["arahawk"]=1;profile.limit["arahawk"]=4;profile.limit["arabow"]=4;
    }
};

// One factory per 60 income at Hard and Absurd: a working factory spends 30 to
// 120 mana a second, so the old target of one per 25 kept them broke. A store
// filling up says production is not spending the income: one more factory.
static void factoryTarget(const Roster& r) {
    auto buildsKeep=[&](ai::Difficulty difficulty,float income,int keeps,float mana=6000) {
        sim::World w;terrain(w);w.buildNavClasses(r.reg);
        w.player(0).storage=20000;w.player(0).mana=mana;w.player(0).income=income;
        for (int i=0;i<keeps;++i) busy(w,w.spawn(r("arakeep"),400+float(i%8)*200,400+float(i/8)*200,0,0),r("arasword"));
        w.spawn(r("aramason"),1100,1300,0,0);
        ai::Controller controller(0,r.reg,r.profile,1,difficulty);
        return orders(think(controller,w),net::Cmd::Build,"arakeep")==1;
    };
    for (auto level : {ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
        check(buildsKeep(level,300,5),"Hard/Absurd add a factory below one per 60 income");
        check(!buildsKeep(level,300,6),"Hard/Absurd stop at one factory per 60 income (and one)");
        check(!buildsKeep(level,300,12),"Hard/Absurd do not build a thirteenth factory on 300 income");
        check(!buildsKeep(level,300,8) && buildsKeep(level,300,8,15000),
              "with the store over 60% full Hard/Absurd build one more factory than they have");
    }
    // Normal follows the same rule, eight at most: at one per 40 its factories
    // kept the store empty and the over-commit throttle on as Hard's did.
    check(buildsKeep(ai::Difficulty::Normal,300,5),"Normal adds a factory below one per 60 income");
    check(!buildsKeep(ai::Difficulty::Normal,300,6),"Normal stops at one factory per 60 income (and one)");
    check(!buildsKeep(ai::Difficulty::Normal,300,6) && buildsKeep(ai::Difficulty::Normal,300,6,15000),
          "with the store over 60% full Normal builds one more factory than it has");
    check(buildsKeep(ai::Difficulty::Normal,600,7) && !buildsKeep(ai::Difficulty::Normal,600,8) &&
          !buildsKeep(ai::Difficulty::Normal,600,8,15000),"Normal stops at eight factories, a filling store or not");
    // The defensive levels keep one per 25 without a ceiling.
    check(buildsKeep(ai::Difficulty::Defensive,300,12) && !buildsKeep(ai::Difficulty::Defensive,300,13),
          "Defensive keeps one factory per 25 income");
}

// Broke, with work in hand, Hard and Absurd start nothing new -- but the one
// lodestone retail's AI builds when it runs short of mana, as the other levels do.
static void shortOfManaLodestone(const Roster& r) {
    auto lodes=[&](ai::Difficulty difficulty,float mana,bool underWay) {
        sim::World w;terrain(w);w.buildNavClasses(r.reg);
        w.setManaSpots({{1216,816},{1616,816}});
        w.player(0).storage=5000;w.player(0).income=100;
        shortOfMana(w,0,mana);
        w.spawn(r("arakeep"),800,800,0,0);
        const int site=w.spawn(r("arakeep"),800,1200,0,0);w.unit(site)->underConstruction=true;
        w.spawn(r("araminer"),1000,1000,0,0);
        w.spawn(r("araminer"),1000,1100,0,0);
        if (underWay) {
            const int other=w.spawn(r("araminer"),1600,1000,0,0);
            sim::Order o;o.buildType=r("aralode");o.buildX=sim::Fixed::fromInt(1616);o.buildZ=sim::Fixed::fromInt(816);
            w.unit(other)->orders.push_back(o);
        }
        ai::Controller controller(0,r.reg,r.profile,1,difficulty);
        return orders(think(controller,w),net::Cmd::Build,"aralode");
    };
    for (auto level : {ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
        check(lodes(level,10,false)==1,"Hard/Absurd, broke with work in hand, start one lodestone");
        check(lodes(level,10,true)==0,"... one at a time: none while a builder is on one");
    }
    check(lodes(ai::Difficulty::Normal,10,false)==1,"Normal's short-of-mana lodestone is unchanged");
}

// A full store: more income buys nothing. Hard and Absurd then build no
// lodestone; the builder is free for the next think.
static void fullStore(const Roster& r) {
    auto buildsLode=[&](ai::Difficulty difficulty,float mana) {
        sim::World w;terrain(w);w.buildNavClasses(r.reg);
        w.setManaSpots({{1216,816}});
        w.player(0).storage=10000;w.player(0).mana=mana;w.player(0).income=300;
        for (int i=0;i<6;++i) busy(w,w.spawn(r("arakeep"),400+float(i)*200,400,0,0),r("arasword"));
        w.spawn(r("araminer"),1000,1000,0,0);
        ai::Controller controller(0,r.reg,r.profile,1,difficulty);
        return orders(think(controller,w),net::Cmd::Build,"aralode")==1;
    };
    for (auto level : {ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
        check(buildsLode(level,5000),"Hard/Absurd keep adding lodestones while the store has room");
        check(!buildsLode(level,9500),"Hard/Absurd build no lodestone beside a full store");
    }
}

// Normal's lodestones grow to the income its next factory needs (one per 60):
// between its old income target (25 + 20 a factory) and that, a side wanted
// neither a lodestone nor a factory, and stood still.
static void normalIncomeTarget(const Roster& r) {
    auto buildsLode=[&](float income,int keeps) {
        sim::World w;terrain(w);w.buildNavClasses(r.reg);
        w.setManaSpots({{1216,816}});
        w.player(0).storage=10000;w.player(0).mana=4000;w.player(0).income=income;
        for (int i=0;i<keeps;++i) busy(w,w.spawn(r("arakeep"),400+float(i)*200,400,0,0),r("arasword"));
        w.spawn(r("araminer"),1000,1000,0,0);
        ai::Controller controller(0,r.reg,r.profile,1,ai::Difficulty::Normal);
        return orders(think(controller,w),net::Cmd::Build,"aralode")==1;
    };
    check(buildsLode(58,1) && buildsLode(100,2),"Normal adds lodestones below 60 income a factory");
    check(!buildsLode(130,2),"... and none past it while the store has room and no factory is short of mana");
}

// With a unit cap set, lodestones take a quarter of it at most, none is started
// in the last twentieth of it, and an order given but not yet a unit counts.
static void unitCap(const Roster& r) {
    // `lodes` lodestones standing, `army` swordsmen, then an idle miner beside a
    // free deposit and an idle keep.
    struct Result { int lodes,swords; };
    auto room=[&](ai::Difficulty difficulty,int cap,int lodes,int army,bool queued) {
        sim::World w;terrain(w);w.buildNavClasses(r.reg);
        std::vector<std::pair<float,float>> spots{{1216,1408}};
        for (int i=0;i<lodes;++i) spots.emplace_back(400+float(i%10)*128,3008+float(i/10)*128);
        w.setManaSpots(spots);
        w.setUnitCap(cap);
        w.player(0).storage=40000;w.player(0).mana=8000;w.player(0).income=300;
        w.spawn(r("araminer"),1000,1400,0,0);
        w.spawn(r("arakeep"),800,800,0,0);
        if (queued) {
            // A second keep whose queued swordsman has not begun: no unit yet.
            const int keep=w.spawn(r("arakeep"),1400,800,0,0);
            w.unit(keep)->buildQueue.push_back(r("arasword"));
        }
        for (int i=0;i<lodes;++i) w.spawn(r("aralode"),spots[size_t(i)+1].first,spots[size_t(i)+1].second,0,0);
        for (int i=0;i<army;++i) w.spawn(r("arasword"),2000+float(i%20)*40,2000+float(i/20)*40,0,0);
        ai::Controller controller(0,r.reg,r.profile,1,difficulty);
        const auto cs=think(controller,w);
        return Result{orders(cs,net::Cmd::Build,"aralode"),orders(cs,net::Cmd::Train,"arasword")};
    };
    for (auto level : {ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
        check(room(level,40,9,5,false).lodes==1,"below a quarter of the cap a lodestone is still built");
        check(room(level,40,10,5,false).lodes==0,"lodestones stop at a quarter of the unit cap");
        check(room(level,0,10,5,false).lodes==1,"with no unit cap the lodestones have no share to keep to");
        // 38 of 40: two slots left. The miner comes first in unit order.
        const auto last=room(level,40,2,34,false);
        check(last.lodes==0 && last.swords==1,"the last slots under the cap go to fighters, not lodestones");
        check(room(level,40,2,33,false).lodes==1,"before the last twentieth of the cap a lodestone is still built");
    }
    // 39 of 40, and one more swordsman queued: the cap is spoken for.
    for (auto level : {ai::Difficulty::Normal,ai::Difficulty::Hard,ai::Difficulty::Absurd,ai::Difficulty::Defensive}) {
        const auto full=room(level,40,0,36,true);
        check(full.swords==0 && full.lodes==0,"an order not yet begun counts against the unit cap");
        check(room(level,40,0,35,true).swords==1,"... and one slot below it a fighter is still trained");
    }
}

// Hard and Absurd reach the whole map for deposits, but not one beside armed
// enemies they know of, nor inside the base of an enemy still there, nor where
// they just lost a site, or its builder beside it or on its way there.
static void safeDeposits(const Roster& r) {
    // The miner at (2000,1000): deposit A 200 px east of it, B 600 px west.
    const float ax=2208,az=1008,bx=1408,bz=1008;
    auto world=[&](sim::World& w,int& miner,float income=300) {
        terrain(w);w.buildNavClasses(r.reg);
        w.setManaSpots({{ax,az},{bx,bz}});
        w.player(0).storage=10000;w.player(0).mana=3000;w.player(0).income=income;
        w.spawn(r("arakeep"),1800,1800,0,0);
        miner=w.spawn(r("araminer"),2000,1000,0,0);
    };
    for (auto level : {ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
        {
            sim::World w;int miner=0;world(w,miner);sight(w);
            ai::Controller controller(0,r.reg,r.profile,1,level);
            auto order=build(think(controller,w,40),"aralode");
            check(order && order->x==ax,"a builder takes the nearest free deposit");
        }
        {
            // An enemy swordsman 150 px from A, in the miner's sight.
            sim::World w;int miner=0;world(w,miner);
            w.spawn(r("arasword"),ax+150,az,0,1);w.setTeam(1,1);sight(w);
            ai::Controller controller(0,r.reg,r.profile,1,level);
            auto order=build(think(controller,w,40),"aralode");
            check(order && order->x==bx,"a deposit beside an armed enemy in sight is passed over");
        }
        {
            // An enemy that may still be at its start, 600 px beyond A.
            sim::World w;int miner=0;world(w,miner);sight(w);
            ai::Controller controller(0,r.reg,r.profile,1,level,{{ax+600,az}});
            auto order=build(think(controller,w,40),"aralode");
            check(order && order->x==bx,"a deposit inside an enemy's base is passed over");
        }
        {
            // A site of ours on A is destroyed before it is finished.
            sim::World w;int miner=0;world(w,miner);
            busy(w,miner,r("aralode"));
            const int site=w.spawn(r("aralode"),ax,az,0,0);w.unit(site)->underConstruction=true;
            sight(w);
            ai::Controller controller(0,r.reg,r.profile,1,level);
            think(controller,w,40);
            auto& dead=*w.unit(site);dead.hp={};dead.deadFor=1;w.blockFoot(*dead.type,ax,az,false);
            w.unit(miner)->orders.clear();
            auto order=build(think(controller,w,80),"aralode");
            check(order && order->x==bx,"a deposit whose site was destroyed unfinished is left alone for a while");
            w.unit(miner)->orders.clear();
            order=build(think(controller,w,80+6*60*30),"aralode");
            check(order && order->x==ax,"... and tried again five minutes later");
        }
        {
            // The builder walking to A is killed on the way.
            sim::World w;int miner=0;world(w,miner);
            const int second=w.spawn(r("araminer"),2000,1100,0,0);
            busy(w,second,r("arasword"));
            sight(w);
            ai::Controller controller(0,r.reg,r.profile,1,level);
            auto cs=think(controller,w,40);
            auto order=build(cs,"aralode");
            const bool sent=order && order->unitId==miner && order->x==ax;
            for (const auto& c:cs) sim::applyCommand(w,r.reg,c);
            think(controller,w,60);   // the order is in the world: the trip is noted
            auto& dead=*w.unit(miner);dead.hp={};dead.deadFor=1;
            w.unit(second)->orders.clear();
            order=build(think(controller,w,80),"aralode");
            check(sent && order && order->unitId==second && order->x==bx,
                  "a deposit whose builder was killed on the way is left alone for a while");
        }
        // A killed builder makes its deposit unsafe only beside it (within 400 px)
        // or on its way there: it had set out (64 px nearer) and was within 1,000
        // px. The deposit is `trip` px west of the miners, the other one 1,600 px
        // east; the first miner is killed `shortOf` px from the deposit.
        struct Killed { float trip,shortOf;bool unsafe;const char* label; };
        for (const auto& k : {Killed{1200,1200,false,"a builder killed at home leaves the far deposit it was sent to free"},
                              Killed{1200,100,true,"a deposit whose builder was killed beside it is left alone for a while"},
                              Killed{1200,600,true,"... and one whose builder was killed on its way, 600 px short of it"},
                              Killed{1200,1100,false,"a builder killed more than 1,000 px short of its deposit leaves it free"},
                              Killed{600,600,false,"a builder killed where it stood when it was sent leaves the deposit free"}}) {
            const float westX=2000-k.trip,eastX=3600;
            sim::World w;int miner=0;world(w,miner);
            w.setManaSpots({{westX,az},{eastX,az}});
            const int second=w.spawn(r("araminer"),2000,1100,0,0);
            busy(w,second,r("arasword"));
            sight(w);
            ai::Controller controller(0,r.reg,r.profile,1,level);
            auto cs=think(controller,w,40);
            auto order=build(cs,"aralode");
            const bool sent=order && order->unitId==miner && order->x==westX;
            for (const auto& c:cs) sim::applyCommand(w,r.reg,c);
            think(controller,w,60);   // the order is in the world: the trip is noted, from here
            w.unit(miner)->x=sim::Fixed::fromInt(int(westX+k.shortOf));
            think(controller,w,80);   // ... and where the builder has come to
            auto& dead=*w.unit(miner);dead.hp={};dead.deadFor=1;
            w.unit(second)->orders.clear();
            order=build(think(controller,w,100),"aralode");
            check(sent && order && order->unitId==second && order->x==(k.unsafe ? eastX : westX),k.label);
        }
    }
    {
        // Normal's reach and rules are its own: unchanged (income 30: it wants a lodestone).
        sim::World w;int miner=0;world(w,miner,30);
        w.spawn(r("arasword"),ax+150,az,0,1);w.setTeam(1,1);sight(w);
        ai::Controller controller(0,r.reg,r.profile,1,ai::Difficulty::Normal);
        auto order=build(think(controller,w,30),"aralode");
        check(order && order->x==ax,"Normal's deposit choice is unchanged");
    }
}

// Zhon's handlers are its factories, and conjure all the time they are not away
// at a lodestone: Normal, Hard and Absurd keep as many as their income feeds (one
// per 60) and a quarter more for the ones away, not another side's whole builder
// allowance on top.
static void zhonAllowance(const Roster& r) {
    auto conjuresHandler=[&](ai::Difficulty difficulty,float income,int handlers,float mana=6000) {
        sim::World w;terrain(w);w.buildNavClasses(r.reg);
        w.player(0).storage=20000;w.player(0).mana=mana;w.player(0).income=income;
        w.spawn(r("zonking"),800,800,0,0);
        for (int i=0;i<handlers;++i) busy(w,w.spawn(r("zonhand"),1400+float(i%6)*60,1400+float(i/6)*60,0,0),r("zongob"));
        for (int i=0;i<handlers*4;++i) w.spawn(r("zongob"),2000+float(i%16)*40,400+float(i/16)*40,0,0);
        ai::Controller controller(0,r.reg,r.profile,1,difficulty);
        return orders(think(controller,w),net::Cmd::Build,"zonhand")==1;
    };
    for (auto level : {ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
        // Income 300: six factories' worth, and two away at lodestones.
        check(conjuresHandler(level,300,7),"Hard/Absurd Zhon adds producers up to one per 60 income and a quarter more");
        check(!conjuresHandler(level,300,8),"Hard/Absurd Zhon keeps no more producers than its income feeds");
        // Twelve producers: ten that its income is taken to feed and two away.
        // One more and its share away make thirteen (a quarter of all twelve
        // counted as away would make the twelve it has, and add none).
        check(!conjuresHandler(level,300,12) && conjuresHandler(level,300,12,15000),
              "with the store over 60% full a Hard/Absurd Zhon conjures one more producer than it has");
    }
    // Normal: the same six and two away on 300 income (its builder allowance of
    // four on top made twelve), and with a filling store one more than the six
    // its eight are taken to feed; ten at most (eight factories and two away).
    check(conjuresHandler(ai::Difficulty::Normal,300,7) && !conjuresHandler(ai::Difficulty::Normal,300,8),
          "Normal Zhon keeps no more producers than its income feeds and a quarter more");
    check(conjuresHandler(ai::Difficulty::Normal,300,8,15000),
          "with the store over 60% full a Normal Zhon conjures one more producer than it has");
    check(!conjuresHandler(ai::Difficulty::Normal,300,10,15000),"a Normal Zhon keeps ten producers at most");
}

// Zhon's handlers are its factories: few leave for a lodestone at a time, and
// none crosses to the enemy's side of the map for one.
static void zhonProducers(const Roster& r) {
    // `handlers` idle handlers at home with four goblins each already standing
    // (past the opening army); deposits near home, or on the far side of the
    // map, where the enemy starts. `builder` swaps them for another side's miners.
    auto lodes=[&](ai::Difficulty difficulty,int handlers,bool nearHome,const char* builder="zonhand") {
        sim::World w;terrain(w);w.buildNavClasses(r.reg);
        std::vector<std::pair<float,float>> spots;
        for (int i=0;i<20;++i) spots.emplace_back((nearHome ? 416.f : 2816.f)+float(i%5)*160,(nearHome ? 1616.f : 2416.f)+float(i/5)*160);
        w.setManaSpots(spots);
        w.player(0).storage=40000;w.player(0).mana=8000;w.player(0).income=2000;
        for (int i=0;i<handlers;++i) w.spawn(r(builder),600+float(i%8)*60,600+float(i/8)*60,0,0);
        if (std::string(builder)!="zonhand") busy(w,w.spawn(r("arakeep"),400,400,0,0),r("arasword"));
        for (int i=0;i<handlers*4;++i) w.spawn(r("zongob"),1600+float(i%16)*40,400+float(i/16)*40,0,0);
        ai::Controller controller(0,r.reg,r.profile,1,difficulty,{{3600,3600}});
        return orders(think(controller,w),net::Cmd::Build,std::string(builder)=="zonhand" ? "zonlode" : "aralode");
    };
    for (auto level : {ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
        check(lodes(level,16,true)==4,"a quarter of Zhon's producers (two at least) are out on lodestones at a time");
        check(lodes(level,6,true)==2,"... two of six");
        check(lodes(level,16,false)==0,"Zhon's producers take no deposit on the enemy's side of the map");
        check(lodes(level,1,false,"araminer")==1,"a constructor of a side with factory buildings still crosses the map");
    }
}

// Hard and Absurd lift the profile limit of the troops their armies use; a
// type no army routine commands (an unarmed scout) keeps its limit.
static void unarmedLimits(const Roster& r) {
    auto trains=[&](ai::Difficulty difficulty,const char* factory,const char* unit,int standing) {
        sim::World w;terrain(w);w.buildNavClasses(r.reg);
        w.player(0).storage=20000;w.player(0).mana=6000;w.player(0).income=300;
        w.spawn(r(factory),800,800,0,0);
        for (int i=0;i<standing;++i) w.spawn(r(unit),1600+float(i%10)*40,1600+float(i/10)*40,0,0);
        ai::Controller controller(0,r.reg,r.profile,1,difficulty);
        return orders(think(controller,w),net::Cmd::Train,unit)==1;
    };
    for (auto level : {ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
        // Their limits are the profile's times 150%: six of a limit of four.
        check(trains(level,"araeyrie","arahawk",5),"an unarmed scout is trained up to its profile limit");
        check(!trains(level,"araeyrie","arahawk",6),"an unarmed scout keeps its profile limit at Hard/Absurd");
        check(trains(level,"ararange","arabow",40),"a fighter's profile limit is still lifted at Hard/Absurd");
    }
    check(!trains(ai::Difficulty::Normal,"araeyrie","arahawk",4),"Normal keeps the scout's limit, as before");
    check(trains(ai::Difficulty::Defensive,"araeyrie","arahawk",40),"the defensive levels still take no profile limit");
}

int main() {
    const Roster roster;
    factoryTarget(roster);
    shortOfManaLodestone(roster);
    fullStore(roster);
    normalIncomeTarget(roster);
    unitCap(roster);
    safeDeposits(roster);
    zhonAllowance(roster);
    zhonProducers(roster);
    unarmedLimits(roster);
    std::printf("%s (%d failed)\n",fails ? "FAILED" : "OK",fails);
    return fails ? 1 : 0;
}
