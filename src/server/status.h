#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace tak::srv {

struct StatusCounts {
    uint32_t runningGames = 0, lobbyGames = 0, connectedClients = 0;
    std::string json() const;
};

// Local monitoring uses loopback UDP on the game TCP port's numeric value.
// It works while TLS/ACME is pending and never consumes a game client slot.
// Called only on the network thread; no worker reads or simulation locks.
class StatusListener {
public:
    explicit StatusListener(uint16_t port);
    ~StatusListener();
    StatusListener(const StatusListener&) = delete;
    StatusListener& operator=(const StatusListener&) = delete;
    int pollFd(); // -1 while the datagram work budget is exhausted
    void respond(const StatusCounts& counts);
private:
    int fd_ = -1;
    unsigned remaining_ = 32;
    std::chrono::steady_clock::time_point refill_;
};

// One bounded local query; failures leave stdout empty in the CLI.
bool queryStatus(uint16_t port, StatusCounts& counts, std::string& error);

} // namespace tak::srv
