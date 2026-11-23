#include "client/distantmodels.h"
#include "distant_models_legacy.h"
#include "client/shadowcomposite.h"
#include "client/geometrysubmit.h"
#include <algorithm>
#include <array>
#include "client/gpuvram.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <cstdlib>
#include <cmath>
#include <cstring>
static void check(bool v,const char* message) {if(!v)throw std::runtime_error(message);}

// Workstream G: the cache against its pre-G implementation (distant_models_legacy.h),
// frame by frame over a scripted scene on whatever renderer is available (both
// caches allowed onto SDL's software renderer). Must be identical: which bodies
// are shown, every quad (position and texture coordinates), every tile's pixels,
// every decision counter. Also exercised: deferred bodies (their source replaced
// by garbage, which prepare() must never read), bounds reuse, grouped bakes,
// page release while other pages are live, bounded migration, and the page
// bookkeeping (validate()) every frame.
static void differentialTest(SDL_Renderer* renderer,SDL_Texture* texture) {
    constexpr int units=1700;
    struct Unit {std::vector<SDL_Vertex> verts,stale;std::pair<SDL_Texture*,int> run{};uint64_t revision=1,id=0;uint32_t tick=0;};
    std::vector<Unit> unit(units);
    uint64_t rng=12345;
    const auto next=[&]{rng=rng*6364136223846793005ull+1442695040888963407ull;return uint32_t(rng>>33);};
    const auto build=[&](int k,float ax,float ay) {
        auto& u=unit[size_t(k)];
        // 16 quads = 96 vertices; every 7th body is too large, every 11th translucent.
        const float cell=k%7==0 ? 8.0f : 1.25f+float(k%5)*0.125f;
        const Uint8 alpha=k%11==0 ? 200 : 255;
        const float wobble=float(u.revision%4)*0.3125f;
        u.verts.resize(96);size_t n=0;
        for(int y=0;y<4;++y)for(int x=0;x<4;++x) {
            SDL_Vertex q[4];
            for(int i=0;i<4;++i) {
                const int dx=i==1 || i==2,dy=i>=2;
                q[i]={{ax+cell*float(x+dx)+wobble*float(y),ay+cell*float(y+dy)},{255,255,255,alpha},
                      {float(x+dx)/4,float(y+dy)/4}};
            }
            for(int i:{0,1,2,0,2,3})u.verts[n++]=q[i];
        }
        u.stale.assign(96,SDL_Vertex{{std::nanf(""),std::nanf("")},{1,2,3,4},{0,0}});
        u.run={texture,96};
    };
    for(int k=0;k<units;++k)unit[size_t(k)].id=uint64_t(k)+1;
    tak::DistantModels cache;legacy::DistantModels reference;
    cache.allowSoftware=reference.allowSoftware=true;
    std::vector<tak::DistantModels::Item> a,b;
    std::vector<int> visible;
    bool pagesDiverged=false;
    size_t predicted=0,late=0,boundsReused=0,releases=0,migrated=0,fewerSwitches=0,pixelChecks=0,frames=0,shown=0;
    struct Context {std::vector<tak::DistantModels::Item>* items;std::vector<Unit>* unit;std::vector<int>* visible;size_t* late;} ctx{&a,&unit,&visible,&late};
    const auto materialize=[](void* p,size_t index) {
        auto& c=*static_cast<Context*>(p);
        auto& item=(*c.items)[index];const auto& u=(*c.unit)[size_t((*c.visible)[index])];
        item.source=u.verts;item.bodyDeferred=false;++*c.late;
    };
    std::vector<unsigned char> pa(64*64*4),pb(64*64*4);
    const auto tile=[&](SDL_Texture* page,const SDL_Vertex& corner,std::vector<unsigned char>& out) {
        const SDL_Rect rect{int(std::lround(corner.tex_coord.x*2048))-1,int(std::lround(corner.tex_coord.y*2048))-1,64,64};
        check(SDL_SetRenderTarget(renderer,page)==0,SDL_GetError());
        check(SDL_RenderReadPixels(renderer,&rect,SDL_PIXELFORMAT_RGBA32,out.data(),64*4)==0,SDL_GetError());
        SDL_SetRenderTarget(renderer,nullptr);
    };
    for(int frame=0;frame<900;++frame) {
        // Phases: everyone in view; only the first and last hundred (the rest
        // expire, a sparse second page empties by migration and is released);
        // everyone again (tiles reused in the pre-G order, a page re-added).
        visible.clear();
        for(int k=0;k<units;++k)
            if(frame<300 || frame>=600 || k<100 || k>=units-100)visible.push_back(k);
        if(frame==650)for(int k=0;k<units;k+=13)unit[size_t(k)].id|=uint64_t(1)<<40;   // new identities
        for(int c=0;c<6;++c)++unit[size_t(next()%units)].revision;                      // pose changes
        if(frame%10==0)for(int k=0;k<units;k+=9)++unit[size_t(k)].tick;               // texture frames
        const float zoom=(frame/97)%2 ? 0.45f : 0.5f;
        const float pan=frame>=150 && frame<250 ? float(frame-150)*0.375f : 0.0f;       // sub-pixel pan
        SDL_RenderSetScale(renderer,frame>=450 && frame<470 ? 2.0f : 1.0f,frame>=450 && frame<470 ? 2.0f : 1.0f);
        float sx=1,sy=1;SDL_RenderGetScale(renderer,&sx,&sy);
        a.assign(visible.size(),{});b.assign(visible.size(),{});
        const uint64_t now=uint64_t(frame)*16+uint64_t(frame/3);   // ~60 Hz with jitter
        for(size_t i=0;i<visible.size();++i) {
            const int k=visible[i];auto& u=unit[size_t(k)];
            const float ax=float(k%60)*32.0f-pan,ay=float(k/60)*32.0f;
            build(k,ax,ay);
            auto& item=a[i];
            // GameView's revision changes whenever the vertices do (any stage, the
            // placement included); so does this one (pan steps of 0.375 px).
            item.id=u.id;item.revision=u.revision*4096+uint64_t(pan*8);item.textureTick=u.tick;item.x=ax;item.y=ay;item.zoom=zoom;
            item.eligible=k%17!=0;item.source=u.verts;item.runs={&u.run,1};
            b[i]=item;
        }
        // The early lookup: a held image's body is not generated. Its vertices
        // (shared by both caches, at the address the image was baked from, as in
        // GameView) are poisoned: neither cache may read them, and both must show
        // the image.
        std::vector<char> deferred(visible.size(),0);
        for(size_t i=0;i<visible.size();++i)
            if(tak::DistantModels::candidate(a[i]) && cache.holds(a[i].id,a[i].zoom,sx,sy,now)) {
                auto& u=unit[size_t(visible[i])];
                std::copy(u.stale.begin(),u.stale.end(),u.verts.begin());
                a[i].bodyDeferred=true;deferred[i]=1;++predicted;
            }
        cache.prepare(renderer,a,now,materialize,&ctx);
        reference.prepare(renderer,b,now);
        ++frames;
        if(const char* why=cache.validate())throw std::runtime_error(why);
        const auto counters=[](const auto& c) {
            return std::array<size_t,12>{c.used,c.refreshed,c.hits,c.held,c.missNew,c.missScale,c.missContent,
                                         c.rejectBudget,c.rejectShape,c.rejectPages,c.bakeVertices,c.bakeDraws};
        };
        if(counters(cache)!=counters(reference)) {
            std::printf("frame %d:",frame);
            for(size_t i=0;i<12;++i)std::printf(" %zu/%zu",counters(cache)[i],counters(reference)[i]);
            std::printf("\n");
        }
        check(cache.used==reference.used && cache.refreshed==reference.refreshed && cache.hits==reference.hits &&
              cache.held==reference.held && cache.missNew==reference.missNew && cache.missScale==reference.missScale &&
              cache.missContent==reference.missContent && cache.rejectBudget==reference.rejectBudget &&
              cache.rejectShape==reference.rejectShape && cache.rejectPages==reference.rejectPages &&
              cache.bakeVertices==reference.bakeVertices && cache.bakeDraws==reference.bakeDraws,
              "decisions equal the pre-G cache");
        // Same pages as the pre-G cache until a page is released: grouping by page
        // can then only save target switches. (After a release images may sit on
        // other pages than before, so the counts are no longer comparable.)
        pagesDiverged=pagesDiverged || cache.pageReleases || cache.migrated;
        check(pagesDiverged || cache.targetSwitches<=reference.targetSwitches,"bakes grouped by page never switch more");
        fewerSwitches+=cache.targetSwitches<reference.targetSwitches;
        check(cache.bytes()<=reference.bytes(),"never more image pages than before");
        boundsReused+=cache.boundsReused;releases+=cache.pageReleases;migrated+=cache.migrated;
        for(size_t i=0;i<visible.size();++i) {
            check(bool(a[i].texture)==bool(b[i].texture),"the same bodies are shown");
            check(!deferred[i] || (a[i].texture && b[i].texture),"a predicted image is shown by both caches");
            if(!a[i].texture)continue;
            ++shown;
            if(std::memcmp(a[i].quad.data(),b[i].quad.data(),sizeof a[i].quad)!=0)
                std::printf("frame %d item %zu unit %d: new (%g,%g uv %g,%g) old (%g,%g uv %g,%g)\n",frame,i,visible[i],
                    a[i].quad[0].position.x,a[i].quad[0].position.y,a[i].quad[0].tex_coord.x*2048,a[i].quad[0].tex_coord.y*2048,
                    b[i].quad[0].position.x,b[i].quad[0].position.y,b[i].quad[0].tex_coord.x*2048,b[i].quad[0].tex_coord.y*2048);
            check(std::memcmp(a[i].quad.data(),b[i].quad.data(),sizeof a[i].quad)==0,"quads equal (position and tile)");
        }
        // Tile pixels at checkpoints and right after a migration/release.
        if(frame%75==74 || cache.migrated || cache.pageReleases) {
            for(size_t i=0;i<visible.size();++i) {
                if(!a[i].texture)continue;
                tile(a[i].texture,a[i].quad[0],pa);tile(b[i].texture,b[i].quad[0],pb);
                check(pa==pb,"tile pixels equal the pre-G cache");
                ++pixelChecks;
            }
        }
    }
    SDL_RenderSetScale(renderer,1,1);
    check(late==0,"every early-lookup prediction held (no deferred body was needed)");
    check(predicted>1000 && boundsReused>1000 && releases>0 && migrated>0 && fewerSwitches>0 && pixelChecks>1000,
          "scenario exercised predictions, bounds reuse, page release, migration and grouped bakes");
    // A wrong prediction must still produce the pre-G result: mark bodies deferred
    // that need a bake (new identities), with garbage sources.
    for(size_t i=0;i<a.size();++i) {
        const int k=visible[i];
        build(k,float(k%60)*32.0f,float(k/60)*32.0f);   // undo the last frame's poison
        const auto& u=unit[size_t(k)];
        b[i].x=float(k%60)*32.0f;b[i].y=float(k/60)*32.0f;b[i].source=u.verts;
        a[i]=b[i];a[i].id=b[i].id=u.id^(uint64_t(1)<<50);a[i].bodyDeferred=true;a[i].source=u.stale;
    }
    late=0;
    cache.prepare(renderer,a,100000,materialize,&ctx);reference.prepare(renderer,b,100000);
    check(late==cache.refreshed+cache.rejectShape+cache.rejectPages && cache.refreshed>0 &&
          cache.refreshed==reference.refreshed && cache.used==reference.used,
          "a body needed for a bake is generated through the callback");
    for(size_t i=0;i<a.size();++i)
        if(a[i].texture) {
            check(std::memcmp(a[i].quad.data(),b[i].quad.data(),sizeof a[i].quad)==0,"late quads equal");
            tile(a[i].texture,a[i].quad[0],pa);tile(b[i].texture,b[i].quad[0],pb);
            check(pa==pb,"late tiles equal");
        }
    // Without a callback a deferred body that needs a bake stays live.
    for(auto& item:a){item.id^=uint64_t(1)<<51;item.bodyDeferred=true;}
    cache.prepare(renderer,a,200000);
    check(cache.refreshed==0 && std::none_of(a.begin(),a.end(),[](const auto& i){return i.texture;}),
          "no bake from a deferred body without a callback");
    std::printf("PASS: distant cache equals the pre-G cache over %zu frames (%zu shown, %zu tile checks, "
                "%zu predictions, %zu bounds reused, %zu page releases, %zu migrated, %zu frames with fewer switches)\n",
                frames,shown,pixelChecks,predicted,boundsReused,releases,migrated,fewerSwitches);
    cache.clear();reference.clear();
}
int main() try {
    SDL_SetMainReady();
    check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
    auto* window=SDL_CreateWindow("distant model test",0,0,96,96,SDL_WINDOW_HIDDEN);
    check(window,SDL_GetError());
    auto* renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_TARGETTEXTURE);
    if(!renderer)renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
    check(renderer,SDL_GetError());
    auto* texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,2,2);
    check(texture,SDL_GetError());
    const unsigned char texels[]={255,0,0,128, 0,255,0,255, 0,0,255,0, 255,255,255,255};
    SDL_UpdateTexture(texture,nullptr,texels,8);SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
    std::vector<SDL_Vertex> vertices;
    for(int y=0;y<8;++y)for(int x=0;x<8;++x) {
        SDL_Vertex q[4];
        for(int i=0;i<4;++i) {
            const int dx=i==1 || i==2,dy=i>=2;
            q[i]={{20.f+2*(x+dx),20.f+2*(y+dy)},{255,255,255,255},{float(x+dx)/8,float(y+dy)/8}};
        }
        for(int i:{0,1,2,0,2,3})vertices.push_back(q[i]);
    }
    std::pair<SDL_Texture*,int> run{texture,int(vertices.size())};
    tak::DistantModels cache;
    tak::DistantModels::Item item;item.id=1;item.revision=1;item.x=20;item.y=20;
    item.zoom=.25;item.eligible=true;item.source=vertices;item.runs={&run,1};
    auto read=[&] {std::vector<unsigned char> p(96*96*4);check(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,p.data(),96*4)==0,SDL_GetError());return p;};
    auto background=[&] {SDL_SetRenderDrawColor(renderer,80,90,100,255);SDL_RenderClear(renderer);};
    background();SDL_RenderGeometry(renderer,texture,vertices.data(),int(vertices.size()),nullptr,0);
    const auto reference=read();
    cache.prepare(renderer,{&item,1},100);
    if(!item.texture) {
        check(!cache.used,"unsupported renderer must retain full geometry");
        if(std::getenv("TAK_REQUIRE_DISTANT_GPU"))throw std::runtime_error("accelerated distant path unavailable");
        std::puts("PASS: unsupported renderer keeps full geometry (GPU comparisons skipped)");
    } else {
        background();SDL_RenderGeometry(renderer,item.texture,item.quad.data(),6,nullptr,0);
        auto actual=read();int worst=0;
        for(size_t i=0;i<actual.size();++i)worst=std::max(worst,std::abs(int(actual[i])-int(reference[i])));
        check(worst<=2,"cached cutout/translucent texels must not darken or lose coverage");
        // Camera translation changes placement immediately, without rebaking the pose.
        const float oldX=item.quad[0].position.x;
        item.x+=4;item.revision=2;
        for(auto& v:vertices)v.position.x+=4;
        cache.prepare(renderer,{&item,1},120);
        check(item.texture && cache.refreshed==0 && item.quad[0].position.x==oldX+4,"smooth translation between pose refreshes");
        cache.prepare(renderer,{&item,1},200);
        check(item.texture && cache.refreshed==1,"changed poses refresh after bounded interval");
        item.textureTick=1;
        cache.prepare(renderer,{&item,1},220);
        check(item.texture && cache.refreshed==0,"texture tick bypassed image refresh interval");
        cache.prepare(renderer,{&item,1},280);
        check(item.texture && cache.refreshed==1,"texture tick must refresh even when geometry revision holds");
        auto relocated=vertices;
        item.source=relocated;
        cache.prepare(renderer,{&item,1},380);
        check(item.texture && cache.refreshed==1,"geometry slot changes invalidate equal revision numbers");
        // A new identity (GameView: a new geometry-entry handle -- reused unit id,
        // replacement, recycled entry) never shows the previous one's image, even
        // inside the refresh interval where the same identity would be held.
        item.id=2;item.revision=3;
        cache.prepare(renderer,{&item,1},390);
        check(item.texture && cache.refreshed==1 && cache.missNew==1 && cache.held==0,
              "a new identity bakes its own image instead of holding another's");
        item.id=1;item.revision=4;
        cache.prepare(renderer,{&item,1},395);
        check(item.texture && cache.refreshed==0 && cache.held==1,"the same identity is held inside the interval");
        SDL_RenderSetScale(renderer,2,2);
        cache.prepare(renderer,{&item,1},400);
        float sx=0,sy=0;SDL_RenderGetScale(renderer,&sx,&sy);
        check(item.texture && cache.refreshed==1 && sx==2 && sy==2,"output scale change refreshes and restores renderer state");
        SDL_RenderSetScale(renderer,1,1);
        item.zoom=1;cache.prepare(renderer,{&item,1},400);
        check(!item.texture,"zooming in restores full geometry immediately");
        item.zoom=.25;item.eligible=false;cache.prepare(renderer,{&item,1},500);
        check(!item.texture,"special render paths must bypass sprites");
        std::printf("PASS: GPU sprite transparency, camera, refresh and zoom (max channel error %d)\n",worst);
        item.eligible=true;
        cache.prepare(renderer,{&item,1},520);
        check(cache.bytes()>0 && cache.used==1 && cache.refreshed==1,
              "AA transition fixture must contain a live distant page and work counters");
        cache.clear();
        check(gpuvram::bytes()==0 && cache.used==0 && cache.refreshed==0 &&
              cache.bakeVertices==0 && cache.bakeDraws==0 && cache.targetSwitches==0,
              "disabling distant images releases AA budget and clears work counters");
        cache.prepare(renderer,{&item,1},550);
        check(item.texture && cache.used==1 && cache.refreshed==1,
              "returning from model AA can rebuild native distant images");
        for(int frame=0;frame<180;++frame)cache.prepare(renderer,{},500);
        check(gpuvram::bytes()==0,"unused distant pages expire without another eligible model");
    }
    // Verify the actual shadow-batch quad builder against SDL's former copy
    // path, including texture alpha and independently overlapping silhouettes.
    SDL_SetTextureColorMod(texture,0,0,0);SDL_SetTextureAlphaMod(texture,127);
    const SDL_Rect src{0,0,2,2};
    const SDL_FRect first{20,20,16,16},second{28,24,16,16};
    background();
    SDL_RenderCopyF(renderer,texture,&src,&first);SDL_RenderCopyF(renderer,texture,&src,&second);
    const auto shadows=read();
    std::vector<SDL_Vertex> shadowBatch;
    tak::appendShadowComposite(shadowBatch,src,first,2,2,127);
    tak::appendShadowComposite(shadowBatch,src,second,2,2,127);
    // Degenerate padding exercises the direct-submit threshold without
    // saturating the overlapping shadows to black and hiding blend errors.
    shadowBatch.resize(600);
    background();tak::GeometrySubmit submit;submit.draw(renderer,texture,shadowBatch);
    const auto batched=read();int shadowError=0;
    for(size_t i=0;i<shadows.size();++i)shadowError=std::max(shadowError,std::abs(int(shadows[i])-int(batched[i])));
    check(shadowError<=2,"batched shadows preserve modulation and overlapping coverage");
    std::printf("PASS: shadow composites (max channel error %d)\n",shadowError);
    cache.clear();check(gpuvram::bytes()==0,"sprite atlas teardown releases GPU budget");
    differentialTest(renderer,texture);
    check(gpuvram::bytes()==0,"differential test releases its pages");
    SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
