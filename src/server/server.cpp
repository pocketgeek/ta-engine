// taserver: the central multiplayer server (docs/multiplayer-design.md, "The server").
//
// A network poll loop hosts the lobby; each running game has its own worker. Each game
// is a server-sequenced deterministic lockstep: clients send commands, the
// server buckets them per tick, closes one tick every 1/30 s, and broadcasts a
// TickBundle to every client in the game. Clients advance their sims in step.
//
// The server runs a REFEREE sim (a headless World built by the same setupMatch
// the clients use, from the install --data names) that also hosts the AI players:
// each tick the AI controllers append their orders to the bundle, and the
// referee's own state hash is the canonical one clients are checked against (with
// a suspicion rule that blames the referee if every client agrees against it).

#include "campaign/campaign.h"
#include "sim/sides.h"
#include "net/mappackage.h"
#include "util/u8path.h"
#include "net/modpackage.h"
#include "server/commands.h"
#include "server/orders.h"
#include "server/acme.h"
#include "server/roomworker.h"
#include "server/tickpacer.h"
#include "server/validationworker.h"
#include "util/stoptoken.h"
#include "util/devenv.h"
#include "server/roomtick.h"
#include "net/roomseats.h"
#include "server/status.h"
#include "net/matchconfig.h"
#include "net/discovery.h"
#include "tnt/mapgen.h"
#include "tnt/ota.h"
#include "net/netcompat.h"
#include "util/winargv.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <tuple>
#include <type_traits>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ai/ai.h"
#include "ai/builtinprofile.h"
#include "ai/classicprofile.h"
#include "hpi/hpi.h"
#include "net/auth.h"
#include "util/storagequota.h"
#include "server/limits.h"
#include "net/crypto.h"
#include "server/accounts.h"
#include "tdf/tdf.h"
#include "net/conn.h"
#include "net/protocol.h"
#include "net/replayhdr.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "sim/simcpus.h"
#include "version.h"
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace ta::net;

