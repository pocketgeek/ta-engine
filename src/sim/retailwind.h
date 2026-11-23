#pragma once

#include "sim/retailmotion.h"
#include "sim/retailrng.h"
#include <cstdint>

namespace tak::sim {

struct RetailWind {
    int32_t minimum = 100, maximum = 2000;
    uint32_t nextTick = 0;
    int32_t x = 0, z = 0, speed = 0;
    uint16_t heading = 0, flags = 0;
    uint32_t generation = 0;
};

// 525170: speed and bearing consume the shared game RNG; the deadline uses
// CRT rand. The direction test intentionally compares abs(z) with signed x.
template<class Random, class CrtRandom>
void retailTickWind(RetailWind& wind, uint32_t tick, Random random, CrtRandom crtRandom) {
    if (wind.nextTick >= tick) { wind.flags &= uint16_t(~1u); return; }
    wind.speed = wind.minimum + int32_t(random(wind.maximum - wind.minimum));
    if (wind.speed) wind.heading = uint16_t(wind.heading + random(16384) - 8192);
    wind.x = retailScaledSine(wind.heading, wind.speed);
    wind.z = retailScaledCosine(wind.heading, wind.speed);
    const int64_t absZ = wind.z < 0 ? -int64_t(wind.z) : wind.z;
    const uint32_t base = absZ > wind.x ? 3 : 5;
    wind.nextTick = tick + (base + (crtRandom() * (base * 2)) / 32768) * 30;
    wind.flags |= 1;
    ++wind.generation;
}

} // namespace tak::sim
