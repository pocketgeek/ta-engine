#pragma once

// Retail's selection ring (icd 0x4fd0d0): the sizes and the points, kept apart from
// the drawing so tools/selection_ring_test.cpp can hold them to the binary.

#include "client/appglobals.h"   // kProjY/kProjZ
#include "client/modelmath.h"    // Xform
#include "tdo/tdo.h"

#include <algorithm>
#include <cmath>

// The box retail reads from 0x546fa0 with its recurse flag 0: the ROOT piece alone,
// which is the unit's ground plate, so the ring lies at the feet and is the plate's
// size however tall the model. It starts at the origin (both corners zeroed, so the
// origin is always inside), counts every vertex, and a root of 2 vertices or fewer
// adds none.
struct SelectionRingBox {
    float halfX=12.0f,halfZ=12.0f;   // defaults cover a model that never loaded
    float midX=0.0f,midY=0.0f,midZ=0.0f;
};

inline SelectionRingBox selectionRingBox(const tak::tdo::Object& root) {
    float lo[3]={0,0,0},hi[3]={0,0,0};
    const float offset[3]={root.x,root.y,root.z};
    if(root.vertices.size()/3>2)
        for(size_t v=0;v+2<root.vertices.size();v+=3)
            for(int k=0;k<3;++k) {
                const float w=root.vertices[v+size_t(k)]+offset[k];
                lo[k]=std::min(lo[k],w);hi[k]=std::max(hi[k],w);
            }
    return {(hi[0]-lo[0])*0.5f,(hi[2]-lo[2])*0.5f,(hi[0]+lo[0])*0.5f,(hi[1]+lo[1])*0.5f,(hi[2]+lo[2])*0.5f};
}

// One ring point at angle (cosA,sinA), relative to the model origin on screen at zoom
// 1. radiusX/radiusZ are 1.2 times the box's half extents (the double at 0x5f03f0).
// Retail turns the point by the unit's three angles (0x535d50 with unit+0x7c), so
// this is the transform the body's own vertices take: a ring lies on the ground plate.
inline SDL_FPoint selectionRingPoint(const SelectionRingBox& box,float radiusX,float radiusZ,
                                     float cosA,float sinA,const Xform& body,float cosF,float sinF) {
    float w[3];
    body.apply(box.midX+radiusX*cosA,box.midY,box.midZ+radiusZ*sinA,w);
    const float rz=-w[0]*sinF+w[2]*cosF;
    return {w[0]*cosF+w[2]*sinF,-(w[1]*kProjY+rz*kProjZ)};
}
