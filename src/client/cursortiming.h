#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace tak {

// Retail keeps one frame index per registered sequence but a single countdown for
// the whole live-pointer animation manager. Only the selected sequence advances;
// changing the cursor selects another sequence without restarting either state.
// Its manager runs from a 30 Hz wall clock (also while simulation is paused) and
// consumes at most five ticks after a delayed frame. This template is SDL-free so
// the switch/resume rule can be checked independently of renderer resources.
template<size_t SequenceCount>
class CursorAnimationClock {
public:
    template<class Sequences>
    size_t frameAt(size_t sequence, uint64_t nowMilliseconds, const Sequences& sequences) {
        if (sequence >= SequenceCount) return 0;
        const uint64_t nowTick = nowMilliseconds * 30 / 1000;
        if (!started_) {
            started_ = true;
            lastTick_ = nowTick;
        } else {
            const uint64_t elapsed = nowTick > lastTick_ ? nowTick - lastTick_ : 0;
            const uint64_t updates = std::min<uint64_t>(5, elapsed);
            lastTick_ = nowTick;
            for (uint64_t i = 0; i < updates; ++i) advanceSelected(sequences);
        }
        selected_ = sequence;
        const auto& frames = sequences[sequence];
        if (frames.empty() || frameIndices_[sequence] >= frames.size()) return 0;
        return frameIndices_[sequence];
    }

private:
    template<class Sequences>
    void advanceSelected(const Sequences& sequences) {
        if (selected_ >= SequenceCount) return;
        const auto& frames = sequences[selected_];
        if (frames.empty() || frameIndices_[selected_] >= frames.size()) return;

        --countdown_;
        if (countdown_ >= 0) return;
        size_t& frame = frameIndices_[selected_];
        frame = (frame + 1) % frames.size();
        countdown_ = frames[frame].delayTicks;
    }

    std::array<size_t, SequenceCount> frameIndices_{};
    size_t selected_ = SequenceCount;
    uint64_t lastTick_ = 0;
    int64_t countdown_ = 0;
    bool started_ = false;
};

// Native 0x4d5930 selects end-of-order cursor markers from the global game tick.
// Its frame interval is twice the first GAF frame's delay, and it does not reset
// when an order is issued. The marker API uses the first frame duration even if
// later frames carry different authored delays.
inline size_t cursorOrderMarkerFrameAt(size_t frameCount, uint16_t firstFrameDelayTicks,
                                      uint64_t gameTick) {
    if (!frameCount) return 0;
    const uint64_t interval = 2 * uint64_t(std::max<uint16_t>(1, firstFrameDelayTicks));
    return size_t((gameTick / interval) % frameCount);
}

} // namespace tak
