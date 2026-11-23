#pragma once

// The AI-profile editor's model: SDL-free, unit-tested by
// tools/cartographer_aiprofile_test.cpp (the design's phase 2).
//
// A map's AI files are raw bytes (Session::aiFiles); the bytes are the truth. The
// editor parses them into drafts, and writes a draft back in one canonical form.
// It never computes what a file does on its own: every effective value comes from
// the engine's parsers -- ai::ClassicProfile::execute for the DEFAULT AI (retail's
// ai/<aiprofile>.txt else ai/default.txt) and ai::applyProfileText for the one file
// all six of our levels read, ai/takengine.txt -- and every write is re-run through
// them (verifyAiDraft).
// What the model adds is line attribution: which line names a unit, which line a
// group word, which lines are shadowed or do nothing. The test checks that
// attribution against both parsers.
//
// The two grammars (ai/classicprofile.h, ai/builtinprofile.h):
//   DEFAULT  case-insensitive; '#' ends a line; "//" skips it; values read C atof /
//            atoi style (missing = 0); an exact unit name locks the unit, so the
//            FIRST line naming it wins; category words (FBI category=, ALL) touch
//            every unlocked unit they cover and compound.
//   Levels   keyword case-sensitive; a line starting with '/' is skipped; values
//            read as whole numbers (">> int": 0.5 reads 0); units by name only;
//            the LAST line for a unit and keyword wins.

#include "ai/ai.h"
#include "ai/classicprofile.h"
#include "cartographer/document.h"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cart {

enum class AiKind { Default, Level };

// ai/takengine.txt (ai::kEngineProfileFile): the one file all six of our levels read,
// each over its own built-in table.
bool aiEngineFile(const std::string& key);
// The levels grammar for ai/takengine.txt; every other ai/ file is in the DEFAULT grammar.
AiKind aiKindOf(const std::string& key);
// "Defensive" .. "Absurdly Defensive"; "DEFAULT AI" for Classic.
const char* aiLevelName(tak::ai::Difficulty);

// The first line of every file the editor writes; a file that starts with it and
// re-writes to the same bytes opens editable, any other file opens as written.
inline constexpr std::string_view kAiProfileMagic = "// TAK Cartographer AI profile 1";
inline constexpr size_t kAiFileMaxBytes = 256u << 10;

struct AiUnitRule {
    std::optional<int> chance;   // DEFAULT: the effective preference, 0..100 (%)
    std::optional<int> weight;   // Level: the whole-number weight
    std::optional<int> limit;    // both; -1 = no limit (written, so it locks for DEFAULT)
    bool empty() const { return !chance && !weight && !limit; }
};
struct AiGroupRule {   // DEFAULT only: a category word line (applies after unit rows)
    bool weight = true;
    std::string word;    // lowercased category word or "all"
    std::string value;   // the value token as written ("" = missing, reads as 0)
};
struct AiDraft {
    AiKind kind = AiKind::Default;
    bool asWritten = false;       // a hand-written file: saved unchanged until makeEditable
    std::string original;         // the bytes as opened (asWritten)
    std::string header;           // one comment line after the magic ("" = none)
    std::map<std::string, AiUnitRule> units;   // lowercased unit key
    std::vector<AiGroupRule> groups;
    std::vector<std::string> kept;   // lines with no effect, verbatim
    // Nothing to write: the file is removed rather than written empty (a map
    // package rejects an empty AI file).
    bool empty() const;
};

// The DEFAULT weight that gives preference p from a fresh 100 (p clamped to 0..100):
// "1" for 100, "0" for 0, else "0." + the three digits of p*10+5 -- the half step
// survives the float store, where p/100 truncates 50 of the 101 values to p-1.
std::string classicChanceText(int p);
// Can an exact line set this unit in the DEFAULT grammar? Mirrors execute's lookup
// (a catalogue name whose index is not 0).
bool classicCanSingleOut(const tak::ai::ClassicCatalogue&, const std::string& name);

// Parse a file into a draft. A canonical file (kAiProfileMagic, one row per unit,
// re-writing to the same bytes and verifying) opens editable; any other opens as
// written with `original` set and no rows.
AiDraft parseAiDraft(std::string_view text, AiKind, const tak::ai::ClassicCatalogue&);
// The canonical form: magic, header, the kind's note, unit rows sorted by key
// (weight then limit), then (DEFAULT) "// group rules" and the group lines in order,
// then "// kept from the original file (no effect)" and the kept lines. LF endings, a
// final newline, no date. An as-written draft returns `original`; an empty draft "".
std::string writeAiDraft(const AiDraft&);
// Re-runs the engine's parser on `bytes` and compares every unit with what the
// draft means: each row's value exactly, every other unit what the group lines
// alone give it (DEFAULT) or nothing (Level). An as-written draft must equal its
// original. `mismatch` gets the first differing unit.
bool verifyAiDraft(const AiDraft&, std::string_view bytes, const tak::ai::ClassicCatalogue&,
                   std::string* mismatch = nullptr);
