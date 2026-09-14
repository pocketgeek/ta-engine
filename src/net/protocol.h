#pragma once

// Wire protocol for the client-server multiplayer (docs/multiplayer-design.md).
// A central takserver relays a server-sequenced deterministic lockstep: clients
// send commands, the server assigns them to ticks and broadcasts one TickBundle
// per tick. This header is shared by the server and the client.
//
// Framing (kept from the old 2-peer lockstep): u32 LE length prefix (payload
// size + 1) then a u8 message kind then the payload. All integers little-endian.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "net/lockstep.h"   // Command / Cmd (the 35-byte command wire format is reused)

namespace tak::net {

constexpr uint32_t kNetVersion = 80;       // 80: the SEARCH refuses to route through a
                                           //     parked body (it is solid to the mover)
                                           // 79: route shortcuts respect parked bodies;
                                           //     spawn claims use the centred footprint
                                           // 78: benchmark/stress fills reserve whole
                                           //     footprints and stop at map capacity
                                           // 77: traced routes are string-pulled from
                                           //     the unit's CURRENT cell before install
                                           // 76: factory output spawns/rallies to FREE
                                           //     spots; buildings hold rally orders
                                           // 75: the SIM summons gods (the client did,
                                           //     the referee never did -> desync)
                                           // 74: per-category DAMAGE is a multiplier
                                           //     on default, not absolute damage
                                           // 73: benchmark/stress spawns snap to terrain
                                           //     the unit can actually occupy
                                           // 72: replacing a unit's orders cancels its
                                           //     pending search; the unload approach
                                           //     must be REACHABLE, not just fit
                                           // 71: an unload approaches the nearest cell
                                           //     the transport FITS, within unload range
                                           // 70: a transport unload routes through the
                                           //     path service instead of an inline A*
                                           // 69: that re-request also adopts the newer
                                           //     exact destination (same cell, new point)
                                           // 68: a path re-request for the same goal from
                                           //     the same cell no longer restarts the search
                                           // 67: GameStarting carries the replay boundary
                                           // 66: cloak defaults OFF (retail CLOAK/DECLOAK are orders)
                                           // 65: reclaim and repair join the one queue;
                                           // reclaimQueue gone, repairs can stack
                                           // 64: ONE order queue -- builds are ordinary
                                           // orders; Unit::buildOrders is gone
                                           // 63: a build queued behind movement rides
                                           // the move queue and blocks it while it builds
                                           // 62: unitstandorders gates the stance
                                           // 61: idle flyers look for a landable spot
                                           // 60: standing orders split into retail's
                                           // move + fire axes, seeded per unit type
                                           // 59: fireatwillrandom scatters auto-acquired
                                           // targets for the 16 missile-troop types
                                           // 58: order queues survive a repath; Patrol,
                                           // Repair and Assist honour their queue flag
                                           // 57: account login (SCRAM-SHA-256) sits
                                           // between the Hello and the Welcome
                                           // 56: canPlace honours the yardmap -- a '.' cell
                                           // is not part of the footprint and is not tested
                                           // 55: the server-side AI no longer shares the sim's
                                           // flow-field cache (it was desyncing the referee);
                                           // flow key is per movement CLASS again
                                           // 54: one nav grid per MOVEMENT CLASS, and one
                                           // shared obstacle overlay (buildings used to block
                                           // the ground grid only -- hover walked through them)
                                           // 53: mobile units get their REAL footprint from
                                           // the movement class (2x2..5x5, and a 7x7 Trebuchet)
                                           // 52: retail terrain passability -- a cell is
                                           // blocked on its OWN quad's height spread, not on
                                           // whether a neighbour rises above it (which severed
                                           // every ramp)
                                           // 51: units are solid -- a parked body blocks a
                                           // move; blocked units clamp-and-slow instead of
                                           // stopping; separation spacing 13 -> 16px
                                           // 50: missed ballistic shells crater where they
                                           // land; target leading + dontleadtargets;
                                           // lobbers ignore the line-of-sight gate;
                                           // all four walls block nav (isStructure)
                                           // 49: cosmetic-lows sim batch -- turninplacerate +
                                           // retail turn/brake coupling, transportdistance,
                                           // [AdjustJoy] repair aura + retail aura falloff,
                                           // guided sub-step speed-up
                                           // 48: Random Start Locations (seeded start-spot shuffle)
                                           // 47: Standby_wander wildlife roam (hashed movement)
                                           // 46: mission VO hook + deadly water (waterdoesdamage)
                                           // 45: Msg::SetPause (player-requested pause) + mission order verbs
                                           // 44: mission GET table + escort/protect win-lose conditions
                                           // 43: storm ids folded into the hash (viewer draws storms)
                                           // 42: weapon classes -- guided homing, Remote Effect,
                                           // wandering storms, mind control, unitsonly
                                           // 41: [EXPLODEAS] death blasts, totalallowed unique cap,
                                           // data-only missions (.cob optional) + InitialMission/ManaPercentage
                                           // 40: fire breath is a Line-of-Sight beam (instant hit, FBI range, no 170 cap)
                                           // 39: a builder mid-job doesn't auto-acquire/wander off
                                           // 38: self-destruct is a 5s toggled countdown (Ctrl+Shift+D)
                                           // 37: don't auto-acquire/keep attacking undamageable targets
                                           // 36: in-sim .crt scenario trigger runner + forced-defeat
                                           // 35: structures exempt from separation/unstick
                                           // 28: retail melee = footprint adjacency
                                           // 26: benchmark level 6 (Extra Absurd)
                                           // 25: GameOptions.benchmark carries an INTENSITY
                                           // level (0=off, 1=Low..N), was a bool
                                           // 24: GameOptions.benchmark (all-AI perf run)
                                           // 20: +control squads/formations (Cmd::SetSquad)
                                           // 15: GameOptions.speed/speedUnlock + Set/SpeedUpdate msgs
                                           // 14: Cmd::Headbang (Shift+H emote)
                                           // 13: under-construction units contribute no income/storage (economy fix)
                                           // 12: builder repair (Cmd::Repair + Unit::repairId)
                                           // 11: unit stance/cloak/active in sim + Cmd::Stance/Cloak/SetActive
                                           // 10: reclaimable features in sim + Cmd::Reclaim (area-clear)
constexpr uint32_t kMaxFrame = 1u << 18;   // 256 KB frame cap (hardening) -- headroom for a
                                           // big tick bundle when many units are ordered at once
constexpr int kMaxSlots = 8;               // players per game (= max map start positions)
constexpr int kServerHz = 30;              // sim/tick rate
constexpr int kHashPeriod = 30;            // ticks between StateHash reports; the
                                           // referee hashes only these ticks too
// Commands one client may land in one tick. The server enforces it as flood
// protection and DISCARDS the excess, so the client has to know it too and spread
// a bigger batch over several ticks -- otherwise ordering a large selection
// silently loses everything past the cap. Shared here so the two cannot drift.
constexpr int kCmdCapPerTick = 64;
// How many commands the server will hold for one client that has run ahead of the
// drain. The client's send credit is capped at exactly this, which is what makes
// overflow unreachable for an honest client: it can never have more outstanding
// than the server can hold, whatever its frame rate or how its uplink batches.
constexpr int kCmdQueueCap = 8 * kCmdCapPerTick;

// Commands a client may send after `ticks` sim ticks have passed, carrying `have`
// unspent credit. Shared so the two sides cannot drift: the server drains
// kCmdCapPerTick per tick, so credit accrues at exactly that rate.
//
// Crediting by ELAPSED TICKS rather than per call is the point. Gating on "once
// per tick" still let the frame rate set the throughput -- one batch per rendered
// frame, so at 10fps a client offered 640 commands/sec against a server willing to
// take 1920, and a 2000-unit order crawled out over three seconds while a Stop
// issued behind it waited its turn.
//
// This is a RATE limit and nothing more. It does NOT bound how much is in flight:
// over a 16-tick uplink stall a client spends and re-accrues, sending 1024 while
// the server's 512 queue sees none of it, and half is dropped when the stall
// clears. Outstanding is bounded separately, by cmdSendWindow below.
inline int cmdSendCredit(int have, uint32_t ticks) {
    const long long c = (long long)have + (long long)ticks * kCmdCapPerTick;
    return int(c > kCmdQueueCap ? kCmdQueueCap : c);
}

// How many commands may be sent right now given `inFlight` already sent but not
// yet seen coming back, and `credit` from the rate limiter above. The window is
// the server's queue, so a client physically cannot overrun it however long its
// uplink stalls.
//
// The acknowledgement is free and already on the wire: this is a lockstep relay,
// so the server broadcasts each tick's bundle to EVERY peer including the sender.
// A client's own commands coming back in a bundle are proof the server took them.
// No new message, no version bump.
inline int cmdSendWindow(int credit, int inFlight) {
    const int room = kCmdQueueCap - inFlight;
    const int n = credit < room ? credit : room;
    return n > 0 ? n : 0;
}

// Message kinds. Lobby and game messages share one stream per connection.
enum class Msg : uint8_t {
    // handshake / session
    Hello = 1,          // C->S: version, buildId, dataHash, name
    Welcome,            // S->C: sessionId, your name accepted
    Reject,             // S->C: reason string (version/data mismatch, etc.)
    Ping, Pong,         // keepalive (either direction)
    Bye,                // clean shutdown, reason string
    // lobby
    ListGames,          // C->S
    GameList,           // S->C: list of GameInfo
    CreateGame,         // C->S: name, password, mapId, options
    JoinGame,           // C->S: gameId, password
    JoinResult,         // S->C: ok+slot or error reason
    LeaveGame,          // C->S
    SlotUpdate,         // C->S: slot edit (own, or any if host)
    LobbyState,         // S->C: full slot table + settings (broadcast)
    Chat,               // both: text
    Kick,               // C->S (host): slot
    StartGame,          // C->S (host)
    GameStarting,       // S->C: final setup + seeds + resume token
    Loaded,             // C->S: map/data loaded, ready to tick
    // in-game
    PlayerCommands,     // C->S: n commands (player byte overwritten by server)
    TickBundle,         // S->C: tick, commands (sorted), events
    StateHash,          // C->S: tick, hash
    Desynced,           // S->C: tick, reason
    PlayerStatus,       // S->C: slot, status, ping (informational)
    // durability (M5)
    Rejoin,             // C->S: gameId, resume token -> GameStarting + bundle log
    Pause,              // S->C: cause, player (game paused; ticks stop)
    Resume,             // S->C: cause, player (game resumes)
    Spectate,           // C->S: gameId, password -> GameStarting (slot 0xFF) + log
    SetGameOptions,     // C->S (host): full GameOptions (lobby: rebroadcast; in-game: speed)
    SpeedUpdate,        // S->C: game speed changed in-game (speed byte) -> client re-paces
    MissionOutcome,     // S->C: campaign mission won/lost (int8: +1 victory, -1 defeat)
    SetPause,           // C->S (host/only-human): u8 want -- pause or resume the game.
                        // Distinct from the drop-driven pause above: it never expires,
                        // because nobody is disconnected and nothing should forfeit.
    // account login (SCRAM-SHA-256; see src/net/auth.h). APPENDED, not slotted in
    // beside Hello where they belong logically: these values are written into
    // replay files, so renumbering the existing ones would break every replay
    // ever recorded.
    AuthRequired,       // S->C: this server wants an account -- sent INSTEAD of Welcome
    AuthBegin,          // C->S: username, client nonce
    AuthChallenge,      // S->C: does the account exist?, salt, iterations, server nonce
    AuthProof,          // C->S: the client's proof  (existing account)
    AuthRegister,       // C->S: StoredKey + ServerKey  (creating a new account)
    AuthResult,         // S->C: status, server signature, message
};

// The outcome of a login, as carried by Msg::AuthResult.
enum class AuthStatus : uint8_t {
    Ok = 0,             // signed in to an existing account
    Created = 1,        // no such account, so one was created -- and you are signed in
    BadPassword = 2,    // the account exists and the proof did not check out
    BadUsername = 3,    // the name breaks the naming rules (not "no such account")
    Throttled = 4,      // too many failed attempts; the payload carries the wait in ms
    NameTaken = 5,      // someone registered the name between challenge and register
    ServerError = 6,    // the server could not save the account
};

// A slot in a game's setup. type: 0=open, 1=human, 2=ai, 3=closed.
struct SlotInfo {
    uint8_t type = 0;
    uint8_t faction = 0;   // 0=ara 1=tar 2=ver 3=zon 4=cre
    uint8_t color = 0;     // 0..9 palette slot
    uint8_t team = 0;      // 0..kMaxSlots-1
    uint8_t ready = 0;
    uint8_t aiLevel = 2;   // AI difficulty: 0=passive 1=easy 2=normal 3=hard 4=absurd
    std::string name;      // player display name ("" for open/ai)
};

// A game as seen in the browser.
struct GameInfo {
    uint32_t id = 0;
    std::string name;
    std::string mapId;
    uint8_t players = 0, capacity = 0;
    uint8_t running = 0;      // 0=lobby (joinable), 1=running (view only)
    uint8_t passworded = 0;
    uint32_t uptimeSec = 0;
};

// Per-game settings chosen by the host.
struct GameOptions {
    uint8_t crusades = 0;
    uint8_t gods = 0;
    uint8_t forfeitSelfDestruct = 0;   // 0=units go inert on forfeit, 1=self-destruct
    // Override tier for this game (hpi::OverridePolicy): 0=none (pure retail),
    // 1=cosmetic (art/sound overrides allowed, never affect the sim), 2=full
    // (gameplay overrides allowed but every player must share the same ones).
    // MP defaults to cosmetic. Every player mounts the room's tier at game start.
    uint8_t overridePolicy = 1;
    // Game speed in tenths (10 = 1.0x). Changes the server's tick CADENCE (and the
    // client's playout rate); the per-tick dt stays 1/kServerHz so the sim math is
    // identical -- only how fast ticks happen in wall-clock changes. Deterministic.
    uint8_t speed = 10;
    uint8_t speedUnlock = 0;   // 1 = the host may change speed in-game with -/+
    // Per-player unit limit (production/build halts a player at this many live units).
    // One of 250/500/1000/2000/5000; 2000 default. Serialised as u32.
    uint16_t unitCap = 2000;
    // When 0 (default) losing your Monarch loses the game; 1 makes it just a unit.
    uint8_t monarchExpendable = 0;
    // Stress test (SP all-AI spectate): 1 = spawn each AI at ~95% of the unit cap in
    // its faction's combat units the moment the game starts, to load-test the sim.
    uint8_t stressTest = 0;
    // Fog of war (client-only display, never hashed -- every client applies the
    // same room rule, so it stays fair): 0 = NOT EXPLORED (cells go dark again the
    // moment they leave sight), 1 = EXPLORED (seen terrain stays dimmed-visible),
    // 2 = FULL VISION (no fog at all: the whole map and every unit are visible).
    // Host-set in the room.
    // Random Start Locations: 0 = fixed (slot N always takes the map's Nth start,
    // so players memorise spawns), 1 = the starts are shuffled for the match.
    // Deterministic -- derived from the match seed, so every peer agrees.
    uint8_t randomStarts = 0;
    uint8_t fogExplored = 1;
    // Benchmark INTENSITY: 0=off, 1=Low..6=Extra Absurd -- an all-AI perf run with a spawn ramp
    // (see MatchConfig::benchmark). Deterministic, so it IS part of the hashed sim.
    uint8_t benchmark = 0;
};

// A sim-affecting server decision, sequenced inside a TickBundle so every peer
// (and the referee) applies it on the same tick and the log replays identically.
struct Event {
    enum class Kind : uint8_t { Forfeit = 1, Leave = 2 /* reserved: AllianceChange=3 */ };
    Kind kind = Kind::Forfeit;
    uint8_t player = 0;
};

// ------- little-endian byte buffer writer/reader ---------------------------

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void f32(float v) { uint32_t x; std::memcpy(&x, &v, 4); u32(x); }
    void str(const std::string& s) {              // u16 length + bytes (capped)
        uint32_t n = uint32_t(s.size() > 4096 ? 4096 : s.size());
        u8(uint8_t(n)); u8(uint8_t(n >> 8));
        b.insert(b.end(), s.begin(), s.begin() + n);
    }
    // Raw bytes, u16 length + payload -- nonces, salts and digests in the login
    // handshake. Same framing as str(), but it carries values with embedded NULs.
    void bytes(const void* p, size_t n) {
        uint32_t m = uint32_t(n > 4096 ? 4096 : n);
        u8(uint8_t(m)); u8(uint8_t(m >> 8));
        const uint8_t* q = static_cast<const uint8_t*>(p);
        b.insert(b.end(), q, q + m);
    }
    void bytes(const std::vector<uint8_t>& v) { bytes(v.data(), v.size()); }
    void cmd(const Command& c);
};

