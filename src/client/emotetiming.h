#pragma once
#include <cstdint>

namespace tak {
// A sequenced cosmetic trigger starts a viewer-local, monotonic ten-second
// track. Simulation speed, paused ticks and delayed selection batches do not
// advance or restart it.
struct EmoteTiming {
    uint64_t sequence = 0, startedMs = 0;
    bool observe(uint64_t next, uint64_t now) {
        if (next == sequence) return false;
        sequence = next; startedMs = now;
        return true;
    }
    bool active(uint64_t now) const { return sequence && now >= startedMs && now - startedMs < 10000; }
    float seconds(uint64_t now) const { return now >= startedMs ? float(now - startedMs) / 1000.f : 0.f; }
};
}
