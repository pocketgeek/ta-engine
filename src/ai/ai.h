#pragma once

// The skirmish AI, extracted from the SDL viewer so it can also run headless on
// the multiplayer server. A Controller reads a const World and EMITS net::Commands
// through a sink instead of mutating the world directly -- so the same AI drives
// a player whether the commands are applied in-process (single-player) or fed into
// the server's command sequencer (server-hosted AI). See docs/multiplayer-design.md.
//
// The AI runs ONLY on the server (the referee), never on clients: its *decisions*
// are server-local, and only the net::Commands it emits are relayed and applied by
// every peer -- so lockstep never depends on the AI being reproducible. We still
// keep it RNG-driven so a --mpai run is repeatable for testing.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "net/lockstep.h"
#include "sim/sim.h"

namespace ta::ai {

// Build weights are FIXED-POINT HUNDREDTHS. 100 (== 1.0) is the weight of a unit
// the profile never mentions, which is the load-bearing rule of TA's format: the
// file states ADJUSTMENTS to a flat default, not the whole table. Reading a
// missing entry as "weight 0" instead makes every unlisted unit unbuildable --
// and since ai/DEFAULT.TXT names only ~90 of the ~270 units, an AI parsed that
// way sits on its Commander and builds nothing at all, with no error anywhere.
// (defined below, next to paramsFor -- forward-declared so Profile can select a plan)
enum class Difficulty : uint8_t;

constexpr int kWeightOne = 100;

// One difficulty's slice of a profile. A key is either a UNIT ID or one of the
// FBI `Category=` tags that unit declares -- retail makes no distinction between
// the two, so `Weight CONSTR 3` boosts every construction unit and `Weight
// ARMMAKR 0.25` damps one. Both live in the same map and are resolved by
// profileWeight() / profileLimit() below.
struct Plan {
    std::unordered_map<std::string, int> weight;  // hundredths; absent => kWeightOne
    std::unordered_map<std::string, int> limit;   // absent => unlimited; 0 => never build
};

// The retail AI profile (ai/DEFAULT.TXT and the per-map/per-mission variants).
// The file is divided into `plan easy` / `plan medium` / `plan hard` sections;
// anything stated BEFORE the first `plan` line applies to all three (which is how
// the base game's DEFAULT.TXT states its six global weights).
struct Profile {
    Plan easy, medium, hard;
    const Plan& forDifficulty(Difficulty d) const;
};

// Parse an ai/*.txt profile from the runtime VFS. One file covers both sides.
// Never throws; a missing file yields an all-default profile, which is a playable
// AI rather than a paralysed one.
Profile loadProfile(const ta::hpi::Vfs& vfs, const std::string& name = "default");

// Parse profile text directly (what loadProfile does once it has the bytes), so
// the format's rules can be pinned without a retail install on the runner.
Profile parseProfile(const std::string& text);

// Resolve a unit's weight/limit against a plan: an entry under the unit's own id
// wins outright, otherwise every matching category applies. Weights MULTIPLY (a
// unit is typically damped by its side's 0.2 and boosted back by its role's 3),
// and limits take the TIGHTEST match, because a cap is a cap.
int profileWeight(const Plan& plan, const ta::sim::UnitType& t);
int profileLimit(const Plan& plan, const ta::sim::UnitType& t);   // -1 = unlimited

// Opponent skill, loosely modelled on retail's easy/normal/hard. It scales HOW the
// AI plays (economy pace, army size before it commits, aggression, reaction rate)
// rather than cheating its economy -- so all three respect the same rules the human
// does. See paramsFor().
// Ordered weakest -> strongest; the value is the wire aiLevel (SlotInfo.aiLevel).
// Passive = Easy that never attacks (defends only). Absurd = Hard with a mana-income
// cheat (see incomeMultFor). Renumbering is versioned by kNetVersion.
enum class Difficulty : uint8_t {
    Passive = 0, Easy = 1, Normal = 2, Hard = 3, Absurd = 4
};

// Map a wire aiLevel to a Difficulty, clamping anything out of range to Normal.
inline Difficulty difficultyFromLevel(uint8_t lvl) {
    return lvl <= uint8_t(Difficulty::Absurd) ? Difficulty(lvl) : Difficulty::Normal;
}

// Per-player mana-income multiplier for a difficulty. This is the ONE economy cheat:
// Absurd earns double from every income source. It scales HASHED sim state (mana), so
// every peer must apply the same factor to the same player -- it is derived from the
// broadcast aiLevel and set on the sim player at match setup, never AI-local.
inline constexpr float incomeMultFor(Difficulty d) {
    return d == Difficulty::Absurd ? 2.0f : 1.0f;
}

// Behaviour knobs derived from Difficulty (paramsFor).
struct DiffParams {
    int  thinkPeriod;      // sim ticks between decisions (30 = 1 Hz); lower = faster reactions
    int  waveSize;         // base army size to gather before an attack; the actual "big
                           // push" threshold scales UP with mana income (a rich AI masses
                           // a larger army, a poor one strikes with what it has)
    int  producersPerThink;// how many idle producers act each think (economy/APM pace)
    int  limitScale;       // percent applied to the profile's unit limits (100 = as shipped)
    bool scout;            // send an early lone scout toward an enemy start
    bool attack;           // commit attack waves at all (Passive never does -- defends only)
    int  raidSize;         // fighters peeled off for a small harassing raid while the main
                           // army musters (0 = no raiding; gated on `scout` difficulties)
};
DiffParams paramsFor(Difficulty d);

// What a buildable unit is FOR, derived from its UnitType (faction-agnostic) so the
// planner can balance an army instead of drawing types blindly. See Controller::categoryOf.
// TA has TWO resources, and which one is scarce decides what to build next. The
// planner inherited a single Economy category from Kingdoms' one-resource mana
// economy, which made a metal extractor one interchangeable draw out of eleven
// economy buildings on the Commander's menu -- so an AI could (and did) run for
// five minutes on +1.0 metal/sec, the Commander's own trickle, while its energy
// climbed past +55. Metal and power are scored separately.
enum class BuildCat { Economy, Power, Factory, Builder, Army, Defense };

// The empire's current shape, assessed once per think. The planner compares these to
// simple targets to decide which category a producer should build next -- so the AI
// bootstraps an economy, adds factories to spend its income, keeps only a few builders,
// and then pours the rest into army, rather than letting a weighted-random draw spiral
// into all-economy / all-builders / no-soldiers (the old seed-fragile failure).
struct Needs {
    float income = 0;          // BASE income (an Absurd AI's cheat divided back out)
    // Own live units per type, filled in the same assessNeeds pass -- weightedPick's
    // limit checks read this instead of re-scanning all units per menu entry.
    std::unordered_map<const ta::sim::UnitType*, int> counts;
    int   economy = 0;         // count: METAL income/storage structures
    int   power = 0;           // count: energy producers
    // The second resource. `income` above is metal, which is what the factory
    // ladder is denominated in; these say whether energy is the binding
    // constraint instead.
    float energyIncome = 0, energyDrain = 0, energyStock = 0;
    // Stock vs capacity, for the storage rule in weightedPick: a store is worth
    // building only when the resource is actually capping out and being wasted.
    float metalStock = 0, metalCap = 1, energyCap = 1;
    int   factories = 0;       // count: structures that train units
    int   builders = 0;        // count: mobile builders (incl. the Commander)
    int   army = 0;            // count: mobile combatants
    int   builderCap = 2;      // stop making builders past this (a handful, not a horde)
    int   desiredFactories = 1;// how many factories the current income wants to feed
    // Metal per second ONE factory draws while producing flat out, measured from
    // the registry (see Controller::factoryAppetite). The economy thresholds are
    // stated in this unit, so they hold for either game's magnitudes rather than
    // being numbers copied from one of them.
    float factoryDraw = 1.0f;
};

// Sink for the commands a Controller decides to issue this tick. Offline this
// applies them immediately; on the server it queues them into the tick sequencer.
using CommandSink = std::function<void(const ta::net::Command&)>;

// One AI brain, driving a single player.
class Controller {
public:
    // enemyStarts: the start positions of the players this AI is NOT allied with, so
    // it can march on a base under fog before it has actually spotted enemy units.
    Controller(int player, const ta::sim::TypeRegistry& registry,
               const Profile& profile, uint32_t seed,
               Difficulty difficulty = Difficulty::Normal,
               std::vector<std::pair<float, float>> enemyStarts = {});

