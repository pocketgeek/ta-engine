#pragma once

#include <SDL.h>
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace tak {
// SDL's software backend can recognize a batched quad and switch to a
// different texture sampler. Keep its existing per-triangle coverage; GPUs
// can submit the same ordered triangles as one run.
inline int submitShadowMaskRun(SDL_Renderer* renderer,SDL_Texture* texture,
                               const SDL_Vertex* vertices,int count,bool batch) {
    const int step=batch ? count : 3;
    if (step<=0) return 0;
    for (int offset=0;offset<count;offset+=step)
        if (SDL_RenderGeometry(renderer,texture,vertices+offset,step,nullptr,0)!=0) return -1;
    return 0;
}

// A MOD texture must be white outside the silhouette: alpha alone does not
// mask SDL_BLENDMODE_MOD. Preserve fractional coverage at antialiased edges.
inline std::vector<uint8_t> shadowMaskPixels(std::span<const uint8_t> rgba,
                                           uint8_t shadowLevel) {
    std::vector<uint8_t> pixels(rgba.begin(),rgba.end());
    for (size_t i=0;i+3<pixels.size();i+=4) {
        const auto value=uint8_t(255-(255-shadowLevel)*pixels[i+3]/255);
        pixels[i]=pixels[i+1]=pixels[i+2]=value;
        pixels[i+3]=255;
    }
    return pixels;
}
// Coverage targets accumulate alpha, then apply darkness once. Keep cutout
// alpha rather than the white/gray RGB used by the direct MOD fallback.
inline std::vector<uint8_t> shadowCoveragePixels(std::span<const uint8_t> rgba) {
    std::vector<uint8_t> pixels(rgba.begin(),rgba.end());
    for(size_t i=0;i+3<pixels.size();i+=4)pixels[i]=pixels[i+1]=pixels[i+2]=0;
    return pixels;
}
// One replicated texel around each frame keeps filtered atlas edges isolated.
inline std::vector<uint8_t> paddedShadowPixels(std::span<const uint8_t> pixels,
                                              int width,int height) {
    std::vector<uint8_t> padded(size_t(width+2)*(height+2)*4);
    for(int y=0;y<height+2;++y)for(int x=0;x<width+2;++x) {
        const size_t src=(size_t(std::clamp(y-1,0,height-1))*width+std::clamp(x-1,0,width-1))*4;
        const size_t dst=(size_t(y)*(width+2)+x)*4;
        std::copy_n(pixels.data()+src,4,padded.data()+dst);
    }
    return padded;
}
inline std::vector<uint8_t> paddedShadowMaskPixels(std::span<const uint8_t> rgba,
                                                  int width,int height,uint8_t level) {
    return paddedShadowPixels(shadowMaskPixels(rgba,level),width,height);
}
inline std::vector<uint8_t> paddedShadowCoveragePixels(std::span<const uint8_t> rgba,
                                                      int width,int height) {
    return paddedShadowPixels(shadowCoveragePixels(rgba),width,height);
}
} // namespace tak
