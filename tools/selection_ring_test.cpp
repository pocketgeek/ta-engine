// selection_ring_test -- the selection ring's box and points against retail's routine
// (icd 0x4fd0d0, box 0x546fa0 with recurse 0, angles 0x535d50 with unit+0x7c).
#include "client/selectionring.h"

#include <cstdio>

namespace {
int failures=0;
void expect(bool ok,const char* what) {
    if(!ok) {std::printf("FAIL: %s\n",what);++failures;}
}
bool near(float a,float b) {return std::fabs(a-b)<0.001f;}

// A root plate of half size (hx,hz) at y=0 under a tall narrow child, as zonglyph.
tak::tdo::Object tallModel(float hx,float hz) {
    tak::tdo::Object root;
    root.name="zonground";
    root.vertices={-hx,0,-hz, hx,0,-hz, hx,0,hz, -hx,0,hz};
    tak::tdo::Object staff;
    staff.name="staff";
    staff.y=20;
    staff.vertices={-30,0,-20, 30,0,20, 0,124,0};
    root.children.push_back(staff);
    return root;
}
}

int main() {
    const float kRad=6.28318530717959f;
    {   // The children are not read: a tall model's ring lies at its feet.
        const auto box=selectionRingBox(tallModel(38,39));
        expect(near(box.halfX,38) && near(box.halfZ,39),"radii come from the root plate, not the model");
        expect(near(box.midX,0) && near(box.midY,0) && near(box.midZ,0),"ring centre is the plate's, at the feet");
    }
    {   // Offset plates centre the ring on all three axes (tarhell's is at z -19).
        tak::tdo::Object root;
        root.vertices={-85,0,-97, 85,0,-97, 85,0,59, -85,0,59};
        const auto box=selectionRingBox(root);
        expect(near(box.midZ,-19) && near(box.halfZ,78),"ring centre follows an offset plate");
    }
    {   // Both corners start at zero, so the origin is always inside the box.
        tak::tdo::Object root;
        root.vertices={10,0,10, 20,0,10, 20,0,30, 10,0,30};
        const auto box=selectionRingBox(root);
        expect(near(box.halfX,10) && near(box.midX,10) && near(box.halfZ,15) && near(box.midZ,15),
               "the box includes the origin");
    }
    {   // The root's own offset counts; a root of 2 vertices or fewer adds nothing.
        tak::tdo::Object root;
        root.x=4;root.y=6;
        root.vertices={-4,0,-4, 4,0,-4, 4,0,4};
        const auto box=selectionRingBox(root);
        expect(near(box.midX,4) && near(box.midY,3),"the root's offset is added");
        tak::tdo::Object two;
        two.vertices={-9,0,-9, 9,9,9};
        const auto none=selectionRingBox(two);
        expect(near(none.halfX,0) && near(none.halfZ,0) && near(none.midY,0),"a 2-vertex root adds nothing");
    }
    const Xform flat=modelBodyTransform(0,0);
    {   // Unturned: z reads up-screen 1:1, x reads across.
        const SelectionRingBox box{10,20,0,0,0};
        const auto east=selectionRingPoint(box,12,24,1,0,flat,1,0);
        const auto north=selectionRingPoint(box,12,24,0,1,flat,1,0);
        expect(near(east.x,12) && near(east.y,0),"x radius lies across the screen");
        expect(near(north.x,0) && near(north.y,-24),"z radius lies up the screen, not foreshortened");
    }
    {   // The ring turns with the unit, by the body's transform.
        const SelectionRingBox box{10,20,0,0,0};
        const float facing=kRad/4,cosF=std::cos(facing),sinF=std::sin(facing);
        const auto a=selectionRingPoint(box,12,24,1,0,flat,cosF,sinF);
        const auto b=selectionRingPoint(box,12,24,0,1,flat,cosF,sinF);
        expect(near(a.x,0) && near(std::fabs(a.y),12),"a quarter turn swaps the x radius onto screen y");
        expect(near(std::fabs(b.x),24) && near(b.y,0),"a quarter turn swaps the z radius onto screen x");
        // The ring's point is exactly where a plate vertex is drawn.
        float w[3];flat.apply(12,0,0,w);
        const float rz=-w[0]*sinF+w[2]*cosF;
        expect(near(a.x,w[0]*cosF+w[2]*sinF) && near(a.y,-(w[1]*kProjY+rz*kProjZ)),"ring and body share a transform");
    }
    {   // An offset centre turns about the unit, and height reads at half.
        const SelectionRingBox box{10,10,0,8,-19};
        const auto c=selectionRingPoint(box,0,0,1,0,flat,1,0);
        expect(near(c.x,0) && near(c.y,19-4),"centre: y at half, z at one");
        const auto turned=selectionRingPoint(box,0,0,1,0,flat,-1,0);
        expect(near(turned.y,-19-4),"an offset centre swings round with the heading");
    }
    if(failures) {std::printf("selection_ring: %d failure(s)\n",failures);return 1;}
    std::printf("selection_ring: ok\n");
    return 0;
}
