#pragma once

// LAN game announcements. A takserver started as a player's LOCAL SERVER (--lan)
// broadcasts one small UDP datagram a second while its game waits in the room, and
// the CONNECT screen listens and lists what it hears under LOCAL SERVER. Nothing
// here is trusted: an announcement only offers an address to connect to, and the
// connection is the ordinary handshake (data hash, protocol) that follows.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tak::net {

constexpr uint16_t kDiscoveryPort = 7678;   // UDP; the game itself is on the announced TCP port

struct LanAnnounce {
    uint32_t version = 0;   // kNetVersion of the server
    uint64_t nonce = 0;     // one per server run: the same server heard twice is one row
    uint16_t port = 0;      // its TCP port
    std::string game, host, map;
    uint8_t players = 0, capacity = 0;
    bool locked = false;    // the game has a password
    bool running = false;   // started: no seats left to take, watchable when spectators allowed
    bool spectators = true; // ALLOW SPECTATORS
};

std::vector<uint8_t> encodeAnnounce(const LanAnnounce& a);
std::optional<LanAnnounce> decodeAnnounce(const uint8_t* data, size_t size);

// Sends announcements to the local network's broadcast address, and to this machine.
class LanBeacon {
public:
    LanBeacon() = default;
    LanBeacon(const LanBeacon&) = delete;
    LanBeacon& operator=(const LanBeacon&) = delete;
    ~LanBeacon();
    void send(const LanAnnounce& a);
private:
    int fd_ = -1;
};

// A server heard recently: its announcement and the address it came from.
struct LanGame {
    LanAnnounce announce;
    std::string address;   // IPv4 text of the sender
    uint64_t seenMs = 0;
};

// Listens on kDiscoveryPort (shared with every other listener on this machine).
class LanListener {
public:
    LanListener() = default;
    LanListener(const LanListener&) = delete;
    LanListener& operator=(const LanListener&) = delete;
    ~LanListener();
    bool open();   // false if the port cannot be bound (nothing is then heard)
    // Take in what has arrived; servers not heard for kForgetMs drop out. Only
    // servers of this build's protocol are kept (others could not be joined).
    void poll(uint64_t nowMs);
    const std::vector<LanGame>& games() const { return games_; }
    static constexpr uint64_t kForgetMs = 3500;
private:
    int fd_ = -1;
    std::vector<LanGame> games_;
};

}  // namespace tak::net