namespace {

uint64_t nowMs() {   // monotonic wall-clock (tick pacing / timeouts; never hashed)
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
uint64_t nowUs() {   // the same clock in microseconds, for the tick deadline
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

constexpr uint64_t kPingIdleMs = 5000;    // ping a quiet client after this
uint64_t gTimeoutMs = 15000;              // drop a silent seated player after this
// A seated client that has been sent GameStarting but has not yet answered Loaded is
// loading on its main thread and does not poll its socket (a slow PC or a huge map can
// take well over 15 s), so it gets longer. Window: GameStarting sent -> its Loaded.
uint64_t gLoadTimeoutMs = 30000;
// A SPECTATOR is display-only and non-authoritative: it holds up nobody (canAdvance
// paces to it only in an all-AI game, and even then a stale ack just slows the sim,
// never desyncs it). A busy client render loop -- e.g. a VRAM-starved frame that
// freezes for several seconds at a big supersampled resolution -- stops pumping the
// socket for that whole frame, so a 15s player timeout evicts a spectator that is
// merely hitching. Give spectators a much longer grace so a transient stall doesn't
// tear the connection down (the client's own self-timeout below is widened to match).
constexpr uint64_t kSpectatorTimeoutMs = 120000;
constexpr uint64_t kLoginDeadlineMs = 30000;
constexpr size_t kMaxClients = 256, kMaxPendingLogins = 64, kMaxClientsPerAddress = 64;
constexpr size_t kMaxServerTxBytes = 64u << 20, kMaxClientTxBytes = 4u << 20;

// Service work quotas survive socket churn. Bound identity memory as well as
// work, and retain quiet keys briefly so reconnecting cannot reset the quota.
struct WorkBudget {
    uint64_t window = 0;
    unsigned used = 0;
    bool available(uint64_t now,unsigned limit,unsigned cost=1) const {return now-window>=1000 || (used<=limit && cost<=limit-used);}
    bool take(uint64_t now, unsigned limit,unsigned cost=1) {
        if (now-window >= 1000) {window=now;used=0;}
        if (used>limit || cost>limit-used)return false;
        used+=cost;return true;
    }
};
class SharedWorkBudget {
public:
    explicit SharedWorkBudget(uint64_t retentionMs=60000):retentionMs_(retentionMs) {}
    bool take(const std::string& key,uint64_t now,unsigned limit,unsigned cost=1) {
        if(now-lastSweep_>=1000) {
            std::erase_if(keys_,[this,now](const auto& entry){return now-entry.second.window>=retentionMs_;});
            lastSweep_=now;
        }
        auto found=keys_.find(key);
        if(found==keys_.end()) {
            if(keys_.size()>=4096)return false;
            found=keys_.emplace(key,WorkBudget{}).first;
        }
        return found->second.take(now,limit,cost);
    }
    // Would take() succeed? (Nothing is charged.)
    bool available(const std::string& key,uint64_t now,unsigned limit,unsigned cost=1) const {
        const auto found=keys_.find(key);
        if(found==keys_.end())return keys_.size()<4096 && cost<=limit;
        return found->second.available(now,limit,cost);
    }
private:
    std::map<std::string,WorkBudget> keys_;
    uint64_t lastSweep_ = 0;
    uint64_t retentionMs_;
};
// per-client command cap per tick lives in protocol.h: the CLIENT has to know it
// too, so it can spread a big batch instead of having the excess discarded here.
using ta::net::kCmdCapPerTick;
// The server never runs more than this many ticks ahead of the slowest seated human
// player: a player whose machine can't sustain the game speed gracefully SLOWS the
// whole match to what it can handle (the actual speed drops below the requested one)
// instead of desyncing or being dropped. Generous enough not to throttle normal play
// (hashes are reported every kHashPeriod ticks, plus the client's jitter buffer).
constexpr uint32_t kMaxLeadTicks = 120;
// How long to wait before re-checking a room that is flow-controlled (waiting on
// a lagging player). Rebasing its deadline to `now` instead leaves the poll
// timeout at 0, so the server spins at full CPU while making no progress.
constexpr uint64_t kFlowRetryMs = 5;
// How many commands one client may have waiting. A few ticks' worth:
// enough that an honest burst (a big selection, or two render steps landing in one
// server tick) always survives, small enough that a flooder cannot make the server
// hold an unbounded queue on its behalf.
constexpr size_t kCmdQueueMax = size_t(ta::net::kCmdQueueCap);
// The disconnect wait (net/protocol.h): how long a seated player may be silent before
// the room stops for it, and each human seat's total wait for the whole game (spent
// over all of its stalls) before a server AI takes the seat. A test server shortens
// both (TA_STALL_MS, TA_DROP_WAIT_MS). The budget is spent from the player's last
// contact, so the silence before a wait begins counts, and so does a shorter silence (or
// any other stop) that holds the game: the room lead-limited on the seat's
// acknowledgements (canAdvance) for longer than kHoldGraceMs, which ordinary network
// jitter never is.
uint64_t gStallMs = ta::net::kStallMs;
uint64_t gDropWaitMs = ta::net::kDropWaitMs;
constexpr uint64_t kHoldGraceMs = 1000;
// A game that has ended keeps its room while a watcher is still being sent the bundle
// log (Server::catchingUp), but only while that goes forward: the room is erased once
// no watcher has taken another bundle of it for this long (a silent watcher's own
// timeout), so one that never takes the room's map cannot hold an ended game. A test
// server shortens it (TA_OVER_DRAIN_MS).
uint64_t gOverDrainMs = kSpectatorTimeoutMs;

// A resume token (unguessable per session; not sim state, so no determinism or
// cross-platform concern). std::random_device is entropy where available; mix in
// the clock so a deterministic random_device (some libstdc++ builds) still varies.
uint64_t randToken() {
    std::random_device rd;
    std::mt19937_64 g((uint64_t(rd()) << 32) ^ rd() ^ nowMs());
    uint64_t v = g();
    return v ? v : 1;
}

struct Client {
    Conn conn;
    uint32_t id = 0;
    std::string name;
    // Signing in: the exchange sits between the Hello and the Welcome, so a
    // connection on a server that requires an account passes through Auth before
    // it can do anything at all. See src/net/auth.h.
    struct PendingAuth {
        std::string user;                     // as typed, for display and transcript
        std::vector<uint8_t> clientNonce, serverNonce, salt;
        uint32_t iters = 0;
        bool newAccount = false;              // no such account: this is a registration
        bool challenged = false;              // guards against a second AuthBegin
    } pendAuth;
    std::string account;                      // set once signed in ("" = open server)
    std::string peer = "?";                   // numeric address, for rate limiting
    enum State { Handshake, Auth, Lobby, InGame } state = Handshake;
    uint32_t roomId = 0;
    int slot = -1;
    uint64_t lastRecvMs = 0;
    uint64_t connectedMs = 0;
    uint64_t lastPingMs = 0;
    bool loaded = false;
    uint32_t mapReadyRoom = 0, mapOfferedRoom = 0;
    ta::net::maps::Receiver modReceive;
    ta::net::maps::Sender modSend;
    std::string modReady,modOffered;
    uint64_t modStarted=0,modProgress=0;
    ta::net::maps::Receiver mapReceive;
    ta::net::maps::Sender mapSend;

    uint32_t ackTick = 0;   // latest tick this client reported processing (StateHash or Progress)
    uint32_t arrivedTick = 0;   // latest tick it reported having received (Progress; >= ackTick)
    // How far the room may run past ackTick for this client: kMaxLeadTicks until it sends
    // Msg::Progress, then flowLeadTicks() from that report's RTT (WE S3).
    uint32_t leadTicks = kMaxLeadTicks;
    // kCmdCapPerTick is a per-TICK budget, so it has to be tracked per client
    // across messages. Clamping each message independently caps nothing: the
    // server drains every readable message before closing the tick and appends
    // each one's commands to the same pending list, so N messages buy N*cap
    // commands -- more sim work for everyone, and a bundle that can grow past
    // the 256 KiB frame limit every receiver enforces, disconnecting them.
    // EVERY command from this client lands here on arrival, and the per-tick drain
    // in closeTick is the ONLY thing that feeds a room's pending list. One queue,
    // one budget, one scheduling rule.
    //
    // The first cut had two entry points -- accept up to the budget straight into
    // r.pending, defer the rest here -- and that was wrong three ways at once: a
    // command arriving next tick jumped ahead of older deferred ones (a stale Move
    // overriding a newer Stop), reception and the drain each granted a fresh
    // 64-command allowance so one client could put 128 in a tick, and the drain
    // ignored TA_SRV_DELAY so deferred commands outran delayed ones.
    std::deque<Command> cmdQueue;
    uint64_t cmdDropped = 0;     // past the queue cap; logged, not silent
    // Selection-order transactions (server/orders.h): the open one
    // and the committed orders this client has queued. A committed order joins
    // cmdQueue as ONE Cmd::Order item, so it keeps its place among this client's
    // commands.
    ta::srv::OrderInbox orders;
    uint64_t ordersLogAt = 0;
    void clearCommands() { cmdQueue.clear(); orders.clear(); }
    // Catch-up streaming. A resuming or spectating client needs every bundle
    // logged so far, which for a long game is far more than belongs in one
    // socket buffer -- pushing it all at once put the entire replay in memory
    // and handed one client an unbounded write queue. Instead we hold a cursor
    // and feed it as the buffer drains. Every live bundle is appended to r.log
    // too, so the cursor catches up to the present by itself; until it does,
    // this client is skipped by the live TickBundle broadcast so the stream
    // cannot go out of order.
    WorkBudget lobbyWork,uploadWork,commandBytes,commandCount;
    uint64_t commandLogAt=0;
    // The package validation this client's offer is waiting on, one per kind: a map change
    // resets the map's alone, so a game-mod validation running beside it survives.
    uint64_t mapValidationId=0, modValidationId=0;
    uint32_t xferSeq=0;   // this connection's transfer ids (MapRequest/ModRequest the server sends)
    uint64_t modRefusedAt=0;   // last refused game-mod upload answered (Server::modMsg)
    std::optional<Frame> deferredMapOffer;
    uint64_t mapReceiveStarted=0,mapReceiveProgress=0;
    bool replaying = false;
    size_t replayPos = 0;        // next index into Room::log to send
    // Voice chat: relayed beside the game, never into a bundle, the log or
    // the referee. voiceListen is the client's own VoiceListen (off until it says so).
    // Its frames are budgeted per second: past kVoiceFramesPerSec / kVoiceBytesPerSec
    // they are dropped (a frame stall followed by catch-up is normal), past
    // kVoiceAbusePerSec the connection is failed. voiceOut counts frames relayed, to
    // repeat the talker's name every kVoiceNameEvery of them.
    // voiceOpen: an audio frame of this talker's current transmission has been relayed
    // and its END has not, so the next END is owed to the listeners.
    bool voiceListen = false, voiceOpen = false;
    WorkBudget voiceFrames, voiceBytes, voiceAbuse;
    uint64_t voiceDropped = 0, voiceLogAt = 0;
    uint32_t voiceOut = 0;
    // As a LISTENER in a running game: voice to this client stops while its acknowledged
    // tick lags the room (Server::voiceBehind). baseRttMs is the lowest round trip its
    // Progress reports have shown (allowed to creep up 1 ms a report, so a route that got
    // slower is followed), i.e. its round trip with no voice queued ahead of the ticks.
    uint32_t baseRttMs = UINT32_MAX;
    bool voiceMuted = false;
    unsigned voiceStrikes = 0;
    uint64_t voiceMutedUntil = 0, voiceTripAt = 0;
};

// Voice relay budgets (per talking connection; voice is 50 frames a second of ~60 bytes).
constexpr unsigned kVoiceFramesPerSec = 60;
constexpr unsigned kVoiceBytesPerSec = 16000;
constexpr unsigned kVoiceAbusePerSec = 300;
constexpr unsigned kVoiceGlobalFramesPerSec = 4000;   // all talkers on this server
constexpr uint32_t kVoiceNameEvery = 25;

using ta::srv::TickResult;
static_assert(ta::srv::TickPacer::kHz == kServerHz, "TickPacer paces kServerHz ticks per second at 1x");

    struct DataSet {
        ta::hpi::Vfs vfs;
        ta::sim::TypeRegistry reg;
        uint64_t hash = 0;
        bool built = false;
    };

struct Room {
    std::unique_ptr<DataSet> modData; // outlives the referee and room VFS
    uint32_t id = 0;
    std::string name, password, mapId;
    std::shared_ptr<ta::net::maps::Package> mapPackage;
    std::unique_ptr<ta::hpi::Vfs> mapVfs;
    // Our levels' build profiles for this game, by wire aiLevel (ai/builtinprofile.h):
    // the built-in tables, each adjusted by its own ai/<x>.txt from the room's game mods,
    // else from its map package. Classic's stays empty (its brain bakes ai/default.txt
    // itself). Set at start; a Controller holds a reference, so they live with the room.
    std::array<ta::ai::Profile, ta::ai::kAiLevels> aiProfiles;
    // The TA AI's setup every computer seat gets at the start, kept for a seat the
    // AI takes over later (takeOver): the map's (or mission's) aiprofile script.
    ta::ai::ClassicProfileScript classicProfile;

    std::string mission;           // campaign mission stem (empty = ordinary skirmish/MP)
    GameOptions opts;
    uint32_t hostId = 0;            // the host's connection (0 = nobody holds the host's powers)
    // In a running game the host's powers (pause, speed) belong to its SEAT, not to the
    // connection: a host that reconnects (Rejoin) into it is the host again under its new
    // connection id, and when the seat leaves the game for good they pass on (passHost).
    // -1: a spectator host, or nobody. Set at the start; the lobby goes by hostId alone.
    int hostSlot = -1;
    SlotInfo slots[kMaxSlots];
    int slotClient[kMaxSlots];      // client id in each slot, -1 = none/ai/open
    std::vector<uint32_t> spectators;   // watching, not seated (no slot, no hash)
    bool running = false;
    bool priv = false;             // private (single-player): hidden from the game list
    int cap = kMaxSlots;            // map capacity (from the host's CreateGame or SetRoomMap)
    // Room editing (323). roomRev counts the host's changes in the lobby (bumpRules); a
    // READY sent on an older one is stored as not ready. changeBits: what the last one
    // changed (RoomChange). mapSerial: the last SetRoomMap answered.
    uint32_t roomRev = 1, mapSerial = 0;
    uint8_t changeBits = 0;
    std::string modDigest;          // the last verified game-mod package's digest ("" = none)
    uint64_t createdMs = 0;
    // Per-game RNG seed, broadcast in GameStarting so every peer derives the same
    // arrangement from it. Rolled per ROOM rather than computed from the room id: a
    // single-player game launches a fresh private server, whose ids restart at 1, so
    // `0x7a6b0000 + id` handed every SP game the identical seed -- and "Random Start
    // Locations" then dealt the identical "random" layout every single time.
    uint32_t seed = 0;
    // running state
    uint32_t tick = 0;                          // next tick to close
    std::vector<Command> pending;               // commands for the next tick
    // Explicit orders: the highest serial each slot has had put into a tick
    // bundle (a reconnecting client continues above it; serials of orders that
    // never activated may be reused), and the slot that drains first next tick
    // (the per-tick order budget goes round the room, so no client starves).
    uint32_t orderSerialFloor[kMaxSlots] = {};
    int orderRotor = 0;
    // A Benchmark room's wall clock (Cmd::BenchClock): when its first tick closed, the
    // spawn stages it has told the sims are due, and whether it has ended the run.
    uint64_t benchStartUs = 0;
    uint32_t benchDueSent = 0;
    bool benchEndSent = false;
    std::map<uint32_t, std::vector<Command>> pendingAt;  // server input-delay: cmds bucketed by future tick
    std::vector<Event> pendingEvents;           // sequenced events for the next tick
    // Tick pacing (server wall clock only; never sim state): see server/tickpacer.h.
    // Rebased by rebasePace() at every start, resume and retry.
    ta::srv::TickPacer pace;
    uint64_t nextTickUs() const { return pace.nextUs(); }
    void rebasePace(uint64_t atUs) { pace.rebase(atUs, tick, int(opts.speed)); }
    // hash cross-check: tick -> (clientId -> hash)
    std::map<uint32_t, std::map<uint32_t, uint64_t>> hashes;
    std::map<uint32_t, bool> desyncFlagged;     // clientId -> already told
    // referee sim (server-side, drives server-hosted AI + a canonical hash)
    std::unique_ptr<ta::sim::World> ref;
    const ta::sim::TypeRegistry* reg = nullptr;   // the balance this game uses
    std::vector<ta::ai::Controller> ai;        // one per AI slot
    std::map<uint32_t, uint64_t> refHash;       // tick -> referee hash (bounded ring)
    // The replay's hash trail, kept SEPARATELY from refHash. refHash is a ring pruned
    // to 300 entries because desync checking only ever looks a few ticks back -- but
    // writing the replay from it meant a long game shipped checkpoints for its last
    // few seconds and nothing else, so a divergence early on could not be located,
    // which is the one thing the trail is for. This is append-only and sampled
    // coarsely: an hour at 30Hz costs a few thousand entries, about 40KB.
    std::vector<ta::net::ReplayCheck> replayChecks;
    bool refSuspect = false;                    // referee itself suspected desynced
    int8_t missionOutcomeSent = 0;              // campaign result already broadcast (0 = none)
    // durability (M5): the full bundle log for spectators, reconnects and the replay,
    // per-slot resume tokens, and the disconnect wait.
    size_t logBytes=0;
    bool resourceLimited=false;
    std::vector<std::vector<uint8_t>> log;      // serialized TickBundle payload per tick
    SlotInfo startSlots[kMaxSlots];             // slot config at game start (for the replay)
    uint64_t slotToken[kMaxSlots] = {};         // resume token per human slot (0 = none)
    // The disconnect wait: when each seat the room is waiting for goes to the AI (0 =
    // not waiting). The seat's connection may still be open (silent past gStallMs) or
    // gone (slotClient -1, held for a reconnect). While any is set, no tick closes.
    uint64_t waitDeadline[kMaxSlots] = {};
    // The wait budget (gDropWaitMs per seat per game): when the running wait began, and
    // how much of the budget the seat's earlier waits spent. Server-only: what the room
    // sees is the countdown (Msg::Waiting) and the takeover tick (Event::AiTakeover).
    // waitBegan is the seat's last contact (the silence before the wait is spent too).
    uint64_t waitBegan[kMaxSlots] = {};
    uint64_t waitSpentMs[kMaxSlots] = {};
    // A hold without a wait (trackHolds): when the room was first found lead-limited on
    // the seat (0 = not holding), how long it has been since (not counting a pause or
    // another seat's wait), and when that was last seen (0 = suspended).
    uint64_t holdBegan[kMaxSlots] = {};
    uint64_t holdMs[kMaxSlots] = {};
    uint64_t holdSeen[kMaxSlots] = {};
    // The token a seat had when an AI took it over, so a late reconnect is told why.
    uint64_t takenToken[kMaxSlots] = {};
    // Where each slot started.
    std::vector<std::pair<float, float>> slotPos;
    bool paused = false;                        // the host paused the game (Msg::SetPause)
    std::shared_ptr<ta::net::mods::Package> modPackage;
    std::future<TickResult> tickJob;
    std::array<bool,kMaxSlots> defeated{}; // last completed tick; safe during a worker tick
    // The match's standing after the last completed tick (TickResult): who has fielded
    // units so far, whether the result is in, and the winning team then (-1: nobody).
    uint16_t fielded = 0;
    bool decided = false;
    int8_t winningTeam = -1;
    // The game has ended here with someone still watching (Server::endGame): no tick
    // closes any more, the watchers have been told (Msg::GameOver), and the room goes
    // as soon as none of them is still being sent the bundle log. overWhy: the reason
    // logged when it does. overSent / overSentMs: how many bundles those watchers had
    // been sent in all when that last grew, and when (Server::catchingUp).
    bool over = false;
    const char* overWhy = "";
    uint64_t overSent = 0, overSentMs = 0;
    std::string simulationError;
    // Last member: join on shutdown before destroying the World/AI it owns.
    // Normal room teardown waits for readiness without blocking the network loop.
    std::unique_ptr<ta::srv::RoomWorker> worker;
    Room() { for (int i = 0; i < kMaxSlots; ++i) slotClient[i] = -1; }
    int capacity() const { return cap; }
    bool waiting() const {
        for (int i = 0; i < kMaxSlots; ++i) if (waitDeadline[i]) return true;
        return false;
    }
    bool held() const { return paused || over || waiting(); }   // no tick closes
    // In play: started and not ended. An ended room (over) is no game any more, only
    // the log its last watchers are still being sent, and a Watch on it is refused: the
    // game list, the LAN beacon and every count of running games (--max-running-games,
    // each game's share of --sim-threads, --status) go by this, not by `running`.
    bool playing() const { return running && !over; }
    int usedSlots() const {
        int n = 0;
        for (int i = 0; i < kMaxSlots; ++i) if (slots[i].type == 1 || slots[i].type == 2) ++n;
        return n;
    }
};

class Server {
public:
    ~Server() {for(auto& job:validations_)job.stop.request_stop();}
    void setReplayDir(const std::string& d) { replayDir_ = d; }
    // Pin every game's RNG seed (--seed). Games are otherwise seeded randomly, which
    // is what makes "Random Start Locations" actually random -- but it also makes a
    // harness run unrepeatable, and the headless --mpai check exists precisely to
    // produce the same state hash twice. Not for a real server.
    void setFixedSeed(uint32_t v) { fixedSeed_ = v; fixedSeedSet_ = true; }
    // --sim-threads: S, the threads the running games' referee sims share, each
    // game's room worker included (0 = auto, every CPU thread the server may use;
    // a count above those means all of them). Each running game gets
    // sim::SimPool::serverShare(S, running games). Scheduling only. A debug build's
    // TA_SIM_WORKERS overrides auto, and only auto: every game then runs that.
    void setSimThreads(int choice) {
        using ta::sim::SimPool;
        simAuto_ = choice <= 0;
        simTotal_ = simAuto_ ? SimPool::usableCpus() : std::min(unsigned(choice), SimPool::usableCpus());
        simDebug_ = simAuto_ ? SimPool::debugWorkers() + 1 : 0;
    }
    // --sim-cpus: the CPUs the games' simulation threads are pinned to, in order
    // (empty, the default: nothing is pinned). Each running game takes the next
    // `share` of them in turn (gameCpus): its worker thread, then its pool's
    // workers; a thread past the end of the list is not pinned, and neither is one
    // whose place holds -1 (a CPU the server may not use). Scheduling only.
    void setSimCpus(std::vector<int> cpus) { simCpus_ = std::move(cpus); }
    void setNoAuth() { requireAuth_ = false; }
#ifndef NDEBUG
    void allowBenchmarks() {testWork_=true;}
#endif
    void setAcme(std::shared_ptr<ta::srv::AcmeCertificates> acme) {acme_=std::move(acme);tlsContext_=acme_->context();}
    void setTls(std::shared_ptr<ta::net::TlsContext> context) {tlsContext_=std::move(context);}
    void setLimits(ta::srv::Limits limits) {limits_=limits;}
    void closeRegistration() { registrationOpen_=false; }
    void setLoopbackOnly() { loopbackOnly_ = true; }
    // A player's own single-player server: loopback only, no sign-in, not a LAN host.
    bool privateSinglePlayer() const { return loopbackOnly_ && !requireAuth_ && !lan_; }
    // A player's LOCAL SERVER (--lan): no accounts, plain TCP on every interface; it
    // announces its open game on the LAN once a second and tells clients in its
    // Welcome, so a joiner goes straight into that game.
    void setLan() { lan_ = true; }
    // Load (or start) the account file. Returns false with `err` set if it exists
    // but cannot be read -- starting anyway would mean running an open server
    // while looking like a closed one.
    bool loadAccounts(const std::string& path, std::string& err) {
        return accounts_.load(path, &err);
    }
    // `dataRoot` is never empty -- main() refuses to start without --data.
    Server(uint16_t port, const std::string& dataRoot,const std::string& mapRoot) : port_(port), dataRoot_(dataRoot),mapRoot_(mapRoot) {
        // The referee reads the retail install directly, plus the ROOM's game
        // mods. Build the pure-retail set eagerly (the connection-level Hello
        // check + every game without game mods); a game-mod set is built lazily
        // per room the first time such a game starts.
        buildDataSet(retail_);
        aiNames_ = loadAiNames(retail_.vfs);
        std::fprintf(stderr, "taserver: loaded game data from %s (referee sim + AI), "
                     "retail gameplay hash %016llx\n",
                     dataRoot_.c_str(),
                     (unsigned long long)retail_.hash);
    }
    int run();

private:
    uint16_t port_;
    std::string dataRoot_,mapRoot_;
    // Accounts. requireAuth_ is the default; --no-auth turns it off for a private
    // or LAN server (single-player launches one of those).
    bool requireAuth_ = true;
    bool registrationOpen_ = true;
    bool testWork_=false;
    // Speeds past retail's 2x exist only for the debug timing harnesses and the
    // debug TA_FAST_SPEED keys, on a debug server that is a test or private
    // loopback one; a release server always holds the retail range.
    int maxGameSpeed() const {
#ifndef NDEBUG
        if (testWork_ || (loopbackOnly_ && !requireAuth_)) return ta::net::kMaxDevGameSpeed;
#endif
        return ta::net::kMaxGameSpeed;
    }
    // A benchmark room may also run at the Benchmark's +100 (benchmark rooms are only
    // accepted from a local, unauthenticated client; see CreateGame).
    int maxGameSpeed(const ta::net::GameOptions& o) const {
        return o.benchmark ? std::max(maxGameSpeed(), ta::net::kMaxBenchmarkGameSpeed) : maxGameSpeed();
    }
    ta::srv::Limits limits_;
    std::shared_ptr<ta::net::TlsContext> tlsContext_;
    std::shared_ptr<ta::srv::AcmeCertificates> acme_;
    bool loopbackOnly_ = false;
    bool lan_ = false;
    // --sim-threads (setSimThreads): S and whether it was auto; simDebug_ is the fixed
    // per-game count of a debug build's TA_SIM_WORKERS (0: none). simShare is what
    // each of `running` games runs on, simShareLogged_ the share last logged (and
    // simRunningLogged_ the games then running: with --sim-cpus, who has which CPUs).
    bool simAuto_ = true;
    unsigned simTotal_ = ta::sim::SimPool::usableCpus(), simDebug_ = 0, simShareLogged_ = 0;
    size_t simRunningLogged_ = 0;
    std::vector<int> simCpus_;   // --sim-cpus (setSimCpus)
    size_t runningGames() const {
        size_t running = 0;
        for (const auto& [id, room] : rooms_) { (void)id; running += room.playing(); }
        return running;
    }
    unsigned simShare(size_t running) const {
        return simDebug_ ? simDebug_ : ta::sim::SimPool::serverShare(simTotal_, running);
    }
    // The `game`-th running game's part of --sim-cpus when each has `share` threads.
    std::vector<int> gameCpus(size_t game, unsigned share) const {
        const size_t from = std::min(game * share, simCpus_.size()), to = std::min(from + share, simCpus_.size());
        return std::vector<int>(simCpus_.begin() + std::ptrdiff_t(from), simCpus_.begin() + std::ptrdiff_t(to));
    }
    void logSimThreads(size_t running) {
        simShareLogged_ = simShare(running);
        simRunningLogged_ = running;
        // ... pinned to cpus 1,3 / 5,7: each running game's CPUs, in the games' order.
        std::string cpus;
        unsigned unpinned = 0;
        for (size_t game = 0; game < running && !simCpus_.empty(); ++game) {
            std::vector<int> own = gameCpus(game, simShareLogged_);
            std::erase(own, -1);   // a CPU the server may not use: its thread is not pinned
            cpus += (game ? " / " : "") + (own.empty() ? std::string("none") : ta::sim::cpuListText(own));
            unpinned += simShareLogged_ - unsigned(own.size());
        }
        const std::string pinned = ta::sim::pinnedText(cpus, unpinned);
        if (simDebug_) std::fprintf(stderr, "sim threads: %u (auto: TA_SIM_WORKERS)%s\n", simShareLogged_, pinned.c_str());
        else std::fprintf(stderr, "sim threads: %u (%s%u over %zu game%s)%s\n", simShareLogged_, simAuto_ ? "auto: " : "",
                          simTotal_, running, running == 1 ? "" : "s", pinned.c_str());
    }
    ta::net::LanBeacon beacon_;
    uint64_t beaconAtMs_ = 0, lanNonce_ = 0;
    std::map<std::string, std::string> lanMapTitles_;   // map id -> its shown title, for the announcement
    std::map<std::string, std::string> lanTranslate_;   // translate/*maps.tdf: lowercase key -> English
    void announceLan(uint64_t now);
    ta::srv::AccountStore accounts_;
    SharedWorkBudget acceptKeys_,lobbyWorkKeys_;
    WorkBudget acceptWork_,commandBytes_,commandCount_,commandLogs_;
    SharedWorkBudget commandByteKeys_,commandCountKeys_;
    WorkBudget lobbyGlobalWork_, uploadGlobalWork_, accountWriteWork_;
    WorkBudget voiceGlobal_;                // kVoiceGlobalFramesPerSec, all rooms together
    uint64_t voiceGlobalDropped_ = 0;
    void relayVoice(Client& c, Room& r, const Frame& f);
    bool voiceBehind(Client& listener, const Room& r, uint64_t now);
    bool allowLobbyWork(Client& c,const Frame& f);
    bool canRead(const Client& c,uint64_t now) const {return c.commandBytes.available(now,4u<<20) && commandBytes_.available(now,32u<<20) && ((!c.mapReceive.size && !c.modReceive.size) || (c.uploadWork.available(now,4096,64) && uploadGlobalWork_.available(now,16384,64)));}
    size_t mapMemory() const;
    struct Validation {
        uint64_t id; uint32_t client,room,size; bool mods,cache;
        std::string digest,map;
        ta::StopSource stop;
        std::future<std::shared_ptr<ta::net::maps::Package>> result;
    };
    std::vector<Validation> validations_;
    uint64_t nextValidation_=1;
    ta::srv::ValidationWorker validationWorker_;
    void validatePackage(Client& c,Room& room,bool mods,bool cache);
    void finishValidations();
    SharedWorkBudget loginWorkKeys_{ta::srv::LoginThrottle::kForgetMs};
    WorkBudget loginGlobalWork_;
    Writer replayBytes(Room& room);
    ta::srv::LoginThrottle throttle_;
    ta::srv::SessionTokens tokens_;
    // A mounted data set (retail, or retail + one room's game mods): the VFS, its unit
    // registry, and the gameplay-data fingerprint peers are held to.
    DataSet retail_;                   // shared immutable retail data; game-mod sets are room-local
    std::vector<std::string> aiNames_;   // retail's gamedata/ainames.tdf pool

    static std::vector<std::string> loadAiNames(const ta::hpi::Vfs& vfs) {
        std::vector<std::string> out;
        try {
            auto b = vfs.read("gamedata/ainames.tdf");
            auto root = ta::tdf::parseText(std::string(b.begin(), b.end()), "ainames.tdf");
            if (const auto* sec = root.child("AI_NAMES"))
                for (const auto& [k, v] : sec->values)
                    if (!v.empty()) out.push_back(v);
        } catch (const std::exception&) {}
        return out;
    }
    // Give an AI slot a random name from the pool, avoiding names already taken by
    // other AI slots in the room. Display-only (broadcast in the slot), so a plain RNG.
    void assignAiName(Room& r, int slot) {
        if (aiNames_.empty()) return;
        std::set<std::string> used;
        for (int i = 0; i < kMaxSlots; ++i)
            if (i != slot && r.slots[i].type == 2 && !r.slots[i].name.empty())
                used.insert(r.slots[i].name);
        std::vector<const std::string*> avail;
        for (const auto& n : aiNames_)
            if (!used.count(n)) avail.push_back(&n);
        if (avail.empty())
            for (const auto& n : aiNames_) avail.push_back(&n);
        static std::mt19937 rng{std::random_device{}()};
        r.slots[slot].name = *avail[rng() % avail.size()];
    }
    void buildDataSet(DataSet& ds) {
        ds.vfs = ta::hpi::mountRetailRoot(ta::u8path(dataRoot_));
        ta::sim::setupRegistry(ds.reg, ds.vfs);
        ds.hash = ta::hpi::gameplayHash(ds.vfs);
        ds.built = true;
    }
    std::string replayDir_;
    uint32_t fixedSeed_ = 0;           // --seed N (N may be 0)
    bool fixedSeedSet_ = false;        // false = roll one per game
    int listenFd_ = -1;
    uint32_t nextClientId_ = 1, nextRoomId_ = 1;
    std::unordered_map<uint32_t, std::unique_ptr<Client>> clients_;
    std::map<uint32_t, Room> rooms_;

    void onFrame(Client& c, const Frame& f);
    void handshake(Client& c, const Frame& f);
    void authMsg(Client& c, const Frame& f);
    // The two keys every login attempt is measured against: the host doing the
    // guessing, and the account being guessed at. See LoginThrottle for why they
    // are policed differently.
    std::string ipKeyFor(const Client& c) const { return "ip:" + c.peer; }
    static std::string userKeyFor(const std::string& user) {
        return "user:" + ta::auth::foldUsername(user);
    }
    // Milliseconds the caller must refuse for, or 0 to proceed.
    uint64_t loginLocked(const Client& c, const std::string& user, uint64_t now) const {
        return std::max(throttle_.lockedFor(ipKeyFor(c), now),
                        throttle_.lockedFor(userKeyFor(user), now));
    }
    void sendThrottled(Client& c, uint64_t wait) {
        sendAuthResult(c, AuthStatus::Throttled, nullptr,
                       "too many failed sign-ins -- try again in " +
                       std::to_string((wait + 999) / 1000) + "s");
        std::fprintf(stderr, "client %u (%s) login throttled (%llums left)\n",
                     c.id, c.peer.c_str(), (unsigned long long)wait);
    }
    void sendWelcome(Client& c);
    void signedIn(Client& c, const ta::srv::Account& a, const ta::crypto::Digest* sig, uint64_t now,
                  const ta::srv::SessionTokens::Chain* rotated = nullptr);
    // `earned`: a successful sign-in on that account, which on an encrypted connection
    // carries its new sign-in token: the start of a line, or the next one in `rotated`.
    void sendAuthResult(Client& c, AuthStatus st, const ta::crypto::Digest* sig,
                        const std::string& msg, const ta::srv::Account* earned = nullptr,
                        const ta::srv::SessionTokens::Chain* rotated = nullptr);
    void lobbyMsg(Client& c, const Frame& f);
    void mapMsg(Client& c, const Frame& f);
    void acceptMap(Room& r, std::shared_ptr<ta::net::maps::Package> package);
    bool mapsReady(const Room& r) const;

    void gameMsg(Client& c, const Frame& f);

    void sendReject(Client& c, const std::string& why);
    void sendGameList(Client& c);
    void broadcastLobby(Room& r);
    void broadcastRoom(Room& r, Msg kind, const Writer& w, uint32_t exceptClient = 0);
    Room* roomOf(Client& c) { auto it = rooms_.find(c.roomId); return it == rooms_.end() ? nullptr : &it->second; }
    void dropPendingCommands(Client& c, Room& r);   // both disconnect paths
    // One OrderBegin/OrderMembers/OrderCommit/OrderAbort frame (server/orders.h);
    // false: malformed, the caller fails the connection.
    bool handleOrderFrame(Client& c, Room& r, const Frame& f);
    // Msg::InputDropped: `commands` of a PlayerCommands message, or the order
    // transaction `serial`, were dropped and will never come back in a bundle.
    static void sendInputDropped(Client& c, uint32_t commands, uint32_t serial) {
        Writer w; w.u32(commands); w.u32(serial); c.conn.send(Msg::InputDropped, w);
    }
    // The player-input budgets, all or nothing: `bytes` of frames against the
    // client's, its account's (or address's) and the server's byte budgets, and
    // `count` commands (an order's members count as commands) against the three
    // command budgets. Nothing is charged unless every budget has room.
    bool takeInputBudget(Client& c, uint64_t now, unsigned bytes, unsigned count);
    void leaveRoom(Client& c, const char* reason);
    // The host left a running game for good (`leaving`: its connection, 0 = none): its powers
    // pass to a seated player still in the game (connected, else held for a reconnect), or to
    // nobody, and its pause ends. A LOCAL SERVER's game closes instead. The caller tells the
    // room (broadcastLobby carries hostId).
    void passHost(Room& r, uint32_t leaving);
    void tryStart(Client& c);
    // A START the server will not run: the host is told why (Msg::MapError, which the
    // room shows under its buttons) and the log says so.
    void refuseStart(Client& c, const Room& r, const std::string& why);
    void modMsg(Client& c,const Frame& f);
    DataSet& roomData(Room& r) {return r.modData ? *r.modData : retail_;}
    void closeTick(Room& r, unsigned simThreads, size_t game);
    void finishTick(Room& r);
    bool canAdvance(const Room& r) const;   // false = wait for a lagging player (flow control)
    void checkHashes(Room& r, uint32_t tick);
    void dropClient(uint32_t id, const char* reason);
    // The disconnect wait. beginWait stops the room for a seat it has not
    // heard from since `since` (kept if it is already waiting: the countdown never
    // restarts); endWait lets it go on; takeOver hands the seat to a server Normal AI once
    // the wait runs out. Each tells the room (Msg::Waiting). trackHolds charges the seats
    // that hold the game without a wait (flow control), endHold settles one such hold.
    void beginWait(Room& r, int slot, const char* why, uint64_t since);
    void endWait(Room& r, int slot);
    bool waitedWith(const Room& r, int slot) const;
    void trackHolds(Room& r, uint64_t now);
    void endHold(Room& r, int slot, uint64_t heldMs);
    void takeOver(Room& r, int slot);
    void sendWaiting(Client& c, const Room& r);
    void broadcastWaiting(Room& r);
    // Detach a connection from its seat without the leave path (no Leave event): the
    // seat is being given to a reconnect or an AI. Closes it with `reason`.
    void unseat(Client& c, const char* reason);
    void writeSlots(Writer& w, Room& r, bool fromStart = false);
    void sendGameStarting(Client& c, Room& r, uint8_t slot, uint64_t token, bool catchUp);
    void writeReplay(Room& r);
    // The end of a running game with someone still watching: the room closes no more
    // ticks (Room::over) and everyone in it is told how long the game was and who won
    // (Msg::GameOver). catchingUp: a watcher of the ended room is still being sent its
    // log, and has taken more of it within gOverDrainMs.
    void endGame(Room& r, const char* why);
    bool catchingUp(Room& r, uint64_t now);
    // ---- room editing (protocol 323) ----
    // A host change in the lobby: roomRev moves on with `bits` (RoomChange) and every ready
    // guest is unreadied, in the same step as the change, before its broadcast. Returns
    // how many seats it unreadied.
    int bumpRules(Room& r, uint8_t bits);
    // The room's map package is gone (a new map, the game mods changed): every client's
    // map transfer and the room's map validations with it. Game-mod state is untouched.
    void resetRoomMap(Room& r);
    // The room's seats for a new capacity (roomseats.h planCapacity, which the caller has
    // checked fits): humans past it move down (each one's client id into `moved`, to be told
    // its seat), AIs past it or displaced are removed, the seats past it close and the ones
    // it gains open.
    struct CapacityResult { int aiRemoved = 0, movedHumans = 0; bool changed = false; };
    CapacityResult applyCapacity(Room& r, int newCap, std::vector<uint32_t>& moved);
    // A refusal the host can act on (Msg::RoomError), then the room as it is, to `c` alone.
    void roomRefuse(Client& c, Room& r, RoomRequest req, uint32_t serial, const std::string& why);
    // Every JoinResult: ok, the slot (kWatchSeat = watching) and why, and the room's revision.
    void sendJoin(Client& c, bool ok, int slot, const std::string& why, const Room* r);
    void setRoomMap(Client& c, Room& r, const Frame& f);
    void setRoomInfo(Client& c, Room& r, const Frame& f);
};

// A short form of a room's rules for the log ("sight=0 fog=1 starts=0 ...").
static std::string optionsSummary(const GameOptions& o) {
    char b[192];
    std::snprintf(b, sizeof b, "sight=%d fog=%d los=%d starts=%d cap=%d path=%d codes=%d scenario=%d spectators=%d mods=%d",
                  o.doubleSight, o.fogExplored, o.losType, o.randomStarts, o.unitCap, int(o.pathfindingMode), o.powerCodes,
                  o.mapScenario, o.allowSpectators, o.gameMods);
    return b;
}

// The rules two option sets differ in, by name, the speed left out ("" = the same rules).
static std::string optionsChanged(const GameOptions& a, const GameOptions& b) {
    std::string out;
    const auto field = [&](bool differs, const char* name) {
        if (differs) { if (!out.empty()) out += ' '; out += name; }
    };
    field(a.forfeitSelfDestruct != b.forfeitSelfDestruct, "forfeit");
    field(a.gameMods != b.gameMods, "mods");
    field(a.speedUnlock != b.speedUnlock, "speedunlock");
    field(a.unitCap != b.unitCap, "cap");
    field(a.monarchExpendable != b.monarchExpendable, "monarch");
    field(a.stressTest != b.stressTest, "stress");
    field(a.randomStarts != b.randomStarts, "starts");
    field(a.fogExplored != b.fogExplored, "fog");
    field(a.losType != b.losType, "los");
    field(a.pathfindingMode != b.pathfindingMode, "path");
    field(a.doubleSight != b.doubleSight, "sight");
    field(a.benchmark != b.benchmark, "benchmark");
    field(a.allowSpectators != b.allowSpectators, "spectators");
    field(a.mapScenario != b.mapScenario, "scenario");
    field(a.powerCodes != b.powerCodes, "codes");
    return out;
}

// A room name from a player name: printable ASCII only (a --no-auth Hello can carry
// anything), then "'s Game", cut to kRoomNameMax bytes.
static std::string defaultRoomName(const std::string& player) {
    std::string clean;
    for (unsigned char ch : player) if (ch >= 0x20 && ch <= 0x7e) clean += char(ch);
    if (clean.empty()) clean = "Player";
    std::string name = clean + "'s Game";
    if (name.size() > kRoomNameMax) name.resize(kRoomNameMax);
    return name;
}

// A running game is abandoned once nobody is left to watch it. Normally that means
// no human slot has a live or held (disconnected, inside the wait) client. A game created by
// a SPECTATOR host to watch the AIs fight (single-player spectate / TA_MP_WATCH) has
// NO human slots at all, so it also stays alive while a spectator is connected and at
// least one AI is still playing -- otherwise the server would tear it down the instant
// it started.
static bool roomActive(const Room& r) {
    bool aiPresent = false;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (r.slots[i].type == 1 && (r.slotClient[i] >= 0 || r.waitDeadline[i])) return true;
        if (r.slots[i].type == 2) aiPresent = true;
    }
    return aiPresent && !r.spectators.empty();
}

// True while SOMEBODY is still connected to the room -- a live seated player or a
// spectator. A HELD slot does not count: holding it is what lets a player reconnect,
// but if every other human has gone too then nobody is left watching, and the room
// would otherwise keep its referee sim and AIs running with not a single client
// attached. Games are meant to stop when the last human leaves, so a room that
// empties is torn down immediately rather than ticking on unattended. Note this
// deliberately gives up the reconnect window when the LAST player drops: there is
// nobody for a paused game to be unfair to, and a ghost game costs the server a sim
// + AI per room.
static bool roomOccupied(const Room& r) {
    for (int i = 0; i < kMaxSlots; ++i)
        if (r.slotClient[i] >= 0) return true;
    return !r.spectators.empty();
}

// The match has its result and nobody is seated to play on: the last completed tick
// left one team standing, or nobody (Room::decided), and no slot is a human's -- a
// room of AIs from the start, or one whose humans have all left (their slots are
// closed) or been taken over (their slots are AIs). The server ends such a game at
// that tick: it would otherwise simulate it, and record it, for as long as anyone
// kept watching (an all-AI game once ran 14 hours past its result). A room with a
// human still seated is never ended this way, decided or not: the players' clients
// leave at the result themselves, and the room goes when the last of them has. Nor
// is a campaign mission (its own outcome, Msg::MissionOutcome) or a Benchmark (its
// end is the clock's). A match that can never be decided -- one team only, or
// survivors that cannot reach each other -- runs until its watchers leave.
static bool matchOver(const Room& r) {
    if (!r.decided || !r.mission.empty() || r.opts.benchmark) return false;
    for (int i = 0; i < kMaxSlots; ++i)
        if (r.slots[i].type == 1) return false;
    return true;
}

// Builds the whole replay into a second buffer and writes it synchronously on the
// server loop, so a teardown does stall every other room for the duration. Measured
// before worrying about it: a 3600-tick game is 58 KB and 2.9 ms end to end. Even at
// 20x that it is under 60 ms, once, at room teardown. Left synchronous on purpose --
// an async writer here would be complexity bought against a cost I could not measure.
// Worth revisiting only if replays get much larger (many seated humans issuing orders
// every tick) or the store is slow.
Writer Server::replayBytes(Room& r) {
    // Self-contained replay: header (see net/replayhdr.h -- one definition, filled by
    // replayHeaderFor for both writers), every tick bundle, then the referee's
    // recorded hash checkpoints. A viewer can rebuild the world, play it back, and
    // compare its own hashes against what actually happened. The header records the
    // START slot table, not the forfeited end state.
    ta::net::ReplayHeader h = ta::net::replayHeaderFor(r.opts, r.mapId, r.mission, r.startSlots, r.seed);
    if (r.modPackage) h.modDigest=r.modPackage->digest;
    if (r.mapPackage) h.mapDigest = r.mapPackage->digest;
    h.dataHash = roomData(r).hash;
    Writer w;
    ta::net::writeReplayHeader(w, h);
    w.u32(uint32_t(r.log.size()));
    for (const auto& b : r.log) { w.u32(uint32_t(b.size())); w.b.insert(w.b.end(), b.begin(), b.end()); }
    // The referee's hash trail. Two identical reruns only prove the reruns agree;
    // this is what lets a replay say where it diverged from the real game.
    w.u32(uint32_t(r.replayChecks.size()));
    for (const auto& c : r.replayChecks) { w.u32(c.tick); w.u64(c.hash); w.u64(c.posDigest); }
    return w;
}
void Server::writeReplay(Room& r) {
    if (replayDir_.empty() || r.log.empty()) return;
    try {ta::storageRoom(ta::storageUsage(replayDir_),r.logBytes+65536,limits_.replayDisk);}
    catch(const std::exception& e) {std::fprintf(stderr,"replay not saved: %s\n",e.what());return;}
    const Writer w=replayBytes(r);
    std::string path = replayDir_ + "/game-" + std::to_string(r.id) + "-" +
                       std::to_string(r.createdMs) + ".tarep";
    // Written under a temporary name and renamed, so a server stopped part way never
    // leaves a truncated .tarep; u8path, so a non-ASCII --replaydir works on Windows.
    const auto final = ta::u8path(path);
    auto temp = final; temp += ".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(w.b.data()), std::streamsize(w.b.size()));
    out.close();
    std::error_code ec;
    if (!out) { std::filesystem::remove(temp, ec); std::fprintf(stderr, "game %u: replay not saved: cannot write %s\n", r.id, path.c_str()); return; }
    std::filesystem::rename(temp, final, ec);
    if (ec) { std::filesystem::remove(temp, ec); std::fprintf(stderr, "game %u: replay not saved: cannot rename %s\n", r.id, path.c_str()); return; }
    std::fprintf(stderr, "game %u: wrote replay %s (%zu ticks, %zu bytes)\n",
                 r.id, path.c_str(), r.log.size(), w.b.size());
}

// Msg::Reject ENDS the connection: the client shows DISCONNECTED with the reason, whether or
// not the caller also fail()s the socket. It is for what is fatal by design -- a protocol
// violation, a failed or refused sign-in, a version or data mismatch, an abuse limit, a
// game or room the server closes. A request a player may ordinarily make is refused with
// its own message and the connection kept: sendJoin (CreateGame, JoinGame, Spectate,
// Rejoin), refuseStart (START), roomRefuse (the host's room edits).
void Server::sendReject(Client& c, const std::string& why) {
    Writer w; w.str(why);
    c.conn.send(Msg::Reject, w);
    // Most callers fail()/close the connection right after: push the reason out
    // NOW, or the queued Reject dies with the socket and the client can only
    // report "peer closed" instead of WHY it was turned away.
    c.conn.flushWrite();
    std::fprintf(stderr, "reject client %u: %s\n", c.id, why.c_str());
}

void Server::handshake(Client& c, const Frame& f) {
    if (f.kind != Msg::Hello) { sendReject(c, "expected Hello"); c.conn.fail("no hello"); return; }
    Reader r(f.payload.data(), f.payload.size());
    uint32_t ver = r.u32();
    std::string build = r.str();
    uint64_t dataHash = r.u64();
    std::string name = r.str();
    (void)build; (void)dataHash;   // logged; clients are gated by version here
    if (!r.ok || r.p!=r.end) { sendReject(c, "malformed hello"); c.conn.fail("bad hello"); return; }
    if (ver != kNetVersion) {
        sendReject(c, "protocol version mismatch (server " + std::to_string(kNetVersion) +
                       ", client " + std::to_string(ver) + ") -- update your build");
        c.conn.fail("version");
        return;
    }
    // Gameplay-data agreement: every peer must feed its sim byte-identical gameplay
    // data (verifies the retail files are unmodified, and that every player of a
    // game-mods room shares the host's game mods). The referee's own hash is the authority,
    // never a client's.
    // The Hello hash is the client's PURE-RETAIL gameplay fingerprint (mounted with
    // no mods), so the base game files must match regardless of anyone's mods.
    // (Game mods are checked per game at Loaded.)
    const uint64_t want = retail_.hash;
    if (dataHash != want) {
        char msg[128];
        std::snprintf(msg, sizeof msg,
                      "retail game data mismatch (server %016llx, you %016llx) -- your "
                      "base game files differ", (unsigned long long)want,
                      (unsigned long long)dataHash);
        sendReject(c, msg);
        c.conn.fail("datahash");
        std::fprintf(stderr, "client %u rejected: data hash %016llx != %016llx\n",
                     c.id, (unsigned long long)dataHash, (unsigned long long)want);
        return;
    }
    if (requireAuth_) {
        // The name in the Hello is only a suggestion and is discarded: on a server
        // with accounts, who you are is the account you prove, not what you typed.
        c.state = Client::Auth;
        c.conn.send(Msg::AuthRequired);
        return;
    }
    c.name = name.empty() ? ("player" + std::to_string(c.id)) : name;
    sendWelcome(c);
}

void Server::sendWelcome(Client& c) {
    c.state = Client::Lobby;
    Writer w; w.u32(c.id); w.str(c.name); w.u8(lan_ ? 1 : 0);   // 1 = a LAN host's server
    c.conn.send(Msg::Welcome, w);
    std::fprintf(stderr, "client %u '%s' joined lobby\n", c.id, c.name.c_str());
}

void Server::sendAuthResult(Client& c, AuthStatus st, const ta::crypto::Digest* sig,
                            const std::string& msg, const ta::srv::Account* earned,
                            const ta::srv::SessionTokens::Chain* rotated) {
    Writer w;
    w.u8(uint8_t(st));
    if (sig) w.bytes(sig->data(), sig->size()); else w.bytes(nullptr, 0);
    w.str(msg);
    if (earned && c.conn.encrypted()) {
        try {
            std::vector<uint8_t> token = tokens_.issue(*earned, nowMs(), rotated);
            w.bytes(token);
            ta::crypto::wipe(token.data(), token.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "taserver: %s\n", e.what());   // just no token: the login stands
        }
    }
    c.conn.send(Msg::AuthResult, w);
    ta::crypto::wipe(w.b.data(), w.b.size());
}

// The end of a good sign-in, by password proof or by token.
void Server::signedIn(Client& c, const ta::srv::Account& a, const ta::crypto::Digest* sig,
                      uint64_t now, const ta::srv::SessionTokens::Chain* rotated) {
    c.account = a.name;
    c.name = a.name;
    std::string err;
    if(!accountWriteWork_.take(now,16)) {sendReject(c,"login persistence rate exceeded; retry later");c.conn.fail("login write budget");return;}
    if (!accounts_.noteLogin(a.name, &err))
        std::fprintf(stderr, "taserver: could not record login: %s\n", err.c_str());
    sendAuthResult(c, AuthStatus::Ok, sig, "signed in", &a, rotated);
    std::fprintf(stderr, "client %u (%s) signed in as '%s'\n",
                 c.id, c.peer.c_str(), c.name.c_str());
    sendWelcome(c);
}

// The login exchange. Everything here runs BEFORE the client can reach the lobby,
// so the only frames entertained are the three login ones; anything else drops
// the connection rather than being queued up for later.
void Server::authMsg(Client& c, const Frame& f) {
    Reader r(f.payload.data(), f.payload.size());
    const uint64_t now = nowMs();
    if(!loginWorkKeys_.take(c.peer,now,64) || !loginGlobalWork_.take(now,256)) {
        sendReject(c,"login request rate exceeded; retry later");c.conn.fail("login rate");return;
    }

    if (f.kind == Msg::AuthBegin) {
        if (c.pendAuth.challenged) { sendReject(c, "duplicate login"); c.conn.fail("dup auth"); return; }
        std::string user = r.str();
        std::vector<uint8_t> cnonce = r.bytes(ta::auth::kNonceLen);
        if (!r.ok || r.p!=r.end) { sendReject(c, "malformed login"); c.conn.fail("bad auth"); return; }

        // Rate-limit on BOTH the name and the address, so neither hammering one
        // account from many hosts nor many accounts from one host gets a free run.
        // The address is checked first and always, which is also what stops this
        // endpoint being used to sweep for which usernames exist.
        if (uint64_t wait = loginLocked(c, user, now)) { sendThrottled(c, wait); return; }
        std::string why;
        if (!ta::auth::validUsername(user, &why)) {
            throttle_.fail(ipKeyFor(c), now, ta::srv::LoginThrottle::kAddress);
            sendAuthResult(c, AuthStatus::BadUsername, nullptr, why);
            return;
        }

        c.pendAuth = Client::PendingAuth{};
        c.pendAuth.user = user;
        c.pendAuth.clientNonce = std::move(cnonce);
        try {
            c.pendAuth.serverNonce = ta::crypto::randomVec(ta::auth::kNonceLen);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "taserver: %s\n", e.what());
            sendAuthResult(c, AuthStatus::ServerError, nullptr, "the server cannot sign you in");
            return;
        }
        const ta::srv::Account* a = accounts_.find(user);
        if (a) {
            c.pendAuth.newAccount = false;
            c.pendAuth.salt = a->cred.salt;
            c.pendAuth.iters = a->cred.iters;
        } else {
            // No such account. Say so -- the client offers to create it -- and hand
            // over a fresh server-chosen salt for it to derive against. The salt is
            // ours, not the client's, so a client cannot register with a weak or
            // shared one.
            c.pendAuth.newAccount = true;
            c.pendAuth.iters = ta::auth::kPbkdf2Iters;
            try {
                c.pendAuth.salt = ta::crypto::randomVec(ta::auth::kSaltLen);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "taserver: %s\n", e.what());
                sendAuthResult(c, AuthStatus::ServerError, nullptr, "the server cannot sign you in");
                return;
            }
        }
        c.pendAuth.challenged = true;
        Writer w;
        w.u8(c.pendAuth.newAccount ? 1 : 0);
        w.bytes(c.pendAuth.salt);
        w.u32(c.pendAuth.iters);
        w.bytes(c.pendAuth.serverNonce);
        c.conn.send(Msg::AuthChallenge, w);
        return;
    }

    if (f.kind == Msg::AuthProof) {
        if (!c.pendAuth.challenged || c.pendAuth.newAccount) {
            sendReject(c, "unexpected login proof"); c.conn.fail("bad auth"); return;
        }
        std::vector<uint8_t> proofBytes = r.bytes(ta::crypto::kHashLen);
        if (!r.ok || r.p!=r.end) { sendReject(c, "malformed login proof"); c.conn.fail("bad auth"); return; }
        // Check the lock HERE too, not just at AuthBegin. A guesser that opens a
        // hundred connections first, collects a hundred challenges, and only then
        // starts sending proofs would otherwise never meet the throttle at all --
        // the one gate it passed was armed before any of its guesses were made.
        if (uint64_t wait = loginLocked(c, c.pendAuth.user, now)) {
            sendThrottled(c, wait);
            c.pendAuth.challenged = false;
            return;
        }
        const ta::srv::Account* a = accounts_.find(c.pendAuth.user);
        if (!a) { sendAuthResult(c, AuthStatus::BadPassword, nullptr, "that account no longer exists"); return; }

        ta::crypto::Digest proof{};
        std::memcpy(proof.data(), proofBytes.data(), proof.size());
        std::vector<uint8_t> am = ta::auth::authMessage(c.pendAuth.user, c.pendAuth.clientNonce,
                                                    c.pendAuth.serverNonce, a->cred.salt,
                                                    a->cred.iters);
        const std::string ipKey = ipKeyFor(c), userKey = userKeyFor(c.pendAuth.user);
        if (!ta::auth::verifyClientProof(a->cred, am, proof)) {
            throttle_.fail(ipKey, now, ta::srv::LoginThrottle::kAddress);
            throttle_.fail(userKey, now, ta::srv::LoginThrottle::kAccount);
            sendAuthResult(c, AuthStatus::BadPassword, nullptr, "that password is not right");
            std::fprintf(stderr, "client %u (%s) failed sign-in for '%s'\n",
                         c.id, c.peer.c_str(), a->name.c_str());
            // Do NOT drop the connection: the client may simply have fumbled the
            // password and can try again, which is what the throttle is for.
            c.pendAuth.challenged = false;
            return;
        }
        // Clear the ACCOUNT only. Clearing the address as well would hand anyone
        // with one valid account a reset button for their own failure record,
        // to be pressed between guesses at somebody else's.
        throttle_.succeed(userKey);
        ta::crypto::Digest sig = ta::auth::serverSignature(a->cred.serverKey, am);
        signedIn(c, *a, &sig, now);
        return;
    }

