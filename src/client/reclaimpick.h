#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace tak::client {

// Reclaim target picking, shared by the broom cursor, the right-click order and the
// armed CLEAR order (GameView::reclaimTargetAt), plus the right-drag area preview.
// The Revive cursor sizes corpses with the same pick circle.
// Header-only so a test can drive it without a GameView.
//
// One radius rule for features AND corpses, from the FEATURE footprint. Retail
// installs a corpse as an ordinary map feature of its corpse/statue feature type
// (512ee0, verified cell for cell by tools/re/check_corpse_features.py), and the
// sim mirrors that in corpseFootprints_ with the feature type's fx/fz. Sizing a
// corpse from the dead UNIT's footX/footZ, as the click paths once did, made the
// cursor and the click disagree around every wreck whose feature is a different size.
inline float reclaimPickRadius(int fx,int fz) { return 18.f+8.f*float(std::max(fx,fz)); }

// Nearest candidate whose pick circle holds the point. Ids are sim ids: a feature
// id (which can be 0, the feature anchored at cell 0,0), or -unitId for a corpse.
struct ReclaimPick {
    ReclaimPick(float x,float z):wx(x),wz(z) {}
    float wx,wz;
    std::optional<int> id;
    float best=1e18f;
    void offer(int targetId,float x,float z,int fx,int fz) {
        const float dx=x-wx,dz=z-wz,d=dx*dx+dz*dz,r=reclaimPickRadius(fx,fz);
        if (d<r*r && d<best) { best=d;id=targetId; }
    }
};

// Area preview: does the sim's sampling of the box land in a footprint of 16px cells
// [cx,cx+fx) x [cz,cz+fz)? tickReclaimArea samples the box every 16px from its min
// corner (retailReclaimAreaTarget) and resolves each sample's cell back to its anchor,
// so a multi-cell tree whose centre lies outside the box is still cleared when a
// sample hits one of its cells -- and a cell the box only grazes past the last sample
// is NOT. Per axis: the first sample at or after the footprint's first pixel must
// still be inside both the box and the footprint.
inline bool reclaimSampleHits(int c,int f,float lo,float hi) {
    const float start=float(c*16);
    const float s=lo>=start?lo:lo+16.f*std::ceil((start-lo)/16.f);
    return s<=hi && s<float((c+f)*16);
}
inline bool reclaimFootprintInBox(int cx,int cz,int fx,int fz,float minx,float maxx,float minz,float maxz) {
    return reclaimSampleHits(cx,fx,minx,maxx) && reclaimSampleHits(cz,fz,minz,maxz);
}

}