struct Reader {
    const uint8_t* p; const uint8_t* end;
    bool ok = true;
    Reader(const uint8_t* d, size_t n) : p(d), end(d + n) {}
    bool avail(size_t n) const { return size_t(end - p) >= n; }
    uint8_t u8() { if (!avail(1)) { ok = false; return 0; } return *p++; }
    uint32_t u32() {
        if (!avail(4)) { ok = false; return 0; }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(*p++) << (8 * i);
        return v;
    }
    uint64_t u64() {
        if (!avail(8)) { ok = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(*p++) << (8 * i);
        return v;
    }
    float f32() { uint32_t x = u32(); float v; std::memcpy(&v, &x, 4); return v; }
    std::string str() {
        if (!avail(2)) { ok = false; return {}; }
        uint32_t n = uint32_t(p[0]) | (uint32_t(p[1]) << 8); p += 2;
        if (n > 4096 || !avail(n)) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), n); p += n; return s;
    }
    // Counterpart to Writer::bytes. `want` (when non-zero) is the exact length
    // the caller expects -- a digest is always 32 bytes, and accepting any other
    // length would mean parsing an attacker-chosen size into a fixed buffer.
    std::vector<uint8_t> bytes(size_t want = 0) {
        if (!avail(2)) { ok = false; return {}; }
        uint32_t n = uint32_t(p[0]) | (uint32_t(p[1]) << 8); p += 2;
        if (n > 4096 || !avail(n) || (want && n != want)) { ok = false; return {}; }
        std::vector<uint8_t> v(p, p + n); p += n; return v;
    }
    Command cmd();
};

}  // namespace tak::net
