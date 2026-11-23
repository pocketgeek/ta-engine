#include "net/conn.h"

#include "net/netcompat.h"
#include "net/revocation.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/ioctl.h>
#endif
#ifdef __linux__
#include <linux/sockios.h>
#endif
#ifdef _WIN32
#include <mstcpip.h>
#endif
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

void limitUnsent(int fd) {
#ifdef TCP_NOTSENT_LOWAT
    const int bytes = 16 << 10;
    setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, reinterpret_cast<const char*>(&bytes), sizeof bytes);
#else
    (void)fd;
#endif
}

#ifdef _WIN32
namespace {
// SIO_TCP_INFO version 0 (mstcpip.h declares it only for _WIN32_WINNT >= Windows 10
// 1703, above what this build targets, so the layout is restated here; the ioctl simply
// fails on an older Windows).
struct TcpInfoV0 {
    int state;
    ULONG mss;
    ULONG64 connectionTimeMs;
    BOOLEAN timestampsEnabled;
    ULONG rttUs, minRttUs, bytesInFlight, cwnd, sndWnd, rcvWnd, rcvBuf;
    ULONG64 bytesOut, bytesIn;
    ULONG bytesReordered, bytesRetrans, fastRetrans, dupAcksIn, timeoutEpisodes;
    UCHAR synRetrans;
};
constexpr DWORD kSioTcpInfo = _WSAIORW(IOC_VENDOR, 39);
}  // namespace
#endif

long kernelQueued(int fd, uint64_t written) {
#if defined(__linux__) && defined(SIOCOUTQ)
    (void)written;
    int n = 0;
    return ioctl(fd, SIOCOUTQ, &n) == 0 ? long(n) : -1;
#elif defined(__APPLE__) && defined(SO_NWRITE)
    (void)written;
    int n = 0;
    socklen_t len = sizeof n;
    return getsockopt(fd, SOL_SOCKET, SO_NWRITE, &n, &len) == 0 ? long(n) : -1;
#elif defined(_WIN32)
    DWORD version = 0, got = 0;
    TcpInfoV0 info{};
    if (fd < 0 || WSAIoctl(SOCKET(fd), kSioTcpInfo, &version, sizeof version, &info, sizeof info, &got, nullptr,
                           nullptr) != 0 || got < sizeof info)
        return -1;
    // BytesOut counts retransmissions too; what is left once those and the bytes in flight
    // are taken off has been acknowledged.
    const uint64_t acked = info.bytesOut - info.bytesRetrans - info.bytesInFlight;
    return written > acked ? long(std::min<uint64_t>(written - acked, 1u << 30)) : 0;
#else
    (void)fd; (void)written;
    return -1;
#endif
}

long Conn::kernelQueued() const {
    // Under TLS every byte goes through the socket BIO, whose count includes the handshake.
    const uint64_t written = tls_ ? BIO_number_written(SSL_get_wbio(static_cast<SSL*>(tls_))) : wireOut_;
    return tak::net::kernelQueued(fd_, written);
}

Conn::Conn(int fd,std::shared_ptr<TlsContext> tls):tlsContext_(std::move(tls)),fd_(fd) {
    if(tlsContext_) {
        tls_=SSL_new(tlsContext_->handle);
        if(!tls_ || !attachTlsSocket(static_cast<SSL*>(tls_),fd_)) {err_="cannot initialize TLS connection";return;}
        SSL_set_accept_state(static_cast<SSL*>(tls_));
    }
}
bool Conn::bufferedInput() const {return tls_ && SSL_pending(static_cast<SSL*>(tls_))>0;}
bool Conn::tlsHandshake() {
    if(!tls_ || tlsReady_)return true;
    const int result=SSL_do_handshake(static_cast<SSL*>(tls_));
    if(result==1) {tlsReady_=true;tlsWantWrite_=false;return true;}
    const int why=SSL_get_error(static_cast<SSL*>(tls_),result);
    tlsWantWrite_=why==SSL_ERROR_WANT_WRITE;
    if(why!=SSL_ERROR_WANT_READ && why!=SSL_ERROR_WANT_WRITE)err_="TLS handshake failed";
    return false;
}
Conn::~Conn() { closeNow(); }

Conn& Conn::operator=(Conn&& o) noexcept {
    if (this != &o) {
        closeNow();
        tlsContext_=std::move(o.tlsContext_);tls_=o.tls_;o.tls_=nullptr;
        tlsReady_=o.tlsReady_;tlsWantWrite_=o.tlsWantWrite_;tlsWriteSize_=o.tlsWriteSize_;
        o.tlsReady_=o.tlsWantWrite_=false;o.tlsWriteSize_=0;
        fd_ = o.fd_; err_ = std::move(o.err_);
        rxBuf_ = std::move(o.rxBuf_); rxOff_ = o.rxOff_;
        txBuf_ = std::move(o.txBuf_); txOff_ = o.txOff_;
        wireOut_ = o.wireOut_; o.wireOut_ = 0;
        low_ = std::move(o.low_); lowDropped_ = o.lowDropped_; o.low_.clear(); o.lowDropped_ = 0;
        lowGated_ = o.lowGated_; o.lowGated_ = false;
        peerClosed_ = o.peerClosed_;   // part of the socket's state, like err_
        rxDrained_ = o.rxDrained_;
        o.fd_ = -1; o.rxOff_ = o.txOff_ = 0; o.peerClosed_ = o.rxDrained_ = false;
    }
    return *this;
}

