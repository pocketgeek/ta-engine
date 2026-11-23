#pragma once
#include <cstdint>
#include <span>

namespace tak::sim {
struct RetailEffectClock {
    uint16_t frame = 0, remaining = 0;
    bool active = false;
    void start(std::span<const uint16_t> durations, uint32_t initial = 0) {
        frame = initial < durations.size() ? uint16_t(initial) : 0;
        active = !durations.empty();remaining = active ? durations[frame] : 0;
    }
    void tick(std::span<const uint16_t> durations, bool loop) {
        if (!active) return;
        if (remaining >= 2) { --remaining;return; }
        ++frame;
        if (frame >= durations.size()) {
            if (!loop) { active = false;return; }
            frame = 0;
        }
        remaining = durations[frame];
    }
};
} // namespace tak::sim
