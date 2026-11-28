#pragma once

// Wire protocol for the client-server multiplayer (docs/multiplayer-design.md).
// A central taserver relays a server-sequenced deterministic lockstep: clients
// send commands, the server assigns them to ticks and broadcasts one TickBundle
// per tick. This header is shared by the server and the client.
//
// Framing: u32 LE length prefix (payload size + 1) then a u8 message kind then
// the payload. All integers little-endian.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sim/pathmode.h"

#include "net/lockstep.h"   // Command / Cmd (the 35-byte command wire format is reused)

namespace ta::net {

// The protocol number. Client and server must match it exactly (Hello/Welcome), and a
// replay plays only under the number it was recorded with (supportedReplayProtocol).
// Bump it whenever the wire layout changes or the simulation can compute a different
// result from the same inputs -- a peer or replay of another number would desync.
// The CHANGELOG records what each release changed.
constexpr uint32_t kNetVersion = 359;      // 359: extractor upgrades only where the ground takes the better one; 358: extractor upgrades (a build queued behind its reclaim of a weaker extractor); 357: random-map open ground from a mix of flat tiles; 356: factories take Guard (a guard rally for their output); 355: random-map deposits on buildable ground, aqua ore on the sea floor; 354: Targeting Facility radar targeting (TotalA.exe 0x40ad80 fallback to the radar list); 353: random Lava/Metal maps on their real land families, lava seas, per-world sea level; 352: random-map WATER (coast sections by corner pattern), deposits spaced 6 cells, no aqua ore where a world has patches; 351: random maps lay trees, rocks and metal deposits by world (METAL / TREES / ROCKS), portable draws; 350: doubled True sight casts rays to every cell of its disc (no lattice); 349: DOUBLE SIGHT/RADAR passes retail's sight caps (True rays scaled x2, Circular discs past 14 cells); 348: a seen enemy inside mincloakdistance blocks the cloak 90 ticks (TotalA.exe 0x467680); 347: units under construction see (TotalA.exe 0x482ac0 at creation); 346: LINE OF SIGHT option (GameOptions::losType, True LOS rays from los.tdf) and +LOSType, submerged units need sonar; 345: TA Circular sight (anims/vismasks.gaf at the projected centre) and campaign vision (mapping=1 is the shroud on); 344: TA's cheat codes (Cmd::PowerCode admits Meteor, ATM's unitId names the viewed player, LOS takes no argument, DoubleShot/HalfShot independent) and the meteor storm; 343: ten players (kMaxSlots 10: RoomState/replay slot tables, u16 Chat/VoiceUp recipient masks; sim::kNeutralPlayer 10); 342: Cmd::StandingOrder (TA's separate move / fire standing orders); 341: Cmd::Land (a flyer's landing on an air base); 340: Cmd::ReclaimUnit (TA's RECLAIMUNIT; the server admits DGun and ReclaimUnit); 339: Cmd::DGun (TA's D-gun order; plain Attack no longer fires commandfire); 338: random maps only from TA sections (Kingdoms generator and its sanitize sizing gone); 337: GameOptions/replay header drop the Kingdoms Crusades byte; 336: Legion: unfinished ground bodies (conjures, sites) block and are walked round
constexpr uint32_t kMaxFrame = 1u << 18;   // 256 KB frame cap (hardening) -- headroom for a
                                           // big tick bundle when many units are ordered at once
// Players per game: retail's ten (TotalA.exe's ten 0x14b-byte player records at
// game+0x1b63; StartPos1..10). Equals sim::kMaxPlayers (static_assert in matchconfig.cpp).
constexpr int kMaxSlots = 10;
// A set of seats on the wire (Chat and VoiceUp recipients): bit N = slot N.
// kEverySlot is "the whole room" (spectators included, as the Chat rules say).
using SlotMask = uint16_t;
constexpr SlotMask kEverySlot = 0xffff;
constexpr int kServerHz = 30;              // sim/tick rate
constexpr int kHashPeriod = 30;            // ticks between StateHash reports; the
                                           // referee hashes only these ticks too
// The disconnect wait. The server stops the room when it has heard
// nothing from a seated player for kStallMs (a healthy client is never that quiet: it
// reports progress every few ticks and answers a ping within the ping interval), or at
// once when the connection closes without a Leave, and waits for it to come back before
// a server AI takes the seat. kDropWaitMs is each seat's wait for the whole game, spent
// over all of its stalls from each one's last contact (whole seconds per stall, rounded
// up): a stall waits only what is left less the silence before it, and one whose silence
// has used it all is taken over at once. A shorter silence that still holds the game
// (flow control) is charged past a 1 s grace (server.cpp trackHolds). A test server may shorten
// both (TA_STALL_MS, TA_DROP_WAIT_MS); the client reconnects within the same window.
constexpr uint32_t kStallMs = 8000;
constexpr uint32_t kDropWaitMs = 30000;
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

// Explicit selection orders (Cmd::Order). One click of the player's
// over a selection travels as ONE order whatever its size, instead of one command
// per unit through the 64-a-tick drain above: a 10,000-unit Move used to need 157
// ticks of drain at best (308 with the uplink window), so its members started out
// over several seconds and became several crowds.
//
// Uplink: a transaction, in one client's FIFO stream --
//   OrderBegin   u32 serial, template command (Writer::cmd), u8 shape, u32 members
//   OrderMembers u32 serial, u32 count, count member ids (putMembers: varint deltas,
//                continuing from the previous chunk's last id) -- at most
//                kOrderChunkMembers per message, so a frame stays far under kMaxFrame
//   OrderCommit  u32 serial, u32 members, u64 orderDigest(members)
//   OrderAbort   u32 serial
// The server checks the framing (one open transaction per client, serials strictly
// increasing per player per game, counts, digest, the template) and the limits
// below; a committed order joins that client's FIFO behind the commands sent before
// it and is drained whole into one tick, which is its ACTIVATION tick, by a byte
// budget per tick (kTickOrderBytes) shared by the room's clients in rotation. The
// sim checks ownership and existence per member at activation (applyCommand), where
// that state is. Chunks never reach the sim, so no partial order is ever visible.
//
// Limits (an honest client keeps them through orderSendWindow): members per
// order, orders and record bytes outstanding per client (staged at the server,
// queued there, or sent and not yet seen back). The server can still drop an
// order the window allowed -- over the per-second byte budgets, which several
// connections of one account (or one address, without accounts) and the whole
// server share -- and then says so (Msg::InputDropped), which is what releases
// it from the client's window: an order the server drops never comes back in a
// bundle, and without the notice it would hold its place in the window for the
// rest of the game, and with enough of them the outbox, Stop included, for good.
constexpr uint32_t kOrderMaxSerial = 0x7ffffffeu;  // the top bit tags an explicit area key (sim/convoy.h); 0 = none
constexpr uint32_t kOrderMaxMembers = 16384;
constexpr uint32_t kOrderChunkMembers = 2048;
constexpr size_t kOrderMaxBytes = 96 * 1024;       // one order's bundle record (16384 ids of 5 bytes fit)
constexpr size_t kTickOrderBytes = kOrderMaxBytes; // order records in one tick bundle: any order fits an empty tick
// An order's bundle record before its members: kind, player, serial, template, shape,
// count (orderVerb never appends x2/z2). Each member then adds its varint delta.
constexpr size_t kOrderRecordHeader = 2 + 4 + 35 + 1 + 4;
// The window is mostly bytes (the server's memory per client); the order count is
// generous so that many small clicks are never throttled by it: at 8 orders a
// 16-tick round trip carried only half an order a tick, and the scale suite's
// many-10000 (400 squads clicked over 600 ticks) waited up to 196 ticks for it.
constexpr int kOrderWindow = 64;
constexpr size_t kOrderWindowBytes = 4 * kOrderMaxBytes;
// May another order of `bytes` record bytes go out with `inFlight` orders of
// `inFlightBytes` outstanding?
inline bool orderSendWindow(int inFlight, size_t inFlightBytes, size_t bytes) {
    return inFlight < kOrderWindow && inFlightBytes + bytes <= kOrderWindowBytes;
}
// The verbs an order may carry: the per-unit commands the client gives a whole
// selection. Each member gets exactly that command (Offsets: Move only).
bool orderVerb(Cmd c);
// A command coordinate the sim can take: finite and inside signed 16.16 (the
// server's check for PlayerCommands and order templates alike).
inline bool commandCoordinate(float v) { return v == v && v >= -32768.0f && v < 32768.0f; }

// Flow control (WE S3). A client reports the tick it has PROCESSED every
// kProgressPeriod ticks with Msg::Progress, and the server runs at most
// flowLeadTicks() past that report instead of a flat kMaxLeadTicks past a hash
// sent every kHashPeriod. A client-bound game then queues a few dozen ticks
// instead of 120-150 (0.15 s of command latency at 8x instead of 0.5 s).
//   lead = clamp(kProgressPeriod + 2 * rttTicks + kFlowReserve, 16, 120)
// rttTicks is the client's measured round trip (its ping, which also carries one
// rendered frame of polling delay) in ticks at the room's speed. The reserve is
// fixed: an earlier cut let the client add its jitter-buffer depth, but under flow
// control bundles arrive in bursts, the measured jitter grows, and the lead fed
// back into itself (a slow client's backlog crept from 27 to 40 over 2 minutes).
// The RTT comes from the client, so a lying client can only buy back the old fixed
// lead of 120, never more.
constexpr int kProgressPeriod = 4;
constexpr uint32_t kFlowLeadMin = 16, kFlowLeadMax = 120, kFlowReserve = 16;
inline uint32_t flowLeadTicks(int speedTenths, uint32_t rttMs) {
    if (rttMs > 60000) rttMs = 60000;
    const uint64_t sp = uint64_t(speedTenths > 0 ? speedTenths : 10);
    // ticks per second = 30 * speed / 10 = 3 * speedTenths; round the RTT up.
    const uint64_t rttTicks = (uint64_t(rttMs) * 3 * sp + 999) / 1000;
    const uint64_t lead = uint64_t(kProgressPeriod) + 2 * rttTicks + kFlowReserve;
    return uint32_t(lead < kFlowLeadMin ? kFlowLeadMin : lead > kFlowLeadMax ? kFlowLeadMax : lead);
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
    GameList,           // S->C: list of GameInfo (allowSpectators last)
    CreateGame,         // C->S: name ("" = the server's "<name>'s Game"), password, mapId, mission,
                        // options, capacity, spectate, priv. A name or password past the limits
                        // below is refused with JoinResult(0, 0, why).
    JoinGame,           // C->S: gameId, password
    JoinResult,         // S->C: u8 ok, u8 slot (0xFF = watching), str why, u32 roomRev
    LeaveGame,          // C->S
    SlotUpdate,         // C->S: slot edit (own, or any if host): u8 slot, type, faction, color,
                        // team, ready, aiLevel, u32 roomRev (the revision the sender last saw:
                        // a READY sent on an older view is stored as not ready)
    LobbyState,         // S->C: RoomState (full slot table + settings; broadcast). In a running game too, as
                        // the host changes: hostId is the connection holding the host's powers (the
                        // host's seat's, or a successor's); none of the room's (0, or one gone) while
                        // nobody connected holds them.
    Chat,               // C->S: str text, u16 recipients (SlotMask), u8 spectators; S->C: str name, str text
    Kick,               // C->S (host): slot
    StartGame,          // C->S (host)
    GameStarting,       // S->C: final setup + seeds + resume token
    Loaded,             // C->S: map/data loaded, ready to tick
    // in-game
    PlayerCommands,     // C->S: n commands (player byte overwritten by server)
    TickBundle,         // S->C: tick, commands (sorted), events
    StateHash,          // C->S: tick, hash
    Desynced,           // S->C: tick, reason
    PlayerStatus,       // S->C: u8 slot, u8 status, u32 value (informational). status 0 connected,
                        // 1 dropped, 2 loaded, 3 loading (value = its percent, from LoadProgress)
    // durability (M5)
    Rejoin,             // C->S: u32 gameId, u64 resume token, u32 fromTick (the first tick whose
                        // bundle this client does not have). Only a seat still held for the
                        // disconnect wait (or still seated: the old connection is replaced) ->
                        // Reseated, then the bundles from fromTick; refused with JoinResult.
    Pause,              // S->C: cause, player (host-requested pause; ticks stop)
    Resume,             // S->C: cause, player (game resumes)
    Spectate,           // C->S: gameId, password -> GameStarting (slot 0xFF) + log
    SetGameOptions,     // C->S (host): full GameOptions (lobby: rebroadcast; in-game: speed)
    SpeedUpdate,        // S->C: game speed changed in-game (speed byte) -> client re-paces
    MissionOutcome,     // S->C: campaign mission won/lost (int8: +1 victory, -1 defeat)
    SetPause,           // C->S (host/only-human): u8 want -- pause or resume the game.
                        // Distinct from the disconnect wait (Msg::Waiting): it never
                        // expires, because nobody is disconnected; it ends if the host leaves.
    // account login (SCRAM-SHA-256; see src/net/auth.h). Listed here, not beside
    // Hello where they belong logically: these values are written into replay
    // files, so renumbering the existing ones is a wire change.
    AuthRequired,       // S->C: this server wants an account -- sent INSTEAD of Welcome
    AuthBegin,          // C->S: username, client nonce
    AuthChallenge,      // S->C: does the account exist?, salt, iterations, server nonce
    AuthProof,          // C->S: the client's proof  (existing account)
    AuthRegister,       // C->S: StoredKey + ServerKey  (creating a new account)
    AuthResult,         // S->C: status, server signature, message [, sign-in token: a success
                        // on an encrypted connection only (src/net/auth.h)]
    MapOffer,          // both: room, map id, SHA-256, byte count
    MapRequest,        // both: room, SHA-256, xfer; request the offered package (xfer: the
                       // requester's transfer id, net/mappackage.h Receiver)
    MapChunk,          // both: room, xfer, offset, <=64 KiB raw bytes
    MapReady,          // C->S: room, verified SHA-256
    MapError,          // both: room, explanation
    ModOffer, ModRequest, ModChunk, ModReady, ModError,   // as the Map* ones; the label is "mods"
    Progress,           // C->S: u32 tick (last tick PROCESSED), u32 rttMs (the client's smoothed ping),
                        // u32 received (the newest tick whose bundle has ARRIVED, >= tick).
                        // Flow control only (WE S3), every kProgressPeriod processed
                        // ticks: it moves the sender's ack tick and its
                        // lead (flowLeadTicks), is never hash-checked and never recorded. A tick
                        // the server has not closed is ignored, as for StateHash. `received`
                        // tells a slow link (bundles late to arrive) from a slow machine
                        // (bundles waiting to run): voice is paused only for the first.
    // Selection-order transaction (formats beside kOrderMaxMembers).
    OrderBegin,         // C->S
    OrderMembers,       // C->S
    OrderCommit,        // C->S
    OrderAbort,         // C->S
    InputDropped,       // S->C: u32 commands, u32 order serial (0 = none). Input of this
                        // connection the server dropped over a budget or a window (a
                        // PlayerCommands message, or an order transaction at its
                        // Commit): it will never come back in a bundle, so the sender
                        // releases it from its send windows (cmdInFlight, OrderWindow)
                        // instead of waiting for an acknowledgement that cannot come.
    // Voice chat (constants below). Presentation only: the server relays
    // these beside the game and never puts them in a tick bundle, a replay or the
    // referee, and drops them rather than let them delay game traffic.
    VoiceListen,        // C->S: u8 on. Whether this connection wants voice at all (voice chat
                        // switched on). Off by default, so a client that never asks -- the
                        // headless harness -- is sent no voice.
    VoiceUp,            // C->S: u16 seq | u16 recipients | u8 flags | u16 len | len bytes Opus.
                        // recipients (u16 SlotMask since 343) is the talker's diplomacy VOICE mask,
                        // with the same rules as Chat's: kEverySlot (or a room not yet running) is the whole room,
                        // spectators included; otherwise the seated players whose bit is set.
                        // Never echoed to the talker. len <= kVoiceMaxOpus; 0 exactly with END
                        // (which is never combined with START).
    VoiceDown,          // S->C: u32 talker client id | u8 talker slot (0xFF = spectator) | u16 seq |
                        // u8 flags | u16 len | len bytes | str name (only with kVoiceHasName).
    MoveSeat,           // C->S: u8 slot. A seated player (the host too) takes that OPEN seat
                        // (type 0, inside the map capacity), in the lobby only and never in a
                        // campaign mission room; anything else is ignored.
                        // Side, colour, team and READY move with the player; the old seat
                        // opens with the target's colour and team. The server answers the
                        // mover with JoinResult (ok, new slot) and then broadcasts LobbyState.
                        // The host may also give kWatchSeat (leave its seat and watch), and a
                        // watching host may take an OPEN seat again (323).
    // Disconnect wait (docs/multiplayer-design.md section 4).
    Waiting,            // S->C: u8 n, then n x (u8 slot, u32 ms left), then u8 running, u32 ms.
                        // The seated players the room is waiting for, each with its own
                        // countdown to the AI takeover; sent to the whole room whenever the set
                        // changes (n = 0: nobody, the game goes on) and to a spectator or a
                        // reseated player as it arrives. While n > 0 the server closes no tick.
                        // The tail is the recipient's own wait budget: running 1 = its seat is
                        // being waited for and ms is that countdown, else ms is what its seat
                        // has left to spend (the window its client reconnects within).
    Reseated,           // S->C: u8 slot, u64 rotated resume token, u32 fromTick, u32 logEnd. The
                        // answer to a good Rejoin: the client keeps its world, and the bundles
                        // from fromTick follow (those before logEnd are history it catches up on).
                        // Before them come what no bundle carries: LobbyState (the room as it is
                        // now: the game speed, and the host -- this new connection when the seat is
                        // the host's), Pause while the host has the game paused, then Waiting.
    // The sign-in token (src/net/auth.h), on an encrypted connection only. Answered by
    // AuthResult; a failure counts against the login throttle.
    AuthToken,          // C->S: username, token  (sign in with it, instead of AuthBegin)
    AuthRevoke,         // C->S: username, token  (the player quit: it is no longer good)
    // Room editing (323): the host changes its room's map and name in the lobby.
    SetRoomMap,         // C->S (host): u32 serial, str mapId, u8 capacity, u8 mapScenario,
                        // u8 randomStarts. Answered by a LobbyState carrying the serial
                        // (RoomState::mapSerial), after a RoomError when refused.
    SetRoomInfo,        // C->S (host): u8 flags (kRoomInfoName | kRoomInfoPassword), str name,
                        // str password. Only the flagged fields apply.
    RoomError,          // S->C: u8 request (RoomRequest), u32 serial (SetRoomMap's; 0 for
                        // SetRoomInfo), str reason. A refusal the host can act on; never fatal,
                        // always followed by a LobbyState to the same client.
    // The end of a watched game (docs/multiplayer-design.md section 4, "Game end").
    GameOver,           // S->C: u32 ticks, u8 winning team (0xFF = nobody). The server has ended
                        // this running game and closes no more ticks: its bundles 0..ticks-1 are
                        // the whole game (a watcher still catching up is sent the rest of them),
                        // and the team is World::winningTeam() after the last one. Sent to
                        // everyone still in the room -- its watchers -- when the match has its
                        // result with no human seated (one team left, or nobody), or when the
                        // last seated player has gone with a watcher still there. Never recorded
                        // and no sim state: the replay simply ends at that tick.
    // The loading screen's player strip (TotalA.exe 0x497ce0; retail's packet 0x2a).
    LoadProgress,       // C->S: u8 percent (0..100), the mean of the six load parts. Seated
                        // players only, between GameStarting and Loaded; relayed to the room
                        // as PlayerStatus status 3. Presentation only, never recorded. Added
                        // without a version bump: an older server ignores it (no case), an
                        // older client reads only status 2.
};

// Room editing (323). MoveSeat to kWatchSeat: the host leaves its seat and watches.
constexpr uint8_t kWatchSeat = 0xFF;
constexpr size_t kRoomNameMax = 32;           // bytes, printable ASCII (0x20..0x7e)
constexpr size_t kRoomPasswordMax = 128;      // bytes, printable ASCII (as the CONNECT box allows)
constexpr uint8_t kRoomInfoName = 1, kRoomInfoPassword = 2;   // SetRoomInfo flags
enum class RoomRequest : uint8_t { Map = 1, Info = 2 };       // RoomError
// RoomState::changeBits: what the last roomRev bump changed.
enum RoomChange : uint8_t { kChangeMap = 1, kChangeScenario = 2, kChangeOptions = 4,
                            kChangePlayers = 8, kChangeMods = 16, kChangeCapacity = 32 };
// Printable ASCII only (0x20..0x7e), as room names and passwords must be.
inline bool printableAscii(const std::string& s) {
    for (unsigned char ch : s) if (ch < 0x20 || ch > 0x7e) return false;
    return true;
}

// Voice chat (Msg::VoiceUp / VoiceDown): Opus, 48 kHz mono, one 20 ms frame per message.
constexpr size_t kVoiceMaxOpus = 400;      // bytes of one frame's packet (24 kbit/s is ~60)
constexpr uint8_t kVoiceStart = 1;         // first frame of a transmission (resets the receiver)
constexpr uint8_t kVoiceEnd = 2;           // the talker released push-to-talk; carries no audio
constexpr uint8_t kVoiceHasName = 4;       // VoiceDown: the talker's display name follows
constexpr uint8_t kVoiceSpectators = 8;    // VoiceUp (316): spectators hear it too (the Diplomacy SPECTATORS row);
                                           // stripped before VoiceDown

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
    uint8_t aiLevel = 2;   // AI difficulty: 0=passive (Defensive) 1=easy 2=normal 3=hard 4=absurd
                           // 5=absurdly defensive 6=classic (ai::Difficulty)
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
    uint8_t allowSpectators = 1;   // 0 = the room refuses Spectate (GameOptions::allowSpectators)
};

