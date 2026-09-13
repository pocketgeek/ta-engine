// conn_test -- regression tests for Conn::recv() around peer close.
//
// The bug: recv() looped reading into rxBuf_, and on EOF returned false immediately.
// A peer that writes its last message and closes usually lands the payload and the FIN
// in the same segment, so that one call had ALREADY buffered a complete frame -- and
// both callers bail out on a false return before their poll() drain. The final message
// was silently dropped. On the server that turned a deliberate LeaveGame into a
// disconnect (slot held for the whole grace period); on the client it threw away a
// Reject that the server had spelled out, leaving a bare "peer closed".
//
// These tests drive Conn over a socketpair, which reproduces the exact "data + FIN
// already queued" condition deterministically: everything is written and the write end
// closed BEFORE Conn ever reads, so the first recv() sees payload then EOF.

#include "net/conn.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
int main() { std::printf("conn_test: skipped (no socketpair on Windows)\n"); return 0; }
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using tak::net::Conn;
using tak::net::Frame;
using tak::net::Msg;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  %-62s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}

// Serialize one frame the way Conn::send does: u32 length (kind + payload), kind, bytes.
static std::vector<uint8_t> frameBytes(Msg kind, const std::string& payload) {
    std::vector<uint8_t> b;
    uint32_t len = uint32_t(1 + payload.size());
    b.push_back(uint8_t(len & 0xff));
    b.push_back(uint8_t((len >> 8) & 0xff));
    b.push_back(uint8_t((len >> 16) & 0xff));
    b.push_back(uint8_t((len >> 24) & 0xff));
    b.push_back(uint8_t(kind));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

// Write `bytes` into one end of a socketpair, optionally close it, and hand back a Conn
// wrapping the other end.
struct Pair {
    int peer = -1;
    Conn conn;
};
static Pair makePair(const std::vector<uint8_t>& bytes, bool closePeer) {
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::perror("socketpair");
        std::exit(2);
    }
    if (!bytes.empty()) {
        ssize_t n = ::write(sv[1], bytes.data(), bytes.size());
        if (n != ssize_t(bytes.size())) { std::perror("write"); std::exit(2); }
    }
    if (closePeer) { ::close(sv[1]); sv[1] = -1; }
    // Conn::recv() drains until the socket would block, which on a socketpair means it
    // must be NON-BLOCKING -- Conn::connect() does this itself, but the fd constructor
    // takes whatever it is handed. Without it the still-open case blocks forever.
    int fl = ::fcntl(sv[0], F_GETFL, 0);
    if (fl < 0 || ::fcntl(sv[0], F_SETFL, fl | O_NONBLOCK) != 0) {
        std::perror("fcntl O_NONBLOCK");
        std::exit(2);
    }
    Pair p;
    p.peer = sv[1];
    p.conn = Conn(sv[0]);
    return p;
}

