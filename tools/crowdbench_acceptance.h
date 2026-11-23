#pragma once

// Implementation-agnostic acceptance observation for crowdbench (all modes).
// Turns the navigation requirements -- route around obstacles, do not snag on
// (jagged) terrain unless actually trapped, stop instead of spinning -- into
// integer counters computed from per-tick unit state (x, z, heading) only.
//
// Static legality comes from a PRIVATE shadow World with the same terrain and
// walls and no bodies, queried through World::mobilePlacement: the exact rule
// the arrival check uses. Nothing here touches the measured World's state.
// Include from crowdbench_matrix.h only.
//
// A unit with no progress over the window (< progressPx in `window` ticks)
// is classified (crowdbench_matrix.h): crowd_held if another body stands on
// its static-field path (foot+2 steps), else terrain_stuck if it touches
// terrain, else open_idle. NARROWED 2026-10-06 BY USER DECISION: a unit that
// would be terrain_stuck but is queued behind a moving leader is crowd_held
// instead. Leader = a same-player mobile member whose centre is within two
// body widths ((foot + leader foot) * 16 px), lies within 60 degrees of the
// unit's goal direction or its static-field travel direction, and itself
// made >= progressPx of progress over the same window. A unit against a wall
// with nobody moving ahead of it is still terrain_stuck. The rule is
// observation only, identical for every mode, and changes no sim hash.
#include "sim/sim.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <queue>
#include <tuple>
#include <vector>

namespace crowdbench_acceptance {
using namespace tak::sim;

constexpr int window=90;          // W: progress / spin window, ticks
constexpr int progressPx=16;      // S: net displacement below this over W = no progress
constexpr int trappedGrace=150;   // G: ticks a trapped unit may still move or turn
constexpr int minTurnBam=364;       // 2 degrees
constexpr int64_t minStepRaw=32768; // 1/2 px in Fixed raw units
constexpr int orth=5,diag=7;      // octile step costs (16 px per 5 units)
constexpr uint16_t far=0xffff;

struct Wall {int x,z,w,h;};

// One footprint's static legality, components and lazily built distance fields.
struct Footprint {
    int foot=0;
    std::vector<uint8_t> legal;          // origin legality, no bodies
    std::vector<int32_t> label;          // 8-connected component (-1 = illegal)
    struct Field {int x0=0,z0=0,w=0,h=0;std::vector<uint16_t> d;
        uint16_t at(int x,int z) const {
            if(x<x0||z<z0||x>=x0+w||z>=z0+h)return far;
            return d[size_t(z-z0)*w+x-x0];
        }};
    std::map<std::tuple<int,int,int>,std::shared_ptr<Field>> fields;
};

class Observer {
public:
    int width=0,height=0;
    std::map<int,Footprint> prints;
    size_t fieldBytes=0;
    static constexpr size_t fieldBudget=size_t(256)<<20;
    std::unique_ptr<World> shadow;
    std::vector<int32_t> occupancy;      // cell -> unit id this tick (0 = none)
    // Scratch for Dijkstra, reset by touched list.
    std::vector<uint16_t> dist;std::vector<int> touched;

