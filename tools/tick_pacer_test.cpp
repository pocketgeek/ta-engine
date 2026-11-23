// The server's tick pacing (src/server/tickpacer.h) on a simulated wall clock: exact rates,
// speed changes, and the regression where a server that could not hold 8x (a Windows server
// sleeping a whole timer quantum per tick) owed thousands of 8x ticks after the host dropped
// back to 1x, and paid them back as "catching up" bursts at 1x.
#include "server/tickpacer.h"
#include <cstdint>
#include <cstdio>

using tak::srv::TickPacer;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// A simulated server: closes the next tick whenever it is due and the server is free, and
// publishes it `costUs` later (the tick job plus whatever the platform's wait costs).
struct SimServer {
    TickPacer pace;
    uint32_t tick = 0;
    uint64_t now = 0;
    uint64_t costUs = 0;
    // Run until wall time `untilUs`; returns the ticks published.
    uint32_t runUntil(uint64_t untilUs) {
        const uint32_t start = tick;
        for (;;) {
            const uint64_t at = pace.nextUs() > now ? pace.nextUs() : now;   // sleep to the deadline
            if (at + costUs > untilUs) break;
            now = at + costUs;
            ++tick;
            pace.advance(tick, now);
        }
        now = untilUs;
        return tick - start;
    }
    void setSpeed(int speed) { pace.changeSpeed(tick, speed, now); }
};
}  // namespace

int main() {
    // Exact rates: 240 ticks at 8x and 30 at 1x take exactly one second.
    {
        SimServer s; s.pace.rebase(0, 0, 80);
        s.runUntil(1000000 - 1);
        check(s.tick == 240 && s.pace.nextUs() == 1000000, "8x is exactly 240 ticks/s");
        SimServer t; t.pace.rebase(0, 0, 10);
        t.runUntil(1000000 - 1);
        check(t.tick == 30 && t.pace.nextUs() == 1000000, "1x is exactly 30 ticks/s");
    }
    // A speed change on schedule keeps the pending tick's deadline and paces on from it.
    {
        SimServer s; s.pace.rebase(0, 0, 10);
        s.runUntil(500000);                       // 15 ticks published, tick 15 due at 500 ms
        const uint64_t pending = s.pace.nextUs();
        s.setSpeed(80);
        check(s.pace.nextUs() == pending, "speed change keeps the pending deadline");
        const uint32_t n = s.runUntil(pending + 1000000 - 1);
        check(n == 240, "8x after a change from 1x runs 240 ticks/s");
    }
    // The regression: 8x on a server that can only publish a tick every 15.6 ms, then 1x.
    {
        SimServer s; s.pace.rebase(0, 0, 10);
        s.costUs = 15600;
        s.runUntil(2000000);
        s.setSpeed(80);
        const uint32_t fast = s.runUntil(12000000);
        check(fast >= 600 && fast <= 650, "a slow server runs 8x as fast as it can");
        s.setSpeed(10);
        s.costUs = 0;
        // Back at 1x: 30 ticks per second from the change, with no burst of owed 8x ticks.
        for (int second = 0; second < 5; ++second) {
            const uint32_t n = s.runUntil(12000000 + uint64_t(second + 1) * 1000000);
            if (n < 29 || n > 31) {
                std::fprintf(stderr, "  second %d after 8x -> 1x: %u ticks\n", second, n);
                check(false, "1x after a slow 8x stretch runs 30 ticks/s, not a catch-up burst");
                break;
            }
        }
    }
    // A short hiccup is paid back (the long-run rate stays exact); a long stall is not
    // (the room resumes at speed instead of flooding the clients).
    {
        SimServer s; s.pace.rebase(0, 0, 10);
        s.runUntil(1000000);
        s.now += 100000;                          // a 100 ms stall
        s.runUntil(3000000 - 1);
        check(s.tick == 90, "a 100 ms stall is caught up: 90 ticks in 3 s");
        SimServer t; t.pace.rebase(0, 0, 10);
        t.runUntil(1000000);
        t.now += 3000000;                         // a 3 s stall
        const uint32_t burst = t.runUntil(5000000);
        check(burst <= 31 + uint32_t(TickPacer::kMaxLagUs * 30 / 1000000),
              "a 3 s stall is not paid back as a burst");
    }
    if (failures) { std::fprintf(stderr, "tick_pacer: %d failure(s)\n", failures); return 1; }
    std::printf("tick_pacer: ok\n");
    return 0;
}
