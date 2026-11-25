#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ta::crt {

// .crt scenario files: the initial unit placements for a map, the per-player
// trigger rules, custom unit-type stat overrides, and named regions.
//
// Layout (static analysis of Cartographer.exe's writer/loader, cross-validated
// byte-exact against every shipped .crt):
//
//   f32 version = 1.0 (the Kingdoms-era rank format) or 2.0 (kills; see below)
//   i32 numCustomTypes;  CustomType[272] x numCustomTypes
//   i32 numUnits;        UnitRecord[568]  x numUnits
//   i32 numPlayers (=9);
//       per player: i32 numGroups
//           per group: i32 numConditions; Rule[324] x n
//                      i32 numActions;    Rule[324] x n
//   i32 numRegions;      RegionDef[272] x numRegions
//
// Veterancy. A version 1.0 file carries the Kingdoms-era veteran RANK at unit
// +0x220 (0..9) and custom type stat[3] (0..10); version 2.0 carries TA's kill
// count there instead (TotalA.exe unit +0xb8, the count TA's damage scaling
// 0x489bf3 and accuracy 0x48a324 / 0x49e468 read). parse() converts a 1.0
// file's rank N to N x 5 kills (killsForRank: the five kills per step of TA's
// damage scaling, so rank 2 = 10 kills = 8% less damage taken); write() writes
// `version`'s form back, so a 1.0 file round-trips byte for byte. New
// scenarios are 2.0.
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
    int32_t kills = 0;           // starting kill count (@0x220; a 1.0 file's rank x 5)
};

// A unit type whose default health/armor/weapon differ from (100,100,100).
struct CustomType {
    std::string name;                          // @0x000, char[256]
    int32_t stat[4] = {100, 100, 100, 0};      // @0x100: health,armor,weapon,kills (1.0: rank x 5)
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

inline constexpr float kVersionRank = 1.0f;    // +0x220 / stat[3] hold the old rank
inline constexpr float kVersionKills = 2.0f;   // +0x220 / stat[3] hold the kill count

// An old rank's kill count: TA steps its veterancy every five kills (0x489bf3
// k = min(kills / 5, 5)), so rank N is N x 5 kills. Saturates instead of
// overflowing on a damaged field.
inline int32_t killsForRank(int32_t rank) {
    return rank > INT32_MAX / 5 ? INT32_MAX / 5 * 5 : rank < INT32_MIN / 5 ? INT32_MIN / 5 * 5 : rank * 5;
}
inline int32_t rankForKills(int32_t kills) { return kills / 5; }

// A parsed .crt in full. `players` holds All Players at index 0, followed by
// Player 1 through 8; each entry contains that rule bank's groups.
struct Scenario {
    float version = kVersionKills;
    std::vector<CustomType> customTypes;
    std::vector<Unit> units;
    std::vector<std::vector<RuleGroup>> players;
    std::vector<Region> regions;
};

// Parse a whole .crt. Returns an empty Scenario (version 0) if `d` is not a
// version 1.0 or 2.0 .crt or is structurally malformed.
Scenario parse(const std::vector<uint8_t>& d);

// Serialize a Scenario to bytes. Structurally byte-exact to retail (same order,
// counts, and record sizes); the in-memory residue retail leaves in unused
// record bytes is zero-filled. parse(write(s)) == s for the fields above.
std::vector<uint8_t> write(const Scenario& s);

} // namespace ta::crt