    // A sign-in token (src/net/auth.h): spent here, to sign in or because its player
    // quit. It is a credential, so a bad one is a failed login like a wrong password.
    static_assert(ta::srv::SessionTokens::kRetryMs >= ta::net::kDropWaitMs,
                  "an in-game reconnect must be able to resend a token until it gives up");
    if (f.kind == Msg::AuthToken || f.kind == Msg::AuthRevoke) {
        if (c.pendAuth.challenged) { sendReject(c, "duplicate login"); c.conn.fail("dup auth"); return; }
        std::string user = r.str();
        std::vector<uint8_t> token = r.bytes(ta::auth::kTokenLen);
        const bool wellFormed = r.ok && r.p==r.end;
        if (!wellFormed || !c.conn.encrypted()) {
            ta::crypto::wipe(token.data(), token.size());
            sendReject(c, wellFormed ? "sign-in tokens need an encrypted connection" : "malformed login");
            c.conn.fail("bad auth");
            return;
        }
        std::string why;
        ta::srv::SessionTokens::Chain chain;
        if (uint64_t wait = loginLocked(c, user, now)) {
            sendThrottled(c, wait);
        } else if (!ta::auth::validUsername(user, &why)) {
            throttle_.fail(ipKeyFor(c), now, ta::srv::LoginThrottle::kAddress);
            sendAuthResult(c, AuthStatus::BadUsername, nullptr, why);
        } else if (const ta::srv::Account* a = accounts_.find(user);
                   a && (f.kind == Msg::AuthToken ? tokens_.spend(*a, token, now, &chain)
                                                  : tokens_.revoke(*a, token))) {
            throttle_.succeed(userKeyFor(user));   // the account only, as for a password
            if (f.kind == Msg::AuthToken) {
                signedIn(c, *a, nullptr, now, &chain);
            } else {
                sendAuthResult(c, AuthStatus::Ok, nullptr, "signed out");
                c.conn.flushWrite();
                c.conn.fail("signed out");
            }
        } else {
            throttle_.fail(ipKeyFor(c), now, ta::srv::LoginThrottle::kAddress);
            if (a) throttle_.fail(userKeyFor(user), now, ta::srv::LoginThrottle::kAccount);
            sendAuthResult(c, AuthStatus::BadPassword, nullptr, "sign in with your password");
            std::fprintf(stderr, "client %u (%s) failed token sign-in for '%s'\n",
                         c.id, c.peer.c_str(), user.c_str());
        }
        ta::crypto::wipe(token.data(), token.size());
        return;
    }

    if (f.kind == Msg::AuthRegister) {
        if (!c.pendAuth.challenged || !c.pendAuth.newAccount) {
            sendReject(c, "unexpected registration"); c.conn.fail("bad auth"); return;
        }
        std::vector<uint8_t> stored = r.bytes(ta::crypto::kHashLen);
        std::vector<uint8_t> serverKey = r.bytes(ta::crypto::kHashLen);
        if (!r.ok || r.p!=r.end) { sendReject(c, "malformed registration"); c.conn.fail("bad auth"); return; }

        // Registration is the one unauthenticated operation that WRITES, and each
        // one rewrites the whole account file on the tick thread -- so a flood is
        // both a disk-filling attack and a way to stall every running game. Meter
        // it per address on the same escalating curve as a failed password.
        const std::string ipKey = ipKeyFor(c);
        if (uint64_t wait = throttle_.lockedFor(ipKey, now)) { sendThrottled(c, wait); return; }
        throttle_.fail(ipKey, now, ta::srv::LoginThrottle::kAddress);

        if(!registrationOpen_ || accounts_.size()>=limits_.accounts || !accountWriteWork_.take(now,8)) {
            sendAuthResult(c,AuthStatus::ServerError,nullptr,"registration unavailable; retry later");
            c.pendAuth.challenged=false;return;
        }
        ta::auth::Credential cred;
        cred.iters = c.pendAuth.iters;
        cred.salt = c.pendAuth.salt;
        std::memcpy(cred.storedKey.data(), stored.data(), cred.storedKey.size());
        std::memcpy(cred.serverKey.data(), serverKey.data(), cred.serverKey.size());

        std::string err;
        if (!accounts_.create(c.pendAuth.user, cred, &err)) {
            // Either the name was taken in the moments since the challenge, or the
            // file could not be written. Both are worth distinguishing to the player.
            bool taken = accounts_.find(c.pendAuth.user) != nullptr;
            sendAuthResult(c, taken ? AuthStatus::NameTaken : AuthStatus::ServerError, nullptr,
                           taken ? "that name was just taken -- pick another" : err);
            std::fprintf(stderr, "taserver: registration of '%s' failed: %s\n",
                         c.pendAuth.user.c_str(), err.c_str());
            c.pendAuth.challenged = false;
            return;
        }
        c.account = c.pendAuth.user;
        c.name = c.pendAuth.user;
        // Sign the result with the ServerKey we were just handed, exactly as on
        // the sign-in path. The client can then demand a valid signature for BOTH
        // outcomes instead of having to trust an unsigned "Created" -- which a
        // machine posing as the server would otherwise use to skip the check.
        std::vector<uint8_t> am = ta::auth::authMessage(c.pendAuth.user, c.pendAuth.clientNonce,
                                                         c.pendAuth.serverNonce, c.pendAuth.salt,
                                                         c.pendAuth.iters);
        ta::crypto::Digest sig = ta::auth::serverSignature(cred.serverKey, am);
        // A registration that got this far is not a failed attempt.
        throttle_.succeed(userKeyFor(c.pendAuth.user));
        sendAuthResult(c, AuthStatus::Created, &sig, "new account created",
                       accounts_.find(c.pendAuth.user));
        std::fprintf(stderr, "client %u (%s) created account '%s' (%zu total)\n",
                     c.id, c.peer.c_str(), c.name.c_str(), accounts_.size());
        sendWelcome(c);
        return;
    }

    sendReject(c, "expected a login");
    c.conn.fail("no login");
}

void Server::sendGameList(Client& c) {
    Writer w;
    uint64_t t = nowMs();
    // Private (single-player) games are not advertised in the browser, nor is a game
    // that has ended and only waits for its watchers' logs (Room::over): it would show
    // as running, and a Watch on it is refused.
    uint32_t n = 0;
    for (auto& [id, r] : rooms_) if (!r.priv && !r.over) ++n;
    w.u32(n);
    for (auto& [id, r] : rooms_) {
        if (r.priv || r.over) continue;
        w.u32(r.id);
        w.str(r.name);
        w.str(r.mapId);
        w.u8(uint8_t(r.usedSlots()));
        w.u8(uint8_t(r.capacity()));
        w.u8(r.running ? 1 : 0);
        w.u8(r.password.empty() ? 0 : 1);
        w.u32(uint32_t((t - r.createdMs) / 1000));
        w.u8(r.opts.allowSpectators);   // the browser's WATCH honours it
    }
    c.conn.send(Msg::GameList, w);
}

// Snap an incoming unit-cap value to the allowed lobby set (defensive against a
// malformed client); anything unexpected falls back to the 2000 default.
static uint16_t clampUnitCap(uint16_t v) {
    for (uint16_t a : {250, 500, 1000, 2000}) if (v == a) return v;
#ifndef NDEBUG
    // Debug builds only: a harness cap past the menu's 2000 (TA_UNITCAP), so the
    // real client/server path can carry a 10,000-unit selection (Legion 10,000).
    if (v > 2000 && v <= 16000) return v;
#endif
    return 2000;
}

// `fromStart` writes the table the game BEGAN with instead of the live one. A
// spectator replays the whole bundle log from tick 0, so it must rebuild the world
// from the START configuration: hand it the current table and a slot closed since (a
// forfeit) reads as unused, or one an AI took over reads as an AI seat from the start,
// so the rebuilt world diverges from the game it is watching -- and any replay that
// client saves records the wrong match.
void Server::writeSlots(Writer& w, Room& r, bool fromStart) {
    RoomState s;
    s.id = r.id; s.name = r.name; s.mapId = r.mapId; s.mission = r.mission; s.opts = r.opts;
    s.hostId = r.hostId; s.mapsReady = mapsReady(r);
    s.capacity = uint8_t(r.cap); s.passworded = !r.password.empty();
    s.roomRev = r.roomRev; s.changeBits = r.changeBits; s.mapSerial = r.mapSerial;
    for (int i = 0; i < kMaxSlots; ++i) s.slots[i] = (fromStart && r.running) ? r.startSlots[i] : r.slots[i];
    put(w, s);
}

// GameStarting: the START slot table + options, then your slot (0xFF = spectator),
// the seed, your resume token and where the bundle log ends. At a fresh start the
// log is empty and the start table is the live one. `catchUp` (a spectate)
// re-offers the room's packages first and then streams the whole log to
// this client from tick 0, paced by txPending(). Flow control starts over; each
// caller sets `loaded` itself.
void Server::sendGameStarting(Client& c, Room& r, uint8_t slot, uint64_t token, bool catchUp) {
    Writer w; writeSlots(w, r, /*fromStart=*/true);
    w.u8(slot); w.u32(r.seed); w.u64(token);
    // Where the HISTORY ends. The client must not send -- or treat its own
    // commands coming back as acknowledgements -- until it has consumed
    // every bundle logged before it arrived. It cannot work that out
    // locally: the replay is streamed in chunks paced by the socket, so its
    // receive buffer legitimately runs dry BETWEEN chunks while history is
    // still coming, and a buffer-depth guess releases the gate early.
    w.u32(uint32_t(r.log.size()));
    if (catchUp) {
        if (r.mapPackage) {
            c.mapReadyRoom = 0; c.mapOfferedRoom = r.id;
            c.conn.send(Msg::MapOffer, ta::net::maps::offer(r.id, r.mapId, *r.mapPackage));
        }
        if(r.modPackage)c.conn.send(Msg::ModOffer,ta::net::maps::offer(r.id,"mods",*r.modPackage));
    }
    c.conn.send(Msg::GameStarting, w);
    if (catchUp) { c.replaying = true; c.replayPos = 0; }
    c.ackTick = c.arrivedTick = 0; c.leadTicks = kMaxLeadTicks;   // flow control starts over with this game
}

void Server::broadcastLobby(Room& r) {
    Writer w; writeSlots(w, r);
    for (int i = 0; i < kMaxSlots; ++i) {
        if (r.slotClient[i] < 0) continue;
        auto it = clients_.find(uint32_t(r.slotClient[i]));
        if (it != clients_.end()) it->second->conn.send(Msg::LobbyState, w);
    }
    // Lobby spectators (e.g. a host who created the game to watch) see the room
    // fill too -- and this is how they learn hostId, so they can start it.
    for (uint32_t sid : r.spectators) {
        auto it = clients_.find(sid);
        if (it != clients_.end()) it->second->conn.send(Msg::LobbyState, w);
    }
    if (r.mapPackage) for (auto& [id, peer] : clients_) {
        if (peer->roomId == r.id && peer->mapOfferedRoom != r.id) {
            peer->conn.send(Msg::MapOffer, ta::net::maps::offer(r.id, r.mapId, *r.mapPackage));
            peer->mapOfferedRoom = r.id;
        }
    }

    if(r.opts.gameMods && r.modPackage)for(auto& [id,peer]:clients_) {
        if(peer->roomId==r.id && peer->modOffered!=r.modPackage->digest){
            peer->conn.send(Msg::ModOffer,ta::net::maps::offer(r.id,"mods",*r.modPackage));
            peer->modOffered=r.modPackage->digest;
        }
    }

}

void Server::broadcastRoom(Room& r, Msg kind, const Writer& w, uint32_t exceptClient) {
    for (int i = 0; i < kMaxSlots; ++i) {
        if (r.slotClient[i] < 0) continue;
        uint32_t cid = uint32_t(r.slotClient[i]);
        if (cid == exceptClient) continue;
        auto it = clients_.find(cid);
        // A client still streaming catch-up gets its bundles from the log cursor
        // instead; sending the live one now would land it ahead of the history.
        if (it != clients_.end() &&
            !(kind == Msg::TickBundle && it->second->replaying))
            it->second->conn.send(kind, w);
    }
    // Spectators receive the same stream (bundles, chat, pause/resume, status).
    for (uint32_t cid : r.spectators) {
        if (cid == exceptClient) continue;
        auto it = clients_.find(cid);
        if (it != clients_.end() &&
            !(kind == Msg::TickBundle && it->second->replaying))
            it->second->conn.send(kind, w);
    }
}

void Server::sendJoin(Client& c, bool ok, int slot, const std::string& why, const Room* r) {
    Writer jr; jr.u8(ok ? 1 : 0); jr.u8(uint8_t(slot)); jr.str(why); jr.u32(r ? r->roomRev : 0);
    c.conn.send(Msg::JoinResult, jr);
}

int Server::bumpRules(Room& r, uint8_t bits) {
    ++r.roomRev;
    r.changeBits = bits;
    int unreadied = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        SlotInfo& s = r.slots[i];
        if (s.type == 1 && s.ready && r.slotClient[i] != int(r.hostId)) { s.ready = 0; ++unreadied; }
    }
    return unreadied;
}

void Server::resetRoomMap(Room& r) {
    r.mapVfs.reset(); r.mapPackage.reset();
    for (auto& [id, peer] : clients_)
        if (peer->roomId == r.id) {
            peer->mapReadyRoom = peer->mapOfferedRoom = 0;
            peer->mapSend = {}; peer->mapReceive = {};
            peer->mapValidationId = 0; peer->deferredMapOffer.reset();
        }
    for (auto& job : validations_)
        if (job.room == r.id && !job.mods) job.stop.request_stop();
}

Server::CapacityResult Server::applyCapacity(Room& r, int newCap, std::vector<uint32_t>& moved) {
    CapacityResult out;
    const auto plan = planCapacity(r.slots, newCap);
    if (!plan.ok) return out;   // the caller refused it first
    const int oldCap = r.cap;
    const auto empty = [&](int i, uint8_t type) {
        SlotInfo& s = r.slots[i];
        if (s.type != type) out.changed = true;
        s.type = type; s.name.clear(); s.ready = 0;
    };
    // The AIs first: a displaced one leaves the seat a human moves into.
    for (int i : plan.removedAi) { empty(i, 0); ++out.aiRemoved; }
    for (const auto& [from, to] : plan.moves) {
        // As MoveSeat: side, colour and team travel with the player.
        std::swap(r.slots[from], r.slots[to]);
        r.slots[to].ready = 0;
        r.slotClient[to] = r.slotClient[from]; r.slotClient[from] = -1;
        if (const auto it = clients_.find(uint32_t(r.slotClient[to])); it != clients_.end()) {
            it->second->slot = to;
            moved.push_back(it->first);
        }
        ++out.movedHumans; out.changed = true;
    }
    for (int i = newCap; i < kMaxSlots; ++i) empty(i, 3);
    // Seats it gains were closed only by the capacity: SlotUpdate allows nothing else past it.
    for (int i = oldCap; i < newCap && i < kMaxSlots; ++i) empty(i, 0);
    out.changed |= oldCap != newCap;
    r.cap = newCap;
    return out;
}

void Server::roomRefuse(Client& c, Room& r, RoomRequest req, uint32_t serial, const std::string& why) {
    Writer e; e.u8(uint8_t(req)); e.u32(serial); e.str(why);
    c.conn.send(Msg::RoomError, e);
    Writer ls; writeSlots(ls, r);
    c.conn.send(Msg::LobbyState, ls);
}

// Is this listener too far behind the running room to be sent voice? Voice shares the
// listener's TCP stream with its tick bundles, so on a link too slow for both it delays
// them -- and a seated player's delay holds the whole room (canAdvance). Conn's own rules
// keep voice behind game traffic and bound how much the kernel holds, but not what is
// already on its way (in flight, or in a slow reader's receive buffer). So the tick the
// listener last reported RECEIVING decides: past 3/4 of the lead it would get on an idle
// link (flowLeadTicks at its base round trip), voice to it stops and its queued voice is
// dropped; it resumes once it is back within half that lead, but not before a back-off of
// 5 s that doubles with each repeat (to 60 s; forgotten after 2 quiet minutes), so a link
// that cannot carry voice is not probed again every few seconds. The game's own pace never
// depends on voice. The received tick, not the processed one: a slow MACHINE has its
// bundles in hand and runs them late, and in a room it holds back it lags by its whole
// lead for as long as that lasts -- voice is not what delays it. (A client that does not
// report it counts as having received only what it processed.)
bool Server::voiceBehind(Client& d, const Room& r, uint64_t now) {
    const uint32_t lead = d.baseRttMs == UINT32_MAX ? d.leadTicks
                                                    : ta::net::flowLeadTicks(int(r.opts.speed), d.baseRttMs);
    const uint32_t arrived = std::max(d.ackTick, d.arrivedTick);
    const uint64_t lag = r.tick > arrived ? r.tick - arrived : 0;
    if (d.voiceMuted) {
        if (now < d.voiceMutedUntil || lag * 2 > lead) return true;
        d.voiceMuted = false;
        return false;
    }
    if (lag * 4 <= uint64_t(lead) * 3) return false;
    if (now - d.voiceTripAt > 120000) d.voiceStrikes = 0;
    d.voiceTripAt = now;
    d.voiceMutedUntil = now + std::min<uint64_t>(5000ull << std::min(d.voiceStrikes, 4u), 60000);
    ++d.voiceStrikes;
    d.voiceMuted = true;
    d.conn.dropLow();
    if (commandLogs_.take(now, 4))
        std::fprintf(stderr, "game %u: client %u lags %llu ticks (lead %u): voice to it paused %llu s\n", r.id, d.id,
                     (unsigned long long)lag, lead, (unsigned long long)((d.voiceMutedUntil - now) / 1000));
    return true;
}

// Voice chat: one frame from a talker in room r, to the recipients its VOICE mask picks
// with exactly the Chat rules -- the whole room (spectators included) while the room is
// not running, otherwise the seated players whose bit is set, and the spectators when the
// frame carries kVoiceSpectators; a spectator in a running game reaches nobody
// (spectators never talk in game) -- minus the
// talker, minus anyone who has not asked for voice or is still catching up, and minus
// anyone lagging the running game (voiceBehind). Sent as low-priority frames
// (Conn::sendLow), which a backed-up recipient drops.
void Server::relayVoice(Client& c, Room& r, const Frame& f) {
    Reader rd(f.payload.data(), f.payload.size());
    const uint16_t seq = rd.u16();
    const SlotMask recipients = rd.u16();
    const uint8_t flags = rd.u8();
    const uint16_t len = rd.u16();
    // An END marker carries no audio and opens nothing: audio, or START, with END is malformed
    // (the client never sends it), as is an audio frame without audio.
    if (!rd.ok || len > kVoiceMaxOpus || size_t(rd.end - rd.p) != len || (flags & ~(kVoiceStart | kVoiceEnd | kVoiceSpectators)) ||
        ((flags & kVoiceEnd) ? (len != 0 || (flags & kVoiceStart)) : len == 0)) {
        c.conn.fail("invalid voice message");
        return;
    }
    const uint64_t now = nowMs();
    if (!c.voiceAbuse.take(now, kVoiceAbusePerSec)) { c.conn.fail("voice rate exceeded"); return; }
    // Budgets drop, they do not disconnect: a talker whose client stalled sends a burst.
    // An END marker skips the budgets so a cut-off transmission still closes, but only the
    // one END owed to a transmission that relayed audio: ENDs can never outnumber the audio
    // frames the budgets let through, and any other END reaches nobody.
    const bool end = flags & kVoiceEnd;
    if (end) {
        if (!c.voiceOpen) return;
        c.voiceOpen = false;
    } else if (!c.voiceFrames.take(now, kVoiceFramesPerSec) || !c.voiceBytes.take(now, kVoiceBytesPerSec, len)) {
        ++c.voiceDropped;
        if (now - c.voiceLogAt >= 5000 && commandLogs_.take(now, 4)) {
            c.voiceLogAt = now;
            std::fprintf(stderr, "game %u: client %u voice over budget (%llu frames dropped)\n", r.id, c.id,
                         (unsigned long long)c.voiceDropped);
        }
        return;
    }
    if (!end && !voiceGlobal_.take(now, kVoiceGlobalFramesPerSec)) { ++voiceGlobalDropped_; return; }
    if (!end) c.voiceOpen = true;
    const bool named = (flags & kVoiceStart) || c.voiceOut % kVoiceNameEvery == 0;
    ++c.voiceOut;
    Writer w;
    w.u32(c.id); w.u8(c.slot >= 0 ? uint8_t(c.slot) : uint8_t(0xFF)); w.u16(seq);
    const bool toSpectators = flags & kVoiceSpectators;
    w.u8(uint8_t((flags & ~kVoiceSpectators) | (named ? kVoiceHasName : 0))); w.u16(len);
    w.b.insert(w.b.end(), rd.p, rd.p + len);
    if (named) w.str(c.name);
    auto deliver = [&](uint32_t cid) {
        if (cid == c.id) return;
        auto it = clients_.find(cid);
        if (it == clients_.end() || !it->second->voiceListen || it->second->replaying) return;
        if (r.running && voiceBehind(*it->second, r, now)) return;
        it->second->conn.sendLow(Msg::VoiceDown, w.b);
    };
    if (r.running && c.slot < 0) return;   // spectators never talk in a running game
    for (int slot = 0; slot < kMaxSlots; ++slot)
        if ((!r.running || (recipients & (1u << slot))) && r.slotClient[slot] >= 0) deliver(uint32_t(r.slotClient[slot]));
    if (!r.running || toSpectators)
        for (uint32_t cid : r.spectators) deliver(cid);
}