// writeAiDraft + verifyAiDraft + the size limit. `bytes` "" means delete the file.
// On failure `error` says "AI profile could not be written exactly" and the unit.
bool saveAiDraft(const AiDraft&, const tak::ai::ClassicCatalogue&, std::string& bytes, std::string& error);
// Converts an as-written draft into rows (design 5.4): DEFAULT keeps category lines
// in order as group rules and writes a row for each unit an effective exact line
// set; levels keep the last line per unit and keyword. Lines with no effect are
// kept verbatim, shadowed lines dropped. Accepted only if the converted file has
// exactly the original's effect on every unit; otherwise `draft` is unchanged.
bool makeEditable(AiDraft&, const tak::ai::ClassicCatalogue&, std::string* why = nullptr);
// `bytes` with only its header line (the comment after the magic) replaced by `header`,
// when they are in the canonical form and the result verifies; every other file --
// written by hand, empty, or one whose new bytes would not verify -- comes back
// unchanged. Save as game mod gives a DEFAULT profile written as ai/default.txt the
// header aiModDefaultHeader makes.
std::string aiFileWithHeader(std::string_view bytes, AiKind, const std::string& header,
                             const tak::ai::ClassicCatalogue&);
// "DEFAULT AI profile for game mod "<pack>" (ai/default.txt), saved from map "<map>"
// (<from>). Edit in Cartographer: SCENARIO > AI Profiles."
std::string aiModDefaultHeader(const std::string& pack, const std::string& mapName, const std::string& from);

// One line of a file and what it does in its grammar.
struct AiLine {
    enum class What {
        Blank,       // whitespace only
        Comment,     // DEFAULT: "//" or '#' first; Level: '/' first or under 2 bytes
        Other,       // a command other than weight/limit (Level: also 'Weight')
        Unknown,     // DEFAULT: the word names no unit and no category (or none given)
        Skipped,     // Level: weight/limit without a readable whole-number value
        Exact,       // sets one unit
        Group,       // DEFAULT: a category word line
        Shadowed,    // DEFAULT: an exact line after an earlier exact line for that unit
        Overridden,  // Level: a later line sets the same unit and keyword
    };
    int number = 0;                // 1-based
    std::string_view text;         // the line, without its '\n'
    What what = What::Blank;
    bool weight = false;           // the keyword (Exact, Group, Shadowed, Skipped, Overridden)
    std::string command;           // the first token as written
    std::string word;              // lowercased unit or category word
    std::string value;             // the value token ("" when none)
    bool hasValue = false;
    int levelValue = 0;            // Level: what ">> int" read
    uint16_t unit = 0;             // DEFAULT Exact/Shadowed: catalogue index
    int by = 0;                    // Shadowed: the line that set the unit; Overridden: the line that wins
    std::vector<int> combines;     // DEFAULT exact weight: earlier group lines that scaled the unit
};
// Every line of `text` (split at '\n'; a final newline adds no line).
std::vector<AiLine> attributeAiLines(std::string_view text, AiKind, const tak::ai::ClassicCatalogue&);

// The units an AI may name: the DEFAULT catalogue (FBI unitnames, variants and
// category words) and, for sides, builders and lodestones, the registry (null skips
// those checks).
struct AiRoster {
    const tak::ai::ClassicCatalogue* catalogue = nullptr;
    const tak::sim::TypeRegistry* registry = nullptr;
};
// The units some builder of their side can make, from each side's Monarch
// (TypeRegistry::buildable, which counts the Zhon's mobile builders); Monarchs
// themselves are not included. The window hides the rest by default.
std::set<std::string> aiReachableUnits(const tak::sim::TypeRegistry&);
struct AiIssue {
    enum class Severity { Info, Warning, Error };
    Severity severity = Severity::Warning;
    std::string file;      // "ai/takengine.txt"; "" for the map's aiprofile name
    int line = 0;          // 0 = the whole file
    std::string unit;
    std::string message;
};
// The design's 6.2 checks for one file (none block saving).
std::vector<AiIssue> checkAiFile(const std::string& key, std::string_view bytes, const AiRoster&);
// The map-level checks (the aiprofile name, files that do not travel) and every
// file's own checks. `base` is the install the DEFAULT AI may fall back to.
std::vector<AiIssue> checkAiMap(const AiFiles&, const std::string& aiProfile, const tak::hpi::Vfs& base,
                                const AiRoster&);

