#pragma once

// Retained CPU buffers: trim after sustained low demand (rendering workstream J).
//
// The renderer's per-frame buffers (assembled body vertices, painter and command
// lists, shadow scratch, worker scratch) keep their peak capacity so steady frames
// never reallocate. That is right while the demand lasts and wrong after it: a
// 10,000-unit battle leaves ~50 MB of body vertices resident in an empty view for
// the rest of the session. A TrimTracker decides, per buffer, when the capacity is
// no longer needed:
//
//   * Demand = the frame's PEAK need in bytes (callers note buffers that are filled
//     more than once a frame before each refill, not just at the frame end).
//   * Sustained: the decision looks at the last whole window -- two half-window
//     buckets, each at least windowMs/2 AND windowFrames/2 long -- so one quiet frame,
//     or a few quiet frames between busy ones, never trims. Decisions happen at bucket
//     boundaries only: never per frame.
//   * Hysteresis: trim only when capacity > factor x the window's peak need, and then
//     to the peak need + 1/16 (never below the floor), so normal fluctuation never
//     triggers it and the buffer keeps headroom for the demand that remains.
//   * No thrash: a buffer that has to grow back past what it was trimmed to soon after
//     a trim (within four windows) doubles its window for the next trim (up to 8x); a
//     trim that holds for four windows halves it again.
//
// Pure arithmetic on (need, capacity, frame, time): the clock is the caller's (wall
// time in play, the render study's presentation clock in benchmarks), so a study run
// trims at the same frames every time. Never feeds the simulation.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tak::render {

struct TrimPolicy {
    uint64_t windowMs = 5000;      // a whole window of low demand, by time ...
    uint32_t windowFrames = 120;   // ... and by frames (a slow frame rate does not shorten it)
    uint32_t factor = 2;           // trim only when capacity > factor x the window's peak need
    size_t floorBytes = size_t(64) << 10;   // never trim below this
    uint32_t maxBackoff = 8;       // window multiplier ceiling after regrowth
};

class TrimTracker {
public:
    // Once per frame, after the buffer's last use: the frame's peak need and the
    // retained capacity, both in bytes. Returns the capacity to shrink to (bytes),
    // or 0 to keep it.
    size_t observe(size_t need, size_t capacity, uint64_t frame, uint64_t nowMs, const TrimPolicy& p) {
        if (!started_ || nowMs < bucketMs_ || frame < bucketFrame_) {
            started_ = true;
            bucketFrame_ = frame;
            bucketMs_ = nowMs;
            prev_ = cur_ = need;
            havePrev_ = false;
        }
        // Regrowth after a trim: the trimmed size was too small for this demand.
        if (trimmedTo_ && capacity > trimmedTo_) {
            if (nowMs - trimMs_ < 4 * window(p)) backoff_ = std::min(backoff_ * 2, std::max(p.maxBackoff, 1u));
            trimmedTo_ = 0;
        } else if (trimmedTo_ && nowMs - trimMs_ >= 4 * window(p)) {
            backoff_ = std::max(backoff_ / 2, 1u);   // the trim held: relax the backoff
            trimmedTo_ = 0;
        }
        cur_ = std::max(cur_, need);
        const uint64_t halfMs = window(p) / 2;
        const uint64_t halfFrames = uint64_t(p.windowFrames) * backoff_ / 2;
        if (frame - bucketFrame_ < halfFrames || nowMs - bucketMs_ < halfMs) return 0;
        // Bucket boundary: [prev, cur] cover the last whole window.
        size_t result = 0;
        if (havePrev_) {
            const size_t peak = std::max(prev_, cur_);
            const size_t keep = std::max(peak + peak / 16, p.floorBytes);
            if (capacity > keep && capacity > uint64_t(std::max(p.factor, 2u)) * std::max(peak, p.floorBytes)) {
                result = keep;
                trimmedTo_ = keep;
                trimMs_ = nowMs;
            }
        }
        prev_ = cur_;
        havePrev_ = true;
        cur_ = 0;
        bucketFrame_ = frame;
        bucketMs_ = nowMs;
        return result;
    }
    void reset() { *this = TrimTracker{}; }
    uint32_t backoff() const { return backoff_; }

private:
    uint64_t window(const TrimPolicy& p) const { return p.windowMs * backoff_; }
    bool started_ = false, havePrev_ = false;
    uint64_t bucketFrame_ = 0, bucketMs_ = 0;
    size_t prev_ = 0, cur_ = 0;
    size_t trimmedTo_ = 0;   // capacity of the last trim, until regrowth or relaxation
    uint64_t trimMs_ = 0;
    uint32_t backoff_ = 1;
};

// Shrink `v` to hold at least max(size, targetElems) elements, keeping its
// contents. Returns the bytes of capacity released (0 if nothing shrank).
template <class T>
size_t shrinkVector(std::vector<T>& v, size_t targetElems) {
    const size_t keep = std::max(targetElems, v.size());
    if (v.capacity() <= keep) return 0;
    const size_t before = v.capacity();
    std::vector<T> fresh;
    fresh.reserve(keep);
    fresh.insert(fresh.end(), std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
    v.swap(fresh);
    return (before - v.capacity()) * sizeof(T);
}

} // namespace tak::render
