#pragma once

// A non-blocking, buffered TCP connection with the protocol's length-prefix
// framing. Shared by takserver and the takclient client. One Conn per socket.

#include <cstdint>
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
    explicit Conn(int fd) : fd_(fd) {}
    ~Conn();
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;
    Conn(Conn&& o) noexcept { *this = std::move(o); }
    Conn& operator=(Conn&& o) noexcept;

    // Client: resolve host (IPv4/IPv6/hostname via getaddrinfo) and connect,
    // blocking up to timeoutMs. Returns false and sets error() on failure.
    bool connect(const std::string& host, uint16_t port, int timeoutMs = 5000);

    int fd() const { return fd_; }
    bool ok() const { return fd_ >= 0 && err_.empty(); }
    const std::string& error() const { return err_; }
    void fail(const std::string& why) { if (err_.empty()) err_ = why; }
    void closeNow();

    // Queue a framed message (kind + payload) into the send buffer.
    void send(Msg kind, const std::vector<uint8_t>& payload);
    void send(Msg kind, const Writer& w) { send(kind, w.b); }
    void send(Msg kind) { send(kind, std::vector<uint8_t>{}); }

    // Non-blocking I/O against the ready socket:
    //  recv(): read available bytes into rxBuf_; false on peer close/error.
    //  poll(): pop the next complete frame (returns false if none buffered yet).
    //  flushWrite(): push queued bytes out; false on error. wantWrite() true
    //  while bytes remain (register POLLOUT).
    bool recv();
    bool poll(Frame& out);
    bool flushWrite();
    bool wantWrite() const { return txOff_ < txBuf_.size(); }
    // Bytes still queued for this peer. Callers feeding a large backlog (the
    // resume/spectate replay) use this to pace themselves instead of pushing the
    // whole thing into memory at once.
    size_t txPending() const { return txBuf_.size() - txOff_; }

private:
    int fd_ = -1;
    std::string err_;
    std::vector<uint8_t> rxBuf_;
    size_t rxOff_ = 0;
    std::vector<uint8_t> txBuf_;
    size_t txOff_ = 0;
    void compactRx();
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

}  // namespace tak::net
