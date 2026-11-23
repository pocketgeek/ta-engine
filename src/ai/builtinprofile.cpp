#include "ai/builtinprofile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <sstream>
#include <string>

#include "ai/classicprofile.h"

namespace tak::ai {

namespace {

struct Entry { const char* unit; int weight, limit; };

// The profile our levels were tuned on: these values reproduce the weight/limit
// pairs of the retail default AI profile (the base install's winning
// ai/default.txt), which every level read until the levels got their own tables.
// ai_profile_data re-checks them against that file. One entry per unit,
// {unit, weight, limit}, sorted by unit name.
constexpr Entry kTuned[] = {
    {"araarch", 5, 16},      {"araat", 1, 8},         {"arabow", 8, 16},
    {"arabroad", 5, 16},     {"arabuild", 10, 4},     {"aracan", 4, 5},
    {"aracastl", 10, 2},     {"aradrag", 25, 1},      {"arafast", 1, 4},
    {"arafly", 10, 4},       {"aragod", 0, 0},        {"aragren", 8, 10},
    {"arakeep", 10, 2},      {"araknigh", 5, 16},     {"aralode", 10, 7},
    {"aramana", 5, 8},       {"arangate", 0, 0},      {"arapal", 8, 12},
    {"arapries", 10, 4},     {"arapult", 1, 8},       {"arasiege", 8, 10},
    {"arasmith", 8, 16},     {"araspy", 4, 4},        {"arassh", 1, 12},
    {"arasword", 5, 12},     {"aratre", 1, 3},        {"arawall", 0, 0},
    {"arawar", 1, 4},        {"creacad", 10, 2},      {"creaeri", 25, 1},
    {"creauto", 5, 20},      {"crebarn", 1, 5},       {"crebeas", 12, 30},
    {"crebomb", 2, 12},      {"crechie", 10, 4},      {"credrag", 8, 7},
    {"crefire", 5, 16},      {"cregate", 0, 0},       {"cregatl", 1, 10},
    {"cregod", 0, 0},        {"creiron", 3, 7},       {"crelode", 10, 7},
    {"cremana", 10, 12},     {"cremech", 10, 4},      {"crenavy", 10, 2},
    {"crepris", 3, 15},      {"creshoc", 11, 30},     {"cresmit", 10, 3},
    {"crester", 3, 7},       {"cresubm", 3, 7},       {"cretort", 8, 20},
    {"crewall", 0, 0},       {"tarang", 9, 3},        {"tararch", 5, 16},
    {"tarbeak", 5, 12},      {"tarblack", 5, 12},     {"tarcage", 1, 8},
    {"tarcan", 8, 14},       {"tarcastl", 10, 2},     {"tardemon", 5, 18},
    {"tardrag", 25, 1},      {"tardung", 10, 2},      {"tarfire", 8, 18},
    {"targarg", 1, 4},       {"targod", 0, 0},        {"tarhand", 5, 12},
    {"tarhell", 10, 2},      {"tarkam", 8, 10},       {"tarknigh", 5, 16},
    {"tarlich", 1, 8},       {"tarlode", 10, 7},      {"tarmage", 1, 4},
    {"tarmana", 5, 9},       {"tarmind", 1, 2},       {"tarngate", 0, 0},
    {"tarpries", 10, 3},     {"tarsh", 1, 12},        {"tarship", 1, 8},
    {"tarspout", 5, 16},     {"tartb", 10, 3},        {"tartroop", 8, 16},
    {"tarwall", 0, 0},       {"tarwitch", 1, 7},      {"tarzom", 3, 16},
    {"verarch", 5, 16},      {"verasy", 10, 2},       {"verat", 1, 12},
    {"verbal", 15, 12},      {"verball", 3, 4},       {"verbers", 8, 16},
    {"vercastl", 10, 2},     {"vercrus", 8, 16},      {"verdrag", 25, 1},
    {"verflag", 1, 2},       {"verfltwr", 1, 5},      {"vergod", 0, 0},
    {"verharp", 3, 10},      {"verkeep", 10, 2},      {"verknigh", 8, 16},
    {"verliege", 10, 3},     {"verlight", 1, 6},      {"verlihr", 10, 3},
    {"verlode", 10, 7},      {"verman", 3, 4},        {"vermana", 10, 9},
    {"vermer", 4, 12},       {"vermort", 1, 6},       {"vermusk", 8, 16},
    {"verngate", 0, 0},      {"verpar", 1, 4},        {"verpill", 1, 2},
    {"verpult", 3, 11},      {"verscout", 1, 6},      {"versword", 5, 16},
    {"vertower", 1, 9},      {"vertrans", 1, 1},      {"vertre", 3, 6},
    {"verwall", 0, 0},       {"zonamoe", 3, 8},       {"zonbar", 1, 6},
    {"zonbasil", 1, 4},      {"zonbat", 1, 4},        {"zondrag", 25, 1},
    {"zondrake", 5, 15},     {"zonfire", 1, 4},       {"zonflies", 3, 16},
    {"zongiant", 8, 16},     {"zonglyph", 1, 9},      {"zongob", 3, 16},
    {"zongod", 0, 0},        {"zongryp", 5, 16},      {"zonhand", 10, 3},
    {"zonharp", 5, 15},      {"zonkrak", 1, 8},       {"zonlode", 10, 7},
    {"zonlord", 10, 3},      {"zonmana", 10, 9},      {"zonorc", 8, 16},
    {"zonroc", 1, 1},        {"zonsham", 10, 3},      {"zonspide", 0, 0},
    {"zonswamp", 4, 13},     {"zonter", 5, 16},       {"zontrain", 10, 3},
    {"zontroll", 8, 16},     {"zonwolf", 1, 12},
};

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
    return d != Difficulty::Classic ? kEngineProfileFile : nullptr;
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

const std::vector<uint8_t>* engineProfileText(const tak::hpi::Vfs::Files* gameModFiles,
                                              const tak::hpi::Vfs::Files* mapFiles, bool* fromMap) {
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

Profile levelProfile(Difficulty d, const tak::hpi::Vfs::Files* gameModFiles,
                     const tak::hpi::Vfs::Files* mapFiles, std::string* source, bool* fromMap) {
    if (source) source->clear();
    if (fromMap) *fromMap = false;
    if (!levelProfileFile(d)) return builtinProfile(d);
    const auto* text = engineProfileText(gameModFiles, mapFiles, fromMap);
    if (text && source) *source = kEngineProfileFile;
    return levelProfileFrom(d, text);
}

} // namespace tak::ai