void Server::lobbyMsg(Client& c, const Frame& f) {
    switch (f.kind) {
        case Msg::ListGames: sendGameList(c); break;
        case Msg::CreateGame: {
            // The server's game limit refuses the request as any other refusal does: the
            // player stays connected, in the game list, and is told why (JoinResult).
            if(rooms_.size()>=limits_.rooms) {
                sendJoin(c,false,0,"server game capacity reached",nullptr);
                std::fprintf(stderr,"client %u: create refused -- server game capacity reached\n",c.id);
                return;
            }
            Reader r(f.payload.data(), f.payload.size());
            std::string name = r.str(), pass = r.str(), mapId = r.str(), mission = r.str();
            GameOptions o; get(r, o);
            if (!ta::sim::validPathfindingMode(uint8_t(o.pathfindingMode))) { sendReject(c,"invalid pathfinding mode"); return; }
            // Sanitise the raw options (get() checks only the pathfinder byte).
            // The lobby always asks for game mods; 0 comes from a debug --mods launch.
            o.gameMods = o.gameMods ? 1 : 0;
            if(!mission.empty())o.gameMods=0;   // campaigns never run game mods
            if(o.benchmark)o.gameMods=0;          // nor the Benchmark: plain retail data
            if (o.speed < 1) o.speed = 10;
            o.unitCap = clampUnitCap(o.unitCap);
            o.monarchExpendable = o.monarchExpendable ? 1 : 0;
            o.stressTest = std::min<uint8_t>(o.stressTest, kStressStaged);   // 0/1/2, see GameOptions
            // THREE-state, not a bool: 0 = not explored, 1 = explored, 2 = full
            // vision. Decoding it as `? 1 : 0` silently folded FULL VISION back to
            // EXPLORED, so the room's fog button cycled through a setting the
            // server threw away -- it looked like the button did nothing.
            o.fogExplored = std::min<uint8_t>(o.fogExplored, 2);
            // Start Locations is three-state as well (FIXED, RANDOM, BY SLOT); anything
            // else reads as RANDOM, which is what a nonzero byte meant before BY SLOT.
            o.randomStarts = normalizeStartMode(o.randomStarts);
            o.doubleSight = o.doubleSight ? 1 : 0;
            o.allowSpectators = o.allowSpectators ? 1 : 0;
            if (!mission.empty()) { o.pathfindingMode = ta::sim::PathfindingMode::Retail; o.doubleSight = 0; }
            // CHEAT CODES: on or off, never in a campaign mission (TotalA.exe 0x497462: cheats off for game type 1).
            o.powerCodes = o.powerCodes && mission.empty() ? 1 : 0;
            o.losType = o.losType || !mission.empty() ? 1 : 0;   // a campaign is True (0x4363d9)
            // A campaign mission, or any game on a private single-player server (a
            // loopback, no-auth, non-LAN local server: PLAY THE MACHINE / skirmish), is
            // played alone: its player may always change the game speed with -/+, as in
            // TA's single player.
            if (!mission.empty() || privateSinglePlayer()) o.speedUnlock = 1;
            // MAP SCENARIO: only an ordinary file-map room can play a map's scenario. The map
            // itself is checked at START -- its package has not arrived yet.
            o.mapScenario = (o.mapScenario && mission.empty() && !ta::mapgen::isGeneratedMapId(mapId) &&
                             !o.stressTest && !o.benchmark) ? 1 : 0;
            if (o.mapScenario) o.randomStarts = 0;   // ownership and rule regions follow fixed seats
            int cap = int(r.u8());
            uint8_t spectate = r.u8();   // host watches, taking no slot (all-AI game)
            uint8_t priv = r.u8();       // private (single-player): hidden from the list
            // Only a broken client sends a malformed frame: dropped without an answer.
            if (!r.ok || r.p!=r.end || mapId.size()>4096 || mission.size()>256) return;
            // The name and password limits are the room's (SetRoomInfo keeps the same ones);
            // a request past them is answered, so the player sees why.
            if (!name.empty() && (name.size() > kRoomNameMax || !printableAscii(name))) {
                sendJoin(c, false, 0, "the game name must be 1 to " + std::to_string(kRoomNameMax) +
                         " plain characters", nullptr);
                return;
            }
            if (pass.size() > kRoomPasswordMax || !printableAscii(pass)) {
                sendJoin(c, false, 0, "the password must be at most " + std::to_string(kRoomPasswordMax) +
                         " plain characters", nullptr);
                return;
            }
            // A random map is built from its id at START: refuse one this build cannot make.
            // Its capacity is its player count, which the server reads from the id itself.
            if (mission.empty() && ta::mapgen::isGeneratedMapId(mapId)) {
                try { cap = std::clamp<int>(ta::mapgen::parseMapId(mapId).players, 2, kMaxSlots); }
                catch (const std::exception& e) { sendReject(c, e.what()); return; }
            }
            if(!(testWork_ || (loopbackOnly_ && !requireAuth_)) && (o.stressTest || o.benchmark || !mission.empty())) {
                sendReject(c,"public servers do not accept benchmark, stress or campaign mission requests");return;
            }
            o.speed=uint8_t(std::clamp<int>(o.speed,kMinGameSpeed,maxGameSpeed(o)));
            if (cap < 2 || cap > kMaxSlots) cap = kMaxSlots;   // sane default
            Room& room = rooms_[nextRoomId_];
            room.id = nextRoomId_++;
            room.name = name.empty() ? defaultRoomName(c.name) : name;
            room.password = pass;
            room.mapId = mapId;
            room.mission = mission;
            room.opts = o;
            room.priv = priv != 0;
            room.cap = cap;
            room.hostId = c.id;
            room.createdMs = nowMs();
            // Roll this game's seed here, once. It is broadcast in GameStarting, so
            // every peer and the referee derive the same arrangement from it -- it
            // just must not be a function of the room id (see Room::seed).
            room.seed = fixedSeedSet_ ? fixedSeed_ : uint32_t(randToken());
            // Open slots up to the map capacity, close the rest (the map has no
            // start position for them).
            for (int i = 0; i < kMaxSlots; ++i) {
                room.slots[i].type = i < cap ? 0 : 3;
                room.slots[i].color = uint8_t(i);
                room.slots[i].team = uint8_t(i);
            }
            if (spectate) {
                // Host watches without a slot -- every capacity slot stays open (fill
                // with AIs). The host still owns the room (add AIs, start). ALLOW
                // SPECTATORS does not apply: it governs who ELSE may watch.
                room.spectators.push_back(c.id);
                c.clearCommands();   // never inherit a previous room's queue
                c.state = Client::InGame; c.roomId = room.id; c.slot = -1;
                sendJoin(c, true, kWatchSeat, "", &room);   // ok, spectator
            } else {
                // Host takes slot 0 (human).
                room.slots[0].type = 1; room.slots[0].faction = 0; room.slots[0].color = 0;
                room.slots[0].team = 0; room.slots[0].name = c.name;
                room.slotClient[0] = int(c.id);
                c.clearCommands();   // never inherit a previous room's queue
                c.state = Client::InGame; c.roomId = room.id; c.slot = 0;
                sendJoin(c, true, 0, "", &room);   // ok, slot 0
            }
            broadcastLobby(room);
            std::fprintf(stderr, "client %u created game %u '%s' on '%s' (%s)\n", c.id, room.id, room.name.c_str(),
                         room.mission.empty() ? room.mapId.c_str() : room.mission.c_str(), optionsSummary(room.opts).c_str());
            break;
        }
        case Msg::JoinGame: {
            Reader r(f.payload.data(), f.payload.size());
            uint32_t gid = r.u32(); std::string pass = r.str();
            if (!r.ok) return;
            auto it = rooms_.find(gid);
            auto reject = [&](const std::string& why) { sendJoin(c, false, 0, why, nullptr); };
            if (it == rooms_.end()) { reject("no such game"); break; }
            Room& room = it->second;
            if (room.running) { reject("game already started"); break; }
            if (!room.password.empty() && room.password != pass) { reject("wrong password"); break; }
            int freeSlot = -1;
            for (int i = 0; i < kMaxSlots; ++i)
                if (room.slots[i].type == 0) { freeSlot = i; break; }
            if (freeSlot < 0) { reject("game is full"); break; }
            room.slots[freeSlot].type = 1;
            room.slots[freeSlot].name = c.name;
            room.slots[freeSlot].ready = 0;   // a newcomer is never READY for a game it has not seen
            room.slotClient[freeSlot] = int(c.id);
            c.clearCommands();   // never inherit a previous room's queue
            c.state = Client::InGame; c.roomId = room.id; c.slot = freeSlot;
            sendJoin(c, true, freeSlot, "", &room);
            broadcastLobby(room);
            std::fprintf(stderr, "client %u joined game %u at slot %d\n", c.id, room.id, freeSlot);
            break;
        }
        case Msg::Rejoin: {
            // A quick reconnect inside the disconnect wait. The client
            // kept its world: it names the first tick it has no bundle for and gets the
            // log from there, not a rebuild from tick 0 (the long rejoin that replayed
            // the whole game is gone -- past the wait an AI has the seat).
            Reader r(f.payload.data(), f.payload.size());
            uint32_t gid = r.u32(); uint64_t token = r.u64(); uint32_t fromTick = r.u32();
            if (!r.ok || r.p!=r.end) return;
            auto it = rooms_.find(gid);
            auto reject = [&](const std::string& why) { sendJoin(c, false, 0, why, nullptr); };
            if (it == rooms_.end() || !it->second.running) { reject("game not found or ended"); break; }
            Room& room = it->second;
            int slot = -1;
            for (int i = 0; i < kMaxSlots; ++i)
                if (room.slots[i].type == 1 && token != 0 && room.slotToken[i] == token) { slot = i; break; }
            if (slot < 0) {
                bool taken = false;
                for (int i = 0; i < kMaxSlots; ++i) taken |= token != 0 && room.takenToken[i] == token;
                reject(taken ? "the wait ran out: an AI took over your seat" : "invalid or expired resume token");
                break;
            }
            if (fromTick > room.log.size()) { reject("the game has no such tick"); break; }
            // The seat's old connection, when the server has not seen it go yet (a
            // client that lost its link first): this one replaces it.
            if (int old = room.slotClient[slot]; old >= 0 && uint32_t(old) != c.id) {
                auto oit = clients_.find(uint32_t(old));
                if (oit != clients_.end()) unseat(*oit->second, "replaced by a reconnect");
            }
            // Re-seat the client and rotate the token (single use).
            room.slotClient[slot] = int(c.id);
            room.slotToken[slot] = randToken();
            // The host's powers are the seat's: a host back under a new connection is the
            // host again. Only the seat's own token gets here, and a seat whose powers passed
            // on (passHost) no longer is room.hostSlot, so nobody else can claim them.
            const bool hostBack = room.hostSlot == slot && room.hostId != c.id;
            if (hostBack) room.hostId = c.id;
            c.clearCommands();   // never inherit a previous room's queue
            c.state = Client::InGame; c.roomId = gid; c.slot = slot; c.loaded = true;
            // It has the map, the game mods and the world already.
            c.mapReadyRoom = c.mapOfferedRoom = room.id;
            if (room.modPackage) c.modReady = c.modOffered = room.modPackage->digest;
            Writer w; w.u8(uint8_t(slot)); w.u64(room.slotToken[slot]); w.u32(fromTick); w.u32(uint32_t(room.log.size()));
            c.conn.send(Msg::Reseated, w);
            // What changed while it was away that no bundle carries, before the bundles it
            // missed: the room as it is now (LobbyState: the game speed and the host, the
            // room told too when that is this seat again), and the host's pause. Its world,
            // its recording and the tick it continues from stay its own.
            if (hostBack) broadcastLobby(room);
            else { Writer ls; writeSlots(ls, room); c.conn.send(Msg::LobbyState, ls); }
            if (room.paused) { Writer pw; pw.u8(1); pw.u8(0); c.conn.send(Msg::Pause, pw); }
            // The bundles it is missing, streamed like a spectator's catch-up. Flow
            // control resumes from where it says it is (a client that lies only paces
            // the room for itself, as a lying Progress could).
            c.replaying = fromTick < room.log.size(); c.replayPos = fromTick;
            c.ackTick = c.arrivedTick = fromTick ? fromTick - 1 : 0; c.leadTicks = kMaxLeadTicks;
            const bool waited = room.waitDeadline[slot] != 0;
            if (waited) endWait(room, slot);
            else sendWaiting(c, room);
            Writer ps; ps.u8(uint8_t(slot)); ps.u8(0 /*connected*/); ps.u32(0);
            broadcastRoom(room, Msg::PlayerStatus, ps, c.id);
            std::fprintf(stderr, "game %u: client %u RESEATED slot %d from tick %u (%zu ticks to send)%s\n",
                         room.id, c.id, slot, fromTick, room.log.size() - fromTick, waited ? ", wait over" : "");
            break;
        }
        case Msg::Spectate: {
            Reader r(f.payload.data(), f.payload.size());
            uint32_t gid = r.u32(); std::string pass = r.str();
            if (!r.ok) return;
            auto it = rooms_.find(gid);
            auto reject = [&](const std::string& why) { sendJoin(c, false, 0, why, nullptr); };
            // A game that is over (Room::over) is only waiting for its watchers' logs.
            if (it == rooms_.end() || !it->second.running || it->second.over) { reject("game not found or ended"); break; }
            Room& room = it->second;
            // The host's ALLOW SPECTATORS rule. This is the ONLY way into a room without
            // a slot after creation, so refusing it here covers a browser's WATCH, a
            // seated player who left and asks to watch, and a dropped spectator coming
            // back (spectators hold no resume token; Rejoin seats only seated players).
            // Checked before the password so a locked room still says why.
            if (!room.opts.allowSpectators) {
                reject("spectators are not allowed in this game");
                std::fprintf(stderr, "game %u: client %u refused as a spectator (spectators off)\n", room.id, c.id);
                break;
            }
            if (!room.password.empty() && room.password != pass) { reject("wrong password"); break; }
            // Seat nothing: a spectator has no slot, sends no commands or hashes,
            // and holds nothing on disconnect. It just receives the stream.
            if (std::find(room.spectators.begin(), room.spectators.end(), c.id) == room.spectators.end())
                room.spectators.push_back(c.id);
            c.clearCommands();   // never inherit a previous room's queue
            c.state = Client::InGame; c.roomId = gid; c.slot = -1; c.loaded = true;
            // GameStarting (slot 0xFF = spectator) rebuilds the world; the whole
            // bundle log replays it to now, then live bundles stream via broadcast.
            // That rebuild starts at tick 0, so it needs the START table, not one with
            // mid-game forfeits and AI takeovers already applied (see writeSlots).
            sendGameStarting(c, room, 0xFF, 0, /*catchUp=*/true);
            if (room.waiting()) sendWaiting(c, room);   // a countdown already running
            std::fprintf(stderr, "game %u: client %u SPECTATING (replaying %zu ticks)\n",
                         room.id, c.id, room.log.size());
            break;
        }
        default: break;   // ignore other messages while in the lobby
    }
}

// Everything a connection has in flight for a room, dropped in one place.
//
// Queued commands are ROOM state living on a CONNECTION: left behind they drain
// into whatever game this connection joins next, carrying the old player stamp
// and unit ids into a world where those ids mean something else.
//
// The SCHEDULED ones matter for a different reason. With server input delay
// (TA_SRV_DELAY > 0) a departing player's commands sit in pendingAt buckets
// several ticks out, which puts them in bundles AFTER the tick a reconnect
// catches up from -- so they arrive looking like acknowledgements for commands
// sent after the reconnect. Safe to purge: nothing here has been bundled or
// broadcast yet, so every peer still agrees. A player who just dropped should not
// have orders fire seconds later in any case.
//
// Called from BOTH disconnect paths. It first lived only in leaveRoom, which a
// seated player disconnecting from a running game never reaches -- that takes the
// slot-preserving branch of dropClient, which is exactly the reconnect path this
// is meant to protect. The fix ran everywhere except where it was needed.
bool Server::takeInputBudget(Client& c, uint64_t now, unsigned bytes, unsigned count) {
    const auto key = c.account.empty() ? "ip:" + c.peer : "user:" + ta::auth::foldUsername(c.account);
    if (!(c.commandBytes.available(now, 4u << 20, bytes) && commandByteKeys_.available(key, now, 4u << 20, bytes) &&
          commandBytes_.available(now, 32u << 20, bytes) &&
          (!count || (c.commandCount.available(now, 65536, count) && commandCountKeys_.available(key, now, 65536, count) &&
                      commandCount_.available(now, 524288, count)))))
        return false;
    if (bytes) {
        c.commandBytes.take(now, 4u << 20, bytes); commandByteKeys_.take(key, now, 4u << 20, bytes);
        commandBytes_.take(now, 32u << 20, bytes);
    }
    if (count) {
        c.commandCount.take(now, 65536, count); commandCountKeys_.take(key, now, 65536, count);
        commandCount_.take(now, 524288, count);
    }
    return true;
}

bool Server::handleOrderFrame(Client& c, Room& r, const Frame& f) {
    const auto now = nowMs();
    // A frame's bytes at receipt; the order's members are charged as commands when
    // closeTick drains it (a 10,000-unit order costs what its 10,000 commands did).
    auto admit = [&](size_t bytes) { return takeInputBudget(c, now, unsigned(bytes), 0); };
    std::shared_ptr<const ta::net::SelectionOrder> order;
    using R = ta::srv::OrderInbox::Result;
    const R result = c.orders.frame(f.kind, f.payload, r.orderSerialFloor[size_t(c.slot)], admit, order);
    if (result == R::Malformed) return false;
    if (result == R::Committed) {
        Command cmd;
        cmd.kind = ta::net::Cmd::Order;
        cmd.player = uint8_t(c.slot);
        c.orders.queued(ta::net::orderRecordBytes(*order));
        cmd.order = std::move(order);
        c.cmdQueue.push_back(std::move(cmd));
    } else if (result == R::Dropped) {
        // Never acknowledged by a bundle, so the client must hear of it: its window
        // releases the order (otherwise it would hold its place there for good).
        sendInputDropped(c, 0, c.orders.droppedSerial());
        if (now - c.ordersLogAt >= 5000 && commandLogs_.take(now, 4)) {
            c.ordersLogAt = now;
            std::fprintf(stderr, "client %u: order budget/queue exceeded; dropped %llu orders total\n", c.id,
                         (unsigned long long)c.orders.dropped());
        }
    }
    return true;
}

void Server::dropPendingCommands(Client& c, Room& r) {
    c.modSend={};c.modReceive={};c.modReady.clear();c.modOffered.clear();
    c.mapSend = {}; c.mapReceive = {}; c.mapReadyRoom = c.mapOfferedRoom = 0;
    c.clearCommands();
    c.cmdDropped = 0;
    if (c.slot < 0) return;
    const int slot = c.slot;
    size_t purged = 0;
    for (auto& [tick, cmds] : r.pendingAt) {
        const size_t before = cmds.size();
        cmds.erase(std::remove_if(cmds.begin(), cmds.end(),
                                  [slot](const Command& q) { return int(q.player) == slot; }),
                   cmds.end());
        purged += before - cmds.size();
    }
    if (purged)
        std::fprintf(stderr, "game %u: slot %d detached -- purged %zu scheduled command(s)\n",
                     r.id, slot, purged);
}

void Server::leaveRoom(Client& c, const char* reason) {
    Room* r = roomOf(c);
    if (!r) return;
    dropPendingCommands(c, *r);
    // First the connection detaches: a spectator just leaves the stream (no slot, nothing
    // to forfeit); a seated player frees its seat in the lobby and forfeits it in a game.
    // Then, separately, the host's departure (a spectator host's included).
    auto& sp = r->spectators;
    const auto watching = std::find(sp.begin(), sp.end(), c.id);
    const bool spectator = watching != sp.end();
    if (spectator) {
        sp.erase(watching);
    } else if (c.slot >= 0 && c.slot < kMaxSlots) {
        r->slotClient[c.slot] = -1;
        // The seat opens unready: a READY left on it would seat its next player ready.
        if (!r->running) { r->slots[c.slot].type = 0; r->slots[c.slot].name.clear(); r->slots[c.slot].ready = 0; }
        else {
            // Voluntary leave mid-game = immediate forfeit (sequenced event so the
            // sim disposes the units in lockstep).
            r->slots[c.slot].type = 3;
            r->pendingEvents.push_back({ta::net::Event::Kind::Leave, uint8_t(c.slot)});
            endWait(*r, c.slot);   // it was silent, came back and left: nothing to wait for
        }
    }
    const uint32_t rid = r->id;
    const bool wasHost = (r->hostId == c.id);
    c.mapValidationId=c.modValidationId=0;c.deferredMapOffer.reset();
    c.state = Client::Lobby; c.roomId = 0; c.slot = -1; c.loaded = false;
    if (spectator) std::fprintf(stderr, "client %u stopped spectating game %u (%s)\n", c.id, rid, reason);
    // A spectator that is not the host changes nothing the room is shown.
    if (spectator && !wasHost) return;
    // The host leaving a running game: its powers pass on (or, on a LOCAL SERVER, the
    // game closes with it).
    if (r->running && wasHost) passHost(*r, c.id);
    // A LOCAL SERVER's room is its host's: it closes with them, and everyone still in
    // it is told so (their clients go back to the CONNECT screen).
    if (!r->running && wasHost && lan_) {
        std::vector<uint32_t> others(r->spectators.begin(), r->spectators.end());
        for (int i = 0; i < kMaxSlots; ++i)
            if (r->slotClient[i] >= 0 && uint32_t(r->slotClient[i]) != c.id) others.push_back(uint32_t(r->slotClient[i]));
        rooms_.erase(rid);
        for (uint32_t id : others)
            if (auto it = clients_.find(id); it != clients_.end()) {
                Client& o = *it->second;
                o.state = Client::Lobby; o.roomId = 0; o.slot = -1; o.loaded = false;
                sendReject(o, "the host closed the game");
                o.conn.fail("host closed the game");
            }
        std::fprintf(stderr, "game %u closed by its host (%zu others)\n", rid, others.size());
        return;
    }
    // If the host left in the lobby, pass host to the next human (or dissolve).
    if (!r->running && wasHost) {
        int next = -1;
        for (int i = 0; i < kMaxSlots; ++i)
            if (r->slotClient[i] >= 0) { next = r->slotClient[i]; break; }
        if (next >= 0) r->hostId = uint32_t(next);
        else { rooms_.erase(rid); std::fprintf(stderr, "game %u dissolved\n", rid); return; }
    }
    broadcastLobby(*r);   // reachable only when the room still exists (not dissolved)
    if (!spectator) std::fprintf(stderr, "client %u left game %u (%s)\n", c.id, rid, reason);
}

void Server::passHost(Room& r, uint32_t leaving) {
    if (lan_) {
        // A LOCAL SERVER's game is its host's (its lobby too, see leaveRoom): nobody
        // succeeds the host. Everyone still in it is told and their connections close; the
        // run loop then tears the empty room down (a tick may be in flight, so not here).
        r.hostId = 0; r.hostSlot = -1;
        size_t others = 0;
        for (auto& [id, peer] : clients_)
            if (id != leaving && peer->roomId == r.id) {
                sendReject(*peer, "the host closed the game");
                peer->conn.fail("host closed the game");
                ++others;
            }
        std::fprintf(stderr, "game %u closed by its host (%zu others)\n", r.id, others);
        return;
    }
    // The first seated player still connected and still playing, else the first one the
    // room holds the seat of for a reconnect (its powers wait for it, as the host's own
    // would: Rejoin). None (only AIs, the defeated or spectators are left): nobody holds
    // the host's powers from now on.
    int next = -1;
    for (int i = 0; i < kMaxSlots && next < 0; ++i) {
        if (r.slots[i].type != 1 || r.defeated[size_t(i)] || r.slotClient[i] < 0 || uint32_t(r.slotClient[i]) == leaving)
            continue;
        if (const auto it = clients_.find(uint32_t(r.slotClient[i])); it != clients_.end() && it->second->conn.ok()) next = i;
    }
    for (int i = 0; i < kMaxSlots && next < 0; ++i)
        if (r.slots[i].type == 1 && !r.defeated[size_t(i)] && r.slotClient[i] < 0 && r.waitDeadline[i]) next = i;
    r.hostSlot = next;
    r.hostId = next >= 0 && r.slotClient[next] >= 0 ? uint32_t(r.slotClient[next]) : 0;
    // The pause was the host's: it ends with them rather than hold the game for a
    // successor who never asked for it (or for nobody at all).
    if (r.paused) {
        r.paused = false;
        if (!r.held()) r.rebasePace(nowUs());
        Writer rw; rw.u8(1); rw.u8(0);
        broadcastRoom(r, Msg::Resume, rw);
    }
    if (next >= 0) std::fprintf(stderr, "game %u: the host's powers pass to slot %d (client %u%s)\n", r.id, next, r.hostId,
                                r.hostId ? "" : ": away, until it reconnects");
    else std::fprintf(stderr, "game %u: nobody is left to hold the host's powers\n", r.id);
}

bool Server::mapsReady(const Room& r) const {
    if(r.opts.gameMods){
        if(!r.modPackage)return false;
        for(const auto& [id,peer]:clients_)if(peer->roomId==r.id && peer->modReady!=r.modPackage->digest)return false;
    }
    if (!r.mission.empty() || ta::mapgen::isGeneratedMapId(r.mapId)) return true;
    if (!r.mapPackage) return false;
    for (const auto& [id, peer] : clients_)
        if (peer->roomId == r.id && peer->mapReadyRoom != r.id) return false;
    return true;
}
void Server::validatePackage(Client& c,Room& room,bool mods,bool cache) {
    // Live jobs only: a cancelled one stays listed until its worker returns, and must not
    // refuse the next real offer.
    if(std::count_if(validations_.begin(),validations_.end(),[](const Validation& job){return !job.stop.get_token().stop_requested();})>=3)
        throw std::runtime_error("map validation capacity reached; retry later");
    auto& receive=mods?c.modReceive:c.mapReceive;
    if(!receive.size || mapMemory()>limits_.mapMemory)throw std::runtime_error("server map memory budget reached");
    Validation job{nextValidation_++,c.id,room.id,receive.size,mods,cache,receive.digest,room.mapId,{}, {}};
    const auto token=job.stop.get_token();
    const auto root=ta::u8path(mapRoot_);
    const auto digest=job.digest;const auto size=job.size;const auto quota=limits_.mapDisk;
    job.result=validationWorker_.submit([root,digest,size,quota,mods,cache,token,bytes=std::move(receive.bytes)]() mutable {
        if(token.stop_requested())throw std::runtime_error("map validation cancelled");
        std::shared_ptr<ta::net::maps::Package> package;
        if(cache)package=mods?ta::net::mods::loadCache(root,digest,size,size):ta::net::maps::loadCache(root,digest,size,size,token);
        else {
            if(bytes.size()!=size)throw std::runtime_error("uploaded package size mismatch");
            package=mods?ta::net::mods::decode(std::move(bytes),digest):ta::net::maps::decode(std::move(bytes),digest,token);
        }
        if(token.stop_requested())throw std::runtime_error("map validation cancelled");
        if(package && !cache) {
            if(mods)ta::net::mods::saveCache(root,*package,quota);
            else ta::net::maps::saveCache(root,*package,quota);
        }
        return package;
    });
    (mods?c.modValidationId:c.mapValidationId)=job.id;receive={};
    validations_.push_back(std::move(job));
}
void Server::finishValidations() {
    for(auto it=validations_.begin();it!=validations_.end();) {
        auto peer=clients_.find(it->client);auto room=rooms_.find(it->room);
        // A game-mod job does not depend on the map: the room may change its map while one runs.
        const auto kindId=[&](Client& cl)->uint64_t& {return it->mods?cl.modValidationId:cl.mapValidationId;};
        const bool valid=peer!=clients_.end() && room!=rooms_.end() &&
            kindId(*peer->second)==it->id && peer->second->roomId==it->room &&
            room->second.hostId==it->client && !room->second.running && (it->mods || room->second.mapId==it->map) &&
            (!it->mods || room->second.opts.gameMods);
        if(!valid)it->stop.request_stop();
        if(it->result.wait_for(std::chrono::seconds(0))!=std::future_status::ready) {++it;continue;}
        // A dropped job no longer holds its client's offer: it may offer again.
        if(!valid && peer!=clients_.end() && kindId(*peer->second)==it->id)kindId(*peer->second)=0;
        auto job=std::move(*it);it=validations_.erase(it);
        try {
            auto package=job.result.get();
            if(!valid)continue;
            auto& c=*peer->second;auto& r=room->second;(job.mods?c.modValidationId:c.mapValidationId)=0;
            if(!package && job.cache) {
                if(uint64_t(mapMemory())+uint64_t(job.size)*2>limits_.mapMemory)throw std::runtime_error("server map memory budget reached");
                auto& receive=job.mods?c.modReceive:c.mapReceive;
                const uint32_t xfer=++c.xferSeq;
                receive.begin(job.room,xfer,job.size,job.digest);
                if(job.mods)c.modStarted=c.modProgress=nowMs();
                else c.mapReceiveStarted=c.mapReceiveProgress=nowMs();
                Writer request;request.u32(job.room);request.str(job.digest);request.u32(xfer);
                c.conn.send(job.mods?Msg::ModRequest:Msg::MapRequest,request);
                continue;
            }
            if(!package || package->bytes.size()!=job.size || uint64_t(mapMemory())+uint64_t(package->bytes.size())*2>limits_.mapMemory)
                throw std::runtime_error("package size/budget mismatch");
            if(job.mods) {
                r.modPackage=std::move(package);r.modDigest=r.modPackage->digest;broadcastLobby(r);
            } else acceptMap(r,std::move(package));
        } catch(const std::exception& e) {
            if(valid) {
                auto& c=*peer->second;(job.mods?c.modValidationId:c.mapValidationId)=0;
                Writer error;error.u32(job.room);error.str(e.what());c.conn.send(job.mods?Msg::ModError:Msg::MapError,error);
            }
        }
    }
    for(auto& [id,peer]:clients_)if(!peer->modValidationId && !peer->modReceive.size && peer->deferredMapOffer) {
        auto frame=std::move(*peer->deferredMapOffer);peer->deferredMapOffer.reset();
        mapMsg(*peer,frame);
    }
}
void Server::acceptMap(Room& room, std::shared_ptr<ta::net::maps::Package> package) {
    if(uint64_t(mapMemory())+uint64_t(package->bytes.size())*2>limits_.mapMemory)throw std::runtime_error("server map memory budget reached");
    room.mapPackage = std::move(package);
    std::fprintf(stderr, "game %u: map verified %s (%zu bytes)\n", room.id,
                 room.mapPackage->digest.c_str(), room.mapPackage->bytes.size());
    room.mapVfs = std::make_unique<ta::hpi::Vfs>(&roomData(room).vfs);
    room.mapVfs->setMapFiles(room.mapPackage->files);
    broadcastLobby(room);
}
void Server::modMsg(Client& c,const Frame& f) {
    Room* room=roomOf(c);if(!room)return;
    // Only the host of a skirmish room with game mods on (every lobby room; a debug
    // --mods launch may turn them off) may upload game packs.
    // Anyone else (a guest, or any player of a campaign mission room) is refused
    // outright, with an error rather than silence, so a modified client learns it.
    if(f.kind==Msg::ModOffer || f.kind==Msg::ModChunk) {
        const char* refused=!room->mission.empty() ? "game mods are not allowed in campaigns"
            : room->hostId!=c.id ? "only the host selects game mods"
            : !room->opts.gameMods && f.kind==Msg::ModOffer ? "game mods are off in this game" : nullptr;
        if(refused) {
            if(room->hostId!=c.id)c.modReceive={};
            // One answer (and one log line, within the shared log budget) per
            // client every 5 s: a stream of refused chunks costs no more than one.
            const uint64_t now=nowMs();
            if(!c.modRefusedAt || now-c.modRefusedAt>=5000) {
                c.modRefusedAt=now;
                Writer error;error.u32(room->id);error.str(refused);c.conn.send(Msg::ModError,error);
                if(commandLogs_.take(now,4))
                    std::fprintf(stderr,"game %u: refused game-mod upload from client %u: %s\n",room->id,c.id,refused);
            }
            return;
        }
    }
    if(!room->mission.empty())return;
    try {
        Reader rd(f.payload.data(),f.payload.size());
        if(f.kind==Msg::ModOffer){
            if(room->running || c.modReceive.size || c.modValidationId)return;
            const auto id=rd.u32();const auto label=rd.str(),digest=rd.str();const auto count=rd.u32();
            if(!rd.ok || rd.p!=rd.end || id!=room->id || label!="mods")throw std::runtime_error("invalid game mod offer");
            if(count>ta::net::maps::kMaxBytes || uint64_t(mapMemory())+uint64_t(count)*2>limits_.mapMemory)throw std::runtime_error("server game mod memory budget reached");
            // Other packs than the verified ones (the host ticked a pack, or a successor host
            // offers its own) change the game: the guests ready again.
            if(!room->modDigest.empty() && digest!=room->modDigest) {
                const int unreadied=bumpRules(*room,kChangeMods);
                std::fprintf(stderr,"game %u: host changed the game mods (rev %u, %d unreadied)\n",room->id,room->roomRev,unreadied);
            }
            c.modReceive.begin(id,0,count,digest);c.modStarted=c.modProgress=nowMs();
            room->modData.reset();room->modPackage.reset();room->modDigest.clear();
            resetRoomMap(*room);
            for(auto& [pid,peer]:clients_)if(peer->roomId==id){peer->modReady.clear();peer->modOffered.clear();peer->modSend={};if(pid!=c.id){peer->modReceive={};peer->modValidationId=0;}}
            broadcastLobby(*room);
            validatePackage(c,*room,true,true);
        }else if(f.kind==Msg::ModChunk){
            if(room->running || !room->opts.gameMods)return;   // a late chunk after GAME MODS went off
            const auto got=c.modReceive.append(rd);
            if(got==ta::net::maps::Append::Stale)return;   // an earlier transfer's
            c.modProgress=nowMs();
            if(got==ta::net::maps::Append::Done)validatePackage(c,*room,true,false);
        }else if(f.kind==Msg::ModRequest){
            // Starts the upload, or starts it over from offset 0 for a newer request (xfer).
            auto id=rd.u32();auto digest=rd.str();auto xfer=rd.u32();
            if(!rd.ok || rd.p!=rd.end || id!=room->id || !room->modPackage || digest!=room->modPackage->digest)return;
            c.modSend={id,xfer,0,room->modPackage};
        }else if(f.kind==Msg::ModReady){
            auto id=rd.u32();auto digest=rd.str();
            if(!rd.ok || rd.p!=rd.end || id!=room->id)throw std::runtime_error("game mod verification failed");
            if(!room->modPackage || digest!=room->modPackage->digest)return;
            c.modReady=digest;if(!room->running)broadcastLobby(*room);
        }else if(f.kind==Msg::ModError){
            auto id=rd.u32();auto why=rd.str();if(rd.ok && rd.p==rd.end && id==room->id && why.size()<=512){c.modReady.clear();c.modSend={};c.modReceive={};}
        }
    }catch(const std::exception& e){
        c.modValidationId=0;c.modReceive={};c.modSend={};c.modReady.clear();
        Writer error;error.u32(room->id);error.str(e.what());c.conn.send(Msg::ModError,error);
    }
}

