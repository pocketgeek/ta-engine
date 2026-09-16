#pragma once

// SDL-free match setup shared by the taclient client and the taserver referee
// sim (docs/multiplayer-design.md, M4). Both must build a BIT-IDENTICAL initial
// world -- same terrain, feature nav-blocking, mana deposits, players, teams,
// and monarch spawns -- so their state hashes agree in lockstep.

#include <string>
#include <utility>
#include <vector>

#include "net/protocol.h"   // Command, Cmd, Event
#include "sim/sim.h"
#include "tnt/tnt.h"        // tnt::Map (registerMapFeatures)

namespace ta::sim {

// Apply one sequenced command to a world (the shared lockstep step used by every
// client AND the server's referee sim, so they mutate identically). Enforces
// per-unit ownership against Command::player.
void applyCommand(World& world, const TypeRegistry& reg, const ta::net::Command& c);

// Apply one sequenced lifecycle event (Forfeit/Leave -> the player's units go
// inert). Symmetric across peers so the sim stays in lockstep.
void applyEvent(World& world, const ta::net::Event& e);


// One player slot in a match (index = sim player id).
struct MatchSlot {
    bool used = false;
    int faction = 0;   // index into SIDEDATA's sides: 0 = ARM, 1 = CORE
    int team = 0;
    float manaMult = 1.0f;   // per-player income multiplier (Absurd AI = 2); Player::manaMult
};

struct MatchConfig {
    const hpi::Vfs* vfs = nullptr;  // the retail-root read-path (shared by peers)
    std::string mapPath;            // VFS path to the map .tnt (.ota sibling = start pos)
    std::vector<MatchSlot> slots;   // index = player; sized to the player count
    float startMana = 2800;
    int unitCap = 2000;             // per-player live-unit limit (0 = unlimited)
    bool monarchExpendable = true;  // false = losing your Monarch loses the game
    bool stressTest = false;        // spawn each player at ~95% of the unit cap in combat
                                    // units at setup (SP all-AI load test)
    // Random Start Locations: shuffle which start position each slot takes, so a
    // map's spawns can't be memorised. Deterministic (seeded below), so every peer
    // and the referee produce the same assignment.
    bool randomStarts = false;
    uint32_t startSeed = 0;   // match seed the shuffle draws from
    int benchmark = 0;              // benchmark INTENSITY: 0=off, 1=Low..5=Absurd. Builds a
                                    // deterministic ramp (1 unit/faction every 1/spawnsPerSec
                                    // seconds for 60s), executed by World::tick.
};

// Benchmark intensity levels (0=off, 1..6). Each faction spawns one unit every
// 1/spawnsPerSec seconds over the 60s run. Shared by the sim (setupMatch) and the client
// (menu + results label) so both agree.
constexpr int kBenchLevels = 6;
inline int benchmarkSpawnsPerSec(int level) { return (level >= 1 && level <= kBenchLevels) ? (1 << (level - 1)) : 0; }
inline int benchmarkSpawns(int level) { return benchmarkSpawnsPerSec(level) * 60; }
inline const char* benchmarkLevelName(int level) {
    static const char* n[kBenchLevels + 1] = {"", "LOW", "MEDIUM", "HIGH", "VERY HIGH", "ABSURD",
                                              "EXTRA ABSURD +WTH"};
    return (level >= 1 && level <= kBenchLevels) ? n[level] : "";
}
inline const char* benchmarkLevelInterval(int level) {
    static const char* iv[kBenchLevels + 1] = {"", "1S", "0.5S", "0.25S", "0.125S", "0.0625S",
                                               "0.03125S"};
    return (level >= 1 && level <= kBenchLevels) ? iv[level] : "";
}

// The starting commander of each side, in SIDEDATA order (index = side id).
// Read from gamedata/SIDEDATA.TDF rather than hardcoded: TA states each side's
// `commander=` in its data, so the roster is the install's business, not the
// engine's. Empty if the install has no readable SIDEDATA.
std::vector<std::string> sideCommanders(const hpi::Vfs& vfs);

// Load the unit registry from the VFS: MOVEINFO, then every unit, then the build
// tree out of SIDEDATA. The VFS has already merged base + expansions + patch +
// community units into one namespace by retail precedence. Deterministic.
void setupRegistry(TypeRegistry& reg, const hpi::Vfs& vfs);

// Start positions from the map's .ota (world pixels), ordered by StartPos index.
std::vector<std::pair<float, float>> parseStartPositions(const hpi::Vfs& vfs,
                                                         const std::string& mapPath);

// Build the world for a match. Idempotent w.r.t. terrain (setTerrain rebuilds the
// nav grid), so it may run after a client has already loaded the map for render.
// Returns the start position assigned to each USED slot, in slot order (for the
// camera). Every peer that calls this with the same config gets the same world.
// Register the map's obstacle features (+ burn-type table) into a world built
// WITHOUT setupMatch (client local-harness / mission / scenario paths).
void registerMapFeatures(World& world, const ta::tnt::Map& map, const hpi::Vfs& vfs,
                         const TypeRegistry* reg = nullptr);

std::vector<std::pair<float, float>> setupMatch(World& world, const TypeRegistry& reg,
                                                const MatchConfig& cfg);

// Build the world for a campaign mission: terrain + features (reusing setupMatch), the
// placed units and player slots from the mission `.ota`, and the in-sim MissionScript
// (attached + started). Returns false if the mission bundle isn't found; sets humanOut
// to the human player index. See docs/campaign-design.md.
// What a mission needs beyond the world itself: which player slots the server
// should run a skirmish AI for, where each slot's forces are, and which build
// profile to give them. EVERY shipped mission marks at least one player
// "strategic opponent" -- without an AI those bases never build or counterattack
// and the mission plays as a static set-piece.
struct MissionSetup {
    std::vector<int> aiSlots;                        // world slots to drive with AI
    std::vector<std::pair<float, float>> slotPos;    // per-slot centroid of placed units
    std::string aiProfile;                           // .ota aiprofile= (ai/<name>.txt)
    bool fullVision = false;   // .ota lineofsight=0: the mission is played revealed
    bool preMapped = false;    // .ota mapping=1: terrain starts explored (units still fogged)
};

bool setupMission(World& world, const TypeRegistry& reg, const hpi::Vfs& vfs,
                  const std::string& stem, int& humanOut, MissionSetup* out = nullptr);

}  // namespace ta::sim
