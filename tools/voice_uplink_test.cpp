// voice_uplink_test -- a talker's own voice must not hold up its game traffic (protocol
// 276). Over real loopback TCP, so it runs on Windows too, where the user plays and where
// the kernel's send queue was once unknowable.
//
//  - Conn::kernelQueued() answers on every platform (Windows: TCP_INFO and the bytes
//    send() accepted): 0 on an idle connection, kLowKernelMax or more while the peer does
//    not read. Voice waits on it and goes once the peer has read; where it cannot answer,
//    voice is dropped, never sent ungated.
//  - A talker on an uplink too slow for its voice: a real MpClient transmits (a frame every
//    20 ms) and reports Progress every 4 ticks to a scripted server that reads 2500 bytes a
//    second (a 4 KiB receive buffer). Before the uplink guard, Windows let its voice fill
//    Winsock's 64 KiB send buffer ahead of every Progress (5.5 s of delay after 15 s, 22 s
//    after 60 s), and Linux still queued up to ~2.5 s in the receiver's buffers. Now its
//    round-trip probe sees the queue and holds voice (an END closes the transmission, a
//    START opens the next one): no Progress waits a second, and voice still went out.
//  - On a fast link nothing is held and every frame arrives -- also when the talker's own
//    machine stutters (a 180 ms frame every 0.7 s, a 250 ms one every 0.5 s). The probe's
//    Pong is read once a frame, so a long frame after the Ping used to count as round trip:
//    the guard held a stuttering talker's voice for 54 s of every 60 on a fast link. And a
//    slow uplink is still held when the machine stutters too.
#include "net/client.h"
#include "net/conn.h"
#include "net/netcompat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace tak;

namespace {
int checks = 0, failures = 0;
void check(bool value, const char* what) {
    ++checks;
    std::printf("  %-74s %s\n", what, value ? "ok" : "FAIL");
    if (!value) ++failures;
}

uint64_t nowUs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int listener(uint16_t& port, int rcvbuf = 0) {
    std::string error;
    const int fd = net::listenOn(0, error, true);
    if (fd < 0) throw std::runtime_error(error);
    if (rcvbuf) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof rcvbuf);
    sockaddr_storage address{}; socklen_t n = sizeof address;
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &n)) throw std::runtime_error("getsockname");
    port = address.ss_family == AF_INET ? ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port)
                                        : ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    return fd;
}

int acceptOne(int fd, const std::function<void()>& pump = {}) {
    const uint64_t deadline = nowUs() + 3000000;
    while (nowUs() < deadline) {
        if (pump) pump();
        int peer = int(::accept(fd, nullptr, nullptr));
        if (peer >= 0) { net::setupSocket(peer); return peer; }
        sleepMs(1);
    }
    throw std::runtime_error("timed out accepting");
}

// Read whatever the socket has (non-blocking); returns the byte count.
size_t drainSocket(int fd, std::vector<uint8_t>* keep = nullptr) {
    size_t total = 0;
    char buf[65536];
    for (;;) {
        const int n = int(::recv(fd, buf, sizeof buf, 0));
        if (n <= 0) break;
        total += size_t(n);
        if (keep) keep->insert(keep->end(), buf, buf + n);
    }
    return total;
}

