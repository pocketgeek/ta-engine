#pragma once

// Deterministic in-sim runner for a map's `.crt` scenario triggers: the
// per-player groups of conditions + actions (26 + 26 opcodes) reverse-
// engineered from Cartographer.exe and stored/round-tripped by ta::crt.
// Like MissionScript it is built on every peer from identical data, ticked
// inside World::tick, and folded into stateHash, so its flag/timer state,
// spawns, and win/lose stay in lockstep. Display actions push cosmetic
// messages (NOT hashed) that the client drains for the viewing player.
//
// A rule group fires its actions on every tick that ALL its conditions hold
// (the retail model -- authors gate one-shot effects with flags), unless the
// group has been disabled by a "Disable rule" action or its player is defeated.

#include "crt/crt.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ta::sim {

class World;
class TypeRegistry;
struct UnitType;

class ScenarioScript {
public:
    // scen: the parsed .crt (typed rules + regions). reg: type lookup.
    // viewPlayer: the local human's slot (for message filtering; -1 = show all).
    // maxPlayer: number of world slots (scenario .crt players are clamped into it).
    // mapWCells/mapHCells: map size in 16px cells (for "Anywhere"/whole-map).
    ScenarioScript(const ta::crt::Scenario& scen, const TypeRegistry& reg,
                   int viewPlayer, int maxPlayer, int mapWCells, int mapHCells);

    bool active() const { return !players_.empty(); }

    void start(World& w);              // first-tick hook (arms "start of game")
    void step(World& w, float dt);     // evaluate every player's rule groups
    void unitDied(World& w, int id);   // death -> per-player/type kill & loss tallies
    void foldHash(uint64_t& h) const;  // fold flag/timer/disabled/outcome state

    // Cosmetic display messages queued since the last drain (game time, target
    // player, text). Target -1 = All Players. NOT part of the hash.
    struct Msg { float t = 0; int player = -1; std::string text; };
    std::vector<Msg> drainMessages();

    bool showClock() const { return showClock_; }   // "Display gameclock" latched

private:
    struct Timer { float value = 0; bool countUp = false; };
    struct PState {
        std::map<std::string, int32_t> flags;    // sorted -> deterministic hashing
        std::map<int32_t, Timer> timers;
        std::map<std::string, int32_t> killed;   // lowercased type -> count I killed
        std::map<std::string, int32_t> lost;     // lowercased type -> count I lost
    };

    // ---- evaluation ----
    bool evalCond(World& w, int player, const ta::crt::Rule& c);
    void runAction(World& w, int player, int group, const ta::crt::Rule& a);
    int countControl(World& w, int player, const UnitType* t, const std::string& loc) const;

    // ---- parameter helpers ----
    const UnitType* findType(const std::string& name) const;
    const ta::crt::Region* region(const std::string& name) const;   // nullptr => whole map
    bool inRegion(World& w, float x, float z, const std::string& loc) const;
    void regionCenter(const std::string& loc, float& x, float& z) const;
    int parsePlayer(const std::string& s) const;   // "Player N"->N-1, "All..."->-1
    void forceDefeatTeam(World& w, int player, bool allies);   // defeat player's team
    void forceDefeatOthers(World& w, int player);              // defeat everyone else

    const TypeRegistry& reg_;
    int view_;
    int maxPlayer_;
    int mapW_ = 0, mapH_ = 0;   // cells
    std::vector<std::vector<ta::crt::RuleGroup>> players_;   // rules, clamped to maxPlayer_
    std::vector<std::vector<uint8_t>> disabled_;              // per player/group
    std::vector<std::vector<uint8_t>> fired_;                 // per player/group: edge latch
    std::vector<PState> state_;
    std::vector<ta::crt::Region> regions_;
    std::vector<Msg> pending_;
    float clock_ = 0;
    uint32_t ticks_ = 0;
    uint64_t rng_ = 0x9e3779b97f4a7c15ULL;   // deterministic stream for "Random"
    bool showClock_ = false;
    bool started_ = false;
};

}  // namespace ta::sim
