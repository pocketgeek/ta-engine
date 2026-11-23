#include "server/status.h"
#include "net/netcompat.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace tak::srv {
namespace {
using namespace tak::net;
using Clock = std::chrono::steady_clock;
constexpr std::string_view kRequest = "takserver-status-v1\n";
constexpr size_t kReplySize = kRequest.size() + 12;

sockaddr_in localAddress(uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return address;
}

struct Socket {
    int fd;
    ~Socket() { if (fd >= 0) sockClose(fd); }
};
}

std::string StatusCounts::json() const {
    return "{\n  \"running_games\": " + std::to_string(runningGames) +
        ",\n  \"lobby_games\": " + std::to_string(lobbyGames) +
        ",\n  \"connected_clients\": " + std::to_string(connectedClients) + "\n}\n";
}

StatusListener::StatusListener(uint16_t port) : refill_(Clock::now() + std::chrono::seconds(1)) {
    netStartup();
    Socket socket{int(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))};
    if (socket.fd < 0) throw std::runtime_error("cannot create local status socket: " + sockErrStr(sockErr()));
#ifdef _WIN32
    // Do not allow another listener to steal queries using SO_REUSEADDR.
    int exclusive = 1;
    if (setsockopt(socket.fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof exclusive) != 0)
        throw std::runtime_error("cannot reserve local status socket: " + sockErrStr(sockErr()));
#endif
    const auto address = localAddress(port);
    if (::bind(socket.fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
        throw std::runtime_error("cannot bind local status UDP port " + std::to_string(port) + ": " + sockErrStr(sockErr()));
    sockSetNonBlock(socket.fd);
    fd_ = socket.fd;
    socket.fd = -1;
}

StatusListener::~StatusListener() { if (fd_ >= 0) sockClose(fd_); }

int StatusListener::pollFd() {
    const auto now = Clock::now();
    if (now >= refill_) {
        remaining_ = 32;
        refill_ = now + std::chrono::seconds(1);
    }
    return remaining_ ? fd_ : -1;
}

void StatusListener::respond(const StatusCounts& counts) {
    std::array<char, kReplySize> reply{};
    std::copy(kRequest.begin(), kRequest.end(), reply.begin());
    size_t offset = kRequest.size();
    for (const auto value : {counts.runningGames, counts.lobbyGames, counts.connectedClients})
        for (unsigned i = 0; i < 4; ++i) reply[offset++] = char(value >> (i * 8));
    for (unsigned packets = 0; packets < 8 && remaining_; ++packets) {
        // One extra byte rejects oversized requests even when recvfrom truncates.
        std::array<char, kRequest.size() + 1> request{};
        sockaddr_in source{};
        socklen_t length = sizeof source;
        const int size = int(recvfrom(fd_, request.data(), int(request.size()), 0,
            reinterpret_cast<sockaddr*>(&source), &length));
        if (size < 0 && (sockWouldBlock(sockErr()) || sockInterrupted(sockErr()))) break;
        --remaining_; // Malformed/oversized packets consume the same work budget.
        if (size != int(kRequest.size()) || source.sin_family != AF_INET ||
            (ntohl(source.sin_addr.s_addr) >> 24) != 127 ||
            std::string_view(request.data(), size) != kRequest) continue;
        // Nonblocking, tiny response; a full socket drops the probe instead of
        // queuing work, allocating per-client state, or delaying game traffic.
        sendto(fd_, reply.data(), int(reply.size()), 0,
            reinterpret_cast<const sockaddr*>(&source), length);
    }
}

bool queryStatus(uint16_t port, StatusCounts& counts, std::string& error) {
    netStartup();
    Socket socket{int(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))};
    const auto fail = [&](const std::string& reason) {
        error = "status query to 127.0.0.1:" + std::to_string(port) + ": " + reason;
        return false;
    };
    if (socket.fd < 0) return fail(sockErrStr(sockErr()));
    sockSetNonBlock(socket.fd);
    const auto address = localAddress(port);
    // A connected UDP socket accepts replies only from the queried endpoint.
    if (::connect(socket.fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
        return fail(sockErrStr(sockErr()));
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    auto retry = Clock::now();
    while (Clock::now() < deadline) {
        const auto now = Clock::now();
        if (now >= retry) {
            if (::send(socket.fd, kRequest.data(), int(kRequest.size()), 0) < 0 && !sockWouldBlock(sockErr()))
                return fail(sockErrStr(sockErr()));
            retry = now + std::chrono::seconds(1);
        }
        pollfd p{}; p.fd = socket.fd; p.events = POLLIN;
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(std::min(deadline, retry) - Clock::now()).count();
        const int polled = TAK_POLL(&p, 1, int(std::max<int64_t>(1, wait)));
        if (polled < 0) {
            if (sockInterrupted(sockErr())) continue;
            return fail(sockErrStr(sockErr()));
        }
        if (!polled) continue;
        std::array<char, kReplySize + 1> reply{};
        const int size = int(::recv(socket.fd, reply.data(), int(reply.size()), 0));
        if (size < 0) {
            if (sockWouldBlock(sockErr()) || sockInterrupted(sockErr())) continue;
            return fail(sockErrStr(sockErr()));
        }
        if (size != int(kReplySize) || std::string_view(reply.data(), kRequest.size()) != kRequest)
            return fail("invalid status response");
        size_t offset = kRequest.size();
        for (auto* value : {&counts.runningGames, &counts.lobbyGames, &counts.connectedClients}) {
            *value = 0;
            for (unsigned i = 0; i < 4; ++i) *value |= uint32_t(uint8_t(reply[offset++])) << (i * 8);
        }
        return true;
    }
    return fail("timed out; check that an updated takserver is running on this port");
}

} // namespace tak::srv
