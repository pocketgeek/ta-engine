#include "cartographer/thumbnails.h"
#include "hpi/hpi.h"
#include <algorithm>
#include <chrono>

namespace cart {
Thumbnails::Thumbnails(SDL_Renderer* renderer,const tak::hpi::Vfs& assets,size_t limit)
    :renderer_(renderer),assets_(assets),compositor_(std::make_unique<tak::terrain::Compositor>(assets)),limit_(std::max(size_t(1),limit)) {}
Thumbnails::~Thumbnails() {reset();}
void Thumbnails::reset() {
    if(job_.valid()) {job_.wait();job_=std::future<Result>{};}
    for(auto& [path,entry]:cache_)if(entry.texture)SDL_DestroyTexture(entry.texture);
    cache_.clear();bytes_=decoded_=0;compositor_->clear();
}
SDL_Texture* Thumbnails::get(const std::string& path) {
    if(job_.valid() && job_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        auto result=job_.get();Entry entry;entry.used=++clock_;
        const auto& img=result.image;
        if(img.width>0 && img.height>0 && img.rgba.size()==size_t(img.width)*img.height*4) {
            entry.texture=SDL_CreateTexture(renderer_,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,img.width,img.height);
            if(entry.texture) {
                SDL_UpdateTexture(entry.texture,nullptr,img.rgba.data(),img.width*4);
                SDL_SetTextureScaleMode(entry.texture,SDL_ScaleModeLinear);entry.bytes=img.rgba.size();
            }
        }
        bytes_+=entry.bytes;cache_.emplace(std::move(result.path),entry);
        // Keep the most recently used image even if it alone exceeds the budget.
        while(cache_.size()>1 && (cache_.size()>limit_ || bytes_>32*1024*1024)) {
            auto oldest=std::min_element(cache_.begin(),cache_.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
            bytes_-=oldest->second.bytes;if(oldest->second.texture)SDL_DestroyTexture(oldest->second.texture);cache_.erase(oldest);
        }
        if(++decoded_>=8) {compositor_->clear();decoded_=0;}
    }
    if(auto found=cache_.find(path);found!=cache_.end()) {found->second.used=++clock_;return found->second.texture;}
    if(!job_.valid())job_=std::async(std::launch::async,[this,path] {
        Result result;result.path=path;
        try {
            if(path.ends_with(".jpg")) {
                result.image=tak::jpeg::load(assets_.read(path));
                return result;
            }
            auto map=tak::tnt::Map::load(assets_.read(path),path);
            // Section thumbnails are not whole-map renders. Reject corrupt or
            // misplaced oversized prefabs before allocating the raster.
            if(map.blocksX>0 && map.blocksY>0 && map.blocksX<=128 && map.blocksY<=128)
                result.image=compositor_->renderMap(map);
        } catch(const std::exception&) {} // cache missing/corrupt entries, too
        return result;
    });
    return nullptr;
}
}
