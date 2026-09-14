// replay_test -- the .takrep header round-trips, and the loader refuses what it
// cannot faithfully replay.
//
// Reviewed complaint, all of it true of the old format: the loader "accepts
// unsupported format versions, ignores recorded tick indices, and can return success
// after malformed inner bundles or an incomplete header", and the header itself was
// missing inputs the match started from -- random starts, the per-slot AI level, the
// benchmark intensity, the mission -- while the seed and the protocol version were
// recorded and then thrown away.
//
// Every one of those is a SILENT failure: playback simulates a different game and
// says nothing, which is exactly the property that makes a replay useless for
// diagnosing a desync. So the tests below care less about the happy path than about
// each way a bad file used to be accepted.

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
    h.mission = "camp03";
    h.engineVersion = "9.9.9";
    h.crusades = 1;
    h.gods = 1;
    h.forfeitSelfDestruct = 1;
    h.overridePolicy = 2;
    h.unitCap = 1234;
    h.monarchExpendable = 1;
    h.stressTest = 1;
    h.randomStarts = 1;
    h.benchmark = 4;
    h.seed = 0xabcdef01u;
    h.dataHash = 0x0123456789abcdefull;
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
        check(out.mission == in.mission, "mission (a campaign replay is not a skirmish)");
        check(out.engineVersion == in.engineVersion, "engine version");
        check(out.seed == in.seed, "seed (was recorded, then discarded by the loader)");
        check(out.randomStarts == in.randomStarts, "randomStarts (shuffled start positions)");
        check(out.benchmark == in.benchmark, "benchmark intensity (the staged spawns)");
        check(out.dataHash == in.dataHash, "gameplay data hash");
        check(out.unitCap == in.unitCap, "unitCap");
        check(out.monarchExpendable == in.monarchExpendable, "monarchExpendable");
        check(out.stressTest == in.stressTest, "stressTest");
        check(out.overridePolicy == in.overridePolicy, "overridePolicy");
        bool slotsOk = true;
        for (int i = 0; i < kMaxSlots; ++i)
            slotsOk &= out.slotType[i] == in.slotType[i] &&
                       out.slotFaction[i] == in.slotFaction[i] &&
                       out.slotColor[i] == in.slotColor[i] &&
                       out.slotTeam[i] == in.slotTeam[i] &&
                       out.slotAiLevel[i] == in.slotAiLevel[i];
        check(slotsOk, "every slot, INCLUDING its AI level");
        // The AI level is the one that silently changed the simulation: the income
        // multiplier is derived from it at setup and the recorded commands do not
        // carry it, so an Absurd AI replayed on normal income.
        check(out.slotAiLevel[4] == in.slotAiLevel[4], "AI level specifically",
              std::to_string(out.slotAiLevel[4]));
    }

    std::printf("the loader refuses what it cannot replay:\n");
    {
        // A format from the future. The old loader read it anyway, interpreting
        // whatever bytes happened to follow.
        Writer w;
        writeReplayHeader(w, sample());
        w.b[4] = uint8_t(kReplayFormat + 7);   // format is the u32 right after the magic
        Reader r(w.b.data() + 4, w.b.size() - 4);
        ReplayHeader out;
        uint32_t fmt = 0, proto = 0;
        check(!readReplayHeader(r, out, fmt, proto), "a NEWER format version is refused");
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
        // Truncated header. The old reader ran off the end and returned success.
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
        size_t countAt = probe.b.size() - size_t(kMaxSlots) * 5 - 1;
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
