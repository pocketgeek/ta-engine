// viewcount_bench -- the current-view count footprint updates of a real game,
// replayed through retail's full removal and re-add (retailUpdateSight) and
// through the delta path (retailSightMove), on the same terrain. Timing and
// operation counts per update; both planes must end equal.
//
//   viewcount_bench <situation.scn> --data <install> [--mode retail|legion]
//       [--warm N] [--ticks N] [--reps N] [--atomic] [--save FILE]
//   viewcount_bench --load FILE [--reps N] [--atomic]
//   viewcount_bench --synthetic [--reps N] [--atomic]
//
// A situation (tools/scenarios/we-timing, decompressed) runs N warm-up ticks,
// then every unit's view footprint is compared before and after each of the
// next --ticks ticks: a footprint that stays counted on one plane and moves
// (cell or eye) is a "move"; new, dropped and re-owned footprints are counted
// apart (both paths treat them alike). The moves are then replayed --reps
// times each way, alternating, plain adds (serial pass) or --atomic relaxed
// adds (the worker pass). --synthetic replays generated moves instead (flat,
// rolling and rough terrain; the audit probe's flat sight-320 disc).
// --save writes the terrain and the captured moves to FILE (capture on the
// build/test cores); --load replays such a file without running the world
// (the part to time on the timing cores).
#include "legion_scn.h"
#include "sim/retailviewcounts.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace tak::sim;

