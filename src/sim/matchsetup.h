#pragma once

// SDL-free match setup shared by the takclient client and the takserver referee
// sim (docs/multiplayer-design.md, "Determinism contract"). Both must build a BIT-IDENTICAL initial
// world -- same terrain, feature nav-blocking, mana deposits, players, teams,
// and monarch spawns -- so their state hashes agree in lockstep.

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "net/protocol.h"   // Command, Cmd, Event
#include "sim/sim.h"
#include "sim/scenario.h"
#include "tnt/tnt.h"        // tnt::Map (registerMapFeatures)

namespace tak::sim {

// Apply one sequenced command to a world (the shared lockstep step used by every
// client AND the server's referee sim, so they mutate identically). Enforces
// per-unit ownership against Command::player.
void applyCommand(World& world, const TypeRegistry& reg, const tak::net::Command& c);

// Apply one sequenced lifecycle event (Forfeit/Leave -> the player's units go
// inert; AiTakeover -> the player becomes computer-owned). Symmetric across peers so
// the sim stays in lockstep.
void applyEvent(World& world, const tak::net::Event& e);


// One player slot in a match (index = sim player id).
struct MatchSlot {
    bool used = false;
    int faction = 0;   // 0 ara, 1 tar, 2 ver, 3 zon, 4 cre
    int team = 0;
    float manaMult = 1.0f;   // per-player income multiplier (Absurd AI = 2); Player::manaMult
    bool automaticGates = false;
    bool defensiveAi = false; // Defensive AI uses a non-pursuing default stance from birth
};

// Start Locations (net::GameOptions::randomStarts, read through normalizeStartMode).
// Fixed is retail: the k-th seated slot takes the map's k-th start, so an empty seat
// takes none. Random shuffles the starts first. BySlot is not retail: slot N takes
// the map's N-th start whichever seats are empty (startSpotsFor).
enum class StartMode : uint8_t { Fixed = 0, Random = 1, BySlot = 2 };

struct MatchConfig {
    const hpi::Vfs* vfs = nullptr;  // the retail-root read-path (shared by peers)
    std::string mapPath;            // VFS path to the map .tnt (.ota sibling = start pos)
    std::vector<MatchSlot> slots;   // index = player; sized to the player count
    // No `gods` flag: retail decides with a 10% roll off Gods.tdf, not an option
    // (see the port of 0x519420 in setupMatch). startSeed drives the draw.
    bool loadCrt = true;             // opt-in Test Map placements/rules; campaign OTA setup opts out
    // The room's MAP SCENARIO option (GameOptions::mapScenario): run the map's own .crt
    // placements/rules and Use Only list when its .ota declares hasscenario. A Test Map
    // copy's [TAKPlaytest] opt-in runs them regardless. Off by default, so fixtures and
    // rooms that leave it off keep plain skirmish setup.
    bool mapScenario = false;
    // The room's POWER CODES option (GameOptions::powerCodes): World::applyPowerCode
    // honours Cmd::PowerCode only when it is on. Never on for a campaign mission.
    bool powerCodes = false;
    // The room starts with the map revealed (FOG OF WAR: EXPLORED or FULL VISION), so
    // +Mapping starts with the shroud off (World::setPowerCodes). Only read with
    // powerCodes on; replays carry it in the POWER CODES byte (bit 1).
    bool powerMapRevealed = false;
    // The room's FOG OF WAR option as the sim reads it (World::setRoomVision):
    // EXPLORED and FULL VISION start with the map revealed, FULL VISION also with
    // line of sight off. Only area clear's fog test reads them. Replays carry the
    // option itself (format 15).
    bool roomMapRevealed = false, roomLosOff = false;
    ScenarioScript::TraceSink scenarioTrace; // local diagnostics only, installed before initial rules
    int scenarioViewPlayer = -1;     // cosmetic message filtering only; never hashed
    // The pool every player holds before the units a game opens with credit
    // their mogriumstorage (World::creditCreationStorage): 0, as retail.
    float startMana = 0;
    int unitCap = 2000;             // per-player live-unit limit (0 = unlimited)
    bool monarchExpendable = true;  // false = losing your Monarch loses the game
    PathfindingMode pathfindingMode = PathfindingMode::Retail;
    bool patrolRepairs = true;
    bool doubleSight = false;       // double authored sight and radar distances (not weapon range)
    bool stressTest = false;        // spawn each player at ~95% of the unit cap in combat
                                    // units at setup (SP all-AI load test)
    // Stress fill timing. By default the fill is staged over the first kStressStageTicks
    // ticks (World::setStagedSpawns), so a stress run does not open with one 55-89 ms
    // tick of ~15k Create scripts and a first mass movement pass. Positions, types and
    // order are the same; only when each body appears changes. Fixtures that inspect or
    // order the whole army right after setupMatch (simperf, patrol perf, placement and
    // retail-gap checks) set this to spawn it immediately instead, and so do games whose
    // room option is net::kStressImmediate.
    bool stressImmediate = false;
    // Start Locations: which start each seated slot takes (StartMode). Random shuffles
    // the map's spawns so they can't be memorised. Deterministic (seeded below), so
    // every peer and the referee produce the same assignment.
    StartMode startMode = StartMode::Fixed;
    uint32_t startSeed = 0;   // match seed the shuffle draws from
    int benchmark = 0;              // benchmark INTENSITY: 0=off, 1=Low..5=Absurd. Builds a
                                    // deterministic ramp (1 unit/faction every 1/spawnsPerSec
                                    // seconds for 60s), executed by World::tick.
};

// Benchmark intensity levels (0=off, 1..6). Each faction spawns one unit every
// 1/spawnsPerSec seconds over the 60s run. Shared by the sim (setupMatch) and the client
// (menu + results label) so both agree.
constexpr int kBenchLevels = 6;
// Ticks the staged stress fill is spread over (each player's army arrives evenly across
// ticks 1..kStressStageTicks, about 512 bodies a tick for a full 8-player fill).
constexpr int kStressStageTicks = 30;
inline int benchmarkSpawnsPerSec(int level) { return (level >= 1 && level <= kBenchLevels) ? (1 << (level - 1)) : 0; }
inline int benchmarkSpawns(int level) { return benchmarkSpawnsPerSec(level) * 60; }
// A run lasts kBenchmarkSeconds of REAL time at any game speed: the server sends the sims
// Cmd::BenchClock from its wall clock (stages due, and the end).
constexpr int kBenchmarkSeconds = 60;
inline const char* benchmarkLevelName(int level) {
    static const char* n[kBenchLevels + 1] = {"", "LOW", "MEDIUM", "HIGH", "VERY HIGH", "ABSURD",
                                              "EXTRA ABSURD"};
    return (level >= 1 && level <= kBenchLevels) ? n[level] : "";
}
inline const char* benchmarkLevelInterval(int level) {
    static const char* iv[kBenchLevels + 1] = {"", "1S", "0.5S", "0.25S", "0.125S", "0.0625S",
                                               "0.03125S"};
    return (level >= 1 && level <= kBenchLevels) ? iv[level] : "";
}

// The starting Monarch of each faction (index = faction id).
extern const char* const kMonarchs[5];

// Load the unit registry from the VFS: MOVEINFO, the roster (units, or unitscb
// alone for the Crusades balance -- no fallback, as retail), then canbuild menus
// filtered by that roster. Iron Plague and community units are already merged
// into these namespaces by the VFS. Deterministic.
void setupRegistry(TypeRegistry& reg, const hpi::Vfs& vfs, bool crusades);

// setupMatch's refusal of a scenario that PLACES a unit type its registry lacks.
// Retail cannot play one either: its CRT loader dereferences the failed name lookup
// (0x4cc8e1 -> 0x4cc8e6). The type rides along so a caller can say why: under the
// Crusades balance it is usually a standard unit the Crusades roster leaves out
// (monelem, monmum, tarcship, tarhel, tarnecr2, zonspide). A custom type the registry
// lacks is not refused: retail skips it (see setupMatch).
struct ScenarioTypeMissing : std::runtime_error {
    std::string type;   // as the CRT spells it
    explicit ScenarioTypeMissing(const std::string& name)
        : std::runtime_error("Scenario unit type is missing: " + name), type(name) {}
};
// The player-facing text for that refusal. `standard` is the standard-balance registry
// when the game plays the Crusades one (else null): a type it has is named as a unit
// that does not exist with CRUSADES on.
std::string scenarioTypeMissingText(const ScenarioTypeMissing& missing, const TypeRegistry* standard);

// Whether either balance's unit directory (units/, unitscb/) has `name`.fbi. A Test Map
// copy refuses a Use Only entry its roster lacks as the author's typo, but not one the
// other balance has (zonspide with CRUSADES on): that entry permits nothing, as an
// unknown entry does on a retail map. setupMatch and inspectScenario agree on it.
bool balanceUnitFile(const hpi::Vfs& vfs, const std::string& name);

// Start positions from the map's .ota (world pixels), ordered by StartPos index.
std::vector<std::pair<float, float>> parseStartPositions(const hpi::Vfs& vfs,
                                                         const std::string& mapPath);

// The entry of setupMatch's spot list each slot opens at (index = slot, -1 = not
// seated). The list is the map's `authored` starts in StartPos order (shuffled for
// Random), extended with extra spots when more slots are seated than there are
// starts. Fixed and Random: the k-th seated slot takes entry k. BySlot: seated slot
// N takes entry N when the map has it; the others, in slot order, take the lowest
// entry no seated slot claims by its own number, so a start before an extra spot.
// Every entry is below max(authored, seated slots). The room screen shows BySlot's
// fallbacks with this same function.
std::vector<int> startSpotsFor(const std::vector<MatchSlot>& slots, size_t authored, StartMode mode);

// Build the world for a match. Idempotent w.r.t. terrain (setTerrain rebuilds the
// nav grid), so it may run after a client has already loaded the map for render.
// Returns the start position assigned to each USED slot, in slot order (for the
// camera). Every peer that calls this with the same config gets the same world.
// Register the map's obstacle features (+ burn-type table) into a world built
// WITHOUT setupMatch (client local-harness / mission / scenario paths).
void registerMapFeatures(World& world, const tak::tnt::Map& map, const hpi::Vfs& vfs,
                         const TypeRegistry* reg = nullptr);

// `startTable`, when given, receives every spot a seat could have opened at: the
// map's starts (a generated map's, the benchmark ring), with any extra spots for
// more seats than starts, in no particular order. A map scenario that places
// units leaves it untouched.
std::vector<std::pair<float, float>> setupMatch(World& world, const TypeRegistry& reg,
                                                const MatchConfig& cfg,
                                                std::vector<std::pair<float, float>>* startTable = nullptr);

// Build the world for a campaign mission: terrain + features (reusing setupMatch), the
// placed units and player slots from the mission `.ota`, and the in-sim MissionScript
// (attached + started). Returns false if the mission bundle isn't found; sets humanOut
// to the human player index -- always 0, the .ota's Player1, as in retail. See
// docs/campaign-design.md.
// What a mission needs beyond the world itself: which player slots the server
// should run a skirmish AI for, where each slot's forces are, and which build
// profile to give them. EVERY shipped mission marks at least one player
// "strategic opponent" -- without an AI those bases never build or counterattack
// and the mission plays as a static set-piece.
struct MissionSetup {
    std::vector<int> aiSlots;                        // world slots to drive with AI
    std::vector<std::pair<float, float>> slotPos;    // per-slot centroid of placed units
    std::string aiProfile;                           // .ota aiprofile= (ai/<name>.txt)
    // Per world slot, the .ota Player<N> value as written (empty for none): the
    // Classic AI reads a computer player's `massattack` keyword from it.
    std::vector<std::string> playerDefs;
    bool fullVision = false;   // .ota lineofsight=0: the mission is played revealed
    bool preMapped = false;    // .ota mapping=1: terrain starts explored (units still fogged)
};

bool setupMission(World& world, const TypeRegistry& reg, const hpi::Vfs& vfs,
                  const std::string& stem, int& humanOut, MissionSetup* out = nullptr);

}  // namespace tak::sim
