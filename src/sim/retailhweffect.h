#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace tak {
// 4f40f0: color ramps use an integer per-entry increment, not interpolation
// of the full fraction at each entry. Only the last entry gets the exact end.
inline void retailEffectColorRamp(std::array<uint32_t,256>& palette,int first,int last,
        uint32_t start,uint32_t end) {
    if(last<=first)return;
    std::array<int,3> step{};
    for(unsigned channel=0;channel<3;++channel)
        step[channel]=(int(uint8_t(end>>(channel*8)))-int(uint8_t(start>>(channel*8))))/(last-first);
    uint32_t color=start;
    for(int index=first;index<last;++index) {
        palette[size_t(index)]=color;
        for(unsigned channel=0;channel<3;++channel) {
            const unsigned shift=channel*8;
            const uint8_t component=uint8_t(int(uint8_t(color>>shift))+step[channel]);
            color=(color&~(255u<<shift))|(uint32_t(component)<<shift);
        }
    }
    palette[size_t(last)]=end;
}

// 4f4ed0: the named effect's texture is ARGB4444. Opacity comes from the
// intensity index, independently of RGB. Native does not clamp overbright bins.
inline std::array<uint16_t,256> retailEffectPalette(const std::array<uint32_t,256>& colors,
        int32_t intensity) {
    std::array<uint16_t,256> result{};
    for(unsigned index=0;index<256;++index) {
        const uint32_t color=colors[index];
        const uint32_t alpha=uint32_t(int32_t(index*255)/(intensity>>8));
        result[index]=uint16_t(((alpha&0xfff0u)<<8)|((color&0xf0u)<<4)|
            ((color>>8)&0xf0u)|((color>>20)&15u));
    }
    return result;
}

// 4f46b0: named hardware effects use an 8-bit intensity plane and particles
// with 24.8 X/Y/intensity. New emission occurs between diffuse() and stamp().
struct RetailEffectParticle {
    std::array<int32_t,3> position{},velocity{};
    bool advance(int width,int height) {
        for(unsigned axis=0;axis<3;++axis)
            position[axis]=std::bit_cast<int32_t>(uint32_t(position[axis])+uint32_t(velocity[axis]));
        const int x=position[0]>>8,y=position[1]>>8;
        return position[2]>0 && x>0 && x<width-1 && y>0 && y<height-2;
    }
};

inline void retailEffectAdvance(std::vector<RetailEffectParticle>& particles,int width,int height) {
    std::erase_if(particles,[&](auto& particle){return !particle.advance(width,height);});
}

inline void retailEffectDiffuse(std::span<uint8_t> pixels,int width,int height,bool rise) {
    // Descending, in-place rows are intentional: rising fields sample rows
    // already updated this tick. Border bytes are left untouched.
    for(int y=height-3;y>0;--y) {
        const int middle=y+int(rise);
        for(int x=1;x<width-1;++x) {
            const unsigned sum=unsigned(pixels[size_t(middle-1)*width+x])+
                pixels[size_t(middle+1)*width+x]+pixels[size_t(middle)*width+x-1]+
                pixels[size_t(middle)*width+x+1];
            pixels[size_t(y)*width+x]=uint8_t(sum/4);
        }
    }
}

inline void retailEffectStamp(std::span<uint8_t> pixels,int width,
        std::span<const RetailEffectParticle> particles) {
    // Native traversal overwrites rather than adds: the last particle at a
    // texel wins, even when it is dimmer than the previous particle.
    for(const auto& particle:particles)
        pixels[size_t(particle.position[1]>>8)*width+(particle.position[0]>>8)]=
            uint8_t(particle.position[2]>>8);
}

// 4f3f20 / 4f3df0: the line-lightning source writes a recursively jittered
// polyline into the intensity plane, then supplies its origin as a particle.
// Authored endpoints must be inside the plane, as required by the native loader.
template<class Random>
void retailEffectLightning(std::span<uint8_t> pixels,int width,int height,
        std::array<int32_t,2> start,std::array<int32_t,2> end,int32_t intensity,
        bool fade,Random random) {
    int x0=start[0]>>8,y0=start[1]>>8,x1=end[0]>>8,y1=end[1]>>8;
    if(x1<x0) {std::swap(x0,x1);std::swap(y0,y1);}
    const int first=intensity>>8,last=fade ? 0 : first;
    pixels[size_t(y0)*width+x0]=uint8_t(first);
    pixels[size_t(y1)*width+x1]=uint8_t(last);
    const int variation=std::min(8,std::max(x1-x0,std::abs(y1-y0))/4);
    auto subdivide=[&](auto&& self,int ax,int ay,int ai,int bx,int by,int bi,int spread)->void {
        if(ax==bx && ay==by)return;
        int mx=(ax+bx)/2,my=(ay+by)/2,mi=(ai+bi)/2;
        const int jitter=int(uint32_t(random())*uint32_t(spread)*2u/32768u)-spread;
        if(bx-ax<std::abs(by-ay))mx+=jitter;else my+=jitter;
        if(mx>0 && mx<width-1 && my>0 && my<height-2)
            pixels[size_t(my)*width+mx]=uint8_t(mi);
        if(spread>1)--spread;
        if(mx-ax>1 || std::abs(my-ay)>1)self(self,ax,ay,ai,mx,my,mi,spread);
        if(bx-mx>1 || std::abs(by-my)>1)self(self,mx,my,mi,bx,by,bi,spread);
    };
    subdivide(subdivide,x0,y0,first,x1,y1,last,variation);
}

struct RetailLightningSource {
    std::array<int32_t,2> position{},end{};
    int32_t rate=0,countdown=0; // Cloning an emitter resets its countdown to zero.
    bool fade=false;
    bool ready() {
        countdown=std::bit_cast<int32_t>(uint32_t(countdown)-1u);
        if(countdown>0)return false;
        countdown=rate;
        return true;
    }
};

struct RetailLightningEffect {
    int width=0,height=0;
    uint32_t capacity=0,nextSource=0;
    int32_t intensity=0,decay=0;
    bool rise=false;
    std::vector<RetailLightningSource> sources;
    std::vector<RetailEffectParticle> particles;
    std::vector<uint8_t> pixels;

    template<class Random>
    bool tick(bool emit,Random random) {
        retailEffectAdvance(particles,width,height);
        retailEffectDiffuse(pixels,width,height,rise);
        if(emit && particles.size()<capacity && !sources.empty()) {
            const uint32_t first=nextSource;
            do {
                auto& source=sources[nextSource];
                if(source.ready()) {
                    retailEffectLightning(pixels,width,height,source.position,source.end,
                        intensity,source.fade,random);
                    particles.push_back({{source.position[0],source.position[1],intensity},{0,0,decay}});
                }
                nextSource=uint32_t((nextSource+1u)%sources.size());
            } while(particles.size()<capacity && nextSource!=first);
        }
        retailEffectStamp(pixels,width,particles);
        return !particles.empty();
    }
};
} // namespace tak
