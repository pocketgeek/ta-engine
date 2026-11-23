#pragma once
#include <cstdint>
#include <optional>

namespace tak {
// Native feature smoke reads the burning feature's body frame before advancing
// its per-instance clock. Elapsed tick one therefore samples the initial frame.
inline std::optional<uint32_t> retailFeatureSmokeAge(uint32_t tick,uint32_t burnStarted) {
    const uint32_t elapsed=tick-burnStarted;
    if(!elapsed || elapsed>=0x80000000u)return std::nullopt;
    return elapsed-1;
}
} // namespace tak