void Server::mapMsg(Client& c, const Frame& f) {
    Room* room = roomOf(c); if (!room) return;
    try {
        Reader rd(f.payload.data(), f.payload.size());
        if (f.kind == Msg::MapOffer) {
            if (room->running || room->hostId != c.id) return;
            auto id = rd.u32(); auto name = rd.str(), hash = rd.str(); auto size = rd.u32();
            if (!rd.ok || rd.p != rd.end || id != room->id ||
                !room->mission.empty() || ta::mapgen::isGeneratedMapId(name))
                throw std::runtime_error("invalid map offer");
            // An offer of a map the room no longer plays (sent before the host saw its change).
            if (name != room->mapId) return;
            if (room->mapPackage) {
                // Already verified: a host whose transfer state was lost (its offer crossed a
                // change it later saw undone) is told so again, and reaches MAP READY.
                if (hash == room->mapPackage->digest) {
                    c.mapOfferedRoom = room->id;
                    c.conn.send(Msg::MapOffer, ta::net::maps::offer(room->id, room->mapId, *room->mapPackage));
                }
                return;
            }
            if (c.mapReceive.size || c.mapValidationId) return;
            // Clients pipeline their map offer behind game-mod chunks. Hold one
            // bounded offer until the asynchronous game-mod validation finishes.
            if(c.modValidationId || c.modReceive.size) {
                if(f.payload.size()>2048)throw std::runtime_error("map offer too large");
                c.deferredMapOffer=f;return;
            }
            // Reserve the advertised upload, not only bytes received so far.
            // Decoding keeps both the canonical package and extracted files.
            if(size>ta::net::maps::kMaxBytes || uint64_t(mapMemory())+uint64_t(size)*2>limits_.mapMemory)
                throw std::runtime_error("server map memory budget reached");
            c.mapReceive.begin(id, 0, size, hash);
            c.mapReceiveStarted=c.mapReceiveProgress=nowMs();
            // Installed maps follow the same content-addressed upload path on
            // a cache miss; never rebuild arbitrary maps on the network thread.
            validatePackage(c,*room,false,true);
        } else if (f.kind == Msg::MapChunk) {
            if (room->running || room->hostId != c.id || room->mapPackage) return;
            const auto got = c.mapReceive.append(rd);
            if (got == ta::net::maps::Append::Stale) return;   // an earlier transfer's (the map changed)
            c.mapReceiveProgress=nowMs();
            if (got == ta::net::maps::Append::Done) validatePackage(c,*room,false,false);
        } else if (f.kind == Msg::MapRequest) {
            // A guest asks for the verified package: the upload starts, or starts over from
            // offset 0 for a newer request (xfer). A request for another digest is stale.
            auto id = rd.u32(); auto digest = rd.str(); auto xfer = rd.u32();
            if (!rd.ok || rd.p != rd.end || id != room->id || !room->mapPackage || digest != room->mapPackage->digest) return;
            c.mapSend = {room->id, xfer, 0, room->mapPackage};
        } else if (f.kind == Msg::MapReady) {
            auto id = rd.u32(); auto hash = rd.str();
            if (!rd.ok || id != room->id)throw std::runtime_error("map verification failed");
            if(!room->mapPackage || hash!=room->mapPackage->digest)return;
            c.mapReadyRoom = room->id;
            if (!room->running) broadcastLobby(*room);
        } else if (f.kind == Msg::MapError) {
            auto id = rd.u32(); auto why = rd.str();
            if (rd.ok && rd.p==rd.end && why.size()<=512 && id == room->id) {
                c.mapValidationId=0;c.mapReadyRoom = 0; c.mapReceive = {}; c.mapSend = {};
                Writer chat; chat.str("SERVER"); chat.str(c.name + ": " + why);
                broadcastRoom(*room, Msg::Chat, chat);
            }
        }
    } catch (const std::exception& e) {
        c.mapValidationId=0;c.mapReadyRoom = 0; c.mapReceive = {}; c.mapSend = {};
        Writer error; error.u32(room->id); error.str(e.what()); c.conn.send(Msg::MapError, error);
    }
}

void Server::refuseStart(Client& c, const Room& r, const std::string& why) {
    Writer w; w.u32(r.id); w.str(why);
    c.conn.send(Msg::MapError, w);
    std::fprintf(stderr, "game %u: start refused -- %s\n", r.id, why.c_str());
}

// The room's game-mod and map-package files, where its AI reads its own ai/ files
// (null when the room plays without that layer).
static const ta::hpi::Vfs::Files* roomModFiles(const Room& r) {
    return r.modData && r.modPackage && r.modPackage->files ? r.modPackage->files.get() : nullptr;
}
static const ta::hpi::Vfs::Files* roomMapFiles(const Room& r) {
    return r.mapPackage && r.mapVfs && r.mapPackage->files ? r.mapPackage->files.get() : nullptr;
}

// Our levels' profiles for a room (Room::aiProfiles): the room's one ai/taengine.txt
// (its game mods' copy, else its map package's), read once and applied over each
// level's own built-in table; logged once when there is one.
static void loadAiProfiles(Room& r) {
    bool fromMap = false;
    const auto* text = ta::ai::engineProfileText(roomModFiles(r), roomMapFiles(r), &fromMap);
    for (uint8_t level = 0; level < ta::ai::kAiLevels; ++level)
        r.aiProfiles[level] = ta::ai::levelProfileFrom(ta::ai::Difficulty(level), text);
    if (text)
        std::fprintf(stderr, "game %u: our AI levels play their built-in profiles adjusted by "
                     "the %s %s\n", r.id, fromMap ? "map package's" : "game mods'", ta::ai::kEngineProfileFile);
}

// The Classic AI's profile script for a room: retail's file choice (ai/<aiprofile>.txt,
// else ai/default.txt), each file taken from the room's game mods, else its map
// package, else the base data (a game mod wins over the map for ai/ files, whatever
// the view's precedence for other map content), so a map, campaign or game mod that
// carries its own file plays it. A view with no map package (a mission, a generated
// map) skips the install's Maps/ archives: an AI file in a .kmp belongs to that map
// and reaches a game only in the map's package. Logged once per game with the layer
// the file came from.
static ta::ai::ClassicProfileScript classicProfileFor(const Room& r, const std::string& key,
                                                        const ta::hpi::Vfs& vfs, bool skipMapArchives) {
    const ta::hpi::Vfs view(&vfs, skipMapArchives);
    ta::ai::ClassicProfileLayer layer;
    auto script = ta::ai::classicProfileScript(view, key, roomModFiles(r), roomMapFiles(r), &layer);
    const char* from = layer == ta::ai::ClassicProfileLayer::GameMods ? "the game mods"
                     : layer == ta::ai::ClassicProfileLayer::Map      ? "the map package"
                     : layer == ta::ai::ClassicProfileLayer::Base     ? "the base data" : "";
    std::fprintf(stderr, "game %u: DEFAULT AI profile %s%s%s\n", r.id,
                 script.path.empty() ? "(none: every type 100, unlimited)" : script.path.c_str(),
                 *from ? " from " : "", from);
    return script;
}

void Server::tryStart(Client& c) {
    const uint64_t blockedFrom = nowMs();   // see the clock shift before GameStarting
    Room* r = roomOf(c);
    if (!r || r->hostId != c.id || r->running) return;
    // The server's limit on running games refuses this START and nothing more: the room
    // stays as it is, and its host may press START again later.
    if(uint64_t(runningGames())>=limits_.running) {
        refuseStart(c,*r,"server running-game capacity reached; retry later");return;
    }
    if (!mapsReady(*r)) {
        Writer w; w.u32(r->id); w.str("waiting for every player and the server to verify the map");
        c.conn.send(Msg::MapError, w); return;
    }
    // Validate: >=2 used slots, every human ready, unique colors among used slots.
    // A game that plays its map's scenario (MAP SCENARIO, or a Test Map copy's opt-in)
    // may be a solo objective. Both are checked against the VERIFIED package, never the
    // host's claim (scenarioInfo reads the package's own .ota/.crt). Campaign missions
    // retain their separate minimum-player exemption.
    const auto scenario = r->mapPackage ? ta::net::maps::scenarioInfo(*r->mapPackage) : ta::sim::ScenarioInfo{};
    if (r->opts.mapScenario && !(scenario.runnable() && scenario.content())) {
        refuseStart(c, *r, "this map has no scenario to play -- turn MAP SCENARIO off");
        return;
    }
    // setupMatch's own gate, read from the package: every scenario file it will read must
    // be IN the verified package. A file the package leaves out would come from each
    // machine's own install (a game mod pack can supply a map's .tdf), which the package
    // digest does not cover -- so the referee and the peers could play different rules.
    // (A damaged or misnamed file in the package is setupMatch's to refuse, by name.)
    const bool setupReads = r->mission.empty() && !r->opts.stressTest && !r->opts.benchmark && r->mapPackage &&
                            (scenario.marker || (r->opts.mapScenario && scenario.hasScenario));
    if (setupReads && (scenario.status == ta::sim::ScenarioStatus::CrtMissing || scenario.useOnlyMissing)) {
        refuseStart(c, *r, std::string("the map package leaves out its ") +
                    (scenario.useOnlyMissing ? "Use Only list" : "scenario file (.crt)"));
        return;
    }
    const bool scenarioOn = r->mission.empty() && !r->opts.stressTest && !r->opts.benchmark &&
                            r->mapPackage && scenario.active(r->opts.mapScenario != 0);
    if (r->mission.empty() && r->usedSlots() < (scenarioOn ? 1 : 2)) return;
    bool usedColor[10] = {};
    for (int i = 0; i < kMaxSlots; ++i) {
        const SlotInfo& s = r->slots[i];
        if (s.type != 1 && s.type != 2) continue;
        if (s.type == 1 && !s.ready) return;             // a human isn't ready
        if (s.color < 10) { if (usedColor[s.color]) return; usedColor[s.color] = true; }
    }
    // Every seat the scenario's placements and rules belong to must be played (the room
    // tags them and holds START; setupMatch stays the final backstop, same message).
    if (scenarioOn)
        for (int i = 0; i < kMaxSlots; ++i)
            if ((scenario.requiredSeats() >> i & 1) && r->slots[i].type != 1 && r->slots[i].type != 2) {
                refuseStart(c, *r, "Seat player " + std::to_string(i + 1) + " in the lobby to play this scenario");
                return;
            }
    // The referee is MANDATORY, so resolve what it needs before committing to run:
    // it holds the canonical hash and, for a mission, the strategic AI. A game that
    // cannot be refereed does not start; the host is told why.
    if(r->opts.gameMods && !r->modData) {
        if(!r->modPackage){refuseStart(c,*r,"waiting for game mods");return;}
        try {
            auto ds=std::make_unique<DataSet>();ds->vfs=ta::hpi::Vfs(&retail_.vfs);
            ds->vfs.setModFiles(r->modPackage->files);
            ta::sim::setupRegistry(ds->reg,ds->vfs);
            ds->hash=ta::hpi::gameplayHash(ds->vfs);
            ds->built=true;r->modData=std::move(ds);
            if(r->mapPackage){r->mapVfs=std::make_unique<ta::hpi::Vfs>(&r->modData->vfs);r->mapVfs->setMapFiles(r->mapPackage->files);}
        }catch(const std::exception& e){refuseStart(c,*r,std::string("cannot load game mods: ")+e.what());return;}
    }
    DataSet& dataSet = roomData(*r);
    const bool wantMission = !r->mission.empty();
    std::string mapResolved = wantMission ? std::string()
                                          : r->mapPackage ? r->mapPackage->mapPath
                                          : ta::hpi::findMap(dataSet.vfs, r->mapId);
    if (!wantMission && mapResolved.empty()) {
        char msg[192];
        std::snprintf(msg, sizeof msg,
                      "the server has no map '%s' in its game data, so it cannot "
                      "referee this game", r->mapId.c_str());
        sendReject(c, msg);
        std::fprintf(stderr, "game %u: refusing to start -- map '%s' not in server data\n",
                     r->id, r->mapId.c_str());
        return;
    }
    if (!wantMission && ta::mapgen::isGeneratedMapId(r->mapId)) {
        try { ta::net::maps::saveGenerated(ta::u8path(mapRoot_), dataSet.vfs, r->mapId,limits_.mapDisk); }
        catch (const std::exception& e) {
            Writer w; w.u32(r->id); w.str(std::string("cannot save generated map: ") + e.what());
            c.conn.send(Msg::MapError, w); return;
        }
    }
    r->running = true;
    r->hostSlot = c.slot;   // the host's powers stay with this seat (-1: a spectator host)
    r->tick = 0;
    std::fill(std::begin(r->orderSerialFloor), std::end(r->orderSerialFloor), 0u);
    r->orderRotor = 0;
    r->rebasePace(nowUs());
    for (int i = 0; i < kMaxSlots; ++i) r->startSlots[i] = r->slots[i];   // for the replay
    // Build the referee sim (and AI controllers) if we have game data. The world
    // is built by the SAME setupMatch the clients use, so its hash is canonical.
    // Everything the referee reads comes from the data set for THIS game (retail,
    // or retail + the room's game mods), so the referee's sim matches the clients
    // that mounted the same game mods.
    DataSet* ds = &dataSet;
    const bool isMission = wantMission;
    loadAiProfiles(*r);
    // A campaign mission builds its own world (placements + the in-sim god script) and
    // needs no map id; a skirmish resolves its map by name (resolved above, where a
    // failure refuses the start).
    const std::string& mapPath = mapResolved;
    {
        r->reg = &dataSet.reg;
        r->ref = std::make_unique<ta::sim::World>();
        // --sim-threads: this game's share as it starts (it is counted as running);
        // each tick then runs on the share of that moment (closeTick).
        r->ref->setSimWorkers(int(simShare(runningGames())) - 1);
        if (isMission) {
            // The client builds the SAME world (setupMission is deterministic), so the
            // referee and every peer stay in lockstep.
            int human = 0;
            ta::sim::MissionSetup ms;
            // (Debug) TA_FORCE_WIN / TA_FORCE_LOSE: the mission with its outcome forced
            // by its own scenario rules, as the client that started this server reads it.
            const auto forced = ta::forcedOutcomeMission(ds->vfs, r->mission);
            if (!ta::sim::setupMission(*r->ref, *r->reg, forced ? *forced : ds->vfs, r->mission, human, &ms)) {
                // Same rule as a missing map: no referee, no game.
                char msg[192];
                std::snprintf(msg, sizeof msg,
                              "the server has no mission '%s' in its game data, so it "
                              "cannot referee this game", r->mission.c_str());
                sendReject(c, msg);
                std::fprintf(stderr, "game %u: refusing to start -- mission '%s' not in "
                             "server data\n", r->id, r->mission.c_str());
                r->ref.reset();
                r->running = false;
                return;
            } else {
                // Give every "strategic opponent" a brain. The mission script places
                // and choreographs units, but nothing made those bases BUILD or
                // counterattack -- every one of the shipped missions declares at
                // least one strategic player, so they all played as static
                // set-pieces. The AI lives server-side and emits ordinary commands
                // into the tick stream, exactly as it does for a skirmish, so
                // lockstep is unaffected. A "passive neutral" gets nothing: it is
                // scenery. Mission opponents play retail's own AI at the mission's
                // difficulty (RETAIL EASY / CLASSIC (Medium) / RETAIL HARD): it bakes the
                // mission's aiprofile file (ai/<aiprofile>.txt, else ai/default.txt, by
                // the data set's precedence), reads its plan sections for that
                // difficulty word (0x406c90, G+0x37eee), and plays the mission as
                // retail's scenario mode 1, massattack included. Its income scale is
                // set by setupTaMission from the same difficulty.
                const auto missionDiff = ms.difficulty == 0 ? ta::ai::Difficulty::RetailEasy
                                       : ms.difficulty == 2 ? ta::ai::Difficulty::RetailHard
                                                            : ta::ai::Difficulty::Classic;
                const ta::ai::Profile& prof = r->aiProfiles[size_t(missionDiff)];
                const auto classicProfile = classicProfileFor(*r, ms.aiProfile, ds->vfs, true);
                for (int slot : ms.aiSlots) {
                    r->ai.emplace_back(slot, *r->reg, prof, r->seed + uint32_t(slot), missionDiff);
                    r->ai.back().setClassicProfile(classicProfile);
                    r->ai.back().setCampaign(true);
                }
                r->classicProfile = classicProfile;
                r->slotPos = ms.slotPos;
                if (!ms.aiSlots.empty())
                    std::fprintf(stderr, "taserver: mission '%s' -- %zu strategic AI player(s)%s DEFAULT AI (%s)\n",
                                 r->mission.c_str(), ms.aiSlots.size(),
                                 ms.aiProfile.empty() ? "" : (" profile=" + ms.aiProfile).c_str(),
                                 classicProfile.path.empty() ? "no profile" : classicProfile.path.c_str());
            }
        } else {
            // The same builder every client mirrors (net/matchconfig.h): the Absurd and
            // Absurdly Defensive income, the defensive stance and the player count all
            // come from it.
            ta::sim::MatchConfig cfg = ta::net::matchConfigFor(r->opts, r->slots, r->seed);
            cfg.vfs = r->mapVfs ? r->mapVfs.get() : &ds->vfs;
            cfg.mapPath = mapPath;
            const int maxSlot = int(cfg.slots.size()) - 1;
            std::vector<std::pair<float,float>> spots;
            try { spots = ta::sim::setupMatch(*r->ref, *r->reg, cfg); }
            catch (const ta::sim::ScenarioTypeMissing& e) {
                // Named for the player.
                r->ref.reset(); r->running = false; r->ai.clear();
                refuseStart(c, *r, ta::sim::scenarioTypeMissingText(e));
                return;
            }
            catch (const std::exception& e) {
                r->ref.reset(); r->running = false; r->ai.clear();
                refuseStart(c, *r, e.what());
                return;
            }
            // setupMatch returns start positions in USED-slot order, each the position that
            // slot opened at under the room's Start Locations (FIXED, RANDOM or BY SLOT);
            // remap to slot index.
            std::vector<std::pair<float, float>> slotPos(size_t(maxSlot + 1), {0.f, 0.f});
            for (int i = 0, k = 0; i <= maxSlot; ++i)
                if (r->slots[i].type == 1 || r->slots[i].type == 2) {
                    if (k < int(spots.size())) slotPos[size_t(i)] = spots[size_t(k)];
                    ++k;
                }
            r->slotPos = slotPos;
            r->ai.reserve(size_t(maxSlot + 1));
            // The Classic level bakes retail's profile from the map's own aiprofile
            // file (every shipped skirmish map names DEFAULT); read once per game.
            // Every level runs the TA brain and reads the map's profile; a seat the
            // AI takes over later (takeOver) is set up from the same one.
            const auto classicProfile = classicProfileFor(*r, ta::ai::classicMapAiProfile(*cfg.vfs, mapPath),
                                                          *cfg.vfs, !r->mapVfs);
            for (int i = 0; i <= maxSlot; ++i)
                if (r->slots[i].type == 2) {
                    auto diff = ta::ai::difficultyFromLevel(r->slots[i].aiLevel);
                    r->ai.emplace_back(i, *r->reg, r->aiProfiles[size_t(diff)], r->seed, diff);
                    r->ai.back().setClassicProfile(classicProfile);
                }
            r->classicProfile = classicProfile;
            if (r->ref->hasScenarioOutcomes())
                std::fprintf(stderr, "game %u: plays its map scenario (%s) -- %zu units at the start\n", r->id,
                             r->opts.mapScenario ? "MAP SCENARIO" : "Test Map copy", r->ref->units().size());
        }
    }
#ifndef NDEBUG
    // Test hook: make the referee build slow (a huge map, a slow host) without a huge map.
    if (const char* d = std::getenv("TA_TEST_REFEREE_DELAY_MS"))
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, std::atoi(d))));
#endif
    // Everything above (package/map verification, AI profiles, the referee world build)
    // ran on this thread without reading any socket, so no connection's last-heard time
    // can have advanced meanwhile. Silence is the CLIENT's: move every clock forward by
    // the time this thread was busy, or a build longer than the timeout drops the whole
    // room (and anyone else on the server) the moment the game starts. Shifted by the
    // stall, not reset to now: a dead client gains only the server's own busy time, so
    // games starting over and over elsewhere cannot keep it alive.
    const uint64_t now = nowMs(), blocked = now - blockedFrom;
    for (auto& [cid, cl] : clients_) cl->lastRecvMs = std::min(cl->lastRecvMs + blocked, now);
    // GameStarting: final slot table + options + seed + a per-slot resume token
    // (used to reconnect to the seat inside the disconnect wait).
    for (int i = 0; i < kMaxSlots; ++i) {
        r->waitDeadline[i] = r->waitSpentMs[i] = r->takenToken[i] = 0;
        if (r->slotClient[i] < 0) continue;
        r->slotToken[i] = randToken();
        auto it = clients_.find(uint32_t(r->slotClient[i]));
        if (it == clients_.end()) continue;
        sendGameStarting(*it->second, *r, uint8_t(i), r->slotToken[i], /*catchUp=*/false);
        it->second->loaded = false;
    }
    // Spectators (incl. a host who created the game to watch) get a slot-less
    // GameStarting; they don't gate the first tick, so mark them loaded.
    for (uint32_t sid : r->spectators) {
        auto it = clients_.find(sid);
        if (it == clients_.end()) continue;
        sendGameStarting(*it->second, *r, 0xFF, 0, /*catchUp=*/false);
        it->second->loaded = true;
    }
    for(int i=0;i<std::min(kMaxSlots,r->ref->numPlayers());++i)r->defeated[size_t(i)]=r->ref->player(i).defeated;
    std::fprintf(stderr, "game %u starting with %d players\n", r->id, r->usedSlots());
    // What its referee sim runs on: its share of --sim-threads, as every running
    // game's now is. The main loop logs the share again whenever it changes.
    logSimThreads(runningGames());
}

// The host changes the room's map, MAP SCENARIO or start mode (Msg::SetRoomMap). Every
// well-formed request with a new serial is answered: by the LobbyState that carries the
// serial, after a RoomError when refused (and then nothing else changes).
void Server::setRoomMap(Client& c, Room& r, const Frame& f) {
    // The host (seated or watching), in the lobby; a mission's or Benchmark's map is its own.
    if (r.hostId != c.id || r.running || !r.mission.empty() || r.opts.benchmark) return;
    Reader rd(f.payload.data(), f.payload.size());
    const uint32_t serial = rd.u32();
    const std::string mapId = rd.str();
    const uint8_t capByte = rd.u8(), scenario = rd.u8(), starts = rd.u8();
    if (!rd.ok || rd.p != rd.end) return;
    if (serial <= r.mapSerial) return;   // a duplicate, or older than one already answered
    r.mapSerial = serial;
    const auto refuse = [&](const std::string& why) {
        roomRefuse(c, r, RoomRequest::Map, serial, why);
        std::fprintf(stderr, "game %u: map change refused -- %s\n", r.id, why.c_str());
    };
    if (mapId.empty() || mapId.size() > 4096) { refuse("no map was given"); return; }
    // A random map's capacity is its player count, read from the id; a file map's is the
    // host's count of its starts (the server has no file map before its package verifies),
    // trusted as CreateGame's is.
    const bool generated = ta::mapgen::isGeneratedMapId(mapId);
    int cap = std::clamp<int>(capByte, 2, kMaxSlots);
    if (generated) {
        try { cap = std::clamp<int>(ta::mapgen::parseMapId(mapId).players, 2, kMaxSlots); }
        catch (const std::exception& e) { refuse(e.what()); return; }
    }
    const uint8_t mapScenario = scenario && !generated && !r.opts.stressTest && !r.opts.benchmark ? 1 : 0;
    // The host's start mode applies whenever the scenario ends up off.
    const uint8_t randomStarts = mapScenario ? kStartsFixed : normalizeStartMode(starts);
    if (mapId == r.mapId && mapScenario == r.opts.mapScenario && randomStarts == r.opts.randomStarts) {
        Writer ls; writeSlots(ls, r); c.conn.send(Msg::LobbyState, ls);   // nothing to change
        return;
    }
    int humans = 0;
    for (const auto& s : r.slots) humans += s.type == 1;
    if (humans > cap) {
        refuse("this map has " + std::to_string(cap) + " starts and " + std::to_string(humans) + " players are seated");
        return;
    }
    const bool mapChanged = mapId != r.mapId, scenarioChanged = mapScenario != r.opts.mapScenario;
    const bool startsChanged = randomStarts != r.opts.randomStarts;
    // Every guest readies again (counted before the capacity moves any of them).
    const int unreadied = bumpRules(r, 0);
    // A new map has no package yet: the host's client offers it on the LobbyState below.
    if (mapChanged) { resetRoomMap(r); r.mapId = mapId; }
    r.opts.mapScenario = mapScenario;
    r.opts.randomStarts = randomStarts;
    std::vector<uint32_t> moved;
    const auto seats = applyCapacity(r, cap, moved);
    r.changeBits = uint8_t((mapChanged ? kChangeMap : 0) | (scenarioChanged ? kChangeScenario : 0) |
                           (!mapChanged && !scenarioChanged && startsChanged ? kChangeOptions : 0) |
                           (seats.changed ? kChangeCapacity : 0));
    // A player the capacity moved learns its new seat before the table that shows it there.
    for (uint32_t id : moved)
        if (const auto it = clients_.find(id); it != clients_.end()) sendJoin(*it->second, true, it->second->slot, "", &r);
    broadcastLobby(r);
    char detail[160];
    std::snprintf(detail, sizeof detail, "capacity %d, scenario %d, %d AI removed, %d moved, %d unreadied", cap,
                  mapScenario, seats.aiRemoved, seats.movedHumans, unreadied);
    if (mapChanged) std::fprintf(stderr, "game %u: host changed the map to '%s' (%s)\n", r.id, r.mapId.c_str(), detail);
    else if (scenarioChanged) std::fprintf(stderr, "game %u: host turned MAP SCENARIO %s (%s)\n", r.id, mapScenario ? "on" : "off", detail);
    else std::fprintf(stderr, "game %u: host changed the rules (starts; rev %u, %d unreadied)\n", r.id, r.roomRev, unreadied);
}

// The host renames its room or changes its password (Msg::SetRoomInfo). Neither changes
// the game: nobody is unreadied, and the players already in it stay.
void Server::setRoomInfo(Client& c, Room& r, const Frame& f) {
    if (r.hostId != c.id || r.running) return;
    Reader rd(f.payload.data(), f.payload.size());
    const uint8_t flags = rd.u8();
    std::string name = rd.str();
    const std::string password = rd.str();
    if (!rd.ok || rd.p != rd.end || !(flags & (kRoomInfoName | kRoomInfoPassword))) return;
    const auto refuse = [&](const std::string& why) { roomRefuse(c, r, RoomRequest::Info, 0, why); };
    if (flags & kRoomInfoName) {
        const auto first = name.find_first_not_of(' '), last = name.find_last_not_of(' ');
        name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
        if (name.empty() || name.size() > kRoomNameMax || !printableAscii(name)) {
            refuse("the game name must be 1 to " + std::to_string(kRoomNameMax) + " plain characters");
            return;
        }
    }
    if ((flags & kRoomInfoPassword) && (password.size() > kRoomPasswordMax || !printableAscii(password))) {
        refuse("the password must be at most " + std::to_string(kRoomPasswordMax) + " plain characters");
        return;
    }
    // Only the flagged fields: a successor host never knew the password, and a rename must
    // not clear it.
    if (flags & kRoomInfoName) {
        r.name = name;
        std::fprintf(stderr, "game %u: renamed to '%s'\n", r.id, r.name.c_str());
    }
    if (flags & kRoomInfoPassword) {
        r.password = password;
        std::fprintf(stderr, "game %u: password %s\n", r.id, r.password.empty() ? "cleared" : "set");
    }
    broadcastLobby(r);
}

