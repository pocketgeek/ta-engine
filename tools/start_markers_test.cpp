// start_markers_test -- the lobby's numbered start markers (client/startmarkers.h).
//
// Two things can go wrong without anyone noticing on screen: a marker wearing the
// colour of a slot that will not open there (the tint rule drifting from the one
// setupMatch uses), and a marker drawn away from its start (the world -> preview
// scale). Both are pinned here; neither needs game data.

#include "client/startmarkers.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;

static void expect(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; }
}

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
    using tak::net::GameOptions;
    using tak::net::SlotInfo;
    using tak::net::kMaxSlots;

    // FIXED: a human in slot 1 and an AI in slot 8 open at StartPos1 and StartPos2.
    SlotInfo room[kMaxSlots];
    room[0].type = 1;   // human
    room[7].type = 2;   // AI
    room[3].type = 3;   // closed: takes nothing
    {
        GameOptions o;
        const auto who = tak::startmarkers::occupants(o, room, 4);
        expect(who == std::vector<int>({0, 7, -1, -1}), "FIXED: slot 1 -> start 1, slot 8 -> start 2");
        const auto few = tak::startmarkers::occupants(o, room, 1);
        expect(few == std::vector<int>({0}), "a seat past the map's starts marks nothing");
    }
    // BY SLOT: slot 8 opens at StartPos8 when the map has one, and falls back to the
    // lowest start no seated slot claims when it does not.
    {
        GameOptions o; o.randomStarts = tak::net::kStartsBySlot;
        expect(tak::startmarkers::occupants(o, room, 8) == std::vector<int>({0, -1, -1, -1, -1, -1, -1, 7}),
               "BY SLOT: slot 1 -> start 1, slot 8 -> start 8");
        expect(tak::startmarkers::occupants(o, room, 4) == std::vector<int>({0, 7, -1, -1}),
               "BY SLOT: slot 8 on a 4-start map -> start 2");
    }
    // RANDOM and BENCHMARK: no start belongs to a slot before the match starts.
    {
        GameOptions o; o.randomStarts = 1;
        expect(tak::startmarkers::occupants(o, room, 4) == std::vector<int>(4, -1), "RANDOM: all neutral");
        GameOptions b; b.benchmark = 1;
        expect(tak::startmarkers::occupants(b, room, 4) == std::vector<int>(4, -1), "BENCHMARK: all neutral");
    }

    // Scale: the radar picture covers the whole map, so a start scales straight on.
    {
        auto p = tak::startmarkers::place({2304, 1792}, 4608, 3584, 10, 20, 200, 100, 5);
        expect(near(p.x, 110) && near(p.y, 70), "map centre lands on the rect centre");
        p = tak::startmarkers::place({37 * 16, 190 * 16}, 4608, 3584, 0, 0, 511, 386, 0);
        expect(near(p.x, 37 * 16 / 4608.0f * 511) && near(p.y, 190 * 16 / 3584.0f * 386),
               "Abnar's Terrace StartPos1 scales per axis");
        p = tak::startmarkers::place({0, 3584}, 4608, 3584, 10, 20, 200, 100, 8);
        expect(near(p.x, 18) && near(p.y, 112), "an edge start keeps its whole marker inside");
        p = tak::startmarkers::place({50, 50}, 0, 0, 10, 20, 200, 100, 8);
        expect(near(p.x, 110) && near(p.y, 70), "unknown map size: centred, not divided by zero");
    }

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("start_markers_test: ok\n");
    return 0;
}
