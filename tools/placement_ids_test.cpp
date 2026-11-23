// Ground placement with unit ids past 65,535. World::mobilePlacement (507d10's
// footprint loop) used to carry the occupant's id and the asker's own id in 16
// bits, as retail's slot index is. World's ids are never reused, so a long game
// passes 65,535 (the audited 8-AI game reached 40,834 in 3.6 hours), and then:
//  - a unit whose id is 65,536 or more stopped blocking: its id read as a low id
//    that no longer (or never) held a unit, so its cells looked empty;
//  - a unit with id 65,536+k took the cells of unit k for its own.
// Both let ground bodies step into each other, leave factories onto them, and
// pass Legion's placement checks. The ids are carried whole now.
#include "sim/footprint.h"
#include "sim/sim.h"
#include <cstdio>
#include <vector>
using namespace ta;

namespace {
int failures=0;
void check(bool ok,const char* label) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",label);failures+=!ok;
}
sim::UnitType walker(const char* id) {
    sim::UnitType t;t.id=id;t.maxHp=500;t.footX=t.footZ=2;t.maxVel=sim::Fixed::fromInt(1);
    t.canMove=true;t.upright=true;t.sight=40;t.sightHeight=24;return t;
}
void setup(sim::World& w) {
    w.setSerialThreads(true);
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);w.setPlayerCount(2);
    w.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
}
// May `asker` stand where `holder` stands? Never, for two ground bodies.
bool mayStandOn(const sim::World& w,int asker,int holder) {
    const sim::Unit* a=w.unit(asker);const sim::Unit* h=w.unit(holder);
    return w.mobilePlacement(*a,sim::footprintOrigin(h->x,a->type->footX),
                             sim::footprintOrigin(h->z,a->type->footZ),false);
}
}

int main() {
    const auto type=walker("walker");
    {
        // A unit created after 65,536 ids: its low 16 bits name an id no unit holds.
        sim::World w;setup(w);
        const int early=w.spawn(&type,200,200,0,0);
        w.setNextUnitId(65536+40);     // ids 2..65,575 came and went (never reused)
        const int late=w.spawn(&type,400,400,0,1);
        w.tick(1.f/30);
        std::printf("  ids %d and %d (low 16 bits %d; no unit %d)\n",early,late,late&0xffff,late&0xffff);
        check(late>65535 && w.unit(late&0xffff)==nullptr,"setup: the late unit's low 16 bits name no unit");
        check(!mayStandOn(w,early,late),"a unit with id 65,536+ blocks an older unit");
        check(!mayStandOn(w,late,early),"an older unit blocks a unit with id 65,536+");
        check(!w.mobilePlacement(type,0,sim::footprintOrigin(w.unit(late)->x,2),
                                 sim::footprintOrigin(w.unit(late)->z,2),false),
              "a unit with id 65,536+ blocks a factory's output (no asker)");
        check(w.mobilePlacement(*w.unit(late),sim::footprintOrigin(w.unit(late)->x,2),
                                sim::footprintOrigin(w.unit(late)->z,2),false),
              "a unit with id 65,536+ still does not block itself");
    }
    {
        // A unit 65,536 ids after a living one: the same low 16 bits.
        sim::World w;setup(w);
        const int old=w.spawn(&type,200,200,0,0);
        w.setNextUnitId(65536+old);
        const int alias=w.spawn(&type,400,400,0,1);
        w.tick(1.f/30);
        std::printf("  ids %d and %d (same low 16 bits: %d)\n",old,alias,int((alias&0xffff)==old));
        check((alias&0xffff)==old,"setup: the two ids share their low 16 bits");
        check(!mayStandOn(w,alias,old),"a unit with id 65,536+k does not take unit k's cells for its own");
        check(!mayStandOn(w,old,alias),"unit k does not take the cells of unit 65,536+k for its own");
    }
    if (failures) { std::printf("%d failure(s)\n",failures);return 1; }
    std::printf("placement ids: all passed\n");
    return 0;
}
