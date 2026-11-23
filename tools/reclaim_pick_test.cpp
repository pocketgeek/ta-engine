// Reclaim target picking (client/reclaimpick.h): the one radius rule shared by the broom
// cursor, the right-click and armed CLEAR orders, and the area preview's cell test.
#include "client/reclaimpick.h"
#include <cstdio>
#include <stdexcept>

using namespace tak::client;
static void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}

int main()try {
    check(reclaimPickRadius(1,1)==26.f && reclaimPickRadius(2,3)==42.f,"radius is 18 + 8 * the larger footprint side");
    {   // A 4x4 unit leaving a 1x1 wreck: the pick circle is the WRECK's (26), not the
        // unit's (50). The click paths once used the unit footprint, so a click 30px off
        // ordered a reclaim the broom cursor never offered.
        ReclaimPick pick{130.f,100.f};
        pick.offer(-7,100.f,100.f,1,1);
        check(!pick.id,"corpse is sized by its feature footprint, not the dead unit's");
        ReclaimPick near{120.f,100.f};
        near.offer(-7,100.f,100.f,1,1);
        check(near.id && *near.id==-7,"corpse inside its feature radius is picked as -unitId");
    }
    {   // Nearest wins across features and corpses, whatever the offer order.
        ReclaimPick pick{100.f,100.f};
        pick.offer(5,120.f,100.f,2,2);
        pick.offer(-3,108.f,100.f,1,1);
        pick.offer(9,90.f,100.f,1,1);
        check(pick.id && *pick.id==-3,"closest candidate wins");
    }
    {   // Feature id 0 is the feature anchored at cell 0,0 -- a real target, not "none".
        ReclaimPick pick{10.f,10.f};
        pick.offer(0,8.f,8.f,1,1);
        check(pick.id && *pick.id==0,"feature id 0 is pickable");
        ReclaimPick none{500.f,500.f};
        none.offer(0,8.f,8.f,1,1);
        check(!none.id,"nothing in range picks nothing");
    }
    {   // Area preview: cells, not centres. A 3x3 tree anchored at cell (10,10) spans
        // x 160..208; its centre (184) is outside a box ending at x=170, one cell is inside.
        check(reclaimFootprintInBox(10,10,3,3,0.f,170.f,0.f,170.f),"multi-cell footprint touching the box is caught");
        check(!reclaimFootprintInBox(10,10,3,3,0.f,159.f,0.f,170.f),"footprint left of the box edge is not");
        check(reclaimFootprintInBox(10,10,3,3,0.f,160.f,0.f,160.f),"box edge on the anchor cell's first pixel counts");
        check(!reclaimFootprintInBox(10,10,3,3,208.f,300.f,0.f,300.f),"the pixel past the footprint belongs to the next cell");
        check(reclaimFootprintInBox(10,10,3,3,207.f,300.f,0.f,300.f),"the footprint's last pixel counts");
        // The sim samples every 16px from the box's min corner: a box 150..161 samples
        // x=150 only (cell 9), so the footprint it grazes at 160..161 is not cleared.
        check(!reclaimFootprintInBox(10,10,3,3,150.f,161.f,0.f,300.f),"a cell grazed past the last sample is not taken");
        check(reclaimFootprintInBox(10,10,3,3,150.f,166.f,0.f,300.f),"the next sample (166) lands in the footprint");
    }
    std::puts("reclaim_pick: ok");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr,"reclaim_pick FAILED: %s\n",e.what());
    return 1;
}
