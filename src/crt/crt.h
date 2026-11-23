#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tak::crt {

// .crt scenario files: the initial unit placements for a map, the per-player
// trigger rules, custom unit-type stat overrides, and named regions.
//
// Layout (static analysis of Cartographer.exe's writer/loader, cross-validated
// byte-exact against every shipped .crt):
//
//   f32 version = 1.0
//   i32 numCustomTypes;  CustomType[272] x numCustomTypes
//   i32 numUnits;        UnitRecord[568]  x numUnits
//   i32 numPlayers (=9);
//       per player: i32 numGroups
//           per group: i32 numConditions; Rule[324] x n
//                      i32 numActions;    Rule[324] x n
//   i32 numRegions;      RegionDef[272] x numRegions
//
// There is no inter-record padding. Bytes outside the fields below are in-memory
// residue in retail files (the record is a raw dump of the editor's live object);
// a clean writer zero-fills them and the game loads the result identically.

// ---- Full typed model (parse/write) --------------------------------------

struct Unit {
    std::string objectName;      // FBI type name (@0x000, char[256])
    std::string uniqueName;      // custom display name (@0x100; runtime uses first 31 bytes)
    int32_t x = 0, z = 0;        // 16px footprint-origin cells (@0x200 / @0x208)
    int32_t y = 200;             // retained vertical field; native loader ignores it (@0x204)
    int32_t player = 0;          // 0..8 (@0x20c)
    int32_t health = 100;        // %   0..100  (@0x210)
    int32_t armor = 100;         // %   0..1000 (@0x214)
    int32_t weapon = 100;        // %   0..1000 (@0x218)
    int32_t angle = 0;           // deg 0..359  (@0x21c)
    int32_t veteran = 0;         //     0..9    (@0x220)
};

// A unit type whose default health/armor/weapon differ from (100,100,100).
struct CustomType {
    std::string name;                          // @0x000, char[256]
    int32_t stat[4] = {100, 100, 100, 0};      // @0x100: health,armor,weapon,(veteran?)
};

// One condition or action record (324 bytes on disk). Conditions and actions
// share this format but use independent opcode spaces.
struct Rule {
    int32_t opcode = 0;          // @0x000
    std::string slot[5];         // @0x004: five 64-byte operand strings
};

// A trigger "group": a set of conditions and the actions to run when they hold.
struct RuleGroup {
    std::vector<Rule> conditions;
    std::vector<Rule> actions;
    std::string editorName; // Cartographer sidecar only; never part of retail CRT bytes.
};

// A rule group that does something. An empty group (no condition, no action) never
// does: it is not scenario content and does not need a lobby seat.
inline bool hasRules(const RuleGroup& g) { return !g.conditions.empty() || !g.actions.empty(); }
// Victory/defeat ACTION opcodes (5, 6, 21-24; sim/scenario.cpp). Condition opcodes are a
// separate space: condition 21/22 ("opponents left") is not an outcome.
inline bool isOutcomeAction(int32_t opcode) { return opcode == 5 || opcode == 6 || (opcode >= 21 && opcode <= 24); }

struct Region {
    std::string name;            // @0x000, char[64]
    int32_t x1 = 0, z1 = 0, x2 = 0, z2 = 0;   // 16px cells (@0x100)
};

// A parsed .crt in full. `players` holds All Players at index 0, followed by
// Player 1 through 8; each entry contains that rule bank's groups.
struct Scenario {
    float version = 1.0f;
    std::vector<CustomType> customTypes;
    std::vector<Unit> units;
    std::vector<std::vector<RuleGroup>> players;
    std::vector<Region> regions;
};

// Parse a whole .crt. Returns an empty Scenario (version 0) if `d` is not a
// version-1.0 .crt or is structurally malformed.
Scenario parse(const std::vector<uint8_t>& d);

// Serialize a Scenario to bytes. Structurally byte-exact to retail (same order,
// counts, and record sizes); the in-memory residue retail leaves in unused
// record bytes is zero-filled. parse(write(s)) == s for the fields above.
std::vector<uint8_t> write(const Scenario& s);

} // namespace tak::crt
