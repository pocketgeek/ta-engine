#pragma once

// What a room's seats become when its capacity changes (a new map, MAP SCENARIO turned
// on or off): a pure plan the server applies (Server::applyCapacity) and the client reads
// to warn before it asks, so the two cannot drift.

#include <algorithm>
#include <utility>
#include <vector>

#include "net/protocol.h"

namespace tak::net {

struct SeatPlan {
    bool ok = true;                         // false: more humans than seats (nothing planned)
    int humans = 0;
    std::vector<std::pair<int, int>> moves; // human from -> to
    std::vector<int> removedAi;             // AI seats emptied (past capacity, or displaced), ascending
};

// Humans are the type-1 seats. Each one at a slot >= newCap, in slot order, moves to the
// lowest OPEN seat below newCap not yet taken by the plan, else the lowest CLOSED one, else
// the HIGHEST AI seat (that AI is removed). Every AI at a slot >= newCap is removed too.
// A watching host holds no seat, so it is not counted.
inline SeatPlan planCapacity(const SlotInfo (&slots)[kMaxSlots], int newCap) {
    SeatPlan plan;
    newCap = std::clamp(newCap, 0, int(kMaxSlots));
    for (const auto& s : slots) plan.humans += s.type == 1;
    if (plan.humans > newCap) { plan.ok = false; return plan; }
    bool taken[kMaxSlots] = {};
    const auto lowest = [&](uint8_t type) {
        for (int i = 0; i < newCap; ++i)
            if (!taken[i] && slots[i].type == type) return i;
        return -1;
    };
    for (int from = newCap; from < kMaxSlots; ++from) {
        if (slots[from].type != 1) continue;
        int to = lowest(0);
        if (to < 0) to = lowest(3);
        if (to < 0)
            for (int i = newCap - 1; i >= 0 && to < 0; --i)
                if (!taken[i] && slots[i].type == 2) { to = i; plan.removedAi.push_back(i); }
        if (to < 0) { plan.ok = false; return plan; }   // unreachable: humans <= newCap
        taken[to] = true;
        plan.moves.push_back({from, to});
    }
    for (int i = newCap; i < kMaxSlots; ++i)
        if (slots[i].type == 2) plan.removedAi.push_back(i);
    std::sort(plan.removedAi.begin(), plan.removedAi.end());
    return plan;
}

}  // namespace tak::net