void Server::gameMsg(Client& c, const Frame& f) {
    Room* r = roomOf(c);
    if (!r) return;
    switch (f.kind) {
        case Msg::SlotUpdate: {
            Reader rd(f.payload.data(), f.payload.size());
            int slot = int(rd.u8());
            uint8_t type = rd.u8(), faction = rd.u8(), color = rd.u8(), team = rd.u8(), ready = rd.u8();
            uint8_t aiLevel = rd.u8();
            const uint32_t seenRev = rd.u32();   // the room revision the sender last saw
            if (!rd.ok || slot < 0 || slot >= kMaxSlots || r->running) return;
            bool isHost = (r->hostId == c.id);
            // A player edits only their own slot; the host may edit any.
            if (!isHost && slot != c.slot) return;
            SlotInfo& s = r->slots[slot];
            const SlotInfo before = s;
            // Only the server seats a human (JoinGame, MoveSeat, leaving), so in the
            // lobby a HUMAN slot is exactly one with a client. A host edit never
            // changes a seated slot's type nor makes an empty one HUMAN: an edit sent
            // before a seat move reached the sender (its table still showing the old
            // seats) arrives as exactly that, and is dropped whole -- otherwise the
            // host's left seat became a clientless ready HUMAN, or a seat a player had
            // just taken became AI under them.
            if (isHost && (r->slotClient[slot] >= 0 ? type != s.type : type == 1)) return;
            // Non-host can only touch faction/color/team/ready on their own slot.
            // Slots past the map capacity stay closed (no start position for them).
            if (isHost) {
                if (type <= 3 && !(slot >= r->cap && type != 3)) {
                    uint8_t old = s.type;
                    s.type = type;
                    if (type == 2 && old != 2) assignAiName(*r, slot);  // new AI: random name
                    else if (type != 2 && old == 2) s.name.clear();     // no longer AI
                }
            }
            s.faction = uint8_t(faction % std::max(1, ta::sides::count()));
            s.color = color % 10;
            s.team = uint8_t(team % kMaxSlots);
            // READY: only a seat's own player changes it, never the host for someone else; a
            // guest's READY counts only for the room it saw (a change it had not seen yet
            // would have it ready for a game it never looked at). An OPEN or CLOSED seat is
            // never ready.
            const int owner = r->slotClient[slot];
            if (s.type == 0 || s.type == 3) s.ready = 0;
            else if (owner >= 0 && uint32_t(owner) != c.id) {}   // the host editing a guest's seat
            else if (s.type == 1 && owner != int(r->hostId)) s.ready = ready && seenRev == r->roomRev ? 1 : 0;
            else s.ready = ready ? 1 : 0;
            // 0=passive 1=easy 2=normal 3=hard 4=absurd 5=absurdly defensive 6=classic (DEFAULT)
            s.aiLevel = aiLevel >= ta::ai::kAiLevels ? 2 : aiLevel;
            // Any seat edit of the host's but its own READY is a change the guests ready again for.
            SlotInfo unready = before; unready.ready = s.ready;
            if (isHost && (unready.type != s.type || unready.faction != s.faction || unready.color != s.color ||
                           unready.team != s.team || unready.aiLevel != s.aiLevel || unready.name != s.name)) {
                const int unreadied = bumpRules(*r, kChangePlayers);
                std::fprintf(stderr, "game %u: host changed the players (rev %u, %d unreadied)\n", r->id, r->roomRev, unreadied);
            }
            broadcastLobby(*r);
            break;
        }
        case Msg::MoveSeat: {
            // A seated human takes another seat: only an OPEN one inside the map's
            // capacity, only in the lobby. Under START LOCATIONS: BY SLOT the seat is
            // the start position, and under FIXED the seat order decides it, so this
            // is how a player picks where they open. Never in a campaign mission (the
            // mission says which seat is the player's).
            Reader rd(f.payload.data(), f.payload.size());
            const int to = int(rd.u8());
            if (!rd.ok || rd.p != rd.end || r->running || !r->mission.empty()) return;
            const bool isHost = r->hostId == c.id;
            const int from = c.slot;
            const bool seated = from >= 0 && from < kMaxSlots && r->slotClient[from] == int(c.id);
            if (to == kWatchSeat) {
                // The seated host leaves its seat to watch (single player's WATCH). ALLOW
                // SPECTATORS does not apply: it governs who ELSE may watch.
                if (!isHost || !seated) return;
                SlotInfo& s = r->slots[from];   // its colour and team stay with the open seat
                s.type = 0; s.name.clear(); s.ready = 0;
                r->slotClient[from] = -1;
                r->spectators.push_back(c.id);
                c.slot = -1;
                const int unreadied = bumpRules(*r, kChangePlayers);
                sendJoin(c, true, kWatchSeat, "", r);
                broadcastLobby(*r);
                std::fprintf(stderr, "client %u now watches game %u (rev %u, %d unreadied)\n", c.id, r->id, r->roomRev, unreadied);
                break;
            }
            if (to < 0 || to >= kMaxSlots || to == from || to >= r->cap) return;
            if (r->slots[to].type != 0 || r->slotClient[to] >= 0) return;
            if (!seated) {
                // A watching host sits down again; nobody else in the room is a lobby
                // spectator with a seat to take.
                const auto watching = std::find(r->spectators.begin(), r->spectators.end(), c.id);
                if (!isHost || from != -1 || watching == r->spectators.end()) return;
                r->spectators.erase(watching);
                SlotInfo& s = r->slots[to];   // the seat's own side, colour and team
                s.type = 1; s.name = c.name; s.ready = 0;
                r->slotClient[to] = int(c.id);
                c.slot = to;
                const int unreadied = bumpRules(*r, kChangePlayers);
                sendJoin(c, true, to, "", r);
                broadcastLobby(*r);
                std::fprintf(stderr, "client %u takes slot %d in game %u (rev %u, %d unreadied)\n", c.id, to, r->id,
                             r->roomRev, unreadied);
                break;
            }
            // The player's side, colour, team and READY travel with them; the seat
            // they leave opens with the target's old colour and team, so the room's
            // defaults (slot N: colour N, team N) stay distinct.
            std::swap(r->slots[from], r->slots[to]);
            r->slots[from].type = 0; r->slots[from].name.clear(); r->slots[from].ready = 0;
            r->slotClient[to] = int(c.id); r->slotClient[from] = -1;
            c.slot = to;
            // The host moving is a change of the room's players.
            if (isHost) bumpRules(*r, kChangePlayers);
            // The mover learns its new seat the way it learned the first one, before
            // the table that shows it there.
            sendJoin(c, true, to, "", r);
            broadcastLobby(*r);
            std::fprintf(stderr, "client %u moved from slot %d to slot %d in game %u\n", c.id, from, to, r->id);
            break;
        }
        case Msg::Kick: {
            if (r->hostId != c.id || r->running) return;
            Reader rd(f.payload.data(), f.payload.size());
            int slot = int(rd.u8());
            if (!rd.ok || slot < 0 || slot >= kMaxSlots) return;
            int cid = r->slotClient[slot];
            if (cid >= 0 && uint32_t(cid) != c.id) {
                auto it = clients_.find(uint32_t(cid));
                if (it != clients_.end()) {
                    leaveRoom(*it->second, "kicked"); it->second->conn.send(Msg::Bye, Writer{});
                    // A change of the room's players: the guests left ready again.
                    const int unreadied = bumpRules(*r, kChangePlayers);
                    broadcastLobby(*r);
                    std::fprintf(stderr, "game %u: host changed the players (rev %u, %d unreadied)\n", r->id, r->roomRev, unreadied);
                }
            }
            break;
        }
        case Msg::SetPause: {
            // A player asking to pause. Only the host may (in single-player the host
            // IS the only human), and only in a running game. Unlike the disconnect
            // wait this one never runs out: nobody has disconnected.
            if (!r || !r->running || r->hostId != c.id) return;
            Reader rd(f.payload.data(), f.payload.size());
            bool want = rd.u8() != 0;
            if (want && !r->paused) {
                r->paused = true;
                Writer pw; pw.u8(1); pw.u8(0);
                broadcastRoom(*r, Msg::Pause, pw);
                std::fprintf(stderr, "game %u: paused by request\n", r->id);
            } else if (!want && r->paused) {
                r->paused = false;
                if (!r->held()) r->rebasePace(nowUs());
                Writer rw; rw.u8(1); rw.u8(0);
                broadcastRoom(*r, Msg::Resume, rw);
                std::fprintf(stderr, "game %u: resumed by request\n", r->id);
            }
            break;
        }
        case Msg::SetGameOptions: {
            if (r->hostId != c.id) return;   // host only
            Reader rd(f.payload.data(), f.payload.size());
            GameOptions o; get(rd, o);
            if (!ta::sim::validPathfindingMode(uint8_t(o.pathfindingMode))) { sendReject(c,"invalid pathfinding mode"); return; }
            // Sanitise the raw options (gameMods and speed are handled below).
            o.unitCap = clampUnitCap(o.unitCap); o.monarchExpendable = o.monarchExpendable ? 1 : 0;
            o.stressTest = std::min<uint8_t>(o.stressTest, kStressStaged);
            o.fogExplored = std::min<uint8_t>(o.fogExplored, 2);   // 0/1/2, see CreateGame
            o.randomStarts = normalizeStartMode(o.randomStarts);   // as CreateGame
            o.doubleSight = o.doubleSight ? 1 : 0;
            // ALLOW SPECTATORS is adopted here in the lobby only: the running branch
            // below takes nothing but speed, so it is fixed once the game starts and a
            // spectator admitted while it was on is never stranded by a later change.
            // The only lobby spectator is a host who created the room to watch, whom
            // the rule never removes (see Spectate).
            o.allowSpectators = o.allowSpectators ? 1 : 0;
            o.powerCodes = o.powerCodes ? 1 : 0;
            o.losType = o.losType ? 1 : 0;
            // MAP SCENARIO changes only with the map (SetRoomMap): the capacity it implies
            // moves seats, which a rules change does not.
            o.mapScenario = r->opts.mapScenario;
            if (o.stressTest || o.benchmark) o.mapScenario = 0;   // as CreateGame clears it
            if (o.mapScenario) o.randomStarts = kStartsFixed;     // ownership and rule regions follow fixed seats
            if (!r->mission.empty()) o.doubleSight = 0;
            if (!r->mission.empty() || privateSinglePlayer()) o.speedUnlock = 1;   // as CreateGame
            if (!rd.ok || rd.p!=rd.end) return;
            if(!(testWork_ || (loopbackOnly_ && !requireAuth_)) && (o.stressTest || o.benchmark)) {sendReject(c,"public benchmark/stress requests disabled");return;}
            o.speed = uint8_t(std::clamp<int>(o.speed, kMinGameSpeed, maxGameSpeed(o)));   // 0.1x .. 2x (a benchmark 11x)
            if (!r->running) {
                // Lobby: adopt the options (protocol 323 adoption table). A campaign mission's
                // rules are the mission's: only the speed and ALLOW SPECTATORS change there.
                if (!r->mission.empty()) {
                    const uint8_t speed = uint8_t(std::min<int>(o.speed, maxGameSpeed(r->opts))), spectators = o.allowSpectators;
                    o = r->opts; o.speed = speed; o.allowSpectators = spectators;
                }
                // A Benchmark room keeps the unit limit it was made with.
                if (r->opts.benchmark) o.unitCap = r->opts.unitCap;
                o.gameMods = o.gameMods ? 1 : 0;
                // Campaign mission rooms never run game mods.
                if(!r->mission.empty())o.gameMods=0;
                if(o.benchmark || r->opts.benchmark)o.gameMods=0;   // the Benchmark runs the plain retail data
                if(o.gameMods!=r->opts.gameMods){
                    r->modData.reset();r->modPackage.reset();r->modDigest.clear();
                    resetRoomMap(*r);
                    for(auto& [id,peer]:clients_)if(peer->roomId==r->id){peer->modReady.clear();peer->modOffered.clear();peer->modSend={};peer->modReceive={};peer->modValidationId=0;}
                }
                // Every rule but the speed is a change the guests ready again for.
                const std::string changed = optionsChanged(r->opts, o);
                r->opts = o;
                if (!changed.empty()) {
                    const int unreadied = bumpRules(*r, kChangeOptions);
                    std::fprintf(stderr, "game %u: host changed the rules (%s; rev %u, %d unreadied)\n", r->id,
                                 changed.c_str(), r->roomRev, unreadied);
                }
                broadcastLobby(*r);
            } else if (r->opts.speedUnlock && r->opts.speed != o.speed) {
                // In-game: only speed can change (re-cadences the sim), and only if the
                // game was created with speed-unlock. Tell every peer to re-pace.
                // Re-pace from the pending tick (never inheriting a deficit run up at the
                // old speed: see TickPacer::changeSpeed).
                r->opts.speed = o.speed;
                r->pace.changeSpeed(r->tick, int(o.speed), nowUs());
                Writer w; w.u8(o.speed);
                broadcastRoom(*r, Msg::SpeedUpdate, w);
            }
            break;
        }
        case Msg::StartGame: tryStart(c); break;
        case Msg::SetRoomMap: setRoomMap(c, *r, f); break;
        case Msg::SetRoomInfo: setRoomInfo(c, *r, f); break;
        case Msg::Loaded: {
            // The client reports its gameplay-data fingerprint with the room's game
            // mods mounted. With referee data we hold it to the room's canonical hash,
            // so a player missing (or differing on) a game mod is caught
            // here, before the first tick, instead of desyncing mid-game.
            Reader rd(f.payload.data(), f.payload.size());
            uint64_t clientHash = rd.u64();
            if (rd.ok && clientHash != 0) {
                uint64_t want = roomData(*r).hash;
                if (clientHash != want) {
                    char msg[176];
                    std::snprintf(msg, sizeof msg,
                        "game data mismatch (game mods %s: server %016llx, you %016llx) -- "
                        "you don't have the same game mods as the host",
                        r->opts.gameMods ? "on" : "off", (unsigned long long)want,
                        (unsigned long long)clientHash);
                    sendReject(c, msg);
                    c.conn.fail("gamedatahash");
                    std::fprintf(stderr, "game %u: client %u rejected at load: %016llx != %016llx (game mods %d)\n",
                        r->id, c.id, (unsigned long long)clientHash, (unsigned long long)want,
                        int(r->opts.gameMods));
                    return;
                }
            }
            c.loaded = true;
            // Tell the room who just finished loading, so everyone else's loading
            // screen can fill that player's bar instead of sitting at "waiting".
            {
                int slot = -1;
                for (int i = 0; i < kMaxSlots; ++i)
                    if (r->slotClient[i] == int(c.id)) { slot = i; break; }
                if (slot >= 0) {
                    Writer ps; ps.u8(uint8_t(slot)); ps.u8(2 /*loaded*/); ps.u32(0);
                    broadcastRoom(*r, Msg::PlayerStatus, ps);
                }
            }
            break;
        }
        case Msg::LoadProgress: {
            // A seated player still loading: pass its percent to the room's loading
            // screens (PlayerStatus status 3). Presentation only.
            Reader rd(f.payload.data(), f.payload.size());
            const uint8_t pct = rd.u8();
            if (!rd.ok || !r->running || c.loaded || c.slot < 0) break;
            Writer ps; ps.u8(uint8_t(c.slot)); ps.u8(3 /*loading*/); ps.u32(std::min<uint32_t>(pct, 100));
            broadcastRoom(*r, Msg::PlayerStatus, ps, c.id);
            break;
        }
        case Msg::LeaveGame: leaveRoom(c, "left"); break;
        case Msg::Chat: {
            Reader rd(f.payload.data(), f.payload.size());
            std::string text = rd.str();
            const SlotMask recipients = rd.u16();
            // Spectators read it only when the player ticks them (the Diplomacy
            // SPECTATORS row); before the game starts the whole room does. A
            // spectator in a running game cannot chat at all.
            const bool toSpectators = rd.u8() != 0;
            if (!rd.ok || text.size() > 512) return;
            Writer w; w.str(c.name); w.str(text);
            if (!r->running) broadcastRoom(*r, Msg::Chat, w);
            else if (c.slot < 0) break;   // spectators never chat in a running game
            else {
                for (int slot=0;slot<kMaxSlots;++slot) {
                    if (!(recipients & (1u<<slot))) continue;
                    auto found=clients_.find(uint32_t(r->slotClient[slot]));
                    if (found!=clients_.end()) found->second->conn.send(Msg::Chat,w.b);
                }
                if (toSpectators)
                    for (uint32_t cid : r->spectators)
                        if (auto found=clients_.find(cid); found!=clients_.end()) found->second->conn.send(Msg::Chat,w.b);
            }
            break;
        }
        case Msg::VoiceUp: relayVoice(c, *r, f); break;
        case Msg::PlayerCommands: {
            if (!r->running) return;
            if (c.slot < 0) { c.conn.fail("spectators cannot issue commands"); return; }
            Reader header(f.payload.data(),f.payload.size());
            const auto count=header.u32();
            if(!header.ok || count>f.payload.size()/35) {c.conn.fail("invalid player command count");return;}
            const auto now=nowMs();
            const bool allowed=header.ok && takeInputBudget(c,now,unsigned(f.payload.size()),count);
            // Queued orders (Cmd::Order items, server/orders.h) have limits of their own.
            const size_t ordinary=c.cmdQueue.size()-std::min(c.cmdQueue.size(),size_t(c.orders.queuedOrders()));
            if(!allowed || ordinary>kCmdQueueMax || count>kCmdQueueMax-ordinary) {
                if(!allowed) {c.commandBytes.window=now;c.commandBytes.used=4u<<20;}
                c.cmdDropped+=count;
                sendInputDropped(c,count,0);   // never acknowledged by a bundle: release them from the client's window
                if(now-c.commandLogAt>=5000 && commandLogs_.take(now,4)) {
                    c.commandLogAt=now;
                    std::fprintf(stderr,"client %u: command budget/queue exceeded; dropped %llu commands total\n",c.id,(unsigned long long)c.cmdDropped);
                }
                break;
            }
            std::vector<Command> parsed;
            if (!ta::srv::parsePlayerCommands(f.payload,parsed)) {
                c.conn.fail("invalid player commands");
                return;
            }
            for(auto& cmd:parsed) {
                // CHEAT CODES off: a code does nothing (TotalA.exe leaves group 2 out of the mask, 0x493e25, and skips the
                // handler), so it never reaches a bundle. Not a fault: the chat line
                // that carried it is ordinary chat.
                if(cmd.kind==ta::net::Cmd::PowerCode && !r->opts.powerCodes &&
                   !ta::sim::alwaysPowerCode(cmd.targetId)) continue;
                cmd.player=uint8_t(c.slot);
                c.cmdQueue.push_back(cmd);
            }
            break;
        }
        case Msg::OrderBegin:
        case Msg::OrderMembers:
        case Msg::OrderCommit:
        case Msg::OrderAbort: {
            if (!r->running) return;
            if (c.slot < 0) { c.conn.fail("spectators cannot issue orders"); return; }
            // Legion clients send their selections as orders; a Retail game keeps the
            // per-unit commands and their 64-a-tick drain (Retail stays as it was).
            if (r->opts.pathfindingMode != ta::sim::PathfindingMode::Legion) {
                c.conn.fail("explicit orders are for Legion games"); return;
            }
            if (!handleOrderFrame(c, *r, f)) {
                c.conn.fail("invalid order transaction");
                return;
            }
            break;
        }
        case Msg::Progress: {
            // Flow control only (WE S3): move the ack tick and size this client's lead. Never
            // hash-checked, never recorded; a tick the server has not closed is ignored.
            if (!r->running) return;
            Reader rd(f.payload.data(), f.payload.size());
            const uint32_t tk = rd.u32(), rtt = rd.u32(), arrived = rd.u32();
            if (!rd.ok || rd.p != rd.end) return;
            if (tk >= r->tick) break;
            if (tk > c.ackTick) c.ackTick = tk;
            if (arrived >= tk && arrived < r->tick && arrived > c.arrivedTick) c.arrivedTick = arrived;
            c.leadTicks = ta::net::flowLeadTicks(int(r->opts.speed), rtt);
            c.baseRttMs = c.baseRttMs == UINT32_MAX ? rtt : std::min(rtt, c.baseRttMs + 1);
            break;
        }
        case Msg::StateHash: {
            if (!r->running) return;
            Reader rd(f.payload.data(), f.payload.size());
            uint32_t tk = rd.u32(); uint64_t h = rd.u64();
            if (!rd.ok) return;
            // A report is only meaningful for a tick the server has actually CLOSED.
            // Without this check `tk` is whatever the peer says: a future tick both
            // advanced ackTick (so the client claimed flow-control progress it had not
            // made) and allocated a r.hashes entry that consensus can never retire,
            // because checkHashes() bails out while waiting for a second client that
            // will never report a tick the server never emitted. Thousands of distinct
            // fabricated ticks therefore grew the room forever.
            // r->tick is the NEXT tick to close, so the newest tick actually emitted is
            // r->tick - 1 and ">=" is the right test. (It also reads correctly at
            // r->tick == 0, where nothing has been emitted and every report is bogus.)
            if (tk >= r->tick) break;   // not emitted yet -- ignore, do not ack
            if (tk > c.ackTick) c.ackTick = tk;   // flow control: this client is up to `tk`
            // SEATED clients only. A spectator's StateHash is a progress ACK and
            // nothing more -- it deliberately sends a trivial 0 rather than
            // folding thousands of units into an FNV on the render thread every
            // 0.4s (see gameview_net.cpp, which states outright that the server
            // never desync-checks it). We were checking it anyway: the 0 landed
            // in the consensus map, disagreed with the referee's canonical hash
            // and dropped the spectator. Its first heartbeat carries ackTick 0,
            // so every spectator died at tick 0 -- spectating was broken outright.
            //
            // Ignoring it is also the correct trust boundary: a spectator is
            // non-authoritative by design (it holds nobody up in canAdvance), so
            // it must not be able to vote on what the canonical state is either.
            if (c.slot >= 0) {
                r->hashes[tk][c.id] = h;
                checkHashes(*r, tk);
                // Retention bound that does NOT depend on consensus. checkHashes only
                // trims once every live client has reported a tick, so one client
                // reporting while another lags (or stops reporting entirely, without
                // yet being dropped) leaves entries pinned indefinitely. Clients report
                // every kHashPeriod ticks, so this window is generous in real time while
                // still being finite. Oldest first -- the map is ordered by tick.
                constexpr size_t kMaxHashTicks = 256;   // ~8500 ticks of slack at kHashPeriod
                while (r->hashes.size() > kMaxHashTicks)
                    r->hashes.erase(r->hashes.begin());
            }
            break;
        }
        default: break;
    }
}

void Server::checkHashes(Room& r, uint32_t tick) {
    auto it = r.hashes.find(tick);
    if (it == r.hashes.end()) return;
    // Wait until every live human client in the room has reported this tick.
    int live = 0;
    for (int i = 0; i < kMaxSlots; ++i)
        if (r.slots[i].type == 1 && r.slotClient[i] >= 0) ++live;
    if (int(it->second.size()) < live || live == 0) return;

    // The canonical hash is the REFEREE's. A game only runs if a referee was built
    // (tryStart refuses otherwise), so there is always one; the ring just may not
    // hold this tick yet, in which case wait rather than judging anybody. (A client
    // majority would be no judge: with two clients it has no majority to find, and
    // with a modded pair against one honest client it would convict the honest one.)
    if (!r.refHash.count(tick)) return;
    const uint64_t canon = r.refHash[tick];
    // Referee suspicion needs a client CONSENSUS to appeal against the server
    // sim: with >=2 clients all agreeing with each other but NOT the referee,
    // the server sim is the odd one out (a server bug) -- don't punish the
    // clients. With a single client there is no consensus, so the referee is
    // authoritative and a lone disagreeing client is simply desynced.
    if (live >= 2) {
        std::map<uint64_t, int> ctally;
        for (auto& [cid, h] : it->second) ++ctally[h];
        if (ctally.size() == 1 && it->second.begin()->second != canon && !r.refSuspect) {
            r.refSuspect = true;
            std::fprintf(stderr, "game %u: REFEREE SUSPECT at tick %u -- all %d clients agree "
                                 "with each other but disagree with the server sim; not dropping.\n",
                         r.id, tick, live);
        }
    }
    if (r.refSuspect) { r.hashes.erase(r.hashes.begin(), std::next(it)); return; }
    for (auto& [cid, h] : it->second) {
        if (h != canon && !r.desyncFlagged[cid]) {
            r.desyncFlagged[cid] = true;
            auto ci = clients_.find(cid);
            if (ci != clients_.end()) {
                Writer w; w.u32(tick); w.str("desync detected (state diverged from the game)");
                ci->second->conn.send(Msg::Desynced, w);
                std::fprintf(stderr, "game %u: client %u DESYNCED at tick %u\n", r.id, cid, tick);
            }
        }
    }
    r.hashes.erase(r.hashes.begin(), std::next(it));   // drop this and older
}

bool Server::canAdvance(const Room& r) const {
    // Slow to the slowest CONSUMER so nobody is flooded past what they can process:
    // the server may not run more than a consumer's lead past its acked tick
    // (kMaxLeadTicks, or flowLeadTicks() once it sends Msg::Progress -- WE S3).
    // Seated humans are the primary constraint (AIs are server-run and always
    // current). When a game has NO seated humans -- an all-AI game watched by
    // spectators -- pace to the slowest spectator instead, otherwise the server
    // outruns a spectator that can't sustain the speed and floods it until the
    // connection breaks. (A spectator NEVER holds up a real match: if any human is
    // seated, spectators lag on their own.)
    uint32_t slowest = r.tick;
    bool anyHuman = false;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (r.slots[i].type != 1 || r.slotClient[i] < 0) continue;
        // An eliminated client must never pace the surviving players.
        if (r.defeated[size_t(i)]) continue;
        auto it = clients_.find(uint32_t(r.slotClient[i]));
        if (it == clients_.end() || !it->second->loaded) continue;
        anyHuman = true;
        slowest = std::min(slowest, it->second->ackTick + it->second->leadTicks);
    }
    if (anyHuman) return r.tick <= slowest;
    bool anySpec = false;
    for (uint32_t sid : r.spectators) {
        auto it = clients_.find(sid);
        if (it == clients_.end()) continue;
        anySpec = true;
        slowest = std::min(slowest, it->second->ackTick + it->second->leadTicks);
    }
    if (anySpec) return r.tick <= slowest;
    return true;   // no live consumers at all
}

void Server::closeTick(Room& r, unsigned simThreads, size_t game) {
    if(r.tickJob.valid())return;
    // All humans must have loaded before the first tick.
    if (r.tick == 0) {
        for (int i = 0; i < kMaxSlots; ++i)
            if (r.slots[i].type == 1 && r.slotClient[i] >= 0) {
                auto it = clients_.find(uint32_t(r.slotClient[i]));
                if (it == clients_.end() || !it->second->loaded) { r.rebasePace(nowUs() + 100000); return; }
            }
    }
    // The ONE place a client's commands enter a tick: strict FIFO, one budget per
    // client per tick, and the same input-delay rule for all of them.
    //
    // Server-side input delay (debug builds: TA_SRV_DELAY=K, default 0) buckets commands K ticks
    // into the future so one never "just misses" a tick boundary; costs K ticks of
    // latency.
    static const int srvDelay = [] {
        const char* e = ta::devEnv("TA_SRV_DELAY"); return e ? std::max(0, std::atoi(e)) : 0;
    }();
    //
    // An explicit order (Cmd::Order) is ONE item of its client's FIFO: it is taken
    // whole when the tick's order budget (kTickOrderBytes, shared by the room) still
    // holds it, and that tick is its activation tick. Neither budget is jumped: a
    // client's drain stops at its first item that does not fit, so nothing it sent
    // later overtakes it (a Stop behind a big Move stays behind it). The client that
    // drains first goes round the room tick by tick (orderRotor), so any order, at
    // most kOrderMaxBytes = kTickOrderBytes, is taken within kMaxSlots ticks.
    size_t orderBytes = 0;
    for (int k = 0; k < kMaxSlots; ++k) {
        const int i = (r.orderRotor + k) % kMaxSlots;
        if (r.slotClient[i] < 0) continue;
        auto ci = clients_.find(uint32_t(r.slotClient[i]));
        if (ci == clients_.end()) continue;
        Client& c = *ci->second;
        int taken = 0;
        while (!c.cmdQueue.empty()) {
            const Command& head = c.cmdQueue.front();
            if (head.kind == ta::net::Cmd::Order) {
                const size_t bytes = head.order ? ta::net::orderRecordBytes(*head.order) : 0;
                if (orderBytes + bytes > ta::net::kTickOrderBytes) break;
                // The members are commands to the per-second command budget: over it the
                // order WAITS here (and what this client sent after it waits too) until
                // the budget's second turns over; it is never dropped, so an honest
                // client's order window cannot leak.
                if (head.order && !takeInputBudget(c, nowMs(), 0, unsigned(head.order->members.size()))) break;
                orderBytes += bytes;
                c.orders.drained(bytes);
                if (head.order) r.orderSerialFloor[i] = std::max(r.orderSerialFloor[i], head.order->serial);
            } else if (taken < kCmdCapPerTick) ++taken;
            else break;
            if (srvDelay > 0) r.pendingAt[r.tick + uint32_t(srvDelay)].push_back(c.cmdQueue.front());
            else r.pending.push_back(c.cmdQueue.front());
            c.cmdQueue.pop_front();
        }
    }
    r.orderRotor = (r.orderRotor + 1) % kMaxSlots;
    // Benchmark: the run is 60 REAL seconds whatever the game speed, and its spawn stages
    // fall due evenly across them. The wall clock is the server's alone, so it reaches the
    // sims (and the replay) as a command in this tick's bundle.
    if (r.opts.benchmark && !r.benchEndSent) {
        const uint64_t now = nowUs();
        if (r.tick == 0) r.benchStartUs = now;
        const uint64_t runUs = uint64_t(ta::sim::kBenchmarkSeconds) * 1000000u;
        const uint64_t elapsed = now - r.benchStartUs;
        const uint64_t stages = uint64_t(ta::sim::benchmarkSpawns(r.opts.benchmark));
        const uint32_t due = uint32_t(std::min(stages, stages * std::min(elapsed, runUs) / runUs));
        const bool ended = elapsed >= runUs;
        if (due > r.benchDueSent || ended) {
            Command c;
            c.kind = ta::net::Cmd::BenchClock;
            c.targetId = int32_t(ended ? stages : due);
            c.queue = ended ? 1 : 0;
            r.pending.push_back(c);
            r.benchDueSent = uint32_t(c.targetId);
            r.benchEndSent = ended;
        }
    }
    ta::srv::TickInput input;
    input.tick=r.tick;input.roomId=r.id;input.simThreads=simThreads;
    // --sim-cpus: the CPUs of this game's turn among the running ones (`game`).
    if(!simCpus_.empty()) {input.simPin=true;input.simCpus=gameCpus(game,simThreads);}
    // The referee's hash is read only by checkHashes, against the reports of
    // HUMAN slots (a spectator's is a progress ack), and by the replay trail. A
    // slot becomes human only before the start (joins, seat moves and slot edits
    // are refused while running; a leave or takeover makes it closed or AI), so
    // with no human slot left -- an all-AI game watched by spectators -- nobody
    // can ever report a hash to compare and only the trail's ticks need one.
    input.clientHashes=std::any_of(std::begin(r.slots),std::end(r.slots),[](const auto& s){return s.type==1;});
    input.replayTrail=!replayDir_.empty();
    input.commands=std::exchange(r.pending,{});input.events=std::exchange(r.pendingEvents,{});
    input.replayBudget=limits_.replayMemory-std::min<uint64_t>(limits_.replayMemory,r.logBytes);
    input.fielded=r.fielded;
    if (auto it=r.pendingAt.find(r.tick);it!=r.pendingAt.end()) {
        input.scheduled=std::move(it->second);r.pendingAt.erase(it);
    }
    if(!r.worker)r.worker=std::make_unique<ta::srv::RoomWorker>();
    // Frozen inputs become committed at submission. New commands/events stay on
    // main for the following tick, including leaves or reconnects during this job.
    r.tickJob=r.worker->submit([ref=r.ref.get(),ai=&r.ai,reg=r.reg,input=std::move(input)]() mutable {
        return ta::srv::simulateRoomTick(*ref,*reg,*ai,std::move(input));
    });
}

