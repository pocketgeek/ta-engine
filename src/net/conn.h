#pragma once

// A non-blocking, buffered TCP connection with the protocol's length-prefix
// framing. Shared by takserver and the takclient client. One Conn per socket.

#include <cstdint>
#include <deque>
#include <memory>
#include "net/tls.h"
#include <string>
#include <vector>

#include "net/protocol.h"

namespace tak::net {

// One received message: kind + payload bytes (the payload excludes the kind byte).
struct Frame {
    Msg kind;
    std::vector<uint8_t> payload;
};

class Conn {
public:
    Conn() = default;
    explicit Conn(int fd, std::shared_ptr<TlsContext> tls = {});
    ~Conn();
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;
    Conn(Conn&& o) noexcept { *this = std::move(o); }
    Conn& operator=(Conn&& o) noexcept;

    // Client: resolve host (IPv4/IPv6/hostname via getaddrinfo) and connect,
    // blocking up to timeoutMs. Returns false and sets error() on failure.
    bool connect(const std::string& host, uint16_t port, int timeoutMs = 5000);

    int fd() const { return fd_; }
    bool encrypted() const { return tls_ != nullptr; }   // this socket speaks TLS
    bool ok() const { return fd_ >= 0 && err_.empty(); }
    const std::string& error() const { return err_; }
    void fail(const std::string& why) { if (err_.empty()) err_ = why; }
    void closeNow();

    // Queue a framed message (kind + payload) into the send buffer.
    void send(Msg kind, const std::vector<uint8_t>& payload);
    void send(Msg kind, const Writer& w) { send(kind, w.b); }
    void send(Msg kind) { send(kind, std::vector<uint8_t>{}); }
    // Queue a LOW-PRIORITY frame (voice). It waits in its own small queue and joins the
    // byte stream only when every ordinary frame queued so far has left this program AND
    // the kernel holds less than kLowKernelMax bytes for this peer (unsent or not yet
    // acknowledged, see kernelQueued()), and then at most kLowPromoteBytes at a time. So
    // it is never queued ahead of a tick bundle or a command here, and a bundle queued
    // later finds at most a few KiB of voice ahead of it in the kernel, whatever the link
    // speed: voice takes only the capacity the game leaves. Where the kernel cannot say
    // (an old Windows, see kernelQueued()) the voice is dropped: it never goes out ungated.
    // It is dropped, never buffered without bound, when this peer is not keeping up: the
    // newest kLowMaxFrames survive, and nothing is queued while kLowBackpressure bytes of
    // ordinary frames wait. Returns false when the frame was dropped on arrival.
    static constexpr size_t kLowMaxFrames = 16;          // 320 ms of voice frames
    static constexpr size_t kLowBackpressure = 64u << 10;
    static constexpr long kLowKernelMax = 2048;
    static constexpr size_t kLowPromoteBytes = 1024;
    bool sendLow(Msg kind, const std::vector<uint8_t>& payload);
    // Throw away the queued low-priority frames (the server stops voice to a lagging peer).
    void dropLow() { lowDropped_ += low_.size(); low_.clear(); lowGated_ = false; }
    // Low-priority frames dropped so far (on arrival or as the oldest of a full queue).
    uint64_t lowDropped() const { return lowDropped_; }
    size_t lowPending() const { return low_.size(); }
    // Low-priority frames are waiting for the kernel's queue to drain. Writability says
    // nothing about that, so wantWrite() leaves them out and the caller must flushWrite()
    // again soon on its own (the server caps its poll timeout while this holds).
    bool lowWaiting() const { return lowGated_ && !low_.empty(); }
    // Bytes the kernel holds for this peer (unsent, or sent and not yet acknowledged), or
    // -1 where it cannot say. See the free kernelQueued() below.
    long kernelQueued() const;

