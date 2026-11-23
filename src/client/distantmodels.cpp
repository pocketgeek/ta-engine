#include "client/distantmodels.h"
#include "client/gpuvram.h"
#include "client/rendertelemetry.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace tak {
namespace {
// SDL target changes reset viewport/scale/clip. Restore all of them, including
// when an allocation or submission fails partway through a frame.
struct RenderState {
    SDL_Renderer* r;
    SDL_Texture* target;
    SDL_Rect viewport,clip;
    SDL_bool clipped;
    float sx,sy;
    SDL_BlendMode blend;
    Uint8 red,green,blue,alpha;
    explicit RenderState(SDL_Renderer* renderer):r(renderer),target(SDL_GetRenderTarget(r)) {
        SDL_RenderGetViewport(r,&viewport);clipped=SDL_RenderIsClipEnabled(r);
        SDL_RenderGetClipRect(r,&clip);SDL_RenderGetScale(r,&sx,&sy);
        SDL_GetRenderDrawBlendMode(r,&blend);SDL_GetRenderDrawColor(r,&red,&green,&blue,&alpha);
    }
    ~RenderState() {
        SDL_SetRenderTarget(r,target);SDL_RenderSetScale(r,sx,sy);
        SDL_RenderSetViewport(r,&viewport);SDL_RenderSetClipRect(r,clipped ? &clip : nullptr);
        SDL_SetRenderDrawBlendMode(r,blend);SDL_SetRenderDrawColor(r,red,green,blue,alpha);
    }
};
// Transparent target rendering stores premultiplied RGB. Composite with ONE
// rather than SRC_ALPHA to avoid dark outlines and double-multiplied cutouts.
SDL_BlendMode compositeBlend() {
    return SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,SDL_BLENDOPERATION_ADD);
}
}
SDL_BlendMode DistantModels::pageBlendMode() { return compositeBlend(); }
void DistantModels::clear() {
    for(auto* page:pages_)if(page)gpuvram::destroy(page);
    pages_.clear();pageLive_.clear();pageUsed_.clear();entries_.clear();bounds_.clear();free_.clear();
    bakes_.clear();shown_.clear();unsupported_=false;frame_=0;formerPages_=0;
    used=refreshed=bakeVertices=bakeDraws=targetSwitches=0;
    hits=held=missNew=missScale=missContent=rejectBudget=rejectShape=rejectPages=0;
    boundsReused=lateBodies=pageReleases=migrated=0;
}
size_t DistantModels::livePages() const {
    return size_t(std::count_if(pages_.begin(),pages_.end(),[](const SDL_Texture* p){return p!=nullptr;}));
}
size_t DistantModels::metadataBytes() const {
    // Node-based maps: one allocation per element (value + key + next pointer +
    // cached hash) plus the bucket array.
    const size_t node=2*sizeof(void*)+sizeof(uint64_t);
    return entries_.size()*(sizeof(Entry)+node)+entries_.bucket_count()*sizeof(void*)+
           bounds_.size()*(sizeof(Bounds)+node)+bounds_.bucket_count()*sizeof(void*)+
           pages_.capacity()*sizeof(pages_[0])+pageLive_.capacity()*sizeof(pageLive_[0])+
           pageUsed_.capacity()*sizeof(pageUsed_[0])+free_.capacity()*sizeof(free_[0])+
           scratch_.capacity()*sizeof(scratch_[0])+bakes_.capacity()*sizeof(bakes_[0])+
           shown_.capacity()*sizeof(shown_[0]);
}
void DistantModels::markTile(int page,int local,bool on) {
    auto& word=pageUsed_[size_t(page)][size_t(local>>6)];
    const uint64_t bit=uint64_t(1)<<(local&63);
    if(bool(word&bit)==on)return;
    word^=bit;pageLive_[size_t(page)]+=on ? 1 : -1;
}
// Creates the page texture in slot `at` (a released slot or one past the end).
// Returns `at`, -1 when there is no room or no VRAM, -2 when the renderer cannot
// composite with the premultiplied blend (the cache is then unsupported).
int DistantModels::createPage(SDL_Renderer* r,int at) {
    if(livePages()>=size_t(maxPages) || gpuvram::blocked() || !gpuvram::wouldFit(size_t(pageSize)*pageSize*4))return -1;
    auto* page=gpuvram::create(r,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,pageSize,pageSize);
    if(!page) {gpuvram::noteFail();return -1;}
    // Tests on the software renderer (no custom blend modes): the page's own blend
    // only matters when it is composited, never for what is baked into it.
    if(SDL_SetTextureBlendMode(page,compositeBlend())!=0 &&
       !(allowSoftware && SDL_SetTextureBlendMode(page,SDL_BLENDMODE_BLEND)==0)) {gpuvram::destroy(page);return -2;}
    SDL_SetTextureScaleMode(page,SDL_ScaleModeLinear);
    if(size_t(at)>=pages_.size()) {pages_.resize(size_t(at)+1,nullptr);pageLive_.resize(pages_.size(),0);pageUsed_.resize(pages_.size());}
    pages_[size_t(at)]=page;pageLive_[size_t(at)]=0;pageUsed_[size_t(at)]={};
    return at;
}
void DistantModels::releasePage(int page) {
    gpuvram::destroy(pages_[size_t(page)]);
    pages_[size_t(page)]=nullptr;pageLive_[size_t(page)]=0;pageUsed_[size_t(page)]={};
    ++pageReleases;
}
// A free tile for a new image, or -1 (no room) / -2 (unsupported). The tile
// POSITION is always the one the pre-G code would take (the last freed first);
// only the page may differ, which no pixel depends on.
int DistantModels::takeSlot(SDL_Renderer* r) {
    const auto firstHole=[&] {
        for(size_t p=0;p<pages_.size();++p)if(!pages_[p])return int(p);
        return int(pages_.size());
    };
    if(free_.empty()) {
        // As before G: a page is added only when every tile of every page the
        // former code would hold is in use (then those pages are all live).
        const int page=createPage(r,firstHole());
        if(page<0)return page;
        ++formerPages_;
        const int base=page*tilesPerPage;
        for(int i=tilesPerPage-1;i>=0;--i)free_.push_back(base+i);
    }
    int slot=free_.back();free_.pop_back();
    const int local=slot%tilesPerPage;
    // The position on the lowest live page where it is free: images gather on
    // the low pages, so the high ones empty first when demand falls. (The hinted
    // page may have been released, or a migrated image may hold the position.)
    // There always is one within the pre-G page count (validate()); otherwise a
    // page is added.
    int page=-1;
    for(size_t p=0;p<pages_.size();++p)
        if(pages_[p] && !tileUsed(int(p),local)) {page=int(p);break;}
    if(page<0) {
        page=createPage(r,firstHole());
        if(page<0) {free_.push_back(slot);return page;}
    }
    slot=page*tilesPerPage+local;
    markTile(page,local,true);
    return slot;
}
const char* DistantModels::validate() const {
    if(pageLive_.size()!=pages_.size() || pageUsed_.size()!=pages_.size())return "page tables out of step";
    if(livePages()>size_t(formerPages_))return "more live pages than the former code would hold";
    std::vector<int> live(pages_.size(),0);
    std::vector<int> atLocal(tilesPerPage,0);
    std::vector<std::array<uint64_t,16>> seen(pages_.size());
    for(const auto& [id,e]:entries_) {
        (void)id;
        const int page=e.slot/tilesPerPage,local=e.slot%tilesPerPage;
        if(e.slot<0 || size_t(page)>=pages_.size() || !pages_[size_t(page)])return "image on a released page";
        if(!tileUsed(page,local))return "image tile not marked used";
        auto& word=seen[size_t(page)][size_t(local>>6)];
        if(word>>(local&63)&1u)return "two images share a tile";
        word|=uint64_t(1)<<(local&63);
        ++live[size_t(page)];++atLocal[size_t(local)];
    }
    for(size_t p=0;p<pages_.size();++p) {
        if(live[p]!=pageLive_[p])return "page image count wrong";
        if(seen[p]!=pageUsed_[p])return "occupancy bit without an image";
    }
    for(int slot:free_)++atLocal[size_t(slot%tilesPerPage)];
    for(int local=0;local<tilesPerPage;++local)
        if(atLocal[size_t(local)]!=formerPages_)return "free list does not match the former code's tiles";
    return nullptr;
}
bool DistantModels::holds(uint64_t id,float zoom,float sx,float sy,uint64_t now) const {
    if(unsupported_)return false;
    const auto found=entries_.find(id);
    if(found==entries_.end())return false;
    const Entry& e=found->second;
    return e.valid && e.zoom==zoom && e.sx==sx && e.sy==sy && now-e.painted<kRefreshMs;
}
// Every 60 prepares: expire images unseen for 120, then (G) release pages whose
// last image expired and move the images of a sparse highest page down so it
// can go too.
void DistantModels::sweep(SDL_Renderer* r) {
    for(auto it=entries_.begin();it!=entries_.end();) {
        if(frame_-it->second.seen>120) {
            const int slot=it->second.slot;
            free_.push_back(slot);markTile(slot/tilesPerPage,slot%tilesPerPage,false);
            it=entries_.erase(it);
        } else ++it;
    }
    for(auto it=bounds_.begin();it!=bounds_.end();)
        if(frame_-it->second.seen>120)it=bounds_.erase(it);else ++it;
    // Expire images while zoomed in or offscreen too, instead of keeping the
    // last distant view's pages resident until the session ends.
    if(entries_.empty() && !pages_.empty()) {clear();return;}
    for(size_t p=0;p<pages_.size();++p)
        if(pages_[p] && pageLive_[p]==0)releasePage(int(p));
    // Bounded migration. An image keeps its tile POSITION and changes page only,
    // so its pixels, its texture coordinates and every later bake position are
    // exactly what they would have been; the copy is a 1:1 texel copy with
    // blending off. All images must find a place, or nothing moves.
    int source=-1;
    for(size_t p=pages_.size();p-->0;)if(pages_[p]) {source=int(p);break;}
    if(source<0 || livePages()<2 || pageLive_[size_t(source)]>kMigrateMax)return;
    struct Move {Entry* entry;int to;};
    std::vector<Move> moves;
    for(auto& [id,e]:entries_) {
        (void)id;
        if(e.slot/tilesPerPage!=source)continue;
        const int local=e.slot%tilesPerPage;
        int to=-1;
        for(int p=0;p<source;++p)if(pages_[size_t(p)] && !tileUsed(p,local)) {to=p;break;}
        if(to<0)return;
        moves.push_back({&e,to});
    }
    std::stable_sort(moves.begin(),moves.end(),[](const Move& a,const Move& b){return a.to<b.to;});
    SDL_Texture* from=pages_[size_t(source)];
    {
        RenderState state(r);
        if(SDL_SetTextureBlendMode(from,SDL_BLENDMODE_NONE)!=0)return;
        bool ok=true;
        for(const auto& m:moves) {
            if(SDL_GetRenderTarget(r)!=pages_[size_t(m.to)]) {
                ++targetSwitches;
                if(SDL_SetRenderTarget(r,pages_[size_t(m.to)])!=0) {ok=false;break;}
                SDL_RenderSetScale(r,1,1);SDL_RenderSetViewport(r,nullptr);SDL_RenderSetClipRect(r,nullptr);
            }
            const int local=m.entry->slot%tilesPerPage;
            const SDL_Rect tile{(local%32)*tileSize,(local/32)*tileSize,tileSize,tileSize};
            if(SDL_RenderCopy(r,from,&tile,&tile)!=0) {ok=false;break;}
        }
        if(SDL_SetTextureBlendMode(from,compositeBlend())!=0 && allowSoftware)
            SDL_SetTextureBlendMode(from,SDL_BLENDMODE_BLEND);
        if(SDL_GetRenderTarget(r)!=state.target)++targetSwitches;
        // A failed copy only wrote free tiles (cleared before any bake uses them).
        if(!ok)return;
    }
    for(const auto& m:moves) {
        const int local=m.entry->slot%tilesPerPage;
        markTile(source,local,false);markTile(m.to,local,true);
        m.entry->slot=m.to*tilesPerPage+local;++migrated;
    }
    releasePage(source);
}
void DistantModels::prepare(SDL_Renderer* r,std::span<Item> items,uint64_t now,Materialize materialize,void* context) {
    used=refreshed=bakeVertices=bakeDraws=targetSwitches=0;++frame_;
    hits=held=missNew=missScale=missContent=rejectBudget=rejectShape=rejectPages=0;
    boundsReused=lateBodies=pageReleases=migrated=0;
    for(auto& item:items)item.texture=nullptr;
    if(frame_%60==0 || sweepEveryPrepare)sweep(r);
    if(unsupported_ || std::none_of(items.begin(),items.end(),[](const auto& i){return candidate(i);}))return;
    SDL_RendererInfo info{};
    if(SDL_GetRendererInfo(r,&info)!=0 || (!(info.flags&SDL_RENDERER_ACCELERATED) && !allowSoftware) ||
       !SDL_RenderTargetSupported(r) || (info.max_texture_width && info.max_texture_width<pageSize) ||
       (info.max_texture_height && info.max_texture_height<pageSize))return;
    RenderState state(r);
    if(!(state.sx>0 && state.sy>0) || !std::isfinite(state.sx) || !std::isfinite(state.sy))return;
    // 1. Decide, in painter order, exactly as before G: which images are shown,
    //    which are baked (the bake budget and tile allocation follow this order).
    bakes_.clear();
    shown_.assign(items.size(),nullptr);
    for(size_t index=0;index<items.size();++index) {
        auto& item=items[index];
        if(!candidate(item))continue;
        auto found=entries_.find(item.id);
        Entry* entry=found==entries_.end() ? nullptr : &found->second;
        if(entry)entry->seen=frame_;
        const bool scaleChanged=entry && (entry->zoom!=item.zoom || entry->sx!=state.sx || entry->sy!=state.sy);
        const bool contentChanged=entry && (entry->revision!=item.revision || entry->textureTick!=item.textureTick ||
              entry->source!=reinterpret_cast<uintptr_t>(item.source.data()));
        const bool repaint=!entry || !entry->valid || scaleChanged ||
            (contentChanged && now-entry->painted>=kRefreshMs);
        if(!repaint) {
            if(contentChanged)++held;else ++hits;
            shown_[index]=entry;
            continue;
        }
        if(!entry || !entry->valid)++missNew;else if(scaleChanged)++missScale;else ++missContent;
        // At most 128 model bakes per frame. If a crowd exceeds the budget,
        // retain the full model instead of freezing its pose indefinitely.
        if(refreshed>=kBakeBudget) {++rejectBudget;continue;}
        if(item.bodyDeferred) {
            // A prediction failed: the bake needs this frame's body.
            if(!materialize)continue;
            materialize(context,index);++lateBodies;
        }
        // Bounds and opacity: retained for unchanged content (G), else scanned.
        auto& bounds=bounds_[item.id];
        const uintptr_t source=reinterpret_cast<uintptr_t>(item.source.data());
        if(bounds.seen && bounds.revision==item.revision && bounds.source==source && bounds.count==item.source.size()) {
            ++boundsReused;
        } else {
            float minX=std::numeric_limits<float>::max(),minY=minX,maxX=-minX,maxY=-minX;
            bool opaque=true;
            for(const auto& v:item.source) {
                minX=std::min(minX,v.position.x);minY=std::min(minY,v.position.y);
                maxX=std::max(maxX,v.position.x);maxY=std::max(maxY,v.position.y);
                opaque=opaque && v.color.a==255;
            }
            bounds={item.revision,0,source,item.source.size(),minX,minY,maxX,maxY,opaque};
        }
        bounds.seen=frame_;
        const float minX=bounds.minX,minY=bounds.minY,maxX=bounds.maxX,maxY=bounds.maxY;
        if(!bounds.opaque || !std::isfinite(minX+minY+maxX+maxY) || maxX-minX>28 || maxY-minY>28) {++rejectShape;continue;}
        const int pw=int(std::ceil((maxX-minX)*state.sx))+2;
        const int ph=int(std::ceil((maxY-minY)*state.sy))+2;
        if(pw<=2 || ph<=2 || pw>tileSize-2 || ph>tileSize-2) {++rejectShape;continue;}
        if(!entry) {
            const int slot=takeSlot(r);
            if(slot==-2) {unsupported_=true;break;}
            if(slot<0) {++rejectPages;continue;}
            Entry fresh;fresh.slot=slot;
            entry=&entries_.emplace(item.id,fresh).first->second;
        }
        entry->valid=false;entry->seen=frame_;
        bakes_.push_back({index,entry,minX,minY,pw,ph});
        bakeVertices+=item.source.size();++refreshed;
        shown_[index]=entry;
    }
    // 2. Bake, grouped by destination page (G). Each bake clears and fills only
    //    its own tile, so their order does not change any pixel; compositing
    //    below and in the body pass stays in painter order.
    std::stable_sort(bakes_.begin(),bakes_.end(),[](const Bake& a,const Bake& b) {
        return a.entry->slot/tilesPerPage<b.entry->slot/tilesPerPage;
    });
    for(const auto& bake:bakes_) {
        const auto& item=items[bake.item];
        Entry* entry=bake.entry;
        const int local=entry->slot%tilesPerPage;
        const int tx=(local%32)*tileSize,ty=(local/32)*tileSize;
        auto* target=pages_[size_t(entry->slot/tilesPerPage)];
        if(SDL_GetRenderTarget(r)!=target)++targetSwitches;
        if(SDL_SetRenderTarget(r,target)!=0)continue;
        SDL_RenderSetScale(r,1,1);SDL_RenderSetViewport(r,nullptr);SDL_RenderSetClipRect(r,nullptr);
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);SDL_SetRenderDrawColor(r,0,0,0,0);
        SDL_Rect clear{tx,ty,tileSize,tileSize};
        if(SDL_RenderFillRect(r,&clear)!=0)continue;
        scratch_.assign(item.source.begin(),item.source.end());
        for(auto& v:scratch_) {
            v.position.x=(v.position.x-bake.minX)*state.sx+tx+2;
            v.position.y=(v.position.y-bake.minY)*state.sy+ty+2;
        }
        int offset=0;bool ok=true;
        for(const auto& run:item.runs) {
            if(run.second<0 || size_t(offset)+size_t(run.second)>scratch_.size()) {ok=false;break;}
            int drawn=0;
            if(drawRun_)drawn=drawRun_(drawRunContext_,r,run.first,scratch_.data()+offset,run.second);
            else {
                render::countSdlGeometry(size_t(run.second),size_t(run.second)*sizeof(SDL_Vertex));
                drawn=SDL_RenderGeometry(r,run.first,scratch_.data()+offset,run.second,nullptr,0);
            }
            if(drawn!=0) {ok=false;break;}
            offset+=run.second;++bakeDraws;
        }
        if(!ok)continue;
        entry->x=bake.minX-item.x-1/state.sx;entry->y=bake.minY-item.y-1/state.sy;
        entry->w=float(bake.pw)/state.sx;entry->h=float(bake.ph)/state.sy;
        entry->pixelsW=bake.pw;entry->pixelsH=bake.ph;entry->zoom=item.zoom;
        entry->sx=state.sx;entry->sy=state.sy;entry->painted=now;
        entry->revision=item.revision;entry->textureTick=item.textureTick;
        entry->source=reinterpret_cast<uintptr_t>(item.source.data());entry->valid=true;
    }
    // 3. Quads, in painter order.
    for(size_t index=0;index<items.size();++index) {
        Entry* entry=shown_[index];
        if(!entry || !entry->valid)continue;
        auto& item=items[index];
        const int local=entry->slot%tilesPerPage;
        const float u=float((local%32)*tileSize+1)/pageSize;
        const float v=float((local/32)*tileSize+1)/pageSize;
        const float du=float(entry->pixelsW)/pageSize,dv=float(entry->pixelsH)/pageSize;
        const float x=item.x+entry->x,y=item.y+entry->y;
        const SDL_Color white{255,255,255,255};
        const SDL_Vertex q[4]={{{x,y},white,{u,v}},{{x+entry->w,y},white,{u+du,v}},
            {{x+entry->w,y+entry->h},white,{u+du,v+dv}},{{x,y+entry->h},white,{u,v+dv}}};
        constexpr int indices[]={0,1,2,0,2,3};
        for(int i=0;i<6;++i)item.quad[size_t(i)]=q[indices[i]];
        item.texture=pages_[size_t(entry->slot/tilesPerPage)];++used;
    }
    if(SDL_GetRenderTarget(r)!=state.target)++targetSwitches;
}
}