// The aiprofile name rules (design 7.1). The file a name reads ("ai/<name>.txt"
// keyed, the extension after the last '.' dropped as the engine does), or "" for
// an empty name; ai/default.txt is the standard profile.
std::string aiProfileFileKey(const std::string& name);
bool standardAiProfile(const std::string& name);
// The map name lowercased, keeping [a-z0-9 _-], at most 32 characters, never a
// reserved name ("map" when nothing is left).
std::string proposeAiProfileName(std::string_view mapName);
// Why a newly typed name is refused ("" = accepted): '.', '/' or '\', default,
// takengine, over 32 characters, or not a valid file name. Existing names are kept.
std::string aiProfileNameError(const std::string& name);
// The map's files that travel in its package (and reach Test Map): ai/default.txt,
// ai/takengine.txt and ai/<aiprofile>.txt, when not empty.
bool aiFileTravels(const std::string& key, const std::string& aiProfile);
AiFiles travellingAiFiles(const AiFiles&, const std::string& aiProfile);
// Sets one file; empty bytes remove it.
void setAiFile(AiFiles&, const std::string& key, std::string_view bytes);
// Renaming the map's own profile moves ai/<old>.txt to ai/<new>.txt (an existing
// file of the new name is replaced). False when either name has no file of its own.
bool renameAiProfile(AiFiles&, const std::string& oldName, const std::string& newName);

// The overwrite compare for one file (design 7.2, Save as game mod): the line
// counts, the first changed line, and -- from the engine's own parsers -- how many
// units play differently (ai/takengine.txt measured over each level's built-in table).
std::string compareAiFiles(const std::string& key, std::string_view before, std::string_view after,
                           const tak::ai::ClassicCatalogue&);

// Which file a profile plays (design 6.1), from the engine's own lookup
// (ai::classicProfileScript for DEFAULT, ai::levelProfile for the levels) over this
// map's travelling files, an optional game mod's files and the base install.
struct AiFileChoice {
    enum class From { None, Base, GameMod, Map };
    From from = From::None;
    std::string path;      // the file read; "" = none (DEFAULT: every unit 100 %; levels: built-in)
    std::string message;   // the footer line
};
AiFileChoice whichFile(tak::ai::Difficulty, const tak::hpi::Vfs& base, const AiFiles& mapFiles,
                       const std::string& aiProfile, const tak::hpi::Vfs::Files* gameModFiles = nullptr);

// ---- DEFAULT group rules (design 4, the Advanced disclosure) --------------------------
// The words a group rule may use: this catalogue's category words ("all" first, the
// rest sorted), less any word that is also a unit name -- the engine reads such a
// word as that one unit, never as the group.
std::vector<std::string> aiGroupWords(const tak::ai::ClassicCatalogue&);
// One unit a group rule covers. `skipped`: the unit is set individually for the rule's
// keyword (its own chance row for a weight rule, its own limit row for a limit rule),
// so the engine's lock leaves it alone. `times` > 1 when the unit lists the word more
// than once (FBI category= repeats), which applies a weight that many times.
struct AiGroupUnit {
    uint16_t index = 0;   // catalogue index
    bool skipped = false;
    int times = 1;
};
// The units `draft.groups[rule]` covers, in catalogue order (rows come before every
// group line in the canonical form, so a row is the lock).
std::vector<AiGroupUnit> aiGroupRuleUnits(const AiDraft&, size_t rule, const tak::ai::ClassicCatalogue&);
// The same for any file: the units line `lines[line]` (a Group line) covers, skipped
// when an earlier exact line of the same keyword set the unit.
std::vector<AiGroupUnit> aiGroupLineUnits(const std::vector<AiLine>&, size_t line, const tak::ai::ClassicCatalogue&);
// A weight rule's value as what it makes of a unit at 100 % (0..100); a limit rule's
// value as the engine reads it (-1 = no limit; a missing value reads 0).
int aiGroupRuleChance(const AiGroupRule&);
int aiGroupRuleLimit(const AiGroupRule&);

// ---- our levels' limits in play (design 4, the per-level preview) ----------------------
// How a level treats a profile limit while it plays (ai::paramsFor and the AI's build
// pick, ai.cpp): Easy halves it, Hard and Absurd raise it by half (minimum 1), Normal
// keeps it; the defensive levels ignore every limit except 0. A mobile builder never
// passes its own number; Hard and Absurd lift economy buildings with income and may
// raise army, factory and defence limits to their plan in long games. The scale and
// the defensive rule mirror ai::paramsFor (tools/cartographer_aiprofile_test checks
// them against it).
int aiLevelLimitScale(tak::ai::Difficulty);   // percent
bool aiLevelIgnoresLimits(tak::ai::Difficulty);
struct AiLevelLimit {
    int limit = -1;         // in play: -1 = no cap from the profile, 0 = never built
    bool changed = false;   // differs from the profile's number
    bool mayRise = false;   // Hard / Absurd may raise it further in long games
    std::string text;       // "4 in play", "no cap in play", ...
};
// `limit`: the profile's limit for the unit (the file over the built-in table), or none.
// `registry` (may be null) tells mobile production sides apart.
AiLevelLimit aiEffectiveLimit(tak::ai::Difficulty, std::optional<int> limit, const tak::sim::UnitType*,
                              const tak::sim::TypeRegistry* registry = nullptr);

} // namespace cart
