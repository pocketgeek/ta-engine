#pragma once
// Camera-driven priority for terrain chunk work (rendering workstream K).
//
// Pure policy -- no SDL, no threads -- so the rules are unit-tested on their own
// (tools/terrain_work_test.cpp). TerrainChunks feeds it the frame's view and asks:
//
//  - what a queued, running or completed chunk job is worth now (Tier), and in which
//    order the worker should take queued jobs (priority);
//  - which jobs are obsolete and may be cancelled, with hysteresis: work for cells
//    that just left the view stays queued (last) inside a keep band, and work of the
//    level a zoom just left stays queued for a grace period, so small reversals and a
//    zoom oscillating across a level boundary do not cancel and requeue the same work;
//  - which cached image may be evicted to make room, keeping the images that stand in
//    for visible cells whose own level is still being prepared;
//  - how much of a frame's upload budget each upload may use (UploadBudget), sections
//    and chunks together, holes first, priced by measured CPU submission cost.
//
// Chunk cells are the same world rectangles at every level (a level-n image of cell
// (cx,cy) covers what the level-0 image does, at 1/2^n the texels), so regions are in
// cell coordinates and apply to every level.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tak::terrainwork {

// Chunk-cell rectangle, inclusive bounds; empty when x1 < x0.
struct Rect {
    int x0=0,y0=0,x1=-1,y1=-1;
    bool empty() const {return x1<x0 || y1<y0;}
    bool contains(int x,int y) const {return x>=x0 && x<=x1 && y>=y0 && y<=y1;}
    // Grown by n cells on every side, clamped to a grid of nx x ny cells.
    Rect grown(int n,int nx,int ny) const {
        if(empty())return *this;
        return {std::max(0,x0-n),std::max(0,y0-n),std::min(nx-1,x1+n),std::min(ny-1,y1+n)};
    }
    friend bool operator==(const Rect&,const Rect&)=default;
};

// What a piece of terrain work is worth to the current view, most urgent first.
enum class Tier : uint8_t {
    Hole,         // visible at the wanted level, no image of any level: the section fallback draws it
    Upgrade,      // visible at the wanted level; another level's image stands in meanwhile
    Nearby,       // next to the view at the wanted level (queued only on the side the camera moves toward)
    Speculative,  // no longer wanted, but inside the keep band / level grace: kept, done last
    Obsolete      // outside the keep band, or another level past the grace: cancel
};

