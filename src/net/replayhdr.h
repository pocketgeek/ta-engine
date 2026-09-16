#pragma once

// The .tarep header, in ONE place.
//
// It used to be written out field-by-field in two places -- Server::writeReplay and
// the client's saveReplayFile -- and read in a third, with a comment in each asking
// the next person to keep them in step. That is not a contract, it is a hope. The
// struct below is the contract: both writers fill it, the loader reads it, and a
// field that is added is added once.
//
// FORMAT 6 exists because a replay has to carry EVERY input the match started from,
// or playback silently simulates a different game. Format 5 was missing:
//   * randomStarts -- playback used fixed start positions for a shuffled match;
//   * the per-slot AI level -- an Absurd AI lost its doubled income, changing its
//     whole production curve (the recorded commands do not carry that; the income
//     multiplier is derived from the level at setup);
//   * the benchmark intensity -- playback skipped the staged spawns entirely;
//   * the mission stem -- a campaign replay was rebuilt as an ordinary skirmish,
//     with none of its placements or scripting;
//   * the seed, which was written but DISCARDED by the loader;
//   * any way to tell that playback ran the same engine and the same game data.
//
// It also carries hash checkpoints. Two identical reruns only prove the reruns
// agree; a recorded (tick, hash) trail from the original game is what lets a replay
// say WHERE it diverged from what actually happened.

#include <cstdint>
#include <string>
#include <vector>

#include "net/protocol.h"

namespace ta::net {

// Bump when the layout changes, and handle the older values in readReplayHeader.
inline constexpr uint32_t kReplayFormat = 6;

struct ReplayHeader {
    std::string mapId;
    std::string mission;          // campaign mission stem ("" = skirmish)
    std::string engineVersion;    // ta::kVersion of the build that recorded it
    uint8_t forfeitSelfDestruct = 0;
    uint8_t overridePolicy = 1;
    uint32_t unitCap = 0;
    uint8_t monarchExpendable = 0, stressTest = 0, randomStarts = 0;
    uint8_t benchmark = 0;        // benchmark intensity (0 = off)
    uint32_t seed = 0;
    uint64_t dataHash = 0;        // hpi::gameplayHash of the data the game ran on
    uint8_t slotType[kMaxSlots] = {};
    uint8_t slotFaction[kMaxSlots] = {};
    uint8_t slotColor[kMaxSlots] = {};
    uint8_t slotTeam[kMaxSlots] = {};
    uint8_t slotAiLevel[kMaxSlots] = {};
};

// One recorded (tick, hash) pair from the ORIGINAL game.
struct ReplayCheck {
    uint32_t tick = 0;
    uint64_t hash = 0;
};

inline void writeReplayHeader(Writer& w, const ReplayHeader& h) {
    for (char ch : {'T', 'A', 'K', 'R'}) w.u8(uint8_t(ch));
    w.u32(kReplayFormat);
    w.u32(kNetVersion);
    w.str(h.mapId);
    w.u8(h.forfeitSelfDestruct);
    w.u8(h.overridePolicy);
    w.u32(h.unitCap);
    w.u8(h.monarchExpendable);
    w.u8(h.stressTest);
    w.u32(h.seed);
    // --- format 6 ---
    w.u8(h.randomStarts);
    w.u8(h.benchmark);
    w.str(h.mission);
    w.str(h.engineVersion);
    w.u64(h.dataHash);
    w.u8(uint8_t(kMaxSlots));
    for (int i = 0; i < kMaxSlots; ++i) {
        w.u8(h.slotType[i]);
        w.u8(h.slotFaction[i]);
        w.u8(h.slotColor[i]);
        w.u8(h.slotTeam[i]);
        w.u8(h.slotAiLevel[i]);   // format 6: was missing, so Absurd AI lost its income
    }
}

// Reads the header. `fmt` and `proto` come back so the caller can refuse a file it
// cannot faithfully replay -- both were recorded before and neither was ever checked.
// Returns false on a short or malformed header.
inline bool readReplayHeader(Reader& r, ReplayHeader& h, uint32_t& fmt, uint32_t& proto) {
    fmt = r.u32();
    proto = r.u32();
    if (!r.ok || fmt == 0 || fmt > kReplayFormat) return false;
    h.mapId = r.str();
    h.forfeitSelfDestruct = r.u8();
    if (fmt >= 2) h.overridePolicy = r.u8();
    h.unitCap = 0;                       // fmt<3 ran without a unit cap
    if (fmt >= 3) h.unitCap = r.u32();
    h.monarchExpendable = 1;             // fmt<4 ran monarch-expendable
    if (fmt >= 4) h.monarchExpendable = r.u8();
    if (fmt >= 5) h.stressTest = r.u8();
    h.seed = r.u32();
    if (fmt >= 6) {
        h.randomStarts = r.u8();
        h.benchmark = r.u8();
        h.mission = r.str();
        h.engineVersion = r.str();
        h.dataHash = r.u64();
    }
    const uint8_t nslots = r.u8();
    if (!r.ok || nslots > kMaxSlots) return false;
    for (int i = 0; i < nslots; ++i) {
        h.slotType[i] = r.u8();
        h.slotFaction[i] = r.u8();
        h.slotColor[i] = r.u8();
        h.slotTeam[i] = r.u8();
        if (fmt >= 6) h.slotAiLevel[i] = r.u8();
    }
    return r.ok;
}

}  // namespace ta::net
