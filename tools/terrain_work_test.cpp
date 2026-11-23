// Workstream K: the camera-driven terrain work policy (client/terrainwork.h), without
// SDL or threads. Tiers, priority order, hysteresis (keep band, level grace), leading
// edge, stand-in eviction ranks and the shared upload budget, plus queue simulations of
// rapid pans, reversals, zoom-boundary oscillation, jumps and a large map.
#include "client/terrainwork.h"
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace tak::terrainwork;
static int checks=0;
static void check(bool value,const std::string& message) {++checks;if(!value)throw std::runtime_error(message);}

namespace {
constexpr int kSpan=1024;
// A view of w x h cells with its top-left cell at (x,y): the centre in world px.
struct View {int level,x,y,w,h;};
void apply(Demand& d,const View& v,int nx,int ny,uint64_t nowMs,uint64_t frame,const Params& p=Params{}) {
    const Rect r{std::max(0,v.x),std::max(0,v.y),std::min(nx-1,v.x+v.w-1),std::min(ny-1,v.y+v.h-1)};
    d.update(v.level,r,(float(v.x)+float(v.w)*.5f)*kSpan,(float(v.y)+float(v.h)*.5f)*kSpan,float(v.w)*kSpan,float(v.h)*kSpan,kSpan,nx,ny,nowMs,frame,p);
}

// A pending-job set driven the way TerrainChunks drives it: every frame each queued job
// is re-classified (obsolete ones cancelled), then every visible cell without an image or
// a job is queued. Nothing completes, so this isolates the queue's response to motion.
struct Queue {
    std::set<std::tuple<int,int,int>> jobs;
    int cancelled=0,queued=0;
    void frame(const Demand& d,const Params& p) {
        for(auto i=jobs.begin();i!=jobs.end();) {
            const auto [l,x,y]=*i;
            if(p.cancel && d.classify(l,x,y,false)==Tier::Obsolete) {i=jobs.erase(i);++cancelled;}
            else ++i;
        }
        const auto& v=d.visible();
        for(int y=v.y0;y<=v.y1;++y)for(int x=v.x0;x<=v.x1;++x)
            if(jobs.size()<p.maxPending && jobs.insert({d.level(),x,y}).second)++queued;
    }
};
}

