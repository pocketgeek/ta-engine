#include "client/replayfile.h"

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
    uint32_t fmt = r.u32();        // format version
    r.u32();                       // protocol version
    out.mapId = r.str();
    uint8_t crusades = r.u8(); uint8_t gods = r.u8(); r.u8();
    if (fmt >= 2) out.overridePolicy = r.u8();   // override tier the game ran under
    out.cfg.unitCap = 0;                          // fmt<3 replays ran without a unit cap
    if (fmt >= 3) out.cfg.unitCap = uint16_t(r.u32());
    out.cfg.monarchExpendable = true;             // fmt<4 replays ran monarch-expendable
    if (fmt >= 4) out.cfg.monarchExpendable = r.u8() != 0;
    if (fmt >= 5) out.cfg.stressTest = r.u8() != 0;   // fmt<5 had no stress test
    r.u32();                       // seed (setupMatch derives its own timing)
    uint8_t nslots = r.u8();
    out.crusades = crusades != 0;
    out.cfg.gods = gods != 0;
    out.cfg.slots.resize(nslots);
    int maxUsed = 0;
    for (int i = 0; i < nslots; ++i) {
        uint8_t type = r.u8(), faction = r.u8(); r.u8(); uint8_t team = r.u8();
        bool used = (type == 1 || type == 2);
        out.cfg.slots[size_t(i)] = {used, faction % 5, team};
        if (used) maxUsed = i;
    }
    // The game used setPlayerCount(maxUsedSlot+1); match it exactly (empty trailing
    // players would otherwise enter the state hash and diverge from the recording).
    out.cfg.slots.resize(size_t(maxUsed + 1));
    uint32_t nticks = r.u32();
    for (uint32_t t = 0; t < nticks && r.ok; ++t) {
        uint32_t len = r.u32();
        if (!r.avail(len)) return false;
        tak::net::Reader br(r.p, len);
        r.p += len;
        br.u32();                  // tick index (implicit = t)
        tak::net::Bundle bd;
        uint32_t nc = br.u32();
        for (uint32_t i = 0; i < nc && br.ok; ++i) bd.cmds.push_back(br.cmd());
        uint32_t ne = br.u32();
        for (uint32_t i = 0; i < ne && br.ok; ++i) {
            tak::net::Event e; e.kind = tak::net::Event::Kind(br.u8()); e.player = br.u8();
            bd.events.push_back(e);
        }
        out.bundles.push_back(std::move(bd));
    }
    return true;
}

std::string saveReplayFile(const std::string& dir, const tak::net::MpClient& mp,
                           uint64_t stampMs) {
    const auto& log = mp.replayLog();
    if (dir.empty() || log.empty()) return {};
    const tak::net::RoomView& room = mp.room();
    using tak::net::Writer;
    Writer w;
    // Header, field for field as Server::writeReplay lays it out. Keep the two in
    // step: loadReplayFile reads exactly this, and a client file and a server file
    // for the same game should be byte-identical.
    for (char ch : {'T', 'A', 'K', 'R'}) w.u8(uint8_t(ch));
    w.u32(5);                 // replay format (see Server::writeReplay for history)
    w.u32(tak::net::kNetVersion);
    w.str(room.mapId);
    w.u8(room.opts.crusades); w.u8(room.opts.gods); w.u8(room.opts.forfeitSelfDestruct);
    w.u8(room.opts.overridePolicy);
    w.u32(room.opts.unitCap); w.u8(room.opts.monarchExpendable); w.u8(room.opts.stressTest);
    w.u32(mp.startSeed());
    w.u8(uint8_t(tak::net::kMaxSlots));
    const tak::net::SlotInfo* slots = mp.startSlots();
    for (int i = 0; i < tak::net::kMaxSlots; ++i) {
        const tak::net::SlotInfo& s = slots[i];
        w.u8(s.type); w.u8(s.faction); w.u8(s.color); w.u8(s.team);
    }
    w.u32(uint32_t(log.size()));
    for (const auto& b : log) {
        w.u32(uint32_t(b.size()));
        w.b.insert(w.b.end(), b.begin(), b.end());
    }
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