// ---- 1. Conn: the kernel's queue, and voice waiting on it ----------------------------
void kernelGate() {
    std::printf("the kernel's send queue (Conn::kernelQueued):\n");
    uint16_t port = 0;
    const int lfd = listener(port);
    net::Conn c;
    if (!c.connect("127.0.0.1", port, 3000)) throw std::runtime_error("connect: " + c.error());
    const int peer = acceptOne(lfd);
    long q = -1;
    for (int i = 0; i < 100 && (q = c.kernelQueued()) != 0; ++i) sleepMs(10);
    check(q == 0, "an idle connection: the kernel holds nothing (and can say so)");

    // The peer reads nothing: game bytes go out 1 KiB at a time, each taken whole, until the
    // peer's buffers are full and the kernel holds some of them itself (on Windows loopback
    // that happens all at once at the end, so a bigger frame would never be taken whole).
    const std::vector<uint8_t> chunk(1019, 7);
    size_t queued = 0;
    std::vector<uint8_t> got;
    for (int i = 0; i < 40000 && c.txPending() == 0 && c.kernelQueued() < net::Conn::kLowKernelMax; ++i) {
        c.send(net::Msg::TickBundle, chunk);
        c.flushWrite();
        queued += chunk.size() + 5;
    }
    // A last frame only partly taken: let the rest out, a little read at a time.
    for (int i = 0; i < 5000 && c.txPending() > 0; ++i) {
        char buf[256];
        const int n = int(::recv(peer, buf, sizeof buf, 0));
        if (n > 0) got.insert(got.end(), buf, buf + n);
        c.flushWrite();
        sleepMs(1);
    }
    q = c.kernelQueued();
    std::printf("  (kernel holds %ld bytes)\n", q);
    check(c.txPending() == 0 && q >= net::Conn::kLowKernelMax, "the kernel reports the unread bytes it holds");
    const std::vector<uint8_t> held = {'h', 'e', 'l', 'd'};
    c.sendLow(net::Msg::VoiceDown, held);
    c.flushWrite();
    check(c.lowPending() == 1 && c.lowWaiting() && !c.wantWrite(), "voice waits while the kernel holds them");
    // The peer reads everything; the queue drains and the voice goes, last.
    const uint64_t deadline = nowUs() + 5000000;
    while (nowUs() < deadline && (got.size() < queued || c.lowPending())) {
        drainSocket(peer, &got);
        c.flushWrite();
        sleepMs(1);
    }
    drainSocket(peer, &got);
    sleepMs(20);
    drainSocket(peer, &got);
    const bool last = got.size() == queued + 9 && std::memcmp(got.data() + queued + 5, "held", 4) == 0 &&
                      got[queued + 4] == uint8_t(net::Msg::VoiceDown);
    check(c.lowPending() == 0 && last, "once the peer has read them the voice follows, after every game byte");
    for (int i = 0; i < 100 && (q = c.kernelQueued()) != 0; ++i) sleepMs(10);
    check(q == 0, "and the queue reads 0 again");
    c.closeNow();
    net::sockClose(peer);

    // A Conn whose socket is gone cannot ask: its voice is dropped, not sent.
    net::Conn dead;
    dead.sendLow(net::Msg::VoiceDown, held);
    dead.flushWrite();
    check(dead.kernelQueued() < 0 && dead.lowPending() == 0 && dead.lowDropped() == 1,
          "with no answer from the kernel voice is dropped (fail closed)");
    net::sockClose(lfd);
}

