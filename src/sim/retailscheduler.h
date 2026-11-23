#pragma once

// Admission/accounting layer of 0x416430. Search phases are supplied by the
// caller; this does not turn the legacy PathService phases into retail phases.
// Slots are offsets in each player's allocated entity pool, including holes.
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace ta::sim {

// Navigator wantsPath 0x44f260: bit 1 set and tick >= last request + 60; a
// success stamps the tick, so a unit re-paths at most once every 60 ticks.
constexpr bool retailAdmitNavigator(bool pending,uint32_t tick,uint32_t& stamp) {
    if (!pending || tick<stamp+60u) return false;
    stamp=tick;
    return true;
}

// TotalA.exe's path scheduler, 0x40eb70 (docs/retail-pathfinding.md 2.1): one
// search at a time; each tick every active player's PERSISTENT credit gains
// budget / activePlayers, and work runs while the credits' sum is positive. The
// current player keeps its turn while its credit is positive; otherwise the
// cursor moves to the next player with credit. With no search running the
// player's unit-slot cursor advances one record (cost 1) and admits that unit if
// it wants a path. Slices of a running search are charged to its player.
// Every 150 ticks each player's slot visits / maxunits sets the heuristic
// weight later searches take: < 1 sweep 9.0, < 2 sweeps 4.5, else 1.5.
class RetailSearchScheduler {
public:
    static constexpr int playerCount = 10;
    static constexpr int kWeightPeriod = 150;
    static constexpr int kWeightFast = 98304, kWeightMid = 294912, kWeightSlow = 589824;   // 1.5 / 4.5 / 9.0
    struct Player {
        bool enabled = false, priority = false;   // priority: unused by TA (kept for callers)
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
    std::array<int, playerCount> credit{};
    std::array<int, playerCount> visits{};
    std::array<int, playerCount> weight{kWeightFast, kWeightFast, kWeightFast, kWeightFast, kWeightFast,
                                        kWeightFast, kWeightFast, kWeightFast, kWeightFast, kWeightFast};
    int weightClock = 0;
    int maxUnits = 250;
    int remaining = 0;
    bool newAdmission = false;

    void cancel(Request request) {
        if (active == request) { active = {}; newAdmission = false; }
    }

    // validActive(request): the running search's owner still exists.
    // eligible(request): the slot's unit has a navigator that wants a path.
    // run(request, admitted, remaining): one dispatch with its own work count.
    template<class ValidActive, class Eligible, class Run>
    void tick(const std::array<Player, playerCount>& players, int budget,
              int /*divisor*/, int simulationTick, ValidActive validActive, Eligible eligible, Run run) {
        int active_players = 0;
        for (const auto& p : players) {
            if (p.slots < 0) throw std::invalid_argument("negative entity pool size");
            if (p.enabled) ++active_players;
        }
        if (++weightClock >= kWeightPeriod) {
            weightClock = 0;
            for (int i = 0; i < playerCount; ++i) {
                const int sweeps = visits[size_t(i)] / std::max(1, maxUnits);
                weight[size_t(i)] = sweeps < 1 ? kWeightSlow : sweeps < 2 ? kWeightMid : kWeightFast;
                visits[size_t(i)] = 0;
            }
        }
        if (!active_players) return;
        const int grant = budget / active_players;
        remaining = 0;
        for (int i = 0; i < playerCount; ++i) {
            if (players[size_t(i)].enabled) credit[size_t(i)] += grant;
            else if (credit[size_t(i)] > 0) credit[size_t(i)] = 0;   // a player gone idle keeps no credit
            remaining += credit[size_t(i)];
        }
        while (remaining > 0) {
            int work = 0;
            if (active.player >= 0 && validActive(active)) {
                playerCursor = active.player;
                const Slice slice = run(active, newAdmission, remaining);
                newAdmission = false;
                if (slice.work < 0) throw std::logic_error("negative search work");
                work = slice.work;
                if (slice.complete) active = {};
            } else {
                active = {};
                newAdmission = false;
                if (credit[size_t(playerCursor)] <= 0) {
                    int guard = 0;
                    do { playerCursor = (playerCursor + 1) % playerCount; }
                    while (credit[size_t(playerCursor)] <= 0 && ++guard < playerCount);
                    if (credit[size_t(playerCursor)] <= 0) break;
                }
                const size_t player = size_t(playerCursor);
                if (players[player].slots <= 0) {   // nothing to visit
                    remaining -= credit[player];
                    credit[player] = 0;
                    continue;
                }
                work = 1;
                ++visits[player];
                if (slotCursor[player] >= players[player].slots - 1) {
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
            credit[size_t(playerCursor)] -= work;
        }
    }
};

} // namespace ta::sim
