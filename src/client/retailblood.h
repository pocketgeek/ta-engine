#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include "sim/retailmotion.h"

namespace tak {
struct RetailBloodParticle {
    std::array<int32_t,3> position{},velocity{};
    uint32_t color=0;
    template<class Random>
    static RetailBloodParticle emit(const std::array<int32_t,3>& origin,
            const std::array<uint32_t,3>& colors,Random random) {
        RetailBloodParticle particle;particle.position=origin;
        particle.color=colors[uint64_t(random())*3/32768];
        const uint16_t angle=uint16_t(random()*2);
        const int32_t radius=int32_t(random())*5-73728;
        particle.velocity[0]=sim::retailScaledCosine(angle,radius);
        particle.velocity[1]=int32_t(random()/2)*5+8192;
        particle.velocity[2]=sim::retailScaledSine(angle,radius);
        return particle;
    }
    struct Result {bool alive;bool stain;};
    template<class Terrain>
    Result tick(int32_t gravity,uint8_t sea,Terrain terrain) {
        for(unsigned i=0;i<3;++i)
            position[i]=std::bit_cast<int32_t>(uint32_t(position[i])+uint32_t(velocity[i]));
        velocity[1]=std::bit_cast<int32_t>(uint32_t(velocity[1])-uint32_t(gravity));
        const int y=position[1]>>16;
        if(terrain(position)<y)return {true,false};
        if(y<=sea)return {false,false};
        const auto height=uint32_t(terrain(position));
        // Native replaces only the integer half, retaining subpixel height.
        position[1]=std::bit_cast<int32_t>((uint32_t(position[1])&65535u)|(height<<16));
        return {false,true};
    }
};
}