    // Evaluate the AI for sim tick `simTick`. Does nothing off its think cadence.
    // Reads `world` (never mutates it) and emits any orders through `sink`.
    void tick(const ta::sim::World& world, uint32_t simTick, const CommandSink& sink);


    // Metal/sec one factory consumes running flat out -- the median over what the
    // registry's producers can actually build. Computed once and cached: it
    // depends only on the type data, which does not change during a match.
    // Public because it is the unit the economy thresholds are stated in, and a
    // data-backed test checks the figure this install yields is sane.
    float factoryAppetite() const;

private:
    // --- deterministic RNG (retail-style LCG) --------------------------------
    int rand(int n) {
        rng_ = rng_ * 1103515245u + 12345u;
        return n > 0 ? int((rng_ >> 16) % uint32_t(n)) : 0;
    }

    // --- decision helpers (all read-only over the world) ---------------------
    // Needs-based build planner: assess the empire, score each category against its
    // target, and let a producer build the most-needed thing its menu offers.
    Needs assessNeeds(const ta::sim::World&) const;
    mutable float factoryDraw_ = 0.0f;   // 0 = not yet computed
    BuildCat categoryOf(const ta::sim::UnitType*) const;
    int   desire(BuildCat, const Needs&) const;
    // `excludeCats` is a bitmask of 1<<int(BuildCat): categories already tried and
    // found unproducible this think, so the pick falls through to the next best.
    const ta::sim::UnitType* weightedPick(const ta::sim::World&,
                                           const ta::sim::Unit& producer, const Needs&,
                                           int excludeCats = 0);
    // True if an order was actually emitted. False means the pick could not be
    // acted on (no build site, nowhere to conjure) -- the caller then retries in
    // another category rather than burning the producer's turn on it.
    bool produce(const ta::sim::World&, const ta::sim::Unit& producer,
                 const ta::sim::UnitType* pick, const CommandSink&);
    bool placeSite(const ta::sim::World&, const ta::sim::UnitType*, float nx, float nz,
                   float& outX, float& outZ) const;
    // Fog of war for the AI: an enemy is targeted only when one of the AI's own units
    // is within its sight/radar. For direction (before anything is spotted) the AI
    // falls back to the known enemy start positions.
    bool nearestVisibleEnemy(const ta::sim::World&, float cx, float cz,
                             const ta::sim::UnitType* atype, float& tx, float& tz) const;
    bool nearestEnemyStart(float cx, float cz, float& tx, float& tz) const;
    void sendWaves(const ta::sim::World&, uint32_t simTick, const CommandSink&);
    // The AI's home: the centroid of its own buildings (its base). Used to keep the
    // Commander anchored near home for safety instead of wandering to distant builds.
    std::pair<float, float> homeOf(const ta::sim::World&) const;

    // A fighter is free to be committed to a wave when it's idle or only doing a plain
    // move -- NOT while it's already fight-moving or attacking (so re-commanding it each
    // think doesn't reset its march and thrash it in place).
    static bool waveFree(const ta::sim::Unit& u) {
        return u.orders.empty() ||
               (u.orders.front().targetId == 0 && !u.orders.front().attackMove);
    }
    void emit(const CommandSink& sink, ta::net::Cmd kind, int unitId,
              const std::string& type, float x, float z) const;

    int player_;
    const ta::sim::TypeRegistry& registry_;
    const Profile& profile_;
    uint32_t rng_;
    Difficulty diff_;
    DiffParams dp_;
    std::vector<std::pair<float, float>> enemyStarts_;
    bool scouted_ = false;        // one-shot early scout sent
    uint32_t lastRaidTick_ = 0;   // last tick a harassing raid was sent (raid cooldown)
};

}  // namespace ta::ai
