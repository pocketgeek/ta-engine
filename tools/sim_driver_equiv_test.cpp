// Single-player / multiplayer driver-equivalence regression test.
//
// SP and MP reach the world through the SAME shared functions -- setupMatch,
// applyCommand, applyEvent, World::tick -- so the RULES are identical. A full SP
// game and a full MP game are NOT bit-identical (command latency, an AI
// observation-tick offset, intra-tick ordering, variable SP substep dt and
// different AI seeds all diverge by design). What IS load-bearing -- and what
// keeps every MP peer in lockstep -- is narrower:
//
//   Given the SAME commands + events at the SAME ticks with a fixed 1/30 step,
//   the sim produces the SAME state whether commands are applied directly (as
//   single-player does) or transported through the multiplayer wire format
//   (Writer -> bytes -> Reader) first.
//
// This guards the real MP-specific risk -- a command/event serialization bug, or
// a sim-affecting side effect added to one path only -- that is invisible in
// same-process tests. It does NOT claim SP == MP for a whole game; see above.
//
// Part 1 (always, no game data): the wire format round-trips commands, events,
//   and a full bundle byte-for-byte, and the server's per-tick command sort is
//   deterministic. This is the CI-gated part.
// Part 2 (only with `<map.tnt> <dataRoot>`): builds two real worlds and drives
//   an identical schedule through the direct path vs the wire path, asserting
//   equal World::stateHash EVERY tick. Needs (gitignored) game data, so it runs
//   locally / for anyone with the assets, and skips cleanly otherwise.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "hpi/hpi.h"
#include "net/protocol.h"
#include "sim/matchsetup.h"

#include <filesystem>

using ta::net::Cmd;
using ta::net::Command;
using ta::net::Event;
using ta::net::Reader;
using ta::net::Writer;

