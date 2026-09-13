// Does the command path lose orders, or reorder them?
//
// The pacing rule is shared (net::cmdSendCredit) and the server's side of it is a
// bounded FIFO drained kCmdCapPerTick per tick. This drives that pair over a range
// of frame rates and uplink batching, with bursts far larger than one tick's
// budget, and asserts the two properties that matter to a player: every command
// arrives, and it arrives in the order it was issued.
//
// SCOPE, stated plainly: this exercises the shared credit function and a faithful
// model of the server's queue policy -- it is not the server binary. It pins the
// POLICY. Three successive versions of this pacing were wrong in ways a policy
// test would have caught immediately (whole outbox per frame, one batch per frame,
// then one batch per tick), which is why it exists.
#include "net/protocol.h"

#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace tak;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
    if (!ok) ++fails;
}

struct Result { int sent = 0, got = 0, dropped = 0; bool ordered = true; };

// fps: client render steps per second, against a 30Hz sim tick.
// stallAt/stallFor: a delivery BLACKOUT -- nothing reaches the server for that many
//   ticks, then everything held is released on ONE tick. The first version of this
//   test added a constant offset to each message's delivery time instead, which is
//   latency, not batching: it shifted the stream while preserving its spacing, so
//   it never released a burst and could not fail. That is precisely the case the
//   rate limiter alone could not survive.
// burst: commands issued at once (a big selection order).
// windowed: apply the in-flight window (the fix) as well as the rate credit.
// frameCoupled models the OLD rule -- one kCmdCapPerTick batch per RENDER STEP,
// no credit for elapsed ticks -- so the test can show it catches what it claims to.
static Result run(int fps, int stallAt, int stallFor, int burst, int ticks,
                  bool frameCoupled = false, bool windowed = true) {
    Result r;
    std::deque<int> outbox;                 // client-side, ids in issue order
    for (int i = 0; i < burst; ++i) outbox.push_back(i);
    int nextId = burst;

    int credit = net::kCmdCapPerTick;
    uint32_t lastSendTick = 0;
    std::deque<std::pair<int, std::vector<int>>> wire;   // (deliverTick, commands)
    int inFlight = 0;
    std::deque<int> serverQueue;
    std::vector<int> received;

    double stepsPerTick = double(fps) / 30.0;
    double stepAcc = 0;
    for (uint32_t t = 0; t < uint32_t(ticks); ++t) {
        // A trickle of ordinary orders on top of the burst, so the test is not
        // purely a drain of one block.
        if (t % 10 == 0) outbox.push_back(nextId++);

        // Client render steps that fall inside this tick.
        for (stepAcc += stepsPerTick; stepAcc >= 1.0; stepAcc -= 1.0) {
            if (frameCoupled) {
                credit = (t != lastSendTick) ? net::kCmdCapPerTick : 0;
                lastSendTick = t;
            } else if (t != lastSendTick) {
                credit = net::cmdSendCredit(credit, t - lastSendTick);
                lastSendTick = t;
            }
            const int sendable = windowed ? net::cmdSendWindow(credit, inFlight) : credit;
            if (outbox.empty() || sendable <= 0) continue;
            const int n = int(outbox.size()) < sendable ? int(outbox.size()) : sendable;
            std::vector<int> msg;
            for (int i = 0; i < n; ++i) { msg.push_back(outbox.front()); outbox.pop_front(); }
            credit -= n;
            inFlight += n;
            r.sent += n;
            // Held for the whole blackout, then released together on one tick.
            const bool stalled = stallFor > 0 && int(t) >= stallAt && int(t) < stallAt + stallFor;
            wire.push_back({stalled ? stallAt + stallFor : int(t), std::move(msg)});
        }

        // Uplink delivery (several messages can land together after a stall).
        while (!wire.empty() && wire.front().first <= int(t)) {
            for (int id : wire.front().second) {
                if (serverQueue.size() >= size_t(net::kCmdQueueCap)) { ++r.dropped; continue; }
                serverQueue.push_back(id);
            }
            wire.pop_front();
        }

        // Server drain: FIFO, one budget per tick. The drained commands go into the
        // tick bundle the server broadcasts, which is the client's acknowledgement.
        for (int taken = 0; taken < net::kCmdCapPerTick && !serverQueue.empty(); ++taken) {
            received.push_back(serverQueue.front());
            serverQueue.pop_front();
            if (inFlight > 0) --inFlight;
        }
    }
    r.got = int(received.size());
    for (size_t i = 1; i < received.size(); ++i)
        if (received[i] < received[i - 1]) { r.ordered = false; break; }
    return r;
}

