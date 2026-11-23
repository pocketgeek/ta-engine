#pragma once
// Server tick pacing: when each tick of a running room is due on the wall clock. Server wall
// clock only -- never sim state, never on the wire -- so nothing here can change a hash.
#include <algorithm>
#include <cstdint>

namespace tak::srv {

// The deadline of tick `t` is base + (t - baseTick) * 1e7 / (30 * speed) microseconds (speed in
// tenths), computed from the base each tick rather than accumulated, so a period that is not a
// whole number of microseconds (8x = 4166.67 us) neither rounds the rate (adding a 4 ms period
// ran 8x at 250 ticks/s) nor drifts.
//
// A server that falls behind its deadlines catches up by running the due ticks back to back.
// That debt is bounded: a deficit deeper than kMaxLagUs means the server cannot sustain the
// speed (or stalled), and paying it back would only flood every client with a burst of ticks.
// Rebasing at `now` instead runs the room as fast as it can go and no faster. A speed change
// likewise never inherits the old speed's debt: owed 8x ticks paid back at 1x were the
// periodic "catching up" bursts after a drop from 8x on a server that could not hold 8x.
class TickPacer {
public:
    static constexpr int kHz = 30;                   // ticks per second at 1x (kServerHz)
    static constexpr uint64_t kMaxLagUs = 250000;    // deepest deficit still paid back

    // Start pacing at `atUs` with `tick` due then (game start, resume, retry).
    void rebase(uint64_t atUs, uint32_t tick, int speed) {
        baseUs_ = atUs; baseTick_ = tick; speed_ = std::max(1, speed);
        nextUs_ = atUs;
    }
    // `tick` is the next tick to close, just after the previous one was published.
    void advance(uint32_t tick, uint64_t nowUs) {
        nextUs_ = baseUs_ + uint64_t(tick - baseTick_) * 10000000ull / uint64_t(kHz * speed_);
        if (nextUs_ + kMaxLagUs < nowUs) rebase(nowUs, tick, speed_);
    }
    // In-game speed change. Rebased at the pending tick's old-speed deadline, so the ticks
    // already paced keep their times and the new rate starts there -- unless that deadline has
    // already passed, in which case the new rate starts now.
    void changeSpeed(uint32_t tick, int speed, uint64_t nowUs) {
        rebase(std::max(nextUs_, nowUs), tick, speed);
    }
    uint64_t nextUs() const { return nextUs_; }
    int speed() const { return speed_; }

private:
    uint64_t nextUs_ = 0;    // wall deadline of the next tick
    uint64_t baseUs_ = 0;    // wall time of tick baseTick_
    uint32_t baseTick_ = 0;
    int speed_ = 10;         // tenths, as of the last rebase
};

}  // namespace tak::srv