// Live game speed, in tenths (10 = 1x). Retail's -10..+10 setting is linear:
// level d runs (d+10)/10 x normal, 0.1x steps up to 2x (multiplayer floors at
// 0.1x). The UI shows d = speed - 10. kMaxDevGameSpeed is only for the headless
// timing harnesses (TA_SPEED) on a test/loopback server. kMaxBenchmarkGameSpeed is the
// menu Benchmark's +100 (11x), allowed only in a benchmark room (private, local only).
inline constexpr int kMinGameSpeed = 1, kMaxGameSpeed = 20, kMaxDevGameSpeed = 80, kMaxBenchmarkGameSpeed = 110;

// Per-game settings chosen by the host.
constexpr uint8_t kStressImmediate = 1, kStressStaged = 2;
// Start Locations, GameOptions::randomStarts (see there). normalizeStartMode reads
// any nonzero value other than these three as RANDOM.
constexpr uint8_t kStartsFixed = 0, kStartsRandom = 1, kStartsBySlot = 2;
constexpr uint8_t normalizeStartMode(uint8_t v) {
    return v == kStartsBySlot ? kStartsBySlot : v ? kStartsRandom : kStartsFixed;
}

struct GameOptions {
    // Total Annihilation start resources, in hundreds (retail's lobby keeps the value
    // / 100 and multiplies on start: docs/retail-economy.md section 9). 10 = 1000.
    uint8_t startMetal100 = 10, startEnergy100 = 10;
    uint8_t forfeitSelfDestruct = 0;   // 0=units go inert on forfeit, 1=self-destruct
    // Game mods for this game: 0=off (no game packs), 1=on (the host's selected
    // mods/game packs are transferred to and mounted by every player). Cosmetic
    // mods are each player's own choice and never travel. The lobby always sends 1
    // (multiplayer works like single player; the host's always-enabled game packs
    // travel in the same package); 0 comes from a debug --mods launch, and the
    // server forces 0 for campaign mission and Benchmark rooms.
    uint8_t gameMods = 0;
    // Game speed in tenths (10 = 1.0x). Changes the server's tick CADENCE (and the
    // client's playout rate); the per-tick dt stays 1/kServerHz so the sim math is
    // identical -- only how fast ticks happen in wall-clock changes. Deterministic.
    uint8_t speed = 10;
    uint8_t speedUnlock = 0;   // 1 = the host may change speed in-game with -/+
    // Per-player unit limit (production/build halts a player at this many live units).
    // One of 250/500/1000/2000; 2000 default. Serialised as u32.
    uint16_t unitCap = 2000;
    // When 0 (default) losing your Monarch loses the game; 1 makes it just a unit.
    uint8_t monarchExpendable = 0;
    // Stress test (SP all-AI spectate): spawn each AI at ~95% of the unit cap in its
    // faction's combat units at the start, to load-test the sim. 0 = off;
    // kStressStaged (2, what TA_STRESS asks for) spreads the fill over the first
    // kStressStageTicks ticks; kStressImmediate (1) spawns it all at setup.
    uint8_t stressTest = 0;
    // Fog of war (client-only display, never hashed -- every client applies the
    // same room rule, so it stays fair): 0 = NOT EXPLORED (terrain starts hidden),
    // 1 = EXPLORED (terrain starts mapped). Both retain seen terrain under fog;
    // 2 = FULL VISION (no fog at all: the whole map and every unit are visible).
    // Host-set in the room.
    // Start Locations (the name is from when it was a RANDOM toggle; the byte keeps its
    // place): kStartsFixed (0) = the SEATED slots take the map's starts in slot order
    // and an empty seat takes none, as retail does -- a human in slot 1 and an AI in
    // slot 10 start at StartPos1 and StartPos2; kStartsRandom (1) = the starts are
    // shuffled for the match; kStartsBySlot (2, not retail) = slot N takes StartPos N
    // whichever seats are empty, and a slot past the map's last start takes the lowest
    // start no seated slot claims (sim::startSpotsFor). Other values read as RANDOM
    // (normalizeStartMode). Deterministic -- derived from the match seed and the slot
    // table, so every peer agrees.
    uint8_t randomStarts = 0;
    uint8_t fogExplored = 1;
    // LINE OF SIGHT (retail's LOSType, game +0x14281 bit 2): 1 TRUE -- los.tdf's rays
    // over the terrain, retail's default -- 0 CIRCULAR, vismasks.gaf. A campaign
    // mission is always TRUE. Sim-affecting (MatchConfig::trueLos, in the replay header).
    uint8_t losType = 1;
    sim::PathfindingMode pathfindingMode = sim::PathfindingMode::Retail;
    uint8_t doubleSight = 0;   // skirmish/MP only; ignored for campaign missions
    // Benchmark INTENSITY: 0=off, 1=Low..6=Extra Absurd -- an all-AI perf run with a spawn ramp
    // (see MatchConfig::benchmark). Deterministic, so it IS part of the hashed sim.
    uint8_t benchmark = 0;
    // ALLOW SPECTATORS (host-set; 1 = on, the default):
    // 0 makes the server refuse every Spectate into this room -- a browser's WATCH, a
    // seated player who left and asks to watch, a dropped spectator reconnecting. The
    // host's own create-as-spectator seat is the host's choice and is exempt; seated
    // players' Rejoin is untouched. Changeable in the lobby, fixed once the game starts
    // (in-game SetGameOptions changes only speed). An admission rule, never part of the
    // sim.
    uint8_t allowSpectators = 1;
    // MAP SCENARIO: 1 = play the map's own scenario (.crt placements and rules, the
    // Use Only .tdf) instead of a skirmish. Chosen when the room is created; in the lobby
    // it changes only with the map (Msg::SetRoomMap: the capacity it implies moves seats
    // with it), never through SetGameOptions. The server clears it for missions,
    // generated maps, stress and benchmark rooms, forces randomStarts off with it, and
    // refuses START when the verified map has no scenario content or a seat the
    // scenario needs is empty. Sim-affecting
    // (MatchConfig::mapScenario, in the replay header). A Test Map copy's [TAPlaytest]
    // opt-in plays its scenario even with this 0.
    uint8_t mapScenario = 0;
    // CHEAT CODES (retail's battle-room "Cheat Codes" box, LOUNGE2.GUI CHEATING; the
    // host's option bit 13, TotalA.exe 0x49730f): 1 lets every player type the ten
    // +codes (sim/powercodes.h) in the chat box. Host-set in the lobby (a skirmish
    // room adopts it from SetGameOptions), fixed once the game starts. The server
    // clears it for campaign missions -- retail never allows cheats in one
    // (0x497462) -- and drops every Cmd::PowerCode while it is 0.
    // Sim-affecting (MatchConfig::powerCodes, in the replay header).
    uint8_t powerCodes = 0;
};

