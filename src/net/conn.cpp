#include "net/conn.h"

#include "net/netcompat.h"

#include <cstring>
#include <utility>

namespace tak::net {

void setupSocket(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef __APPLE__
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&one), sizeof one);
#endif
    sockSetNonBlock(fd);
}

Conn::~Conn() { closeNow(); }

Conn& Conn::operator=(Conn&& o) noexcept {
    if (this != &o) {
        closeNow();
        fd_ = o.fd_; err_ = std::move(o.err_);
        rxBuf_ = std::move(o.rxBuf_); rxOff_ = o.rxOff_;
        txBuf_ = std::move(o.txBuf_); txOff_ = o.txOff_;
        peerClosed_ = o.peerClosed_;   // part of the socket's state, like err_
        o.fd_ = -1; o.rxOff_ = o.txOff_ = 0; o.peerClosed_ = false;
    }
    return *this;
}

void Conn::closeNow() {
    if (fd_ >= 0) { sockClose(fd_); fd_ = -1; }
}

bool Conn::connect(const std::string& host, uint16_t port, int timeoutMs) {
    netStartup();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;       // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        err_ = "cannot resolve " + host;
        return false;
    }
    bool connected = false;
    for (addrinfo* a = res; a && !connected; a = a->ai_next) {
        int fd = int(socket(a->ai_family, a->ai_socktype, a->ai_protocol));
        if (fd < 0) continue;
        // Blocking connect with a timeout via a temporary non-block + poll.
        sockSetNonBlock(fd);
        int r = ::connect(fd, a->ai_addr, socklen_t(a->ai_addrlen));
        if (r == 0) { connected = true; fd_ = fd; }
        else if (sockInProgress(sockErr())) {
            pollfd pf{};
            pf.fd = fd; pf.events = POLLOUT;
            if (TAK_POLL(&pf, 1, timeoutMs) > 0 && (pf.revents & POLLOUT)) {
                int se = 0; socklen_t sl = sizeof se;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&se), &sl);
                if (se == 0) { connected = true; fd_ = fd; }
            }
        }
        if (!connected) sockClose(fd);
    }
    freeaddrinfo(res);
    if (!connected) { err_ = "connect failed to " + host; return false; }
    setupSocket(fd_);
    return true;
}

void Conn::send(Msg kind, const std::vector<uint8_t>& payload) {
    // A receiver too slow to keep up must not be allowed to make us hold an
    // unbounded queue on its behalf. Well above any legitimate burst (the replay
    // stream paces itself at 256 KiB), so hitting this means the peer has stopped
    // reading; the connection is failed and dropped by the caller.
    constexpr size_t kMaxTxBacklog = 32u << 20;
    if (txBuf_.size() - txOff_ > kMaxTxBacklog) {
        err_ = "send backlog exceeded";
        return;
    }
    // frame = u32 len(payload+1) | u8 kind | payload
    uint32_t len = uint32_t(payload.size() + 1);
    for (int i = 0; i < 4; ++i) txBuf_.push_back(uint8_t(len >> (8 * i)));
    txBuf_.push_back(uint8_t(kind));
    txBuf_.insert(txBuf_.end(), payload.begin(), payload.end());
}

bool Conn::flushWrite() {
    // Drop the part already on the wire rather than carrying it until the buffer
    // happens to empty. A peer that drains partially and is topped up again --
    // exactly what the paced replay stream does -- never reaches the empty state
    // that used to be the only thing resetting this, so the consumed prefix grew
    // even while the UNSENT backlog stayed inside its limit.
    constexpr size_t kCompactAt = 1u << 20;          // 1 MiB of dead prefix
    if (txOff_ >= kCompactAt && txOff_ * 2 >= txBuf_.size()) {
        txBuf_.erase(txBuf_.begin(), txBuf_.begin() + long(txOff_));
        txOff_ = 0;
    }
    while (txOff_ < txBuf_.size()) {
        long long n = ::send(fd_, reinterpret_cast<const char*>(txBuf_.data() + txOff_),
                             int(txBuf_.size() - txOff_), MSG_NOSIGNAL);
        if (n > 0) { txOff_ += size_t(n); continue; }
        if (n < 0 && sockWouldBlock(sockErr())) break;   // socket full
        err_ = "send failed: " + sockErrStr(sockErr());
        return false;
    }
    if (txOff_ == txBuf_.size()) { txBuf_.clear(); txOff_ = 0; }
    return true;
}

