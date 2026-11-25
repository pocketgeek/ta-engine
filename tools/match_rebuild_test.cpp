// match_rebuild_test: a match built in a World that already held another map is the
// match the referee builds in a fresh one.
//
//   match_rebuild_test <retail-install-dir>
//
// The referee (taserver) runs setupMatch in a fresh World. A client does not: GameView's
// constructor first sets its World up on the launch map -- setTerrain + registerMapFeatures,
// and a game opened from the menu's lobby launches on the fallback map, Great Divide --
// and setupMatch then builds the room's map on top. Whatever setup does not rebuild
// carries the launch map into the match without showing in the state hash until
// something reads it. The feature metal map did: Great Divide's ore grid outlived
// setTerrain, so on Core Prime Industrial Area (no ore features, surfacemetal 255) the
// client's extractors past Great Divide's edge read 0 metal, the referee's 255, and a
// single-player game desynced at the first hash after its player's first extractors
// came up (tick 600 of the recording this was found in).
//
// Three worlds build the same Core Prime match: a fresh one (the referee), one first set
// up on Great Divide as GameView's constructor does (setTerrain must not keep its ore),
// and one set up so and then World::resetForMatch (what GameView now does before
// setupMatch). Every cell's metal, the state hash after setup and the state hash after
// extractors have run must agree.
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "tnt/tnt.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ta;

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

char hex[17];
const char* h16(uint64_t v) { std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)v); return hex; }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: match_rebuild_test <retail-install-dir>\n");
        return 2;
    }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    sim::setupRegistry(reg, vfs);
    const std::string launch = hpi::findMap(vfs, "great divide");   // main.cpp's lobby fallback
    const std::string room = hpi::findMap(vfs, "Core Prime Industrial Area");
    const sim::UnitType* mex = reg.find("armmex");
    if (launch.empty() || room.empty() || !mex) {
        std::fprintf(stderr, "match_rebuild_test: Great Divide, Core Prime Industrial Area or armmex missing\n");
        return 2;
    }
    const tnt::Map launchMap = tnt::Map::load(vfs.read(launch), launch);
    const tnt::Map roomMap = tnt::Map::load(vfs.read(room), room);

    sim::MatchConfig cfg;   // the recording's room: two Arm seats, Legion, starts by slot
    cfg.vfs = &vfs;
    cfg.mapPath = room;
    cfg.startSeed = 1534423101u;
    cfg.pathfindingMode = sim::PathfindingMode::Legion;
    cfg.slots.resize(2);
    for (int i = 0; i < 2; ++i) { cfg.slots[size_t(i)].used = true; cfg.slots[size_t(i)].faction = 0; cfg.slots[size_t(i)].team = i; }

    // The launch map's setup, as GameView's constructor runs it.
    const auto launchSetup = [&](sim::World& w) {
        w.setTerrain(launchMap.heights, launchMap.width, launchMap.height, launchMap.seaLevel, &launchMap.features);
        sim::registerMapFeatures(w, launchMap, vfs, &reg);
    };
    sim::World referee, reused, client;
    for (sim::World* w : {&referee, &reused, &client}) w->setSerialThreads(true);
    launchSetup(reused);
    launchSetup(client);
    {
        int ore = 0;
        for (int z = 0; z < launchMap.height; ++z)
            for (int x = 0; x < launchMap.width; ++x) ore += client.cellMetal(x, z) != 0;
        check(ore > 0, "the launch map stamps ore (" + std::to_string(ore) + " cells)");
    }
    {
        const sim::World fresh;
        sim::World reset;
        launchSetup(reset);
        reset.setSimWorkers(3);
        reset.resetForMatch();
        check(reset.stateHash() == fresh.stateHash(), "resetForMatch leaves a fresh World's state hash");
    }
    client.resetForMatch();
    sim::setupMatch(referee, reg, cfg);
    sim::setupMatch(reused, reg, cfg);
    sim::setupMatch(client, reg, cfg);

    const int W = roomMap.width, H = roomMap.height;
    int reusedOff = 0, clientOff = 0;
    for (int z = 0; z < H; ++z)
        for (int x = 0; x < W; ++x) {
            reusedOff += reused.cellMetal(x, z) != referee.cellMetal(x, z);
            clientOff += client.cellMetal(x, z) != referee.cellMetal(x, z);
        }
    std::printf("room %dx%d cells, launch map %dx%d; referee metal at (501,527) %d\n", W, H,
                launchMap.width, launchMap.height, referee.cellMetal(501, 527));
    check(reusedOff == 0, "setTerrain drops the launch map's ore (" + std::to_string(reusedOff) + " cells differ)");
    check(clientOff == 0, "a World reset for the match has the referee's ore (" + std::to_string(clientOff) + " cells differ)");
    check(reused.stateHash() == referee.stateHash() && client.stateHash() == referee.stateHash(),
          std::string("the same state hash after setup (referee ") + h16(referee.stateHash()) + ")");

    // The recording's four extractors (player 0, x 8024..8168, z 8440: past Great Divide's
    // edge) and one inside it, finished, then five seconds of the economy.
    for (sim::World* w : {&referee, &reused, &client}) {
        w->setStartResources(0, 1000.f, 0.f);   // an empty metal store, so what they extract shows
        for (float x : {8024.f, 8072.f, 8120.f, 8168.f}) w->spawn(mex, x, 8440.f, 0.f, 0);
        w->spawn(mex, 2360.f, 2008.f, 0.f, 1);
        for (int t = 0; t < 150; ++t) w->tick(1.0f / 30.0f);
    }
    const float metal = referee.player(0).books.stored[1];
    std::printf("player 0 metal stored after 150 ticks: referee %.3f, reused %.3f, reset %.3f\n", metal,
                reused.player(0).books.stored[1], client.player(0).books.stored[1]);
    check(reused.stateHash() == referee.stateHash(), "setTerrain alone: the referee's state hash after the extractors ran");
    check(client.stateHash() == referee.stateHash(), "resetForMatch: the referee's state hash after the extractors ran");

    if (failures) { std::printf("match_rebuild_test: %d FAILED\n", failures); return 1; }
    std::printf("match_rebuild_test: all passed\n");
    return 0;
}
