#pragma once
#include <array>
#include <bit>
#include <cstdint>

namespace tak {
struct RetailFlameScan {
    std::array<int32_t,3> endpoint{};
    uint32_t lifetime=1;
};
// 52d3fa: pre-scan the whole ray, retaining its endpoint but leaving the
// traveling front at the muzzle. Collision may clip the queried position.
// Native weapon loading guarantees a nonzero substep-speed product.
template<class Collision>
RetailFlameScan retailFlameScan(std::array<int32_t,3> origin,
        const std::array<int32_t,3>& step,uint32_t range,uint32_t speed,
        uint32_t substeps,Collision collision) {
    const uint32_t ticks=(range<<16)/(speed*substeps);
    for(uint32_t tick=0;tick<ticks;++tick) {
        for(uint32_t sub=0;sub<substeps;++sub) {
            for(unsigned axis=0;axis<3;++axis)
                origin[axis]=std::bit_cast<int32_t>(uint32_t(origin[axis])+uint32_t(step[axis]));
            if(collision(origin)==2)return {origin,tick+1};
        }
    }
    return {origin,ticks+1};
}

// LineOfSightFire 52d58f: a fresh particle travels from the current muzzle
// toward the retained endpoint, with one CRT draw per component.
inline std::array<int32_t,3> retailFlameVelocity(const std::array<int32_t,3>& origin,
        const std::array<int32_t,3>& endpoint,int32_t lifetime,const std::array<uint16_t,3>& rolls) {
    std::array<int32_t,3> velocity{};
    for(unsigned axis=0;axis<3;++axis) {
        const int32_t delta=std::bit_cast<int32_t>(uint32_t(endpoint[axis])-uint32_t(origin[axis]));
        const int32_t base=delta/lifetime;
        const int32_t twice=std::bit_cast<int32_t>(uint32_t(base)*2u);
        const int64_t spread=int64_t(twice/20)*rolls[axis]/32768-base/20;
        velocity[axis]=std::bit_cast<int32_t>(uint32_t(base)+uint32_t(spread));
    }
    return velocity;
}

// FlameParticle 4f1790/4f16a0/4f16e0. Expiry precedes motion; the sprite
// sequence spans the particle's own flight lifetime, not a global frame rate.
struct RetailFlameParticle {
    std::array<int32_t,3> position{},velocity{};
    int32_t remaining=0,lifetime=0;
    uint32_t kind=0;
    bool tick() {
        remaining=std::bit_cast<int32_t>(uint32_t(remaining)-1u);
        if(remaining<=0)return false;
        for(unsigned axis=0;axis<3;++axis)
            position[axis]=std::bit_cast<int32_t>(uint32_t(position[axis])+uint32_t(velocity[axis]));
        return true;
    }
    int frame(uint16_t count) const {
        const auto age=uint32_t(lifetime)-uint32_t(remaining);
        const int32_t product=std::bit_cast<int32_t>(age*count);
        return product/lifetime;
    }
};
} // namespace tak