void Server::finishTick(Room& r) {
    if(!r.tickJob.valid() || r.tickJob.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
    TickResult result;
    try {result=r.tickJob.get();}
    catch(const std::exception& e) {r.simulationError=e.what();}
    catch(...) {r.simulationError="unknown simulation failure";}
    if(!r.simulationError.empty() || result.resourceLimited) {
        r.resourceLimited=result.resourceLimited;
        if(!r.simulationError.empty())std::fprintf(stderr,"game %u simulation failed: %s\n",r.id,r.simulationError.c_str());
        return;
    }
    // Publish one completed tick atomically from the network thread. A reconnect
    // sees either the old replay boundary or this complete bundle, never half a tick.
    Writer w;w.b=std::move(result.bundle);
    broadcastRoom(r,Msg::TickBundle,w);
    r.logBytes+=w.b.size()+2*sizeof(std::vector<uint8_t>);
    r.log.push_back(std::move(w.b));
    r.defeated=result.defeated;
    r.fielded=result.fielded;r.decided=result.decided;r.winningTeam=result.winningTeam;
    if(result.hash) {
        r.refHash[r.tick]=*result.hash;
        if(!replayDir_.empty() && (r.tick/uint32_t(kHashPeriod))%10==0)
            r.replayChecks.push_back({r.tick,*result.hash,result.posDigest});
        while(r.refHash.size()>300)r.refHash.erase(r.refHash.begin());
    }
    if(!r.missionOutcomeSent && result.missionOutcome) {
        r.missionOutcomeSent=int8_t(result.missionOutcome);
        Writer mw;mw.u8(uint8_t(r.missionOutcomeSent));broadcastRoom(r,Msg::MissionOutcome,mw);
        std::fprintf(stderr,"game %u mission %s: %s\n",r.id,r.mission.c_str(),result.missionOutcome>0?"VICTORY":"DEFEAT");
    }
    ++r.tick;
    r.pace.advance(r.tick, nowUs());
    // TA_SRV_PACELOG=N (debug builds): log the wall time every N published ticks, to measure the
    // real tick rate (8x must be 240.0 ticks/s, 1x 30.0).
    static const uint32_t paceLog = [] {
        const char* e = ta::devEnv("TA_SRV_PACELOG"); return e ? uint32_t(std::max(0, std::atoi(e))) : 0u;
    }();
    if (paceLog && r.tick % paceLog == 0)
        std::fprintf(stderr, "PACE game %u tick %u us %llu speed %d\n", r.id, r.tick,
                     (unsigned long long)nowUs(), int(r.opts.speed));
}

// The game ends here, at the room's last completed tick (the caller has no tick in
// flight). The log is complete from now on, so its length is the game's: a watcher is
// done when it has played that many bundles, however far behind it is, and the replay
// the room writes when it goes ends on the same tick. The winner is the standing of
// that tick; a game its players abandoned undecided has none.
void Server::endGame(Room& r, const char* why) {
    r.over = true;
    r.overWhy = why;
    const int team = r.decided ? int(r.winningTeam) : -1;
    Writer w; w.u32(uint32_t(r.log.size())); w.u8(team >= 0 ? uint8_t(team) : uint8_t(0xFF));
    broadcastRoom(r, Msg::GameOver, w);
    if (team >= 0) std::fprintf(stderr, "game %u over after %zu ticks (%s): team %d wins\n", r.id, r.log.size(), why, team + 1);
    else std::fprintf(stderr, "game %u over after %zu ticks (%s): no winner\n", r.id, r.log.size(), why);
}

bool Server::catchingUp(Room& r, uint64_t now) {
    bool any = false;
    uint64_t sent = 0;
    for (uint32_t sid : r.spectators) {
        const auto it = clients_.find(sid);
        if (it == clients_.end() || !it->second->replaying) continue;
        any = true;
        sent += it->second->replayPos;
    }
    if (!any) return false;
    if (!r.overSentMs || sent != r.overSent) { r.overSent = sent; r.overSentMs = now; }
    if (now - r.overSentMs < gOverDrainMs) return true;
    std::fprintf(stderr, "game %u: a watcher took no more of the ended game's log for %llus -- not waiting for it\n",
                 r.id, (unsigned long long)(gOverDrainMs / 1000));
    return false;
}

void Server::dropClient(uint32_t id, const char* reason) {
    auto it = clients_.find(id);
    if (it == clients_.end()) return;
    Client& c = *it->second;
    Room* r = c.roomId ? roomOf(c) : nullptr;
    // Defeated players normally disconnect from the result screen. Their
    // departure must not stop the surviving players for the disconnect wait.
    const bool defeated = r && c.slot >= 0 && c.slot < kMaxSlots && r->defeated[size_t(c.slot)];
    if (r && r->running && c.slot >= 0 && c.slot < kMaxSlots &&
        r->slots[c.slot].type == 1 && !defeated) {
        // A disconnect from a running game without a Leave HOLDS the seat: the game
        // stops and waits gDropWaitMs for the player to reconnect (Msg::Rejoin with
        // the resume token), then an AI takes the seat (takeOver). A seat already
        // waiting (it went silent first) keeps its countdown.
        int s = c.slot;
        dropPendingCommands(c, *r);   // the seat is HELD; its in-flight orders are not
        r->slotClient[s] = -1;
        // Silence counts as it does for a stall (the run loop): only once the game has
        // ticked and the client has loaded, since loading has its own deadlines.
        beginWait(*r, s, "disconnected", r->tick > 0 && c.loaded ? c.lastRecvMs : nowMs());
        Writer w; w.u8(uint8_t(s)); w.u8(1 /*dropped*/); w.u32(0);
        broadcastRoom(*r, Msg::PlayerStatus, w);
    } else if (r) {
        leaveRoom(c, reason);
    }
    std::fprintf(stderr, "client %u dropped (%s)\n", id, reason);
    clients_.erase(it);
}

void Server::unseat(Client& c, const char* reason) {
    if (Room* r = c.roomId ? roomOf(c) : nullptr) {
        dropPendingCommands(c, *r);
        if (c.slot >= 0 && c.slot < kMaxSlots && r->slotClient[c.slot] == int(c.id)) r->slotClient[c.slot] = -1;
    }
    c.replaying = false; c.mapValidationId = c.modValidationId = 0; c.deferredMapOffer.reset();
    c.state = Client::Lobby; c.roomId = 0; c.slot = -1; c.loaded = false;
    Writer w; w.str(reason); c.conn.send(Msg::Bye, w);
    c.conn.fail(reason);
}

void Server::sendWaiting(Client& c, const Room& r) {
    const uint64_t now = nowMs();
    Writer w;
    uint8_t n = 0;
    for (int i = 0; i < kMaxSlots; ++i) n += r.waitDeadline[i] != 0;
    w.u8(n);
    for (int i = 0; i < kMaxSlots; ++i)
        if (r.waitDeadline[i]) {
            w.u8(uint8_t(i));
            w.u32(uint32_t(r.waitDeadline[i] > now ? std::min<uint64_t>(r.waitDeadline[i] - now, UINT32_MAX) : 0));
        }
    // The recipient's own budget, so a player whose link goes counts down what the room
    // will really wait for it: what its running wait has left, else what its seat has
    // left to spend (all of it for a spectator).
    const int s = c.slot;
    const bool mine = s >= 0 && s < kMaxSlots && r.waitDeadline[s];
    uint64_t own = gDropWaitMs;
    if (mine) own = r.waitDeadline[s] > now ? r.waitDeadline[s] - now : 0;
    else if (s >= 0 && s < kMaxSlots) own = gDropWaitMs - std::min<uint64_t>(r.waitSpentMs[s], gDropWaitMs);
    w.u8(mine ? 1 : 0);
    w.u32(uint32_t(std::min<uint64_t>(own, UINT32_MAX)));
    c.conn.send(Msg::Waiting, w);
}

void Server::broadcastWaiting(Room& r) {
    for (auto& [id, peer] : clients_) {
        (void)id;
        if (peer->roomId == r.id && peer->state == Client::InGame) sendWaiting(*peer, r);
    }
}

// A seat's budget is charged in whole seconds, rounded up, never past the budget: the
// countdown on screen shows whole seconds, so a player back while it read 1 has spent
// the budget, and a stall of a moment still costs a second.
static void chargeWait(uint64_t& spent, uint64_t ms) {
    spent = std::min(gDropWaitMs, spent + (ms + 999) / 1000 * 1000);
}

// A seat waits out what is left of its budget, counted from its last contact (`since`):
// the silence that made the room stop is already spent, so a player who keeps dropping
// and coming back cannot hold the game for longer than gDropWaitMs in all. A seat whose
// silence has used what was left is due at once (the deadline is now, and the caller's
// deadline check takes it over) and the room is never told to wait for it.
void Server::beginWait(Room& r, int slot, const char* why, uint64_t since) {
    if (r.waitDeadline[slot]) return;
    const uint64_t now = nowMs();
    since = std::min(since, now);
    // A hold already running is the silence's from the last contact on; what it held
    // before (a seat that talked but did not keep up, then went quiet) is charged now.
    if (r.holdBegan[slot]) endHold(r, slot, since > r.holdBegan[slot] ? std::min(r.holdMs[slot], since - r.holdBegan[slot]) : 0);
    const uint64_t left = gDropWaitMs - std::min(r.waitSpentMs[slot], gDropWaitMs);
    r.waitBegan[slot] = since;
    r.waitDeadline[slot] = std::max(since + left, now);
    if (since + left <= now) {
        std::fprintf(stderr, "game %u: slot %d %s -- its wait is spent, an AI takes over\n", r.id, slot, why);
        return;
    }
    broadcastWaiting(r);
    std::fprintf(stderr, "game %u: slot %d %s -- waiting %llus for it (silent %.1fs, of %llus), then an AI takes over\n",
                 r.id, slot, why, (unsigned long long)((since + left - now + 999) / 1000), double(now - since) / 1000.0,
                 (unsigned long long)(gDropWaitMs / 1000));
}

// A wait that ends charges the seat's budget for the whole silence, from its last contact
// to now (chargeWait).
void Server::endWait(Room& r, int slot) {
    if (!r.waitDeadline[slot]) return;
    chargeWait(r.waitSpentMs[slot], nowMs() - r.waitBegan[slot]);
    r.waitDeadline[slot] = 0;
    if (!r.held()) r.rebasePace(nowUs());   // rebase the tick clock (no burst)
    broadcastWaiting(r);
    std::fprintf(stderr, "game %u: slot %d is back -- wait over, %llus of it left\n", r.id, slot,
                 (unsigned long long)((gDropWaitMs - r.waitSpentMs[slot]) / 1000));
}

// The room waits for a seat only with someone else to wait with: another seated player
// still connected, or a spectator. A player alone in the room (single-player) keeps the
// plain timeout, and the room ends when it goes.
bool Server::waitedWith(const Room& r, int slot) const {
    if (!r.spectators.empty()) return true;
    for (int j = 0; j < kMaxSlots; ++j)
        if (j != slot && r.slotClient[j] >= 0) return true;
    return false;
}

// A hold without a wait: a seated player's silence shorter than gStallMs (or a client
// that talks but does not keep up, or one catching up after a reconnect) still stops the
// game once the room has run its lead past the seat's last acknowledged tick (canAdvance).
// Called where a tick would close, so only time the game was really held counts (not a
// pause or another seat's wait, which suspend it). A hold that ends is charged for what
// it lasted past kHoldGraceMs (endHold); one that lasts past the grace plus what the
// budget has left is taken over at once, as a wait that runs out. Only called with no
// tick job in flight (takeOver).
void Server::trackHolds(Room& r, uint64_t now) {
    for (int i = 0; i < kMaxSlots; ++i) {
        bool holding = false;
        if (r.tick > 0 && r.slots[i].type == 1 && !r.defeated[size_t(i)] && r.slotClient[i] >= 0 && waitedWith(r, i)) {
            auto it = clients_.find(uint32_t(r.slotClient[i]));
            holding = it != clients_.end() && it->second->loaded &&
                      r.tick > it->second->ackTick + it->second->leadTicks;
        }
        if (!holding) {
            if (r.holdBegan[i]) endHold(r, i, r.holdMs[i]);
            continue;
        }
        if (!r.holdBegan[i]) r.holdBegan[i] = now;
        else if (r.holdSeen[i]) r.holdMs[i] += now - r.holdSeen[i];
        r.holdSeen[i] = now;
        const uint64_t left = gDropWaitMs - std::min(r.waitSpentMs[i], gDropWaitMs);
        if (r.holdMs[i] >= kHoldGraceMs + left) {
            std::fprintf(stderr, "game %u: slot %d held the game %.1fs -- its wait is spent\n", r.id, i,
                         double(r.holdMs[i]) / 1000.0);
            takeOver(r, i);
        }
    }
}

// A hold over: what it held the game past kHoldGraceMs is charged (chargeWait), so an
// occasional hitch of a second costs nothing and repeated short silences cannot hold the
// game for longer than the budget.
void Server::endHold(Room& r, int slot, uint64_t heldMs) {
    r.holdBegan[slot] = r.holdMs[slot] = r.holdSeen[slot] = 0;
    if (heldMs <= kHoldGraceMs) return;
    chargeWait(r.waitSpentMs[slot], heldMs - kHoldGraceMs);
    std::fprintf(stderr, "game %u: slot %d held the game %.1fs -- %llus of its wait left\n", r.id, slot,
                 double(heldMs) / 1000.0, (unsigned long long)((gDropWaitMs - r.waitSpentMs[slot]) / 1000));
    // The seat's client keeps its own budget (Msg::Waiting) for a reconnect window.
    if (int cid = r.slotClient[slot]; cid >= 0)
        if (auto it = clients_.find(uint32_t(cid)); it != clients_.end() && it->second->state == Client::InGame)
            sendWaiting(*it->second, r);
}

// The wait ran out. The seat's connection, if it is still open, is closed; the seat
// becomes a server AI at Normal from the next tick, sequenced as Event::AiTakeover so
// every peer, the referee and the replay mark the player computer-owned on the same
// tick. The AI starts from the world as it is: it reads the empire afresh every think
// (home = its buildings, its producers, its army), so nothing about the first ticks of
// the game is needed. Only called with no tick job in flight (r.ai belongs to the
// worker while one runs).
void Server::takeOver(Room& r, int slot) {
    if (r.slots[slot].type != 1 || r.defeated[size_t(slot)]) { endWait(r, slot); return; }
    r.waitDeadline[slot] = 0;
    r.waitSpentMs[slot] = gDropWaitMs;
    r.holdBegan[slot] = r.holdMs[slot] = r.holdSeen[slot] = 0;
    if (int cid = r.slotClient[slot]; cid >= 0) {
        auto it = clients_.find(uint32_t(cid));
        if (it != clients_.end()) unseat(*it->second, "you stopped responding: an AI took over your seat");
        r.slotClient[slot] = -1;
    }
    r.takenToken[slot] = r.slotToken[slot];
    r.slotToken[slot] = 0;
    r.slots[slot].type = 2;
    r.slots[slot].aiLevel = uint8_t(ta::ai::Difficulty::Normal);
    r.pendingEvents.push_back({ta::net::Event::Kind::AiTakeover, uint8_t(slot)});
    if (r.ref) {
        r.ai.emplace_back(slot, *r.reg, r.aiProfiles[size_t(ta::ai::Difficulty::Normal)],
                          r.seed + uint32_t(slot), ta::ai::Difficulty::Normal);
        // The setup a computer seat gets at the match start (tryStart): the map's (or
        // mission's) aiprofile script. Without it the TA AI knows no build
        // preferences and builds nothing.
        r.ai.back().setClassicProfile(r.classicProfile);
        r.ai.back().setCampaign(!r.mission.empty());   // scenario mode 1 (0x40bb70)
    }
    std::fprintf(stderr, "game %u: player %d stopped communicating -- AI (Normal) takes over\n", r.id, slot);
    // The host's seat is gone for good: its powers pass on, and the room is told.
    if (slot == r.hostSlot) { passHost(r, 0); broadcastLobby(r); }
    if (!r.held()) r.rebasePace(nowUs());
    broadcastWaiting(r);
}

// Ordinary lobby work needs a wall-clock budget too; a per-poll frame cap
// alone lets a peer flood indefinitely by sending just below that cap.
bool Server::allowLobbyWork(Client& c,const Frame& f) {
    unsigned cost=1;
    switch(f.kind) {
        case Msg::ModChunk: case Msg::MapChunk:return true; // upload reads are paced before recv(), not discarded
        case Msg::CreateGame: case Msg::StartGame: case Msg::ModOffer: case Msg::MapOffer: cost=16;break;
        case Msg::SetRoomMap: cost=4;break;   // its expensive part, a MapOffer, is charged on its own
        case Msg::ListGames: case Msg::JoinGame: case Msg::Spectate:
        case Msg::ModRequest: case Msg::ModReady: case Msg::ModError:
        case Msg::Chat: case Msg::MapRequest: case Msg::MapReady: case Msg::MapError:
        case Msg::Ping: case Msg::Pong: case Msg::SlotUpdate: case Msg::MoveSeat: case Msg::SetGameOptions:
        case Msg::LeaveGame: case Msg::Rejoin: case Msg::Kick: case Msg::SetPause:
        case Msg::VoiceListen: case Msg::SetRoomInfo: break;
        default:return true; // Login and command queues have their own budgets.
    }
    auto& keys=lobbyWorkKeys_;
    auto& global=lobbyGlobalWork_;
    const unsigned personal=128,shared=2048;
    const auto now=nowMs();
    auto& local=c.lobbyWork;
    if(!local.take(now,personal,cost)) {c.conn.fail("request rate exceeded");return false;}
    // A hostile peer behind a NAT must not disconnect healthy neighbours.
    // Keepalive has only the per-connection budget; shared exhaustion drops
    // expensive requests without revoking anybody else's session.
    // VoiceListen too: one byte, sent only when the switch changes and never repeated, so a
    // dropped one would leave this connection deaf to voice until the player toggled it.
    // LeaveGame as well: it has no answer, so its sender cannot tell a dropped one (the Ping
    // the client sends behind it is answered all the same) and would browse the game list
    // while the room still seated it. It only frees a seat, which a budgeted request took.
    if(f.kind==Msg::Ping || f.kind==Msg::Pong || f.kind==Msg::VoiceListen || f.kind==Msg::LeaveGame)return true;
    return keys.take("ip:"+c.peer,now,personal*4,cost) &&
       (c.account.empty() || keys.take("user:"+ta::auth::foldUsername(c.account),now,personal*2,cost)) &&
       global.take(now,shared,cost);
}

size_t Server::mapMemory() const {
    size_t used=0;
    for(const auto& job:validations_)used+=size_t(job.size)*2;
    for(const auto& [id,peer]:clients_)used+=(size_t(peer->mapReceive.size)+peer->modReceive.size)*2;
    for(const auto& [id,room]:rooms_)if(room.mapPackage)used+=room.mapPackage->bytes.size()*2;
    for(const auto& [id,room]:rooms_)if(room.modPackage)used+=room.modPackage->bytes.size()*2;
    return used;
}

void Server::onFrame(Client& c, const Frame& f) {
    c.lastRecvMs = nowMs();
    if(!allowLobbyWork(c,f))return;
    if (f.kind == Msg::Ping) { c.conn.send(Msg::Pong); return; }
    if (f.kind == Msg::Pong) return;
    if (f.kind == Msg::Bye) { c.conn.fail("bye"); return; }
    // Voice preference: connection state, kept across lobby and rooms.
    if (f.kind == Msg::VoiceListen) {
        if (c.state != Client::Lobby && c.state != Client::InGame) return;
        if (f.payload.size() != 1 || f.payload[0] > 1) { c.conn.fail("invalid voice message"); return; }
        c.voiceListen = f.payload[0] == 1;
        return;
    }
    if (f.kind == Msg::VoiceDown) { c.conn.fail("invalid voice message"); return; }   // server-to-client only
    if(c.state==Client::InGame && f.kind>=Msg::ModOffer && f.kind<=Msg::ModError){modMsg(c,f);return;}
    if (c.state == Client::InGame && (f.kind == Msg::MapOffer || f.kind == Msg::MapRequest ||
        f.kind == Msg::MapChunk || f.kind == Msg::MapReady || f.kind == Msg::MapError)) {
        mapMsg(c, f); return;
    }
    switch (c.state) {
        case Client::Handshake: handshake(c, f); break;
        case Client::Auth: authMsg(c, f); break;
        case Client::Lobby: lobbyMsg(c, f); break;
        case Client::InGame: gameMsg(c, f); break;
    }
}

// The game (not private) announced once a second: its name, the host's name, the map,
// seats taken and the room's capacity, whether it is locked, started and watchable.
// One waiting in its room first; else a started one that allows spectators. Nothing
// before the host creates it, nor for a game that has ended (Room::over).
void Server::announceLan(uint64_t now) {
    if (now < beaconAtMs_) return;
    beaconAtMs_ = now + 1000;
    if (!lanNonce_) lanNonce_ = std::random_device{}() * 0x9E3779B97F4A7C15ull ^ now;
    const Room* pick = nullptr;
    for (const auto& [id, r] : rooms_) {
        (void)id;
        if (r.priv || r.over || (r.running && !r.opts.allowSpectators)) continue;
        if (!pick || (pick->running && !r.running)) pick = &r;
    }
    if (pick) {
        const Room& r = *pick;
        ta::net::LanAnnounce a;
        a.version = kNetVersion; a.nonce = lanNonce_; a.port = port_;
        a.game = r.name;
        if (const auto host = clients_.find(r.hostId); host != clients_.end()) a.host = host->second->name;
        if (ta::mapgen::isGeneratedMapId(r.mapId)) a.map = ta::mapgen::friendlyLabel(ta::mapgen::decodeMapId(r.mapId));
        else {
            // The map's own title where its .ota spells the same name (as the lobby
            // shows it: "Ulasem Arena", not the file's "ulasem arena").
            auto [it, fresh] = lanMapTitles_.try_emplace(r.mapId, r.mapId);
            auto lower = [](std::string t) { for (auto& c : t) c = char(std::tolower((unsigned char)c)); return t; };
            if (fresh && lanTranslate_.empty())   // the original maps' English names, as the lobby reads them
                for (const char* path : {"translate/maps.tdf", "translate/ipmaps.tdf"}) try {
                    const auto b = retail_.vfs.read(path);
                    const auto root = ta::tdf::parseText(std::string(b.begin(), b.end()), path);
                    for (const auto& [key, node] : root.children)
                        if (const std::string* en = node.value("english"); en && !en->empty()) lanTranslate_[lower(key)] = *en;
                } catch (const std::exception&) {}
            bool titled = false;
            if (const auto t = lanTranslate_.find(lower(r.mapId)); fresh && t != lanTranslate_.end() && lower(t->second) == lower(r.mapId))
                it->second = t->second, titled = true;
            else if (fresh) try {
                const ta::hpi::Vfs& vfs = r.mapVfs ? *r.mapVfs : retail_.vfs;
                std::string path = ta::hpi::findMap(vfs, r.mapId);
                if (const auto dot = path.rfind('.'); dot != std::string::npos) path.resize(dot);
                if (const auto ota = vfs.tryRead(path + ".ota")) {
                    const auto title = ta::tnt::Scenario::parse(std::string(ota->begin(), ota->end())).missionName;
                    if (!title.empty() && lower(title) == lower(r.mapId)) it->second = title, titled = true;
                }
            } catch (const std::exception&) {}
            a.map = it->second;
            // The raw id stands in until the map's package has verified (a map the server's
            // install does not have): look again then, rather than keep the stand-in.
            if (fresh && !titled && !r.mapVfs) lanMapTitles_.erase(it);
        }
        for (int i = 0; i < std::min(r.cap, int(kMaxSlots)); ++i)
            if (r.slots[i].type == 1 || r.slots[i].type == 2) ++a.players;
        a.capacity = uint8_t(r.cap);
        a.locked = !r.password.empty();
        a.running = r.running;
        a.spectators = r.opts.allowSpectators != 0;
        beacon_.send(a);
    }
}

int Server::run() {
    std::string err;
    listenFd_ = listenOn(port_, err, loopbackOnly_);
    if (listenFd_ < 0) { std::fprintf(stderr, "taserver: %s on port %u\n", err.c_str(), port_); return 1; }
    ta::srv::StatusListener status(port_);
    std::fprintf(stderr, "taserver %s listening on %s port %u (protocol v%u)\n",
                 ta::kVersion, loopbackOnly_ ? "loopback" : "all interfaces", port_, kNetVersion);
    // Build id on its own line: the harness greps it to refuse a server built from
    // different source than the client, which otherwise looks exactly like a desync.
    std::fprintf(stderr, "taserver: build %s\n", ta::kBuildId);
    std::fprintf(stderr,"taserver: transport %s\n",acme_?"TLS (ACME; connections wait for certificate)":tlsContext_?"TLS (plaintext rejected)":"UNENCRYPTED TCP");
    if (requireAuth_)
        std::fprintf(stderr, "taserver: accounts required -- %zu in %s\n",
                     accounts_.size(), accounts_.path().c_str());
    else
        std::fprintf(stderr, "taserver: NO ACCOUNTS REQUIRED (--no-auth)%s\n",
                     loopbackOnly_ ? "" : " -- anyone who can reach this port can play");

    // Hoisted out of the loop so their capacity persists across wakeups (this loop
    // runs at least at tick rate; rebuilding the contents is cheap, reallocating
    // them thousands of times a second is not).
    std::vector<pollfd> pfds;
    std::vector<uint32_t> ids;
    // With --replaydir, a SIGTERM (systemctl stop) or Ctrl-C writes the replays of the
    // games still running before the server exits (otherwise a replay is written
    // only when a room is torn down). Without it nothing is kept, so the
    // default (exit at once) stays -- a single-player client stops its private server
    // with SIGTERM and waits for it.
    static std::atomic<bool> stopRequested{false};
    if (!replayDir_.empty()) {
        std::signal(SIGTERM, [](int) { stopRequested.store(true, std::memory_order_relaxed); });
        std::signal(SIGINT, [](int) { stopRequested.store(true, std::memory_order_relaxed); });
    }
    for (;;) {
        if (stopRequested.load(std::memory_order_relaxed)) {
            for (auto& [rid, r] : rooms_) {
                (void)rid;
                if (r.tickJob.valid()) { r.tickJob.wait(); finishTick(r); }
                if (r.running) writeReplay(r);
            }
            std::fprintf(stderr, "taserver: stopped by signal\n");
            return 0;
        }
        finishValidations();
        for(auto& [id,room]:rooms_) {(void)id;finishTick(room);}
        // Build the pollfd set: listen + every client (POLLOUT when it has pending writes).
        pfds.clear();
        ids.clear();
        // Field-wise (not brace) init: a socket fd is `int` here but `SOCKET`
        // (unsigned) in a Windows pollfd, which brace-init would reject as narrowing.
        pollfd lp{}; lp.fd = listenFd_; lp.events = POLLIN; pfds.push_back(lp);
        ids.push_back(0);
        const int statusFd = status.pollFd();
        if (statusFd >= 0) {
            pollfd sp{}; sp.fd = statusFd; sp.events = POLLIN; pfds.push_back(sp);
            ids.push_back(0);
        }
        const size_t clientsBegin = pfds.size();
        for (auto& [id, c] : clients_) {
            short ev = canRead(*c,nowMs()) ? POLLIN : 0;
            if (c->conn.wantWrite()) ev |= POLLOUT;
            pollfd cp{}; cp.fd = c->conn.fd(); cp.events = ev; pfds.push_back(cp);
            ids.push_back(id);
        }
        // Timeout = time until the soonest running room's next tick (or 1s idle).
        uint64_t now = nowMs();
        uint64_t soonest = now + (validations_.empty()?1000:10);
        if (lan_) { announceLan(now); soonest = std::min(soonest, beaconAtMs_); }
        // Only rooms ELIGIBLE to tick may pull the poll deadline in. A paused room
        // is skipped by the tick loop below but its tick deadline still slides into
        // the past, so counting it here pinned the timeout at 0 and span the
        // server at full CPU for as long as the pause lasted -- and a manual
        // pause has no expiry, so that is indefinite.
        for (auto& [rid, r] : rooms_) {
            (void)rid;
            if(r.tickJob.valid()) {soonest=std::min(soonest,now+2);continue;}
            // Round the deadline UP to the poll's millisecond, so the wake-up is never
            // early (an early wake finds nothing due and polls again with timeout 0).
            const uint64_t dueMs = (r.nextTickUs() + 999) / 1000;
            if (r.running && !r.held() && dueMs < soonest) soonest = dueMs;
        }
        // A client still streaming catch-up needs servicing regardless of the
        // tick clock -- otherwise resuming into a PAUSED game would feed it one
        // chunk per idle second, since a paused room no longer pulls the
        // deadline in.
        for (const auto& [id, c] : clients_) {
            if (c->conn.bufferedInput() && canRead(*c,now))soonest=now;
            if ((c->replaying || c->mapSend.package || c->mapReceive.size || c->modSend.package || c->modReceive.size ||
                 c->conn.lowWaiting()) && now + 10 < soonest) soonest = now + 10;
        }
        int timeout = int(soonest > now ? soonest - now : 0);

        // A tick in flight on a room worker caps the timeout at 2 ms above, but sleeping in
        // poll() for it costs a whole timer quantum on Windows (~15.6 ms), not 2 ms -- one
        // quantum per tick, which held a Windows server near 70 ticks/s whatever the speed
        // (8x wants 240). So poll without sleeping, and if no socket is ready wait on the
        // job's future instead: its completion wakes the wait at once.
        std::future<TickResult>* inFlight = nullptr;
        for (auto& [rid, r] : rooms_) { (void)rid; if (r.tickJob.valid()) { inFlight = &r.tickJob; break; } }
        int n = TA_POLL(pfds.data(), (unsigned)pfds.size(), inFlight ? 0 : timeout);
        if (n < 0) { if (sockInterrupted(sockErr())) continue; break; }
        if (n == 0 && inFlight && timeout > 0) inFlight->wait_for(std::chrono::milliseconds(timeout));

        for(auto& [id,room]:rooms_) {(void)id;finishTick(room);}
        throttle_.expire(now);   // forget hosts that have long since behaved
        tokens_.expire(now);

        if (statusFd >= 0 && (pfds[1].revents & POLLIN)) {
            ta::srv::StatusCounts counts;
            for (const auto& [id, room] : rooms_) {
                if (room.playing()) ++counts.runningGames;
                else if (!room.running) ++counts.lobbyGames;
            }
            // Count established player/spectator/browser sessions, not sockets
            // still authenticating. Status queries never create such sessions.
            for (const auto& [id, client] : clients_)
                if (client->conn.ok() && !client->conn.peerClosed() &&
                    (client->state == Client::Lobby || client->state == Client::InGame))
                    ++counts.connectedClients;
            status.respond(counts);
        }

        // Accept new connections.
        if (pfds[0].revents & POLLIN) {
            for (unsigned accepted=0;accepted<32;++accepted) {
                int fd = int(accept(listenFd_, nullptr, nullptr));
                if (fd < 0) break;
                const auto address=peerAddress(fd);
                if(!acceptKeys_.take(address,now,32) || !acceptWork_.take(now,128)) {sockClose(fd);continue;}
                size_t pending=0,fromAddress=0;
                for(const auto& [id,peer]:clients_) {
                    (void)id;
                    pending+=peer->state==Client::Handshake || peer->state==Client::Auth;
                    fromAddress+=peer->peer==address;
                }
                if(clients_.size()>=kMaxClients || pending>=kMaxPendingLogins || fromAddress>=kMaxClientsPerAddress) {
                    sockClose(fd);continue;
                }
                // Never silently accept plaintext while initial ACME issuance waits.
                if(acme_ && !acme_->context()) {sockClose(fd);continue;}
                setupSocket(fd);
                limitUnsent(fd);   // voice yields to game traffic in the kernel's queue too
                auto c = std::make_unique<Client>();
                c->id = nextClientId_++;
                c->conn = Conn(fd,acme_?acme_->context():tlsContext_);
                c->peer = address;
                c->connectedMs = c->lastRecvMs = nowMs();
                clients_[c->id] = std::move(c);
            }
        }
        // Service clients.
        std::vector<uint32_t> dead;
        for (size_t i = clientsBegin; i < pfds.size(); ++i) {
            uint32_t id = ids[i];
            auto it = clients_.find(id);
            if (it == clients_.end()) continue;
            Client& c = *it->second;
            if(!canRead(c,now) && (pfds[i].revents & (POLLHUP|POLLERR))) {dead.push_back(id);continue;}
            if (((pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) || c.conn.bufferedInput()) && canRead(c,now)) {
                const bool uploading=c.mapReceive.size!=0 || c.modReceive.size!=0;
                if(uploading) {c.uploadWork.take(now,4096,64);uploadGlobalWork_.take(now,16384,64);}
                if (!c.conn.recv(uploading?65536:1u<<20)) { dead.push_back(id); continue; }
                Frame fr;
                unsigned frames=0;
                while (c.conn.poll(fr)) {
                    if(++frames>1024) {c.conn.fail("message flood");break;}
                    try {onFrame(c,fr);}
                    catch(const std::exception& e) {
                        std::fprintf(stderr,"client request failed: %s\n",e.what());
                        c.conn.fail("invalid client request");
                    }
                    if (!c.conn.ok() || !canRead(c,nowMs())) break;
                }
                // A clean close is only final once its trailing frames are drained
                // above -- the last LeaveGame usually arrives in the same segment.
                if (c.conn.peerClosed()) c.conn.fail("peer closed");
            }
            if (!c.conn.ok()) { dead.push_back(id); continue; }
            if (pfds[i].revents & POLLOUT) c.conn.flushWrite();
        }
        // The disconnect wait. A seated player the room has heard nothing from for
        // gStallMs stops the game (the same as a closed connection, see dropClient), the
        // countdown already short by that silence; one that talks again, on the same
        // connection, lets it go on; one still away when its countdown runs out has its
        // seat taken over by an AI. Silence counts only once the game has ticked (loading
        // has its own deadlines) and only with someone else still connected to wait with
        // (waitedWith). The countdown
        // itself runs out whatever the tick: a seat that closed before the first tick
        // (dropClient) would otherwise hold the room forever.
        now = nowMs();
        for (auto& [rid, r] : rooms_) {
            (void)rid;
            if (!r.running) continue;
            for (int i = 0; i < kMaxSlots && r.tick > 0; ++i) {
                if (r.slots[i].type != 1 || r.defeated[size_t(i)] || r.slotClient[i] < 0) continue;
                auto it = clients_.find(uint32_t(r.slotClient[i]));
                if (it == clients_.end() || !it->second->loaded || !it->second->conn.ok()) continue;
                const bool silent = now - it->second->lastRecvMs >= gStallMs;
                if (!silent) { endWait(r, i); continue; }
                if (r.waitDeadline[i]) continue;
                if (waitedWith(r, i)) beginWait(r, i, "stopped responding", it->second->lastRecvMs);
            }
            if (r.tickJob.valid()) continue;   // r.ai belongs to the worker until it lands
            for (int i = 0; i < kMaxSlots; ++i)
                if (r.waitDeadline[i] && now >= r.waitDeadline[i]) takeOver(r, i);
        }
        // Tear down abandoned running games (everyone left/forfeited): write the
        // replay, then erase.
        std::vector<std::pair<uint32_t, const char*>> doneRooms;
        for (auto& [rid, r] : rooms_) {
            if (!r.running || r.tickJob.valid()) continue;
            if(r.resourceLimited || !r.simulationError.empty()) {
                for(auto& [cid,peer]:clients_)if(peer->roomId==rid) {
                    sendReject(*peer,r.resourceLimited?"game replay memory limit reached":"game simulation failed");peer->conn.fail("game stopped");
                }
                doneRooms.push_back({rid,r.resourceLimited?"replay memory limit":"simulation failure"});continue;
            }
            if (!roomOccupied(r)) { doneRooms.push_back({rid, "last player left"}); continue; }
            // Someone is still connected, so a game that ends now ends in front of them:
            // every seated player has gone (and no AI is left to watch), or the match has
            // its result with no human seated (matchOver). They are told once, at the
            // room's last completed tick -- this scan runs before any further tick is
            // submitted -- and the room goes when they all have the whole log.
            if (!r.over) {
                const char* why = !roomActive(r) ? "all players gone" : matchOver(r) ? "match over" : nullptr;
                if (!why) continue;
                endGame(r, why);
            }
            if (!catchingUp(r, now)) doneRooms.push_back({rid, r.overWhy});
        }
        for (auto& [rid, why] : doneRooms) {
            Room& r = rooms_.at(rid);
            writeReplay(r);
            // Detach any lingering spectators before the room vanishes: reset their
            // server-side state to Lobby (their client has been told the game is over,
            // Msg::GameOver, and shows its result; it can browse or leave on its own).
            for (uint32_t sid : r.spectators) {
                auto it = clients_.find(sid);
                if (it == clients_.end()) continue;
                it->second->state = Client::Lobby;
                it->second->roomId = 0; it->second->slot = -1;
            }
            std::fprintf(stderr, "game %u ended (%s)\n", rid, why);
            rooms_.erase(rid);
        }
        // A room that never started and that nobody is in any more holds no world and
        // nothing to save, but would count against --max-games for good. leaveRoom
        // dissolves a lobby as its host goes; this is the backstop for any departure that
        // leaves one behind. A connection still naming the room keeps it (its map and
        // game-mod uploads and validations are that connection's), and finishValidations
        // cancels the validation of a room that is gone.
        if (!rooms_.empty()) {
            std::set<uint32_t> named;
            for (const auto& [id, peer] : clients_) if (peer->roomId) named.insert(peer->roomId);
            for (auto it = rooms_.begin(); it != rooms_.end();) {
                if (it->second.running || roomOccupied(it->second) || named.count(it->first)) { ++it; continue; }
                std::fprintf(stderr, "game %u dissolved (nobody left in it)\n", it->first);
                it = rooms_.erase(it);
            }
        }
        // No global batch join: slow rooms keep their own worker busy while
        // networking and other rooms continue. Only one tick per room is in flight.
        now=nowMs();
        const uint64_t nowTickUs=nowUs();
        // Each running game's share of --sim-threads, recomputed as games start and
        // end; a tick takes the share of the moment it is submitted.
        const size_t running=runningGames();
        const unsigned simThreads=simShare(running);
        // With --sim-cpus, also when another game's end or start moves the CPUs.
        if(running && (simThreads!=simShareLogged_ || (!simCpus_.empty() && running!=simRunningLogged_)))logSimThreads(running);
        size_t games=0;
        for(auto& [id,room]:rooms_) {
            (void)id;
            const size_t game=room.playing()?games++:0;   // its turn among the running games
            // A pause or a wait is not a seat's hold: suspend those running (trackHolds).
            if(room.running && room.held())for(auto& seen:room.holdSeen)seen=0;
            if(!room.running || room.held() || room.tickJob.valid() ||
               room.resourceLimited || !room.simulationError.empty() ||
               !roomOccupied(room) || !roomActive(room) || room.nextTickUs()>nowTickUs)continue;
            trackHolds(room,now);
            if(!canAdvance(room)) {room.rebasePace(nowTickUs+kFlowRetryMs*1000);continue;}
            try {closeTick(room,simThreads,game);}
            catch(const std::exception& e) {
                room.simulationError=e.what();
                std::fprintf(stderr,"game %u tick submission failed: %s\n",id,e.what());
            }
        }
        // Feed catch-up streams. A client resuming or spectating takes its
        // history from the room log a chunk at a time, only topping up when its
        // write buffer has drained, so a long game cannot put its whole replay
        // into one socket buffer. The log keeps growing with live bundles, so
        // reaching the end IS being caught up -- at which point the live
        // broadcast takes over.
        constexpr size_t kReplayChunkBytes = 256u << 10;
        for (auto& [id, c] : clients_) {
            if (!c->replaying) continue;
            auto rit = rooms_.find(c->roomId);
            if (rit == rooms_.end()) { c->replaying = false; continue; }
            if (rit->second.mapPackage && c->mapReadyRoom != rit->second.id) continue;
            const auto& log = rit->second.log;
            while (c->replayPos < log.size() &&
                   c->conn.txPending() < kReplayChunkBytes)
                c->conn.send(Msg::TickBundle, log[c->replayPos++]);
            if (c->replayPos >= log.size()) c->replaying = false;
        }
        // Flush all pending writes (bundles just queued) + keepalive + timeouts.
        now = nowMs();
        size_t queued=0;
        std::vector<std::pair<size_t,uint32_t>> backlogs;
        for(const auto& [id,c]:clients_) {
            const auto bytes=c->conn.txPending();
            if(bytes>kMaxClientTxBytes) {c->conn.fail("send backlog exceeded");dead.push_back(id);}
            else {queued+=bytes;backlogs.emplace_back(bytes,id);}
        }
        std::sort(backlogs.rbegin(),backlogs.rend());
        for(const auto& [bytes,id]:backlogs) {
            if(queued<=kMaxServerTxBytes)break;
            clients_.at(id)->conn.fail("server send backlog exceeded");dead.push_back(id);queued-=bytes;
        }
        for (auto& [id, c] : clients_) {
            if(!c->conn.ok()) {dead.push_back(id);continue;}
            if(c->mapReceive.size && (now-c->mapReceiveProgress>30000 || now-c->mapReceiveStarted>300000)) {
                c->mapReceive={};c->conn.fail("map upload deadline exceeded");dead.push_back(id);continue;
            }
            if(c->modReceive.size && (now-c->modProgress>30000 || now-c->modStarted>300000)){
                c->modReceive={};c->conn.fail("game mod upload deadline exceeded");dead.push_back(id);continue;
            }
            c->modSend.pump(c->conn,Msg::ModChunk);
            c->mapSend.pump(c->conn);
            if (!c->conn.flushWrite()) { dead.push_back(id); continue; }
            if (c->state != Client::Handshake && now - c->lastRecvMs > kPingIdleMs &&
                now - c->lastPingMs > kPingIdleMs) {
                c->conn.send(Msg::Ping); c->lastPingMs = now; c->conn.flushWrite();
            }
            // Spectators (in a game, no seat) get a far longer grace than seated players.
            bool spectator = c->state == Client::InGame && c->slot < 0;
            if((c->state==Client::Handshake || c->state==Client::Auth) && now-c->connectedMs>kLoginDeadlineMs)
                {c->conn.fail("login deadline exceeded");dead.push_back(id);}
            // A seat the room is waiting for is the wait's to end (takeOver), not this
            // timeout's: closing it here would only restart nothing and lose the chance
            // of the same connection coming back.
            const Room* in = c->state == Client::InGame && c->slot >= 0 ? roomOf(*c) : nullptr;
            const bool waitedFor = in && c->slot < kMaxSlots && in->waitDeadline[c->slot] &&
                                   in->slotClient[c->slot] == int(id);
            // GameStarting sent, Loaded not yet received. Only in a RUNNING room: a seat in a
            // room that has not started is also unloaded, and the lobby keeps the play limit.
            const bool loading = in && in->running && !c->loaded;
            const uint64_t limit = spectator ? kSpectatorTimeoutMs : loading ? gLoadTimeoutMs : gTimeoutMs;
            if (!waitedFor && now - c->lastRecvMs > limit) {
                c->conn.fail(spectator ? "silent past the spectator timeout"
                             : loading ? "timed out while loading (" + std::to_string(gLoadTimeoutMs / 1000) + " s)"
                                       : "silent past the player timeout");
                dead.push_back(id);
            }
        }
        // Say WHY: a bare "disconnected" hid whether a drop was silence, a deadline or a
        // send backlog, all of which the client only sees as "peer closed".
        for (uint32_t id : dead) {
            const auto it = clients_.find(id);
            if (it == clients_.end()) continue;
            const std::string why = it->second->conn.error().empty() ? "disconnected" : it->second->conn.error();
            dropClient(id, why.c_str());
        }
    }
    return 0;
}

// Where map and game-mod uploads (MapCache/, ModCache/) and saved generated maps
// (Maps/) go without --map-cache-dir: a per-user folder, never the game data folder
// (--data is only read). XDG_DATA_HOME wins on every platform, as for the game.
std::string defaultMapCacheDir() {
    namespace fs = std::filesystem;
    fs::path base;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) base = ta::u8path(x);
#ifdef _WIN32
    else if (const wchar_t* a = _wgetenv(L"APPDATA"); a && *a) base = fs::path(a);
#elif defined(__APPLE__)
    else if (const char* h = std::getenv("HOME"); h && *h) base = ta::u8path(h) / "Library" / "Application Support";
#else
    else if (const char* h = std::getenv("HOME"); h && *h) base = ta::u8path(h) / ".local" / "share";
#endif
    if (base.empty()) return {};
    const auto text = (base / "TAengine" / "taserver").u8string();
    return {text.begin(), text.end()};
}

}  // namespace

