#pragma once
// How a feature destroyed by damage is drawn while it plays its death
// (display only).
//
// Retail (icd): damage's destruction (494ce0 mode 0) gives a sprite feature
// with a TDF seqnamedie an object whose clock (+0x4, loaded nonlooping at
// 493eff) steps once a tick through 5373d0 -- each frame held for max(1,
// delay) ticks -- and the tick it runs out (495ef7) featuredead replaces the
// feature. The draw 4fd430 then shows the clock's current frame (4fd821) and
// only a seqnamedieshad shadow (4fd7cc), which no shipped feature has.
//
// The sim keeps Feature::dieLeft (hashed); the client draws from it.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace tak::featuredie {

// Ticks the clock has run in the snapshot of `gameTick`, for a playback of
// `totalTicks` that ends (featuredead) at tick `end`.
inline int elapsed(int totalTicks, uint32_t end, uint32_t gameTick) {
    const int64_t left = std::max<int64_t>(0, int64_t(end) - int64_t(gameTick));
    return int(std::max<int64_t>(0, int64_t(totalTicks) - left));
}

// The frame shown after `ticks` steps: the frames whose cumulative hold
// (`tickEnd`, max(1, delay) each) has run out are behind it. The last frame
// stays until the sim replaces the feature.
inline size_t frame(const std::vector<int>& tickEnd, int ticks) {
    if (tickEnd.empty()) return 0;
    const size_t index = size_t(std::upper_bound(tickEnd.begin(), tickEnd.end(), ticks) - tickEnd.begin());
    return std::min(index, tickEnd.size() - 1);
}

} // namespace tak::featuredie
