#pragma once

#include <cstdint>
#include <algorithm>
#include <array>
#include <vector>
#include <optional>
#include <string>

namespace ta::sim {

struct UnitType;

struct RetailPlayerCacheClock {
    bool enabled=false;
    uint32_t lastRefresh=0;
};

// 411a90: player unit/catalogue bookkeeping (including human players).
// Unsigned last+7 comparison is intentional, including its wrap behavior.
// Returns whether the periodic availability-cache rebuild was selected.
// The World host answers availability/count queries from current state rather
// than retaining retail's caches, but this clock still advances the shared RNG.
template<class Random>
bool retailTickPlayerCache(RetailPlayerCacheClock& clock,uint32_t tick,Random random) {
    if (!clock.enabled || tick<clock.lastRefresh+7u) return false;
    clock.lastRefresh=tick;
    return random(30)==0;
}

} // namespace ta::sim