void Conn::closeNow() {
    if(tls_) {if(tlsReady_ && err_.empty())SSL_shutdown(static_cast<SSL*>(tls_));SSL_free(static_cast<SSL*>(tls_));tls_=nullptr;}
    tlsContext_.reset();tlsReady_=tlsWantWrite_=false;tlsWriteSize_=0;
    if (fd_ >= 0) { sockClose(fd_); fd_ = -1; }
}

bool Conn::connect(const std::string& address, uint16_t port, int timeoutMs) {
    const bool secure=address.starts_with("tls://");
    const std::string host=secure?address.substr(6):address;
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
    if(secure) {
        try {tlsContext_=TlsContext::client();}
        catch(const std::exception& e) {err_=e.what();closeNow();return false;}
        tls_=SSL_new(tlsContext_->handle);
        if(!tls_ || !attachTlsSocket(static_cast<SSL*>(tls_),fd_)) {err_="TLS initialization failed";closeNow();return false;}
        auto* ssl=static_cast<SSL*>(tls_);
        SSL_set_connect_state(ssl);
        X509_VERIFY_PARAM_set_hostflags(SSL_get0_param(ssl),X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        unsigned char ip[16];
        const bool numeric=inet_pton(AF_INET,host.c_str(),ip)==1 || inet_pton(AF_INET6,host.c_str(),ip)==1;
        if((numeric ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl),host.c_str()) : SSL_set1_host(ssl,host.c_str()))!=1 ||
           (!numeric && SSL_set_tlsext_host_name(ssl,host.c_str())!=1)) {err_="invalid TLS hostname";closeNow();return false;}
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
        while(!tlsHandshake() && err_.empty()) {
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
            if(remaining<=0) {err_="TLS handshake timed out";break;}
            pollfd p{};p.fd=fd_;p.events=tlsWantWrite_?POLLOUT:POLLIN;
            if(TAK_POLL(&p,1,int(remaining))<=0) {err_="TLS handshake timed out";break;}
        }
        if(!err_.empty()) {closeNow();return false;}
        // Soft-fail: only a valid CRL listing this certificate refuses the server.
        const auto revocation=checkPeerRevocation(ssl);
        if(revocation.state==Revocation::Revoked) {err_="TLS certificate revoked";closeNow();return false;}
        if(revocation.state==Revocation::Unavailable)
            std::fprintf(stderr,"TLS: revocation status unavailable (%s); continuing\n",revocation.detail.c_str());
    }
    return true;
}

void Conn::send(Msg kind, const std::vector<uint8_t>& payload) {
    // A receiver too slow to keep up must not be allowed to make us hold an
    // unbounded queue on its behalf. Well above any legitimate burst (the replay
    // stream paces itself at 256 KiB), so hitting this means the peer has stopped
    // reading; the connection is failed and dropped by the caller.
    constexpr size_t kMaxTxBacklog = 32u << 20;
    if (payload.size() >= kMaxFrame || payload.size()+5 > kMaxTxBacklog ||
        txBuf_.size()-txOff_ > kMaxTxBacklog-(payload.size()+5)) {
        err_ = "send backlog exceeded";
        return;
    }
    // frame = u32 len(payload+1) | u8 kind | payload
    uint32_t len = uint32_t(payload.size() + 1);
    for (int i = 0; i < 4; ++i) txBuf_.push_back(uint8_t(len >> (8 * i)));
    txBuf_.push_back(uint8_t(kind));
    txBuf_.insert(txBuf_.end(), payload.begin(), payload.end());
}

bool Conn::sendLow(Msg kind, const std::vector<uint8_t>& payload) {
    if (!err_.empty() || payload.size() >= kMaxFrame) return false;
    if (txPending() > kLowBackpressure) { ++lowDropped_; return false; }
    if (low_.size() >= kLowMaxFrames) { low_.pop_front(); ++lowDropped_; }   // newest audio wins
    std::vector<uint8_t> frame;
    frame.reserve(payload.size() + 5);
    const uint32_t len = uint32_t(payload.size() + 1);
    for (int i = 0; i < 4; ++i) frame.push_back(uint8_t(len >> (8 * i)));
    frame.push_back(uint8_t(kind));
    frame.insert(frame.end(), payload.begin(), payload.end());
    low_.push_back(std::move(frame));
    return true;
}

