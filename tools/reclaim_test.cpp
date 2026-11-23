// Retail RECLAIM (406250) reach and timing, as emulated (see docs/retail-engine.md
// "Reclaim"): work starts only when the builder's footprint touches the
// target's, takes trunc(energy + 15) ticks counted down two at a time whatever
// the builder, pays no mana, and the feature leaves the plane 34 ticks after
// the order ends. tools/re/check_reclaim_reach.py checks the pure rules
// against the emulated routines; this drives the World.
#include "sim/sim.h"
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
namespace tak::sim {
struct RetailReplayProbe {
    static bool corpse(World& w,Unit& u,int type) {return w.placeCorpse(u,type);}
    static Feature& feature(World& w,int id) {return w.features_[w.featureIdx_.at(id)];}
    // Through igniteFeature, so tickBurning's marks see it (a bare burn=1 is
    // never visited).
    static Feature& ignite(World& w,int id) {w.igniteFeature(feature(w,id));return feature(w,id);}
    static bool hasCorpseFootprint(const World& w,int id) {return w.corpseFootprints_.contains(id);}
};
}
using namespace tak;
using Probe=sim::RetailReplayProbe;
static int failures=0;
static void check(bool ok,const std::string& what) {
    std::printf("%s %s\n",ok ? "PASS" : "FAIL",what.c_str());
    failures+=!ok;
}
static constexpr int kW=64;
static void flat(sim::World& w) {
    w.setTerrain(std::vector<uint8_t>(kW*kW,100),kW,kW,20);
}
static sim::UnitType builderType(int foot=1,float workerTime=10,int buildDist=0) {
    sim::UnitType t;t.id="reclaimer";t.maxHp=100;
    t.isBuilder=t.canMove=t.canReclaim=true;t.upright=true;
    t.maxVel=sim::Fixed::fromInt(2);t.footX=t.footZ=foot;
    t.turnRate=10000;t.turnInPlaceRate=0;   // no FacingNoFormation child (as emulated)
    t.workerTime=workerTime;t.buildDist=float(buildDist);
    return t;
}
// Cell centre of a footprint origin.
static float at(int origin,int foot) {return float(origin*16+foot*8);}
static const sim::Order* reclaimLeg(const sim::World& w,int id) {
    const auto* u=w.unit(id);
    if (!u || u->orders.empty()) return nullptr;
    const auto& leg=u->orders[sim::World::currentLeg(u->orders)];
    return leg.reclaimFeat ? &leg : nullptr;
}
static int origin(sim::Fixed v,int foot) {return sim::footprintOrigin(v,foot);}

struct Timing { int start=-1,end=-1,removed=-1; double mana=0; };
// One 1x1 feature at cell (20,20); builders start on its edge cells, so the
// arrival is immediate. Ticks are counted from the arrival (stage 1 handled).
static Timing timeReclaim(float energy,int builders,float workerTime,int stagger=0) {
    sim::World w;flat(w);
    w.addFeature(1,at(20,1),at(20,1),energy,1,1,false);
    auto type=builderType(1,workerTime);
    const int cells[4][2]={{19,20},{21,20},{20,19},{20,21}};
    std::vector<int> ids;
    for (int i=0;i<builders;++i) ids.push_back(w.spawn(&type,at(cells[i][0],1),at(cells[i][1],1),0.0f,0));
    w.player(0).mana=0;w.player(0).income=0;
    Timing t;
    for (int i=0;i<builders;++i) if (!stagger || i==0) w.reclaim(ids[size_t(i)],1,false);
    for (int tick=0;tick<3000;++tick) {
        if (stagger && tick==stagger) for (int i=1;i<builders;++i) w.reclaim(ids[size_t(i)],1,false);
        w.tick(1.f/30);
        const auto* leg=reclaimLeg(w,ids[0]);
        if (t.start<0 && leg && leg->mission.stage>=2) t.start=tick;
        if (t.start>=0 && t.end<0 && !leg) t.end=tick;
        if (t.removed<0 && !w.feature(1)->alive) { t.removed=tick;break; }
    }
    t.mana=w.player(0).mana;
    return t;
}

