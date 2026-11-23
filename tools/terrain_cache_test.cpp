#include "client/mapview.h"
#include "client/gpuvram.h"
#include "hpi/hpi.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
static void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv) try {
    if(argc!=2 && argc!=3)return 2;
    SDL_SetMainReady();
    SDL_SetHint("SDL_SHUTDOWN_DBUS_ON_QUIT","1"); // release SDL's process-global DBus allocations for leak checks
    check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
    auto* window=SDL_CreateWindow("terrain chunks",0,0,640,480,SDL_WINDOW_HIDDEN);
    auto* renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_TARGETTEXTURE);
    if(!renderer)renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
    check(renderer,SDL_GetError());
    const auto baseline=gpuvram::bytes();
    {
        auto vfs=tak::hpi::mountRetailRoot(argv[1]);
        MapView map(renderer,vfs,tak::hpi::findMap(vfs,argc==3 ? argv[2] : "Ulasem Arena"));map.finishChunks();
        auto* target=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,1280,960);
        check(target,SDL_GetError());SDL_SetRenderTarget(renderer,target);
        auto draw=[&] {
            SDL_SetRenderDrawColor(renderer,70,80,90,255);SDL_RenderClear(renderer);
            map.draw(610,450);
            check(SDL_GetRenderTarget(renderer)==target,"render target unchanged");
        };
        auto warm=[&] {
            const auto start=SDL_GetTicks();
            do {
                draw();const auto stats=map.chunkStats();
                if(stats.visible && stats.visible==stats.ready)return;
                check(SDL_GetTicks()-start<60000,"terrain chunks failed to finish");SDL_Delay(1);
            }while(true);
        };
        auto read=[&] {std::vector<unsigned char> pixels(1280*960*4);
            check(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,pixels.data(),1280*4)==0,SDL_GetError());return pixels;};
        for(float scale:{1.f,1.5f,2.f}) {
            SDL_RenderSetScale(renderer,scale,scale);
            SDL_Rect viewport{4,6,620,460},clip{2,3,601,441};
            SDL_RenderSetViewport(renderer,&viewport);SDL_RenderSetClipRect(renderer,&clip);
            map.setZoom(.05f);map.setOffset(300,300);warm();
            const auto first=read();const auto wideUploads=map.chunkStats().uploads;
            for(int frame=0;frame<6;++frame) {map.setOffset(300+frame,300+frame);draw();}
            check(map.chunkStats().uploads==wideUploads,"panning reuses map-space images");
            map.setOffset(300,300);draw();check(read()==first,"returning camera restores image");
            map.setZoom(1);warm();const auto bothUploads=map.chunkStats().uploads;
            map.setZoom(.05f);draw();check(map.chunkStats().uploads==bothUploads,"returning to a cached resolution does not rebuild");
            map.setBilinear(true);draw();check(map.chunkStats().uploads==bothUploads,"filter changes reuse chunk pixels");
            map.setBilinear(false);draw();check(read()==first,"filter reset restores nearest image");
            SDL_Rect actual{};SDL_RenderGetViewport(renderer,&actual);
            check(actual.x==viewport.x && actual.y==viewport.y && actual.w==viewport.w && actual.h==viewport.h,"viewport unchanged");
            SDL_RenderGetClipRect(renderer,&actual);
            check(SDL_RenderIsClipEnabled(renderer) && actual.x==clip.x && actual.w==clip.w && actual.y==clip.y && actual.h==clip.h,"clip unchanged");
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);check(sx==scale && sy==scale,"AA scale unchanged");
            map.invalidateRenderTargets();check(map.chunkStats().bytes==0,"reset frees images");warm();check(read()==first,"reset recreates same pixels");
        }
        // A local brush edit must retain unrelated queued/in-flight snapshots,
        // even before their textures have reached the render thread.
        SDL_RenderSetScale(renderer,1,1);map.setZoom(1);map.setOffset(800,800);
        map.invalidateRenderTargets();draw();
        const auto pending=map.chunkStats().pending;
        check(pending>=4,"fixture queued visible chunks");
        map.tilesEdited(100,100,1,1);
        check(map.chunkStats().pending==pending,"offscreen stamp must retain pending visible chunks");
        map.tilesEdited(25,25,1,1);
        check(map.chunkStats().pending==pending-1,"interior stamp cancels only its pending chunk");
        map.tilesEdited(25,25,1,1);
        check(map.chunkStats().pending==pending-1,"repeated edits coalesce before the next draw");
        warm();check(map.chunkStats().ready==map.chunkStats().visible,"retained and replacement snapshots finish together");
        // Full-detail texels, including both sides of a chunk boundary, match
        // the independent CPU terrain compositor. This catches crop/gutter seams.
        SDL_RenderSetScale(renderer,1,1);SDL_RenderSetViewport(renderer,nullptr);
        SDL_Rect terrainClip{0,0,610,450};SDL_RenderSetClipRect(renderer,&terrainClip);
        map.setZoom(1);map.setOffset(800,800);warm();const auto full=read();
        for(int y:{31,32,223,224,225,440})for(int x:{31,32,223,224,225,600}) {
            const int wx=800+x,wy=800+y;
            std::vector<uint8_t> block(32*32*4);
            map.compositor().renderBlock(map.map(),wx/32,wy/32,block,32,0,0);
            const size_t from=(size_t(wy%32)*32+wx%32)*4,to=(size_t(y)*1280+x)*4;
            for(int c=0;c<4;++c)check(full[to+c]==block[from+c],"full-resolution terrain texel or chunk seam mismatch");
        }
        auto& edited=map.editMap();
        map.invalidateRenderTargets();warm();
        const auto resident=map.chunkStats().images;
        check(resident>=4,"fixture spans four terrain chunks");
        edited.tileCols[size_t(25)*edited.blocksX+25]^=1;
        map.tilesEdited(25,25,1,1);
        check(map.chunkStats().images==resident-1,"interior stamp retains unaffected terrain images");
        warm();const auto partial=read();
        map.invalidateRenderTargets();warm();
        check(read()==partial,"partial terrain rebuild matches full rebuild");
        edited.tileCols[size_t(32)*edited.blocksX+32]^=1;
        map.tilesEdited(32,32,1,1);
        check(map.chunkStats().images==0,"corner edit invalidates neighbouring filtering gutters");
        draw(); // allow a worker snapshot to start, then change the same tile
        edited.tileCols[size_t(32)*edited.blocksX+32]^=3;
        map.tilesEdited(32,32,1,1);warm();const auto raced=read();
        map.invalidateRenderTargets();warm();
        check(read()==raced,"old in-flight tile snapshots cannot overwrite new edits");
        const auto before=read();
        for(auto& col:edited.tileCols)col^=1;
        map.tilesEdited();check(map.chunkStats().images==0,"terrain edits invalidate images");warm();
        check(read()!=before,"edited terrain appears in rebuilt chunks");
        check(map.chunkStats().bytes<=(size_t(256)<<20),"chunk memory budget");
        // Reload while chunk composition may still be reading the old source.
        // The section decoder and chunk worker must both relinquish its images.
        for(int i=0;i<4;++i) {
            map.invalidateRenderTargets();map.setZoom(.1f);draw();
            map.reload(vfs,tak::hpi::findMap(vfs,i%2 ? "Ulasem Arena" : "Cairbray Coast Landing"));
            check(map.chunkStats().bytes==0,"reload frees old chunk images");
        }
        warm();
        // Fallback sections upload on demand (this view never calls finishChunks, which
        // pins them): a frame whose chunks are all still baking is drawn completely from
        // sections the same frame, and once chunks cover the view the duplicate sections
        // are released. Magenta clear = a hole (no underlay is set here).
        {
            MapView fresh(renderer,vfs,tak::hpi::findMap(vfs,argc==3 ? argv[2] : "Ulasem Arena"));
            SDL_RenderSetScale(renderer,1,1);SDL_RenderSetViewport(renderer,nullptr);SDL_RenderSetClipRect(renderer,&terrainClip);
            const auto start=SDL_GetTicks();
            while(!fresh.sectionsDecoded()) {fresh.ensureChunks(610,450);check(SDL_GetTicks()-start<60000,"sections failed to decode");SDL_Delay(1);}
            fresh.ensureChunks(610,450);
            check(fresh.sectionBytes()==0,"no section is uploaded before the fallback needs it");
            auto holes=[&] {
                const auto px=read();size_t n=0;
                for(int y=0;y<450;++y)for(int x=0;x<610;++x) {
                    const auto* p=&px[(size_t(y)*1280+x)*4];n+=p[0]==255 && p[1]==0 && p[2]==255;
                }
                return n;
            };
            // The view's clock (section idle times) is simulated: 16 ms a frame. On wall time
            // the release waits below were 10 s windows, and a loaded machine missed them: the
            // software renderer batches every frame's draws until a readback or a texture
            // destroy, so the first released section ran hundreds of queued frames (6-16 s
            // under load) before the window closed.
            uint64_t clockMs=SDL_GetTicks64();
            auto frame=[&](float x,float y,float zoom=1) {
                clockMs+=16;fresh.setClockMs(clockMs);
                fresh.setZoom(zoom);fresh.setOffset(x,y);
                SDL_SetRenderDrawColor(renderer,255,0,255,255);SDL_RenderClear(renderer);fresh.draw(610,450);
            };
            // The fallback's section uploads share the frame's upload budget
            // (terrainwork::UploadBudget: 6 ms of estimated time for hole fills, priced by
            // the measured upload cost), and what does not fit waits for the next frame,
            // the underlay showing meanwhile. On a loaded machine or an unoptimized build the
            // measured cost can exceed the budget, so "drawn completely in the same frame" is
            // a wall-time property. The budget's estimate (a moving average of the cost per
            // byte, starting from the view's earlier uploads) stays near this frame's upload
            // time plus the bytes at the earlier rate, so the property is asserted when that
            // is two thirds of the budget or less (quiet cores: about 3 ms for the first frame,
            // 2 ms for the jump). Otherwise the frame's holes must be fallback tiles whose
            // section the budget postponed, and later frames at the same view fill them.
            auto drawnFromSections=[&](const MapView::SectionStats& before,float x,float y,const char* what) {
                const auto after=fresh.sectionStats();
                const double bytes=double(after.uploadBytes-before.uploadBytes),ns=double(after.uploadNs-before.uploadNs);
                const double earlier=std::max(0.15,before.uploadBytes ? double(before.uploadNs)/double(before.uploadBytes) : 0.);
                const double est=ns+earlier*bytes;
                const size_t h=holes();
                std::fprintf(stderr,"%s: %llu section uploads, %.1f MiB in %.2f ms (est %.2f ms), %zu hole px\n",what,
                             (unsigned long long)(after.uploads-before.uploads),bytes/1048576.,ns/1e6,est/1e6,h);
                if(!h)return;
                check(est>4e6,what);
                check(fresh.underlayTiles()>0,"holes only where the upload budget postponed a section");
                for(int f=0;f<32 && holes();++f)frame(x,y);
                check(holes()==0,"postponed sections arrive in later frames");
            };
            auto uploadsBefore=fresh.sectionStats();
            frame(2000,2000);
            check(fresh.chunkStats().ready==0,"fixture: the first frame has no chunk images");
            drawnFromSections(uploadsBefore,2000,2000,"chunk-less first frame is drawn completely from sections");
            check(fresh.sectionBytes()>0,"the fallback uploaded its sections");
            const auto fallback=read();
            const auto settle=SDL_GetTicks();
            do {frame(2000,2000);check(SDL_GetTicks()-settle<60000,"chunks failed to finish");SDL_Delay(1);}
            while(fresh.chunkStats().ready!=fresh.chunkStats().visible);
            check(holes()==0,"chunked frame has no holes");
            // SDL's software rasteriser samples geometry slightly differently from
            // copies, so the texel-exact comparison is a GL-only check.
            SDL_RendererInfo info{};SDL_GetRendererInfo(renderer,&info);
            if(info.name && !std::strcmp(info.name,"opengl"))
                check(read()==fallback,"section fallback matches the chunk image texel for texel");
            for(int f=0;f<400 && fresh.sectionBytes();++f)frame(2000,2000);   // 6.4 s: idle past kSectionIdleMs
            check(fresh.sectionBytes()==0,"sections are released once chunks cover the view");
            // A zoom that changes the chunk level: until the new level bakes, the cached
            // images of another level stand in, so no section is needed at all.
            {
                const auto t=SDL_GetTicks();
                do {frame(5000,5000,.5f);check(SDL_GetTicks()-t<60000,"chunks failed to finish");SDL_Delay(1);}
                while(fresh.chunkStats().ready!=fresh.chunkStats().visible);
                for(int f=0;f<400 && fresh.sectionBytes();++f)frame(5000,5000,.5f);
                frame(5000,5000,1);
                check(fresh.chunkStats().ready<fresh.chunkStats().visible,"fixture: the zoom changed the chunk level");
                check(holes()==0,"a level change leaves no hole");
                check(fresh.sectionBytes()==0,"a level change draws the cached level, not sections");
            }
            // A jump to an unbaked spot: the fallback re-uploads in the same frame.
            uploadsBefore=fresh.sectionStats();
            frame(9000,9000);
            drawnFromSections(uploadsBefore,9000,9000,"a jump draws completely from re-uploaded sections");
            // finishChunks (screenshots, the campaign briefing) lifts the limits for ONE
            // draw only: afterwards the sections are budgeted and released as usual.
            fresh.finishChunks();frame(6000,6000);
            check(holes()==0,"the finishChunks frame is complete");
            {
                const auto t=SDL_GetTicks();
                do {frame(6000,6000);check(SDL_GetTicks()-t<60000,"chunks failed to finish");SDL_Delay(1);}
                while(fresh.chunkStats().ready!=fresh.chunkStats().visible);
                for(int f=0;f<400 && fresh.sectionBytes();++f)frame(6000,6000);
                check(fresh.sectionBytes()==0,"finishChunks does not pin sections for the session");
            }
            // Lowering the limit below what is held sheds off-screen chunks next frame.
            for(float x:{2000.f,4000.f,6000.f,8000.f}) {
                const auto t=SDL_GetTicks();
                do {frame(x,2000);check(SDL_GetTicks()-t<60000,"chunks failed to finish");SDL_Delay(1);}
                while(fresh.chunkStats().ready!=fresh.chunkStats().visible);
            }
            const auto held=fresh.chunkStats().bytes;
            const size_t pretend=size_t(600)<<20;   // stand-in for everything else resident
            gpuvram::addBuffer(pretend);gpuvram::setLimit(pretend+(size_t(32)<<20));
            frame(8000,2000);
            check(fresh.chunkStats().bytes<held,"a lowered limit sheds off-screen chunks");
            check(fresh.chunkStats().ready==fresh.chunkStats().visible,"...but keeps every visible one");
            check(holes()==0,"...and leaves no hole");
            gpuvram::setLimit(gpuvram::kDefaultLimit);gpuvram::removeBuffer(pretend);
        }
        // Workstream J: a decoded-section budget far below what the map decodes to
        // (Ulasem Arena ~64 MiB) evicts and re-decodes, yet every finished frame is
        // texel-identical to the unbudgeted one, nothing stays pinned between jobs,
        // and the cache settles inside the budget.
        {
            const std::string name=argc==3 ? argv[2] : "Ulasem Arena";
            MapView reference(renderer,vfs,tak::hpi::findMap(vfs,name));
            MapView budgeted(renderer,vfs,tak::hpi::findMap(vfs,name));
            reference.compositor().setBudget(SIZE_MAX,SIZE_MAX);
            const size_t high=size_t(4)<<20,low=size_t(3)<<20;
            budgeted.compositor().setBudget(high,low);
            reference.setMipBudget(SIZE_MAX,SIZE_MAX,UINT64_MAX);
            const size_t mipHigh=size_t(1)<<20;
            budgeted.setMipBudget(mipHigh,mipHigh/2,200);
            SDL_RenderSetScale(renderer,1,1);SDL_RenderSetViewport(renderer,nullptr);SDL_RenderSetClipRect(renderer,&terrainClip);
            const auto settled=[&](MapView& view,float x,float y,float zoom) {
                view.setZoom(zoom);view.setOffset(x,y);
                const auto start=SDL_GetTicks();
                for(;;) {
                    SDL_SetRenderDrawColor(renderer,255,0,255,255);SDL_RenderClear(renderer);view.draw(610,450);
                    const auto stats=view.chunkStats();
                    if(stats.visible && stats.visible==stats.ready)break;
                    check(SDL_GetTicks()-start<120000,"budgeted terrain chunks failed to finish");SDL_Delay(1);
                }
                return read();
            };
            const float views[][3]={{300,300,.05f},{800,800,1},{5000,5000,.5f},{9000,9000,1},{2000,6000,.25f},{800,800,1}};
            for(const auto& v:views)
                check(settled(budgeted,v[0],v[1],v[2])==settled(reference,v[0],v[1],v[2]),
                      "budgeted sections render the same terrain");
            const auto cpu=budgeted.sectionStats().cpu;
            const auto mips=budgeted.chunkStats();
            check(mips.mipEvictions>0,"fixture: the mip budget evicted images");
            check(mips.mipBytes<=mipHigh && mips.mipPinnedBytes==0,"mips settle inside their budget, unpinned");
            check(reference.chunkStats().mipEvictions==0,"an unbounded mip cache never evicts");
            // Sustained low demand (200 ms and 120 frames without terrain work): the worker
            // drops its mips; a view that needs them again regenerates the same pixels.
            {
                // New work, then fewer than 120 frames: never a trim, however long they take.
                // The view's clock is simulated from here, and held while it settles: on wall
                // time a loaded machine spent 200 ms and 120 settle frames inside one compose
                // job (a running job is not terrain work for the idle rule), requesting a trim
                // before the check began.
                uint64_t clockMs=SDL_GetTicks64();budgeted.setClockMs(clockMs);
                budgeted.invalidateRenderTargets();settled(budgeted,9000,9000,.5f);
                for(int frame=0;frame<100;++frame) {SDL_RenderClear(renderer);budgeted.draw(610,450);}
                check(budgeted.chunkStats().mipTrims==mips.mipTrims,"no idle trim within 120 frames of terrain work");
                const auto t=SDL_GetTicks();
                while(budgeted.chunkStats().mipTrims==mips.mipTrims || budgeted.chunkStats().mipBytes) {
                    clockMs+=16;budgeted.setClockMs(clockMs);   // 16 ms a frame: 120 frames pass 200 ms
                    SDL_RenderClear(renderer);budgeted.draw(610,450);SDL_Delay(2);
                    check(SDL_GetTicks()-t<30000,"idle mip trim did not happen");
                }
                check(settled(budgeted,2000,6000,.25f)==settled(reference,2000,6000,.25f),"regenerated mips render the same terrain");
                budgeted.invalidateRenderTargets();reference.invalidateRenderTargets();
                check(settled(budgeted,300,300,.05f)==settled(reference,300,300,.05f),"regenerated coarse mips render the same terrain");
                check(budgeted.chunkStats().mipRegens>0,"fixture: mips were regenerated after the trim");
            }
            check(cpu.pinnedBytes==0,"no section stays pinned after its jobs");
            check(cpu.usedBytes<=high,"decoded sections settle inside the budget");
            check(reference.sectionStats().cpu.evictions==0,"an unbounded compositor never evicts");
            std::fprintf(stderr,"budgeted sections: decodes=%llu redecodes=%llu evictions=%llu used=%zu peak=%zu\n",
                (unsigned long long)cpu.decodes,(unsigned long long)cpu.redecodes,
                (unsigned long long)cpu.evictions,cpu.usedBytes,cpu.peakBytes);
            const auto end=budgeted.chunkStats();
            std::fprintf(stderr,"budgeted mips: evictions=%llu regens=%llu trims=%llu peak=%zu\n",
                (unsigned long long)end.mipEvictions,(unsigned long long)end.mipRegens,
                (unsigned long long)end.mipTrims,end.mipPeakBytes);
        }
        SDL_SetRenderTarget(renderer,nullptr);SDL_DestroyTexture(target);
    }
    check(gpuvram::bytes()==baseline,"terrain teardown frees textures");
    SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    std::puts("PASS: terrain chunk pan/zoom reuse, filtering, AA, edits, reset and cleanup");return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