int main() try {
    // Rect: growth clamps to the grid; empty stays empty.
    {
        const Rect r{2,3,4,5};
        check(r.grown(1,10,10)==Rect{1,2,5,6},"grow by one");
        check(r.grown(5,6,7)==Rect{0,0,5,6},"growth clamps to the grid");
        check(Rect{}.empty() && Rect{}.grown(3,10,10).empty(),"empty rect stays empty");
        check(r.contains(2,3) && r.contains(4,5) && !r.contains(5,5) && !r.contains(1,4),"inclusive bounds");
    }
    // Tiers of every region, at the wanted level and others.
    {
        Demand d;Params p;
        apply(d,{0,10,10,4,3},32,32,0,0,p);   // visible cells 10..13 x 10..12
        check(d.classify(0,11,11,false)==Tier::Hole,"visible without any image = hole");
        check(d.classify(0,11,11,true)==Tier::Upgrade,"visible with a stand-in = upgrade");
        check(d.classify(0,9,11,false)==Tier::Nearby && d.classify(0,14,13,false)==Tier::Nearby,"guard ring = nearby");
        check(d.classify(0,8,11,false)==Tier::Speculative && d.classify(0,15,14,false)==Tier::Speculative,"keep band = speculative");
        check(d.classify(0,7,11,false)==Tier::Obsolete && d.classify(0,11,15,false)==Tier::Obsolete,"beyond the keep band = obsolete");
        check(d.classify(1,11,11,false)==Tier::Obsolete,"another level with no level change = obsolete");
        check(d.previousLevel()==-1 && !d.levelInGrace(),"the first view has no previous level");
    }
    // Level grace: the level a zoom left stays (speculative) until BOTH the ms and the
    // frame windows have passed; two levels away never; outside the keep band never.
    {
        Demand d;Params p;
        apply(d,{1,10,10,4,3},32,32,1000,100,p);
        apply(d,{0,10,10,4,3},32,32,1016,101,p);
        check(d.previousLevel()==1 && d.levelInGrace(),"a level change opens the grace");
        check(d.classify(1,11,11,false)==Tier::Speculative,"the previous level's visible work is kept");
        check(d.classify(1,8,11,false)==Tier::Speculative,"... also inside the keep band");
        check(d.classify(1,6,11,false)==Tier::Obsolete,"... but not beyond it");
        check(d.classify(2,11,11,false)==Tier::Obsolete,"two levels away is obsolete");
        apply(d,{0,10,10,4,3},32,32,1016+p.levelGraceMs,101+5,p);
        check(d.levelInGrace() && d.classify(1,11,11,false)==Tier::Speculative,"ms passed, frames not: still kept");
        apply(d,{0,10,10,4,3},32,32,1016+20,101+p.levelGraceFrames,p);
        check(d.levelInGrace(),"frames passed, ms not: still kept");
        apply(d,{0,10,10,4,3},32,32,1016+p.levelGraceMs,101+p.levelGraceFrames,p);
        check(!d.levelInGrace() && d.classify(1,11,11,false)==Tier::Obsolete,"both passed: obsolete");
    }
    // Priority: tier first, then distance from the view centre; equal distances tie.
    {
        Demand d;Params p;
        apply(d,{0,10,10,5,5},32,32,0,0,p);   // centre = cell 12,12
        const uint32_t centre=d.priority(Tier::Hole,12,12),edge=d.priority(Tier::Hole,10,10);
        check(centre<edge,"the centre cell first");
        check(d.priority(Tier::Hole,10,12)==d.priority(Tier::Hole,14,12),"symmetric cells tie");
        check(edge<d.priority(Tier::Upgrade,12,12),"every hole before any upgrade");
        check(d.priority(Tier::Upgrade,10,10)<d.priority(Tier::Nearby,15,12),"upgrades before nearby work");
        check(d.priority(Tier::Nearby,9,9)<d.priority(Tier::Speculative,12,12),"nearby before speculative");
        check(d.priority(Tier::Hole,31,31)<d.priority(Tier::Upgrade,12,12),"distance never crosses a tier");
    }
    // Motion and the leading edge: only the guard cells the camera moves toward lead; a
    // still camera and a jump lead nowhere.
    {
        Demand d;Params p;
        apply(d,{0,10,10,4,3},32,32,0,0,p);
        check(!d.moving() && !d.leading(14,11),"the first frame has no motion");
        Rect r{10,10,13,12};
        d.update(0,r,12.f*kSpan+3,11.5f*kSpan,4096.f,3072.f,kSpan,32,32,16,1,p);   // 3 px right
        check(d.dirX()==1 && d.dirY()==0,"moving right");
        check(d.leading(14,11) && d.leading(14,9) && d.leading(14,13),"right column (corners included) leads");
        check(!d.leading(9,11) && !d.leading(12,9) && !d.leading(12,11),"left, top and visible cells do not lead");
        check(!d.leading(15,11),"the keep band beyond the guard does not lead");
        d.update(0,r,12.f*kSpan+3.2f,11.5f*kSpan,4096.f,3072.f,kSpan,32,32,32,2,p);   // 0.2 px: still
        check(!d.moving(),"below minSpeed the camera is still");
        d.update(0,Rect{20,20,23,22},22.f*kSpan,21.5f*kSpan,4096.f,3072.f,kSpan,32,32,48,3,p);   // jump
        check(!d.moving(),"a jump is not motion");
        d.update(0,Rect{19,20,22,22},21.f*kSpan,21.5f*kSpan-40,4096.f,3072.f,kSpan,32,32,64,4,p);
        check(d.dirX()==-1 && d.dirY()==-1 && d.leading(18,21) && d.leading(20,19),"diagonal motion leads on two sides");
    }
    // Calm gate: fast motion (in view sizes per second of the owner's clock) and a second
    // jump within the storm window pause new work until calmMs have passed.
    {
        Params p;Demand d;
        const Rect r{10,10,13,12};const float vw=4096,vh=3072,cy0=11.5f*kSpan;
        float x=12.f*kSpan;uint64_t t=0,f=0;
        const auto step=[&](float dx,uint64_t dt) {x+=dx;t+=dt;d.update(0,r,x,cy0,vw,vh,kSpan,32,32,t,++f,p);};
        step(0,16);check(d.calm(),"a fresh view is calm");
        for(int i=0;i<30;++i)step(4096*0.2f*16/1000,16);   // 0.2 views/s
        check(d.calm(),"a slow pan is calm");
        step(4096*1.0f*16/1000,16);                         // 1 view/s
        check(!d.calm() && d.gated(),"a fast pan gates new work");
        for(int i=0;i<15;++i)step(0,16);                    // 240 ms still
        check(!d.calm(),"... until calmMs have passed");
        step(0,16);check(d.calm(),"... then calm again");
        step(500,0);check(d.calm(),"a frame of 0 ms measures no speed ...");
        step(0,1);check(!d.calm(),"... its step counts at the next clock tick");
        for(int i=0;i<16;++i)step(0,16);
        check(d.calm(),"calm again");
        // Jumps: the first is immediate, a second within jumpStormMs gates.
        d.update(0,Rect{20,20,23,22},22.f*kSpan,21.5f*kSpan,vw,vh,kSpan,32,32,t+=16,++f,p);
        check(d.calm(),"a single jump is not gated (the new view's work starts at once)");
        d.update(0,r,12.f*kSpan,cy0,vw,vh,kSpan,32,32,t+=500,++f,p);
        check(!d.calm(),"a second jump within the storm window gates");
        d.update(0,r,12.f*kSpan,cy0,vw,vh,kSpan,32,32,t+=p.calmMs,++f,p);
        check(!d.calm(),"a storm outlasts calmMs: it ends after stormCalmMs without a jump");
        d.update(0,r,12.f*kSpan,cy0,vw,vh,kSpan,32,32,t+=p.stormCalmMs-p.calmMs,++f,p);
        check(d.calm(),"calm again after the storm");
        d.update(0,Rect{20,20,23,22},22.f*kSpan,21.5f*kSpan,vw,vh,kSpan,32,32,t+=p.jumpStormMs,++f,p);
        check(d.calm(),"a jump after the storm window is immediate again");
        check(!Params::legacy().calmGate && !Params{}.nearby,"legacy has no gate; nearby prefetch is opt-in");
        // A zoom (the view size changes) is neither motion nor a jump, however far the
        // centre moves.
        for(int i=0;i<5;++i) {
            const float z=i%2 ? 1.f : 20.f;
            d.update(0,r,12.f*kSpan+z*100,cy0,vw*z,vh*z,kSpan,32,32,t+=16,++f,p);
            check(d.calm() && !d.moving(),"zoom steps do not gate");
        }
    }
    // The gate's scope. Fast motion: level 0 only by default (from level 1 chunk images
    // are small and the fallback draws many more tiles; gating made zoomed-out fast pans
    // 1.4-3.2x slower). Jump storms: every level (a storm's views are left within a
    // second; composing them at level 1 made a 7680x2160 storm 1.7x slower).
    for(int level:{0,1,3}) {
        for(int variant=0;variant<4;++variant) {
            Params p;
            if(variant==1)p.calmGateMaxLevel=5;    // debug gateall: motion at every level
            if(variant==2)p.calmGate=false;
            if(variant==3)p.stormGateMaxLevel=0;   // debug stormlevel0
            Demand d;const Rect r{10,10,13,12};const float vw=4096,vh=3072,cy0=11.5f*kSpan;
            float x=12.f*kSpan;uint64_t t=0,f=0;
            const auto step=[&](float dx) {x+=dx;t+=16;d.update(level,r,x,cy0,vw,vh,kSpan,32,32,t,++f,p);};
            step(0);check(!d.gated(),"a fresh view is not gated");
            step(4096*1.0f*16/1000);                     // 1 view/s
            check(!d.motionCalm() && d.stormCalm(),"a fast pan is motion, not a storm, at any level");
            check(d.gated()==(variant!=2 && (level==0 || variant==1)),
                  "fast motion gates at level 0 only (every level with gateall; never when off)");
            for(int i=0;i<20;++i)step(0);
            check(d.calm() && !d.gated(),"fixture: calm before the storm");
            d.update(level,Rect{20,20,23,22},22.f*kSpan,21.5f*kSpan,vw,vh,kSpan,32,32,t+=16,++f,p);
            check(!d.gated(),"a single jump is not gated at any level");
            d.update(level,r,12.f*kSpan,cy0,vw,vh,kSpan,32,32,t+=300,++f,p);
            check(d.motionCalm() && !d.stormCalm(),"a second jump within the window is a storm, not motion");
            check(d.gated()==(variant!=2 && (level==0 || variant!=3)),
                  "a jump storm gates at every level (level 0 only with stormlevel0; never when off)");
            d.update(level,r,12.f*kSpan,cy0,vw,vh,kSpan,32,32,t+=p.stormCalmMs,++f,p);
            check(!d.gated(),"calm again after the storm");
        }
    }
    check(Params{}.calmGateMaxLevel==0 && Params{}.stormGateMaxLevel>=5,"default gate scope: motion level 0, storms every level");
    // Eviction ranks.
    {
        Params p,legacy=Params::legacy();
        check(evictionRank(true,false,false,p)==0 && evictionRank(true,true,true,p)==0,"visible images are never evicted");
        check(evictionRank(false,false,false,p)==1,"cold images first");
        check(evictionRank(false,true,true,p)==2,"the uploaded cell's own stand-in next");
        check(evictionRank(false,true,false,p)==3,"another cell's stand-in last");
        check(evictionRank(false,true,false,legacy)==1 && evictionRank(false,true,true,legacy)==1,"legacy: plain LRU");
    }
    // Upload budget.
    {
        UploadBudget b;b.beginFrame(false);
        const size_t chunk=size_t(1026)*1026*4,section=size_t(512)*512*4;
        b.nsPerByte[UploadBudget::Chunk]=b.nsPerByte[UploadBudget::Section]=0.1;
        check(b.admit(UploadBudget::Chunk,false,chunk),"a non-urgent chunk on an idle frame");
        b.record(UploadBudget::Chunk,chunk,uint64_t(chunk*0.1));
        check(!b.admit(UploadBudget::Chunk,true,chunk),"the chunk cap: 8 MiB per frame, the first always");
        check(b.admit(UploadBudget::Chunk,true,size_t(256)*256*4),"a small chunk still fits the cap");
        // Urgent sections up to the byte cap; then refused.
        int n=0;while(b.admit(UploadBudget::Section,true,section)) {b.record(UploadBudget::Section,section,uint64_t(section*0.1));++n;}
        check(b.bytes<=b.urgentBytes && b.bytes+section>b.urgentBytes,"urgent uploads stop at the byte cap");
        check(n==int((b.urgentBytes-chunk)/section),"exactly the bytes that fit");
        // A spent frame defers non-urgent work.
        UploadBudget c;c.beginFrame(false);c.nsPerByte[0]=c.nsPerByte[1]=0.1;
        for(int i=0;i<16;++i)c.record(UploadBudget::Section,section,uint64_t(section*0.1));   // 1.68 ms
        check(!c.admit(UploadBudget::Chunk,false,chunk),"a frame that filled holes defers upgrades");
        check(c.admit(UploadBudget::Chunk,true,chunk),"... but not hole fills");
        // The ms estimate binds when uploads are slow.
        UploadBudget s;s.beginFrame(false);s.nsPerByte[1]=2.0;   // 2 ms per MiB
        check(s.admit(UploadBudget::Section,true,section),"the first urgent upload always proceeds");
        s.record(UploadBudget::Section,section,uint64_t(section*2.0));
        int more=0;while(s.admit(UploadBudget::Section,true,section)) {s.record(UploadBudget::Section,section,uint64_t(section*2.0));++more;}
        check(s.estNs<=s.urgentNs && more==1,"slow uploads stop at the time budget (6 ms at 2.1 ms per section: two)");
        // Progress: the first urgent upload proceeds even when it alone exceeds the caps.
        UploadBudget g;g.beginFrame(false);g.nsPerByte[1]=10.0;g.urgentBytes=1024;
        check(g.admit(UploadBudget::Section,true,section),"the first urgent upload always proceeds, however large");
        check(g.admit(UploadBudget::Section,false,section),"... and so does a non-urgent one (slow uploads never starve)");
        g.record(UploadBudget::Section,section,uint64_t(section*10.0));
        check(!g.admit(UploadBudget::Section,true,section) && !g.admit(UploadBudget::Section,false,section),"... but only one");
        // finishChunks frames: sections unlimited; chunks keep their cap.
        UploadBudget f;f.beginFrame(true);f.nsPerByte[1]=10.0;
        for(int i=0;i<100;++i) {check(f.admit(UploadBudget::Section,true,section),"unlimited sections");f.record(UploadBudget::Section,section,1);}
        // Legacy caps: only the chunk cap; sections unlimited here (MapView counts them).
        UploadBudget l;l.coordinate=false;l.beginFrame(false);
        for(int i=0;i<100;++i)l.record(UploadBudget::Section,section,uint64_t(section));
        check(l.admit(UploadBudget::Chunk,false,chunk) && l.admit(UploadBudget::Section,false,section),"legacy: no shared budget");
        l.record(UploadBudget::Chunk,chunk,1);
        check(!l.admit(UploadBudget::Chunk,true,chunk),"legacy: the chunk cap stays");
        // The rate estimate follows measured uploads; tiny ones are ignored.
        UploadBudget r;r.beginFrame(false);
        for(int i=0;i<64;++i)r.record(UploadBudget::Chunk,chunk,uint64_t(double(chunk)*0.05));
        check(std::abs(r.nsPerByte[0]-0.05)<0.002,"the estimate converges to the measured rate");
        const double before=r.nsPerByte[0];
        r.record(UploadBudget::Chunk,64,1000000);
        check(r.nsPerByte[0]==before,"a tiny upload does not move the estimate");
        r.record(UploadBudget::Chunk,chunk,uint64_t(1e12));
        check(r.nsPerByte[0]<=before+50*0.125,"one outlier is clamped");
    }
    // Simulations of the queue under camera motion (32x32 grid, a 5x3-cell view).
    {
        Params p;
        // Reversal: wobbling one cell either way never leaves the keep band -> no cancels.
        {
            Demand d;Queue q;
            for(int f=0;f<200;++f) {apply(d,{0,10+(f/10)%2,10,5,3},32,32,uint64_t(f)*16,uint64_t(f),p);q.frame(d,p);}
            check(q.cancelled==0,"a small reversal cancels nothing");
            check(q.queued==18,"a one-cell wobble queues only the two views' cells");
        }
        // Rapid pan: one cell per frame to the right. Jobs left behind beyond the keep band
        // are cancelled, so the queue holds only the view and its band -- never 64 stale jobs.
        {
            Demand d;Queue q,legacyQ;Demand dl;const Params legacy=Params::legacy();
            for(int f=0;f<25;++f) {
                apply(d,{0,f,10,5,3},32,32,uint64_t(f)*16,uint64_t(f),p);q.frame(d,p);
                apply(dl,{0,f,10,5,3},32,32,uint64_t(f)*16,uint64_t(f),legacy);legacyQ.frame(dl,legacy);
                for(const auto& [l,x,y]:q.jobs)check(d.keep().contains(x,y),"no queued job outside the keep band");
            }
            check(q.jobs.size()<=size_t(9*3),"the pan's queue is bounded by the keep band");
            check(q.cancelled>0,"left-behind jobs were cancelled");
            check(legacyQ.jobs.size()==legacy.maxPending,"legacy: the queue fills with stale jobs");
            // Legacy can no longer queue the cells now in view.
            int missing=0;const auto& v=dl.visible();
            for(int y=v.y0;y<=v.y1;++y)for(int x=v.x0;x<=v.x1;++x)missing+=!legacyQ.jobs.count({0,x,y});
            check(missing>0,"legacy: the visible cells are starved");
            int missingK=0;const auto& vk=d.visible();
            for(int y=vk.y0;y<=vk.y1;++y)for(int x=vk.x0;x<=vk.x1;++x)missingK+=!q.jobs.count({0,x,y});
            check(missingK==0,"K: every visible cell is queued");
        }
        // Zoom-boundary oscillation: the level flips every 5 frames; both levels' jobs stay
        // queued (grace); once zoom rests on one level past the grace, the other's go.
        {
            Demand d;Queue q;
            uint64_t f=0;
            for(;f<60;++f) {apply(d,{int(f/5)%2,10,10,5,3},32,32,f*16,f,p);q.frame(d,p);}
            check(q.cancelled==0,"oscillating across a level boundary cancels nothing");
            check(q.queued==30,"each level's cells are queued once");
            const uint64_t restAt=f;
            for(;f<restAt+p.levelGraceFrames+70;++f) {apply(d,{0,10,10,5,3},32,32,f*16,f,p);q.frame(d,p);}
            check(q.cancelled==15,"after the grace the other level's jobs are cancelled");
            for(const auto& [l,x,y]:q.jobs)check(l==0,"only the resting level remains");
        }
        // Jump: everything queued for the old view is obsolete at once.
        {
            Demand d;Queue q;
            apply(d,{0,2,2,5,3},32,32,0,0,p);q.frame(d,p);
            apply(d,{0,20,20,5,3},32,32,16,1,p);q.frame(d,p);
            check(q.cancelled==15 && q.jobs.size()==15,"a jump cancels the old view's jobs and queues the new one");
            check(!d.moving(),"no leading edge after a jump");
        }
        // Large map, view at the grid edge: regions clamp and stay consistent.
        {
            Demand d;
            apply(d,{0,58,60,8,6},64,64,0,0,p);
            check(d.visible()==Rect{58,60,63,63} && d.guard()==Rect{57,59,63,63} && d.keep()==Rect{56,58,63,63},"edge clamping");
            check(d.classify(0,63,63,false)==Tier::Hole && d.classify(0,55,60,false)==Tier::Obsolete,"edge tiers");
        }
    }
    std::printf("PASS: terrain work policy (%d checks)\n",checks);
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
