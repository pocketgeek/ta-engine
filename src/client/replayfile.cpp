#include "client/replayfile.h"

#include "version.h"

#include "ai/ai.h"
#include "net/protocol.h"      // tak::net::Reader / Event / Command

#include <cstdio>
#include <utility>

bool loadReplayFile(const std::string& path, ReplayFile& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d(size_t(n < 0 ? 0 : n));
    if (!d.empty() && std::fread(d.data(), 1, d.size(), f) != d.size()) { std::fclose(f); return false; }
    std::fclose(f);
    if (d.size() < 4 || d[0] != 'T' || d[1] != 'A' || d[2] != 'K' || d[3] != 'R') return false;
    tak::net::Reader r(d.data() + 4, d.size() - 4);
    tak::net::ReplayHeader h;
    uint32_t fmt = 0, proto = 0;
    if (!tak::net::readReplayHeader(r, h, fmt, proto)) return false;
    // REFUSE what we cannot replay faithfully. Both of these were recorded from the
    // start and neither was ever checked, so an old file ran happily under new
    // simulation rules and quietly produced a different game.
    out.formatVersion = fmt;
    out.protocolVersion = proto;
    if (proto != tak::net::kNetVersion) {
        out.error = "recorded under protocol v" + std::to_string(proto) +
                    ", this build is v" + std::to_string(tak::net::kNetVersion) +
                    " -- the simulation rules have changed since";
        return false;
    }
    out.mapId = h.mapId;
    out.mission = h.mission;
    out.crusades = h.crusades != 0;
    out.overridePolicy = h.overridePolicy;
    out.dataHash = h.dataHash;
    out.engineVersion = h.engineVersion;
    out.cfg.gods = h.gods != 0;
    out.cfg.unitCap = uint16_t(h.unitCap);
    out.cfg.monarchExpendable = h.monarchExpendable != 0;
    out.cfg.stressTest = h.stressTest != 0;
    out.cfg.randomStarts = h.randomStarts != 0;
    out.cfg.benchmark = h.benchmark;
    out.cfg.startSeed = h.seed;          // the game's own seed: was recorded, then dropped
    out.cfg.slots.resize(tak::net::kMaxSlots);
    int maxUsed = 0;
    for (int i = 0; i < tak::net::kMaxSlots; ++i) {
        const bool used = (h.slotType[i] == 1 || h.slotType[i] == 2);
        // An AI slot's income multiplier is derived from its LEVEL at setup, and the
        // recorded commands do not carry it -- so without this an Absurd AI replayed
        // on normal income and its whole production curve diverged.
        const float mm = h.slotType[i] == 2
            ? tak::ai::incomeMultFor(tak::ai::difficultyFromLevel(h.slotAiLevel[i])) : 1.0f;
        out.cfg.slots[size_t(i)] = {used, h.slotFaction[i] % 5, h.slotTeam[i], mm};
        if (used) maxUsed = i;
    }
    // The game used setPlayerCount(maxUsedSlot+1); match it exactly (empty trailing
    // players would otherwise enter the state hash and diverge from the recording).
    out.cfg.slots.resize(size_t(maxUsed + 1));
    uint32_t nticks = r.u32();
    if (!r.ok) return false;
    // Bound the count by what is actually LEFT in the file before reserving. A count is
    // just four bytes a corrupt file can set to anything, and reserving on trust turns
    // a truncated 200-byte replay into a multi-gigabyte allocation that throws where
    // the caller expects a clean false. Each bundle costs at least its 4-byte length.
    if (uint64_t(nticks) * 4 > uint64_t(r.end - r.p)) return false;
    out.bundles.reserve(nticks);
    for (uint32_t t = 0; t < nticks; ++t) {
        uint32_t len = r.u32();
        if (!r.ok || !r.avail(len)) return false;
        tak::net::Reader br(r.p, len);
        r.p += len;
        const uint32_t tick = br.u32();
        // The recorded tick index was read and thrown away. It is the one thing that
        // can prove the bundle stream is intact and in order, so check it.
        if (!br.ok || tick != t) return false;
        tak::net::Bundle bd;
        uint32_t nc = br.u32();
        for (uint32_t i = 0; i < nc && br.ok; ++i) bd.cmds.push_back(br.cmd());
        uint32_t ne = br.u32();
        for (uint32_t i = 0; i < ne && br.ok; ++i) {
            tak::net::Event e; e.kind = tak::net::Event::Kind(br.u8()); e.player = br.u8();
            bd.events.push_back(e);
        }
        if (!br.ok) return false;    // a truncated bundle used to load as a success
        out.bundles.push_back(std::move(bd));
    }
    // Hash checkpoints (format 6+). Absent in an older file, which simply means
    // playback cannot be checked against the original.
    if (fmt >= 6) {
        uint32_t nchecks = r.u32();
        if (!r.ok) return false;
        if (uint64_t(nchecks) * 12 > uint64_t(r.end - r.p)) return false;   // u32 + u64 each
        out.checks.reserve(nchecks);
        for (uint32_t i = 0; i < nchecks; ++i) {
            tak::net::ReplayCheck c;
            c.tick = r.u32();
            c.hash = r.u64();
            if (!r.ok) return false;
            out.checks.push_back(c);
        }
    }
    return true;
}

