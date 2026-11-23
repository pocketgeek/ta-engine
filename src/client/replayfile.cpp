#include "client/replayfile.h"

#include "net/matchconfig.h"   // matchConfigFor / replayHeaderFor
#include "net/protocol.h"      // tak::net::Reader / getBundle
#include "net/replayhdr.h"     // kMaxReplayBytes
#include "net/replaywriter.h"  // kReplayOpenCount

#include <cstdio>
#include <filesystem>
#include <utility>
#include "util/u8path.h"

bool loadReplayFile(const std::string& path, ReplayFile& out) {
#ifdef _WIN32
    FILE* f = _wfopen(tak::u8path(path).c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0 || uint64_t(n) > tak::net::kMaxReplayBytes) {
        std::fclose(f); out.error = "replay file exceeds supported size"; return false;
    }
    std::vector<uint8_t> d(size_t(n < 0 ? 0 : n));
    if (!d.empty() && std::fread(d.data(), 1, d.size(), f) != d.size()) { std::fclose(f); return false; }
    std::fclose(f);
    if (d.size() < 4 || d[0] != 'T' || d[1] != 'A' || d[2] != 'K' || d[3] != 'R') return false;
    tak::net::Reader r(d.data() + 4, d.size() - 4);
    tak::net::ReplayHeader h;
    uint32_t fmt = 0, proto = 0;
    const bool headerOk = tak::net::readReplayHeader(r, h, fmt, proto);
    out.formatVersion = fmt;
    out.protocolVersion = proto;
    // REFUSE what we cannot replay faithfully: a file from any other engine version
    // would run under different simulation rules and quietly produce a different game.
    if (r.ok && !tak::net::supportedReplayProtocol(fmt, proto)) {
        out.error = proto != tak::net::kNetVersion
            ? "recorded under protocol v" + std::to_string(proto) + ", this build is v" +
                  std::to_string(tak::net::kNetVersion) + " -- this replay protocol is unsupported"
            : "replay format " + std::to_string(fmt) + ", this build reads format " +
                  std::to_string(tak::net::kReplayFormat) + " -- this replay format is unsupported";
        return false;
    }
    if (!headerOk) return false;
    out.mapId = h.mapId;
    out.mapDigest = h.mapDigest;
    out.modDigest = h.modDigest;
    out.mission = h.mission;
    out.crusades = h.crusades != 0;
    out.gameMods = h.gameMods;
    out.dataHash = h.dataHash;
    out.engineVersion = h.engineVersion;
    // The same builder the referee and the live clients use (net/matchconfig.h), fed
    // the options and START slot table the header recorded.
    tak::net::GameOptions opts;
    tak::net::SlotInfo slots[tak::net::kMaxSlots];
    tak::net::matchInputsFrom(h, opts, slots);
    out.cfg = tak::net::matchConfigFor(opts, slots, h.seed, !h.mission.empty());
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
        // The recorded tick index proves the bundle stream is intact and in order.
        uint32_t tick = 0;
        tak::net::Bundle bd;
        if (!tak::net::getBundle(br, tick, bd, /*strict=*/true) || tick != t) return false;
        out.bundles.push_back(std::move(bd));
    }
    // Hash checkpoints from the original game.
    uint32_t nchecks = r.u32();
    if (!r.ok) return false;
    if (uint64_t(nchecks) * tak::net::kReplayCheckBytes > uint64_t(r.end - r.p)) return false;
    out.checks.reserve(nchecks);
    for (uint32_t i = 0; i < nchecks; ++i) {
        tak::net::ReplayCheck c;
        c.tick = r.u32();
        c.hash = r.u64();
        c.posDigest = r.u64();
        if (!r.ok) return false;
        out.checks.push_back(c);
    }
    return r.ok && r.p == r.end;
}

bool peekReplayFile(const std::string& path, ReplayPeek& out) {
    out = {};
#ifdef _WIN32
    FILE* f = _wfopen(tak::u8path(path).c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    // The header is a few hundred bytes (five short strings and the slot table); the
    // tick count follows it.
    uint8_t head[4096];
    const size_t got = std::fread(head, 1, sizeof head, f);
    std::fclose(f);
    if (got < 12 || head[0] != 'T' || head[1] != 'A' || head[2] != 'K' || head[3] != 'R') return false;
    tak::net::Reader r(head + 4, got - 4);
    tak::net::ReplayHeader h;
    const bool headerOk = tak::net::readReplayHeader(r, h, out.formatVersion, out.protocolVersion);
    std::error_code ec;
    const auto size = std::filesystem::file_size(tak::u8path(path), ec);
    out.playable = tak::net::supportedReplayProtocol(out.formatVersion, out.protocolVersion) &&
                   !ec && uint64_t(size) <= tak::net::kMaxReplayBytes;
    if (!out.playable || !headerOk) return true;
    out.mapId = h.mapId;
    const uint32_t ticks = r.u32();
    // An open count marks a recording that was never finished: its length is not known here.
    if (r.ok && ticks != tak::net::kReplayOpenCount) out.ticks = ticks;
    return true;
}

tak::net::ReplayHeader clientReplayHeader(const tak::net::MpClient& mp, uint64_t gameplayHash) {
    const tak::net::RoomView& room = mp.room();
    // Header shared with the server writer and the loader -- see net/matchconfig.h.
    tak::net::ReplayHeader h = tak::net::replayHeaderFor(room.opts, room.mapId, room.mission, mp.startSlots(), mp.startSeed());
    if (mp.modPackage()) h.modDigest = mp.modPackage()->digest;
    if (mp.mapPackage()) h.mapDigest = mp.mapPackage()->digest;
    // The hash of the data this GAME ran on, with its game mods -- not the
    // pure-retail fingerprint the handshake uses. With game mods on the two
    // differ, and it is the effective one a replay has to be checked against.
    h.dataHash = gameplayHash;
    return h;
}