// A sim-affecting server decision, sequenced inside a TickBundle so every peer
// (and the referee) applies it on the same tick and the log replays identically.
struct Event {
    // Leave/Forfeit leave the player's army inert. Kind 3 is reserved and refused on
    // the wire (getBundle).
    // AiTakeover: the player stopped communicating and the disconnect
    // wait ran out, so a server-run Normal AI plays the seat from this tick. The units
    // keep their orders; the sim only marks the player computer-owned (automaticGates).
    enum class Kind : uint8_t { Forfeit = 1, Leave = 2, /* 3 reserved */ AiTakeover = 4 };
    Kind kind = Kind::Forfeit;
    uint8_t player = 0;
};

// ------- little-endian byte buffer writer/reader ---------------------------

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void f32(float v) { uint32_t x; std::memcpy(&x, &v, 4); u32(x); }
    void varint(uint32_t v) { while (v >= 0x80) { u8(uint8_t(v | 0x80)); v >>= 7; } u8(uint8_t(v)); }
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
    // Whole-file payloads (mod package entries): u32 length + payload,
    // never truncated. Not for login fields -- those keep str()/bytes() and their
    // 4 KiB cap. The caller bounds the total (e.g. maps::kMaxBytes).
    void blob(const std::vector<uint8_t>& v) {
        u32(uint32_t(v.size()));
        b.insert(b.end(), v.begin(), v.end());
    }
    void cmd(const Command& c);
};

