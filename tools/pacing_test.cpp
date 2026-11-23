// The client jitter-buffer pacing math (src/net/pacing.h) on a scripted clock: a server sends
// one bundle per tick at the game speed, the client polls once a display frame and reads every
// bundle that has arrived. 0.9.1 fix D: a client stall must not be read as network jitter, and
// the buffer target must never ask for more ticks than server flow control lets a client hold
// (the regression: +10 played at 1.40x, "+4", for a minute after an alt-tab or a watch game's
// load gap).
#include "net/pacing.h"
#include <cmath>
#include <cstdint>
#include <cstdio>

using tak::net::ArrivalJitter;

namespace {
int failures = 0;
void check(bool ok, const char* what, double got = 0) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s (got %.2f)\n", what, got); ++failures; }
}

// Server bundles every 1000/(3*sendTenths) ms from t=0 (sendTenths: the rate the server actually
// ticks, which flow control may hold below the room speed); the client polls every pollMs and
// reads each bundle whose arrival time has passed. stallAt/stallMs: no poll in that window.
struct Link {
    ArrivalJitter jit;
    int speedTenths = 10;     // the room speed (the jitter schedule)
    int sendTenths = 10;      // the rate bundles actually leave the server
    double nextBundle = 0;    // arrival time of the next unsent bundle
    double gapAt = -1, gapMs = 0;   // a network gap: bundles due in [gapAt, gapAt+gapMs) land at its end
    uint64_t now = 0;
    void poll() {
        jit.beginPoll(now);
        for (;;) {
            double at = nextBundle;
            if (gapAt >= 0 && at >= gapAt && at < gapAt + gapMs) at = gapAt + gapMs;
            if (at > double(now)) break;
            jit.noteBundle(now, speedTenths);
            nextBundle += 1000.0 / (3.0 * sendTenths);
        }
    }
    void run(uint64_t untilMs, uint64_t pollMs, uint64_t stallAt = 0, uint64_t stallMs = 0) {
        while (now + pollMs <= untilMs) {
            now += pollMs;
            if (stallMs && now >= stallAt && now < stallAt + stallMs) { now = stallAt + stallMs; }
            poll();
        }
    }
};
}  // namespace