// Move queued low-priority frames into the byte stream, but only once it is empty (and
// no TLS record is half written) and the kernel's own queue for this peer is nearly
// drained, and only kLowPromoteBytes at a time: whatever is queued after them waits
// behind at most kLowKernelMax + kLowPromoteBytes bytes of voice. On a link the game
// already fills, the kernel's queue never drains that far and the voice queue overflows
// and drops instead (sendLow keeps only the newest frames). Where the kernel cannot report
// its queue the voice is thrown away: unchecked, it would fill the kernel's whole send
// buffer (64 KiB on Windows) on a slow link, ahead of every tick acknowledgement.
void Conn::promoteLow() {
    lowGated_ = false;
    if (low_.empty() || txOff_ != txBuf_.size() || tlsWriteSize_) return;
    const long queued = kernelQueued();
    if (queued < 0) { dropLow(); return; }
    if (queued >= kLowKernelMax) { lowGated_ = true; return; }
    txBuf_.clear(); txOff_ = 0;
    while (!low_.empty() && txBuf_.size() < kLowPromoteBytes) {
        txBuf_.insert(txBuf_.end(), low_.front().begin(), low_.front().end());
        low_.pop_front();
    }
}

bool Conn::flushWrite() {
    if(!tlsHandshake())return err_.empty();
    promoteLow();
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
    size_t sent=0;
    while (txOff_ < txBuf_.size() && sent<(1u<<20)) {
        long long n;
        if(tls_) {
            if(!tlsWriteSize_)tlsWriteSize_=std::min(txBuf_.size()-txOff_,size_t(16384));
            n=SSL_write(static_cast<SSL*>(tls_),txBuf_.data()+txOff_,int(tlsWriteSize_));
            if(n<=0) {
                const int why=SSL_get_error(static_cast<SSL*>(tls_),int(n));
                if(why==SSL_ERROR_WANT_READ || why==SSL_ERROR_WANT_WRITE) {tlsWantWrite_=why==SSL_ERROR_WANT_WRITE;return true;}
                err_="TLS send failed";return false;
            }
            tlsWriteSize_=0;tlsWantWrite_=false;
        } else n=::send(fd_,reinterpret_cast<const char*>(txBuf_.data()+txOff_),
                       int(std::min(txBuf_.size()-txOff_,size_t(1u<<20)-sent)),MSG_NOSIGNAL);
        if (n > 0) { txOff_ += size_t(n); sent+=size_t(n); if(!tls_)wireOut_+=uint64_t(n); continue; }
        if (n < 0 && sockWouldBlock(sockErr())) break;   // socket full
        err_ = "send failed: " + sockErrStr(sockErr());
        return false;
    }
    if (txOff_ == txBuf_.size()) { txBuf_.clear(); txOff_ = 0; }
    // Everything ordinary went out: let the voice queue follow on the next flush (the
    // caller polls for writability while voice waits, see wantWrite(), or flushes again
    // soon while it waits on the kernel, see lowWaiting()).
    return true;
}

bool Conn::recv(size_t budget) {
    rxDrained_ = false;
    if(!tlsHandshake())return err_.empty();
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
    const size_t kRecvPassBytes = std::min(budget,size_t(1u<<20));    // 1 MiB serviced per pass
    constexpr size_t kMaxRxBacklog  = 1u << 22;    // 4 MiB unread -> peer is abusive
    size_t got = 0;
    for (;;) {
        if (got >= kRecvPassBytes) break;          // yield to the other sockets
        if (rxBuf_.size() - rxOff_ > kMaxRxBacklog) {
            err_ = "receive backlog exceeded";
            return false;
        }
        long long n;
        if(tls_) {
            n=SSL_read(static_cast<SSL*>(tls_),buf,int(std::min(sizeof buf,kRecvPassBytes-got)));
            if(n<=0) {
                const int why=SSL_get_error(static_cast<SSL*>(tls_),int(n));
                if(why==SSL_ERROR_WANT_READ || why==SSL_ERROR_WANT_WRITE) {tlsWantWrite_=why==SSL_ERROR_WANT_WRITE;break;}
                if(why==SSL_ERROR_ZERO_RETURN ||
                   (why==SSL_ERROR_SSL && ERR_GET_REASON(ERR_peek_last_error())==SSL_R_UNEXPECTED_EOF_WHILE_READING)) {
                    // Complete authenticated frames preceding EOF still belong to
                    // the peer. poll() never surfaces an incomplete trailing frame.
                    peerClosed_=true;ERR_clear_error();break;
                }
                err_="TLS receive failed";return false;
            }
        } else n=::recv(fd_,buf,int(std::min(sizeof buf,kRecvPassBytes-got)),0);
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
    rxDrained_ = got < kRecvPassBytes;
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
#ifdef _WIN32
    // Windows SO_REUSEADDR permits another process to bind the same live port.
    // Challenge and game listeners must have exclusive ownership.
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#endif
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