struct Params {
    int guard=1;      // cells around the view that count as nearby
    int keep=2;       // queued work for cells within this band of the view is kept (hysteresis)
    // A level change keeps the previous level's work (one level away) until both have
    // passed; the ms rule makes it independent of the frame rate, the frame rule of
    // stalls (a long frame does not expire it at once).
    uint64_t levelGraceMs=1000;
    uint32_t levelGraceFrames=30;
    size_t maxPending=64;   // queued + running + completed-not-uploaded jobs (the pre-K limit)
    size_t maxNearby=4;     // nearby jobs pending at once
    size_t nearbyReserve=8; // pending slots nearby work leaves free for visible work
    float minSpeed=0.5f;    // world px per frame below which the camera counts as still
    // Calm gate: no new chunk work while the camera moves fast (faster than
    // fastViewsPerSecond view widths/heights per second) or jumps again within
    // jumpStormMs of the previous jump, until it has been calm for calmMs. At level 0 the
    // section fallback draws those cells at full detail meanwhile (texel-identical to a
    // chunk), and with no reuse ahead a 4 MiB chunk upload per cell costs the render
    // thread more than the fallback it replaces (measured; see the progress record,
    // workstream K). Fast motion only up to calmGateMaxLevel: from level 1 a chunk image
    // is 4^level times smaller while the fallback still draws every 32-px tile (4^level
    // times more tiles per screen pixel, resampled), and a panned-over cell stays in view
    // long enough for its image to pay off, so gating there made zoomed-out fast pans
    // 1.4-3.2x slower (measured). Jump storms at every level (stormGateMaxLevel): a
    // storm's views are left within a second, and composing + uploading them at level 1
    // made a 7680x2160 jump storm 1.7x slower (measured).
    float fastViewsPerSecond=0.5f;
    // A storm ends only after stormCalmMs without a jump (the storm window itself):
    // reopening between the storm's jumps (after calmMs) queued work for each view that
    // the next jump cancelled, and uploaded what had finished (measured).
    uint64_t calmMs=250,jumpStormMs=1000,stormCalmMs=1000;
    int calmGateMaxLevel=0,stormGateMaxLevel=5;
    bool prioritize=true;       // worker takes the most urgent queued job (else FIFO)
    bool cancel=true;           // obsolete jobs are cancelled (else only clear()/invalidate() cancel)
    bool calmGate=true;         // pause new work during fast motion / jump storms
    // Leading-edge prefetch while the camera moves. Experimental, off by default: it
    // raised tracked GPU memory 56-80 MiB in ordinary pans with no measured gain.
    bool nearby=false;
    bool protectStandins=true;  // stand-ins are evicted last
    bool coordinate=true;       // one frame budget for section and chunk uploads, holes first
    // The pre-K behaviour: FIFO, no cancellation outside clear()/invalidate(), no nearby
    // work, plain LRU eviction, separate fixed upload caps (8 MiB chunks, 32 sections).
    static Params legacy() {
        Params p;p.prioritize=p.cancel=p.nearby=p.protectStandins=p.coordinate=p.calmGate=false;return p;
    }
};