namespace {

struct Move { RetailSightFootprint from,to; int8_t player; };

struct Terrain {
    int width=0,height=0;
    std::vector<RetailExplorationHeight> heights;
};

struct Ops { uint64_t visits=0,reads=0; };

template<bool Atomic>
struct Counter {
    uint16_t* plane;int width;Ops* ops;
    void operator()(int x,int z,int delta,uint16_t) const {
        uint16_t& cell=plane[size_t(z)*width+x];
        if constexpr (Atomic) __atomic_fetch_add(&cell,uint16_t(delta),__ATOMIC_RELAXED);
        else cell=uint16_t(cell+delta);
        if (ops) ++ops->visits;
    }
};

template<bool Atomic>
void rebuildOne(const Terrain& t,const Move& m,uint16_t* plane,Ops* ops) {
    auto heightAt=[&](int x,int z){if (ops) ++ops->reads;return t.heights[size_t(z)*t.width+x];};
    Counter<Atomic> visit{plane,t.width,ops};
    RetailSightFootprint s=m.from;
    retailSightFootprint(s,false,false,t.width,t.height,0,heightAt,visit);
    s=m.to;s.active=false;
    retailSightFootprint(s,true,false,t.width,t.height,0,heightAt,visit);
}

template<bool Atomic>
void deltaOne(const Terrain& t,const Move& m,uint16_t* plane,Ops* ops) {
    auto heightAt=[&](int x,int z){if (ops) ++ops->reads;return t.heights[size_t(z)*t.width+x];};
    Counter<Atomic> visit{plane,t.width,ops};
    retailSightMove(m.from,m.to,t.width,t.height,heightAt,visit);
}

// The rejected sub-experiment, kept for the record: retailSightMove's rows
// and segments, but each cell admitted by retailSightAdmits (its double
// products per cell and footprint) instead of the per-height limit tables.
template<bool Atomic>
void directOne(const Terrain& t,const Move& m,uint16_t* plane,Ops* ops) {
    auto heightAt=[&](int x,int z){if (ops) ++ops->reads;return t.heights[size_t(z)*t.width+x];};
    Counter<Atomic> visit{plane,t.width,ops};
    const auto& from=m.from;const auto& to=m.to;
    const int radius=retailSightRadius(from);
    if (radius<0) return;
    const int radiusSquared=radius*radius;
    const float slope=retailSightSlope(from);
    RetailSightRows fromRows(from,radius,t.width),toRows(to,radius,t.width);
    const int z0=std::max(0,std::min(int(from.z),int(to.z))-radius);
    const int z1=std::min(t.height-1,std::max(int(from.z),int(to.z))+radius);
    for (int z=z0;z<=z1;++z) {
        int fromLo,fromHi,toLo,toHi;
        fromRows.span(z,fromLo,fromHi);
        toRows.span(z,toLo,toHi);
        auto fromOnly=[&](int lo,int hi) {
            for (int x=lo;x<=hi;++x) if (retailSightAdmits(from,x,z,radiusSquared,slope,heightAt)) visit(x,z,-1,0);
        };
        auto toOnly=[&](int lo,int hi) {
            for (int x=lo;x<=hi;++x) if (retailSightAdmits(to,x,z,radiusSquared,slope,heightAt)) visit(x,z,1,0);
        };
        const int bothLo=std::max(fromLo,toLo),bothHi=std::min(fromHi,toHi);
        if (bothLo>bothHi) {fromOnly(fromLo,fromHi);toOnly(toLo,toHi);continue;}
        fromOnly(fromLo,bothLo-1);toOnly(toLo,bothLo-1);
        for (int x=bothLo;x<=bothHi;++x) {
            const auto heights=heightAt(x,z);
            auto at=[&](int,int){return heights;};
            const bool was=retailSightAdmits(from,x,z,radiusSquared,slope,at);
            const bool now=retailSightAdmits(to,x,z,radiusSquared,slope,at);
            if (was!=now) visit(x,z,now ? 1 : -1,0);
        }
        fromOnly(bothHi+1,fromHi);toOnly(bothHi+1,toHi);
    }
}

enum Way { kRebuild, kDelta, kDirect };

template<bool Atomic>
void one(Way way,const Terrain& t,const Move& m,uint16_t* plane,Ops* ops) {
    if (way==kDelta) deltaOne<Atomic>(t,m,plane,ops);
    else if (way==kDirect) directOne<Atomic>(t,m,plane,ops);
    else rebuildOne<Atomic>(t,m,plane,ops);
}

template<bool Atomic>
double timeAll(const Terrain& t,const std::vector<Move>& moves,std::vector<uint16_t>& planes,Way way) {
    const size_t cells=size_t(t.width)*t.height;
    const auto start=std::chrono::steady_clock::now();
    for (const auto& m:moves) one<Atomic>(way,t,m,planes.data()+size_t(m.player)*cells,nullptr);
    return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
}

struct Pct { double mean,median,p95,p99,max; };
Pct pct(std::vector<double> v) {
    std::sort(v.begin(),v.end());
    double sum=0;for (double x:v) sum+=x;
    auto at=[&](double q){return v[std::min(v.size()-1,size_t(q*double(v.size())))];};
    return {sum/double(v.size()),at(0.5),at(0.95),at(0.99),v.back()};
}

int report(const char* label,const Terrain& t,const std::vector<Move>& moves,int players,int reps,bool atomic) {
    if (moves.empty()) {std::printf("%s: no moves\n",label);return 0;}
    const size_t cells=size_t(t.width)*t.height;
    // Correctness: one pass each way from zero planes.
    std::vector<uint16_t> a(cells*size_t(players),0),b(cells*size_t(players),0),c(cells*size_t(players),0);
    Ops ra,da,ca;
    for (const auto& m:moves) {
        rebuildOne<false>(t,m,a.data()+size_t(m.player)*cells,&ra);
        deltaOne<false>(t,m,b.data()+size_t(m.player)*cells,&da);
        directOne<false>(t,m,c.data()+size_t(m.player)*cells,&ca);
    }
    const bool equal=a==b && a==c;
    // Per-update operation counts, and the cells whose count changes.
    std::vector<double> perRebuild,perDelta;
    for (const auto& m:moves) {
        Ops r,d;
        std::vector<uint16_t> scratch(cells,0);
        rebuildOne<false>(t,m,scratch.data(),&r);
        deltaOne<false>(t,m,scratch.data(),&d);
        perRebuild.push_back(double(r.visits));perDelta.push_back(double(d.visits));
    }
    // Timing: alternate the two ways, reps times; ns per update.
    std::vector<double> tr,td,tc;
    std::vector<uint16_t> planes(cells*size_t(players),0);
    auto timed=[&](Way way){
        const double ns=atomic ? timeAll<true>(t,moves,planes,way) : timeAll<false>(t,moves,planes,way);
        return ns/double(moves.size());
    };
    for (int r=0;r<reps;++r) {   // alternating, the order rotated each rep
        const Way order[3][3]={{kRebuild,kDelta,kDirect},{kDelta,kDirect,kRebuild},{kDirect,kRebuild,kDelta}};
        for (Way way:order[r%3]) (way==kRebuild ? tr : way==kDelta ? td : tc).push_back(timed(way));
    }
    // Per move (one timed call each, after the reps warmed the caches): the
    // cost distribution and its split by radius, where the large sights sit.
    std::vector<double> mr,md;
    std::map<int,std::pair<double,double>> byRadius;   // radius class -> summed ns (rebuild, delta)
    std::map<int,size_t> byRadiusN;
    for (const auto& m:moves) {
        uint16_t* plane=planes.data()+size_t(m.player)*cells;
        auto t0=std::chrono::steady_clock::now();
        if (atomic) one<true>(kRebuild,t,m,plane,nullptr); else one<false>(kRebuild,t,m,plane,nullptr);
        auto t1=std::chrono::steady_clock::now();
        if (atomic) one<true>(kDelta,t,m,plane,nullptr); else one<false>(kDelta,t,m,plane,nullptr);
        auto t2=std::chrono::steady_clock::now();
        const double a=std::chrono::duration<double,std::nano>(t1-t0).count();
        const double b=std::chrono::duration<double,std::nano>(t2-t1).count();
        mr.push_back(a);md.push_back(b);
        const int r=retailSightRadius(m.to),k=r<=8 ? 8 : r<=16 ? 16 : r<=32 ? 32 : 9999;
        byRadius[k].first+=a;byRadius[k].second+=b;++byRadiusN[k];
    }
    const Pct pmr=pct(mr),pmd=pct(md);
    const Pct pr=pct(tr),pd=pct(td),pc=pct(tc);
    const Pct vr=pct(perRebuild),vd=pct(perDelta);
    std::printf("%s: %zu moves, planes %s\n",label,moves.size(),equal ? "EQUAL" : "DIFFERENT");
    std::printf("  counter updates per move: rebuild mean %.1f (p99 %.0f max %.0f), delta mean %.1f (p99 %.0f max %.0f)\n",
                vr.mean,vr.p99,vr.max,vd.mean,vd.p99,vd.max);
    std::printf("  terrain reads per move:   rebuild %.1f, delta %.1f, direct %.1f\n",
                double(ra.reads)/double(moves.size()),double(da.reads)/double(moves.size()),
                double(ca.reads)/double(moves.size()));
    std::printf("  ns per move (%s, %d reps, per-rep means): rebuild median %.1f (mean %.1f p95 %.1f max %.1f) | "
                "delta median %.1f (mean %.1f p95 %.1f max %.1f) | direct median %.1f (mean %.1f) | "
                "delta/rebuild %.3f direct/rebuild %.3f\n",
                atomic ? "atomic" : "plain",reps,pr.median,pr.mean,pr.p95,pr.max,pd.median,pd.mean,pd.p95,pd.max,
                pc.median,pc.mean,pd.median/pr.median,pc.median/pr.median);
    std::printf("  per move ns (one call each): rebuild median %.0f p95 %.0f p99 %.0f max %.0f | delta median %.0f p95 %.0f p99 %.0f max %.0f\n",
                pmr.median,pmr.p95,pmr.p99,pmr.max,pmd.median,pmd.p95,pmd.p99,pmd.max);
    std::printf("  by radius (moves, share of rebuild ns, delta/rebuild):");
    double all=0;for (const auto& [k,v]:byRadius) all+=v.first;
    for (const auto& [k,v]:byRadius)
        std::printf(" r<=%d %zu %.0f%% %.3f;",k==9999 ? 99999 : k,byRadiusN[k],100*v.first/all,v.second/v.first);
    std::printf("\n");
    return equal ? 0 : 1;
}

void histogram(const char* label,const std::map<int,size_t>& h) {
    std::printf("  %s:",label);
    for (const auto& [k,n]:h) std::printf(" %d:%zu",k,n);
    std::printf("\n");
}

// Trace file: "VCB1", width, height, players, moves, then the heights and moves raw.
bool save(const char* path,const Terrain& t,int players,const std::vector<Move>& moves) {
    FILE* f=std::fopen(path,"wb");
    if (!f) return false;
    const int32_t head[4]={t.width,t.height,players,int32_t(moves.size())};
    bool ok=std::fwrite("VCB1",1,4,f)==4 && std::fwrite(head,sizeof head,1,f)==1 &&
            std::fwrite(t.heights.data(),sizeof(RetailExplorationHeight),t.heights.size(),f)==t.heights.size() &&
            (moves.empty() || std::fwrite(moves.data(),sizeof(Move),moves.size(),f)==moves.size());
    return std::fclose(f)==0 && ok;
}

bool load(const char* path,Terrain& t,int& players,std::vector<Move>& moves) {
    FILE* f=std::fopen(path,"rb");
    if (!f) return false;
    char magic[4];int32_t head[4];
    bool ok=std::fread(magic,1,4,f)==4 && std::memcmp(magic,"VCB1",4)==0 && std::fread(head,sizeof head,1,f)==1 &&
            head[0]>0 && head[1]>0 && head[0]<=4096 && head[1]<=4096 && head[2]>0 && head[2]<=32 && head[3]>=0;
    if (ok) {
        t.width=head[0];t.height=head[1];players=head[2];
        t.heights.resize(size_t(t.width)*t.height);moves.resize(size_t(head[3]));
        ok=std::fread(t.heights.data(),sizeof(RetailExplorationHeight),t.heights.size(),f)==t.heights.size() &&
           (moves.empty() || std::fread(moves.data(),sizeof(Move),moves.size(),f)==moves.size());
    }
    std::fclose(f);
    if (ok)
        for (const auto& m:moves) ok=ok && m.player>=0 && m.player<players;
    return ok;
}

int situation(const char* file,const char* data,PathfindingMode mode,int warm,int ticks,int reps,bool atomic,
              const char* savePath) {
    const auto s=tak::scn::load(file);
    tak::scn::BuildOptions bo;
    bo.mode=mode;bo.serial=false;bo.data=data;
    auto b=tak::scn::build(s,bo);
    if (!b->skipped.empty()) {std::fprintf(stderr,"%s\n",b->skipped.c_str());return 77;}
    World& w=*b->world;
    tak::scn::OrderFeed feed(s,*b);
    Terrain t;
    t.width=w.viewPlaneW();t.height=w.viewPlaneH();
    t.heights=retailExplorationHeights(w.mapW(),w.mapH(),uint8_t(w.mapSea()),
        [&](int x,int z){return w.mapHeights()[size_t(z)*w.mapW()+x];});
    std::vector<Move> moves;
    std::unordered_map<int,std::pair<RetailSightFootprint,int8_t>> last;
    size_t added=0,dropped=0,reowned=0,eyeOnly=0;
    std::map<int,size_t> radius,step,eyeStep,spread;
    for (int tick=0;tick<warm+ticks;++tick) {
        feed.apply(uint32_t(tick));
        if (tick==0) w.setPathBudget(1<<28);
        else if (tick==60) w.setPathBudget(kPathBudgetDefault);
        w.tick(1.f/30);
        const bool record=tick>=warm;
        for (const auto& u:w.units()) {
            auto& was=last[u.id];
            const RetailSightFootprint& now=u.viewSight;
            const RetailSightFootprint before=was.first;
            const int8_t beforePlayer=was.second;
            was={now,u.viewPlayer};
            if (!record) continue;
            const bool changed=before.x!=now.x || before.z!=now.z || before.eyeHeight!=now.eyeHeight ||
                               before.active!=now.active;
            if (!changed && beforePlayer==u.viewPlayer) continue;
            if (!before.active && now.active) {++added;continue;}
            if (before.active && !now.active) {++dropped;continue;}
            if (!before.active) continue;
            if (beforePlayer!=u.viewPlayer || before.distance!=now.distance ||
                before.sightHeight!=now.sightHeight) {++reowned;continue;}
            moves.push_back({before,now,u.viewPlayer});
            eyeOnly+=before.x==now.x && before.z==now.z;
            ++radius[retailSightRadius(now)];
            ++step[std::max(std::abs(now.x-before.x),std::abs(now.z-before.z))];
            ++eyeStep[std::min(64,std::abs(now.eyeHeight-before.eyeHeight))/4*4];
            // Terrain spread (lowest-corner height) under the new disc.
            const int r=retailSightRadius(now);
            int lo=255,hi=0;
            for (int z=std::max(0,now.z-r);z<=std::min(t.height-1,now.z+r);++z)
                for (int x=std::max(0,now.x-r);x<=std::min(t.width-1,now.x+r);++x) {
                    const auto h=t.heights[size_t(z)*t.width+x];
                    lo=std::min<int>(lo,std::min(h[0],h[1]));hi=std::max<int>(hi,std::min(h[0],h[1]));
                }
            ++spread[(hi-lo)/16*16];
        }
    }
    std::printf("situation %s (%s): %d warm-up + %d recorded ticks, %zu units at the end\n",file,
                mode==PathfindingMode::Retail ? "retail" : "legion",warm,ticks,w.units().size());
    std::printf("  footprint changes: %zu moves (%zu eye-only), %zu new, %zu dropped, %zu re-owned/resized; %.1f moves per tick\n",
                moves.size(),eyeOnly,added,dropped,reowned,double(moves.size())/std::max(1,ticks));
    histogram("radius (cells)",radius);
    histogram("cell step",step);
    histogram("eye change (bucket 4)",eyeStep);
    histogram("terrain spread under the disc (bucket 16)",spread);
    if (savePath) {
        if (!save(savePath,t,w.numPlayers(),moves)) {std::fprintf(stderr,"viewcount_bench: cannot write %s\n",savePath);return 2;}
        std::printf("  saved %zu moves to %s\n",moves.size(),savePath);
        return 0;
    }
    return report("  replay",t,moves,w.numPlayers(),reps,atomic);
}

int synthetic(int reps,bool atomic) {
    int bad=0;
    struct Case {const char* name;int kind;int distance;int sightHeight;int eyeAbove;bool eyeOnly;};
    const Case cases[]={
        {"audit probe: flat, sight 320, eye 100, one-cell moves",0,320,24,100,false},
        {"audit probe: flat, sight 320, eye 100 +-6",0,320,24,100,true},
        {"flat ground unit, sight 320, sight height 24",0,320,24,0,false},
        {"flat ground unit, sight 180, sight height 30",0,180,30,0,false},
        {"rolling terrain, sight 320, sight height 24",1,320,24,0,false},
        {"rough terrain, sight 320, sight height 24",2,320,24,0,false},
        {"rough terrain, flyer sight 400 at 150",2,400,16,150,false},
    };
    for (const auto& c:cases) {
        Terrain t;t.width=t.height=160;
        t.heights.resize(size_t(t.width)*t.height);
        for (int z=0;z<t.height;++z)
            for (int x=0;x<t.width;++x) {
                int h=100;
                if (c.kind==1) h=100+int(30*std::sin(x*0.11)*std::cos(z*0.07));
                if (c.kind==2) h=100+int(60*std::sin(x*0.5)*std::cos(z*0.37))+((x*7+z*13)%23);
                h=std::clamp(h,0,255);
                t.heights[size_t(z)*t.width+x]={uint8_t(h),uint8_t(h)};
            }
        std::vector<Move> moves;
        uint32_t rng=12345;
        auto next=[&]{rng=rng*1664525u+1013904223u;return rng>>8;};
        for (int i=0;i<20000;++i) {
            RetailSightFootprint a;
            a.distance=int16_t(c.distance);a.sightHeight=uint8_t(c.sightHeight);a.active=true;
            a.x=int16_t(30+next()%100);a.z=int16_t(30+next()%100);
            const auto h=t.heights[size_t(a.z)*t.width+a.x];
            a.eyeHeight=h[1]+c.eyeAbove+c.sightHeight;
            RetailSightFootprint b=a;
            if (c.eyeOnly) b.eyeHeight=a.eyeHeight+((next()&1) ? 6 : -6);
            else {
                const int dir=int(next()%8);
                static const int dx[]={1,1,0,-1,-1,-1,0,1},dz[]={0,1,1,1,0,-1,-1,-1};
                b.x=int16_t(a.x+dx[dir]);b.z=int16_t(a.z+dz[dir]);
                const auto hb=t.heights[size_t(b.z)*t.width+b.x];
                b.eyeHeight=hb[1]+c.eyeAbove+c.sightHeight;
            }
            moves.push_back({a,b,0});
        }
        bad|=report(c.name,t,moves,1,reps,atomic);
    }
    return bad;
}

} // namespace

