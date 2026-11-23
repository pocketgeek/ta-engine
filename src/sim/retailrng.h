#pragma once

#include "sim/fixed.h"

namespace tak::sim {

// Retail's seed initializer, 0x535d30.
constexpr uint32_t retailSeed(uint32_t seed) { return (seed ^ 0x66e29572u) | 1u; }

// CRT rand (5d4444) is a separate stream from 535cc0. Retail also consumes
// this stream in rendering, so reproducing allocation alone is not a full
// stream-order model. Callers must supply the actual current CRT seed.
constexpr uint32_t retailCrtRandom(uint32_t& seed) {
    seed = seed * 0x343fdu + 0x269ec3u;
    return (seed >> 16) & 0x7fffu;
}

// 0x52a330 applies retail's per-recipient damage spread after attack, armour,
// and area-falloff scaling. The CRT draw is supplied by the caller even when
// scaledDamage is too small to produce a non-zero spread. `mode` is the power
// codes' damage mode (sim::DamageMode): DoubleShot doubles the spread float and
// HalfShot halves it through a double, both before the truncation, as 52a330
// does -- int(2 * s) is not 2 * int(s).
inline int retailDamageWithSpread(float scaledDamage, uint32_t crtRoll, uint8_t mode = 0) {
    const int range = int(scaledDamage * 0.15f);
    const int offset = int((2ll * range * crtRoll) / 32768ll) - range;
    float s = scaledDamage + float(offset);
    if (mode == 1) s *= 2.0f;
    else if (mode == 2) s = float(double(s) * 0.5);
    return int(s);
}

// 52f780: a storm owns this stream. Unlike 535cc0, a wrapped negative
// intermediate is not corrected, and even a zero span consumes a draw.
inline double retailWanderRandom(uint32_t& seed, float span) {
    const uint32_t next=seed*16807u-(seed/127773u)*0x7fffffffu;
    seed=next ? next : 0x7fffffffu;
    return double(seed)*0x1p-31*double(span);
}

// 512162..512255: select a uniformly ranked FREE slot, not a random occupied
// starting index followed by a scan. No free slots means no RNG consumption.
// The caller owns allocation flags and lifetime; this only selects the slot.
template<class Occupied,class Random>
int retailAllocateSlot(int capacity, int allocated, Occupied occupied,Random random) {
    const int free = capacity - allocated;
    if (free <= 0) return -1;
    int rank = int(random() * uint32_t(free) / 32768u);
    for (int slot = 0; slot < capacity; ++slot)
        if (!occupied(slot) && rank-- == 0) return slot;
    return -1;
}

template<class Occupied>
int retailAllocateSlot(uint32_t& crtSeed,int capacity,int allocated,Occupied occupied) {
    return retailAllocateSlot(capacity,allocated,occupied,[&]{return retailCrtRandom(crtSeed);});
}

// 0x535cc0: n<2 returns without consuming a draw. Schrage's form also
// preserves retail's special seed=0 result (0x7fffffff, not zero).
constexpr uint32_t retailRandom(uint32_t& seed, int32_t n) {
    if (n < 2) return 0;
    int64_t next = int64_t(seed % 127773) * 16807 - int64_t(seed / 127773) * 2836;
    if (next <= 0) next += 0x7fffffff;
    seed = uint32_t(next);
    return seed % uint32_t(n);
}

// Retail 0x512430: rand(201), add 900, scale by .001 and truncate to 16.16.
// Exhaustively compared with the executable for all 201 possible rolls.
constexpr Fixed individualSpeed(Fixed nominal, unsigned roll) {
    return nominal * Fixed::raw(int32_t((uint64_t(900 + roll) * 65536) / 1000));
}

} // namespace tak::sim
