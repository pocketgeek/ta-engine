// match_codec_test -- the shared match-setup codecs are byte-exact.
//
// GameOptions/SlotInfo used to be written out field by field at seven sites, the
// hashed MatchConfig built in three and the replay header filled in two; fixes kept
// landing in only some of the copies (060e660a, 3f439b67, 2ff96607). The builders in
// net/protocol and net/matchconfig replaced them, and the goldens below were
// captured from the hand-written copies (main @ 6528b01f) BEFORE that refactor, so
// this pins the wire bytes, the replay header, the hashed config and the Crusades
// rules digest to what shipped. A golden that changes is a protocol change: bump
// kNetVersion (or kReplayFormat) deliberately, never re-capture to make it pass.

// GCC 16 sees Writer::str's copy of a short literal as running past the string's
// 8-byte buffer; the copy is bounded by the string's own size.
#pragma GCC diagnostic ignored "-Warray-bounds"

#include "client/replayfile.h"
#include "net/crypto.h"
#include "net/matchconfig.h"
#include "net/protocol.h"
#include "version.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

using namespace tak::net;

static int g_fail = 0, g_checks = 0;
static void check(bool cond, const char* what, const std::string& detail = {}) {
    ++g_checks;
    if (!cond) { ++g_fail; std::printf("  FAIL %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str()); }
}

static std::string hex(const std::vector<uint8_t>& b) {
    std::string s; char t[3];
    for (uint8_t c : b) { std::snprintf(t, 3, "%02x", c); s += t; }
    return s;
}
static void golden(const std::vector<uint8_t>& got, const char* want, const char* what) {
    const std::string h = hex(got);
    check(h == want, what, h == want ? std::string() : "got " + h);
}

// Every field distinct and off its default, so a dropped or swapped field shows.
static GameOptions fxOpts(uint8_t stress) {
    GameOptions o;
    o.crusades = 3; o.forfeitSelfDestruct = 4; o.gameMods = 2; o.speed = 17; o.speedUnlock = 5;   // gameMods: a raw byte here (0/1 in use) so the golden bytes stay
    o.unitCap = 0xBEEF; o.monarchExpendable = 6; o.stressTest = stress; o.fogExplored = 8;
    o.benchmark = 11; o.randomStarts = 9; o.doubleSight = 10; o.pathfindingMode = tak::sim::PathfindingMode::Legion;
    o.allowSpectators = 12; o.mapScenario = 13; o.powerCodes = 14;
    return o;
}
// Humans, an Absurd AI (2x income), a passive AI, open and closed gaps, the last
// used slot at 5. The `empty` table has no used slot at all.
static void fxSlots(SlotInfo (&s)[kMaxSlots], bool empty) {
    for (auto& x : s) x = SlotInfo{};
    if (empty) {
        for (int i = 0; i < kMaxSlots; ++i) {
            s[i].type = i & 1 ? 3 : 0; s[i].faction = uint8_t(i); s[i].team = uint8_t(7 - i);
            s[i].color = uint8_t(i + 2); s[i].aiLevel = 4;
        }
        return;
    }
    s[0] = {1, 7, 3, 1, 1, 9, "alpha"};
    s[1] = {2, 4, 9, 0, 0, 4, "AI bot"};
    s[2] = {0, 2, 5, 6, 1, 0, ""};
    s[3] = {2, 3, 1, 2, 0, 0, "passive"};
    s[4] = {3, 1, 8, 5, 0, 3, ""};
    s[5] = {2, 1, 7, 3, 1, 1, "x"};
    s[6] = {0, 9, 2, 4, 0, 2, ""};
    s[7] = {3, 0, 0, 7, 0, 2, ""};
}

static std::string dumpCfg(const tak::sim::MatchConfig& c) {
    std::string s; char t[256];
    std::snprintf(t, sizeof t, "cap=%d mx=%d ds=%d pm=%d st=%d si=%d bm=%d rs=%d seed=%u crt=%d ms=%d pc=%d pmr=%d rmr=%d rlo=%d mana=%a pr=%d svp=%d n=%zu;",
        c.unitCap, c.monarchExpendable, c.doubleSight, int(c.pathfindingMode), c.stressTest, c.stressImmediate,
        c.benchmark, int(c.startMode), c.startSeed, c.loadCrt, c.mapScenario, c.powerCodes, c.powerMapRevealed,
        c.roomMapRevealed, c.roomLosOff, double(c.startMana), c.patrolRepairs, c.scenarioViewPlayer, c.slots.size());
    s += t;
    for (const auto& m : c.slots) {
        uint32_t bits; std::memcpy(&bits, &m.manaMult, 4);
        std::snprintf(t, sizeof t, "[%d %d %d %08x %d %d]", m.used, m.faction, m.team, bits, m.automaticGates, m.defensiveAi);
        s += t;
    }
    return s;
}

static bool sameOpts(const GameOptions& a, const GameOptions& b) {
    return a.crusades == b.crusades && a.forfeitSelfDestruct == b.forfeitSelfDestruct &&
           a.gameMods == b.gameMods && a.speed == b.speed && a.speedUnlock == b.speedUnlock &&
           a.unitCap == b.unitCap && a.monarchExpendable == b.monarchExpendable && a.stressTest == b.stressTest &&
           a.fogExplored == b.fogExplored && a.benchmark == b.benchmark && a.randomStarts == b.randomStarts &&
           a.doubleSight == b.doubleSight && a.pathfindingMode == b.pathfindingMode &&
           a.allowSpectators == b.allowSpectators && a.mapScenario == b.mapScenario && a.powerCodes == b.powerCodes;
}
static bool sameSlot(const SlotInfo& a, const SlotInfo& b) {
    return a.type == b.type && a.faction == b.faction && a.color == b.color && a.team == b.team &&
           a.ready == b.ready && a.aiLevel == b.aiLevel && a.name == b.name;
}

static void optionsCodec() {
    const GameOptions o = fxOpts(7);
    Writer w; put(w, o);
    // CreateGame / SetGameOptions carry exactly these bytes; LobbyState/GameStarting embed them.
    // After the pathfinder byte come allowSpectators (0c), mapScenario (0d) and powerCodes (0e).
    golden(w.b, "0304021105efbe00000607080b090a040c0d0e", "GameOptions wire bytes");
    GameOptions back; back.gameMods = 0; back.speed = 0; back.fogExplored = 0; back.allowSpectators = 0;
    Reader r(w.b.data(), w.b.size()); get(r, back);
    check(r.ok && r.p == r.end && sameOpts(o, back), "GameOptions round trip");
    // unitCap travels as a u32; the raw get() keeps the low 16 bits (the server clamps).
    { Writer big; put(big, o); big.b[7] = 0x12; Reader rb(big.b.data(), big.b.size()); GameOptions g; get(rb, g);
      check(rb.ok && g.unitCap == 0xBEEF, "GameOptions unitCap high bytes not truncated"); }
    // An unknown pathfinder fails the read but keeps the byte, so the server can say why.
    { Writer bad; put(bad, o); bad.b[15] = 2; Reader rb(bad.b.data(), bad.b.size()); GameOptions g; get(rb, g);
      check(!rb.ok && uint8_t(g.pathfindingMode) == 2, "invalid pathfinding mode accepted or lost"); }
    for (size_t cut = 0; cut < w.b.size(); ++cut) {
        Reader rt(w.b.data(), cut); GameOptions g; get(rt, g);
        if (rt.ok) { check(false, "truncated GameOptions accepted", std::to_string(cut)); break; }
    }
}

static void slotCodec() {
    SlotInfo slots[kMaxSlots]; fxSlots(slots, false);
    // The LobbyState / GameStarting header as Server::writeSlots composes it (RoomState,
    // protocol 323): the 322 bytes with capacity, passworded, roomRev, changeBits and
    // mapSerial between mapsReady and the slots.
    RoomState room; room.id = 0x01020304; room.name = "room name"; room.mapId = "Inner Circle";
    room.opts = fxOpts(7); room.hostId = 0xA0B0C0D0; room.mapsReady = true;
    room.capacity = 6; room.passworded = true; room.roomRev = 0x11223344; room.changeBits = 0x29; room.mapSerial = 0x55667788;
    for (int i = 0; i < kMaxSlots; ++i) room.slots[i] = slots[i];
    Writer w; put(w, room);
    golden(w.b, "040302010900726f6f6d206e616d650c00496e6e657220436972636c6500000304021105efbe00000607080b090a040c0d0ed0c0b0a00106014433221129887766550107030101090500616c7068610204090000040600414920626f740002050601000000020301020000070070617373697665030108050003000002010703010101007800090204000200000300000700020000", "LobbyState slot table bytes");
    {
        Reader r(w.b.data(), w.b.size()); RoomState back; get(r, back);
        bool same = r.ok && r.p == r.end && back.id == room.id && back.name == room.name && back.mapId == room.mapId &&
                    back.mission.empty() && back.hostId == room.hostId && back.mapsReady && back.capacity == 6 &&
                    back.passworded && back.roomRev == room.roomRev && back.changeBits == room.changeBits &&
                    back.mapSerial == room.mapSerial;
        for (int i = 0; i < kMaxSlots; ++i) same = same && sameSlot(back.slots[i], slots[i]);
        check(same, "RoomState round trip");
    }
    for (const auto& s : slots) {
        Writer one; put(one, s); Reader r(one.b.data(), one.b.size()); SlotInfo back; back.aiLevel = 77;
        get(r, back);
        check(r.ok && r.p == r.end && sameSlot(s, back), "SlotInfo round trip", s.name);
    }
}

static ReplayHeader header(const GameOptions& o, const SlotInfo* slots, const std::string& mission) {
    ReplayHeader h = replayHeaderFor(o, "Inner Circle", mission, slots, 0x5EED1234);
    h.modDigest = std::string(64, 'o');
    h.mapDigest = std::string(64, 'm');
    h.dataHash = 0x1122334455667788ull;
    return h;
}

static void replayHeaders() {
    SlotInfo slots[kMaxSlots]; fxSlots(slots, false);
    for (const char* mission : {"", "camp03"}) {
        ReplayHeader h = header(fxOpts(7), slots, mission);
        check(h.engineVersion == tak::kVersion, "replay header engine version");
        h.engineVersion = "9.9.9";   // version-neutral golden: the release and protocol are not pinned here
        Writer w; writeReplayHeader(w, h);
        for (int i = 8; i < 12; ++i) w.b[size_t(i)] = 0;
        // POWER CODES is 03: on (bit 0), map revealed at start (bit 1). The last byte is
        // the room's FOG OF WAR option as given (format 15).
        golden(w.b, *mission ? "54414b520f000000000000000c00496e6e657220436972636c65030402efbe000006073412ed5e090b060063616d7030330500392e392e3988776655443322110a40006d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d40006f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f0408010703010902040900040002050600020301020003010805030201070301000902040203000007020d0308" : "54414b520f000000000000000c00496e6e657220436972636c65030402efbe000006073412ed5e090b00000500392e392e3988776655443322110a40006d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d40006f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f6f0408010703010902040900040002050600020301020003010805030201070301000902040203000007020d0308", *mission ? "mission replay header bytes" : "replay header bytes");
    }
    // Header -> inputs -> config is the live config (every recorded field carried across).
    for (bool empty : {false, true}) {
        fxSlots(slots, empty);
        const GameOptions o = fxOpts(1);
        GameOptions back; SlotInfo backSlots[kMaxSlots];
        matchInputsFrom(header(o, slots, ""), back, backSlots);
        check(dumpCfg(matchConfigFor(back, backSlots, 0x5EED1234)) == dumpCfg(matchConfigFor(o, slots, 0x5EED1234)),
              "config rebuilt from the replay header differs from the live one");
    }
}

static void matchConfigs() {
    SlotInfo full[kMaxSlots], empty[kMaxSlots]; fxSlots(full, false); fxSlots(empty, true);
    struct { uint8_t stress; bool empty; const char* want; } cases[] = {
        {1, false, "cap=48879 mx=1 ds=1 pm=4 st=1 si=1 bm=11 rs=1 seed=1592594996 crt=1 ms=1 pc=1 pmr=1 rmr=1 rlo=0 mana=0x0p+0 pr=1 svp=-1 n=6;[1 2 1 3f800000 0 0][1 4 0 40000000 1 0][0 2 6 3f800000 0 0][1 3 2 3f800000 1 1][0 1 5 3f800000 0 0][1 1 3 3f800000 1 0]"},
        {1, true, "cap=48879 mx=1 ds=1 pm=4 st=1 si=1 bm=11 rs=1 seed=1592594996 crt=1 ms=1 pc=1 pmr=1 rmr=1 rlo=0 mana=0x0p+0 pr=1 svp=-1 n=1;[0 0 7 3f800000 0 0]"},
        {2, false, "cap=48879 mx=1 ds=1 pm=4 st=1 si=0 bm=11 rs=1 seed=1592594996 crt=1 ms=1 pc=1 pmr=1 rmr=1 rlo=0 mana=0x0p+0 pr=1 svp=-1 n=6;[1 2 1 3f800000 0 0][1 4 0 40000000 1 0][0 2 6 3f800000 0 0][1 3 2 3f800000 1 1][0 1 5 3f800000 0 0][1 1 3 3f800000 1 0]"},
        {2, true, "cap=48879 mx=1 ds=1 pm=4 st=1 si=0 bm=11 rs=1 seed=1592594996 crt=1 ms=1 pc=1 pmr=1 rmr=1 rlo=0 mana=0x0p+0 pr=1 svp=-1 n=1;[0 0 7 3f800000 0 0]"},
        {0, false, "cap=48879 mx=1 ds=1 pm=4 st=0 si=0 bm=11 rs=1 seed=1592594996 crt=1 ms=1 pc=1 pmr=1 rmr=1 rlo=0 mana=0x0p+0 pr=1 svp=-1 n=6;[1 2 1 3f800000 0 0][1 4 0 40000000 1 0][0 2 6 3f800000 0 0][1 3 2 3f800000 1 1][0 1 5 3f800000 0 0][1 1 3 3f800000 1 0]"},
        {0, true, "cap=48879 mx=1 ds=1 pm=4 st=0 si=0 bm=11 rs=1 seed=1592594996 crt=1 ms=1 pc=1 pmr=1 rmr=1 rlo=0 mana=0x0p+0 pr=1 svp=-1 n=1;[0 0 7 3f800000 0 0]"},
    };
    for (const auto& c : cases) {
        const std::string got = dumpCfg(matchConfigFor(fxOpts(c.stress), c.empty ? empty : full, 0x5EED1234));
        check(got == c.want, "hashed MatchConfig", got);
    }
}

// The real loader, through a file: header -> MatchConfig, skirmish and mission.
static void replayLoader() {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / ("tak-match-codec-" + tak::crypto::toHex(tak::crypto::randomVec(8)) + ".takrep");
    SlotInfo slots[kMaxSlots]; fxSlots(slots, false);
    struct { const char* mission; uint8_t stress; const char* want; } cases[] = {
        {"", 1, "cap=48879 mx=1 ds=1 pm=4 st=1 si=1 bm=11 rs=1 seed=1592594996 crt=1 ms=1 pc=1 pmr=1 rmr=1 rlo=0 mana=0x0p+0 pr=1 svp=-1 n=6;[1 2 1 3f800000 0 0][1 4 0 40000000 1 0][0 2 6 3f800000 0 0][1 3 2 3f800000 1 1][0 1 5 3f800000 0 0][1 1 3 3f800000 1 0]"},
        {"", 2, "cap=48879 mx=1 ds=1 pm=4 st=1 si=0 bm=11 rs=1 seed=1592594996 crt=1 ms=1 pc=1 pmr=1 rmr=1 rlo=0 mana=0x0p+0 pr=1 svp=-1 n=6;[1 2 1 3f800000 0 0][1 4 0 40000000 1 0][0 2 6 3f800000 0 0][1 3 2 3f800000 1 1][0 1 5 3f800000 0 0][1 1 3 3f800000 1 0]"},
        {"camp03", 1, "cap=48879 mx=1 ds=0 pm=0 st=1 si=1 bm=11 rs=1 seed=1592594996 crt=1 ms=0 pc=0 pmr=0 rmr=0 rlo=0 mana=0x0p+0 pr=1 svp=-1 n=6;[1 2 1 3f800000 0 0][1 4 0 40000000 1 0][0 2 6 3f800000 0 0][1 3 2 3f800000 1 1][0 1 5 3f800000 0 0][1 1 3 3f800000 1 0]"},
        {"camp03", 2, "cap=48879 mx=1 ds=0 pm=0 st=1 si=0 bm=11 rs=1 seed=1592594996 crt=1 ms=0 pc=0 pmr=0 rmr=0 rlo=0 mana=0x0p+0 pr=1 svp=-1 n=6;[1 2 1 3f800000 0 0][1 4 0 40000000 1 0][0 2 6 3f800000 0 0][1 3 2 3f800000 1 1][0 1 5 3f800000 0 0][1 1 3 3f800000 1 0]"},
    };
    for (const auto& c : cases) {
        GameOptions o = fxOpts(c.stress); o.doubleSight = 1;
        Writer w; writeReplayHeader(w, header(o, slots, c.mission));
        Writer b; putBundle(b, 0, {}, {});
        w.u32(1); w.u32(uint32_t(b.b.size())); w.b.insert(w.b.end(), b.b.begin(), b.b.end()); w.u32(0);
        { std::ofstream f(path, std::ios::binary | std::ios::trunc); f.write(reinterpret_cast<const char*>(w.b.data()), std::streamsize(w.b.size())); }
        ReplayFile rf;
        const auto u8 = path.u8string();   // the loader takes UTF-8 (Windows: _wfopen)
        const bool ok = loadReplayFile(std::string(u8.begin(), u8.end()), rf);
        const std::string got = dumpCfg(rf.cfg);
        check(ok && got == c.want, "replay loader MatchConfig", got);
    }
    std::error_code ec; fs::remove(path, ec);
}

static void bundles() {
    std::vector<Command> cmds(3); std::vector<Event> events(2);
    cmds[0].kind = Cmd::Move; cmds[0].player = 1; cmds[0].unitId = 77; cmds[0].targetId = -1; cmds[0].x = 1.5f; cmds[0].z = -2.25f; cmds[0].queue = 1;
    cmds[1].kind = Cmd::ReclaimArea; cmds[1].player = 3; cmds[1].unitId = 5; cmds[1].x = 10; cmds[1].z = 20; cmds[1].x2 = 30; cmds[1].z2 = 40;
    cmds[2].kind = Cmd::BuildManaArea; cmds[2].player = 7; std::strcpy(cmds[2].type, "TARLODE"); cmds[2].x = 3; cmds[2].z = 4; cmds[2].x2 = 5; cmds[2].z2 = 6;
    events[0].kind = Event::Kind::Leave; events[0].player = 2; events[1].kind = Event::Kind::Forfeit; events[1].player = 6;
    Writer w; putBundle(w, 1234, cmds, events);
    golden(w.b, "d20400000300000000014d000000ffffffff0000c03f000010c0010000000000000000000000000000000017030500000000000000000020410000a04100000000000000000000000000000000000000f041000020421a0700000000000000000000404000008040005441524c4f44450000000000000000000000a0400000c0400200000002020106", "TickBundle bytes");
    auto decode = [](const std::vector<uint8_t>& b, Bundle& out) {
        uint32_t tick = 0; Reader r(b.data(), b.size());
        return getBundle(r, tick, out, /*strict=*/true) && tick == 1234;
    };
    auto live = [](const std::vector<uint8_t>& b, Bundle& out) {   // MpClient's TickBundle decode
        uint32_t tick = 0; Reader r(b.data(), b.size());
        return getBundle(r, tick, out, /*strict=*/false) && tick == 1234;
    };
    Bundle bd;
    check(decode(w.b, bd) && bd.cmds.size() == 3 && bd.events.size() == 2, "TickBundle decode");
    if (bd.cmds.size() == 3 && bd.events.size() == 2) {
        Writer again; putBundle(again, 1234, bd.cmds, bd.events);
        check(again.b == w.b, "TickBundle re-encode differs");
    }
    // Replay files are strict.
    auto mutated = [&](auto edit) { std::vector<Command> c = cmds; std::vector<Event> e = events; edit(c, e);
                                    Writer m; putBundle(m, 1234, c, e); return m.b; };
    Bundle x;
    check(decode(mutated([](auto& c, auto&) { c[0].kind = Cmd::BenchClock; }), x), "the last command kind refused");
    check(!decode(mutated([](auto& c, auto&) { c[0].kind = Cmd(uint8_t(Cmd::BenchClock) + 1); }), x = {}), "unknown command kind accepted");
    check(!decode(mutated([](auto& c, auto&) { c[0].player = kMaxSlots; }), x = {}), "command player out of range accepted");
    check(!decode(mutated([](auto& c, auto&) { c[1].x2 = std::numeric_limits<float>::quiet_NaN(); }), x = {}), "non-finite coordinate accepted");
    check(!decode(mutated([](auto&, auto& e) { e[0].kind = Event::Kind(0); }), x = {}), "event kind 0 accepted");
    // Kind 3 is reserved: refused.
    check(!decode(mutated([](auto&, auto& e) { e[0].kind = Event::Kind(3); }), x = {}), "reserved event kind 3 accepted");
    check(!decode(mutated([](auto&, auto& e) { e[1].player = kMaxSlots; }), x = {}), "event player out of range accepted");
    // Event::Kind::AiTakeover is read, and nothing past it is a known kind.
    check(decode(mutated([](auto&, auto& e) { e[0].kind = Event::Kind::AiTakeover; }), x = {}) &&
          x.events.size() == 2 && x.events[0].kind == Event::Kind::AiTakeover, "AI takeover event refused");
    check(!decode(mutated([](auto&, auto& e) { e[0].kind = Event::Kind(uint8_t(Event::Kind::AiTakeover) + 1); }), x = {}),
          "unknown event kind accepted");
    { auto t = w.b; t.push_back(0); check(!decode(t, x = {}), "trailing byte accepted"); }
    for (size_t cut = 0; cut < w.b.size(); ++cut) {
        std::vector<uint8_t> t(w.b.begin(), w.b.begin() + long(cut));
        if (decode(t, x = {})) { check(false, "truncated TickBundle accepted", std::to_string(cut)); break; }
        if (live(t, x = {})) { check(false, "truncated live TickBundle accepted", std::to_string(cut)); break; }
    }
    // The live stream is not: the referee already applied whatever it sent, so a client
    // that dropped the bundle would stall at that tick (main's decoder took it as is).
    {
        auto odd = mutated([](auto& c, auto& e) {
            c[0].kind = Cmd(uint8_t(Cmd::BuildManaArea) + 1); c[1].x2 = std::numeric_limits<float>::quiet_NaN();
            c[2].player = kMaxSlots; e[0].kind = Event::Kind(0); e[1].player = kMaxSlots; });
        odd.push_back(0);
        Bundle l;
        check(live(odd, l) && l.cmds.size() == 3 && l.events.size() == 2 && std::isnan(l.cmds[1].x2) &&
              l.cmds[2].player == kMaxSlots && l.events[1].player == kMaxSlots, "live TickBundle decode dropped an applied bundle");
    }
}

int main() {
    optionsCodec();
    slotCodec();
    replayHeaders();
    matchConfigs();
    replayLoader();
    bundles();
    if (g_fail) { std::printf("FAIL: %d of %d match codec checks\n", g_fail, g_checks); return 1; }
    std::printf("PASS: %d match codec checks\n", g_checks);
    return 0;
}