// This frame's camera demand: the wanted level, the visible cells and the regions
// around them, the camera's motion, and how long ago the level changed.
class Demand {
public:
    // level: the wanted chunk level; visible: the cells in view (empty = no view);
    // centerX/Y: the view centre and viewW/H its size, in world px; span: world px per
    // cell; nx/ny: grid size.
    void update(int level,Rect visible,float centerX,float centerY,float viewW,float viewH,int span,int nx,int ny,
                uint64_t nowMs,uint64_t frame,const Params& p) {
        params_=p;span_=std::max(1,span);
        if(level!=level_) {
            if(level_>=0) {previousLevel_=level_;levelChangedMs_=nowMs;levelChangedFrame_=frame;}
            level_=level;
        }
        nowMs_=nowMs;frame_=frame;
        // Motion: the centre's step since the last frame, between frames of the same view
        // size (a zoom moves the centre without panning; the level grace covers zooms). A
        // step wider than the view is a jump, not motion (nothing "leads" after a jump).
        dirX_=dirY_=0;
        const bool sameSize=std::abs(viewW-lastW_)<=0.01f*viewW && std::abs(viewH-lastH_)<=0.01f*viewH;
        if(hasLast_ && sameSize && !visible.empty()) {
            const float dx=centerX-lastX_,dy=centerY-lastY_;
            const float width=float(visible.x1-visible.x0+1)*float(span_),height=float(visible.y1-visible.y0+1)*float(span_);
            if(std::abs(dx)<=width && std::abs(dy)<=height) {
                dirX_=dx>p.minSpeed ? 1 : dx<-p.minSpeed ? -1 : 0;
                dirY_=dy>p.minSpeed ? 1 : dy<-p.minSpeed ? -1 : 0;
                // Speed in view sizes per second of the owner's clock, measured from the last
                // frame the clock advanced (frames within one ms accumulate).
                if(nowMs>speedMs_ && viewW>0 && viewH>0) {
                    const double views=std::max(std::abs(centerX-speedX_)/viewW,std::abs(centerY-speedY_)/viewH)*1000./double(nowMs-speedMs_);
                    if(views>p.fastViewsPerSecond) {fastMs_=nowMs;fastSeen_=true;}
                }
            } else {   // a jump: a second one within the storm window gates new work too
                if(jumpSeen_ && nowMs-lastJumpMs_<p.jumpStormMs) {stormMs_=nowMs;stormSeen_=true;}
                lastJumpMs_=nowMs;jumpSeen_=true;
            }
        }
        if(!hasLast_ || !sameSize || nowMs>speedMs_) {speedX_=centerX;speedY_=centerY;speedMs_=nowMs;}
        lastX_=centerX;lastY_=centerY;lastW_=viewW;lastH_=viewH;hasLast_=true;
        centerX_=centerX;centerY_=centerY;
        visible_=visible;
        guard_=visible.grown(p.guard,nx,ny);
        keep_=visible.grown(std::max(p.guard,p.keep),nx,ny);
    }
    // anyImage: whether a cached image of any level covers the cell (a stand-in exists).
    Tier classify(int level,int cx,int cy,bool anyImage) const {
        if(level==level_) {
            if(visible_.contains(cx,cy))return anyImage ? Tier::Upgrade : Tier::Hole;
            if(guard_.contains(cx,cy))return Tier::Nearby;
            return keep_.contains(cx,cy) ? Tier::Speculative : Tier::Obsolete;
        }
        if(!keep_.contains(cx,cy))return Tier::Obsolete;
        return level==previousLevel_ && std::abs(level-level_)==1 && levelInGrace() ? Tier::Speculative : Tier::Obsolete;
    }
    // Smaller = sooner: the tier, then the squared distance of the cell's centre from the
    // view centre (in 1/64 cell^2), so the middle of the view fills first.
    uint32_t priority(Tier tier,int cx,int cy) const {
        const double dx=((double(cx)+.5)*span_-centerX_)/span_,dy=((double(cy)+.5)*span_-centerY_)/span_;
        const double d=std::min((dx*dx+dy*dy)*64.,double(0xffffff));
        return uint32_t(tier)<<24 | uint32_t(d);
    }
    // A cell of the guard ring on a side the camera is moving toward.
    bool leading(int cx,int cy) const {
        if(!guard_.contains(cx,cy) || visible_.contains(cx,cy))return false;
        return (dirX_>0 && cx>visible_.x1) || (dirX_<0 && cx<visible_.x0) ||
               (dirY_>0 && cy>visible_.y1) || (dirY_<0 && cy<visible_.y0);
    }
    bool moving() const {return dirX_ || dirY_;}
    // No fast motion or jump storm within calmMs (see Params): new work may be queued.
    bool calm() const {return motionCalm() && stormCalm();}
    bool motionCalm() const {return !fastSeen_ || nowMs_-fastMs_>=params_.calmMs;}
    bool stormCalm() const {return !stormSeen_ || nowMs_-stormMs_>=params_.stormCalmMs;}
    // The calm gate holds back new work this frame (Params::calmGate): fast motion up to
    // calmGateMaxLevel, jump storms up to stormGateMaxLevel.
    bool gated() const {
        return params_.calmGate && ((level_<=params_.calmGateMaxLevel && !motionCalm()) ||
                                    (level_<=params_.stormGateMaxLevel && !stormCalm()));
    }
    bool levelInGrace() const {
        return previousLevel_>=0 && (nowMs_-levelChangedMs_<params_.levelGraceMs || frame_-levelChangedFrame_<params_.levelGraceFrames);
    }
    int level() const {return level_;}
    int previousLevel() const {return previousLevel_;}
    const Rect& visible() const {return visible_;}
    const Rect& guard() const {return guard_;}
    const Rect& keep() const {return keep_;}
    int dirX() const {return dirX_;}
    int dirY() const {return dirY_;}
private:
    Params params_;
    int level_=-1,previousLevel_=-1,span_=1,dirX_=0,dirY_=0;
    uint64_t levelChangedMs_=0,levelChangedFrame_=0,nowMs_=0,frame_=0,speedMs_=0,fastMs_=0,stormMs_=0,lastJumpMs_=0;
    float lastX_=0,lastY_=0,lastW_=0,lastH_=0,centerX_=0,centerY_=0,speedX_=0,speedY_=0;
    bool hasLast_=false,fastSeen_=false,stormSeen_=false,jumpSeen_=false;
    Rect visible_,guard_,keep_;
};

