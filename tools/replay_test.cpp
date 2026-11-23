// replay_test -- the .takrep header round-trips, and the loader refuses what it
// cannot faithfully replay: any other format or protocol, a truncated header, an
// unknown pathfinder or an impossible slot count.
//
// Every one of those would otherwise be a SILENT failure: playback simulates a
// different game and says nothing, which is exactly the property that makes a replay
// useless for diagnosing a desync.

#include "net/replayhdr.h"

#include <cstdio>
#include <string>

using namespace tak::net;

static int g_fail = 0;
static void check(bool cond, const char* what, const std::string& detail = {}) {
    std::printf("  %-62s %s%s%s\n", what, cond ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!cond) ++g_fail;
}

// A header with every field set to something distinctive, so a field that is not
// carried across shows up as a mismatch rather than coincidentally matching a zero.
static ReplayHeader sample() {
    ReplayHeader h;
    h.mapId = "Inner Circle";
    h.mapDigest = std::string(64, 'a');
    h.modDigest = std::string(64, 'b');
    h.mission = "camp03";
    h.engineVersion = "9.9.9";
    h.crusades = 1;
    h.forfeitSelfDestruct = 1;
    h.gameMods = 1;
    h.unitCap = 1234;
    h.monarchExpendable = 1;
    h.stressTest = 1;
    h.randomStarts = 2;   // BY SLOT
    h.doubleSight = 1;
    h.benchmark = 4;
    h.seed = 0xabcdef01u;
    h.dataHash = 0x0123456789abcdefull;
    h.mapScenario = 1;
    h.powerCodes = 1;
    h.fogExplored = 2;
    for (int i = 0; i < kMaxSlots; ++i) {
        h.slotType[i] = uint8_t(i % 3);
        h.slotFaction[i] = uint8_t(i % 5);
        h.slotColor[i] = uint8_t(i);
        h.slotTeam[i] = uint8_t(i % 4);
        h.slotAiLevel[i] = uint8_t(i % 5);
    }
    return h;
}