struct Reader {
    const uint8_t* p; const uint8_t* end;
    bool ok = true;
    Reader(const uint8_t* d, size_t n) : p(d), end(d + n) {}
    bool avail(size_t n) const { return size_t(end - p) >= n; }
    uint8_t u8() { if (!avail(1)) { ok = false; return 0; } return *p++; }
    uint16_t u16() {
        if (!avail(2)) { ok = false; return 0; }
        const uint16_t v = uint16_t(p[0] | (p[1] << 8)); p += 2; return v;
    }
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
    // LEB128, at most 5 bytes; an overlong or out-of-range encoding fails.
    uint32_t varint() {
        uint32_t v = 0;
        for (int i = 0; i < 5; ++i) {
            if (!avail(1)) { ok = false; return 0; }
            const uint8_t b = *p++;
            if (i == 4 && b > 0x0f) { ok = false; return 0; }
            v |= uint32_t(b & 0x7f) << (7 * i);
            if (!(b & 0x80)) return v;
        }
        ok = false; return 0;
    }
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
    // Counterpart to Writer::blob: a u32 length no larger than `maxLen` and no
    // larger than what is actually left, checked before anything is allocated.
    std::vector<uint8_t> blob(size_t maxLen) {
        const uint32_t n = u32();
        if (!ok || n > maxLen || !avail(n)) { ok = false; return {}; }
        std::vector<uint8_t> v(p, p + n); p += n; return v;
    }
    Command cmd();
};

