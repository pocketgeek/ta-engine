#include "net/discovery.h"

#include "net/netcompat.h"
#include "net/protocol.h"

#include <algorithm>
#include <cstring>

#ifndef _WIN32
#include <arpa/inet.h>
#endif

namespace tak::net {

namespace {
constexpr char kMagic[8] = {'T', 'A', 'K', 'L', 'A', 'N', '1', 0};
}

std::vector<uint8_t> encodeAnnounce(const LanAnnounce& a) {
    Writer w;
    for (char c : kMagic) w.u8(uint8_t(c));
    w.u32(a.version); w.u64(a.nonce); w.u16(a.port);
    w.str(a.game.substr(0, 64)); w.str(a.host.substr(0, 64)); w.str(a.map.substr(0, 96));
    w.u8(a.players); w.u8(a.capacity); w.u8(a.locked ? 1 : 0); w.u8(a.running ? 1 : 0); w.u8(a.spectators ? 1 : 0);
    return std::move(w.b);
}

std::optional<LanAnnounce> decodeAnnounce(const uint8_t* data, size_t size) {
    if (size < sizeof kMagic || std::memcmp(data, kMagic, sizeof kMagic) != 0) return {};
    Reader r(data + sizeof kMagic, size - sizeof kMagic);
    LanAnnounce a;
    a.version = r.u32(); a.nonce = r.u64(); a.port = r.u16();
    a.game = r.str(); a.host = r.str(); a.map = r.str();
    a.players = r.u8(); a.capacity = r.u8(); a.locked = r.u8() == 1; a.running = r.u8() == 1; a.spectators = r.u8() == 1;
    if (!r.ok || a.port == 0) return {};
    return a;
}

LanBeacon::~LanBeacon() { if (fd_ >= 0) sockClose(fd_); }

void LanBeacon::send(const LanAnnounce& a) {
    if (fd_ < 0) {
        netStartup();
        fd_ = int(socket(AF_INET, SOCK_DGRAM, 0));
        if (fd_ < 0) return;
        int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
        sockSetNonBlock(fd_);
    }
    const auto bytes = encodeAnnounce(a);
    // The broadcast reaches the local network; the loopback copy reaches this
    // machine even with no network up (listeners keep one row per nonce).
    for (uint32_t to : {uint32_t(INADDR_BROADCAST), uint32_t(INADDR_LOOPBACK)}) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(kDiscoveryPort);
        dst.sin_addr.s_addr = htonl(to);
        sendto(fd_, reinterpret_cast<const char*>(bytes.data()), int(bytes.size()), 0,
               reinterpret_cast<const sockaddr*>(&dst), sizeof dst);
    }
}

LanListener::~LanListener() { if (fd_ >= 0) sockClose(fd_); }

bool LanListener::open() {
    if (fd_ >= 0) return true;
    netStartup();
    fd_ = int(socket(AF_INET, SOCK_DGRAM, 0));
    if (fd_ < 0) return false;
    // Every client on this machine listens on the same port.
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&one), sizeof one);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(kDiscoveryPort);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { sockClose(fd_); fd_ = -1; return false; }
    sockSetNonBlock(fd_);
    return true;
}

void LanListener::poll(uint64_t nowMs) {
    if (fd_ >= 0)
        for (;;) {
            uint8_t buf[512];
            sockaddr_in from{};
#ifdef _WIN32
            int len = sizeof from;
#else
            socklen_t len = sizeof from;
#endif
            const auto n = recvfrom(fd_, reinterpret_cast<char*>(buf), int(sizeof buf), 0,
                                    reinterpret_cast<sockaddr*>(&from), &len);
            if (n <= 0) break;
            const auto a = decodeAnnounce(buf, size_t(n));
            if (!a || a->version != kNetVersion) continue;
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            const bool loopback = (ntohl(from.sin_addr.s_addr) >> 24) == 127;
            auto it = std::find_if(games_.begin(), games_.end(),
                                   [&](const LanGame& g) { return g.announce.nonce == a->nonce; });
            if (it == games_.end()) { games_.push_back({*a, ip, nowMs}); continue; }
            it->announce = *a;
            it->seenMs = nowMs;
            // Its network address over the loopback copy: others reach it that way too.
            if (!loopback) it->address = ip;
        }
    std::erase_if(games_, [&](const LanGame& g) { return nowMs - g.seenMs > kForgetMs; });
}

}  // namespace tak::net