static int serverMain(int argc, char** argv) {
    // Test overrides for the disconnect wait, the silence timeouts and an ended game's
    // wait for its watchers (milliseconds). The only TA_* variables a release build reads
    // (docs/public-server.md); every other hook goes through devEnv() and is debug-only.
    if (const char* g = std::getenv("TA_STALL_MS")) gStallMs = uint64_t(std::max(100ll, std::atoll(g)));
    if (const char* b = std::getenv("TA_DROP_WAIT_MS")) gDropWaitMs = uint64_t(std::max(100ll, std::atoll(b)));
    if (const char* t = std::getenv("TA_TIMEOUT_MS")) gTimeoutMs = uint64_t(std::max(500ll, std::atoll(t)));
    if (const char* t = std::getenv("TA_LOAD_TIMEOUT_MS")) gLoadTimeoutMs = uint64_t(std::max(500ll, std::atoll(t)));
    if (const char* t = std::getenv("TA_OVER_DRAIN_MS")) gOverDrainMs = uint64_t(std::max(500ll, std::atoll(t)));
    uint16_t port = 7677;
    std::string dataRoot, replayDir,tlsCert,tlsKey,mapRoot;
    ta::srv::AcmeOptions acme;acme.state="taserver-acme";
    bool acmeOption=false;
    bool allowTestWork=false;
    bool allowPlaintext=false;
    bool statusQuery=false;
    std::string accountsPath = "taserver-accounts.conf";
    uint32_t fixedSeed = 0;
    bool fixedSeedGiven = false;   // --seed 0 is a pinned seed, not "unset"
    ta::srv::Limits limits;
    int simThreads = 0;   // --sim-threads: 0 = auto, else the count asked for
    std::vector<int> simCpus;   // --sim-cpus: the CPUs to pin to (none: empty)
    bool noAuth = false, loopbackOnly = false, closedRegistration=false, lan=false;
    for (int i = 1; i < argc; ++i) {
        try {if(limits.set(argv[i],i+1<argc?argv[i+1]:"")) {++i;continue;}}
        catch(const std::exception& e) {std::fprintf(stderr,"taserver: %s: %s\n",argv[i],e.what());return 1;}
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            unsigned parsed = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 || parsed > 65535) {
                std::fprintf(stderr, "taserver: --port must be between 1 and 65535\n");
                return 1;
            }
            port = uint16_t(parsed);
        }
        else if (!std::strcmp(argv[i], "--status")) statusQuery=true;
        else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) dataRoot = argv[++i];
        else if (!std::strcmp(argv[i], "--replaydir") && i + 1 < argc) replayDir = argv[++i];
#ifndef NDEBUG
        else if (!std::strcmp(argv[i], "--allow-benchmarks")) allowTestWork=true;
#endif
        else if (!std::strcmp(argv[i], "--map-cache-dir") && i+1<argc)mapRoot=argv[++i];
        else if (!std::strcmp(argv[i], "--accounts") && i + 1 < argc) accountsPath = argv[++i];
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            { fixedSeed = uint32_t(std::strtoul(argv[++i], nullptr, 0)); fixedSeedGiven = true; }
        else if (!std::strcmp(argv[i], "--sim-threads") && i + 1 < argc) {
            // auto, or a count from 1; one above the CPU threads this process may use
            // means all of them (Server::setSimThreads).
            const std::string_view value(argv[++i]);
            unsigned parsed = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            const bool number = error != std::errc::invalid_argument && end == value.data() + value.size();
            if (value == "auto") simThreads = 0;
            else if (number && error == std::errc::result_out_of_range) simThreads = INT_MAX;
            else if (number && parsed >= 1) simThreads = int(std::min(parsed, unsigned(INT_MAX)));
            else {
                std::fprintf(stderr, "taserver: --sim-threads must be auto or a number of threads, 1 or more\n");
                return 1;
            }
        }
        else if (!std::strcmp(argv[i], "--cpus") && i + 1 < argc) {
            // Every thread of this server on these CPUs (a game's PIN TO CCDS puts its
            // local server on one CCD this way): set at once, before any thread starts.
            std::vector<int> cpus;
            if (!ta::sim::parseCpuList(argv[++i], cpus)) {
                std::fprintf(stderr, "taserver: --cpus must be a list of CPU numbers such as 8-15,24-31\n");
                return 1;
            }
            if (ta::sim::setProcessCpus(cpus))
                std::fprintf(stderr, "taserver: running on cpus %s\n", ta::sim::cpuRangesText(cpus).c_str());
            else
                std::fprintf(stderr, "taserver: could not keep this server to cpus %s; it runs on every CPU it may use\n",
                             ta::sim::cpuRangesText(cpus).c_str());
        }
        else if (!std::strcmp(argv[i], "--sim-cpus") && i + 1 < argc) {
            // none (the default), auto (this process's CPUs in the order threads take
            // them: performance cores, efficiency cores, hyperthreads; none where the
            // system cannot pin), or the CPU numbers themselves, in order.
            const std::string_view value(argv[++i]);
            simCpus.clear();
            if (value == "auto") {
                if (auto cpus = ta::sim::detectCpus(); cpus.canPin) simCpus = std::move(cpus.order);
            } else if (value != "none" && !ta::sim::parseCpuList(value, simCpus)) {
                std::fprintf(stderr, "taserver: --sim-cpus must be auto, none or a list of CPU numbers such as 1,3,5-7\n");
                return 1;
            } else if (!simCpus.empty() && !ta::sim::detectCpus().canPin) {
                std::fprintf(stderr, "taserver: this system does not let a program pin threads; --sim-cpus is ignored\n");
                simCpus.clear();
            } else {
                // A number this server may not use (mistyped, outside its cpuset) is
                // said here, once. It keeps its place in the list, as -1: the thread
                // of that place is not pinned, and the log lines leave it out.
                std::vector<int> outside;
                for (int& cpu : simCpus) {
                    if (ta::sim::usableCpu(cpu)) continue;
                    if (std::find(outside.begin(), outside.end(), cpu) == outside.end()) outside.push_back(cpu);
                    cpu = -1;
                }
                if (!outside.empty())
                    std::fprintf(stderr, "taserver: --sim-cpus: this server may not use cpus %s; a thread given one of them is not pinned\n",
                                 ta::sim::cpuListText(outside).c_str());
            }
        }
        else if (!std::strcmp(argv[i], "--no-auth")) noAuth = true;
        else if (!std::strcmp(argv[i], "--closed-registration")) closedRegistration=true;
        else if (!std::strcmp(argv[i], "--tls-cert") && i+1<argc) tlsCert=argv[++i];
        else if (!std::strcmp(argv[i], "--tls-key") && i+1<argc) tlsKey=argv[++i];
        else if (!std::strcmp(argv[i], "--acme-domain") && i+1<argc) {acme.domain=argv[++i];acmeOption=true;}
        else if (!std::strcmp(argv[i], "--acme-email") && i+1<argc) {acme.email=argv[++i];acmeOption=true;}
        else if (!std::strcmp(argv[i], "--acme-state") && i+1<argc) {acme.state=ta::u8path(argv[++i]);acmeOption=true;}
        else if (!std::strcmp(argv[i], "--acme-agree-tos")) {acme.agreeTerms=true;acmeOption=true;}
        else if (!std::strcmp(argv[i], "--acme-staging")) {acme.directory="https://acme-staging-v02.api.letsencrypt.org/directory";acmeOption=true;}
        else if (!std::strcmp(argv[i], "--allow-plaintext")) allowPlaintext=true;
        else if (!std::strcmp(argv[i], "--local")) loopbackOnly = true;
        else if (!std::strcmp(argv[i], "--lan")) { lan = true; noAuth = true; allowPlaintext = true; }
        else if (!std::strcmp(argv[i], "--version") || !std::strcmp(argv[i], "-v")) {
            std::printf("taserver (TA engine) %s (build %s)\n", ta::kVersion, ta::kBuildId);
            return 0;
        }
        else if (!std::strcmp(argv[i], "--help")) {
            std::printf("usage: taserver --data <retail-install-dir> [--port N]\n"
                        "                 [--replaydir <dir>] [--accounts <file>]\n"
                        "                 [--no-auth] [--local] [--lan] [--closed-registration]\n"
                        "                 [--tls-cert fullchain.pem --tls-key private.pem]\n"
                        "                 [--acme-domain hostname --acme-agree-tos]\n"
                        "                 [--acme-email address] [--acme-state directory] [--acme-staging]\n"
                        "                 [--allow-plaintext] (trusted LAN/test networks only)\n"
                        "                 [--map-cache-dir <writable directory>]\n"
                        "                 [--sim-threads <auto|N>] [--sim-cpus <list|auto|none>] [--cpus <list>]\n"
                        "                 [--max-games N] [--max-running-games N] [--max-accounts N]\n"
                        "                 [--map-memory-mib N] [--map-storage-mib N]\n"
                        "                 [--replay-memory-mib N] [--replay-storage-mib N]\n"
                        "       taserver --status [--port N]\n"
                        "  --status queries the local running server and prints JSON, then exits.\n"
                        "  No data, account, or TLS arguments are needed; default port is 7677.\n"
                        "  Status uses loopback-only UDP on the game port's numeric value.\n"
                        "  --data is REQUIRED when hosting: it is what the referee sim and the\n"
                        "  server-hosted AI players read; every game runs both.\n"
                        "  --replaydir writes a .tarep replay file per finished game.\n"
                        "  --map-cache-dir holds uploaded maps and game mods and saved generated\n"
                        "  maps (default: TAengine/taserver in the per-user data folder, e.g.\n"
                        "  ~/.local/share). The --data folder is never written.\n"
                        "  --sim-threads is how many CPU threads the running games' simulations\n"
                        "  share: auto (the default: every CPU thread the server may use) or a\n"
                        "  number from 1 (a number above those CPU threads means all of them).\n"
                        "  Each running game gets that total divided by the number of running\n"
                        "  games, rounded down, but at least 2 threads (1 when the total is 1).\n"
                        "  It never changes a game's outcome.\n"
                        "  --sim-cpus pins those simulation threads to CPUs: none (the default)\n"
                        "  pins nothing; a list such as 1,3,5-7 is the CPU numbers to use, in\n"
                        "  order; auto is every CPU the server may use, performance cores first,\n"
                        "  then efficiency cores, then hyperthreads. Each running game takes the\n"
                        "  next CPUs of the list, one per thread; threads beyond the end of the\n"
                        "  list are not pinned.\n"
                        "  --cpus keeps every thread of the server on those CPUs (a list such as\n"
                        "  8-15,24-31); --sim-threads auto then counts only them.\n"
                        "  --seed N (0 included) pins every game's RNG seed, so a headless run is\n"
                        "  repeatable. Games are otherwise seeded randomly (which is what\n"
                        "  makes Random Start Locations differ game to game).\n"
                        "  --accounts is the account file (default taserver-accounts.conf).\n"
                        "  Players sign in with a name and password; an unused name is\n"
                        "  registered on the spot. No password is stored or transmitted --\n"
                        "  see src/net/auth.h.\n"
                        "  --no-auth serves anyone who connects, with no account at all. Only\n"
                        "  for a private or LAN server; pair it with --local.\n"
                        "  --local binds loopback only, so nothing off this machine connects.\n"
                        "  --lan is a player's LOCAL SERVER: --no-auth --allow-plaintext on every\n"
                        "  interface, announcing its open game on the LAN (UDP 7678) each second.\n"
                        "  --closed-registration allows existing accounts only.\n"
                        "  Limits default to 16 games / 4 running, 10000 accounts, 512 MiB\n"
                        "  shared map memory, 4096 MiB map storage, 256 MiB replay memory\n"
                        "  per game and 4096 MiB replay storage. Limits must be positive.\n"
                        "  Benchmark/stress and local campaign missions require --local --no-auth.\n");
            return 0;
        } else {std::fprintf(stderr,"taserver: unknown option or missing value: %s\n",argv[i]);return 1;}
    }
    if (statusQuery) {
        ta::srv::StatusCounts counts;
        std::string error;
        if (!ta::srv::queryStatus(port, counts, error)) {
            std::fprintf(stderr, "taserver: %s\n", error.c_str());
            return 1;
        }
        std::fputs(counts.json().c_str(), stdout);
        return 0;
    }
    if (lan && (loopbackOnly || acmeOption || !tlsCert.empty())) {
        std::fprintf(stderr, "taserver: --lan is an unencrypted LAN server; it cannot be combined with --local or TLS.\n");
        return 1;
    }
    if(acmeOption && (acme.domain.empty() || !acme.agreeTerms || !tlsCert.empty() || !tlsKey.empty() || allowPlaintext || loopbackOnly || port==80)) {
        std::fprintf(stderr,"taserver: ACME requires --acme-domain and --acme-agree-tos; cannot combine with manual TLS, plaintext, --local, or game port 80.\n");return 1;
    }
    if(tlsCert.empty()!=tlsKey.empty() || (!loopbackOnly && tlsCert.empty() && !allowPlaintext && !acmeOption)) {
        std::fprintf(stderr,"taserver: public listeners require --tls-cert and --tls-key, or --acme-domain and --acme-agree-tos. Use --local for private games, or explicitly --allow-plaintext for a trusted LAN/test network.\n");return 1;
    }
    // --data is mandatory: the referee sim that gives the canonical hash and the AI
    // players both read it.
    if (dataRoot.empty()) {
        std::fprintf(stderr,
            "taserver: --data <retail-install-dir> is required.\n"
            "  It is what the referee sim and the AI players read. Point it at a\n"
            "  Total Annihilation install (the folder holding the root *.hpi and Maps/).\n"
            "  Run with --help for the full usage.\n");
        return 1;
    }
    if (mapRoot.empty()) mapRoot = defaultMapCacheDir();
    if (mapRoot.empty()) {
        std::fprintf(stderr, "taserver: no per-user folder for map downloads; pass --map-cache-dir <dir>.\n");
        return 1;
    }
    // --data is only read: uploads and saved generated maps go here, and every
    // mount lists them (hpi::userMapRoot).
    ta::hpi::setUserMapRoot(ta::u8path(mapRoot));
    Server s(port, dataRoot,mapRoot);
#ifndef NDEBUG
    if(allowTestWork)s.allowBenchmarks();
#else
    (void)allowTestWork;
#endif
    if (!replayDir.empty()) s.setReplayDir(replayDir);
    if (fixedSeedGiven) s.setFixedSeed(fixedSeed);
    s.setSimThreads(simThreads);
    s.setSimCpus(std::move(simCpus));
    s.setLimits(limits);
    if(acmeOption)s.setAcme(std::make_shared<ta::srv::AcmeCertificates>(acme));
    if(!tlsCert.empty())s.setTls(ta::net::TlsContext::server(tlsCert,tlsKey));
    if (loopbackOnly) s.setLoopbackOnly();
    if (lan) s.setLan();
    if (closedRegistration) s.closeRegistration();
    if (noAuth) {
        s.setNoAuth();
    } else {
        std::string err;
        if (!s.loadAccounts(accountsPath, err)) {
            // Refuse to start rather than come up looking like a server with
            // accounts while actually having none of them.
            std::fprintf(stderr, "taserver: %s\n", err.c_str());
            return 1;
        }
    }
    return s.run();
}

int main([[maybe_unused]] int argc,[[maybe_unused]] char** argv) {   // Windows reads its own wide arguments
    try {
#ifdef _WIN32
        return ta::utf8Main(serverMain);
#else
        return serverMain(argc,argv);
#endif
    } catch(const std::exception& e) {std::fprintf(stderr,"taserver: %s\n",e.what());return 1;}
}
