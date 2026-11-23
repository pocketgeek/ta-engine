#pragma once

// The hashed match setup and the .takrep header that records it, built in ONE place.
//
// The referee (Server::tryStart), every client (GameView::startMpGame) and replay
// playback (loadReplayFile) each turned the room's options and slot table into a
// sim::MatchConfig line for line, and inputs added later reached only some of the
// copies (060e660a: the replay lost the seed, AI levels and random starts; 3f439b67:
// stressImmediate added by hand three times). The builder below is the contract.
// Callers add only what is theirs: the VFS, the map path and the client's
// scenario diagnostics.

#include <cstdint>
#include <string>

#include "net/protocol.h"
#include "net/replayhdr.h"
#include "sim/matchsetup.h"

namespace tak::net {

// `slots` is the START table (kMaxSlots entries). The config holds one player per
// slot up to the last used (human or AI) one, as setPlayerCount(maxUsed+1) expects:
// an empty trailing player would enter the state hash. An AI slot takes its income
// multiplier from its level (Absurd and Absurdly Defensive = 2x; the commands do not
// carry it), and levels 0 and 5 play the passive (defensive) stance. `mission` forces Retail pathfinding and no
// double sight; the live paths have that from CreateGame, a replay file does not.
sim::MatchConfig matchConfigFor(const GameOptions& o, const SlotInfo* slots, uint32_t seed,
                                bool mission = false);

// The replay header for a game started from these inputs (the server's writer and the
// client's). The caller adds what only it knows: the map and mod digests and the
// gameplay-data hash the game ran on.
ReplayHeader replayHeaderFor(const GameOptions& o, const std::string& mapId, const std::string& mission,
                             const SlotInfo* startSlots, uint32_t seed);

// The inverse, for playback: the options and slot table a header records, so the
// loader rebuilds the config through matchConfigFor like the live game did. Fields a
// header does not carry (speed, fog, slot names, ...) are left at their defaults.
void matchInputsFrom(const ReplayHeader& h, GameOptions& o, SlotInfo* slots);

}  // namespace tak::net