    void reset(int w,int h,const std::vector<Wall>& walls,const std::vector<int>& foots) {
        width=w;height=h;
        shadow=std::make_unique<World>();
        shadow->setTerrain(std::vector<uint8_t>(size_t(w)*h,100),w,h,64);
        std::vector<uint16_t> cells(size_t(w)*h,0xffff);
        for(const auto& r:walls)for(int z=r.z;z<r.z+r.h;++z)for(int x=r.x;x<r.x+r.w;++x)
            if(x>=0&&z>=0&&x<w&&z<h)cells[size_t(z)*w+x]=0;
        shadow->setMapPlacementFeatures(cells,{{"benchmark-wall",1,1,true,true,false,0}});
        dist.assign(size_t(w)*h,far);occupancy.assign(size_t(w)*h,0);
        fieldBytes=0;prints.clear();
        for(int f:foots) {
            auto& p=prints[f];p.foot=f;p.legal.assign(size_t(w)*h,0);p.label.assign(size_t(w)*h,-1);
            UnitType type{};type.id=type.name="acceptance-probe";type.canMove=true;type.footX=type.footZ=f;
            type.maxVel=Fixed::fromInt(1);
            Unit probe{};probe.type=&type;probe.id=65000;
            for(int z=0;z+f<=h;++z)for(int x=0;x+f<=w;++x)
                p.legal[size_t(z)*w+x]=shadow->mobilePlacement(probe,x,z,false);
            int next=0;std::vector<int> stack;
            for(int i=0;i<w*h;++i)if(p.legal[i]&&p.label[i]<0) {
                p.label[i]=next;stack.push_back(i);
                while(!stack.empty()) {
                    const int c=stack.back();stack.pop_back();
                    forNeighbours(p,c%w,c/w,[&](int n,int){if(p.label[n]<0){p.label[n]=next;stack.push_back(n);}});
                }
                ++next;
            }
        }
    }
    bool legalAt(const Footprint& p,int x,int z) const {
        return x>=0&&z>=0&&x<width&&z<height&&p.legal[size_t(z)*width+x];
    }
    // 8-connected moves; a diagonal needs both orthogonal neighbours (no corner cutting).
    template<class F> void forNeighbours(const Footprint& p,int x,int z,F&& visit) const {
        static constexpr int dx[8]={1,-1,0,0,1,1,-1,-1},dz[8]={0,0,1,-1,1,-1,1,-1};
        for(int k=0;k<8;++k) {
            const int nx=x+dx[k],nz=z+dz[k];
            if(!legalAt(p,nx,nz))continue;
            if(k>=4&&(!legalAt(p,nx,z)||!legalAt(p,x,nz)))continue;
            visit(nz*width+nx,k<4?orth:diag);
        }
    }
    static int centerPx(int origin,int foot) {return origin*16+foot*8;}
    // Legal origins whose footprint centre lies within the goal disc.
    template<class F> void goalOrigins(const Footprint& p,int gx,int gz,int radius,F&& visit) const {
        const int f=p.foot,r=radius/16+2;
        const int ox=(gx-f*8)/16,oz=(gz-f*8)/16;
        for(int z=oz-r;z<=oz+r;++z)for(int x=ox-r;x<=ox+r;++x) {
            if(!legalAt(p,x,z))continue;
            const int64_t ddx=centerPx(x,f)-gx,ddz=centerPx(z,f)-gz;
            if(ddx*ddx+ddz*ddz<=int64_t(radius)*radius)visit(z*width+x);
        }
    }
    bool reachable(int foot,int ox,int oz,int gx,int gz,int radius) const {
        const auto& p=prints.at(foot);
        if(!legalAt(p,ox,oz))return false;
        const int own=p.label[size_t(oz)*width+ox];bool found=false;
        goalOrigins(p,gx,gz,radius,[&](int c){found|=p.label[c]==own;});
        return found;
    }
    // Multi-source Dijkstra from the goal disc; returns a bbox-limited field.
    std::shared_ptr<Footprint::Field> solve(const Footprint& p,int gx,int gz,int radius) {
        using Item=std::pair<uint32_t,int>;
        std::priority_queue<Item,std::vector<Item>,std::greater<Item>> open;
        goalOrigins(p,gx,gz,radius,[&](int c){if(dist[c]){dist[c]=0;touched.push_back(c);open.push({0,c});}});
        while(!open.empty()) {
            const auto [d,c]=open.top();open.pop();
            if(d!=dist[c])continue;
            forNeighbours(p,c%width,c/width,[&](int n,int cost) {
                const uint32_t nd=d+cost;
                if(nd<dist[n]&&nd<far) {if(dist[n]==far)touched.push_back(n);dist[n]=uint16_t(nd);open.push({nd,n});}
            });
        }
        auto field=std::make_shared<Footprint::Field>();
        if(!touched.empty()) {
            int x0=width,z0=height,x1=-1,z1=-1;
            for(int c:touched) {x0=std::min(x0,c%width);x1=std::max(x1,c%width);z0=std::min(z0,c/width);z1=std::max(z1,c/width);}
            field->x0=x0;field->z0=z0;field->w=x1-x0+1;field->h=z1-z0+1;
            field->d.assign(size_t(field->w)*field->h,far);
            for(int c:touched)field->d[size_t(c/width-z0)*field->w+c%width-x0]=dist[c];
        }
        for(int c:touched)dist[c]=far;
        touched.clear();
        return field;
    }
    // Shortest legal path length (px) from an origin to the goal disc, -1 if none.
    double optimalPx(int foot,int ox,int oz,int gx,int gz,int radius) {
        auto& p=prints.at(foot);
        const auto field=solve(p,gx,gz,radius);
        const uint16_t d=field->at(ox,oz);
        return d==far?-1.0:d*16.0/orth;
    }
    // Direction field, keyed on an 8-cell goal tile so per-unit formation goals
    // share fields. nullptr when the memory budget is exhausted.
    const Footprint::Field* direction(int foot,int gx,int gz,int radius) {
        auto& p=prints.at(foot);
        const int tx=gx/128,tz=gz/128,r=std::max(radius,96);
        const auto key=std::make_tuple(tx,tz,r);
        if(auto it=p.fields.find(key);it!=p.fields.end())return it->second.get();
        if(fieldBytes>fieldBudget)return nullptr;
        auto field=solve(p,tx*128+64,tz*128+64,r);
        fieldBytes+=field->d.size()*2;
        return (p.fields[key]=field).get();
    }
};

// Per-member acceptance state.
struct Track {
    std::vector<int32_t> xs,zs;std::vector<uint16_t> turn;std::vector<uint8_t> headingFlip,travelFlip;
    int filled=0,head=0;uint32_t turnSum=0;int headingFlips=0,travelFlips=0;
    int lastTurnSign=0;int32_t lastStepX=0,lastStepZ=0;int32_t lastHeading=0;bool started=false;
    uint64_t spinTicks=0;
    int trappedSince=-1,trappedLastMotion=-1,settleMax=0,lastTrapped=-1;bool everTrapped=false,movingAfterGrace=false;
    bool everTerrainStuck=false,everCrowdHeld=false,everRawTerrain=false,everNarrowed=false;
    int finalClass=-1;double optimal=-1,pathAtGoal=-1;int side=-1;
    void init() {xs.assign(window+1,0);zs.assign(window+1,0);turn.assign(window+1,0);
        headingFlip.assign(window+1,0);travelFlip.assign(window+1,0);}
};

enum Class {AtGoal,Trapped,Progressing,CrowdHeld,TerrainStuck,OpenIdle,Warmup,Unclassified,ClassCount};
inline const char* className(int c) {
    static const char* names[]={"at_goal","trapped","progressing","crowd_held","terrain_stuck","open_idle","warmup","unclassified"};
    return names[c];
}

struct Totals {
    uint64_t classTicks[ClassCount]{};uint64_t spinTicks=0,headingStationaryBam=0,travelReversals=0;
    uint64_t spinTrapped=0,spinCrowdHeld=0,trappedMoving=0;
    // IN-12: no-progress unit-ticks touching terrain before the queued-behind-
    // a-mover narrowing; those the narrowing moved to crowd_held; and those
    // both crowd-held and touching terrain.
    uint64_t rawTerrainTicks=0,narrowedTicks=0,crowdAndTerrainTicks=0;
    double spreadSum=0,spreadMax=0;uint64_t spreadSamples=0;
};

// Signed shortest BAM difference.
inline int32_t bamDelta(int32_t now,int32_t before) {
    int32_t d=(now-before)&0xffff;return d>=0x8000?d-0x10000:d;
}

// Advance one member's window with this tick's state. Returns {stationary, spinning, turned, moved}.
struct Step {bool stationary=false,spinning=false,turned=false,moved=false,reversed=false;uint32_t turnAbs=0;};
inline Step advance(Track& t,int32_t x,int32_t z,int32_t heading) {
    Step s;
    if(!t.started) {t.started=true;t.lastHeading=heading;}
    const int32_t dh=bamDelta(heading,t.lastHeading);t.lastHeading=heading;
    const uint32_t a=uint32_t(dh<0?-dh:dh);
    const int prev=(t.head+window)%(window+1);
    const int32_t sx=t.filled?x-t.xs[prev]:0,sz=t.filled?z-t.zs[prev]:0;
    s.moved=sx||sz;s.turned=a!=0;s.turnAbs=a;
    // A heading reversal: a turn of >= 64 BAM opposite to the previous one.
    // Only visible motion counts: a heading reversal is a turn of >= 2 degrees
    // (364 BAM) opposite to the previous such turn; a travel reversal is a step
    // of >= 1/2 px whose direction opposes the previous such step. Sub-pixel
    // collision jitter of a packed crowd is not spinning.
    uint8_t hf=0;if(a>=minTurnBam) {const int sign=dh>0?1:-1;hf=t.lastTurnSign&&sign!=t.lastTurnSign;t.lastTurnSign=sign;}
    uint8_t tf=0;
    if(int64_t(sx)*sx+int64_t(sz)*sz>=minStepRaw*minStepRaw) {
        tf=(t.lastStepX||t.lastStepZ)&&int64_t(sx)*t.lastStepX+int64_t(sz)*t.lastStepZ<0;t.lastStepX=sx;t.lastStepZ=sz;
    }
    s.reversed=tf;
    // Retire the slot leaving the window, then record this tick.
    const int slot=t.head;
    if(t.filled>window) {t.turnSum-=t.turn[slot];t.headingFlips-=t.headingFlip[slot];t.travelFlips-=t.travelFlip[slot];}
    t.xs[slot]=x;t.zs[slot]=z;t.turn[slot]=uint16_t(std::min<uint32_t>(a,0xffff));t.headingFlip[slot]=hf;t.travelFlip[slot]=tf;
    t.turnSum+=t.turn[slot];t.headingFlips+=hf;t.travelFlips+=tf;
    t.head=(t.head+1)%(window+1);if(t.filled<=window)++t.filled;
    if(t.filled>window) {
        const int oldest=t.head; // slot written window ticks ago
        const int64_t dx=int64_t(x)-t.xs[oldest],dz=int64_t(z)-t.zs[oldest],lim=int64_t(progressPx)<<16;
        s.stationary=dx*dx+dz*dz<lim*lim;
        s.spinning=s.stationary&&(t.turnSum>32768||(t.headingFlips>=3&&t.turnSum>=4096)||t.travelFlips>=4);
    }
    return s;
}
}
