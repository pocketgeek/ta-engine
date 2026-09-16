// navblock_test: the map's obstacle features must block the nav grid, and the TWO
// paths that build a world must agree about it.
//
//   navblock_test <retail-install-dir>
//
// WHY THIS TEST EXISTS.
// Feature nav-blocking used to be done by the CLIENT, in GameView::addFeature, while
// loading a map's features for display. The referee never runs the viewer, so its
// ground grid never received those writes and the two peers' nav_ differed from the
// first tick. Nothing faulted immediately, because nav_ is not folded into
// stateHash -- it surfaced much later as a desync, when losBetween disagreed about
// one line of sight and auto-acquisition picked a different target. In an 8-AI game
// that was a single Cannoneer out of 15215 units, at tick 252.
//
// The fix put blocking in the sim, where both peers run it. But there are two ways a
// world gets built:
//
//   setupMatch()          -- skirmish and multiplayer
//   registerMapFeatures() -- campaign missions, CRT scenarios, local harnesses,
//                            which build worlds WITHOUT setupMatch
//
// They were near-identical loops coordinated only by a comment, and that is exactly
// how they drifted: one blocked, the other did not. They now share one
// scanFeaturePlane(), and this test holds them to the same result so a future edit
// to one cannot silently diverge from the other.
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
// Two Castles is the map whose road network verified the 0xFFFB feature plane, so it
// is known to carry a real spread of features rather than bare terrain.
const char* kMap = "maps/Two Castles.tnt";

void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s %s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
                detail.empty() ? "" : (" (" + detail + ")").c_str());
    if (!ok) ++failures;
}

// A cheap fingerprint of everything that decides passability: the shared obstacle
// overlay is what losBetween and the per-class grids both read.
uint64_t walkFingerprint(sim::World& w, int cols, int rows) {
    uint64_t h = 1469598103934665603ULL;
    sim::NavGrid& g = w.nav();
    for (int z = 0; z < rows; ++z)
        for (int x = 0; x < cols; ++x) {
            h ^= uint64_t(g.walkable(x, z) ? 1u : 0u);
            h *= 1099511628211ULL;
        }
    return h;
}

int blockedCells(sim::World& w, int cols, int rows) {
    int n = 0;
    sim::NavGrid& g = w.nav();
    for (int z = 0; z < rows; ++z)
        for (int x = 0; x < cols; ++x)
            if (!g.walkable(x, z)) ++n;
    return n;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: navblock_test <retail-install-dir>\n"); return 2; }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
    reg.loadDir(vfs, "units/");

    std::string mapPath = kMap;
    if (!vfs.has(mapPath)) {
        std::printf("  (no %s in this install; skipped)\n", kMap);
        return 0;
    }
    tnt::Map map = tnt::Map::load(vfs.read(mapPath), mapPath);
    const int cols = map.width, rows = map.height;

    // ---- 1. setupMatch blocks the map's obstacle features ---------------------
    std::printf("[setupMatch blocks map features]\n");
    sim::World wMatch;
    {
        sim::MatchConfig cfg;
        cfg.vfs = &vfs;
        cfg.mapPath = mapPath;
        cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
        cfg.slots[0].team = 0; cfg.slots[1].team = 1;
        sim::setupMatch(wMatch, reg, cfg);
    }
    const int matchBlocked = blockedCells(wMatch, cols, rows);

    // Terrain alone already blocks cells (cliffs, water, the occlusion pass), so the
    // meaningful comparison is against a world with the SAME terrain and no features.
    sim::World wBare;
    wBare.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    const int bareBlocked = blockedCells(wBare, cols, rows);

    check(matchBlocked > bareBlocked,
          "features add blocked cells beyond bare terrain",
          "bare=" + std::to_string(bareBlocked) + " match=" + std::to_string(matchBlocked));

    // ---- 2. registerMapFeatures blocks identically ----------------------------
    // THE REGRESSION. This is the path missions and scenarios take, and the one that
    // used to delegate blocking to the viewer. It must reach the same passability as
    // setupMatch, cell for cell.
    std::printf("[registerMapFeatures matches setupMatch]\n");
    sim::World wReg;
    wReg.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    sim::registerMapFeatures(wReg, map, vfs, &reg);

    const int regBlocked = blockedCells(wReg, cols, rows);
    check(regBlocked == matchBlocked,
          "same number of blocked cells as setupMatch",
          "register=" + std::to_string(regBlocked) + " match=" + std::to_string(matchBlocked));
    check(walkFingerprint(wReg, cols, rows) == walkFingerprint(wMatch, cols, rows),
          "and the same passability cell for cell");

    // ---- 3. metal patches stay buildable -------------------------------------
    // An extractor's 3x3 footprint is carved clear at every patch centre, so a
    // tree or rock overlapping the patch cannot make it unbuildable. Losing that
    // carve is a quiet failure -- the map keeps its metal but the metal stops
    // being reachable, and the only symptom is a poorer economy.
    std::printf("[metal patches stay buildable]\n");
    const auto& spots = wReg.metalSpots();
    if (spots.empty()) {
        std::printf("  (this map has no metal patches; skipped)\n");
    } else {
        int clear = 0;
        for (const auto& [sx, sz] : spots) {
            int cx = int(sx) / 16 - 1, cz = int(sz) / 16 - 1;
            bool all = true;
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i)
                    if (!wReg.nav().walkable(cx + i, cz + j)) all = false;
            if (all) ++clear;
        }
        check(clear == int(spots.size()),
              "every patch's 3x3 extractor footprint is carved clear",
              std::to_string(clear) + "/" + std::to_string(spots.size()));
        check(spots.size() == wMatch.metalSpots().size(),
              "and both paths find the same patches",
              std::to_string(spots.size()) + " vs " + std::to_string(wMatch.metalSpots().size()));
    }

    std::printf(failures ? "\nFAILED (%d)\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
