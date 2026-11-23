#pragma once
#include "client/retailfog.h"
#include <SDL.h>
#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

namespace tak {
// Uploads the local display plane (client/retailfog.h, one byte per 32 px cell)
// into a texture. Overlay: the main view's fog, two texels per cell each way
// (retailfog::writeTexels, retail's 0x498b60 blend), so the texture is
// (2w x 2h). Minimap: one texel per cell. A new frame need not change a pixel;
// the last successful upload is retained and only changed 32-cell blocks are
// sent again -- for the overlay a block also when a cell bordering it changed,
// since its edge texels blend that neighbour in.
class FogUpload {
public:
    enum class Kind { Overlay, Minimap };
    explicit FogUpload(Kind kind=Kind::Overlay) : kind_(kind) {}
    static constexpr int kBlock=32;   // cells
    int scale() const { return kind_==Kind::Overlay ? 2 : 1; }
    bool update(SDL_Texture* texture,std::span<const uint8_t> plane,int w,int h,bool force=false) {
        uploadedBytes_=0;
        if(!texture || w<=0 || h<=0 || plane.size()!=size_t(w)*h)return false;
        const bool full=force || width_!=w || previous_.size()!=plane.size();
        const int columns=(w+kBlock-1)/kBlock,rows=(h+kBlock-1)/kBlock;
        std::vector<uint8_t> dirty(size_t(columns)*rows,full);
        const int margin=kind_==Kind::Overlay ? 1 : 0;
        if(!full)for(int y=0;y<h;++y) {
            const size_t row=size_t(y)*w;
            if(!std::memcmp(previous_.data()+row,plane.data()+row,size_t(w)))continue;
            for(int x=0;x<w;++x) {
                if(previous_[row+x]==plane[row+x])continue;
                for(int by=std::max(0,y-margin)/kBlock;by<=std::min(h-1,y+margin)/kBlock;++by)
                    for(int bx=std::max(0,x-margin)/kBlock;bx<=std::min(w-1,x+margin)/kBlock;++bx)
                        dirty[size_t(by)*columns+bx]=1;
            }
        }
        const int s=scale();
        auto upload=[&](int x0,int z0,int x1,int z1) {
            SDL_Rect rect{x0*s,z0*s,(x1-x0)*s,(z1-z0)*s};
            void* pixels=nullptr;int pitch=0;
            if(SDL_LockTexture(texture,&rect,&pixels,&pitch)!=0)return false;
            auto* out=static_cast<uint8_t*>(pixels);
            if(kind_==Kind::Overlay) retailfog::writeTexels(plane,w,h,x0,z0,x1,z1,out,pitch);
            else for(int z=z0;z<z1;++z) {
                uint8_t* p=out+size_t(z-z0)*pitch;
                for(int x=x0;x<x1;++x,p+=4) {
                    const uint8_t v=plane[size_t(z)*w+x];
                    p[0]=p[1]=p[2]=0;p[3]=minimapAlpha(v);
                }
            }
            SDL_UnlockTexture(texture);uploadedBytes_+=size_t(rect.w)*rect.h*4;
            return true;
        };
        if(full) { if(!upload(0,0,w,h))return false; }
        else for(int y=0;y<rows;++y)for(int x=0;x<columns;++x) {
            if(!dirty[size_t(y)*columns+x])continue;
            int end=x+1;while(end<columns && dirty[size_t(y)*columns+end])++end;
            if(!upload(x*kBlock,y*kBlock,std::min(end*kBlock,w),std::min((y+1)*kBlock,h)))return false;
            x=end-1;
        }
        if(uploadedBytes_) {previous_.assign(plane.begin(),plane.end());++generation_;}
        width_=w;return true;
    }
    // The minimap (0x4f93a0): an unexplored cell is palette 0 (black); an explored
    // one out of view is the map pixel through a darkening remap table, drawn here
    // as half-opaque black; a cell in view is the map as it is.
    static constexpr uint8_t minimapAlpha(uint8_t v) {
        return v==retailfog::kInView ? 0 : v==retailfog::kExplored ? 128 : 255;
    }
    void clear() { previous_.clear();width_=0; }
    uint32_t generation() const { return generation_; }
    size_t uploadedBytes() const { return uploadedBytes_; }
private:
    Kind kind_;
    std::vector<uint8_t> previous_;
    uint32_t generation_=0;
    int width_=0;
    size_t uploadedBytes_=0;
};
}
