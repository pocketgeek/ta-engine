#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>

namespace tak {
// Sample the native single-tick effect clock without replaying every elapsed
// tick. A zero authored delay still occupies one tick, like delay one.
inline std::optional<size_t> retailEffectFrame(std::span<const uint16_t> durations,
                                              bool loop,uint32_t age) {
    uint64_t cycle=0;
    for (auto duration:durations) cycle+=std::max(1u,unsigned(duration));
    if (!cycle) return std::nullopt;
    uint64_t elapsed=age;
    if (loop) elapsed%=cycle;
    else if (elapsed>=cycle) return std::nullopt;
    for (size_t frame=0;frame<durations.size();++frame) {
        const unsigned duration=std::max(1u,unsigned(durations[frame]));
        if (elapsed<duration) return frame;
        elapsed-=duration;
    }
    return std::nullopt;
}

// The native sprite backend receives the projected world anchor, then places
// each authored frame relative to its own GAF x/y offsets. Use this for weapon
// sprites whose frame dimensions or offsets can vary across the sequence.
struct RetailEffectSpriteOrigin { float x, y; };
inline RetailEffectSpriteOrigin retailEffectSpriteOrigin(float anchorX,float anchorY,
                                                         int frameAnchorX,int frameAnchorY,
                                                         float zoom) {
    return {anchorX-float(frameAnchorX)*zoom,anchorY-float(frameAnchorY)*zoom};
}
} // namespace tak