std::string saveReplayFile(const std::string& dir, const tak::net::MpClient& mp,
                           uint64_t stampMs, uint64_t gameplayHash) {
    const auto& log = mp.replayLog();
    if (dir.empty() || log.empty()) return {};
    const tak::net::RoomView& room = mp.room();
    const tak::net::SlotInfo* slots = mp.startSlots();
    // Header shared with the server writer and the loader -- see net/replayhdr.h.
    tak::net::ReplayHeader h;
    h.mapId = room.mapId;
    h.mission = room.mission;
    h.engineVersion = tak::kVersion;
    h.crusades = room.opts.crusades;
    h.gods = room.opts.gods;
    h.forfeitSelfDestruct = room.opts.forfeitSelfDestruct;
    h.overridePolicy = room.opts.overridePolicy;
    h.unitCap = room.opts.unitCap;
    h.monarchExpendable = room.opts.monarchExpendable;
    h.stressTest = room.opts.stressTest;
    h.randomStarts = room.opts.randomStarts;
    h.benchmark = uint8_t(room.opts.benchmark);
    h.seed = mp.startSeed();
    // The hash of the data this GAME ran on, at its override tier -- not the
    // pure-retail fingerprint the handshake uses. Under a Full-tier game the two
    // differ, and it is the effective one a replay has to be checked against. The
    // server's writer already records that; this recorded the handshake hash.
    h.dataHash = gameplayHash;
    for (int i = 0; i < tak::net::kMaxSlots; ++i) {
        h.slotType[i] = slots[i].type;
        h.slotFaction[i] = slots[i].faction;
        h.slotColor[i] = slots[i].color;
        h.slotTeam[i] = slots[i].team;
        h.slotAiLevel[i] = slots[i].aiLevel;
    }
    tak::net::Writer w;
    tak::net::writeReplayHeader(w, h);
    w.u32(uint32_t(log.size()));
    for (const auto& b : log) {
        w.u32(uint32_t(b.size()));
        w.b.insert(w.b.end(), b.begin(), b.end());
    }
    // This client's own hash trail, so a replay can say WHERE playback diverged
    // from the game as it was actually played -- not merely that two reruns agree.
    const auto& checks = mp.hashLog();
    w.u32(uint32_t(checks.size()));
    for (const auto& c : checks) { w.u32(c.tick); w.u64(c.hash); }
    // Named by the game and a timestamp, so several replays coexist and a rerun of
    // the same game does not overwrite the earlier one.
    std::string path = dir + "game-" + std::to_string(room.id) + "-" +
                       std::to_string(stampMs) + ".takrep";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return {};
    const bool ok = std::fwrite(w.b.data(), 1, w.b.size(), f) == w.b.size();
    std::fclose(f);
    return ok ? path : std::string();
}
