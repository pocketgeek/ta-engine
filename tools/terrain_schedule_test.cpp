// Workstream K in a real MapView / TerrainChunks (software renderer, game data): the
// camera-driven work order, cancellation with hysteresis, stand-ins under memory
// pressure, nearby work, edits and resets mid-queue, and the same finished pixels as
// the pre-K (legacy) policy. Map: a generated 512 x 512-block map (16 x 16 chunks),
// so the test needs no large retail map.
#include "client/mapview.h"
#include "client/gpuvram.h"
#include "hpi/hpi.h"
#include "tnt/mapgen.h"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using tak::terrainwork::Params;
// The policy without the calm gate (scenarios that test hysteresis, re-ranking or nearby
// work under deliberately fast motion; the gate has its own scenario).
static Params ungated(bool nearby=false) {Params p;p.calmGate=false;p.nearby=nearby;return p;}
static void check(bool value,const std::string& message) {if(!value)throw std::runtime_error(message);}

int main(int argc,char** argv) try {
    if(argc!=2)return 2;
    SDL_SetMainReady();
    SDL_SetHint("SDL_SHUTDOWN_DBUS_ON_QUIT","1");
    check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
    auto* window=SDL_CreateWindow("terrain schedule",0,0,640,480,SDL_WINDOW_HIDDEN);
    auto* renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE|SDL_RENDERER_TARGETTEXTURE);
    check(renderer,SDL_GetError());
    const auto baseline=gpuvram::bytes();
    {
        auto vfs=tak::hpi::mountRetailRoot(argv[1]);
        tak::mapgen::Params gp;gp.seed=7;gp.widthCells=gp.heightCells=1024;
        gp.plateauDensity=gp.mountainDensity=gp.townDensity=0;   // the v9 terrain these gates were set on
        const std::string mapId=tak::mapgen::encodeMapId(gp);
        constexpr int W=1200,H=900;
        auto* target=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,1280,960);
        check(target,SDL_GetError());SDL_SetRenderTarget(renderer,target);
        // The policy's clock (calm gate, storm window, level grace) is a simulated one:
        // every frame is 16 ms later, whatever it really took. On wall time an unoptimized
        // build or a loaded machine stretched frames past what the gate calls fast
        // (900 px a frame in scenario 8 is slow at 0.9 s a frame), and the gate scenarios
        // failed. Worker progress stays real (the settle loops wait for it).
        uint64_t clockMs=1000;
        const auto frame=[&](MapView& v,float x,float y,float zoom) {
            clockMs+=16;v.setClockMs(clockMs);
            v.setZoom(zoom);v.setOffset(x,y);
            SDL_SetRenderDrawColor(renderer,255,0,255,255);SDL_RenderClear(renderer);v.draw(W,H);
        };
        const auto read=[&] {
            std::vector<unsigned char> pixels(size_t(W)*H*4);const SDL_Rect r{0,0,W,H};
            check(SDL_RenderReadPixels(renderer,&r,SDL_PIXELFORMAT_RGBA32,pixels.data(),W*4)==0,SDL_GetError());
            return pixels;
        };
        const auto settle=[&](MapView& v,float x,float y,float zoom) {
            const auto start=SDL_GetTicks();
            for(;;) {
                frame(v,x,y,zoom);const auto s=v.chunkStats();
                if(s.visible && s.ready==s.visible)break;
                check(SDL_GetTicks()-start<120000,"terrain failed to settle");SDL_Delay(1);
            }
            return read();
        };
        // A view whose centre is the middle of chunk cell (5,5): zoom 0.6 shows 2000 x 1500
        // world px, cells 4..6 x 4..6 at level 0.
        const float cx=5.5f*1024-1000,cy=5.5f*1024-750;
        const auto cellReady=[](const MapView& v,int x,int y) {return v.chunkCovers(x*32+16,y*32+16);};

        // 1. Priority: after a jump to an unbaked view the centre cell is ready first
        //    (legacy: the top-left cell, row order).
        {
            MapView k(renderer,vfs,mapId),legacy(renderer,vfs,mapId);
            k.setScheduleParams(Params{});legacy.setScheduleParams(Params::legacy());
            check(k.map().blocksX==512 && k.map().blocksY==512,"fixture: a 512-block map");
            for(auto* v:{&k,&legacy}) {
                const auto start=SDL_GetTicks();
                do {frame(*v,cx,cy,.6f);check(SDL_GetTicks()-start<60000,"no chunk finished");SDL_Delay(1);}
                while(v->chunkStats().ready==0);
            }
            check(k.chunkStats().visible==9,"fixture: a 3 x 3-cell view");
            check(cellReady(k,5,5),"K: the centre cell is ready first");
            check(cellReady(legacy,4,4) && !cellReady(legacy,5,5),"legacy: row order (fixture discriminates)");
        }
        // 1b. Re-ranking queued work: view A's jobs are queued, then the camera moves three
        //     cells right to view B. A's remaining jobs are kept (keep band) but queued
        //     before B's; the worker must take B's visible cells first, so B's first image
        //     arrives after at most the job running at the move and B's own first job
        //     (a FIFO worker would first finish A's kept jobs).
        {
            MapView k(renderer,vfs,mapId);k.setScheduleParams(ungated());
            frame(k,cx-1024,cy,.6f);                 // A: cells 3..5
            const auto before=k.chunkStats();
            frame(k,cx+2048,cy,.6f);                 // B: cells 6..8
            const auto moved=k.chunkStats();
            check(before.jobsQueued==9 && moved.jobsQueued-before.jobsQueued-(moved.jobsNearby-before.jobsNearby)==9,
                  "fixture: both views queued 9 visible jobs");
            check(moved.pending>9,"fixture: A's kept jobs are still pending");
            const auto start=SDL_GetTicks();
            size_t completedAtFirst=0;
            for(;;) {
                frame(k,cx+2048,cy,.6f);const auto s=k.chunkStats();
                if(s.ready) {completedAtFirst=s.jobsCompleted;break;}
                check(SDL_GetTicks()-start<60000,"B never got an image");SDL_Delay(1);
            }
            check(completedAtFirst<=moved.jobsCompleted+3,"the worker takes the visible cells before kept work");
        }
        // 2. Rapid pan, then settle: K cancels the work the camera left behind (the queue
        //    holds only the view and its keep band), and finishes the same pixels as legacy.
        {
            MapView k(renderer,vfs,mapId),legacy(renderer,vfs,mapId);
            k.setScheduleParams(Params{});legacy.setScheduleParams(Params::legacy());
            for(int f=0;f<60;++f) {
                const float x=500+float(f)*220;
                frame(k,x,4000,.6f);
                check(k.chunkStats().pending<=64,"K: pending bounded");
                frame(legacy,x,4000,.6f);
            }
            const auto s=k.chunkStats();
            check(s.jobsObsolete>0,"K: work left behind is cancelled");
            check(s.visible==9 && s.pending<=size_t(7*7),"K: only the view and its keep band stay queued");
            check(settle(k,500+59*220,4000,.6f)==settle(legacy,500+59*220,4000,.6f),"same finished terrain as legacy after a pan");
        }
        // 3. Reversal: wobbling inside the keep band cancels and aborts nothing.
        {
            MapView k(renderer,vfs,mapId);k.setScheduleParams(ungated());
            settle(k,4600,4000,1);                   // cells 4..5
            const auto before=k.chunkStats();
            for(int f=0;f<100;++f)frame(k,(f/2)%2 ? 5200.f : 4000.f,4000,1);   // cells 5..6 / 3..5
            const auto after=k.chunkStats();
            check(after.jobsObsolete==before.jobsObsolete && after.jobsAborted==before.jobsAborted,"a reversal inside the keep band cancels nothing");
            check(after.jobsQueued>before.jobsQueued,"fixture: the wobble needed new work");
        }
        // 4. Zoom oscillating across a level boundary (0.45 = level 1, 0.55 = level 0): within
        //    the level grace nothing is cancelled; once both levels are cached no frame has a
        //    hole; after resting past the grace the view settles.
        {
            MapView k(renderer,vfs,mapId);k.setScheduleParams(Params{});
            const auto before=k.chunkStats();
            for(int f=0;f<30;++f)frame(k,9000,3000,(f/3)%2 ? .45f : .55f);
            check(k.chunkStats().jobsObsolete==before.jobsObsolete,"oscillation within the grace cancels nothing");
            settle(k,9000,3000,.45f);settle(k,9000,3000,.55f);
            for(int f=0;f<40;++f) {
                frame(k,9000,3000,f%2 ? .45f : .55f);
                check(k.chunkStats().holes==0,"with both levels cached a level flip shows no hole");
            }
            settle(k,9000,3000,.55f);
        }
        // 5. Jump storm: right after each jump only the new view's work is pending.
        {
            MapView k(renderer,vfs,mapId);k.setScheduleParams(Params{});
            const float at[4][2]={{1000,1000},{12000,1000},{12000,12000},{1000,12000}};
            for(int f=0;f<40;++f) {
                frame(k,at[f%4][0],at[f%4][1],.6f);
                check(k.chunkStats().pending<=k.chunkStats().visible,"after a jump only the new view is pending");
            }
            check(k.chunkStats().jobsObsolete>0,"the storm cancelled stale work");
            settle(k,at[3][0],at[3][1],.6f);
        }
        // 6. Memory pressure: the GPU limit leaves room for the new level plus its stand-ins
        //    only if an unrelated, more recently used view's images go first. K keeps the
        //    stand-ins (no hole while the new level arrives); legacy LRU evicts them (holes).
        {
            const auto run=[&](const Params& p) {
                MapView v(renderer,vfs,mapId);v.setScheduleParams(p);
                settle(v,cx,cy,.45f);            // V at level 1: the future stand-ins
                settle(v,12000,12000,.55f);      // W at level 0: newer, unrelated
                const size_t pretend=size_t(600)<<20;
                gpuvram::addBuffer(pretend);gpuvram::setLimit(gpuvram::bytes()+(size_t(2)<<20));
                size_t holes=0;
                const auto start=SDL_GetTicks();
                for(;;) {
                    frame(v,cx,cy,.55f);const auto s=v.chunkStats();
                    holes+=s.holes;
                    if(s.visible && s.ready==s.visible)break;
                    check(SDL_GetTicks()-start<120000,"terrain failed to settle under the limit");SDL_Delay(1);
                }
                gpuvram::setLimit(gpuvram::kDefaultLimit);gpuvram::removeBuffer(pretend);
                return holes;
            };
            check(run(Params{})==0,"K: stand-ins survive memory pressure (no hole)");
            check(run(Params::legacy())>0,"legacy: LRU evicts the stand-ins (fixture discriminates)");
        }
        // 7. Nearby work, an edit and a reset mid-pan: the finished terrain matches legacy's
        //    of the same edited map.
        {
            MapView k(renderer,vfs,mapId),legacy(renderer,vfs,mapId);
            k.setScheduleParams(ungated(true));legacy.setScheduleParams(Params::legacy());
            for(int f=0;f<40;++f)frame(k,2000+float(f)*40,6000,.6f);
            check(k.chunkStats().jobsNearby>0,"fixture: nearby work was queued while moving");
            for(auto* v:{&k,&legacy}) {
                auto& m=v->editMap();
                for(int y=200;y<204;++y)for(int x=60;x<120;++x)m.tileCols[size_t(y)*m.blocksX+x]^=1;
                v->tilesEdited(60,200,60,4);
            }
            frame(k,3600,6000,.6f);k.invalidateRenderTargets();
            for(int f=0;f<10;++f)frame(k,3600+float(f)*40,6000,.6f);
            check(settle(k,4000,6000,.6f)==settle(legacy,4000,6000,.6f),"edited terrain matches after nearby work and a reset");
        }
        // 8. Calm gate: a fast pan queues no new work (the fallback draws the cells); once
        //    the camera rests the view is composed and matches legacy.
        {
            MapView k(renderer,vfs,mapId),legacy(renderer,vfs,mapId);
            k.setScheduleParams(Params{});legacy.setScheduleParams(Params::legacy());
            settle(k,2000,9000,.6f);
            const auto before=k.chunkStats();
            // 900 px a 16 ms frame: fast (>= half a view per second).
            for(int f=1;f<=10;++f)frame(k,2000+float(f)*900,9000,.6f);
            const auto during=k.chunkStats();
            check(during.framesGated-before.framesGated>=9 && during.jobsQueued==before.jobsQueued,"a fast pan queues no new work");
            check(during.holes>0,"fixture: the pan reached unbaked cells");
            check(settle(k,11000,9000,.6f)==settle(legacy,11000,9000,.6f),"after the pan the view is composed, same as legacy");
            check(k.chunkStats().jobsQueued>during.jobsQueued,"work resumed once calm");
        }
        // 8b. The gate's scope: zoomed out (level 1) the same fast pan keeps queueing work --
        //     chunk images are small there and the fallback draws many more tiles. The
        //     debug gateall scope (every level) gates it (fixture discriminates).
        {
            const auto pan=[&](const Params& p) {
                MapView k(renderer,vfs,mapId);k.setScheduleParams(p);
                settle(k,2000,9000,.45f);
                const auto before=k.chunkStats();
                for(int f=1;f<=10;++f)frame(k,2000+float(f)*900,9000,.45f);
                const auto after=k.chunkStats();
                return std::pair{after.framesGated-before.framesGated,after.jobsQueued-before.jobsQueued};
            };
            const auto [gated,queued]=pan(Params{});
            check(gated==0 && queued>0,"level 1: a fast pan is not gated and queues work");
            Params all;all.calmGateMaxLevel=5;
            const auto [gatedAll,queuedAll]=pan(all);
            check(gatedAll>=9 && queuedAll==0,"gateall: the same pan is gated (fixture discriminates)");
        }
        // 8c. Jump storms are gated at every level: zoomed out (level 1) a second jump within
        //     the storm window queues nothing (a storm's views are left before their work
        //     pays off). stormlevel0 (debug) queues them (fixture discriminates).
        {
            const auto storm=[&](const Params& p) {
                MapView k(renderer,vfs,mapId);k.setScheduleParams(p);
                const float at[4][2]={{1000,1000},{12000,1000},{12000,12000},{1000,12000}};
                frame(k,at[0][0],at[0][1],.45f);
                const auto before=k.chunkStats();
                for(int f=1;f<=12;++f)frame(k,at[f%4][0],at[f%4][1],.45f);   // a jump every frame
                const auto after=k.chunkStats();
                return std::pair{after.framesGated-before.framesGated,after.jobsQueued-before.jobsQueued};
            };
            const auto [gated,queued]=storm(Params{});
            check(gated>=11 && queued<=12,"level 1: a jump storm is gated (only the first jump queues: one 4 x 3-cell view)");
            Params level0;level0.stormGateMaxLevel=0;
            const auto [gated0,queued0]=storm(level0);
            check(gated0==0 && queued0>12,"stormlevel0: the storm queues every view (fixture discriminates)");
        }
        // 9. Holes first: in a frame that still has holes after prepare(), completed upgrades
        //    (cells another level's image stands in for) wait until after the fallback has
        //    drawn (uploadDeferred), so this frame still shows their stand-ins. View P at
        //    level 0 = cells 4..6 x 4..5, all with level-1 stand-ins; its six upgrade jobs
        //    complete; then the view moves one cell right, bringing in two cells with no
        //    image of any level (holes, nothing composed for them yet).
        {
            MapView k(renderer,vfs,mapId);k.setScheduleParams(ungated());
            const float px=4106,py=4106;
            settle(k,px,py,.45f);                            // level 1: cells 4..6 x 4..5
            frame(k,px,py,.55f);                             // level 0: six upgrade jobs
            const auto queued=k.chunkStats();
            check(queued.visible==6 && queued.standin==6 && queued.holes==0,"fixture: P is six upgrades over stand-ins");
            const auto start=SDL_GetTicks();
            while(k.chunkStats().jobsCompleted<queued.jobsCompleted+6) {
                check(SDL_GetTicks()-start<60000,"the upgrade jobs never completed");SDL_Delay(1);
            }
            frame(k,px+1024,py,.55f);                        // cells 5..7: 7 x 4..5 are holes
            const auto s=k.chunkStats();
            check(s.visible==6 && s.holes==2,"fixture: the moved view has two holes");
            check(s.standin==4,"holes remain: the completed upgrades wait for uploadDeferred (stand-ins still drawn)");
            const auto deferredStart=SDL_GetTicks();
            for(;;) {   // the deferred upgrades arrive in later frames, holes or not
                frame(k,px+1024,py,.55f);
                if(k.chunkStats().standin<4)break;
                check(SDL_GetTicks()-deferredStart<60000,"the deferred upgrades were never uploaded");SDL_Delay(1);
            }
            settle(k,px+1024,py,.55f);
        }
        SDL_SetRenderTarget(renderer,nullptr);SDL_DestroyTexture(target);
    }
    check(gpuvram::bytes()==baseline,"terrain teardown frees textures");
    SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    std::puts("PASS: terrain work priority, cancellation, hysteresis, stand-ins, nearby work, edits and resets");
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
