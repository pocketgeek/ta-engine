#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include "sim/fixed.h"

namespace tak {

// Display-only particles emitted by script effects 2..5. Positions and
// velocities are 16.16; the caller supplies the retail terrain sampler.
struct RetailPointParticle {
    std::array<int32_t,3> position{}, velocity{};
    uint32_t stage=0, period=8, countdown=8;

    // RGBA behavior observed at the native particle/renderer boundary.
    std::array<uint8_t,4> color() const {
        switch(stage) {
        case 0:return {41,222,222,255};
        case 1:return {37,194,194,255};
        case 2:return {31,157,157,255};
        case 3:return {24,109,109,255};
        case 4:return {19,76,76,255};
        case 5:return {15,47,47,255};
        default:return {0,0,0,0};
        }
    }

    template<class Random>
    static RetailPointParticle emit(const std::array<int32_t,3>& origin,
            const std::array<int32_t,3>& target,uint32_t period,Random random) {
        RetailPointParticle result;
        result.period=result.countdown=period;
        std::array<int32_t,3> delta{};
        uint64_t squared=0;
        for(size_t i=0;i<3;++i) {
            delta[i]=std::bit_cast<int32_t>(uint32_t(target[i])-uint32_t(origin[i]));
            squared+=uint64_t(int64_t(delta[i])*delta[i]);
        }
        const int32_t twiceLength=std::bit_cast<int32_t>(uint32_t(sim::isqrt64(squared))*2u);
        const int64_t reciprocal=twiceLength ? (int64_t(1)<<32)/twiceLength : 0;
        for(size_t i=0;i<3;++i) {
            result.velocity[i]=std::bit_cast<int32_t>(uint32_t((int64_t(delta[i])*reciprocal)>>16));
            const int32_t jitter=int32_t(uint64_t(random())*7/32768)-3;
            result.position[i]=std::bit_cast<int32_t>(uint32_t(origin[i])+uint32_t(jitter)*65536u);
        }
        return result;
    }

    template<class TerrainHeight>
    bool tick(int32_t seaLevel,TerrainHeight terrainHeight) {
        for(unsigned i=0;i<3;++i)
            position[i]=std::bit_cast<int32_t>(uint32_t(position[i])+uint32_t(velocity[i]));
        if(terrainHeight(position)>=seaLevel)return false;
        if(--countdown!=0)return true;
        countdown=period;
        return ++stage<6;
    }
};

} // namespace tak