// ---- 2. MpClient: a talker's uplink ----------------------------------------------------
struct Uplink {
    int listenFd = -1, peer = -1;
    uint16_t port = 0;
    net::Conn conn;              // the handshake side of the scripted server
    std::atomic<bool> stop{false};
    std::thread reader;
    std::mutex m;
    std::map<uint32_t, uint64_t> progressSent;   // tick -> send time (us)
    std::vector<double> delays;                   // ms, Progress send -> read
    std::vector<std::pair<uint64_t, uint8_t>> voice;   // (read time, flags) of each VoiceUp
    std::atomic<int> pings{0};                    // Pings read
    explicit Uplink(int rcvbuf) { listenFd = listener(port, rcvbuf); }
    ~Uplink() {
        stop = true;
        if (reader.joinable()) reader.join();
        if (listenFd >= 0) net::sockClose(listenFd);
    }
    // Answer the client's requests (as voice_client_test's server does) until it is seated.
    void handshake(net::MpClient& client) {
        peer = acceptOne(listenFd, [&] { client.poll(); });
        conn = net::Conn(peer);
        auto expect = [&](net::Msg kind) {
            const uint64_t deadline = nowUs() + 3000000;
            while (nowUs() < deadline) {
                client.poll();
                if (!conn.recv()) throw std::runtime_error("server receive");
                net::Frame f;
                while (conn.poll(f)) {
                    if (f.kind == net::Msg::Ping) { conn.send(net::Msg::Pong); conn.flushWrite(); continue; }
                    if (f.kind == kind) return;
                }
                sleepMs(1);
            }
            throw std::runtime_error("timed out waiting for a client request");
        };
        auto answer = [&](net::Msg kind, const net::Writer& w) {
            conn.send(kind, w);
            conn.flushWrite();
            for (int i = 0; i < 200; ++i) { client.poll(); sleepMs(1); }
        };
        expect(net::Msg::Hello);
        { net::Writer w; w.u32(5); w.str("Talker"); answer(net::Msg::Welcome, w); }
        client.joinGame(12, "");
        expect(net::Msg::JoinGame);
        { net::Writer w; w.u8(1); w.u8(1); w.str(""); answer(net::Msg::JoinResult, w); }
    }
    // From here on the server reads at `rate` bytes a second (0 = as fast as it comes),
    // answers Pings as it reaches them, and times each Progress.
    void startReading(double rate) {
        reader = std::thread([this, rate] {
            std::vector<uint8_t> buf;
            double credit = 0;
            uint64_t last = nowUs();
            while (!stop) {
                const uint64_t n = nowUs();
                credit = rate <= 0 ? 65536.0 : std::min(256.0, credit + double(n - last) / 1e6 * rate);
                last = n;
                if (credit < 64) { sleepMs(2); continue; }
                char tmp[65536];
                const int r = int(::recv(peer, tmp, int(std::min(credit, double(sizeof tmp))), 0));
                if (r <= 0) { sleepMs(2); continue; }
                credit -= r;
                buf.insert(buf.end(), tmp, tmp + r);
                size_t off = 0;
                while (buf.size() - off >= 5) {
                    const uint32_t len = uint32_t(buf[off]) | uint32_t(buf[off + 1]) << 8 |
                                         uint32_t(buf[off + 2]) << 16 | uint32_t(buf[off + 3]) << 24;
                    if (buf.size() - off - 4 < len) break;
                    const net::Msg kind = net::Msg(buf[off + 4]);
                    const uint8_t* p = buf.data() + off + 5;   // may be one past the end (a bare Ping)
                    const uint64_t at = nowUs();
                    if (kind == net::Msg::Ping) {
                        const uint8_t pong[5] = {1, 0, 0, 0, uint8_t(net::Msg::Pong)};
                        ::send(peer, reinterpret_cast<const char*>(pong), 5, MSG_NOSIGNAL);
                        ++pings;
                    } else if (kind == net::Msg::Progress && len >= 5) {
                        uint32_t tick; std::memcpy(&tick, p, 4);
                        std::lock_guard<std::mutex> lock(m);
                        auto it = progressSent.find(tick);
                        if (it != progressSent.end()) delays.push_back(double(at - it->second) / 1000.0);
                    } else if (kind == net::Msg::VoiceUp && len >= 5) {
                        std::lock_guard<std::mutex> lock(m);
                        voice.push_back({at, p[3]});
                    }
                    off += 4 + len;
                }
                buf.erase(buf.begin(), buf.begin() + long(off));
            }
        });
    }
};

struct TalkResult {
    double p50 = 0, max = 0;
    size_t sent = 0, frames = 0, starts = 0, ends = 0;
    bool everHeld = false;
};

// Talk for `seconds`: a voice frame every 20 ms, a Progress every 4 ticks (133 ms), the
// client polled every 5 ms, as a game frame loop does -- or, with hitchMs, about one frame
// in every hitchEveryMs taking that long instead (the captured audio piles up meanwhile and
// goes out in a burst, as the capture queue does).
TalkResult talk(net::MpClient& client, Uplink& up, double seconds, int hitchMs = 0, int hitchEveryMs = 0,
                bool afterProbe = false) {
    TalkResult res;
    const uint64_t t0 = nowUs(), end = t0 + uint64_t(seconds * 1e6);
    uint64_t nextVoice = t0, nextProgress = t0, nextHitch = t0 + uint64_t(hitchEveryMs) * 1000;
    uint16_t seq = 0;
    uint32_t tick = 0;
    const std::vector<uint8_t> opus(66, 0x5a);
    while (nowUs() < end && client.state() == net::MpClient::State::InRoom) {
        const uint64_t n = nowUs();
        while (n >= nextVoice) {
            nextVoice += 20000;
            client.sendVoice(seq, 0xff, seq == 0 ? net::kVoiceStart : 0, opus);
            ++seq;
        }
        while (n >= nextProgress) {
            nextProgress += 133333;
            tick += 4;
            { std::lock_guard<std::mutex> lock(up.m); up.progressSent[tick] = nowUs(); }
            client.sendProgress(tick);
        }
        const int pings = up.pings.load();
        client.poll();
        res.everHeld = res.everHeld || client.voiceHeld();
        sleepMs(5);
        // On a fast link the long frame comes where it does the most harm: right after a
        // poll that sent a probe (the server has read it), so the read that finds its Pong
        // comes hitchMs late. A slow link reads the probe late, so there it comes on time.
        if (hitchMs && n >= nextHitch) {
            const uint64_t until = nowUs() + 30000;
            while (afterProbe && up.pings.load() == pings && nowUs() < until) sleepMs(1);
            if (!afterProbe || up.pings.load() != pings) {
                nextHitch = nowUs() + uint64_t(hitchEveryMs) * 1000;
                sleepMs(hitchMs);
            }
        }
    }
    res.sent = seq;
    client.sendVoice(seq, 0xff, net::kVoiceEnd, {});
    for (int i = 0; i < 300; ++i) { client.poll(); sleepMs(5); }   // let the queue drain
    std::lock_guard<std::mutex> lock(up.m);
    std::vector<double> d = up.delays;
    std::sort(d.begin(), d.end());
    if (!d.empty()) { res.p50 = d[d.size() / 2]; res.max = d.back(); }
    res.frames = up.voice.size();
    for (auto& v : up.voice) {
        if (v.second & net::kVoiceStart) ++res.starts;
        if (v.second & net::kVoiceEnd) ++res.ends;
    }
    return res;
}

