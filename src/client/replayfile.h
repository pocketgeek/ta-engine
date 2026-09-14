#pragma once

// A parsed .takrep replay: enough to rebuild the world and replay it.
// Extracted from client/main.cpp; kept at global scope so its existing
// unqualified use sites there are unchanged.

#include <cstdint>
#include <string>
#include <vector>

#include "net/client.h"        // tak::net::Bundle (+ transitively net/protocol.h)
#include "sim/matchsetup.h"    // tak::sim::MatchConfig

struct ReplayFile {
    std::string mapId;
    bool crusades = false;
    uint8_t overridePolicy = 1;   // override tier the recorded game ran under
    tak::sim::MatchConfig cfg;
    std::vector<tak::net::Bundle> bundles;
};

// Load a .takrep (header + tick bundles). Returns false on a malformed file.
bool loadReplayFile(const std::string& path, ReplayFile& out);

// ---- writing ---------------------------------------------------------------
//
// Save the replay the CLIENT recorded (MpClient::replayLog) as a .takrep, in the
// same format and the same byte layout the server's --replaydir writes: the header
// fields below are mirrored from Server::writeReplay, and the bundles are the raw
// payloads the server broadcast, so the two writers cannot drift.
//
// Every human player records its own copy, which is what makes a replay survive a
// server that keeps none -- and a client only ever has the game it was in, so there
// is nothing here that a player could not already see.
//
// `dir` is the user's config directory (settingsPath()'s folder), so replays live
// beside settings.ini rather than in whatever the working directory happened to be.
// Returns the written path, or empty on failure.
std::string saveReplayFile(const std::string& dir, const tak::net::MpClient& mp,
                           uint64_t stampMs);
