#pragma once

// The Classic AI's build profile: retail's per-type preference byte and unit
// limit, baked from the map's AI profile script (ai/<aiprofile>.txt). Behaviour
// as reverse-engineered from the retail game (docs/classic-ai-plan.md §1.2):
//
//   * Every type starts at preference 100 and limit -1 (unlimited).
//   * `weight <word> <w>`: pref = clamp(trunc(pref * w), 0, 100) for each type the
//     word names whose preference is not locked. w is read as a float, C atof
//     style, a missing value as 0.
//   * `limit <word> <n>`: limit = n (C atoi style, missing = 0) for each named
//     type whose limit is not locked.
//   * The word is an exact unit name (case-insensitive) or else a category word:
//     one of the FBI `category=` words, or ALL. An exact name LOCKS that type, so
//     the first line to name a unit wins; a category line does not lock, so
//     category lines compound and later exact lines still apply on top.
//   * Unknown words do nothing, and so do commands other than weight/limit (retail
//     runs the script with every console command enabled, but no shipped profile
//     holds anything else, and `plan` is a no-op). A type the file never names
//     keeps 100 / -1. That includes misspellings, with one exception that departs
//     from retail: the shipped profiles' `araseige` is read as the Rolling Tower
//     (arasiege), which retail leaves unlimited (unless a type is named araseige).
//   * Lines split at '\n'; tokens at C isspace; '#' ends a line; a line whose
//     first token starts with "//" is skipped; extra tokens are ignored.
//
// Server-side only (an AI input, never part of the gameplay-data hash).

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tak::hpi { class Vfs; }
namespace tak::sim { class TypeRegistry; struct UnitType; }

namespace tak::ai {

// Retail's unit-type table in its own order (sorted by name, case-insensitively),
// with index 0 the empty sentinel retail keeps there. Every per-type array of the
// Classic brain is indexed by this catalogue.
struct ClassicCatalogue {
    std::vector<const tak::sim::UnitType*> types{nullptr};   // [0] = sentinel
    std::vector<std::string> names{""};                       // lowercased
    std::unordered_map<std::string, uint16_t> byName;
    // Lowercased category word -> catalogue indices ("all" holds every type).
    std::unordered_map<std::string, std::vector<uint16_t>> categories;

    size_t size() const { return names.size(); }
    // One entry per registry type, its categories from the FBI `category=` words.
    static ClassicCatalogue from(const tak::sim::TypeRegistry& registry);
    // A catalogue without real types (tests, the emulation oracle): names in any
    // order (sorted here), each with its category words.
    static ClassicCatalogue synthetic(
        std::vector<std::pair<std::string, std::vector<std::string>>> entries);
    void add(const tak::sim::UnitType* type, std::string name, const std::string& categoryWords);
};

struct ClassicProfile {
    std::vector<uint8_t> pref;
    std::vector<int32_t> limit;
    std::vector<uint8_t> prefLocked, limitLocked;
    std::string path;   // the file the script came from (for the debug dump)

    ClassicProfile() = default;
    explicit ClassicProfile(size_t types) { reset(types); }
    // Retail's reset (0x410670): 100, -1, nothing locked.
    void reset(size_t types);
    // Runs one profile script over the catalogue (retail 0x538f00 with the
    // weight/limit handlers 0x409760/0x4097f0).
    void execute(std::string_view script, const ClassicCatalogue& catalogue);
    // Retail's limit check for an AI player (0x410de0): -1, or under the limit.
    bool allows(size_t type, int count) const {
        return type >= limit.size() || limit[type] == -1 || count < limit[type];
    }
};

// The unit a misspelt name in the shipped profiles means (lowercased in and out:
// "araseige" -> "arasiege"), else `name` itself. Both profile readers apply it
// to a name no type has, so either spelling works.
std::string_view classicUnitSpelling(std::string_view name);
// retail's C-library number readers as the profile executor uses them.
float classicAtof(std::string_view token);
int32_t classicAtoi(std::string_view token);
// retail's weight store for one type: clamp(trunc(pref * w), 0, 100), with the
// x87 conversion's 64-bit truncation and its low 32 bits taken as the result.
uint8_t classicScaledPref(uint8_t pref, float w);

// The `aiprofile=` key of a map's .ota ([GlobalHeader] -> [Map Data]), or "" when
// the map has none (or no .ota).
std::string classicMapAiProfile(const tak::hpi::Vfs& vfs, const std::string& mapPath);
struct ClassicProfileScript { std::string path, text; };
// Retail's file choice (map loader 0x4c8608 and loader 0x4f6860): ai/<key>.txt
// with any extension of the key dropped; an empty key means ai/default.txt; an
// unreadable named file falls back to ai/default.txt; if that is missing too the
// script is empty (every type keeps 100 / -1). Retail's language-variant name
// (ai/<key>_<variant>.txt) is not tried: no such file ships. The VFS applies
// retail's archive precedence, so IPData.hpi's default.txt wins.
ClassicProfileScript classicProfileScript(const tak::hpi::Vfs& vfs, const std::string& key);
// The same choice over a room's layers, for ai/ files only: each candidate file is
// taken from the room's game mods (`gameModFiles`), else its map package
// (`mapFiles`), else `vfs` (the base data) -- a game mod wins over the map, which
// wins over the base, whatever the view's own precedence for other map content.
// Both file sets are keyed by lowercased path, or null for none. `layer`, when
// given, receives where the chosen file came from.
enum class ClassicProfileLayer : uint8_t { None, Base, GameMods, Map };
using ClassicFiles = std::map<std::string, std::vector<uint8_t>>;   // == hpi::Vfs::Files
ClassicProfileScript classicProfileScript(const tak::hpi::Vfs& vfs, const std::string& key,
                                          const ClassicFiles* gameModFiles, const ClassicFiles* mapFiles,
                                          ClassicProfileLayer* layer = nullptr);

}  // namespace tak::ai
