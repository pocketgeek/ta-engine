#include "cartographer/editorfont.h"
#include "cartographer/font5x7.h"
#include "cartographer/textwidth.h"
#include <algorithm>
#include <map>
#include <vector>
namespace cart {
namespace {
#include "cartographer/fontdata.inc"
#include "cartographer/fontwide.inc"
SDL_Renderer* owner=nullptr;
std::map<size_t,SDL_Texture*> pages;
SDL_Texture* pageFor(size_t page) {
    if(auto found=pages.find(page);found!=pages.end())return found->second;
    constexpr int width=256,height=512;
    std::vector<uint8_t> rgba(width*height*4,255);
    for(size_t i=3;i<rgba.size();i+=4)rgba[i]=0;
    for(size_t cell=0;cell<256 && page*256+cell<std::size(fontGlyphs);++cell) {
        const auto& glyph=fontGlyphs[page*256+cell];size_t offset=glyph.offset;int pixel=0;
        while(pixel<512) {
            const int count=fontAlpha[offset++];const uint8_t alpha=fontAlpha[offset++];
            for(int n=0;n<count;++n,++pixel) {
                const int x=int(cell%16)*16+pixel%16,y=int(cell/16)*32+pixel/16;
                rgba[(size_t(y)*width+x)*4+3]=alpha;
            }
        }
    }
    auto* texture=SDL_CreateTexture(owner,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,width,height);
    if(texture) {SDL_UpdateTexture(texture,nullptr,rgba.data(),width*4);SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(texture,SDL_ScaleModeLinear);}
    pages[page]=texture;return texture;
}
// Wide glyphs: 24x24 cells, 2 bits per pixel, on 26-pixel strides so linear filtering never blends neighbours.
constexpr int wideStride=26,widePage=16*wideStride;
SDL_Texture* widePageFor(size_t page) {
    const size_t key=~page;   // shares the page cache with the narrow atlases
    if(auto found=pages.find(key);found!=pages.end())return found->second;
    std::vector<uint8_t> rgba(size_t(widePage)*widePage*4,255);
    for(size_t i=3;i<rgba.size();i+=4)rgba[i]=0;
    for(size_t cell=0;cell<256 && page*256+cell<std::size(fontWideCodes);++cell) {
        const uint8_t* source=&fontWideAlpha[(page*256+cell)*144];
        for(int pixel=0;pixel<576;++pixel) {
            const int level=(source[pixel>>2]>>(6-2*(pixel&3)))&3;
            const int x=int(cell%16)*wideStride+1+pixel%24,y=int(cell/16)*wideStride+1+pixel/24;
            rgba[(size_t(y)*widePage+x)*4+3]=uint8_t(level*85);
        }
    }
    auto* texture=SDL_CreateTexture(owner,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,widePage,widePage);
    if(texture) {SDL_UpdateTexture(texture,nullptr,rgba.data(),widePage*4);SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(texture,SDL_ScaleModeLinear);}
    pages[key]=texture;return texture;
}
// A code point the font does not carry: an outlined box as wide as the glyph would have been.
void drawMissing(SDL_Renderer* renderer,float x,float y,float scale,int cells,Uint8 r,Uint8 g,Uint8 b) {
    const float left=x+.5f*scale,right=x+(cells*6-.5f)*scale,top=y,bottom=y+7.5f*scale,t=scale;
    SDL_SetRenderDrawColor(renderer,r,g,b,255);
    const SDL_FRect edges[]={{left,top,right-left,t},{left,bottom-t,right-left,t},{left,top,t,bottom-top},{right-t,top,t,bottom-top}};
    SDL_RenderFillRectsF(renderer,edges,4);
}
}
EditorFont::EditorFont(SDL_Renderer* renderer):renderer_(renderer) {owner=renderer;}
EditorFont::~EditorFont() {if(owner==renderer_) {for(auto [page,t]:pages)if(t)SDL_DestroyTexture(t);pages.clear();owner=nullptr;}}
bool drawEditorText(SDL_Renderer* renderer,const std::string& text,int x,int y,int scale,Uint8 r,Uint8 g,Uint8 b) {
    if(renderer!=owner)return false;
    for(size_t p=0;p<text.size();) {
        const auto code=nextGlyph(text,p);
        const int cells=glyphCells(code);
        if(cells==2) {
            const auto* end=std::end(fontWideCodes);const auto* glyph=std::lower_bound(std::begin(fontWideCodes),end,code);
            if(glyph!=end && *glyph==code) {
                const size_t index=size_t(glyph-std::begin(fontWideCodes));
                if(auto* texture=widePageFor(index/256)) {
                    SDL_SetTextureColorMod(texture,r,g,b);
                    SDL_Rect source{int(index%16)*wideStride+1,int(index%256/16)*wideStride+1,24,24};
                    SDL_FRect target{float(x),float(y)-3.f*scale,12.f*scale,12.f*scale};
                    SDL_RenderCopyF(renderer,texture,&source,&target);
                }
            } else drawMissing(renderer,float(x),float(y),float(scale),2,r,g,b);
        } else {
            const auto glyph=std::lower_bound(std::begin(fontGlyphs),std::end(fontGlyphs),code,[](auto glyph,uint32_t value){return glyph.code<value;});
            if(glyph==std::end(fontGlyphs) || glyph->code!=code)drawMissing(renderer,float(x),float(y),float(scale),1,r,g,b);
            else {
                const size_t index=size_t(glyph-std::begin(fontGlyphs));
                if(auto* texture=pageFor(index/256)) {
                    SDL_SetTextureColorMod(texture,r,g,b);
                    SDL_Rect source{int(index%16)*16,int(index%256/16)*32,16,32};
                    SDL_FRect target{float(x-scale),float(y)-4.5f*scale,8.f*scale,16.f*scale};
                    SDL_RenderCopyF(renderer,texture,&source,&target);
                }
            }
        }
        x+=cells*6*scale;
    }
    return true;
}
}