// How readily a cached chunk image may be evicted to make room for an upload; lower
// ranks go first (then the least recently used), 0 = not at all.
//  1  neither visible at its level this frame nor standing in for a visible cell;
//  2  standing in for the very cell being uploaded (that upload replaces it);
//  3  standing in for another visible cell: that cell falls back to sections until its
//     own image arrives, so only when nothing else frees enough room.
// Without protectStandins every image not visible at its level is rank 1 (plain LRU).
inline int evictionRank(bool usedThisFrame,bool standinThisFrame,bool sameCell,const Params& p) {
    if(usedThisFrame)return 0;
    if(!p.protectStandins || !standinThisFrame)return 1;
    return sameCell ? 2 : 3;
}

// One frame's terrain upload budget, shared by chunk images and fallback sections.
//
// Urgent uploads fill visible holes (a chunk image for a cell without any image, a
// section the fallback needs); everything else -- an upgrade over a stand-in, nearby
// or speculative results -- is not urgent. The first upload of a frame always proceeds
// (progress, however slow uploads are on this machine); later urgent ones while the
// frame stays within urgentBytes and the estimated cost within urgentNs, later
// non-urgent ones only while the estimated cost stays within deferNs, so on a frame
// that fills holes they wait for the next frame instead of adding to its spike. Chunk
// images also keep the pre-K cap of chunkBytes per frame (the first always proceeds).
//
// Cost is the CPU submission time of texture create + upload, measured by the caller
// and folded into a per-kind estimate (bytes x ns/byte, an exponential average); no
// GPU synchronization is involved, so the estimate prices the render thread's time,
// not the GPU's.
struct UploadBudget {
    enum Kind : uint8_t {Chunk,Section};
    size_t urgentBytes=size_t(32)<<20;
    size_t chunkBytes=size_t(8)<<20;
    double urgentNs=6e6,deferNs=2e6;
    double nsPerByte[2]={0.15,0.15};   // initial guess; measured: ~0.06-0.14 here (RTX 5070, GL)
    bool coordinate=true;   // false: the pre-K caps only (chunks 8 MiB, sections by count)
    // This frame.
    size_t bytes=0,chunk=0,uploads=0,refused=0;
    double estNs=0;
    bool unlimitedSections=false;   // finishChunks frames: every section the view needs
    void beginFrame(bool unlimited) {bytes=chunk=uploads=refused=0;estNs=0;unlimitedSections=unlimited;}
    double estimate(Kind k,size_t b) const {return nsPerByte[k]*double(b);}
    bool admit(Kind k,bool urgent,size_t b) const {
        if(k==Chunk && chunk && chunk+b>chunkBytes)return false;
        if(!coordinate || (k==Section && unlimitedSections))return true;
        if(!uploads)return true;   // progress: one upload per frame, however slow uploads are
        const double est=estNs+estimate(k,b);
        if(urgent)return bytes+b<=urgentBytes && est<=urgentNs;
        return bytes+b<=urgentBytes && est<=deferNs;
    }
    void record(Kind k,size_t b,uint64_t ns) {
        bytes+=b;estNs+=estimate(k,b);++uploads;
        if(k==Chunk)chunk+=b;
        if(b>=4096) {   // tiny uploads are dominated by fixed costs: they would skew the rate
            const double sample=std::clamp(double(ns)/double(b),0.005,50.);
            nsPerByte[k]+=(sample-nsPerByte[k])*0.125;
        }
    }
};

}  // namespace tak::terrainwork
