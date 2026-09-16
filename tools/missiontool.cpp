// missiontool: load a full campaign mission (terrain + placed units + the in-sim
// MissionScript) via setupMission, run it through World::tick, and check the win/lose
// paths. Verifies the reverse-engineered scripting + loader + tick integration.
//
//   missiontool <retail-install-dir> [mission-stem]     (default: takmission01_mt)
//
// A dev harness; not shipped in a game.

#include "campaign/campaign.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace ta;

namespace {
int countType(const sim::World& w, const std::string& id) {
    int n = 0;
    for (const auto& u : w.units()) if (u.alive() && u.type && u.type->id == id) ++n;
    return n;
}
int findType(const sim::World& w, const std::string& id) {
    for (const auto& u : w.units()) if (u.alive() && u.type && u.type->id == id) return u.id;
    return -1;
}
// A player-0 (human-owned) unit that is NOT the escorted NPC -- for tripping a
// region trigger without also satisfying the escort victory condition.
int findHumanMover(const sim::World& w) {
    for (const auto& u : w.units())
        if (u.alive() && u.player == 0 && u.type && u.type->canMove && u.type->id != "npcemen")
            return u.id;
    return -1;
}
void tick(sim::World& w, float seconds) {
    const float dt = 1.0f / 30.0f;
    for (int i = 0; i < int(seconds / dt); ++i) { w.tick(dt); if (w.missionOutcome()) break; }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: missiontool <retail-install-dir> [mission-stem]\n"); return 2; }
    std::string dataRoot = argv[1];
    std::string stem = argc >= 3 ? argv[2] : "takmission01_mt";

    hpi::Vfs vfs = hpi::mountRetailRoot(dataRoot);

    // --campaigns: list the campaign spine (camps/*.tdf) and exit.
    if (stem == "--campaigns") {
        for (const auto& c : ta::loadCampaigns(vfs)) {
            std::printf("%-28s (%s)  %d missions%s%s\n", c.title.c_str(), c.id.c_str(), c.count(),
                        c.altFinal.empty() ? "" : "  +alt ending: ", c.altFinal.c_str());
            for (int i = 0; i < c.count(); ++i)
                std::printf("    %2d. %-18s %s\n", i + 1, c.missions[size_t(i)].stem.c_str(),
                            c.missions[size_t(i)].title.c_str());
        }
        return 0;
    }

    sim::TypeRegistry reg;
    sim::setupRegistry(reg, vfs);

    // ---- full load via setupMission, then run through World::tick ----
    sim::World w;
    int human = 0;
    if (!sim::setupMission(w, reg, vfs, stem, human)) return 1;
    std::printf("=== %s ===\n", stem.c_str());
    std::printf("placed+monarchs: %zu units at load\n", w.units().size());
    tick(w, 2.0f);   // run Start + settle
    std::printf("after 2s: units=%zu  NPCEMEN=%d ARASWORD=%d  outcome=%d\n",
                w.units().size(), countType(w, "npcemen"), countType(w, "arasword"), w.missionOutcome());

    // ---- WIN: drop the escorted NPC onto the objective radius ----
    int emen = findType(w, "npcemen");
    if (emen >= 0 && w.missionOutcome() == 0) {
        if (auto* u = w.unit(emen)) { u->x = 130 * 16 + 8; u->z = 76 * 16 + 8; }
        w.tick(1.0f / 30.0f);
        std::printf("WIN test  (NPCEMEN on objective): outcome=%d %s\n",
                    w.missionOutcome(), w.missionOutcome() == 1 ? "PASS" : "FAIL");
    }

    // ---- LOSE: kill the escorted NPC in a fresh run ----
    sim::World w2;
    int h2 = 0;
    if (sim::setupMission(w2, reg, vfs, stem, h2)) {
        tick(w2, 2.0f);
        int e2 = findType(w2, "npcemen");
        if (e2 >= 0 && w2.missionOutcome() == 0) {
            if (auto* u = w2.unit(e2)) u->hp = 0;   // die -> World::tick fires UnitDestroyed
            tick(w2, 1.0f);
            std::printf("LOSE test (NPCEMEN killed):       outcome=%d %s\n",
                        w2.missionOutcome(), w2.missionOutcome() == -1 ? "PASS" : "FAIL");
        }
    }
    // ---- TRIGGER + escort march: walk a human unit into region 0 (which spawns the
    //      guard squad and gives ARABROAD "s, w 4, m 133 68, ..."), then verify the
    //      SetMission wait+move verbs actually drive it. ----
    sim::World w3;
    int h3 = 0;
    if (sim::setupMission(w3, reg, vfs, stem, h3)) {
        for (int i = 0; i < 30; ++i) w3.tick(1.0f / 30.0f);   // run Start (arms triggers)
        int mover = findHumanMover(w3);
        if (mover >= 0) {
            if (auto* u = w3.unit(mover)) { u->x = 130 * 16 + 8; u->z = 76 * 16 + 8; }
            for (int i = 0; i < 15; ++i) w3.tick(1.0f / 30.0f);   // fire TriggerHit -> squad spawns
            int bro = findType(w3, "arabroad");
            float x0 = 0, z0 = 0;
            size_t program = 0;
            if (auto* b = w3.unit(bro)) { x0 = b->x; z0 = b->z; program = b->orders.size(); }
            // 4s hold (should barely move) then a march that oscillates around the spawn.
            // Track the farthest it gets, its displacement mid-wait, and how many of the
            // queued orders (w/m/w/m/a) it consumes -- proof the sequence executes.
            float maxD = 0, dAtWait = 0;
            for (int i = 0; i < 360; ++i) {
                w3.tick(1.0f / 30.0f);
                if (auto* b = w3.unit(bro)) {
                    float d = std::sqrt((b->x - x0) * (b->x - x0) + (b->z - z0) * (b->z - z0));
                    if (i == 90) dAtWait = d;   // ~3s in, still inside the 4s wait
                    if (d > maxD) maxD = d;
                }
            }
            size_t left = 0;
            if (auto* b = w3.unit(bro)) left = b->orders.size();
            std::printf("TRIGGER test: ARABROAD spawned=%s  wait-held@3s=%.0fpx  marched=%.0fpx  "
                        "orders %zu->%zu  %s\n",
                        bro >= 0 ? "yes" : "no", dAtWait, maxD, program, left,
                        (bro >= 0 && dAtWait < 20.0f && maxD > 25.0f && left < program) ? "PASS" : "FAIL");
        }
    }
    return 0;
}
