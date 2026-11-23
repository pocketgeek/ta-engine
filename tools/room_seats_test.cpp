// room_seats_test -- net::planCapacity (net/roomseats.h): what a room's seats become when
// its capacity changes. The server applies the plan (Server::applyCapacity) and the room
// warns with it before a map change, so both read the one rule.
//
//  - humans past the new capacity move down: to the lowest OPEN seat, else the lowest
//    CLOSED one, else the HIGHEST AI seat (that AI is removed)
//  - every AI past the new capacity is removed
//  - more humans than seats is refused, with nothing planned
//  - a capacity that keeps everyone plans nothing; a watching host holds no seat
#include "net/roomseats.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace tak::net;

static int g_fail = 0, g_checks = 0;
static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_fail; std::printf("  FAIL %s\n", what); }
}

// One letter per seat: H human, A AI, O open, C closed.
static void table(SlotInfo (&s)[kMaxSlots], const char* seats) {
    for (int i = 0; i < kMaxSlots; ++i) {
        s[i] = SlotInfo{};
        s[i].type = seats[i] == 'H' ? 1 : seats[i] == 'A' ? 2 : seats[i] == 'C' ? 3 : 0;
    }
}
using Moves = std::vector<std::pair<int, int>>;

int main() {
    SlotInfo s[kMaxSlots];

    // The spec's example: host 0, guest at 5, AIs at 2, 3 and 6; a 4-start map.
    table(s, "HOAAOHAO");
    auto p = planCapacity(s, 4);
    check(p.ok && p.humans == 2, "two humans fit four seats");
    check(p.moves == Moves{{5, 1}}, "the guest moves to the lowest OPEN seat below the capacity");
    check(p.removedAi == std::vector<int>{6}, "the AI past the capacity is removed, the ones inside stay");

    // No OPEN seat inside: a CLOSED one is taken before any AI is displaced.
    table(s, "HACAHOOO");
    p = planCapacity(s, 4);
    check(p.ok && p.moves == Moves{{4, 2}} && p.removedAi.empty(), "a CLOSED seat before displacing an AI");

    // Neither OPEN nor CLOSED inside: the HIGHEST AI inside makes way.
    table(s, "HAAAHOOO");
    p = planCapacity(s, 4);
    check(p.ok && p.moves == Moves{{4, 3}} && p.removedAi == std::vector<int>{3}, "the highest AI makes way");

    // Several humans past it, in slot order, each to the next free choice.
    table(s, "OAHCAHHO");
    p = planCapacity(s, 4);
    check(p.ok && p.moves == Moves{{5, 0}, {6, 3}} && p.removedAi == std::vector<int>{4},
          "humans move in slot order: OPEN first, then CLOSED; the AI past it goes");
    table(s, "AAAHHHOO");
    p = planCapacity(s, 4);
    check(p.ok && p.moves == Moves{{4, 2}, {5, 1}} && p.removedAi == std::vector<int>{1, 2},
          "two displaced AIs, highest first, listed ascending");

    // More humans than seats: refused, nothing planned.
    table(s, "HHHOOOOO");
    p = planCapacity(s, 2);
    check(!p.ok && p.humans == 3 && p.moves.empty() && p.removedAi.empty(), "three humans on two seats are refused");

    // A capacity that keeps everyone: nothing to do (seats past it are only closed).
    table(s, "HAOOCCCC");
    p = planCapacity(s, 4);
    check(p.ok && p.moves.empty() && p.removedAi.empty(), "a smaller capacity that keeps everyone plans nothing");
    p = planCapacity(s, 8);
    check(p.ok && p.moves.empty() && p.removedAi.empty() && p.humans == 1, "a larger capacity plans nothing");

    // A watching host holds no seat: an all-AI room only loses its AIs past the capacity.
    table(s, "AAAAAAOO");
    p = planCapacity(s, 2);
    check(p.ok && p.humans == 0 && p.moves.empty() && p.removedAi == std::vector<int>{2, 3, 4, 5},
          "an all-AI room keeps the AIs inside the capacity");

    std::printf(g_fail ? "room_seats: %d of %d checks FAILED\n" : "room_seats: all %d checks passed\n",
                g_fail ? g_fail : g_checks, g_checks);
    return g_fail ? 1 : 0;
}