int main(int argc,char** argv) {
    const char* file=nullptr;const char* data=nullptr;const char* savePath=nullptr;const char* loadPath=nullptr;
    int warm=1000,ticks=300,reps=7;
    bool atomic=false,synth=false;
    PathfindingMode mode=PathfindingMode::Retail;
    bool modeSet=false;
    for (int i=1;i<argc;++i) {
        const std::string a=argv[i];
        auto value=[&]{if (i+1>=argc) {std::fprintf(stderr,"%s needs a value\n",a.c_str());std::exit(2);}return argv[++i];};
        if (a=="--data") data=value();
        else if (a=="--warm") warm=std::atoi(value());
        else if (a=="--ticks") ticks=std::atoi(value());
        else if (a=="--reps") reps=std::max(1,std::atoi(value()));
        else if (a=="--atomic") atomic=true;
        else if (a=="--synthetic") synth=true;
        else if (a=="--save") savePath=value();
        else if (a=="--load") loadPath=value();
        else if (a=="--mode") {modeSet=true;mode=std::strcmp(value(),"legion")==0 ? PathfindingMode::Legion : PathfindingMode::Retail;}
        else if (a[0]!='-' && !file) file=argv[i];
        else {std::fprintf(stderr,"usage: viewcount_bench <situation.scn> --data DIR [--mode retail|legion] [--warm N] [--ticks N] [--reps N] [--atomic] [--save FILE] | --load FILE | --synthetic\n");return 2;}
    }
    if (synth) return synthetic(reps,atomic);
    if (loadPath) {
        Terrain t;int players=0;std::vector<Move> moves;
        if (!load(loadPath,t,players,moves)) {std::fprintf(stderr,"viewcount_bench: cannot read %s\n",loadPath);return 2;}
        return report(loadPath,t,moves,players,reps,atomic);
    }
    if (!file) {std::fprintf(stderr,"viewcount_bench: a situation file or --synthetic\n");return 2;}
    if (!modeSet) {
        const char* base=std::strrchr(file,'/');base=base ? base+1 : file;
        if (base[0]=='L') mode=PathfindingMode::Legion;
    }
    try {
        return situation(file,data,mode,warm,ticks,reps,atomic,savePath);
    } catch (const std::exception& e) {
        std::fprintf(stderr,"viewcount_bench: %s\n",e.what());return 2;
    }
}