    // Non-blocking I/O against the ready socket:
    //  recv(): read available bytes into rxBuf_; false on ERROR only -- a clean peer
    //          close returns true and sets peerClosed(), so the caller still drains the
    //          frames that arrived with the FIN.
    //  poll(): pop the next complete frame (returns false if none buffered yet).
    //  flushWrite(): push queued bytes out; false on error. wantWrite() true
    //  while bytes remain (register POLLOUT).
    bool recv(size_t budget=1u<<20);
    // Did the peer send EOF? recv() returns TRUE on a clean close so the caller can
    // still drain whatever complete frames were buffered alongside the FIN; call this
    // AFTER the poll() drain to finish the connection off. See recv() for why.
    bool peerClosed() const { return peerClosed_; }
    // Did the last recv() take everything the socket had (it stopped on an empty socket or
    // the close, not on its budget)? Then a frame not buffered yet had not arrived when it ran.
    bool recvDrained() const { return rxDrained_; }
    bool poll(Frame& out);
    bool flushWrite();
    bool wantWrite() const { return tlsWantWrite_ || txOff_ < txBuf_.size() || (!low_.empty() && !lowGated_); }
    bool bufferedInput() const;
    // Bytes still queued for this peer. Callers feeding a large backlog (the
    // resume/spectate replay) use this to pace themselves instead of pushing the
    // whole thing into memory at once.
    size_t txPending() const { return txBuf_.size() - txOff_; }

private:
    std::shared_ptr<TlsContext> tlsContext_;
    void* tls_=nullptr;
    bool tlsReady_=false,tlsWantWrite_=false;
    size_t tlsWriteSize_=0;
    bool tlsHandshake();
    int fd_ = -1;
    std::string err_;
    bool peerClosed_ = false, rxDrained_ = false;
    std::vector<uint8_t> rxBuf_;
    size_t rxOff_ = 0;
    std::vector<uint8_t> txBuf_;
    size_t txOff_ = 0;
    uint64_t wireOut_ = 0;                   // bytes send() has accepted on this socket (no TLS)
    std::deque<std::vector<uint8_t>> low_;   // framed low-priority messages, oldest first
    uint64_t lowDropped_ = 0;
    bool lowGated_ = false;                  // promoteLow() last held back for the kernel's queue
    void compactRx();
    void promoteLow();
};

// Bind + listen on a port (IPv4+IPv6 via a dual-stack v6 socket where possible).
// Returns the listening fd, or -1 with `err` set.
// Bind and listen. `loopbackOnly` restricts the socket to 127.0.0.1/::1, which is
// what a private single-player server wants: nobody outside this machine has any
// business reaching a server that asks for no login.
int listenOn(uint16_t port, std::string& err, bool loopbackOnly = false);

// Peer address of an accepted socket, as text ("203.0.113.9", "2001:db8::1"), or
// "?" if it cannot be determined. Used to rate-limit login attempts per host.
std::string peerAddress(int fd);

// Set a socket non-blocking + TCP_NODELAY.
void setupSocket(int fd);
// Cap the bytes the kernel holds unsent for this socket (TCP_NOTSENT_LOWAT, where the
// platform has it; Linux enforces it on send(), macOS only on writability, Windows has
// none). The server sets it on every client socket, so a backlog of ordinary frames waits
// in the Conn (where the send-backlog limits see it) rather than in the kernel. It bounds
// the bytes not yet sent, not the bytes in flight, so the link is still kept full; a
// large transfer just needs the server loop to top the kernel up every 16 KiB.
void limitUnsent(int fd);
// Bytes the kernel holds for this socket's peer: not yet sent plus sent but not yet
// acknowledged (SIOCOUTQ on Linux, SO_NWRITE on macOS). Windows has no such query, but
// its TCP_INFO (Windows 10 1703 and later) counts the bytes put on the wire, so there it
// is `written` -- every byte send() has accepted on the socket, which the caller counts
// (Conn::kernelQueued does) -- minus the bytes acknowledged (BytesOut - BytesRetrans -
// BytesInFlight). -1 when the query fails or the platform has none.
long kernelQueued(int fd, uint64_t written = 0);

}  // namespace tak::net
