#pragma once

// Deterministic in-sim runner for a campaign mission's "god" `.cob` script plus the
// data-driven victory/defeat rules from the mission `.ota [GlobalHeader]`. It is built
// on every peer from identical data and ticked inside the sim (World::tick), so its
// spawns / orders / triggers stay in lockstep without relaying any script actions --
// unlike unit-animation cobs, which run viewer-side. The mission `.cob` has no pieces,
// so it never executes the VM's (float) animation opcodes; it uses only integer
// game-logic opcodes, and the COB VM already builds with -ffp-contract=off. See
// docs/campaign-design.md for the reverse-engineered scripting/win-lose model.

#include "cob/cob.h"
#include "cob/vm.h"
#include "tdf/tdf.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ta::sim {

class World;
class TypeRegistry;
struct UnitType;

class MissionScript {
public:
    // cobBytes: the mission `.cob`. header: the parsed `.ota [GlobalHeader]` node (for the
    // win/lose conditions + the default script player). reg: type registry (name lookup).
    // humanPlayer: the campaign player's slot (0-based). origin: for diagnostics.
    // playerMap: .ota player id (1-based) -> World slot, so the script's Create player
    // refs (0-based .ota) land in the same compacted slots as the placed units.
    MissionScript(std::vector<uint8_t> cobBytes, const ta::tdf::Node& header,
                  const TypeRegistry& reg, int humanPlayer, std::string origin,
                  std::vector<int> playerMap = {});

    bool ok() const { return vm_ != nullptr; }

    // Per-placed-unit `InitialMission=` order queues from the .ota, applied at start()
    // in the same mini-language SetMission uses. This is the shipped missions' NPC
    // choreography (patrol routes, ambush holds, timed reinforcement waves).
    void setInitialOrders(std::vector<std::pair<int, std::string>> o) {
        initialOrders_ = std::move(o);
    }

    void start(World& w);              // run the "Start" script once (initial spawns/triggers)
    void step(World& w, float dt);     // tick the VM, sweep triggers, evaluate conditions
    void unitBuilt(World& w, int id);  // a unit finished building -> UnitCreated(id, 0)
    void unitDied(World& w, int id);   // a unit died -> UnitDestroyed(id)

    int outcome() const { return outcome_; }   // 0 running / +1 victory / -1 defeat
    // Fold mission state into the sim's state hash (so mission progress is checksummed).
    void foldHash(uint64_t& h) const;

private:
    // A trigger region armed by SetTrigger (circle: r>=0; rect: x1/z1 the far corner).
    struct Region { bool armed = false, rect = false; float x = 0, z = 0, x2 = 0, z2 = 0, r = 0; };

    // A parsed `.ota` victory/defeat rule. `kind` names the retail *Condition_* class;
    // `type` (optional unit type) + up to 4 numeric args carry its parameters.
    struct Cond {
        enum Kind { MoveUnitToRadius, KillEnemyCommander, DestroyAllUnits, KillAllMobileUnits,
                    KillAllOfType, KillUnitType, VictoryTimerRunsOut, UnitTypePassesX, UnitTypePassesZ,
                    CommanderKilled, AllUnitsKilled, AllUnitsKilledOfType, UnitTypeKilled,
                    DeathTimerRunsOut, AnyUnitPassesX, AnyUnitPassesZ } kind;
        const UnitType* type = nullptr;
        float a = 0, b = 0, c = 0, d = 0;
        bool victory = false;
        // "Destroy all X" rules only arm once such a unit has actually existed, so a
        // mission can't win/lose at t=0 before the target has spawned (many scripts
        // create their enemies in Start / a trigger).
        bool armed = false;
    };

    // ---- VM callback plumbing ----
    int32_t mapCommand(World& w, int nameIdx, const std::vector<int32_t>& args);
    int32_t getValue(World& w, int32_t valId, const std::vector<int32_t>& args);
    void setUnitValue(int32_t valId, int32_t value);

    // ---- verb + order handling ----
    int32_t doCreate(World& w, const std::string& rest, const std::vector<int32_t>& args);
    void applyOrders(World& w, int unitId, const std::string& orders);
    void doSetAttribute(World& w, const std::string& sub, int unitId, int pct);

    // ---- per-step evaluation ----
    void sweepTriggers(World& w);
    void evalConditions(World& w, float dt);
    void parseConditions(const ta::tdf::Node& header);

    const UnitType* findType(const std::string& name) const;   // registry lookup (lowercased)
    static float cellToWorld(float cell) { return cell * 16.0f + 8.0f; }   // .ota cell -> world px

    cob::File cob_;
    std::unique_ptr<cob::Vm> vm_;
    const TypeRegistry& reg_;
    World* world_ = nullptr;   // set on each entry (start/step/unit*) for the VM callbacks
    int human_;
    std::string origin_;
    std::vector<int> playerMap_;   // .ota player (1-based) -> World slot (empty = identity)

    std::array<Region, 16> regions_{};
    std::array<std::unordered_set<int>, 16> inside_{};   // edge-triggered: units currently in region
    std::unordered_map<std::string, int32_t> vars_;      // WriteValue / ReadValue store
    int getUnitContext_ = 0;                             // unit id for GET_UNIT_VALUE(7)

    std::vector<Cond> conds_;

    // A SetMission "b TYPE H X Y" timed reinforcement: spawn `type` for `player` at
    // (x,z) once the mission clock reaches `at`.
    struct PendingSpawn { const UnitType* type = nullptr; int player = 0; float x = 0, z = 0, at = 0; };
    std::vector<PendingSpawn> pendingSpawns_;
    std::vector<std::pair<int, std::string>> initialOrders_;   // .ota InitialMission=, applied at start
    // Named units: the .ota's per-unit Ident= field and the order language's `i`
    // verb both register here, and a `g NAME` clause guards whatever they name.
    std::unordered_map<std::string, int> idents_;
public:
    void setIdents(std::unordered_map<std::string, int> m) { idents_ = std::move(m); }
private:

    float clock_ = 0;         // mission time (s), for timer conditions
    int outcome_ = 0;         // 0 / +1 / -1
    bool started_ = false;
};

}  // namespace ta::sim