// ------- shared codecs -------------------------------------------------------
//
// GameOptions and SlotInfo have ONE byte layout, used by CreateGame, SetGameOptions
// and LobbyState/GameStarting. Each used to be written out
// field by field at every site, and fixes landed in some copies only. get() is RAW:
// it checks nothing but the pathfinder byte (an unknown mode clears r.ok; the byte is
// still stored so the server can reject with a reason). The server's clamps, and which
// fields a running lobby adopts (SetGameOptions' table), stay explicit at its two readers.
void put(Writer& w, const GameOptions& o);
void get(Reader& r, GameOptions& o);
void put(Writer& w, const SlotInfo& s);   // type faction color team ready aiLevel, name
void get(Reader& r, SlotInfo& s);

// The room header of LobbyState and GameStarting, one codec for the server's writer and
// the client's reader (copies drift). GameStarting appends its own tail after it.
struct RoomState {
    uint32_t id = 0;
    std::string name, mapId;
    std::string mission;           // campaign mission stem (empty = ordinary skirmish/MP)
    GameOptions opts;
    uint32_t hostId = 0;
    bool mapsReady = false;
    uint8_t capacity = kMaxSlots;  // the room's seats (slots >= capacity are CLOSED)
    bool passworded = false;       // the room has a password (never the password itself)
    // A change counter: every host change in the lobby but the name, the password, the
    // speed and the host's own READY moves it on, and unreadies the ready guests. A READY
    // sent on an older revision is stored as not ready (Msg::SlotUpdate).
    uint32_t roomRev = 0;
    uint8_t changeBits = 0;        // RoomChange bits of the change that made roomRev
    uint32_t mapSerial = 0;        // the last SetRoomMap serial the server answered
    SlotInfo slots[kMaxSlots];
};
// id name mapId mission, options, hostId, mapsReady, capacity, passworded, roomRev,
// changeBits, mapSerial, then the kMaxSlots slots.
void put(Writer& w, const RoomState& s);
void get(Reader& r, RoomState& s);