int main() {
    std::printf("[command flow: every order arrives, in order]\n");
    // 10fps is the case the frame-coupled versions failed: fewer render steps than
    // ticks, so throughput collapsed even though the server was willing to take more.
    for (int fps : {5, 10, 30, 60, 120, 240}) {
        const int burst = 2000, ticks = 400;
        Result r = run(fps, 0, 0, burst, ticks);
        check(r.dropped == 0 && r.ordered && r.got == r.sent,
              "fps=" + std::to_string(fps) + ": nothing lost, order held",
              "sent=" + std::to_string(r.sent) + " delivered=" + std::to_string(r.got) +
                  " dropped=" + std::to_string(r.dropped) +
                  (r.ordered ? "" : " ORDER BROKEN"));
    }
    // The real case: a delivery BLACKOUT, then every held message released on one
    // tick. The rate limiter alone cannot survive this -- it re-accrues through the
    // stall and hands the server more than its queue holds.
    for (int stall : {4, 8, 16, 32}) {
        // The blackout has to start while the order is still FLOWING -- begin it
        // after the burst has drained and the client has nothing to send, and the
        // stall proves nothing (the first cut of this test did exactly that).
        Result r = run(60, 2, stall, 4000, 500);
        check(r.dropped == 0 && r.ordered && r.got == r.sent,
              "uplink blackout of " + std::to_string(stall) +
                  " ticks then a burst release: nothing lost",
              "sent=" + std::to_string(r.sent) + " delivered=" + std::to_string(r.got) +
                  " dropped=" + std::to_string(r.dropped) +
                  (r.ordered ? "" : " ORDER BROKEN"));
        // Control: the same blackout WITHOUT the in-flight window must lose
        // commands, or this test is not measuring the fix.
        // Only assert the counter-case where a stall can actually overrun the
        // queue: below that the rate limiter alone is enough, and demanding a
        // failure there would be asserting a bug rather than a property.
        if (stall * net::kCmdCapPerTick > net::kCmdQueueCap) {
            Result un = run(60, 2, stall, 4000, 500, false, false);
            check(un.dropped > 0,
                  "...and the rate limiter alone loses them (counter-case)",
                  "unwindowed dropped=" + std::to_string(un.dropped) +
                      ", windowed dropped 0");
        }
    }
    // Throughput floor: a low-fps client must still clear a big order about as fast
    // as the server can take it, not fps*64 per second.
    {
        Result lo = run(5, 0, 0, 4000, 120), hi = run(120, 0, 0, 4000, 120);
        check(lo.got * 100 >= hi.got * 90,
              "throughput does not track frame rate",
              "5fps delivered " + std::to_string(lo.got) + ", 120fps " +
                  std::to_string(hi.got) + " in 4s");
        // Control: the rule this replaced MUST fail that check, else the check is
        // measuring nothing.
        Result oldLo = run(5, 0, 0, 4000, 120, true), oldHi = run(120, 0, 0, 4000, 120, true);
        check(oldLo.got * 100 < oldHi.got * 90,
              "...and the frame-coupled rule it replaced would fail that",
              "old rule: 5fps " + std::to_string(oldLo.got) + " vs 120fps " +
                  std::to_string(oldHi.got));
    }
    std::printf("\n%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
