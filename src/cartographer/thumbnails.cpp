#include "cartographer/thumbnails.h"
#include "gaf/gaf.h"
#include "hpi/hpi.h"
#include "terrain/minimap.h"
#include "util/virtualpath.h"
#include <algorithm>
#include <chrono>

namespace cart {
std::string unitPortraitPath(const ta::hpi::Vfs& assets,const std::string& unitId) {
    std::string id=unitId;
    std::transform(id.begin(),id.end(),id.begin(),[](unsigned char c){return char(std::tolower(c));});
    const auto pcx="unitpics/"+id+".pcx";
    return assets.has(pcx)?pcx:"anims/buildpic/"+id+".jpg";
}
Thumbnails::Thumbnails(SDL_Renderer* renderer,const ta::hpi::Vfs& assets,size_t limit)
    :renderer_(renderer),assets_(assets),compositor_(std::make_unique<ta::terrain::Compositor>(assets)),limit_(std::max(size_t(1),limit)) {}
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
            const auto ext=ta::vpath::extension(path);
            if(ext==".jpg") {
                result.image=ta::jpeg::load(assets_.read(path));
                return result;
            }
            if(ext==".pcx") {   // TA's build pictures: unitpics/<id>.pcx, 8-bit with its own palette
                auto pcx=ta::gaf::loadPcx(assets_.read(path),path);
                result.image.width=pcx.width;result.image.height=pcx.height;result.image.rgba=std::move(pcx.rgba);
                return result;
            }
            if(ext==".sct") {   // a TA terrain section: its own tiles
                result.image=shrink(compositor_->renderMap(ta::tnt::Map::loadSct(assets_.read(path),path)));
                return result;
            }
            auto map=ta::tnt::Map::load(assets_.read(path),path);
            // A TA map: its own radar picture (the part showing the map), not a render.
            if(map.tileCount>0 && !map.minimap.empty()) {
                const auto fit=ta::terrain::minimapContent(map);
                const auto palette=ta::terrain::loadWorldPalette(assets_,"");
                auto& img=result.image;img.width=fit.width;img.height=fit.height;img.rgba.resize(size_t(fit.width)*fit.height*4);
                for(int y=0;y<fit.height;++y)for(int x=0;x<fit.width;++x) {
                    const auto* c=palette.rgba[map.minimap[size_t(y)*map.minimapW+x]];
                    std::copy(c,c+3,img.rgba.begin()+(size_t(y)*fit.width+x)*4);img.rgba[(size_t(y)*fit.width+x)*4+3]=255;
                }
                return result;
            }
            // Section thumbnails are not whole-map renders. Reject corrupt or
            // misplaced oversized prefabs before allocating the raster.
            if(map.blocksX>0 && map.blocksY>0 && map.blocksX<=128 && map.blocksY<=128)
                result.image=shrink(compositor_->renderMap(map));
        } catch(const std::exception&) {} // cache missing/corrupt entries, too
        return result;
    });
    return nullptr;
}
ta::jpeg::Image Thumbnails::shrink(ta::jpeg::Image image) {
    // Box-filter by a whole factor until the long side fits kMaxSide: TA's big
    // sections (Mars-01 is 48x50 blocks) would be 10 MiB textures otherwise.
    const int factor=(std::max(image.width,image.height)+kMaxSide-1)/kMaxSide;
    if(factor<=1)return image;
    ta::jpeg::Image out;out.width=std::max(1,image.width/factor);out.height=std::max(1,image.height/factor);
    out.rgba.resize(size_t(out.width)*out.height*4);
    for(int y=0;y<out.height;++y)for(int x=0;x<out.width;++x)for(int c=0;c<4;++c) {
        uint32_t sum=0;
        for(int dy=0;dy<factor;++dy)for(int dx=0;dx<factor;++dx)sum+=image.rgba[(size_t(y*factor+dy)*image.width+x*factor+dx)*4+c];
        out.rgba[(size_t(y)*out.width+x)*4+c]=uint8_t(sum/uint32_t(factor*factor));
    }
    return out;
}
}