int main() {
    std::printf("replay_test\n");

    std::printf("header round-trip:\n");
    {
        const ReplayHeader in = sample();
        Writer w;
        writeReplayHeader(w, in);
        // Skip the 4-byte magic, as the loader does.
        Reader r(w.b.data() + 4, w.b.size() - 4);
        ReplayHeader out;
        uint32_t fmt = 0, proto = 0;
        check(readReplayHeader(r, out, fmt, proto), "a written header reads back");
        check(fmt == kReplayFormat, "format version survives", std::to_string(fmt));
        check(proto == kNetVersion, "protocol version survives", std::to_string(proto));
        check(out.mapId == in.mapId, "mapId");
        check(out.mapDigest == in.mapDigest, "verified map copy survives recording");
        check(out.modDigest == in.modDigest, "verified game mod copy survives recording");
        check(out.mission == in.mission, "mission (a campaign replay is not a skirmish)");
        check(out.engineVersion == in.engineVersion, "engine version");
        check(out.seed == in.seed, "seed");
        check(out.randomStarts == in.randomStarts, "randomStarts (start locations, BY SLOT included)");
        check(out.doubleSight == in.doubleSight, "double sight survives recording");
        check(out.benchmark == in.benchmark, "benchmark intensity (the staged spawns)");
        check(out.dataHash == in.dataHash, "gameplay data hash");
        check(out.unitCap == in.unitCap, "unitCap");
        check(out.monarchExpendable == in.monarchExpendable, "monarchExpendable");
        check(out.stressTest == in.stressTest, "stressTest");
        check(out.gameMods == in.gameMods, "gameMods");
        check(out.mapScenario == in.mapScenario, "MAP SCENARIO");
        check(out.powerCodes == in.powerCodes, "POWER CODES");
        check(out.fogExplored == in.fogExplored, "FOG OF WAR");
        bool slotsOk = true;
        for (int i = 0; i < kMaxSlots; ++i)
            slotsOk &= out.slotType[i] == in.slotType[i] &&
                       out.slotFaction[i] == in.slotFaction[i] &&
                       out.slotColor[i] == in.slotColor[i] &&
                       out.slotTeam[i] == in.slotTeam[i] &&
                       out.slotAiLevel[i] == in.slotAiLevel[i];
        check(slotsOk, "every slot, INCLUDING its AI level");
        // The income multiplier is derived from the AI level at setup and the recorded
        // commands do not carry it: without it an Absurd AI would replay on normal income.
        check(out.slotAiLevel[4] == in.slotAiLevel[4], "AI level specifically",
              std::to_string(out.slotAiLevel[4]));
    }

    std::printf("the header reads exactly to its end:\n");
    {
        Writer w;
        writeReplayHeader(w, sample());
        w.u8(0xee);   // the bundle count's first byte
        Reader r(w.b.data() + 4, w.b.size() - 4);
        ReplayHeader out;
        uint32_t fmt = 0, proto = 0;
        check(readReplayHeader(r, out, fmt, proto) && r.p + 1 == r.end, "the reader stops at the last header byte");
    }

    std::printf("pathfinding mode:\n");
    for (auto mode : {tak::sim::PathfindingMode::Retail, tak::sim::PathfindingMode::Legion}) {
        auto in = sample(); in.mission.clear(); in.pathfindingMode = mode;
        Writer w; writeReplayHeader(w, in);
        const size_t modeAt = w.b.size() - size_t(kMaxSlots) * 5 - 5;   // ... mode, count, slots, mapScenario, powerCodes, fogExplored
        ReplayHeader out; uint32_t fmt=0, proto=0;
        Reader r(w.b.data()+4,w.b.size()-4);
        check(readReplayHeader(r,out,fmt,proto) && out.pathfindingMode==mode,
              "explicit skirmish pathfinder survives recording");
        // 1-3 are reserved.
        for (uint8_t unknown : {uint8_t(1), uint8_t(2), uint8_t(3), uint8_t(5), uint8_t(255)}) {
            auto bad = w.b; bad[modeAt] = unknown;
            Reader invalid(bad.data()+4,bad.size()-4);
            check(!readReplayHeader(invalid,out,fmt,proto), "unknown pathfinder is refused");
        }
    }
    for(auto mode:{tak::sim::PathfindingMode::Legion}) {
        auto in=sample(); in.pathfindingMode=mode;
        Writer w; writeReplayHeader(w,in);
        Reader r(w.b.data()+4,w.b.size()-4); ReplayHeader out; uint32_t fmt=0,proto=0;
        check(readReplayHeader(r,out,fmt,proto) && out.pathfindingMode==tak::sim::PathfindingMode::Retail,
              "campaign playback always uses Retail");
    }

    std::printf("the loader refuses what it cannot replay:\n");
    check(supportedReplayProtocol(kReplayFormat, kNetVersion), "the current format and protocol replay");
    check(!supportedReplayProtocol(kReplayFormat, kNetVersion - 1), "an earlier protocol is refused");
    check(!supportedReplayProtocol(kReplayFormat, kNetVersion + 1), "a later protocol is refused");
    check(!supportedReplayProtocol(kReplayFormat - 1, kNetVersion), "an earlier format is refused");
    check(!supportedReplayProtocol(kReplayFormat + 1, kNetVersion), "a later format is refused");
    check(!supportedReplayProtocol(0, kNetVersion), "format 0 is refused");
    // Any format but the current one: the reader stops after the two version words,
    // which come back for the caller's message.
    for (uint32_t format : {kReplayFormat - 1, kReplayFormat + 7, 6u}) {
        Writer w;
        writeReplayHeader(w, sample());
        w.b[4] = uint8_t(format);   // format is the u32 right after the magic
        Reader r(w.b.data() + 4, w.b.size() - 4);
        ReplayHeader out;
        uint32_t fmt = 0, proto = 0;
        check(!readReplayHeader(r, out, fmt, proto) && fmt == format && proto == kNetVersion && r.ok,
              "another format version is refused", std::to_string(format));
    }
    {
        Writer w;
        writeReplayHeader(w, sample());
        w.b[4] = 0;                            // format 0 is not a thing
        Reader r(w.b.data() + 4, w.b.size() - 4);
        ReplayHeader out;
        uint32_t fmt = 0, proto = 0;
        check(!readReplayHeader(r, out, fmt, proto), "format 0 is refused");
    }
    {
        // Truncated header.
        Writer w;
        writeReplayHeader(w, sample());
        w.b.resize(w.b.size() / 2);
        Reader r(w.b.data() + 4, w.b.size() - 4);
        ReplayHeader out;
        uint32_t fmt = 0, proto = 0;
        check(!readReplayHeader(r, out, fmt, proto), "a truncated header is refused");
    }
    {
        // A slot count larger than the table it fills.
        Writer w;
        writeReplayHeader(w, sample());
        // The slot count is the last byte before the slot block: find it by rebuilding
        // a header and noting where the block starts.
        Writer probe;
        ReplayHeader h = sample();
        writeReplayHeader(probe, h);
        size_t countAt = probe.b.size() - size_t(kMaxSlots) * 5 - 4;   // ... count, slots, mapScenario, powerCodes, fogExplored
        w.b[countAt] = uint8_t(kMaxSlots + 9);
        Reader r(w.b.data() + 4, w.b.size() - 4);
        ReplayHeader out;
        uint32_t fmt = 0, proto = 0;
        check(!readReplayHeader(r, out, fmt, proto),
              "an impossible slot count is refused (would overrun the table)");
    }

    std::printf(g_fail ? "replay_test: %d FAILURE(S)\n" : "replay_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
