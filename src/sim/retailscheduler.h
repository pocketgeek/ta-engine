#pragma once

// Admission/accounting layer of 0x416430. Search phases are supplied by the
// caller; this does not turn the legacy PathService phases into retail phases.
// Slots are offsets in each player's allocated entity pool, including holes.
#include <array>
#include <cstdint>
#include <stdexcept>

namespace tak::sim {

// Navigator 4e54a0: selecting a pending request stamps it immediately,
// even when the scheduler has no budget left to initialize the search.
constexpr bool retailAdmitNavigator(bool pending,uint32_t tick,uint32_t& stamp) {
    if (!pending || tick<stamp+15u) return false;
    stamp=tick;
    return true;
}

class RetailSearchScheduler {
public:
    static constexpr int playerCount = 10;
    struct Player {
        bool enabled = false, priority = false;
        int pending = 0, slots = 0;
    };
    struct Request {
        int player = -1, slot = -1;
        bool operator==(const Request&) const = default;
    };
    struct Slice {
        int work = 0;
        bool complete = false;
    };

    Request active;
    int playerCursor = 0;
    std::array<int, playerCount> slotCursor{};
    std::array<int, playerCount> lastWrapTick{};
    std::array<int, playerCount> shares{};
    int remaining = 0;
    bool newAdmission = false;

    void cancel(Request request) {
        if (active == request) { active = {}; newAdmission = false; }
    }

    // validActive(request) applies entity flags only: retail does not repeat
    // controller lookup for a suspended search. eligible(request) also applies
    // mover and pending-controller checks when scanning for a new request.
    // run(request, admitted, globalRemaining) performs one phase
    // dispatch, including its own work accounting. A new admission first calls
    // run on the next loop iteration, after charging its seven-unit scan.
    // Existing requests resume before any new admission, regardless of shares.
    template<class ValidActive, class Eligible, class Run>
    void tick(const std::array<Player, playerCount>& players, int budget,
              int divisor, int simulationTick, ValidActive validActive, Eligible eligible, Run run) {
        if (divisor <= 0) return;
        int weights = 0;
        for (const auto& p : players) {
            if (p.slots < 0) throw std::invalid_argument("negative entity pool size");
            if (p.enabled && p.pending > 0) weights += p.priority ? 5 : 1;
        }
        // Retail returns before rebuilding shares when nobody is eligible.
        if (!weights) return;
        const int quantum = (budget / divisor) / weights;
        std::array<int, playerCount> scanned{}, limits{};
        remaining = 0;
        for (int i = 0; i < playerCount; ++i) {
            const auto& p = players[size_t(i)];
            shares[size_t(i)] = p.enabled && p.pending > 0 ? quantum * (p.priority ? 5 : 1) : 0;
            limits[size_t(i)] = p.enabled && p.pending > 0 ? p.slots : 0;
            remaining += shares[size_t(i)];
        }
        while (remaining > 0) {
            int work = 0;
            if (active.player >= 0 && validActive(active)) {
                const Slice slice = run(active, newAdmission, remaining);
                newAdmission = false;
                if (slice.work < 0)
                    throw std::logic_error("negative search work");
                work = slice.work;
                if (slice.complete) active = {};
            } else {
                active = {};
                newAdmission = false;
                do { playerCursor = (playerCursor + 1) % playerCount; }
                while (shares[size_t(playerCursor)] <= 0);
                const size_t player = size_t(playerCursor);
                if (scanned[player] >= limits[player]) {
                    remaining -= shares[player];
                    shares[player] = 0;
                    continue;
                }
                ++scanned[player];
                work = 7;
                if (slotCursor[player] == players[player].slots - 1) {
                    slotCursor[player] = 0;
                    lastWrapTick[player] = simulationTick;
                } else ++slotCursor[player];
                Request candidate{playerCursor, slotCursor[player]};
                if (eligible(candidate)) {
                    active = candidate;
                    newAdmission = true;
                }
            }
            remaining -= work;
            shares[size_t(playerCursor)] -= work;
        }
    }
};

} // namespace tak::sim