// ------- selection orders ----------------------------------------------------
//
// Member ids travel as varint deltas: the first from `prev` (-1 for an order's
// first member), every one at least 1, so the ids are strictly increasing and a
// duplicate cannot be encoded. putMembers writes u32 count then the deltas;
// getMembers appends `count` ids continuing from `prev` (updated), failing on a
// zero delta, an id past INT32_MAX or a truncated stream.
void putMembers(Writer& w, const int32_t* ids, size_t n, int32_t& prev);
bool getMembers(Reader& r, uint32_t count, int32_t& prev, std::vector<int32_t>& out);
// FNV-1a over the member ids (u32 LE): OrderCommit's integrity check.
uint64_t orderDigest(const std::vector<int32_t>& members);
// The template an order may carry: an order verb, unit id 0, finite in-range
// coordinates, Offsets only on Move.
bool validOrderTemplate(const Command& t, OrderShape shape);
// A whole order is valid to activate: serial 1..kOrderMaxSerial, a valid template,
// 1..kOrderMaxMembers strictly increasing non-negative ids, record within kOrderMaxBytes.
bool validOrder(const SelectionOrder& o);
// Bytes of the order's bundle record (Writer::cmd of its Cmd::Order command).
size_t orderRecordBytes(const SelectionOrder& o);

// The commands + sequenced events the server assigned to one tick.
struct Bundle {
    std::vector<Command> cmds;
    std::vector<Event> events;
};

// One TickBundle payload: u32 tick, u32 count + commands, u32 count + events. The
// referee writes it (roomtick), clients decode it live and replays decode it from
// the file. A truncated bundle always fails. `strict` (replay files) also fails a
// command kind or player out of range, a non-finite coordinate, an unknown event or a
// trailing byte, and validates every order whole. The live stream is NOT strict: the
// referee has already applied that bundle, so a client must too -- dropping it would
// stall the client at that tick.
void putBundle(Writer& w, uint32_t tick, const std::vector<Command>& cmds, const std::vector<Event>& events);
bool getBundle(Reader& r, uint32_t& tick, Bundle& out, bool strict);

}  // namespace ta::net
