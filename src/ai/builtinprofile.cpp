#include "ai/builtinprofile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <sstream>
#include <string>

#include "ai/classicprofile.h"

namespace ta::ai {

namespace {

struct Entry { const char* unit; int weight, limit; };

// The built-in table every level starts from: empty. The engine's levels were
// tuned on TA: Kingdoms' default AI profile, one {unit, weight, limit} per unit of
// its five races; Total Annihilation has no such table (its ai/*.txt are plan and
// category scripts, which TotalA.exe's own AI reads: ai/classicprofile.h), and every
// level now runs that AI (ai/taretail.h, Controller), which never reads this table.
// What is left is ai/taengine.txt's lines, which a game mod or map may still supply.
constexpr std::span<const Entry> kTuned{};

// Each level's own changes to kTuned (same {unit, weight, limit} form; a later
// entry wins). All empty for now: every level still plays exactly the profile it
// was tuned on, and a level's table diverges here.
constexpr std::span<const Entry> kNoChanges{};
constexpr std::span<const Entry> kDefensiveChanges = kNoChanges;
constexpr std::span<const Entry> kEasyChanges = kNoChanges;
constexpr std::span<const Entry> kNormalChanges = kNoChanges;
constexpr std::span<const Entry> kHardChanges = kNoChanges;
constexpr std::span<const Entry> kAbsurdChanges = kNoChanges;
constexpr std::span<const Entry> kAbsurdlyDefensiveChanges = kNoChanges;

Profile build(std::span<const Entry> changes) {
    Profile p;
    for (std::span<const Entry> table : {std::span<const Entry>(kTuned), changes})
        for (const Entry& e : table) { p.weight[e.unit] = e.weight; p.limit[e.unit] = e.limit; }
    return p;
}

} // namespace

const Profile& builtinProfile(Difficulty d) {
    static const std::array<Profile, kAiLevels> profiles = [] {
        std::array<Profile, kAiLevels> out;
        out[size_t(Difficulty::Defensive)] = build(kDefensiveChanges);
        out[size_t(Difficulty::Easy)] = build(kEasyChanges);
        out[size_t(Difficulty::Normal)] = build(kNormalChanges);
        out[size_t(Difficulty::Hard)] = build(kHardChanges);
        out[size_t(Difficulty::Absurd)] = build(kAbsurdChanges);
        out[size_t(Difficulty::AbsurdlyDefensive)] = build(kAbsurdlyDefensiveChanges);
        out[size_t(Difficulty::Passive)] = build(kEasyChanges);   // Easy's table, always
        return out;   // Classic stays empty
    }();
    return size_t(d) < profiles.size() ? profiles[size_t(d)] : profiles[size_t(Difficulty::Classic)];
}

const char* levelProfileFile(Difficulty d) {
    return retailLevel(d) ? nullptr : kEngineProfileFile;
}

void applyProfileText(std::string_view text, Profile& prof) {
    std::istringstream f{std::string(text)};
    std::string kw, unit;
    int v;
    for (std::string line; std::getline(f, line);) {
        if (line.size() < 2 || line[0] == '/') continue;
        std::istringstream ss(line);
        if (!(ss >> kw >> unit >> v)) continue;
        std::transform(unit.begin(), unit.end(), unit.begin(), ::tolower);
        unit = classicUnitSpelling(unit);   // the shipped files' araseige
        if (kw == "weight") prof.weight[unit] = v;
        else if (kw == "limit") prof.limit[unit] = v;
    }
}

const std::vector<uint8_t>* engineProfileText(const ta::hpi::Vfs::Files* gameModFiles,
                                              const ta::hpi::Vfs::Files* mapFiles, bool* fromMap) {
    if (fromMap) *fromMap = false;
    // Package keys are already lowercased (MountSet::key), so this is the
    // case-insensitive lookup every VFS read makes. A game mod's file wins.
    for (const auto* files : {gameModFiles, mapFiles}) {
        if (!files) continue;
        const auto it = files->find(kEngineProfileFile);
        if (it == files->end()) continue;
        if (fromMap) *fromMap = files == mapFiles;
        return &it->second;
    }
    return nullptr;
}

Profile levelProfileFrom(Difficulty d, const std::vector<uint8_t>* text) {
    Profile prof = builtinProfile(d);
    if (text && levelProfileFile(d))
        applyProfileText(std::string_view(reinterpret_cast<const char*>(text->data()), text->size()), prof);
    return prof;
}

Profile levelProfile(Difficulty d, const ta::hpi::Vfs::Files* gameModFiles,
                     const ta::hpi::Vfs::Files* mapFiles, std::string* source, bool* fromMap) {
    if (source) source->clear();
    if (fromMap) *fromMap = false;
    if (!levelProfileFile(d)) return builtinProfile(d);
    const auto* text = engineProfileText(gameModFiles, mapFiles, fromMap);
    if (text && source) *source = kEngineProfileFile;
    return levelProfileFrom(d, text);
}

} // namespace ta::ai
