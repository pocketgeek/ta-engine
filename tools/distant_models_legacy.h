#pragma once
// The distant-image cache as it was before workstream G (code of 4d305158),
// kept verbatim for the differential test in distant_models_test.cpp: the only
// changes are the class/namespace name and the `allowSoftware` test switch (so the
// comparison runs on SDL's software renderer, which has no custom blend modes:
// pages then composite with plain blending, which no baked pixel depends on).
// Not built into any program.
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>
#include "client/distantmodels.h"
#include "client/gpuvram.h"

namespace legacy {
class DistantModels {
public:
    using Item=tak::DistantModels::Item;
    void prepare(SDL_Renderer*,std::span<Item>,uint64_t now);
    void clear();
    size_t used=0, refreshed=0;
    size_t bakeVertices=0,bakeDraws=0,targetSwitches=0;
    size_t hits=0,held=0,missNew=0,missScale=0,missContent=0;
    size_t rejectBudget=0,rejectShape=0,rejectPages=0;
    size_t bytes() const {return pages_.size()*size_t(pageSize)*pageSize*4;}
    bool allowSoftware=false;
private:
    static constexpr int tileSize=64,pageSize=2048,tilesPerPage=1024,maxPages=4;
    struct Entry {
        int slot=-1;
        uint64_t seen=0,painted=0,revision=0;
        uint32_t textureTick=0;
        uintptr_t source=0;
        float x=0,y=0,w=0,h=0,zoom=1,sx=1,sy=1;
        int pixelsW=0,pixelsH=0;
        bool valid=false;
    };
    std::unordered_map<uint64_t,Entry> entries_;
    std::vector<SDL_Texture*> pages_;
    std::vector<int> free_;
    std::vector<SDL_Vertex> scratch_;
    uint64_t frame_=0;
    bool unsupported_=false;
};
namespace {
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
}
inline void DistantModels::clear() {
    for(auto* page:pages_)gpuvram::destroy(page);
    pages_.clear();entries_.clear();free_.clear();unsupported_=false;frame_=0;
    used=refreshed=bakeVertices=bakeDraws=targetSwitches=0;
    hits=held=missNew=missScale=missContent=rejectBudget=rejectShape=rejectPages=0;
}
inline void DistantModels::prepare(SDL_Renderer* r,std::span<Item> items,uint64_t now) {
    used=refreshed=bakeVertices=bakeDraws=targetSwitches=0;++frame_;
    hits=held=missNew=missScale=missContent=rejectBudget=rejectShape=rejectPages=0;
    for(auto& item:items)item.texture=nullptr;
    // Expire images while zoomed in or offscreen too, instead of keeping the
    // last distant view's pages resident until the session ends.
    if(frame_%60==0) {
        for(auto it=entries_.begin();it!=entries_.end();) {
            if(frame_-it->second.seen>120){free_.push_back(it->second.slot);it=entries_.erase(it);}else ++it;
        }
        if(entries_.empty() && !pages_.empty())clear();
    }
    if(unsupported_ || std::none_of(items.begin(),items.end(),[](const auto& i) {
        return i.eligible && i.zoom<=0.6f && i.source.size()>=96;
    }))return;
    SDL_RendererInfo info{};
    if(SDL_GetRendererInfo(r,&info)!=0 || (!(info.flags&SDL_RENDERER_ACCELERATED) && !allowSoftware) ||
       !SDL_RenderTargetSupported(r) || (info.max_texture_width && info.max_texture_width<pageSize) ||
       (info.max_texture_height && info.max_texture_height<pageSize))return;
    RenderState state(r);
    if(!(state.sx>0 && state.sy>0) || !std::isfinite(state.sx) || !std::isfinite(state.sy))return;
    // Transparent target rendering stores premultiplied RGB. Composite with ONE
    // rather than SRC_ALPHA to avoid dark outlines and double-multiplied cutouts.
    const SDL_BlendMode composite=SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,SDL_BLENDOPERATION_ADD);
    for(auto& item:items) {
        if(!item.eligible || item.zoom>0.6f || item.source.size()<96)continue;
        auto found=entries_.find(item.id);
        Entry* entry=found==entries_.end() ? nullptr : &found->second;
        if(entry)entry->seen=frame_;
        const bool scaleChanged=entry && (entry->zoom!=item.zoom || entry->sx!=state.sx || entry->sy!=state.sy);
        const bool contentChanged=entry && (entry->revision!=item.revision || entry->textureTick!=item.textureTick ||
              entry->source!=reinterpret_cast<uintptr_t>(item.source.data()));
        const bool repaint=!entry || !entry->valid || scaleChanged ||
            (contentChanged && now-entry->painted>=67);
        if(repaint) {
            if(!entry || !entry->valid)++missNew;else if(scaleChanged)++missScale;else ++missContent;
            // At most 128 model bakes per frame. If a crowd exceeds the budget,
            // retain the full model instead of freezing its pose indefinitely.
            if(refreshed>=128) {++rejectBudget;continue;}
            float minX=std::numeric_limits<float>::max(),minY=minX,maxX=-minX,maxY=-minX;
            bool opaque=true;
            for(const auto& v:item.source) {
                minX=std::min(minX,v.position.x);minY=std::min(minY,v.position.y);
                maxX=std::max(maxX,v.position.x);maxY=std::max(maxY,v.position.y);
                opaque=opaque && v.color.a==255;
            }
            if(!opaque || !std::isfinite(minX+minY+maxX+maxY) || maxX-minX>28 || maxY-minY>28) {++rejectShape;continue;}
            const int pw=int(std::ceil((maxX-minX)*state.sx))+2;
            const int ph=int(std::ceil((maxY-minY)*state.sy))+2;
            if(pw<=2 || ph<=2 || pw>tileSize-2 || ph>tileSize-2) {++rejectShape;continue;}
            if(!entry) {
                if(free_.empty()) {
                    if(pages_.size()>=maxPages || gpuvram::blocked() ||
                       !gpuvram::wouldFit(size_t(pageSize)*pageSize*4)) {++rejectPages;continue;}
                    auto* page=gpuvram::create(r,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,pageSize,pageSize);
                    if(!page) {gpuvram::noteFail();++rejectPages;continue;}
                    if(SDL_SetTextureBlendMode(page,composite)!=0 &&
                       !(allowSoftware && SDL_SetTextureBlendMode(page,SDL_BLENDMODE_BLEND)==0)) {
                        gpuvram::destroy(page);unsupported_=true;return;
                    }
                    SDL_SetTextureScaleMode(page,SDL_ScaleModeLinear);
                    const int base=int(pages_.size())*tilesPerPage;
                    pages_.push_back(page);
                    for(int i=tilesPerPage-1;i>=0;--i)free_.push_back(base+i);
                }
                Entry fresh;fresh.slot=free_.back();free_.pop_back();
                entry=&entries_.emplace(item.id,fresh).first->second;
            }
            entry->valid=false;entry->seen=frame_;
            const int local=entry->slot%tilesPerPage;
            const int tx=(local%32)*tileSize,ty=(local/32)*tileSize;
            const auto* target=pages_[size_t(entry->slot/tilesPerPage)];
            if(SDL_GetRenderTarget(r)!=target)++targetSwitches;
            if(SDL_SetRenderTarget(r,pages_[size_t(entry->slot/tilesPerPage)])!=0)continue;
            SDL_RenderSetScale(r,1,1);SDL_RenderSetViewport(r,nullptr);SDL_RenderSetClipRect(r,nullptr);
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);SDL_SetRenderDrawColor(r,0,0,0,0);
            SDL_Rect clear{tx,ty,tileSize,tileSize};
            if(SDL_RenderFillRect(r,&clear)!=0)continue;
            scratch_.assign(item.source.begin(),item.source.end());
            for(auto& v:scratch_) {
                v.position.x=(v.position.x-minX)*state.sx+tx+2;
                v.position.y=(v.position.y-minY)*state.sy+ty+2;
            }
            int offset=0;bool ok=true;
            for(const auto& run:item.runs) {
                if(run.second<0 || size_t(offset)+size_t(run.second)>scratch_.size()) {ok=false;break;}
                
                if(SDL_RenderGeometry(r,run.first,scratch_.data()+offset,run.second,nullptr,0)!=0) {ok=false;break;}
                offset+=run.second;++bakeDraws;
            }
            bakeVertices+=scratch_.size();++refreshed;
            if(!ok)continue;
            entry->x=minX-item.x-1/state.sx;entry->y=minY-item.y-1/state.sy;
            entry->w=float(pw)/state.sx;entry->h=float(ph)/state.sy;
            entry->pixelsW=pw;entry->pixelsH=ph;entry->zoom=item.zoom;
            entry->sx=state.sx;entry->sy=state.sy;entry->painted=now;
            entry->revision=item.revision;entry->textureTick=item.textureTick;
            entry->source=reinterpret_cast<uintptr_t>(item.source.data());entry->valid=true;
        }
        if(!entry || !entry->valid)continue;
        if(!repaint) {if(contentChanged)++held;else ++hits;}
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
