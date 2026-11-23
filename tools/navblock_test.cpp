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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

using namespace tak;

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

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    return value;
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

    // Terrain alone already blocks cells (cliffs and water), so the
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

    // Lake Lokken's shallow-water TarWave05 art is explicitly blocking=0 in the
    // shipped feature TDF. It must remain absent from the shared nav obstacle
    // overlay, including for boat grids; blanket-blocking non-mana features made
    // Vertrans unload routes fail on decorative wave cells.
    std::printf("[nonblocking wave art stays out of navigation obstacles]\n");
    const std::string lokkenPath = hpi::findMap(vfs, "Lake Lokken");
    if (lokkenPath.empty()) {
        std::printf("  (Lake Lokken is absent from this install; skipped)\n");
    } else {
        tnt::Map lokken = tnt::Map::load(vfs.read(lokkenPath), lokkenPath);
        sim::World waveWorld;
        waveWorld.setTerrain(lokken.heights, lokken.width, lokken.height,
                             lokken.seaLevel, &lokken.features);
        sim::registerMapFeatures(waveWorld, lokken, vfs, &reg);
        const auto waveIt = std::find_if(lokken.featureNames.begin(), lokken.featureNames.end(),
            [](const std::string& name) { return lower(name) == "tarwave05"; });
        if (waveIt == lokken.featureNames.end()) {
            std::printf("  (Lake Lokken has no TarWave05 cells; skipped)\n");
        } else {
            const size_t featureId = size_t(waveIt - lokken.featureNames.begin());
            int waveCells = 0;
            bool overlayClear = true;
            const auto& obstacles = waveWorld.obstacles();
            for (size_t i = 0; i < lokken.features.size(); ++i) {
                if (lokken.features[i] != featureId) continue;
                ++waveCells;
                overlayClear &= i < obstacles.size() && obstacles[i] == 0;
            }
            check(waveCells > 0 && overlayClear,
                  "TarWave05 cells do not enter the shared navigation obstacle overlay",
                  "checked=" + std::to_string(waveCells));
        }
    }

    // ---- 3. mana deposits stay buildable -------------------------------------
    // Standing Stones block, but the 2x2 lodestone footprint at each deposit centre is
    // carved back out so a lodestone can still be placed. Losing that carve is a quiet
    // failure -- the map simply stops being playable as designed.
    std::printf("[mana deposits stay buildable]\n");
    const auto& spots = wReg.manaSpots();
    if (spots.empty()) {
        std::printf("  (this map has no mana deposits; skipped)\n");
    } else {
        int clear = 0;
        for (const auto& [sx, sz] : spots) {
            int cx = int(sx) / 16 - 1, cz = int(sz) / 16 - 1;
            bool all = true;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i)
                    if (!wReg.nav().walkable(cx + i, cz + j)) all = false;
            if (all) ++clear;
        }
        check(clear == int(spots.size()),
              "every deposit's 2x2 lodestone footprint is carved clear",
              std::to_string(clear) + "/" + std::to_string(spots.size()));
        check(spots.size() == wMatch.manaSpots().size(),
              "and both paths find the same deposits",
              std::to_string(spots.size()) + " vs " + std::to_string(wMatch.manaSpots().size()));
    }

    // Isolate the 2x2 tree that exposed the shifted factory-exit blocker.
    tnt::Map isolated;
    isolated.width = isolated.height = 32;
    isolated.seaLevel = 20;
    isolated.heights.assign(32 * 32, 100);
    isolated.features.assign(32 * 32, 0xffff);
    isolated.featureNames = {"ZonTree201"};
    isolated.features[10 * 32 + 12] = 0;
    sim::World treeWorld;
    treeWorld.setTerrain(isolated.heights, 32, 32, 20, &isolated.features);
    sim::registerMapFeatures(treeWorld, isolated, vfs, &reg);
    const auto* tree = treeWorld.feature(10 * 32 + 12);
    check(tree && tree->fx == 2 && tree->fz == 2 && tree->x == sim::Fixed::fromInt(208) &&
          tree->z == sim::Fixed::fromInt(176), "multi-cell feature uses its footprint centre");
    bool anchored = true;
    for (int z = 9; z <= 12; ++z)
        for (int x = 11; x <= 14; ++x)
            anchored &= treeWorld.nav().walkable(x, z) == !(x >= 12 && x < 14 && z >= 10 && z < 12);
    check(anchored, "feature blocks from its recorded origin, with clear neighbours");
    {
        // Two overlapping 2x2 trees: the later one in scan order removes the
        // earlier at install (495360), and retail keeps nothing of it -- no
        // tracked feature, no blocker (Lake Ferrix_JM loses 287 this way).
        tnt::Map ghost = isolated;
        ghost.features[11 * 32 + 13] = 0;
        sim::World ghostWorld;
        ghostWorld.setTerrain(ghost.heights, 32, 32, 20, &ghost.features);
        sim::registerMapFeatures(ghostWorld, ghost, vfs, &reg);
        const auto& installed = ghostWorld.installedMapFeatures();
        check(installed.size() == ghost.features.size() && installed[10 * 32 + 12] == 0xffff &&
              installed[11 * 32 + 13] == 0, "an overlapped earlier feature does not survive install");
        check(!ghostWorld.feature(10 * 32 + 12) && ghostWorld.feature(11 * 32 + 13),
              "and is not tracked, while the later one is");
        check(ghostWorld.nav().walkable(12, 10) && ghostWorld.nav().walkable(13, 10) &&
              ghostWorld.nav().walkable(12, 11) && !ghostWorld.nav().walkable(13, 11) &&
              !ghostWorld.nav().walkable(14, 12), "only the survivor blocks");
    }
    sim::UnitType reclaimer;
    reclaimer.id = "reclaimer"; reclaimer.maxHp = 100;
    reclaimer.isBuilder = reclaimer.canMove = reclaimer.canReclaim = true;
    reclaimer.maxVel = sim::Fixed::fromInt(1); reclaimer.buildDist = 200;
    reclaimer.modelTop = 32 * 65536;
    const int builder = treeWorld.spawn(&reclaimer, 144, 176, 0, 0);
    treeWorld.reclaim(builder, 10 * 32 + 12, false);
    uint32_t emissions=0;
    int lastEmission=-1;
    bool cadence=true,particles=false;
    for (int i = 0; i < 600 && tree && tree->alive; ++i) {
        treeWorld.tick(1.f / 30);
        const auto* worker=treeWorld.unit(builder);
        if (worker->constructionEmissions[0]!=emissions) {
            cadence &= worker->constructionEmissions[0]==emissions+1 &&
                       (lastEmission<0 || i-lastEmission==2);
            lastEmission=i;
            emissions=worker->constructionEmissions[0];
        }
        particles |= worker->cosmeticConstructionEmitter &&
                     !worker->cosmeticConstructionEmitter->particles.empty();
    }
    check(emissions>1 && cadence && particles,
          "active reclaim emits falling worker particles every two ticks");
    check(tree && !tree->alive, "multi-cell feature can be reclaimed");
    for(int i=0;i<180;++i)treeWorld.tick(1.f/30);
    const auto* worker=treeWorld.unit(builder);
    check(worker->constructionEmissions[0]==emissions &&
          worker->cosmeticConstructionEmitter && worker->cosmeticConstructionEmitter->particles.empty(),
          "completed reclaim stops emission and existing particles drain");
    bool freed = true;
    for (int z = 10; z < 12; ++z)
        for (int x = 12; x < 14; ++x) freed &= treeWorld.nav().walkable(x, z);
    check(freed, "reclaim frees the original footprint");

    // Replacing active reclaim must switch the work and its emitter together.
    treeWorld.addFeature(1001,208,176,1000,1,1,false);
    treeWorld.addFeature(1002,240,176,1000,1,1,false);
    tak::net::Command reclaimCommand;
    reclaimCommand.kind=tak::net::Cmd::Reclaim;
    reclaimCommand.unitId=builder;
    reclaimCommand.player=0;
    reclaimCommand.targetId=1001;
    sim::applyCommand(treeWorld,reg,reclaimCommand);
    for(int i=0;i<60;++i)treeWorld.tick(1.f/30);
    check(treeWorld.unit(builder)->reclaimId==1001,"the first reclaim is under way");
    reclaimCommand.targetId=1002;
    sim::applyCommand(treeWorld,reg,reclaimCommand);
    for(int i=0;i<120;++i)treeWorld.tick(1.f/30);
    worker=treeWorld.unit(builder);
    // The work is the builder's countdown on its own order: replacing the
    // order leaves nothing behind on the first feature.
    check(treeWorld.feature(1001)->alive && !treeWorld.feature(1001)->fadeLeft &&
          worker->reclaimId==1002 && worker->orders.size()==1 &&
          worker->orders.front().reclaimFeat==1002 && worker->orders.front().mission.stage>=3 &&
          worker->orders.front().reclaimCountdown<sim::retailReclaimCountdown(1000),
          "replacement reclaim switches work and effects to the new target");
    const auto stoppedEmissions=treeWorld.unit(builder)->constructionEmissions[0];
    reclaimCommand.kind=tak::net::Cmd::Stop;
    sim::applyCommand(treeWorld,reg,reclaimCommand);
    for(int i=0;i<180;++i)treeWorld.tick(1.f/30);
    worker=treeWorld.unit(builder);
    check(treeWorld.feature(1002)->alive && worker->orders.empty() && worker->reclaimId==0 &&
          worker->constructionEmissions[0]==stoppedEmissions &&
          worker->cosmeticConstructionEmitter->particles.empty(),
          "Stop cancels reclaim work and emission while old particles drain");

    treeWorld.addFeature(1003,208,176,0,1,1,false);
    reclaimCommand.kind=tak::net::Cmd::Reclaim;
    reclaimCommand.targetId=1003;
    sim::applyCommand(treeWorld,reg,reclaimCommand);
    for(int i=0;i<150;++i)treeWorld.tick(1.f/30);
    check(!treeWorld.feature(1003)->alive &&
          treeWorld.unit(builder)->constructionEmissions[0]==stoppedEmissions,
          "zero-energy reclaim completes without worker particles");

    std::printf(failures ? "\nFAILED (%d)\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
