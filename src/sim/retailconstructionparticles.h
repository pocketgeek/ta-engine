#pragma once

#include "retailmotion.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <vector>

namespace tak::sim {

// 4f1310/4f12d0: construction particles move vertically relative to their
// owner. Animation is display-owned; capacity and lifetime affect CRT draws.
struct RetailConstructionParticle {
    int32_t x=0,y=0,z=0,speed=0,ceiling=0;
    uint32_t displayAge=0; // Cosmetic clock only; excluded from gameplay hashing.
    bool advance() {
        if (speed<0 ? y<=0 : y>ceiling) return false;
        y=std::bit_cast<int32_t>(uint32_t(y)+uint32_t(speed));
        ++displayAge;
        return true;
    }
};

struct RetailConstructionEmitter {
    uint32_t capacity=0;
    int32_t radius=0,height=0;
    std::vector<RetailConstructionParticle> particles;

    // 4f1430: exactly two CRT draws per admitted particle. The second draw's
    // shift wraps at 32 bits before signed division, including at draw 16384.
    template<class Random>
    void emit(uint32_t count,int32_t ownerHeight,bool rising,Random random) {
        count=std::min(count,capacity>particles.size() ? uint32_t(capacity-particles.size()) : 0u);
        for (uint32_t i=0;i<count;++i) {
            const uint16_t angle=uint16_t(random()*2u);
            const int32_t speed=0x20000+std::bit_cast<int32_t>(uint32_t(random())<<17)/32768;
            RetailConstructionParticle p;
            p.x=std::bit_cast<int32_t>(0u-uint32_t(retailScaledSine(angle,radius)));
            p.z=std::bit_cast<int32_t>(0u-uint32_t(retailScaledCosine(angle,radius)));
            p.y=rising ? 0 : height;
            p.speed=rising ? speed : -speed;
            p.ceiling=rising ? std::bit_cast<int32_t>(uint32_t(ownerHeight)+uint32_t(height)) : 0;
            particles.push_back(p);
        }
    }
    void advance() {
        std::erase_if(particles,[](auto& p) { return !p.advance(); });
    }
};

} // namespace tak::sim