int main() {
    std::printf("conn_test\n");

    // THE REGRESSION, modelled the way the CALLERS actually drive Conn. This is the
    // check that matters: both callers are `if (!recv()) { drop; continue; }` followed by
    // a poll() drain, so the defect is not that the bytes were unreachable -- it is that
    // the drain was never reached. A test that just calls poll() directly passes even
    // against the bug (it does, verified); this one does not.
    {
        std::printf("the caller's own loop (recv -> drop-or-drain):\n");
        Pair p = makePair(frameBytes(Msg::LeaveGame, "deliberate"), /*closePeer=*/true);
        std::vector<Frame> delivered;
        bool dropped = false;
        if (!p.conn.recv()) {
            dropped = true;              // caller bails out here -- drain never runs
        } else {
            Frame f;
            while (p.conn.poll(f)) delivered.push_back(f);
            if (p.conn.peerClosed()) dropped = true;
        }
        check(delivered.size() == 1,
              "the final LeaveGame reached the caller (was lost: drain skipped)");
        check(delivered.size() == 1 && delivered[0].kind == Msg::LeaveGame,
              "and it is a LeaveGame, so the departure reads as deliberate");
        check(dropped, "the peer is still dropped afterwards -- the close is not swallowed");
    }

    // THE REGRESSION: a complete frame followed by a clean close, both already queued.
    // recv() must report success so the caller reaches its drain, the frame must survive,
    // and the close must still be visible afterwards so the connection is finished off.
    {
        std::printf("final message arriving with the FIN:\n");
        Pair p = makePair(frameBytes(Msg::LeaveGame, "bye-now"), /*closePeer=*/true);
        bool r = p.conn.recv();
        check(r, "recv() returns true on a clean close (was false: drain skipped)");
        Frame f;
        bool got = p.conn.poll(f);
        check(got, "the buffered final frame is still readable (this was the lost one)");
        check(got && f.kind == Msg::LeaveGame, "it is the frame that was sent");
        check(got && std::string(f.payload.begin(), f.payload.end()) == "bye-now",
              "its payload survived intact");
        check(!p.conn.poll(f), "no second frame is invented");
        check(p.conn.peerClosed(), "peerClosed() reports the close after the drain");
    }

    // Several frames in the final segment: ALL of them must survive, not just the first.
    {
        std::printf("multiple frames in the final segment:\n");
        std::vector<uint8_t> b = frameBytes(Msg::StateHash, "one");
        for (const char* s : {"two", "three"}) {
            std::vector<uint8_t> n = frameBytes(Msg::StateHash, s);
            b.insert(b.end(), n.begin(), n.end());
        }
        Pair p = makePair(b, true);
        check(p.conn.recv(), "recv() succeeds");
        std::vector<std::string> got;
        Frame f;
        while (p.conn.poll(f)) got.emplace_back(f.payload.begin(), f.payload.end());
        check(got.size() == 3, "all three frames drained");
        check(got.size() == 3 && got[0] == "one" && got[1] == "two" && got[2] == "three",
              "in order, with payloads intact");
        check(p.conn.peerClosed(), "close still reported");
    }

    // COUNTER-CASE: a close with nothing buffered must still be a close. If this passed
    // trivially the tests above would prove nothing about ordering.
    {
        std::printf("close with no data (must not become a silent no-op):\n");
        Pair p = makePair({}, true);
        check(p.conn.recv(), "recv() returns true (the close is reported separately)");
        Frame f;
        check(!p.conn.poll(f), "no frame is produced");
        check(p.conn.peerClosed(), "peerClosed() is set, so the caller drops the peer");
    }

    // COUNTER-CASE: a TRUNCATED frame plus close must not yield a frame. Proves poll()
    // is still validating completeness rather than handing back whatever is buffered.
    {
        std::printf("truncated frame then close:\n");
        std::vector<uint8_t> b = frameBytes(Msg::LeaveGame, "abcdefgh");
        b.resize(b.size() - 3);   // chop the tail: header promises more than arrived
        Pair p = makePair(b, true);
        check(p.conn.recv(), "recv() succeeds");
        Frame f;
        check(!p.conn.poll(f), "the incomplete frame is NOT surfaced");
        check(p.conn.peerClosed(), "close reported, so the caller will drop the peer");
    }

    // An open socket with a complete frame must NOT look closed -- otherwise the fix
    // would terminate healthy connections.
    {
        std::printf("live connection (still open):\n");
        Pair p = makePair(frameBytes(Msg::StateHash, "live"), /*closePeer=*/false);
        check(p.conn.recv(), "recv() succeeds");
        Frame f;
        check(p.conn.poll(f), "frame read");
        check(!p.conn.peerClosed(), "peerClosed() is FALSE while the peer is still there");
        if (p.peer >= 0) ::close(p.peer);
    }

    std::printf(g_fail ? "conn_test: %d FAILURE(S)\n" : "conn_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
#endif
