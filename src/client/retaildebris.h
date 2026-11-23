#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <algorithm>

namespace tak {

template<class Height>
int retailDebrisTerrainHeight(int32_t x,int32_t z,int width,int height,Height sample) {
    const int cx=(x>>16)/16,cz=(z>>16)/16;
    if(cx<0 || cz<0 || cx>=width || cz>=height)return -1;
    const int nx=std::min(cx+1,width-1),nz=std::min(cz+1,height-1);
    const std::array<int,4> values={sample(cx,cz),sample(nx,cz),sample(cx,nz),sample(nx,nz)};
    return (*std::min_element(values.begin(),values.end())+
            *std::max_element(values.begin(),values.end()))/2;
}

// Display-only detached-piece motion (native 492420). Geometry ownership and
// optional attached emitters belong to the caller; this state never enters sim.
struct RetailDebrisMotion {
    std::array<int32_t,3> position{}, velocity{};
    std::array<uint16_t,3> rotation{}, spin{}; // spin already mapped to X/Y/Z
    uint32_t remaining=900, flags=0;
    struct Result { bool alive; int effect=-1, light=-1; };

    // COB EXPLODE dispatch uses three bounded draws, in X/Y/Z order. Keep
    // this separate from geometry allocation: BITMAPONLY consumes no draws.
    template<class Random>
    static bool launch(uint32_t scriptFlags, Random random, RetailDebrisMotion& out) {
        if(scriptFlags&0x20)return false;
        out=RetailDebrisMotion{};
        out.velocity[0]=(20-int32_t(random(40)))*4096;
        out.velocity[1]=int32_t(random(10))*16384;
        out.velocity[2]=(20-int32_t(random(40)))*4096;
        out.flags=((scriptFlags&1)?4u:0u) |
            ((scriptFlags&2)?((scriptFlags&1)?32u:16u):0u) |
            ((scriptFlags&4)?8u:0u) | ((scriptFlags&8)?2u:0u) |
            ((scriptFlags&16)?1u:0u) | (scriptFlags&64);
        return true;
    }

    template<class Terrain>
    Result tick(uint8_t sea, int32_t gravity, bool alternateSplash,
                bool suppressSplash, Terrain terrain) {
        if(--remaining==0)return {false};
        const int32_t y=position[1]>>16;
        if(y<=sea) {
            if((flags&16) && !suppressSplash)
                return {false,alternateSplash ? 10:9,-1};
            return {false};
        }
        if(y+(velocity[1]>>16)<=terrain(position))
            return (flags&16) ? Result{false,0,0}:Result{false};
        for(unsigned i=0;i<3;++i) {
            position[i]=std::bit_cast<int32_t>(uint32_t(position[i])+uint32_t(velocity[i]));
            rotation[i]=uint16_t(rotation[i]+spin[i]);
        }
        if(flags&8)
            velocity[1]=std::bit_cast<int32_t>(uint32_t(velocity[1])-uint32_t(gravity));
        return {true};
    }
};

} // namespace tak
