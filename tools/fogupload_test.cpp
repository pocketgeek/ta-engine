#include "client/fogupload.h"
#include <cstdio>
#include <stdexcept>
static void check(bool yes,const char* message) {if(!yes)throw std::runtime_error(message);}
namespace rf=tak::retailfog;
int main() try {
    // retail's 0x498b60 on its three cell bytes: in view (0x0f) next to out of view
    // (0x8f) and unexplored (0xf0), nibble by nibble with weights 19/5/5/3 of 32.
    check(rf::blendTexel(0x0f,0x0f,0x0f,0x0f)==0x0f && rf::blendTexel(0xf0,0xf0,0xf0,0xf0)==0xf0 &&
          rf::blendTexel(0x8f,0x8f,0x8f,0x8f)==0x8f,"a uniform neighbourhood keeps its byte");
    check(rf::blendTexel(0x0f,0xf0,0x0f,0x0f)==((5*15)>>5<<4 | (27*15)>>5),"one unexplored side neighbour");
    check(rf::blendTexel(0xf0,0x0f,0x0f,0x0f)==((19*15)>>5<<4 | (13*15)>>5),"own cell weighs 19/32");
    {   // buildPlane's four-lane form against the rule cell by cell, odd sizes,
        // several viewers, Map Revealed and LOS off.
        uint32_t seed=12345;
        auto next=[&]{seed=seed*1103515245u+12345u;return seed>>8;};
        for(int round=0;round<40;++round) {
            const int w=1+int(next()%37),h=1+int(next()%23);const size_t cells=size_t(w)*h;
            std::vector<uint16_t> counts(cells*4),explored(cells);
            for(auto& c:counts)c=uint16_t(next()%4==0 ? next()%70000 : 0);
            for(auto& e:explored)e=uint16_t(next());
            rf::PlaneSource src;src.viewCounts=counts;src.explored=explored;src.width=w;src.height=h;
            src.viewers=next()%16;src.player=int(next()%4);src.mapRevealed=round%7==3;src.losOff=round%5==2;
            std::vector<uint8_t> plane(cells,9);
            check(rf::buildPlane(src,plane),"a new plane reports a change");
            for(size_t i=0;i<cells;++i) {
                uint8_t v=rf::kUnexplored;
                if(src.mapRevealed || (explored[i]>>src.player&1)) {
                    v=rf::kExplored;
                    bool seen=false;for(int p=0;p<4;++p)seen=seen || ((src.viewers>>p&1) && counts[size_t(p)*cells+i]);
                    if(src.losOff || seen)v=rf::kInView;
                }
                check(plane[i]==v,"buildPlane matches the rule cell by cell");
            }
            check(!rf::buildPlane(src,plane),"an unchanged plane reports no change");
        }
    }
    {   // RadarCover against every source tested in turn (0x509ac0's circle).
        uint32_t seed=777;
        auto next=[&]{seed=seed*1103515245u+12345u;return seed>>8;};
        for(int round=0;round<30;++round) {
            rf::RadarCover cover;
            struct S{int32_t x,z,r;};std::vector<S> sources;
            const int n=1+int(next()%60);
            for(int k=0;k<n;++k) {
                S src{int32_t((next()%6000)<<16)+int32_t(next()%65536),int32_t((next()%6000)<<16),int32_t(30+next()%(round%3 ? 1500 : 200))};
                sources.push_back(src);cover.add(src.x,src.z,src.r);
            }
            cover.finish();
            for(int q=0;q<400;++q) {
                const int32_t x=int32_t((next()%7000)<<16)-(500<<16)+int32_t(next()%65536),z=int32_t((next()%7000)<<16)-(500<<16);
                bool any=false;
                for(const auto& src:sources) {
                    const int64_t dx=int64_t(x)-src.x,dz=int64_t(z)-src.z;
                    any=any || ((dx*dx)>>32)+((dz*dz)>>32)<=int64_t(src.r)*src.r;
                }
                check(cover.covers(x,z)==any,"radar cover matches every source tested in turn");
            }
        }
    }
    SDL_SetMainReady();check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
    constexpr int W=129,H=97;   // cells; the overlay is 2W x 2H texels
    auto* window=SDL_CreateWindow("fog uploads",0,0,2*W,2*H,SDL_WINDOW_HIDDEN);
    auto* renderer=SDL_CreateRenderer(window,-1,0);check(renderer,SDL_GetError());
    auto* texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,2*W,2*H);
    auto* mini=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,W,H);
    auto* target=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,2*W,2*H);
    check(texture && mini && target,SDL_GetError());
    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_NONE);SDL_SetTextureBlendMode(mini,SDL_BLENDMODE_NONE);
    tak::FogUpload upload,miniUpload(tak::FogUpload::Kind::Minimap);
    std::vector<uint8_t> plane(W*H,rf::kUnexplored);
    auto read=[&](SDL_Texture* t,int w,int h) {
        SDL_SetRenderTarget(renderer,target);SDL_RenderClear(renderer);
        SDL_Rect rect{0,0,w,h};SDL_RenderCopy(renderer,t,nullptr,&rect);
        std::vector<uint8_t> pixels(size_t(2*W)*2*H*4);
        check(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,pixels.data(),2*W*4)==0,"readback failed");
        return pixels;
    };
    auto verify=[&](bool force=false) {
        check(upload.update(texture,plane,W,H,force),"overlay upload failed");
        check(miniUpload.update(mini,plane,W,H,force),"minimap upload failed");
        std::vector<uint8_t> expect(size_t(2*W)*2*H*4);
        rf::writeTexels(plane,W,H,0,0,W,H,expect.data(),2*W*4);
        check(read(texture,2*W,2*H)==expect,"overlay texels match retail's blend");
        const auto m=read(mini,W,H);
        for(int z=0;z<H;++z)for(int x=0;x<W;++x) {
            const uint8_t* p=&m[(size_t(z)*2*W+x)*4];
            check(!p[0] && !p[1] && !p[2] && p[3]==tak::FogUpload::minimapAlpha(plane[size_t(z)*W+x]),
                  "minimap texel per cell");
        }
    };
    verify();check(upload.uploadedBytes()==size_t(4*W*H)*4,"initial complete upload");
    const auto generation=upload.generation();
    verify();check(upload.generation()==generation && upload.uploadedBytes()==0,"identical plane skips upload");
    plane[size_t(40)*W+40]=rf::kInView;verify();
    check(upload.generation()!=generation,"changed cells advance the generation");
    check(upload.uploadedBytes()==size_t(64*64)*4 && miniUpload.uploadedBytes()==size_t(32*32)*4,
          "one changed cell inside a block updates that block");
    plane[size_t(40)*W+40]=rf::kExplored;plane[size_t(40)*W+31]=rf::kInView;verify();
    check(upload.uploadedBytes()==size_t(2*64*64)*4 && miniUpload.uploadedBytes()==size_t(2*32*32)*4,
          "a cell on a block edge also refreshes the neighbouring block's blended texels");
    plane[0]=rf::kExplored;plane.back()=rf::kInView;verify();
    for(int z=30;z<36;++z)for(int x=30;x<70;++x)plane[size_t(z)*W+x]=uint8_t((x+z)%3);
    verify();check(upload.uploadedBytes()<size_t(4*W*H)*4,"moving visibility stays partial");
    verify(true);check(upload.uploadedBytes()==size_t(4*W*H)*4,"forced baseline upload");
    upload.clear();verify();check(upload.uploadedBytes()==size_t(4*W*H)*4,"reset restores whole image");
    SDL_SetRenderTarget(renderer,nullptr);SDL_DestroyTexture(target);SDL_DestroyTexture(texture);SDL_DestroyTexture(mini);
    SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    std::puts("PASS: fog upload: display plane, radar cover, retail texel blend, minimap cells, dirty blocks, edges and reset");return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
