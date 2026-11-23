#pragma once

// The build profiles of OUR AI levels (Defensive, Easy, Normal, Hard, Absurd,
// Absurdly Defensive, Passive): per-unit weight and limit, built into the engine.
//
// These levels never read a game's ai/default.txt. Each has its own built-in table
// (builtinProfile). ONE file, ai/takengine.txt, adjusts all six of them: a GAME MOD
// or the room's MAP PACKAGE may carry it in its ai/ folder -- loose in a game-mod
// pack folder (mods/game/<pack>/ai/takengine.txt), at ai/ inside an archive in a
// game mod, or at ai/ in the map's .kmp (it travels in the room's map package,
// net/mappackage.h).
//
// Each level applies the file's lines on top of its OWN built-in table, so the levels
// keep their differences (and paramsFor's per-level limit scaling still applies): a
// `weight` or `limit` line replaces that unit's entry in every level, and every other
// unit keeps each level's built-in values (`limit <unit> 0` keeps every level from
// building that unit). A game mod's file wins over the map's (the map's is read only
// when no game mod supplies one). A file of that name anywhere else (a pack's root,
// another folder) is not read, and the base install and cosmetic packs can never
// supply one.
// Classic (retail's own AI, shown to players as RETAIL) is not built in and never
// reads ai/takengine.txt: it reads ai/<aiprofile>.txt, else the game's
// ai/default.txt (ai/classicprofile.h).
//
// Line format: `weight <unit> <n>` or
// `limit <unit> <n>`, whitespace-separated, unit names case-insensitive; a line
// starting with '/' and anything that is not one of the two keywords is skipped,
// and a later line for the same unit and keyword wins.
//
// Server-side only (an AI input, never part of the gameplay-data hash).

#include <cstdint>
#include <string_view>
#include <vector>

#include "ai/ai.h"
#include "hpi/hpi.h"

namespace tak::ai {

// A level's built-in profile; Classic (and anything out of range) has an empty one.
const Profile& builtinProfile(Difficulty d);

// The one file that adjusts all of our levels.
inline constexpr const char* kEngineProfileFile = "ai/takengine.txt";

// The file that adjusts a level (kEngineProfileFile for each of our levels), or
// nullptr for Classic.
const char* levelProfileFile(Difficulty d);

// Apply profile lines to `prof`: each `weight`/`limit` line sets that unit's entry.
void applyProfileText(std::string_view text, Profile& prof);

// A room's ai/takengine.txt: from `gameModFiles` (the room's verified game-mod
// package) if it has one, else from `mapFiles` (the room's verified map package), else
// null. Both are Vfs::Files keyed by lowercased path, or null for none. `fromMap`,
// when given, receives whether it was the map package's.
const std::vector<uint8_t>* engineProfileText(const tak::hpi::Vfs::Files* gameModFiles,
                                              const tak::hpi::Vfs::Files* mapFiles = nullptr,
                                              bool* fromMap = nullptr);

// A level's effective profile: builtinProfile(d) with `text` (ai/takengine.txt's
// bytes, or null) applied on top; Classic ignores it.
Profile levelProfileFrom(Difficulty d, const std::vector<uint8_t>* text);

// The same, finding the file with engineProfileText. `source`, when given, receives
// the path of the file applied ("" for the built-in table) and `fromMap` whether it
// was the map package's.
Profile levelProfile(Difficulty d, const tak::hpi::Vfs::Files* gameModFiles,
                     const tak::hpi::Vfs::Files* mapFiles = nullptr, std::string* source = nullptr,
                     bool* fromMap = nullptr);

} // namespace tak::ai