int main(int argc,char**) {
    (void)argc;
    // ---- countdown and wake schedule (406250 stage 1, 406529) -------------
    check(sim::retailReclaimCountdown(0)==15 && sim::retailReclaimCountdown(75)==90 &&
          sim::retailReclaimCountdown(250)==265 && sim::retailReclaimCountdown(525)==540 &&
          sim::retailReclaimCountdown(37.5f)==52,"countdown is trunc(energy + 15)");
    check(sim::retailReclaimCountdown(3e9f)==INT32_MIN && sim::retailReclaimCountdown(-3e9f)==INT32_MIN,
          "an out-of-range countdown is ftol's integer indefinite on every CPU");
    for (const auto& [energy,end]:std::vector<std::pair<float,int>>{{0,18},{75,92},{250,268},{525,542}}) {
        const auto t=timeReclaim(energy,1,10);
        check(t.start>=0 && t.end-t.start==end && t.removed-t.end==sim::kReclaimFadeTicks,
              "energy "+std::to_string(int(energy))+": order ends "+std::to_string(end)+
              " ticks after arrival, removed 34 later (got "+std::to_string(t.end-t.start)+
              ", "+std::to_string(t.removed-t.end)+")");
        check(t.mana==0,"energy "+std::to_string(int(energy))+": reclaim pays no mana");
    }
    {
        const auto slow=timeReclaim(250,1,1),fast=timeReclaim(250,1,5000);
        check(slow.end-slow.start==268 && fast.end-fast.start==268,"workertime does not change the time");
        const auto four=timeReclaim(250,4,10);
        check(four.end-four.start==268 && four.removed-four.end==34,
              "four builders on one feature take as long as one (no shared work)");
        const auto late=timeReclaim(250,3,10,40);
        check(late.end-late.start==268,"a builder joining late does not speed the first one up");
    }
    {
        // Every other builder quits on its next wake once the first removes it.
        sim::World w;flat(w);
        w.addFeature(1,at(20,1),at(20,1),75,1,1,false);
        auto type=builderType();
        const int a=w.spawn(&type,at(19,1),at(20,1),0.0f,0),b=w.spawn(&type,at(21,1),at(20,1),0.0f,0);
        w.reclaim(a,1,false);
        for (int i=0;i<10;++i) w.tick(1.f/30);
        w.reclaim(b,1,false);
        int aEnd=-1,bEnd=-1;
        for (int tick=0;tick<400 && (aEnd<0 || bEnd<0);++tick) {
            w.tick(1.f/30);
            if (aEnd<0 && !reclaimLeg(w,a)) aEnd=tick;
            if (bEnd<0 && !reclaimLeg(w,b)) bEnd=tick;
        }
        check(aEnd>=0 && bEnd>=aEnd && bEnd-aEnd<=2 && w.feature(1)->fadeLeft>0,
              "the second builder quits within one wake of the first one's removal");
    }

    // ---- reach: the rectangle controller (4d4e80 -> 4e3590) ----------------
    {
        bool bounds=true;
        for (int bw=1;bw<=4;++bw) for (int fw=1;fw<=3;++fw) for (int fh=1;fh<=3;++fh) {
            sim::World w;flat(w);
            const int ax=30,az=28;
            w.addFeature(1,at(ax,fw),at(az,fh),0,fw,fh,false);
            auto type=builderType(bw,10,500);
            const int id=w.spawn(&type,at(4,bw),at(4,bw),0.0f,0);
            w.reclaim(id,1,false);w.tick(1.f/30);
            const auto* leg=reclaimLeg(w,id);
            bounds&=leg && leg->buildRectangle &&
                *leg->buildRectangle==sim::RetailRectGoal{ax-bw,ax+fw,az-bw,az+fh};
        }
        check(bounds,"rectangle: footprint-origin cells touching the target, 1..4 builders x 1..3 features");
    }
    // Where work starts: the builder's origin cell on the ring, never at
    // builddistance, for an approach from each side.
    for (const auto& [sx,sz,label]:std::vector<std::tuple<int,int,const char*>>{
             {10,20,"from the west"},{30,20,"from the east"},{20,9,"from the north"},{28,29,"from the south-east"}}) {
        sim::World w;flat(w);w.setPathService(true);
        w.addFeature(1,at(20,1),at(20,1),0,1,1,false);
        auto type=builderType(2,10,500);   // builddistance 500: retail never reads it
        const int id=w.spawn(&type,at(sx,2),at(sz,2),0.0f,0);
        w.reclaim(id,1,false);
        bool started=false,adjacent=false;int moved=0;
        const sim::RetailRectGoal ring{18,21,18,21};
        for (int tick=0;tick<1200 && !started;++tick) {
            w.tick(1.f/30);
            const auto* leg=reclaimLeg(w,id);
            if (!leg) break;
            if (leg->mission.stage>=2) {
                started=true;
                const auto* u=w.unit(id);
                adjacent=ring.accepts(origin(u->x,2),origin(u->z,2));
                moved=std::abs(origin(u->x,2)-sx)+std::abs(origin(u->z,2)-sz);
            }
        }
        check(started && adjacent && moved>0,std::string("work starts only touching the feature, ")+label);
    }
    {
        // A corner touch is enough, and needs no step; one cell of gap is not.
        sim::World w;flat(w);
        w.addFeature(1,at(20,1),at(20,1),0,1,1,false);
        w.addFeature(2,at(40,1),at(40,1),0,1,1,false);
        auto type=builderType(1,10,500);
        const int corner=w.spawn(&type,at(19,1),at(19,1),0.0f,0);
        const int gap=w.spawn(&type,at(38,1),at(40,1),0.0f,0);
        w.reclaim(corner,1,false);w.reclaim(gap,2,false);
        w.tick(1.f/30);w.tick(1.f/30);
        const auto* c=reclaimLeg(w,corner);const auto* g=reclaimLeg(w,gap);
        check(c && c->mission.stage>=2 && origin(w.unit(corner)->x,1)==19 && origin(w.unit(corner)->z,1)==19,
              "a builder touching the feature's corner starts where it stands");
        check(g && g->mission.stage==1,"one cell of gap is not reach, whatever the builddistance");
    }
    {
        // Overlapping a non-blocking feature is refused: the builder steers out
        // to the perimeter first (4e39e0 accepts only the ring).
        sim::World w;flat(w);
        w.addFeature(1,at(20,2),at(20,2),0,2,2,false);
        auto type=builderType(1);
        const int id=w.spawn(&type,at(20,1),at(20,1),0.0f,0);
        w.reclaim(id,1,false);
        bool started=false,outside=false;
        for (int tick=0;tick<300 && !started;++tick) {
            w.tick(1.f/30);
            const auto* leg=reclaimLeg(w,id);
            if (leg && leg->mission.stage>=2) {
                started=true;
                const int ox=origin(w.unit(id)->x,1),oz=origin(w.unit(id)->z,1);
                outside=!(ox>=20 && ox<22 && oz>=20 && oz<22) && sim::RetailRectGoal{19,22,19,22}.accepts(ox,oz);
            }
        }
        check(started && outside,"a builder standing on the feature steers out to its edge before working");
    }
    {
        // Once work has started no distance is tested: pushed 40 cells away
        // the builder still finishes on time (406529 has no reach test).
        sim::World w;flat(w);
        w.addFeature(1,at(20,1),at(20,1),250,1,1,false);
        auto type=builderType();
        const int id=w.spawn(&type,at(19,1),at(20,1),0.0f,0);
        w.reclaim(id,1,false);
        int start=-1,end=-1;
        for (int tick=0;tick<600 && end<0;++tick) {
            w.tick(1.f/30);
            const auto* leg=reclaimLeg(w,id);
            if (start<0 && leg && leg->mission.stage>=3) {
                start=tick;
                auto* u=w.unit(id);u->x=sim::Fixed::fromFloat(at(59,1));
            }
            if (start>=0 && !leg) end=tick;
        }
        check(end-start==268 && w.feature(1)->fadeLeft>0,"a builder pushed far away mid-work keeps working");
    }
    {
        // Stop mid-work: nothing removed, nothing paid (event 0x2 is a cancel).
        sim::World w;flat(w);
        w.addFeature(1,at(20,1),at(20,1),250,1,1,false);
        auto type=builderType();
        const int id=w.spawn(&type,at(19,1),at(20,1),0.0f,0);
        w.player(0).mana=0;
        w.reclaim(id,1,false);
        for (int i=0;i<60;++i) w.tick(1.f/30);
        w.stop(id);
        for (int i=0;i<400;++i) w.tick(1.f/30);
        check(w.feature(1)->alive && !w.feature(1)->fadeLeft && w.player(0).mana==0 && !w.unit(id)->workScriptWorking,
              "stop cancels the reclaim with nothing removed or paid");
    }
    {
        // A burning feature fails the per-wake lookup (4949c0 clears its bit).
        sim::World w;flat(w);
        w.addFeature(1,at(20,1),at(20,1),250,1,1,false);
        auto type=builderType();
        const int id=w.spawn(&type,at(19,1),at(20,1),0.0f,0);
        w.reclaim(id,1,false);
        for (int i=0;i<20;++i) w.tick(1.f/30);
        Probe::feature(w,1).burn=1;
        w.tick(1.f/30);w.tick(1.f/30);w.tick(1.f/30);
        check(!reclaimLeg(w,id) && w.feature(1)->alive && !w.feature(1)->fadeLeft,
              "a feature that catches fire ends the reclaim, unremoved");
    }

    {
        // A fire spreading onto a tree that is already fading must not bring
        // it back as a burnt stage that outlives the fade.
        sim::World w;flat(w);
        sim::FeatType tree;tree.name="tree";tree.reclaimable=tree.flamable=tree.hasBurnAnim=true;
        tree.spreadChance=100;tree.burntType=1;
        sim::FeatType burnt;burnt.name="burnt";burnt.reclaimable=true;
        w.setFeatureTypes({tree,burnt});
        // Ids are the anchor cells: the spread looks its neighbours up by cell.
        const int tree1=20*kW+20,tree2=20*kW+22;
        w.addFeature(tree1,at(20,1),at(20,1),0,1,1,true,0);
        w.addFeature(tree2,at(22,1),at(20,1),0,1,1,true,0);
        auto type=builderType();
        const int id=w.spawn(&type,at(19,1),at(20,1),0.0f,0);
        w.reclaim(id,tree1,false);
        for (int i=0;i<100 && !w.feature(tree1)->fadeLeft;++i) w.tick(1.f/30);
        auto& fire=Probe::ignite(w,tree2);fire.burnLeft=600;fire.spreadIn=1;
        for (int i=0;i<60;++i) w.tick(1.f/30);
        check(!w.feature(tree1)->alive && w.feature(tree1)->type==0 && w.feature(tree2)->burn,
              "a fading feature does not catch fire");
    }

    // ---- corpses: the installed corpse feature, no rubble ------------------
    {
        sim::World w;flat(w);
        sim::FeatType wreck;wreck.name="wreck";wreck.reclaimable=true;wreck.fx=wreck.fz=2;
        wreck.decomposeTicks=600;wreck.energy=0;wreck.deadType=1;
        sim::FeatType rubble;rubble.name="rubble";rubble.reclaimable=true;
        w.setFeatureTypes({wreck,rubble});
        sim::UnitType soldier;soldier.id="soldier";soldier.maxHp=100;soldier.canMove=true;
        soldier.maxVel=sim::Fixed::fromInt(1);soldier.corpseAdjX=1;
        w.mapCorpse(&soldier,0);
        const int dead=w.spawn(&soldier,at(30,1),at(30,1),0.0f,1);
        auto* body=w.unit(dead);body->hp={};body->deadFor=120;body->corpseUntil=2000;
        check(Probe::corpse(w,*body,0),"corpse feature installs");
        auto type=builderType(2);
        const int id=w.spawn(&type,at(20,2),at(30,2),0.0f,0);
        w.reclaim(id,-dead,false);
        w.tick(1.f/30);
        const auto* leg=reclaimLeg(w,id);
        // Origin (31,30): the unit's own cell moved by corpseadjust, 2x2 wreck.
        check(leg && leg->buildRectangle && *leg->buildRectangle==sim::RetailRectGoal{29,33,28,32},
              "corpse reach is the corpse feature's footprint, not the dead unit's");
        int end=-1,retired=-1;
        bool decayHeld=true,shortened=false;
        for (int tick=0;tick<900 && retired<0;++tick) {
            w.tick(1.f/30);
            auto* u=w.unit(dead);
            const auto* work=reclaimLeg(w,id);
            if (end<0 && work && work->mission.stage>=3 && !shortened) {
                // Three updates before it starts sinking into the ground (its
                // decay +0x3c at 3, retailCorpseDecayLife): each work wake
                // reloads the decay instead.
                u->corpseUntil=u->deadFor+58+3;shortened=true;
            }
            if (end<0 && work) decayHeld&=u->deadFor<u->corpseUntil;
            if (end<0 && !work) end=tick;
            if (end>=0 && !Probe::hasCorpseFootprint(w,dead)) retired=tick;
        }
        const auto* u=w.unit(dead);
        bool rubbleLeft=false;
        for (const auto& f:w.features()) rubbleLeft|=f.alive && f.type==1;
        check(end>=0 && shortened && retired-end==sim::kReclaimFadeTicks && decayHeld && !rubbleLeft &&
              (!u || u->corpseUntil==0),
              "a reclaimed corpse fades out with no rubble and does not decay meanwhile");
    }
    // ---- a sinking corpse: the fade's new record stops it ------------------
    // 494ce0 mode 1 replaces the body's record (494e08..494ed8): it stays at
    // the height it had sunk to, its own fade ends, and it goes when the
    // reclaim's fade does (check_corpse_sink.py, reclaim cases).
    {
        sim::World w;
        w.setTerrain(std::vector<uint8_t>(kW*kW,0),kW,kW,120);   // a sea bed the builder walks
        sim::FeatType wreck;wreck.name="wreck";wreck.reclaimable=true;wreck.fx=wreck.fz=2;wreck.energy=0;
        w.setFeatureTypes({wreck});
        sim::UnitType soldier;soldier.id="soldier";soldier.maxHp=100;soldier.canMove=true;
        soldier.maxVel=sim::Fixed::fromInt(1);
        w.mapCorpse(&soldier,0);
        const int dead=w.spawn(&soldier,at(32,1),at(30,1),0.0f,1);
        auto* body=w.unit(dead);body->hp={};body->deadFor=120;body->corpseUntil=INT32_MAX;
        body->groundY=sim::Fixed::fromInt(120);
        check(Probe::corpse(w,*body,0) && w.corpseSink(dead),"the wreck in water sinks");
        auto type=builderType(2);
        const int id=w.spawn(&type,at(30,2),at(30,2),0.0f,0);
        w.reclaim(id,-dead,false);
        int end=-1,retired=-1;int32_t at_end=0;bool still=true,fadeOff=true;
        for (int tick=0;tick<900 && retired<0;++tick) {
            w.tick(1.f/30);
            const auto* sink=w.corpseSink(dead);
            if (!w.unit(dead)) break;
            if (end<0 && w.unit(dead)->corpseFading) { end=tick;at_end=sink ? sink->y : -1; }
            else if (end>=0 && sink) { still&=sink->y==at_end && sink->vy==0;fadeOff&=!sink->fading; }
            if (end>=0 && !Probe::hasCorpseFootprint(w,dead)) retired=tick;
        }
        check(end>=0 && at_end>0 && at_end<(120<<16),"the wreck was reclaimed while it sank");
        check(still && fadeOff,"a reclaimed wreck stops sinking where it is, its own fade over");
        check(retired-end==sim::kReclaimFadeTicks,"a reclaimed wreck goes with the reclaim's fade");
    }
    // ---- a sinking corpse a reclaim leaves: held as a land corpse is ------
    // A water grave decomposes as a land corpse does here (not retail, whose
    // grave decays only once a wake reloads a record still sinking), so a
    // reclaim wake holds it the same way (406615, retailCorpseDecayHold): it
    // goes the decomposition plus 58 updates after the last wake, sinking on
    // meanwhile.
    {
        sim::World w;
        w.setTerrain(std::vector<uint8_t>(kW*kW,0),kW,kW,120);
        sim::FeatType wreck;wreck.name="wreck";wreck.reclaimable=true;wreck.fx=wreck.fz=2;wreck.energy=0;
        wreck.decomposeTicks=90;
        w.setFeatureTypes({wreck});
        sim::UnitType soldier;soldier.id="soldier";soldier.maxHp=100;soldier.canMove=true;
        soldier.maxVel=sim::Fixed::fromInt(1);
        w.mapCorpse(&soldier,0);
        const int dead=w.spawn(&soldier,at(32,1),at(30,1),0.0f,1);
        auto* body=w.unit(dead);body->hp={};body->deadFor=120;body->corpseUntil=120+90+58;
        body->groundY=sim::Fixed::fromInt(120);
        check(Probe::corpse(w,*body,0) && w.corpseSink(dead),"the wreck in water sinks");
        auto type=builderType(2);
        const int id=w.spawn(&type,at(30,2),at(30,2),0.0f,0);
        w.reclaim(id,-dead,false);
        int woke=-1,retired=-1,left=INT32_MAX,heldFor=0;bool sank=true,reclaimed=false;int32_t lastY=INT32_MAX;
        for (int tick=0;tick<900 && retired<0;++tick) {
            w.tick(1.f/30);
            const auto* sink=w.corpseSink(dead);
            if (!sink || !w.unit(dead) || w.unit(dead)->corpseUntil==0) { retired=tick;break; }
            reclaimed|=w.unit(dead)->corpseFading;
            // The wake's tick: the time left goes back up.
            const int now=w.unit(dead)->corpseUntil-w.unit(dead)->deadFor;
            if (woke<0 && now>left) { woke=tick;heldFor=now;w.stop(id); }
            left=now;
            if (woke>=0) { sank&=sink->y<lastY;lastY=sink->y; }
        }
        check(woke>=0 && !reclaimed,"a reclaim wake held the sinking wreck's decay, and the reclaim stopped");
        // Seen after the wake's tick, which counts one of them.
        check(heldFor==90+58-1 && retired-woke==heldFor && sank,
              "the wreck decays as a land corpse would, sinking on meanwhile");
    }
    std::printf(failures ? "\nFAILED (%d)\n" : "\nall passed\n",failures);
    return failures ? 1 : 0;
}