int main() {
    // Steady 2x at a 120 Hz frame: no lateness.
    {
        Link l; l.speedTenths = l.sendTenths = 20;
        l.run(10000, 8);
        check(l.jit.jitterMs < 1.0f, "steady 2x at 120 Hz reads no jitter", l.jit.jitterMs);
    }
    // Slow frames (60 ms) are the frame term's job, not jitter's.
    {
        Link l; l.speedTenths = l.sendTenths = 20;
        l.run(10000, 60);
        check(l.jit.jitterMs < 1.0f, "60 ms frames at 2x read no jitter", l.jit.jitterMs);
    }
    // The regression: the client itself stalls 1500 ms (bundles keep arriving into the socket).
    for (int sp : {10, 20, 80}) {
        Link l; l.speedTenths = l.sendTenths = sp;
        l.run(20000, 8, 10000, 1500);
        check(l.jit.jitterMs < 1.0f, "a 1500 ms client stall leaves jitter ~0", l.jit.jitterMs);
        // The watch game's load gap: the server ticks from the start without waiting for a loading
        // spectator, whose first bundles came in during the handshake and the rest 1.2 s later.
        Link w; w.speedTenths = w.sendTenths = sp;
        w.run(48, 8);
        w.run(1248, 1200);
        w.run(5000, 8);
        check(w.jit.jitterMs < 1.0f, "a 1200 ms load gap leaves jitter ~0", w.jit.jitterMs);
    }
    // A real network gap with the client polling throughout IS jitter (short by at most one poll).
    {
        Link l; l.gapAt = 5000; l.gapMs = 300;
        l.run(5400, 8);
        const float want = 300.0f - 1000.0f / 30.0f;
        check(l.jit.jitterMs >= want - 9.0f && l.jit.jitterMs <= want + 1.0f, "a 300 ms network gap reads ~267 ms",
              l.jit.jitterMs);
        // ... and decays on wall time, 12 ms a second.
        const float at = l.jit.jitterMs;
        l.run(15400, 8);
        check(std::fabs((at - l.jit.jitterMs) - 120.0f) < 2.0f, "jitter decays 120 ms in 10 s", at - l.jit.jitterMs);
    }
    // A throttled client (bundles at 1.4x in a 2x room) decays just as fast: the servo slowing the
    // game must not slow its own recovery (the old per-bundle decay ran at 1.4/2 of the rate).
    {
        Link l; l.speedTenths = 20; l.sendTenths = 20; l.gapAt = 5000; l.gapMs = 800;
        l.run(5900, 8);
        const float at = l.jit.jitterMs;
        l.sendTenths = 14;
        l.run(15900, 8);
        check(std::fabs((at - l.jit.jitterMs) - 120.0f) < 2.0f, "throttled jitter decays 120 ms in 10 s",
              at - l.jit.jitterMs);
    }

    // The target never exceeds what flow control lets the buffer hold: the server leads the last
    // reported processed tick by flowLeadTicks, which trails the played tick by kProgressPeriod
    // plus the worker inbox, and the round trip passes (report out, bundles back) while the client
    // plays on.
    for (int sp = 1; sp <= tak::net::kMaxDevGameSpeed; ++sp)
        for (float rtt : {0.0f, 8.0f, 30.0f, 100.0f, 400.0f, 2000.0f})
            for (float jit : {0.0f, 50.0f, 370.0f, 750.0f, 1500.0f, 5000.0f})
                for (float frame : {4.0f, 8.3f, 33.0f, 60.0f, 250.0f})
                    for (int inbox : {0, 2}) {
                        const int t = tak::net::jitterTargetTicks(jit, rtt, frame, sp, inbox);
                        const uint32_t r = tak::net::progressRttMs(rtt);
                        const int rttTicks = int((uint64_t(r) * 3 * uint64_t(sp) + 999) / 1000);
                        const int reach = int(tak::net::flowLeadTicks(sp, r)) - tak::net::kProgressPeriod -
                                          rttTicks - inbox;
                        if (t < 2 || (t >= reach && t > 2)) {   // 2 is the floor even past a reach the speed cannot hold
                            std::fprintf(stderr, "FAIL: target %d vs reachable %d (speed %d rtt %.0f jitter %.0f frame %.1f inbox %d)\n",
                                         t, reach, sp, rtt, jit, frame, inbox);
                            ++failures;
                        }
                    }
    // The user's game: 750 ms of jitter at 2x on a local link. The old cap was 32 ticks against a
    // reachable ~19; now the target is what the buffer can hold.
    {
        const int t = tak::net::jitterTargetTicks(750.0f, 8.0f, 8.3f, 20, 2);
        check(t == 14, "750 ms jitter at 2x, 8 ms RTT: target 14 (22 lead - 4 - 1 rtt - 2 - 1)", t);
    }
    // An internet link: 100 ms RTT at 2x is 6 ticks each way round, a 32-tick lead holds ~20 in
    // hand, not the 25 a cap without the round trip asked for (the servo sat near its 0.7 floor).
    {
        const int t = tak::net::jitterTargetTicks(1500.0f, 100.0f, 8.3f, 20, 2);
        check(t == 19, "1500 ms jitter at 2x, 100 ms RTT: target 19 (32 lead - 4 - 6 rtt - 2 - 1)", t);
    }
    // Unchanged where the cap does not bite: a quiet local link at 1x and 8x.
    check(tak::net::jitterTargetTicks(0.0f, 8.0f, 8.3f, 10, 2) == 3, "quiet 1x target is 3",
          tak::net::jitterTargetTicks(0.0f, 8.0f, 8.3f, 10, 2));
    check(tak::net::jitterTargetTicks(0.0f, 8.0f, 50.0f, 80, 2) == 14, "8x at 50 ms frames: 2 + 12 frame ticks",
          tak::net::jitterTargetTicks(0.0f, 8.0f, 50.0f, 80, 2));
    check(tak::net::progressRttMs(0) == 0 && tak::net::progressRttMs(7.6f) == 8 && tak::net::progressRttMs(1e9f) == 60000,
          "progress RTT rounding");

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("pacing: all checks passed\n");
    return 0;
}
