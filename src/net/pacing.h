#pragma once

// Client jitter-buffer pacing math: the bundle-arrival jitter estimate (MpClient) and the
// buffer-depth target the playout servo holds (GameView::mpStep). Pure functions of their
// inputs, no clock and no I/O, so tools/pacing_test.cpp can drive them with scripted times.
// Pacing only: which bundles play, and in what order, never changes, so no sim state or hash
// depends on anything here.
//
// Why arrival is stamped where it is ("+10 stuck at +4"): stamped when the client POLLED, the
// client's own stall (alt-tab, a menu, the load gap of a watch game) would read as a second or
// more of network jitter; the target would then ask for more buffered ticks than the server's
// flow control lets a client hold (flowLeadTicks), the buffer could never reach it, and the
// playout would sit at its 0.7 floor -- 0.7 x 2.0 = 1.40x for a minute or more.
// Retail has no jitter buffer at all: its speed setter (0x4999b0) writes the requested and the
// effective level together and it throttles only on frame time and MP lag (0x526090).

#include "net/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tak::net {

// The round trip a client reports in Msg::Progress, which the server sizes its lead from.
inline uint32_t progressRttMs(float rttMs) {
    return uint32_t(std::min(rttMs > 0 ? rttMs + 0.5f : 0.0f, 60000.0f));
}

// Bundle-arrival jitter: how much later than the schedule at the current game speed bundles
// land, as a slowly-decaying recent max. This is what the jitter buffer must cover.
//
// A bundle is read at a poll, so all the client knows is that it arrived after the PREVIOUS
// poll (it was not there yet) and by this one. Its lateness is measured to the previous poll,
// the lower bound: a bundle that came in while the client itself was stalled is then never
// counted, since the poll before the stall had read everything up to it. A real network gap
// still is (the client kept polling and found nothing), short by at most one poll interval,
// which the target's frame term covers. The decay is on wall time, 12 ms per second (0.4 ms
// per 1x tick, as before at 1x), so a client the servo slows does not also slow its recovery.
struct ArrivalJitter {
    static constexpr float kDecayMsPerMs = 0.012f;
    uint64_t lastBundleMs = 0;   // when the previous bundle was read (0: none yet)
    uint64_t prevPollMs = 0;     // start of the previous poll
    uint64_t pollMs = 0;         // start of the current poll
    float jitterMs = 0;
    void beginPoll(uint64_t now) { prevPollMs = pollMs; pollMs = now; }
    void noteBundle(uint64_t now, int speedTenths) {
        if (lastBundleMs) {
            const float sp = float(std::max(1, speedTenths)) / 10.0f;
            const float intervalMs = 1000.0f / (30.0f * sp);
            const float late = prevPollMs > lastBundleMs ? float(prevPollMs - lastBundleMs) - intervalMs : 0.0f;
            const float decayed = jitterMs - kDecayMsPerMs * float(now > lastBundleMs ? now - lastBundleMs : 0);
            jitterMs = std::max({0.0f, late, decayed});
        }
        lastBundleMs = now;
    }
};

// The deepest buffer the client can actually hold: the server runs flowLeadTicks() past the
// processed tick we last reported, which trails the played tick by up to kProgressPeriod and by
// what sits in the sim worker's inbox, and by the round trip: the report takes half of it to
// reach the server and the bundles it releases the other half to come back, while the client
// keeps playing. One more tick of margin. A target past this can never be reached, and the
// servo would pin the playout at its floor (6% per missing tick, 0.7 at 5 short).
inline int flowBufferLimit(int speedTenths, float rttMs, int inboxCap) {
    const uint32_t rtt = progressRttMs(rttMs);
    const int lead = int(flowLeadTicks(speedTenths, rtt));
    const int sp = speedTenths > 0 ? speedTenths : 10;
    const int rttTicks = int((uint64_t(rtt) * 3 * uint64_t(sp) + 999) / 1000);   // as flowLeadTicks rounds it
    return std::max(2, lead - kProgressPeriod - rttTicks - inboxCap - 1);
}

// The jitter-buffer target in ticks at the current game speed (tickMs = 1000/(30*speed)): enough to
// cover the measured jitter, with a gentle RTT floor (1x ticks, as before) and at least one frame of
// arrivals in hand (a frame consumes up to frameMs/tickMs ticks at once; sub-tick frames add nothing).
// Clamped to [2, max(16, 16*speed)] and to what flow control lets the buffer reach.
inline int jitterTargetTicks(float jitterMs, float rttMs, float frameMs, int speedTenths, int inboxCap) {
    const float sp = float(std::max(1, speedTenths)) / 10.0f;
    const float tickMs = 1000.0f / (30.0f * sp);
    const int kJit = int(std::ceil(jitterMs / tickMs));
    const int kRtt = int(std::ceil(rttMs / 60.0f));
    const int kFrame = frameMs > tickMs ? int(std::ceil(frameMs / tickMs)) : 0;
    const int cap = std::min(std::max(16, int(16.0f * sp)), flowBufferLimit(speedTenths, rttMs, inboxCap));
    return std::clamp(2 + std::max({kJit, kRtt, kFrame}), 2, cap);
}

}  // namespace tak::net
