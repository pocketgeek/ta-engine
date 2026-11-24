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

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "net/lockstep.h"
#include "sim/sim.h"

namespace ta::ai {

struct ClassicProfileScript;   // ai/classicprofile.h

// Retail AI profile weights and policy limits. Normal/Easy retain scaled limits;
// sustained-growth modes raise them to their force/production targets. A zero
// limit remains disabled; -1 is unlimited. Actual UnitType limits always apply.
// Missing weight => the AI never builds that unit.
struct Profile {
    std::unordered_map<std::string, int> weight, limit;
};

// Parse a game's ai/<name>.txt (else ai/default.txt) from `vfs` in the
// builtinprofile.h line format. One file covers every faction. Never throws; a
// missing file yields an empty profile (the AI then builds nothing). The retail
// brain reads the map's profile through classicprofile.h instead, so this serves
// only the check that the built-in tables still reproduce the shipped default profile.
Profile loadProfile(const ta::hpi::Vfs& vfs, const std::string& name = "default");

// Opponent levels; the value is the wire aiLevel (SlotInfo.aiLevel). Every level
// runs Total Annihilation's own computer player (TaRetailBrain, ai/taretail.h):
// RETAIL EASY (8), RETAIL MEDIUM (6, named Classic) and RETAIL HARD (9) are its
// Easy / Medium / Hard exactly, with retail's 0.5 / 0.7 / 1.0 income scale; the
// engine's levels map onto its plans (Easy -> easy, Normal -> medium, Hard and
// Absurd -> hard; Absurd and Absurdly Defensive earn double, incomeMultFor),
// and Defensive (0), Absurdly Defensive (5) and Passive (7, Easy that stays
// home) never launch strike groups. Renumbering is versioned by kNetVersion.
enum class Difficulty : uint8_t {
    Defensive = 0, Easy = 1, Normal = 2, Hard = 3, Absurd = 4, AbsurdlyDefensive = 5, Classic = 6, Passive = 7,
    // Total Annihilation's own computer player (ai/taretail.h) at retail Easy and
    // Hard; Classic is the same brain at retail Medium.
    RetailEasy = 8, RetailHard = 9
};
inline constexpr uint8_t kAiLevels = 10;
// The TA retail brain's levels and their retail difficulty index (0 Easy, 1 Medium, 2 Hard).
inline constexpr bool retailLevel(Difficulty d) {
    return d == Difficulty::Classic || d == Difficulty::RetailEasy || d == Difficulty::RetailHard;
}
inline constexpr int retailDifficulty(Difficulty d) {
    return d == Difficulty::RetailEasy ? 0 : d == Difficulty::RetailHard ? 2 : 1;
}

// Map a wire aiLevel to a Difficulty, clamping anything out of range to Normal.
inline Difficulty difficultyFromLevel(uint8_t lvl) {
    return lvl < kAiLevels ? Difficulty(lvl) : Difficulty::Normal;
}

// The defensive levels: no offensive waves, a non-pursuing stance from birth
// (MatchSlot::defensiveAi), and production that follows income without caps.
inline constexpr bool defensiveLevel(Difficulty d) {
    return d == Difficulty::Defensive || d == Difficulty::AbsurdlyDefensive;
}

// Per-player mana-income multiplier for a difficulty. This is the ONE economy cheat:
// Absurd and Absurdly Defensive earn double from every income source. It scales
// HASHED sim state (mana), so every peer must apply the same factor to the same
// player -- it is derived from the broadcast aiLevel and set on the sim player at
// match setup, never AI-local.
inline constexpr float incomeMultFor(Difficulty d) {
    // TotalA.exe scales every resource credit to a computer's units by its
    // difficulty: Easy 0.5, Medium 0.7, Hard 1.0 (docs/retail-ai.md section 2).
    if (d == Difficulty::RetailEasy) return 0.5f;
    if (d == Difficulty::Classic) return 0.7f;
    if (d == Difficulty::RetailHard) return 1.0f;
    return d == Difficulty::Absurd || d == Difficulty::AbsurdlyDefensive ? 2.0f : 1.0f;
}

// Behaviour knobs derived from Difficulty (paramsFor).
struct DiffParams {
    int  thinkPeriod;      // sim ticks between decisions (30 = 1 Hz); lower = faster reactions
    int  waveSize;         // base army size to gather before an attack; the actual "big
                           // push" threshold scales UP with mana income (a rich AI masses
                           // a larger army, a poor one strikes with what it has)
    int  producersPerThink;// how many idle producers act each think (economy/APM pace)
    int  limitScale;       // percent applied to the profile's unit limits (100 = as shipped)
    bool attack;           // commit offensive waves (Defensive only intercepts local threats)
    int  raidSize;         // fighters peeled off for a small harassing raid while the main
                           // army musters (0 = no raiding)
    bool unbounded;        // no AI-imposed caps: the planner's targets scale with income
                           // without ceilings, profile limits do not apply, and spending
                           // is paced by mana alone (the defensive levels)
};
DiffParams paramsFor(Difficulty d);

// Sink for the commands a Controller decides to issue this tick. Offline this
// applies them immediately; on the server it queues them into the tick sequencer.
using CommandSink = std::function<void(const ta::net::Command&)>;

// One AI brain, driving a single player. Every level runs Total Annihilation's
// own computer player (TaRetailBrain, ai/taretail.h): the retail levels exactly,
// the engine's levels mapped onto its difficulty plans (Easy -> easy, Normal ->
// medium, Hard/Absurd -> hard; Absurd keeps its 2x income) with the defensive
// levels and Passive never launching strike groups.
class Controller {
public:
    // `profile` and `enemyStarts` are what the engine's own levels once read;
    // the retail brain takes neither (it scouts and keeps its own tables).
    Controller(int player, const ta::sim::TypeRegistry& registry,
               const Profile& profile, uint32_t seed,
               Difficulty difficulty = Difficulty::Normal,
               std::vector<std::pair<float, float>> enemyStarts = {});
    Controller(Controller&&) noexcept;
    ~Controller();

    // Evaluate the AI for sim tick `simTick`. Reads `world` (never mutates it)
    // and emits any orders through `sink`.
    void tick(const ta::sim::World& world, uint32_t simTick, const CommandSink& sink);

    // The AI profile script (the map's aiprofile file, chosen by the server,
    // ai/classicprofile.h) the retail brain reads its per-type weights from.
    void setClassicProfile(const ClassicProfileScript& script);
    const class TaRetailBrain* retailBrain() const { return retail_.get(); }

    // Read-only identity, for the debug roster dump (server/rosterdump.h).
    int player() const { return player_; }
    Difficulty difficulty() const { return stayHome_ ? Difficulty::Passive : diff_; }

private:
    int player_;
    Difficulty diff_;     // Passive runs as Easy
    bool stayHome_;       // Passive
    std::unique_ptr<class TaRetailBrain> retail_;   // ai/taretail.h
};

}  // namespace ta::ai
