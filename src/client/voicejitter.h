#pragma once

// One talker's voice jitter buffer (client/voice.h). Header-only and free of SDL and
// Opus so the tests drive it directly: it decides, frame by frame, WHAT to play; the
// engine does the decoding.
//
// Voice reaches us over the same TCP connection as the game, relayed by the server, so
// packets never arrive out of order -- but they do arrive in bursts (the network, the
// server's loop, our own frame rate) and the server drops voice frames rather than let
// them delay game traffic. So the buffer holds a few frames before it starts playing,
// conceals a frame that is missing (Opus FEC from the next packet when it is here,
// packet loss concealment otherwise), waits out a late frame instead of skipping it, and
// throws away the oldest frames when a burst leaves it too deep.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace tak::voice {

inline constexpr uint8_t kFlagStart = 1;   // first frame of a transmission
inline constexpr uint8_t kFlagEnd = 2;     // the talker released the key (no audio)

// What to play for the next 20 ms of one talker.
struct Playout {
    enum Kind {
        Idle,      // nothing (not talking, or still buffering)
        Decode,    // decode `data`
        Fec,       // that frame is lost: rebuild it from `data`, the NEXT packet's FEC
        Conceal,   // missing, and nothing to rebuild it from: conceal
    } kind = Idle;
    const std::vector<uint8_t>* data = nullptr;   // valid until the next call to next()/push()
};

class JitterBuffer {
public:
    static constexpr int kMinTarget = 2;    // frames buffered before playing (40 ms)
    static constexpr int kMaxTarget = 8;    // 160 ms
    static constexpr int kMaxDepth = 15;    // deeper than this: drop the oldest down to target
    static constexpr int kMaxWait = 10;     // frames of concealment for a late frame (200 ms)
    static constexpr int kCalmFrames = 500; // frames played without a stall to take back a bump (10 s)

    struct Stats { uint64_t received = 0, decoded = 0, fec = 0, concealed = 0, late = 0, trimmed = 0; };

    // A packet arrived. `nowMs` is a wall clock in milliseconds (arrival jitter).
    void push(uint16_t seq, uint8_t flags, std::vector<uint8_t> opus, uint64_t nowMs) {
        if (flags & kFlagStart) restart(seq);
        const int64_t s = extend(seq);
        if (s < floor_) { ++stats_.late; return; }      // older than this transmission
        if (playing_ && s < next_) { ++stats_.late; return; }
        if (s > highest_) highest_ = s;
        haveHighest_ = true;
        ++stats_.received;
        if (lastArrivalMs_) {
            // Mean deviation from the 20 ms cadence, smoothed; the target depth follows.
            const double d = std::fabs(double(nowMs - lastArrivalMs_) - 20.0);
            jitterMs_ = jitterMs_ * 0.95 + d * 0.05;
            target_ = std::clamp(kMinTarget + int(jitterMs_ / 20.0 + 0.5) + bump_, kMinTarget, kMaxTarget);
        }
        lastArrivalMs_ = nowMs;
        if (flags & kFlagEnd) { ended_ = true; endSeq_ = s; }
        if (!(flags & kFlagEnd) || !opus.empty()) q_[s] = std::move(opus);
        // Nobody is pulling (no output device): never hold more than a burst's worth.
        while (q_.size() > size_t(4 * kMaxDepth)) { q_.erase(q_.begin()); ++stats_.trimmed; }
    }

    // The next 20 ms.
    Playout next() {
        current_.clear();
        if (!playing_) {
            if (q_.empty()) { if (ended_) reset(); return {}; }
            if (int(q_.size()) < target_ && !ended_) return {};   // still buffering
            playing_ = true;
            next_ = q_.begin()->first;
            waits_ = 0;
        }
        // A burst left us too deep: skip to `target` frames behind the newest.
        if (highest_ - next_ + 1 > kMaxDepth) {
            const int64_t to = highest_ - target_ + 1;
            for (auto it = q_.begin(); it != q_.end() && it->first < to;) { it = q_.erase(it); ++stats_.trimmed; }
            next_ = to;
        }
        if (auto it = q_.find(next_); it != q_.end()) {
            current_ = std::move(it->second);
            q_.erase(it);
            ++next_; waits_ = 0; ++stats_.decoded;
            // A stall's extra depth is not for ever: give one frame back per stretch of
            // calm playing, so a long game does not leave this talker at the maximum.
            if (bump_ > 0 && ++calm_ >= kCalmFrames) {
                calm_ = 0; --bump_;
                target_ = std::clamp(kMinTarget + int(jitterMs_ / 20.0 + 0.5) + bump_, kMinTarget, kMaxTarget);
            }
            return {Playout::Decode, &current_};
        }
        if (ended_ && next_ > endSeq_) { reset(); return {}; }   // played to the END marker
        if (q_.empty()) {
            // Nothing newer has arrived: the frame is late, not lost (TCP does not lose
            // it). Conceal without advancing so it plays when it comes; give up and
            // re-buffer if it takes too long, holding more from now on.
            if (ended_) { reset(); return {}; }
            if (++waits_ > kMaxWait) {
                playing_ = false; waits_ = 0;
                bump_ = std::min(bump_ + 1, kMaxTarget - kMinTarget); calm_ = 0;
                target_ = std::clamp(target_ + 1, kMinTarget, kMaxTarget);
                return {};
            }
            ++stats_.concealed;
            return {Playout::Conceal, nullptr};
        }
        // A newer packet is here, so this one was dropped: rebuild it from the next
        // packet's FEC when that one is the very next, else conceal.
        const int64_t lost = next_++;
        waits_ = 0;
        if (auto nx = q_.find(lost + 1); nx != q_.end()) {
            ++stats_.fec;
            return {Playout::Fec, &nx->second};
        }
        ++stats_.concealed;
        return {Playout::Conceal, nullptr};
    }

    bool playing() const { return playing_; }
    bool idle() const { return !playing_ && q_.empty(); }
    int target() const { return target_; }
    int bump() const { return bump_; }
    size_t depth() const { return q_.size(); }
    const Stats& stats() const { return stats_; }

private:
    // Sequence numbers are 16 bits on the wire; widen them relative to the newest seen.
    int64_t extend(uint16_t seq) const {
        if (!haveHighest_) return seq;
        return highest_ + int16_t(uint16_t(seq - uint16_t(highest_)));
    }
    void restart(uint16_t seq) {
        reset();
        const int64_t s = haveHighest_ ? extend(seq) : int64_t(seq);
        highest_ = s; haveHighest_ = true; floor_ = s;
    }
    void reset() {
        q_.clear(); playing_ = false; ended_ = false; waits_ = 0; endSeq_ = 0;
    }

    std::map<int64_t, std::vector<uint8_t>> q_;
    std::vector<uint8_t> current_;
    int64_t next_ = 0, highest_ = 0, floor_ = INT64_MIN, endSeq_ = 0;
    bool haveHighest_ = false, playing_ = false, ended_ = false;
    int waits_ = 0, target_ = kMinTarget, bump_ = 0, calm_ = 0;
    uint64_t lastArrivalMs_ = 0;
    double jitterMs_ = 0;
    Stats stats_;
};

}  // namespace tak::voice
