#pragma once
#include "crt/crt.h"
#include <string>
namespace cart {
// Renames update typed location operands; other strings are never rewritten.
bool setRegion(tak::crt::Scenario& scenario,int index,tak::crt::Region region,
               int width,int height,std::string& error);
bool removeRegion(tak::crt::Scenario& scenario,int index,std::string& error);

// Canvas gesture in map-cell coordinates. Preview stays separate from the
// document until mouse-up, allowing Escape and one history entry per gesture.
struct RegionDrag {
    enum Part { None=0,Left=1,Right=2,Top=4,Bottom=8,Move=16,Draw=32 };
    int index=-1,part=None,grabX=0,grabZ=0;
    float startX=0,startZ=0;
    tak::crt::Region original,preview;
    static int hit(const tak::crt::Region&,float x,float z,float tolerance);
    bool begin(const std::vector<tak::crt::Region>&,int preferred,float x,float z,
               float tolerance,bool create,int width,int height);
    void update(float x,float z,int width,int height);
    void cancel() {part=None;index=-1;}
    bool active() const {return part!=None;}
};
}