namespace {

bool feq(float a, float b) {  // exact bit equality (the state hash mixes raw bits)
    uint32_t x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    return x == y;
}

bool cmdEqual(const Command& a, const Command& b) {
    return a.kind == b.kind && a.player == b.player && a.unitId == b.unitId &&
           a.targetId == b.targetId && feq(a.x, b.x) && feq(a.z, b.z) &&
           a.queue == b.queue && std::memcmp(a.type, b.type, 16) == 0;
}

// Serialize/deserialize one command exactly as the MP transport does.
Command wireRoundTrip(const Command& c) {
    Writer w;
    w.cmd(c);
    Reader r(w.b.data(), w.b.size());
    Command c2 = r.cmd();
    return c2;
}

int testCommandRoundTrip() {
    int fails = 0, n = 0;
    const char* types[] = {"", "ARCHER", "keep", "aramonpriest01", "0123456789abcde"};
    float coords[] = {0.0f, 1.0f, -1.0f, 0.5f, -1234.5f, 65535.75f, 1e-6f, -3.14159f};
    for (int k = 0; k <= int(Cmd::Destroy); ++k)
        for (int ti = 0; ti < 5; ++ti)
            for (float x : coords)
                for (uint8_t q : {uint8_t(0), uint8_t(1)}) {
                    Command c;
                    c.kind = Cmd(k);
                    c.player = uint8_t((k + ti) & 7);
                    c.unitId = (k * 131 - ti * 977);      // includes negatives
                    c.targetId = -(k * 17 + ti);
                    c.x = x;
                    c.z = -x + 0.25f;
                    c.queue = q;
                    std::snprintf(c.type, sizeof c.type, "%s", types[ti]);
                    ++n;
                    Command c2 = wireRoundTrip(c);
                    if (!cmdEqual(c, c2)) {
                        if (fails < 3)
                            std::printf("  cmd round-trip MISMATCH kind=%d type='%s' x=%g\n",
                                        k, types[ti], x);
                        ++fails;
                    }
                }
    std::printf("command round-trip: %d cases, %d mismatch\n", n, fails);
    return fails ? 1 : 0;
}

int testBundleRoundTrip() {
    // Build a bundle exactly as server closeTick() does, read it as the client does.
    std::vector<Command> cmds;
    for (int i = 0; i < 5; ++i) {
        Command c;
        c.kind = Cmd(i % (int(Cmd::Destroy) + 1));
        c.player = uint8_t(i & 3);
        c.unitId = 1000 + i;
        c.targetId = 42 - i;
        c.x = float(i) * 12.5f;
        c.z = float(-i) * 7.25f;
        c.queue = uint8_t(i & 1);
        std::snprintf(c.type, sizeof c.type, "u%d", i);
        cmds.push_back(c);
    }
    std::vector<Event> events = {{Event::Kind::Forfeit, 2}, {Event::Kind::Leave, 1}};
    const uint32_t tick = 12345;

    Writer w;                       // == server.cpp closeTick serialization
    w.u32(tick);
    w.u32(uint32_t(cmds.size()));
    for (const auto& c : cmds) w.cmd(c);
    w.u32(uint32_t(events.size()));
    for (const auto& e : events) { w.u8(uint8_t(e.kind)); w.u8(e.player); }

    Reader r(w.b.data(), w.b.size());   // == client.cpp bundle read
    uint32_t tk = r.u32();
    int fails = (tk != tick);
    uint32_t nc = r.u32();
    if (nc != cmds.size()) { std::printf("  bundle ncmds mismatch\n"); return 1; }
    for (uint32_t i = 0; i < nc; ++i)
        if (!cmdEqual(cmds[i], r.cmd())) ++fails;
    uint32_t ne = r.u32();
    if (ne != events.size()) { std::printf("  bundle nevents mismatch\n"); return 1; }
    for (uint32_t i = 0; i < ne; ++i) {
        Event e;
        e.kind = Event::Kind(r.u8());
        e.player = r.u8();
        if (e.kind != events[i].kind || e.player != events[i].player) ++fails;
    }
    if (!r.ok) ++fails;
    std::printf("bundle round-trip: %s\n", fails ? "MISMATCH" : "exact");
    return fails ? 1 : 0;
}

int testSortDeterminism() {
    // The server orders a tick's commands with a stable_sort by player ascending
    // (server.cpp). Verify it is stable within a player (keeps arrival order),
    // which is what makes every peer apply them identically.
    std::vector<Command> v;
    auto mk = [](uint8_t player, int arrival) {
        Command c; c.player = player; c.unitId = arrival; return c;
    };
    // players out of order, several per player, tagged by arrival in unitId.
    int arr = 0;
    for (uint8_t p : {uint8_t(2), uint8_t(0), uint8_t(2), uint8_t(1), uint8_t(0), uint8_t(2)})
        v.push_back(mk(p, arr++));

    std::stable_sort(v.begin(), v.end(),
                     [](const Command& a, const Command& b) { return a.player < b.player; });

    int fails = 0;
    // player non-decreasing, and arrival non-decreasing within equal players.
    for (size_t i = 1; i < v.size(); ++i) {
        if (v[i].player < v[i - 1].player) ++fails;
        if (v[i].player == v[i - 1].player && v[i].unitId < v[i - 1].unitId) ++fails;
    }
    std::printf("command sort: %s\n", fails ? "NOT stable/ordered" : "stable + player-ordered");
    return fails ? 1 : 0;
}

// ---- Part 2: real-world sim equivalence (needs game data) -----------------

// The commander (first alive unit) of each player, in player order.
std::vector<int> commanders(const ta::sim::World& w, int players) {
    std::vector<int> ids(size_t(players), 0);
    for (const auto& u : w.units())
        if (u.alive() && u.type && u.player >= 0 && u.player < players && ids[size_t(u.player)] == 0)
            ids[size_t(u.player)] = u.id;
    return ids;
}

int testSimEquivalence(const std::string& mapArg, const std::string& dataRoot) {
    ta::hpi::Vfs vfs = ta::hpi::mountRetailRoot(dataRoot);
    std::string mapName = std::filesystem::path(mapArg).stem().string();
    std::string mapPath = ta::hpi::findMap(vfs, mapName);
    if (mapPath.empty()) {
        std::printf("sim-equivalence: SKIP (map '%s' not found)\n", mapName.c_str());
        return 0;
    }
    ta::sim::TypeRegistry reg;
    ta::sim::setupRegistry(reg, vfs);

    ta::sim::MatchConfig cfg;
    cfg.vfs = &vfs;
    cfg.mapPath = mapPath;
    cfg.slots = {{true, 0, 0}, {true, 1, 1}};   // 2 players, FFA
    cfg.startMana = 2800;

    ta::sim::World A, B;                        // separate instances -- never alias
    auto spots = ta::sim::setupMatch(A, reg, cfg);
    ta::sim::setupMatch(B, reg, cfg);
    if (spots.size() < 2) {
        std::printf("sim-equivalence: SKIP (map has < 2 start positions)\n");
        return 0;
    }
    if (A.stateHash() != B.stateHash()) {
        std::printf("sim-equivalence: FAIL -- setupMatch not deterministic (hash differs at tick 0)\n");
        return 1;
    }
    auto mon = commanders(A, 2);
    if (!mon[0] || !mon[1]) {
        std::printf("sim-equivalence: SKIP (no commander spawned)\n");
        return 0;
    }

    // Fixed schedule: tick -> commands (+ a forfeit event via a parallel map).
    // Includes a same-tick two-player pair (order-sensitive) and an empty tick.
    auto move = [](int player, int unit, float x, float z) {
        Command c; c.kind = Cmd::Move; c.player = uint8_t(player);
        c.unitId = unit; c.x = x; c.z = z; return c;
    };
    std::map<uint32_t, std::vector<Command>> sched;
    sched[0]  = {move(0, mon[0], spots[0].first + 120, spots[0].second + 40),
                 move(1, mon[1], spots[1].first - 120, spots[1].second - 40)};  // two players, one tick
    sched[20] = {move(0, mon[0], spots[1].first, spots[1].second)};             // head across the map
    sched[45] = {move(1, mon[1], spots[0].first, spots[0].second)};
    std::map<uint32_t, std::vector<Event>> events;
    events[80] = {{Event::Kind::Forfeit, 1}};                                   // player 1 forfeits

    const int N = 150;
    for (int t = 0; t < N; ++t) {
        auto sc = sched.find(uint32_t(t));
        auto ev = events.find(uint32_t(t));

        // Path A -- direct application (single-player style).
        if (sc != sched.end()) for (const auto& c : sc->second) ta::sim::applyCommand(A, reg, c);
        if (ev != events.end()) for (const auto& e : ev->second) ta::sim::applyEvent(A, e);
        A.tick(1.0f / 30.0f);

        // Path B -- transported through the MP wire format first.
        if (sc != sched.end())
            for (const auto& c : sc->second) ta::sim::applyCommand(B, reg, wireRoundTrip(c));
        if (ev != events.end())
            for (const auto& e : ev->second) {
                Writer w; w.u8(uint8_t(e.kind)); w.u8(e.player);
                Reader r(w.b.data(), w.b.size());
                Event e2; e2.kind = Event::Kind(r.u8()); e2.player = r.u8();
                ta::sim::applyEvent(B, e2);
            }
        B.tick(1.0f / 30.0f);

        if (A.stateHash() != B.stateHash()) {
            std::printf("sim-equivalence: FAIL -- diverged at tick %d\n", t);
            return 1;
        }
    }
    std::printf("sim-equivalence: %d ticks, direct == wire; golden %016llx\n",
                N, (unsigned long long)A.stateHash());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    int fails = 0;
    fails += testCommandRoundTrip();
    fails += testBundleRoundTrip();
    fails += testSortDeterminism();
    if (argc >= 3) {
        fails += testSimEquivalence(argv[1], argv[2]);
    } else {
        std::printf("sim-equivalence: SKIP (pass <map.tnt> <dataRoot> to run the data-backed check)\n");
    }
    if (fails) { std::printf("FAIL (%d check(s))\n", fails); return 1; }
    std::printf("OK\n");
    return 0;
}
