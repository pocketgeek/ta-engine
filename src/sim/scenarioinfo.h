#pragma once

// MAP SCENARIO: what a map's own scenario would do if a room turned it
// on -- read from the map's .ota, .crt and Use Only .tdf alone, without building a world.
//
// One inspector serves the CHANGE MAP panel (the map list's marker and the selected map's
// button), the room's rule lines and seat tags, the server's START checks, the lobby
// capacity and the tests. It is deliberately separate from setupMatch, which mixes the
// same checks with world mutations (custom stats, the neutral owner); scenario_info_test
// keeps the two in agreement: with a registry, status Ok exactly when setupMatch with
// MatchConfig::mapScenario does not refuse the map (placement bounds aside, below).
//
// Not part of the hashed simulation: nothing here touches a World.

#include <cstdint>
#include <string>
#include <vector>

namespace tak::hpi { class Vfs; }

namespace tak::sim {

class TypeRegistry;

enum class ScenarioStatus : uint8_t {
    Ok,             // offerable
    NoScenario,     // no/unparseable .ota, or hasscenario=0 without CRT content
    Leftover,       // hasscenario=0 but the .crt has placements or rules (retail never ran it)
    CrtMissing,     // hasscenario=1, no .crt
    CrtDamaged,     // .crt is not version 1
    Empty,          // no placed unit and no rule group with a condition or action
    BadOwner,       // a placement owner outside 0..8 (or rules past the neutral bank)
    UseOnlyBroken,  // useonlyunits is not "<map>.tdf" beside the map, or the file is missing/unreadable
    MissingType,    // (registry check) a placed or Use Only type this install lacks
};

struct ScenarioInfo {
    ScenarioStatus status = ScenarioStatus::NoScenario;
    bool hasScenario = false;   // [GlobalHeader] hasscenario != 0
    bool marker = false;        // [TAKPlaytest] authoredscenario != 0 (a Test Map copy)
    bool crtValid = false;      // .crt present and version 1
    int placed = 0;             // placed units, every owner
    uint8_t placedSeats = 0;    // bit p: owner p (0..7) has placed units
    uint8_t ruleSeats = 0;      // bit p: rule bank p+1 (Player p+1) has a group with rules
    bool sharedRules = false;   // bank 0 (All Players) has a group with rules
    bool outcomes = false;      // some group has a Victory/Defeat action
    // -1 = unrestricted, else how many types the Use Only list allows (0 = nothing
    // buildable). With a registry a MAP SCENARIO list counts only the types this install
    // has: setupMatch leaves out a name no unit answers to (a Test Map copy refuses it).
    int useOnlyTypes = -1;
    // The map names its own <map>.tdf as its Use Only list, but this view has no such
    // file. Set whatever the status (a Test Map copy's list applies even without a
    // declared scenario), so the server can refuse a package that leaves the list out:
    // setupMatch would then read each machine's own copy, which no package digest covers.
    bool useOnlyMissing = false;
    // (registry check) bit p: owner p starts with something to command -- a placed unit
    // of a kingdom (not wildlife, villagers or monsters: sides LIF/NPC/MON) that can
    // fight, build or move -- or its rule bank or All Players' creates units. Placements
    // replace every Monarch, so a seat outside this mask starts with nothing to play.
    // Every seat without a registry.
    uint8_t armySeats = 0xFF;
    std::string detail;         // MissingType: the type; UseOnlyBroken: the file name
    bool runnable() const { return hasScenario && crtValid; }
    bool content() const { return placed > 0 || ruleSeats != 0 || sharedRules; }
    uint8_t requiredSeats() const { return placedSeats | ruleSeats; }   // == setupMatch's seat rule
    // Player 1 -- the room's creator, and in single player the human -- has an army
    // once the scenario's placements replace the Monarchs (rules-only maps keep them).
    bool seatOneArmy() const { return placed == 0 || (armySeats & 1) != 0; }
    // The CHANGE MAP panel offers it. Player 1's army is not a status: setupMatch starts a
    // map that gives player 1 nothing to command, it is just not a game anyone can play.
    // (40 of 186 downloaded scenario maps checked place only wildlife, villagers, scenery
    // or lodestones for player 1; their authors used the scenario editor to decorate.)
    bool offerable() const { return status == ScenarioStatus::Ok && !marker && seatOneArmy(); }
    // Whether a room plays the scenario. Callers also exclude missions, generated maps,
    // stress tests and benchmarks.
    bool active(bool option) const { return runnable() && (marker || (option && content())); }
};

// Reads <map>.ota, <map>.crt and the named Use Only .tdf through `vfs` (a package view
// or the local install). Never throws. With `reg`, also checks every placed type (a
// custom type the registry lacks is skipped, as setupMatch and retail do), and a Test Map copy's Use Only entries (setupMatch's refusals), and fills
// armySeats. Placement bounds are the one refusal left out (setupMatch keeps that
// check and its message, which the room shows as the server's start refusal; checking it
// here would mean decompressing every scenario map's TNT while the map list is built).
// The counts and masks are filled whenever the files parse, whatever the status, so the
// room can describe a Test Map copy too.
ScenarioInfo inspectScenario(const hpi::Vfs& vfs, const std::string& mapPath, const TypeRegistry* reg = nullptr);

// 1-based seat list: "1-4", "1 3", "2 5-8" (runs of three or more collapse). "" for none.
std::string seatList(uint8_t seats);

struct ScenarioLine { std::string text; bool warning = false; };
// Why the CHANGE MAP panel greys the button (uppercase, <= 26 characters a line): the
// status, or for an Ok map, that player 1 gets no army. Empty for an offerable map. A
// Test Map copy is the caller's case (TEST COPY: ALWAYS ON).
std::vector<std::string> scenarioReason(const ScenarioInfo&);
// What turning the scenario on changes: compact for the CHANGE MAP panel (<= 26 characters
// a line), full for the room's rule lines (<= 44).
std::vector<ScenarioLine> scenarioSummary(const ScenarioInfo&, bool compact);
// The summary's Use Only line on its own ("BUILDS 6 UNIT TYPES" / "ONLY 6 UNIT TYPES
// CAN BE BUILT"); "" for an unrestricted map in the compact form.
std::string scenarioUseOnlyLine(const ScenarioInfo&, bool compact);

}  // namespace tak::sim
