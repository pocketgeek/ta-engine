#pragma once

// Deterministic in-sim runner for a map's `.crt` scenario triggers: the
// per-player groups of conditions + actions (26 + 26 opcodes) reverse-
// engineered from Cartographer.exe and stored/round-tripped by tak::crt.
// Like MissionScript it is built on every peer from identical data, ticked
// inside World::tick, and folded into stateHash, so its flag/timer state,
// spawns, and win/lose stay in lockstep. Display actions push cosmetic
// messages (NOT hashed) that the client drains for the viewing player.
//
// Rules evaluate at initialization and once per game second, repeating while
// all conditions hold unless disabled or their runtime player is defeated.

#include "crt/crt.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace tak::sim {

class World;
class TypeRegistry;
struct UnitType;
struct Unit;

class ScenarioScript {
public:
    // scen: the parsed .crt (typed rules + regions). reg: type lookup.
    // viewPlayer: the local human's slot (for message filtering; -1 = show all).
    // maxPlayer: number of world slots (out-of-range groups are never reassigned).
    // mapWCells/mapHCells: map size in 16px cells (for "Anywhere"/whole-map).
    ScenarioScript(const tak::crt::Scenario& scen, const TypeRegistry& reg,
                   int viewPlayer, int maxPlayer, int mapWCells, int mapHCells,
                   uint32_t participants = 0xff);

    bool active() const { return !players_.empty(); }
    // An exact copy of the rules' state, for a copy of its world (World::clone). The
    // trace sink, like every observer, stays behind.
    std::unique_ptr<ScenarioScript> clone() const {
        auto copy=std::make_unique<ScenarioScript>(*this);
        copy->trace_=nullptr;
        return copy;
    }

    void start(World& w);              // first-tick hook (arms "start of game")
    void step(World& w, float dt);     // evaluate every player's rule groups
    void unitDied(World& w, int id);   // death -> per-player/type kill & loss tallies
    // The same tallies (4cd600) for a death World no longer has a unit for:
    // the old object of a change of owner (514da0), which retires under its
    // old owner with the last hitter it had (+0xfc).
    void objectDied(const UnitType* type, int owner, int lastHitPlayer);
    // Tests: a player's tally of one type's units lost / killed.
    int32_t lostCount(int player, const std::string& type) const { return tally(player, type, false); }
    int32_t killedCount(int player, const std::string& type) const { return tally(player, type, true); }
    void foldHash(uint64_t& h) const;  // fold flag/timer/disabled/outcome state

    // Cosmetic display messages queued since the last drain (game time, target
    // player, text). Target -1 = All Players. NOT part of the hash.
    // `t` is the TICK the message was queued, matching Scenario::clock_. It was
    // float, and clang rejected the narrowing that gcc accepted in silence.
    struct Msg { int32_t t = 0; int player = -1; std::string text; };
    std::vector<Msg> drainMessages();

    bool showClock() const { return showClock_; }   // "Display gameclock" latched
    bool participates(int player) const {
        return player >= 0 && player < maxPlayer_ && player < 8 && (participants_ & (1u << player));
    }
    int outcome(int player) const {
        return participates(player) ? outcomes_[size_t(player)] : 0;
    }
    // Optional local diagnostics, never hashed or enabled by network map data.
    // action == -1 announces a firing group; other records precede its actions.
    using TraceSink=std::function<void(int32_t tick,int player,int group,int action,const crt::Rule*)>;
    void setTraceSink(TraceSink sink) {trace_=std::move(sink);}

private:
    TraceSink trace_;
    void trace(int player,int group,int action,const crt::Rule* rule) noexcept;
    struct Timer { int32_t value = 0; bool countUp = false; };
    struct PState {
        std::map<std::string, int32_t> flags;    // sorted -> deterministic hashing
        std::map<int32_t, Timer> timers;
        std::map<std::string, int32_t> killed;   // lowercased type -> count I killed
        std::map<std::string, int32_t> lost;     // lowercased type -> count I lost
        std::string firstKilled, firstLost; // native type-zero query returns first inserted counter
    };

    // ---- evaluation ----
    void evaluate(World& w, bool initial);
    bool evalCond(World& w, int player, const tak::crt::Rule& c, bool initial);
    void runAction(World& w, int player, int group, const tak::crt::Rule& a);
    int countControl(World& w, int player, const std::string& typeName, const std::string& loc) const;

    // ---- parameter helpers ----
    const UnitType* findType(const std::string& name) const;
    const tak::crt::Region* region(const std::string& name) const;
    bool typeMatches(const Unit& unit, const std::string& name) const;
    bool regionBounds(const std::string& name,int& x1,int& z1,int& x2,int& z2) const;
    int32_t deathCount(const PState& state,bool killed,const std::string& type) const;
    int32_t tally(int player, const std::string& type, bool killed) const {
        if (player < 0 || player >= int(state_.size())) return 0;
        const auto& values = killed ? state_[size_t(player)].killed : state_[size_t(player)].lost;
        const auto it = values.find(type);
        return it == values.end() ? 0 : it->second;
    }
    bool inRegion(World& w, const Unit& unit, const std::string& loc) const;
    bool regionCenter(const std::string& loc, float& x, float& z) const;
    int parsePlayer(const std::string& s) const;   // "Player N"->N-1, "All..."->-1
    void applyOutcome(World& w, int player, int result, int recipients);

    const TypeRegistry& reg_;
    int view_;
    int maxPlayer_;
    uint32_t participants_ = 0;
    std::vector<int8_t> outcomes_; // first terminal result per participant; hashed
    int mapW_ = 0, mapH_ = 0;   // cells
    std::vector<std::vector<tak::crt::RuleGroup>> players_;   // rules, clamped to maxPlayer_
    std::vector<std::vector<uint8_t>> disabled_;              // per player/group
    std::vector<PState> state_;
    std::vector<tak::crt::Region> regions_;
    std::vector<Msg> pending_;
    int32_t clock_ = 0;   // TICKS
    uint32_t ticks_ = 0;
    bool showClock_ = false;
    bool started_ = false;
};

}  // namespace tak::sim