void slowUplink(int hitchMs = 0, int hitchEveryMs = 0) {
    if (hitchMs)
        std::printf("a talker on an uplink too slow for its voice (2500 B/s), a %d ms frame every %d ms:\n",
                    hitchMs, hitchEveryMs);
    else
        std::printf("a talker on an uplink too slow for its voice (2500 B/s):\n");
    Uplink up(4096);
    net::MpClient client;
    check(client.connect("127.0.0.1", up.port, "Talker"), "connect");
    up.handshake(client);
    check(client.state() == net::MpClient::State::InRoom, "seated");
    up.startReading(2500);
    const TalkResult r = talk(client, up, 12.0, hitchMs, hitchEveryMs);
    std::printf("  Progress delay p50 %.0f ms, max %.0f ms; voice frames read %zu (START %zu, END %zu); held %llu frames\n",
                r.p50, r.max, r.frames, r.starts, r.ends, (unsigned long long)client.voiceHeldFrames());
    check(r.max < 1000.0, "no Progress waits a second behind the talker's voice");
    check(r.everHeld && client.voiceHeldFrames() > 0, "the uplink guard held voice");
    check(r.frames >= 25, "voice still went out while the link had room");
    check(r.ends >= 2 && r.starts >= 2, "the hold closed the transmission (END) and the next one opened (START)");
    client.disconnect("done");
}

void fastLink() {
    std::printf("a talker on a fast link:\n");
    Uplink up(0);
    net::MpClient client;
    check(client.connect("127.0.0.1", up.port, "Talker"), "connect");
    up.handshake(client);
    up.startReading(0);
    const TalkResult r = talk(client, up, 3.0);
    std::printf("  Progress delay p50 %.0f ms, max %.0f ms; voice frames read %zu\n", r.p50, r.max, r.frames);
    check(!r.everHeld && client.voiceHeldFrames() == 0, "nothing is held");
    check(r.frames >= 140 && r.starts == 1 && r.ends == 1, "every frame arrives, one START and one END");
    client.disconnect("done");
}

// The talker's machine stutters on a fast link: a long frame between the Ping and the read
// that finds its Pong is the client's own delay, not queueing, and must not hold voice.
void fastLinkHitches(int hitchMs, int hitchEveryMs) {
    std::printf("a talker on a fast link whose machine stutters (a %d ms frame every %d ms):\n", hitchMs, hitchEveryMs);
    Uplink up(0);
    net::MpClient client;
    check(client.connect("127.0.0.1", up.port, "Talker"), "connect");
    up.handshake(client);
    up.startReading(0);
    const TalkResult r = talk(client, up, 6.0, hitchMs, hitchEveryMs, true);
    std::printf("  Progress delay p50 %.0f ms, max %.0f ms; voice frames sent %zu, read %zu; held %llu frames\n",
                r.p50, r.max, r.sent, r.frames, (unsigned long long)client.voiceHeldFrames());
    check(!r.everHeld && client.voiceHeldFrames() == 0, "nothing is held");
    check(r.frames == r.sent + 1 && r.starts == 1 && r.ends == 1, "every frame arrives, one START and one END");
    client.disconnect("done");
}
}  // namespace

int main() {
    net::netStartup();
    try {
        kernelGate();
        fastLink();
        fastLinkHitches(180, 700);
        fastLinkHitches(250, 500);
        slowUplink();
        slowUplink(180, 700);
    } catch (const std::exception& e) {
        std::printf("  FAIL %s\n", e.what()); ++failures;
    }
    std::printf("voice_uplink: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
