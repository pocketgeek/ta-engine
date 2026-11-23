#pragma once

// The .takrep header, in ONE place: both writers (Server::writeReplay and the
// client's net/replaywriter.h) fill this struct and the loader reads it, so a
// field that is added is added once.
//
// A replay carries EVERY input the match started from -- options, the slot table
// with each AI level, the seed, the mission, the map and mod digests -- or playback
// silently simulates a different game. It also carries hash checkpoints: a recorded
// (tick, hash) trail from the original game is what lets a replay say WHERE it
// diverged from what actually happened.
//
// Only the current format under the current protocol is read; anything else is
// refused, since a different engine's simulation cannot be replayed faithfully.

#include <cstdint>
#include <string>
#include <vector>

#include "net/protocol.h"

namespace tak::net {

// Bump when the layout changes.
inline constexpr uint32_t kReplayFormat = 15;

// The largest .takrep the loaders accept (the client picker and the writer's re-read).
inline constexpr uint64_t kMaxReplayBytes = 512ull * 1024 * 1024;

// A replay plays only on the engine that recorded it: the same format and protocol.
inline bool supportedReplayProtocol(uint32_t format, uint32_t protocol) {
    return format == kReplayFormat && protocol == kNetVersion;
}

struct ReplayHeader {
    std::string mapId;
    std::string mapDigest, modDigest;
    std::string mission;          // campaign mission stem ("" = skirmish)
    std::string engineVersion;    // tak::kVersion of the build that recorded it
    uint8_t crusades = 0, forfeitSelfDestruct = 0;
    uint8_t gameMods = 0;          // 1 = the room ran the host's game mods (modDigest)
    uint32_t unitCap = 0;
    uint8_t monarchExpendable = 0, stressTest = 0, randomStarts = 0;
    sim::PathfindingMode pathfindingMode = sim::PathfindingMode::Retail;
    uint8_t doubleSight = 0;
    uint8_t benchmark = 0;        // benchmark intensity (0 = off)
    uint32_t seed = 0;
    uint64_t dataHash = 0;        // hpi::gameplayHash of the data the game ran on
    uint8_t slotType[kMaxSlots] = {};
    uint8_t slotFaction[kMaxSlots] = {};
    uint8_t slotColor[kMaxSlots] = {};
    uint8_t slotTeam[kMaxSlots] = {};
    uint8_t slotAiLevel[kMaxSlots] = {};
    uint8_t mapScenario = 0;      // the room's MAP SCENARIO option
    uint8_t powerCodes = 0;       // bit 0 the room's POWER CODES option, bit 1 its map revealed at start
    uint8_t fogExplored = 1;      // the room's FOG OF WAR option, 0..2 (MatchConfig::roomMapRevealed/roomLosOff)
};

// One recorded (tick, hash) pair from the ORIGINAL game, with posDigest
// (World::posDigest: ids, types, owners, positions, hp and the front order only, no
// bookkeeping), so a verifier can tell a hash-layout change (the hash diverges, the
// digest does not) from a behaviour change (both diverge). 0 = the recorder did not
// compute one for that tick. The digest travels in the file only, never on the wire.
struct ReplayCheck {
    uint32_t tick = 0;
    uint64_t hash = 0;
    uint64_t posDigest = 0;
};

// Bytes per checkpoint record: u32 tick, u64 hash, u64 posDigest.
inline constexpr size_t kReplayCheckBytes = 20;

inline void writeReplayHeader(Writer& w, const ReplayHeader& h) {
    for (char ch : {'T', 'A', 'K', 'R'}) w.u8(uint8_t(ch));
    w.u32(kReplayFormat);
    w.u32(kNetVersion);
    w.str(h.mapId);
    w.u8(h.crusades);
    w.u8(h.forfeitSelfDestruct);
    w.u8(h.gameMods);
    w.u32(h.unitCap);
    w.u8(h.monarchExpendable);
    w.u8(h.stressTest);
    w.u32(h.seed);
    w.u8(h.randomStarts);
    w.u8(h.benchmark);
    w.str(h.mission);
    w.str(h.engineVersion);
    w.u64(h.dataHash);
    w.u8(h.doubleSight);
    w.str(h.mapDigest);
    w.str(h.modDigest);
    w.u8(uint8_t(h.pathfindingMode));
    w.u8(uint8_t(kMaxSlots));
    for (int i = 0; i < kMaxSlots; ++i) {
        w.u8(h.slotType[i]);
        w.u8(h.slotFaction[i]);
        w.u8(h.slotColor[i]);
        w.u8(h.slotTeam[i]);
        w.u8(h.slotAiLevel[i]);
    }
    w.u8(h.mapScenario);
    w.u8(h.powerCodes);
    w.u8(h.fogExplored);
}

// Reads the header. `fmt` and `proto` come back even when the header is refused,
// so the caller can say why. Returns false on a short or malformed header, or on any
// format other than kReplayFormat (the caller still checks `proto`).
inline bool readReplayHeader(Reader& r, ReplayHeader& h, uint32_t& fmt, uint32_t& proto) {
    fmt = r.u32();
    proto = r.u32();
    if (!r.ok || fmt != kReplayFormat) return false;
    h.mapId = r.str();
    h.crusades = r.u8();
    h.forfeitSelfDestruct = r.u8();
    h.gameMods = r.u8();
    h.unitCap = r.u32();
    h.monarchExpendable = r.u8();
    h.stressTest = r.u8();
    h.seed = r.u32();
    h.randomStarts = r.u8();
    h.benchmark = r.u8();
    h.mission = r.str();
    h.engineVersion = r.str();
    h.dataHash = r.u64();
    h.doubleSight = r.u8();
    h.mapDigest = r.str();
    h.modDigest = r.str();
    const auto pathMode = r.u8();
    if (!sim::validPathfindingMode(pathMode)) return false;
    h.pathfindingMode = h.mission.empty() ? sim::PathfindingMode(pathMode) : sim::PathfindingMode::Retail;
    const uint8_t nslots = r.u8();
    if (!r.ok || nslots > kMaxSlots) return false;
    for (int i = 0; i < nslots; ++i) {
        h.slotType[i] = r.u8();
        h.slotFaction[i] = r.u8();
        h.slotColor[i] = r.u8();
        h.slotTeam[i] = r.u8();
        h.slotAiLevel[i] = r.u8();
    }
    h.mapScenario = r.u8();
    h.powerCodes = r.u8();
    h.fogExplored = r.u8();
    return r.ok;
}

}  // namespace tak::net
