#pragma once
// SDL-free scrolling math for the editor chrome: the left browser's scrollbar
// (thumb size/position, drag and track paging, clamping) and the map canvas's
// optional edge scrolling band. Kept here so tools/cartographer_document_test can
// check it without a window; palette.cpp and chrome.cpp draw and wire it.

#include <algorithm>

namespace cart {

// A vertical scrollbar beside a clipped list. All values are logical pixels:
// the track rectangle, the full content height and the visible (viewport) height.
struct ScrollBar {
    int x=0,y=0,w=0,h=0;
    int content=0,view=0;
    static constexpr int kMinThumb=20;   // still grabbable over a 1,000-row list
    int maxScroll() const {return std::max(0,content-view);}
    bool active() const {return content>view && view>0 && h>0;}
    int clamp(int scroll) const {return std::clamp(scroll,0,maxScroll());}
    int thumbHeight() const {return active()?std::clamp(int((long long)h*view/content),std::min(kMinThumb,h),h):h;}
    int thumbTop(int scroll) const {
        const int range=h-thumbHeight(),most=maxScroll();
        return y+(most>0?int((long long)range*clamp(scroll)/most):0);
    }
    bool inTrack(int px,int py) const {return active() && px>=x && px<x+w && py>=y && py<y+h;}
    bool inThumb(int px,int py,int scroll) const {return inTrack(px,py) && py>=thumbTop(scroll) && py<thumbTop(scroll)+thumbHeight();}
    // Dragging: the thumb's top follows the pointer minus where it was grabbed.
    int scrollForThumb(int top) const {
        const int range=h-thumbHeight();
        return range>0?clamp(int(((long long)(top-y)*maxScroll()+range/2)/range)):0;
    }
    // A track click outside the thumb pages one viewport toward the pointer.
    int page(int scroll,int py) const {
        if(py<thumbTop(scroll))return clamp(scroll-view);
        if(py>=thumbTop(scroll)+thumbHeight())return clamp(scroll+view);
        return clamp(scroll);
    }
};

// Edge scrolling: which way the view moves while the pointer rests within `band`
// pixels of the canvas rectangle's inside edges (-1/0/+1 per axis). Outside the
// canvas (palette, menus, status bar) nothing scrolls.
struct EdgeDirection {int x=0,z=0;bool any() const {return x || z;}};
inline EdgeDirection edgeScrollDirection(int px,int py,int left,int top,int width,int height,int band) {
    EdgeDirection d;
    if(width<=0 || height<=0 || px<left || py<top || px>=left+width || py>=top+height)return d;
    band=std::min(band,std::min(width,height)/4);   // a tiny window keeps a usable middle
    if(px<left+band)d.x=-1;else if(px>=left+width-band)d.x=1;
    if(py<top+band)d.z=-1;else if(py>=top+height-band)d.z=1;
    return d;
}
// The window's outermost pixel row and column (window coordinates), whatever lies
// under them: a fullscreen or maximised window's pointer stops there at the screen
// edge, over the browser, menu bar or status bar. Outside the window: nothing.
inline EdgeDirection windowRimDirection(int px,int py,int width,int height) {
    EdgeDirection d;
    if(width<=0 || height<=0 || px<0 || py<0 || px>=width || py>=height)return d;
    if(px==0)d.x=-1;else if(px==width-1)d.x=1;
    if(py==0)d.z=-1;else if(py==height-1)d.z=1;
    return d;
}
// Edge scrolling moves six keyboard arrow steps (MapView: 200 screen px each) per
// second, in screen pixels, so it is as quick as holding an arrow at a slow repeat.
constexpr float kEdgeScrollPixelsPerSecond=6*200.0f;
constexpr int kEdgeScrollBand=16;          // logical px inside the canvas edge
constexpr unsigned kEdgeScrollDelayMs=150; // dwell first: crossing the band toward the browser or menus is not a scroll

}