bool Conn::recv() {
    char buf[16384];
    // Two bounds, because this loop used to run until the socket blocked with no
    // limit on either time or memory. A peer that keeps writing can keep it
    // spinning, which both grows rxBuf_ without limit and starves every other
    // connection in the same service pass.
    //
    // kRecvPassBytes is fairness: take a bounded bite and come back next pass
    // (poll reports the socket readable again, so nothing is lost).
    // kMaxRxBacklog is the hard cap. Frame validation happens in poll(), AFTER
    // buffering, so without this a peer can queue far more than one frame's
    // worth of unvalidated bytes. Well above kMaxFrame so a legitimate maximum
    // frame plus a partial next one always fits.
    constexpr size_t kRecvPassBytes = 1u << 20;    // 1 MiB serviced per pass
    constexpr size_t kMaxRxBacklog  = 1u << 22;    // 4 MiB unread -> peer is abusive
    size_t got = 0;
    for (;;) {
        if (got >= kRecvPassBytes) break;          // yield to the other sockets
        if (rxBuf_.size() - rxOff_ > kMaxRxBacklog) {
            err_ = "receive backlog exceeded";
            return false;
        }
        long long n = ::recv(fd_, buf, sizeof buf, 0);
        if (n > 0) { rxBuf_.insert(rxBuf_.end(), buf, buf + n); got += size_t(n); continue; }
        // EOF. Do NOT fail here: a peer that sends its last message and closes
        // usually lands both in one segment, so this same call has already buffered
        // a COMPLETE frame that returning false would throw away -- the caller bails
        // out before its poll() drain. That silently lost a final LeaveGame, turning
        // a deliberate departure into a disconnect and holding the slot for the whole
        // grace period. Record the close, keep the bytes, and let the caller drain
        // them and then check peerClosed().
        if (n == 0) { peerClosed_ = true; break; }
        int e = sockErr();
        if (sockWouldBlock(e)) break;      // drained
        if (sockInterrupted(e)) continue;
        err_ = "recv failed: " + sockErrStr(e);
        return false;
    }
    return true;
}

void Conn::compactRx() {
    if (rxOff_ > 0) {
        rxBuf_.erase(rxBuf_.begin(), rxBuf_.begin() + long(rxOff_));
        rxOff_ = 0;
    }
}

bool Conn::poll(Frame& out) {
    if (rxBuf_.size() - rxOff_ < 4) return false;
    const uint8_t* p = &rxBuf_[rxOff_];
    uint32_t len = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                   (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    if (len < 1 || len > kMaxFrame) { err_ = "oversized/empty frame"; return false; }
    if (rxBuf_.size() - rxOff_ - 4 < len) return false;   // incomplete
    out.kind = Msg(p[4]);
    out.payload.assign(p + 5, p + 4 + len);
    rxOff_ += 4 + len;
    if (rxOff_ > 65536) compactRx();
    return true;
}

int listenOn(uint16_t port, std::string& err, bool loopbackOnly) {
    netStartup();
    // Loopback-only goes straight to IPv4. A v6 socket bound to ::1 accepts only
    // IPv6 loopback -- dual-stack mapping applies to the wildcard address, not to
    // a specific one -- and every local client dials 127.0.0.1.
    int fd = loopbackOnly ? -1 : int(socket(AF_INET6, SOCK_STREAM, 0));
    bool v6 = fd >= 0;
    if (!v6) fd = int(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) { err = "socket failed"; return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    if (v6) {
        int off = 0;   // dual-stack: accept IPv4-mapped too
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = loopbackOnly ? in6addr_loopback : in6addr_any;
        a.sin6_port = htons(port);
        if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            err = "bind failed"; sockClose(fd); return -1;
        }
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        a.sin_port = htons(port);
        if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            err = "bind failed"; sockClose(fd); return -1;
        }
    }
    if (listen(fd, 64) != 0) { err = "listen failed"; sockClose(fd); return -1; }
    sockSetNonBlock(fd);
    return fd;
}

std::string peerAddress(int fd) {
    sockaddr_storage ss{};
    socklen_t len = sizeof ss;
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return "?";
    char host[NI_MAXHOST];
    if (getnameinfo(reinterpret_cast<sockaddr*>(&ss), len, host, sizeof host,
                    nullptr, 0, NI_NUMERICHOST) != 0)
        return "?";
    // A dual-stack listener reports IPv4 peers as ::ffff:203.0.113.9; strip the
    // prefix so one host cannot present as two different rate-limit keys.
    std::string s(host);
    if (s.rfind("::ffff:", 0) == 0 && s.find('.') != std::string::npos) s = s.substr(7);
    return s;
}

}  // namespace tak::net
