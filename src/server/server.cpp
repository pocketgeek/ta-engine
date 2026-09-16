// taserver: the central multiplayer server (docs/multiplayer-design.md, M3).
//
// A single-threaded poll loop hosts a lobby and any number of games. Each game
// is a server-sequenced deterministic lockstep: clients send commands, the
// server buckets them per tick, closes one tick every 1/30 s, and broadcasts a
// TickBundle to every client in the game. Clients advance their sims in step.
//
// With --data the server runs a REFEREE sim (a headless World built by the same
// setupMatch the clients use) that also hosts the AI players: each tick the AI
// controllers append their orders to the bundle, and the referee's own state
// hash is the canonical one clients are checked against (with a suspicion rule
// that blames the referee if every client agrees against it). Without --data the
// server is a pure relay and clients cross-check hashes among themselves (M3).

#include "net/netcompat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ai/ai.h"
#include "hpi/hpi.h"
#include "net/auth.h"
#include "net/crypto.h"
#include "server/accounts.h"
#include "tdf/tdf.h"
#include "net/conn.h"
#include "net/protocol.h"
#include "net/replayhdr.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "version.h"

using namespace ta::net;

namespace {

uint64_t nowMs() {   // monotonic wall-clock (tick pacing / timeouts; never hashed)
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

constexpr uint64_t kPingIdleMs = 5000;    // ping a quiet client after this
constexpr uint64_t kTimeoutMs = 15000;    // drop a silent seated player after this
// A SPECTATOR is display-only and non-authoritative: it holds up nobody (canAdvance
// paces to it only in an all-AI game, and even then a stale ack just slows the sim,
// never desyncs it). A busy client render loop -- e.g. a VRAM-starved frame that
// freezes for several seconds at a big supersampled resolution -- stops pumping the
// socket for that whole frame, so a 15s player timeout evicts a spectator that is
// merely hitching. Give spectators a much longer grace so a transient stall doesn't
// tear the connection down (the client's own self-timeout below is widened to match).
constexpr uint64_t kSpectatorTimeoutMs = 120000;
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
uint64_t kGraceMs = 300000;     // hold a dropped slot this long (5 min)
uint64_t kPauseBudgetMs = 120000;  // total auto-pause a player may cause

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
    uint64_t lastPingMs = 0;
    bool loaded = false;
    uint32_t ackTick = 0;   // latest tick this client reported a hash for (flow control)
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
    // Catch-up streaming. A resuming or spectating client needs every bundle
    // logged so far, which for a long game is far more than belongs in one
    // socket buffer -- pushing it all at once put the entire replay in memory
    // and handed one client an unbounded write queue. Instead we hold a cursor
    // and feed it as the buffer drains. Every live bundle is appended to r.log
    // too, so the cursor catches up to the present by itself; until it does,
    // this client is skipped by the live TickBundle broadcast so the stream
    // cannot go out of order.
    bool replaying = false;
    size_t replayPos = 0;        // next index into Room::log to send
};

// pausePlayer sentinel for a player-REQUESTED pause: distinct from any real slot,
// so the disconnect budget sweep and the rejoin-resume path both ignore it.
static constexpr int kPauseByRequest = -2;

struct Room {
    uint32_t id = 0;
    std::string name, password, mapId;
    std::string mission;           // campaign mission stem (empty = ordinary skirmish/MP)
    GameOptions opts;
    uint32_t hostId = 0;
    SlotInfo slots[kMaxSlots];
    int slotClient[kMaxSlots];      // client id in each slot, -1 = none/ai/open
    std::vector<uint32_t> spectators;   // watching, not seated (no slot, no hash)
    bool running = false;
    bool priv = false;             // private (single-player): hidden from the game list
    int cap = kMaxSlots;            // map capacity (from the host's CreateGame)
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
    std::map<uint32_t, std::vector<Command>> pendingAt;  // server input-delay: cmds bucketed by future tick
    std::vector<Event> pendingEvents;           // sequenced events for the next tick
    uint64_t nextTickMs = 0;                    // wall deadline for the next tick
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
    std::vector<std::pair<uint32_t, uint64_t>> replayChecks;
    bool refSuspect = false;                    // referee itself suspected desynced
    int8_t missionOutcomeSent = 0;              // campaign result already broadcast (0 = none)
    // durability (M5): the full bundle log for reconnect/replay, per-slot resume
    // tokens, and drop-hold / auto-pause state.
    std::vector<std::vector<uint8_t>> log;      // serialized TickBundle payload per tick
    SlotInfo startSlots[kMaxSlots];             // slot config at game start (for the replay)
    uint64_t slotToken[kMaxSlots] = {};         // resume token per human slot (0 = none)
    bool slotDropped[kMaxSlots] = {};           // slot held after a mid-game disconnect
    uint64_t graceDeadline[kMaxSlots] = {};     // forfeit time for a dropped slot
    bool paused = false;                        // a drop paused the game (ticks stop)
    int pausePlayer = -1;                       // which dropped player paused it
    uint64_t pauseStartMs = 0;                  // when the current pause began
    uint64_t pauseBudgetMs[kMaxSlots] = {};     // remaining pause budget per player
    Room() { for (int i = 0; i < kMaxSlots; ++i) slotClient[i] = -1; }
    int capacity() const { return cap; }
    int usedSlots() const {
        int n = 0;
        for (int i = 0; i < kMaxSlots; ++i) if (slots[i].type == 1 || slots[i].type == 2) ++n;
        return n;
    }
};

// A tiny persistent worker pool: `run(count, fn)` invokes fn(0..count-1) across the
// workers PLUS the calling thread, and blocks until all have finished. Used to tick
// several independent games in parallel (each fn(i) drives one room's referee sim).
// Persistent so the frequent per-tick dispatch never pays thread-creation churn.
class WorkerPool {
public:
    explicit WorkerPool(unsigned workers) {
        for (unsigned i = 0; i < workers; ++i)
            threads_.emplace_back([this] { workerLoop(); });
    }
    ~WorkerPool() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; ++gen_; }
        cvStart_.notify_all();
        for (auto& t : threads_) t.join();
    }
    void run(size_t count, const std::function<void(size_t)>& fn) {
        if (count == 0) return;
        if (threads_.empty()) { for (size_t i = 0; i < count; ++i) fn(i); return; }
        {
            std::lock_guard<std::mutex> lk(m_);
            fn_ = &fn; count_ = count; cursor_.store(0); finished_ = 0; ++gen_;
        }
        cvStart_.notify_all();
        drain();                                   // the caller participates too
        // Wait until every WORKER has left drain() (not merely until the last item
        // finished): only then is it safe for the next run() to reset the cursor.
        std::unique_lock<std::mutex> lk(m_);
        cvDone_.wait(lk, [this] { return finished_ == threads_.size(); });
        fn_ = nullptr;
    }
private:
    void drain() {
        for (;;) {
            size_t i = cursor_.fetch_add(1, std::memory_order_relaxed);
            if (i >= count_) break;
            (*fn_)(i);
        }
    }
    void workerLoop() {
        uint64_t seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> lk(m_);
            cvStart_.wait(lk, [this, seen] { return stop_ || gen_ != seen; });
            if (stop_) return;
            seen = gen_;
            lk.unlock();
            drain();
            std::lock_guard<std::mutex> lk2(m_);
            if (++finished_ == threads_.size()) cvDone_.notify_all();
        }
    }
    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable cvStart_, cvDone_;
    const std::function<void(size_t)>* fn_ = nullptr;
    size_t count_ = 0;
    std::atomic<size_t> cursor_{0};
    size_t finished_ = 0;
    uint64_t gen_ = 0;
    bool stop_ = false;
};

class Server {
public:
    void setReplayDir(const std::string& d) { replayDir_ = d; }
    // Pin every game's RNG seed (--seed). Games are otherwise seeded randomly, which
    // is what makes "Random Start Locations" actually random -- but it also makes a
    // harness run unrepeatable, and the headless --mpai check exists precisely to
    // produce the same state hash twice. Not for a real server.
    void setFixedSeed(uint32_t v) { fixedSeed_ = v; }
    void setNoAuth() { requireAuth_ = false; }
    void setLoopbackOnly() { loopbackOnly_ = true; }
    // Load (or start) the account file. Returns false with `err` set if it exists
    // but cannot be read -- starting anyway would mean running an open server
    // while looking like a closed one.
    bool loadAccounts(const std::string& path, std::string& err) {
        return accounts_.load(path, &err);
    }
    size_t accountCount() const { return accounts_.size(); }
    // `dataRoot` is never empty -- main() refuses to start without --data.
    Server(uint16_t port, const std::string& dataRoot) : port_(port), dataRoot_(dataRoot) {
        // The referee reads the retail install directly, per the ROOM's override
        // tier. Build the pure-retail set eagerly (the connection-level Hello
        // check + the common none/cosmetic game); the Full set is built lazily
        // the first time a Full game starts.
        buildDataSet(retail_, ta::hpi::OverridePolicy::None);
        aiProfile_ = ta::ai::loadProfile(retail_.vfs);
        aiNames_ = loadAiNames(retail_.vfs);
        haveCb_ = retail_.haveCb;
        std::fprintf(stderr, "taserver: loaded game data from %s (referee sim + AI%s), "
                     "retail gameplay hash %016llx\n",
                     dataRoot_.c_str(), haveCb_ ? ", +Crusades" : "",
                     (unsigned long long)retail_.hash);
    }
    int run();

private:
    uint16_t port_;
    std::string dataRoot_;
    // Accounts. requireAuth_ is the default; --no-auth turns it off for a private
    // or LAN server (single-player launches one of those).
    bool requireAuth_ = true;
    bool loopbackOnly_ = false;
    ta::srv::AccountStore accounts_;
    ta::srv::LoginThrottle throttle_;
    // A mounted data set at one override tier: the VFS, its base + Crusades
    // registries, and the gameplay-data fingerprint peers are held to.
    struct DataSet {
        ta::hpi::Vfs vfs;
        ta::sim::TypeRegistry reg, regCb;
        bool haveCb = false;
        uint64_t hash = 0;
        bool built = false;
    };
    DataSet retail_, full_;            // none/cosmetic use retail_; full uses full_
    bool haveCb_ = false;
    ta::ai::Profile aiProfile_;
    // Per-mission build profiles (ai/<name>.txt), cached by name -- a Controller
    // holds a reference to its Profile, so these must outlive the room.
    std::map<std::string, ta::ai::Profile> missionProfiles_;
    const ta::ai::Profile& missionProfile(const ta::hpi::Vfs& vfs, const std::string& name) {
        auto it = missionProfiles_.find(name);
        if (it != missionProfiles_.end()) return it->second;
        return missionProfiles_.emplace(name, ta::ai::loadProfile(vfs, name)).first->second;
    }
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
    void buildDataSet(DataSet& ds, ta::hpi::OverridePolicy pol) {
        ds.vfs = ta::hpi::mountRetailRoot(dataRoot_, pol);
        ta::sim::setupRegistry(ds.reg, ds.vfs, false);
        if (!ds.vfs.list("unitscb").empty()) {
            ta::sim::setupRegistry(ds.regCb, ds.vfs, true);
            ds.haveCb = true;
        }
        ds.hash = ta::hpi::gameplayHash(ds.vfs);
        ds.built = true;
    }
    // The data set a game runs under, by its override policy (0/1 = retail, 2 = full).
    DataSet& dataFor(uint8_t policy) {
        DataSet& ds = (policy == 2) ? full_ : retail_;
        if (!ds.built) buildDataSet(ds, policy == 2 ? ta::hpi::OverridePolicy::Full
                                                    : ta::hpi::OverridePolicy::None);
        return ds;
    }
    const ta::sim::TypeRegistry& registryFor(bool crusades, uint8_t policy) {
        DataSet& ds = dataFor(policy);
        return (crusades && ds.haveCb) ? ds.regCb : ds.reg;
    }
    std::string replayDir_;
    uint32_t fixedSeed_ = 0;           // --seed: 0 = roll one per game
    int listenFd_ = -1;
    uint32_t nextClientId_ = 1, nextRoomId_ = 1;
    std::unordered_map<uint32_t, std::unique_ptr<Client>> clients_;
    std::map<uint32_t, Room> rooms_;
    // Worker pool for ticking several concurrent games in parallel (one referee sim
    // per thread). Sized a little under the core count; idle when only one game runs.
    WorkerPool tickPool_{[] {
        unsigned h = std::thread::hardware_concurrency();
        return std::min(h > 1 ? h - 1 : 1u, 7u);
    }()};

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
    void sendAuthResult(Client& c, AuthStatus st, const ta::crypto::Digest* sig,
                        const std::string& msg);
    void lobbyMsg(Client& c, const Frame& f);
    void gameMsg(Client& c, const Frame& f);

    void sendReject(Client& c, const std::string& why);
    void sendGameList(Client& c);
    void broadcastLobby(Room& r);
    void broadcastRoom(Room& r, Msg kind, const Writer& w, uint32_t exceptClient = 0);
    Room* roomOf(Client& c) { auto it = rooms_.find(c.roomId); return it == rooms_.end() ? nullptr : &it->second; }
    void dropPendingCommands(Client& c, Room& r);   // both disconnect paths
    void leaveRoom(Client& c, const char* reason);
    void tryStart(Client& c);
    void closeTick(Room& r);
    bool canAdvance(const Room& r) const;   // false = wait for a lagging player (flow control)
    void checkHashes(Room& r, uint32_t tick);
    void dropClient(uint32_t id, const char* reason);
    void writeSlots(Writer& w, Room& r, bool fromStart = false);
    void writeReplay(Room& r);
};

// A running game is abandoned once nobody is left to watch it. Normally that means
// no human slot has a live or held (dropped-within-grace) client. A game created by
// a SPECTATOR host to watch the AIs fight (single-player spectate / TA_MP_WATCH) has
// NO human slots at all, so it also stays alive while a spectator is connected and at
// least one AI is still playing -- otherwise the server would tear it down the instant
// it started.
static bool roomActive(const Room& r) {
    bool aiPresent = false;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (r.slots[i].type == 1 && (r.slotClient[i] >= 0 || r.slotDropped[i])) return true;
        if (r.slots[i].type == 2) aiPresent = true;
    }
    return aiPresent && !r.spectators.empty();
}

// True while SOMEBODY is still connected to the room -- a live seated player or a
// spectator. A HELD slot does not count: holding it is what lets a player rejoin,
// but if every other human has gone too then nobody is left watching, and the room
// would otherwise keep its referee sim and AIs running for the whole drop grace
// (or until the auto-pause budget ran out and it resumed at full speed) with not a
// single client attached. Games are meant to stop when the last human leaves, so a
// room that empties is torn down immediately rather than ticking on unattended.
// Note this deliberately gives up the rejoin window when the LAST player drops:
// there is nobody for a paused game to be unfair to, and a ghost game costs the
// server a sim + AI per room.
static bool roomOccupied(const Room& r) {
    for (int i = 0; i < kMaxSlots; ++i)
        if (r.slotClient[i] >= 0) return true;
    return !r.spectators.empty();
}

// Builds the whole replay into a second buffer and writes it synchronously on the
// server loop, so a teardown does stall every other room for the duration. Measured
// before worrying about it: a 3600-tick game is 58 KB and 2.9 ms end to end. Even at
// 20x that it is under 60 ms, once, at room teardown. Left synchronous on purpose --
// an async writer here would be complexity bought against a cost I could not measure.
// Worth revisiting only if replays get much larger (many seated humans issuing orders
// every tick) or the store is slow.
void Server::writeReplay(Room& r) {
    if (replayDir_.empty() || r.log.empty()) return;
    // Self-contained replay: header (see net/replayhdr.h -- one definition, shared
    // with the client writer and the loader), every tick bundle, then the referee's
    // recorded hash checkpoints. A viewer can rebuild the world, play it back, and
    // compare its own hashes against what actually happened.
    ta::net::ReplayHeader h;
    h.mapId = r.mapId;
    h.mission = r.mission;
    h.engineVersion = ta::kVersion;
    h.crusades = r.opts.crusades;
    h.gods = r.opts.gods;
    h.forfeitSelfDestruct = r.opts.forfeitSelfDestruct;
    h.overridePolicy = r.opts.overridePolicy;
    h.unitCap = r.opts.unitCap;
    h.monarchExpendable = r.opts.monarchExpendable;
    h.stressTest = r.opts.stressTest;
    h.randomStarts = r.opts.randomStarts;
    h.benchmark = uint8_t(r.opts.benchmark);
    h.seed = r.seed;
    h.dataHash = dataFor(r.opts.overridePolicy).hash;
    for (int i = 0; i < kMaxSlots; ++i) {
        const SlotInfo& s = r.startSlots[i];   // start config, not the forfeited end state
        h.slotType[i] = s.type;
        h.slotFaction[i] = s.faction;
        h.slotColor[i] = s.color;
        h.slotTeam[i] = s.team;
        h.slotAiLevel[i] = s.aiLevel;
    }
    Writer w;
    ta::net::writeReplayHeader(w, h);
    w.u32(uint32_t(r.log.size()));
    for (const auto& b : r.log) { w.u32(uint32_t(b.size())); w.b.insert(w.b.end(), b.begin(), b.end()); }
    // The referee's hash trail. Two identical reruns only prove the reruns agree;
    // this is what lets a replay say where it diverged from the real game.
    w.u32(uint32_t(r.replayChecks.size()));
    for (const auto& [tk, hs] : r.replayChecks) { w.u32(tk); w.u64(hs); }
    std::string path = replayDir_ + "/game-" + std::to_string(r.id) + "-" +
                       std::to_string(r.createdMs) + ".tarep";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(w.b.data(), 1, w.b.size(), f);
        std::fclose(f);
        std::fprintf(stderr, "game %u: wrote replay %s (%zu ticks, %zu bytes)\n",
                     r.id, path.c_str(), r.log.size(), w.b.size());
    }
}

void Server::sendReject(Client& c, const std::string& why) {
    Writer w; w.str(why);
    c.conn.send(Msg::Reject, w);
    // Every caller fail()s/closes the connection right after: push the reason out
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
    if (!r.ok) { sendReject(c, "malformed hello"); c.conn.fail("bad hello"); return; }
    if (ver != kNetVersion) {
        sendReject(c, "protocol version mismatch (server " + std::to_string(kNetVersion) +
                       ", client " + std::to_string(ver) + ") -- update your build");
        c.conn.fail("version");
        return;
    }
    // Gameplay-data agreement: every peer must feed its sim byte-identical gameplay
    // data (verifies the retail files are unmodified, and that Full-override players
    // share the same overrides). The referee's own hash is the authority -- there is
    // no relay mode adopting the first client's any more, which could only ever hold
    // peers to whatever the earliest arrival happened to have.
    // The Hello hash is the client's PURE-RETAIL gameplay fingerprint (mounted with
    // no overrides), so the base game files must match regardless of anyone's
    // override tier. (Full-tier gameplay overrides are checked per game at Loaded.)
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
    Writer w; w.u32(c.id); w.str(c.name);
    c.conn.send(Msg::Welcome, w);
    std::fprintf(stderr, "client %u '%s' joined lobby\n", c.id, c.name.c_str());
}

void Server::sendAuthResult(Client& c, AuthStatus st, const ta::crypto::Digest* sig,
                            const std::string& msg) {
    Writer w;
    w.u8(uint8_t(st));
    if (sig) w.bytes(sig->data(), sig->size()); else w.bytes(nullptr, 0);
    w.str(msg);
    c.conn.send(Msg::AuthResult, w);
}

// The login exchange. Everything here runs BEFORE the client can reach the lobby,
// so the only frames entertained are the three login ones; anything else drops
// the connection rather than being queued up for later.
void Server::authMsg(Client& c, const Frame& f) {
    Reader r(f.payload.data(), f.payload.size());
    const uint64_t now = nowMs();

    if (f.kind == Msg::AuthBegin) {
        if (c.pendAuth.challenged) { sendReject(c, "duplicate login"); c.conn.fail("dup auth"); return; }
        std::string user = r.str();
        std::vector<uint8_t> cnonce = r.bytes(ta::auth::kNonceLen);
        if (!r.ok) { sendReject(c, "malformed login"); c.conn.fail("bad auth"); return; }

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
        if (!r.ok) { sendReject(c, "malformed login proof"); c.conn.fail("bad auth"); return; }
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
        c.account = a->name;
        c.name = a->name;
        std::string err;
        if (!accounts_.noteLogin(a->name, &err))
            std::fprintf(stderr, "taserver: could not record login: %s\n", err.c_str());
        sendAuthResult(c, AuthStatus::Ok, &sig, "signed in");
        std::fprintf(stderr, "client %u (%s) signed in as '%s'\n",
                     c.id, c.peer.c_str(), c.name.c_str());
        sendWelcome(c);
        return;
    }

    if (f.kind == Msg::AuthRegister) {
        if (!c.pendAuth.challenged || !c.pendAuth.newAccount) {
            sendReject(c, "unexpected registration"); c.conn.fail("bad auth"); return;
        }
        std::vector<uint8_t> stored = r.bytes(ta::crypto::kHashLen);
        std::vector<uint8_t> serverKey = r.bytes(ta::crypto::kHashLen);
        if (!r.ok) { sendReject(c, "malformed registration"); c.conn.fail("bad auth"); return; }

        // Registration is the one unauthenticated operation that WRITES, and each
        // one rewrites the whole account file on the tick thread -- so a flood is
        // both a disk-filling attack and a way to stall every running game. Meter
        // it per address on the same escalating curve as a failed password.
        const std::string ipKey = ipKeyFor(c);
        if (uint64_t wait = throttle_.lockedFor(ipKey, now)) { sendThrottled(c, wait); return; }
        throttle_.fail(ipKey, now, ta::srv::LoginThrottle::kAddress);

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
        sendAuthResult(c, AuthStatus::Created, &sig, "new account created");
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
    // Private (single-player) games are not advertised in the browser.
    uint32_t n = 0;
    for (auto& [id, r] : rooms_) if (!r.priv) ++n;
    w.u32(n);
    for (auto& [id, r] : rooms_) {
        if (r.priv) continue;
        w.u32(r.id);
        w.str(r.name);
        w.str(r.mapId);
        w.u8(uint8_t(r.usedSlots()));
        w.u8(uint8_t(r.capacity()));
        w.u8(r.running ? 1 : 0);
        w.u8(r.password.empty() ? 0 : 1);
        w.u32(uint32_t((t - r.createdMs) / 1000));
    }
    c.conn.send(Msg::GameList, w);
}

// Snap an incoming unit-cap value to the allowed lobby set (defensive against a
// malformed client); anything unexpected falls back to the 2000 default.
static uint16_t clampUnitCap(uint16_t v) {
    for (uint16_t a : {250, 500, 1000, 2000, 5000}) if (v == a) return v;
    return 2000;
}

// `fromStart` writes the table the game BEGAN with instead of the live one. A
// rejoining client replays the whole bundle log from tick 0, so it must rebuild the
// world from the START configuration: hand it the current table and a slot closed
// since (a forfeit, or a drop that ran out its budget) reads as unused, so that
// player's opening units never spawn and the rebuilt world diverges from the game it
// is rejoining -- and any replay that client saves records the wrong match.
void Server::writeSlots(Writer& w, Room& r, bool fromStart) {
    w.u32(r.id);
    w.str(r.name);
    w.str(r.mapId);
    w.str(r.mission);
    w.u8(r.opts.crusades); w.u8(r.opts.gods); w.u8(r.opts.forfeitSelfDestruct);
    w.u8(r.opts.overridePolicy);
    w.u8(r.opts.speed); w.u8(r.opts.speedUnlock); w.u32(r.opts.unitCap); w.u8(r.opts.monarchExpendable);
    w.u8(r.opts.stressTest); w.u8(r.opts.fogExplored); w.u8(r.opts.benchmark);
    w.u8(r.opts.randomStarts);
    w.u32(r.hostId);
    for (int i = 0; i < kMaxSlots; ++i) {
        const SlotInfo& s = (fromStart && r.running) ? r.startSlots[i] : r.slots[i];
        w.u8(s.type); w.u8(s.faction); w.u8(s.color); w.u8(s.team); w.u8(s.ready);
        w.u8(s.aiLevel);
        w.str(s.name);
    }
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

void Server::lobbyMsg(Client& c, const Frame& f) {
    switch (f.kind) {
        case Msg::ListGames: sendGameList(c); break;
        case Msg::CreateGame: {
            Reader r(f.payload.data(), f.payload.size());
            std::string name = r.str(), pass = r.str(), mapId = r.str(), mission = r.str();
            GameOptions o; o.crusades = r.u8(); o.gods = r.u8(); o.forfeitSelfDestruct = r.u8();
            o.overridePolicy = r.u8();
            o.speed = r.u8(); o.speedUnlock = r.u8();
            if (o.speed < 1) o.speed = 10;
            o.unitCap = clampUnitCap(uint16_t(r.u32()));
            o.monarchExpendable = r.u8() ? 1 : 0;
            o.stressTest = r.u8() ? 1 : 0;
            // THREE-state, not a bool: 0 = not explored, 1 = explored, 2 = full
            // vision. Decoding it as `? 1 : 0` silently folded FULL VISION back to
            // EXPLORED, so the room's fog button cycled through a setting the
            // server threw away -- it looked like the button did nothing.
            o.fogExplored = std::min<uint8_t>(r.u8(), 2);
            o.benchmark = r.u8();
            o.randomStarts = r.u8() ? 1 : 0;
            int cap = int(r.u8());
            uint8_t spectate = r.u8();   // host watches, taking no slot (all-AI game)
            uint8_t priv = r.u8();       // private (single-player): hidden from the list
            if (!r.ok) return;
            if (cap < 2 || cap > kMaxSlots) cap = kMaxSlots;   // sane default
            Room& room = rooms_[nextRoomId_];
            room.id = nextRoomId_++;
            room.name = name.empty() ? ("game" + std::to_string(room.id)) : name;
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
            room.seed = fixedSeed_ ? fixedSeed_ : uint32_t(randToken());
            // Open slots up to the map capacity, close the rest (the map has no
            // start position for them).
            for (int i = 0; i < kMaxSlots; ++i) {
                room.slots[i].type = i < cap ? 0 : 3;
                room.slots[i].color = uint8_t(i);
                room.slots[i].team = uint8_t(i);
            }
            if (spectate) {
                // Host watches without a slot -- every capacity slot stays open (fill
                // with AIs). The host still owns the room (add AIs, start).
                room.spectators.push_back(c.id);
                c.cmdQueue.clear();   // never inherit a previous room's queue
            c.state = Client::InGame; c.roomId = room.id; c.slot = -1;
                Writer jr; jr.u8(1); jr.u8(0xFF); jr.str("");   // ok, spectator
                c.conn.send(Msg::JoinResult, jr);
            } else {
                // Host takes slot 0 (human).
                room.slots[0].type = 1; room.slots[0].faction = 0; room.slots[0].color = 0;
                room.slots[0].team = 0; room.slots[0].name = c.name;
                room.slotClient[0] = int(c.id);
                c.cmdQueue.clear();   // never inherit a previous room's queue
            c.state = Client::InGame; c.roomId = room.id; c.slot = 0;
                Writer jr; jr.u8(1); jr.u8(0); jr.str("");   // ok, slot 0
                c.conn.send(Msg::JoinResult, jr);
            }
            broadcastLobby(room);
            std::fprintf(stderr, "client %u created game %u '%s'\n", c.id, room.id, room.name.c_str());
            break;
        }
        case Msg::JoinGame: {
            Reader r(f.payload.data(), f.payload.size());
            uint32_t gid = r.u32(); std::string pass = r.str();
            if (!r.ok) return;
            auto it = rooms_.find(gid);
            auto reject = [&](const std::string& why) {
                Writer jr; jr.u8(0); jr.u8(0); jr.str(why); c.conn.send(Msg::JoinResult, jr);
            };
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
            room.slotClient[freeSlot] = int(c.id);
            c.cmdQueue.clear();   // never inherit a previous room's queue
            c.state = Client::InGame; c.roomId = room.id; c.slot = freeSlot;
            Writer jr; jr.u8(1); jr.u8(uint8_t(freeSlot)); jr.str("");
            c.conn.send(Msg::JoinResult, jr);
            broadcastLobby(room);
            std::fprintf(stderr, "client %u joined game %u at slot %d\n", c.id, room.id, freeSlot);
            break;
        }
        case Msg::Rejoin: {
            Reader r(f.payload.data(), f.payload.size());
            uint32_t gid = r.u32(); uint64_t token = r.u64();
            if (!r.ok) return;
            auto it = rooms_.find(gid);
            auto reject = [&](const std::string& why) {
                Writer jr; jr.u8(0); jr.u8(0); jr.str(why); c.conn.send(Msg::JoinResult, jr);
            };
            if (it == rooms_.end() || !it->second.running) { reject("game not found or ended"); break; }
            Room& room = it->second;
            int slot = -1;
            for (int i = 0; i < kMaxSlots; ++i)
                if (room.slotDropped[i] && token != 0 && room.slotToken[i] == token) { slot = i; break; }
            if (slot < 0) { reject("invalid or expired resume token"); break; }
            // Re-seat the client and rotate the token (single use).
            room.slotClient[slot] = int(c.id);
            room.slotDropped[slot] = false;
            room.slotToken[slot] = randToken();
            room.desyncFlagged.clear();   // fresh sim; old desync flags are stale
            c.cmdQueue.clear();   // never inherit a previous room's queue
            c.state = Client::InGame; c.roomId = gid; c.slot = slot; c.loaded = true;
            // GameStarting rebuilds the client's world; then the whole bundle log
            // replays it up to now, after which it receives live bundles. Because that
            // rebuild starts at tick 0, it needs the START slot table -- see writeSlots.
            Writer w; writeSlots(w, room, /*fromStart=*/true);
            w.u8(uint8_t(slot)); w.u32(room.seed); w.u64(room.slotToken[slot]);
            // Where the HISTORY ends. The client must not send -- or treat its own
            // commands coming back as acknowledgements -- until it has consumed
            // every bundle logged before it rejoined. It cannot work that out
            // locally: the replay is streamed in chunks paced by the socket, so its
            // receive buffer legitimately runs dry BETWEEN chunks while history is
            // still coming, and a buffer-depth guess releases the gate early.
            w.u32(uint32_t(room.log.size()));
            c.conn.send(Msg::GameStarting, w);
            c.replaying = true; c.replayPos = 0;   // streamed below, paced by txPending()
            // Unpause if this was the player we were waiting on.
            if (room.paused && room.pausePlayer == slot) {
                room.pauseBudgetMs[slot] -= std::min(room.pauseBudgetMs[slot], nowMs() - room.pauseStartMs);
                room.paused = false; room.pausePlayer = -1;
                room.nextTickMs = nowMs();   // rebase the tick clock (no burst)
                Writer rw; rw.u8(0); rw.u8(uint8_t(slot));
                broadcastRoom(room, Msg::Resume, rw);
            }
            Writer ps; ps.u8(uint8_t(slot)); ps.u8(0 /*connected*/); ps.u32(0);
            broadcastRoom(room, Msg::PlayerStatus, ps, c.id);
            std::fprintf(stderr, "game %u: client %u REJOINED slot %d (replaying %zu ticks)\n",
                         room.id, c.id, slot, room.log.size());
            break;
        }
        case Msg::Spectate: {
            Reader r(f.payload.data(), f.payload.size());
            uint32_t gid = r.u32(); std::string pass = r.str();
            if (!r.ok) return;
            auto it = rooms_.find(gid);
            auto reject = [&](const std::string& why) {
                Writer jr; jr.u8(0); jr.u8(0); jr.str(why); c.conn.send(Msg::JoinResult, jr);
            };
            if (it == rooms_.end() || !it->second.running) { reject("game not found or ended"); break; }
            Room& room = it->second;
            if (!room.password.empty() && room.password != pass) { reject("wrong password"); break; }
            // Seat nothing: a spectator has no slot, sends no commands or hashes,
            // and holds nothing on disconnect. It just receives the stream.
            if (std::find(room.spectators.begin(), room.spectators.end(), c.id) == room.spectators.end())
                room.spectators.push_back(c.id);
            c.cmdQueue.clear();   // never inherit a previous room's queue
            c.state = Client::InGame; c.roomId = gid; c.slot = -1; c.loaded = true;
            // GameStarting (slot 0xFF = spectator) rebuilds the world; the whole
            // bundle log replays it to now, then live bundles stream via broadcast.
            // Same reason as the rejoin above: that rebuild starts at tick 0, so it
            // needs the START table, not one with mid-game forfeits already applied.
            Writer w; writeSlots(w, room, /*fromStart=*/true);
            w.u8(0xFF); w.u32(room.seed); w.u64(0);
            w.u32(uint32_t(room.log.size()));   // replay boundary (see above)
            c.conn.send(Msg::GameStarting, w);
            c.replaying = true; c.replayPos = 0;   // streamed below, paced by txPending()
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
// several ticks out, which puts them in bundles AFTER the replay boundary a
// rejoin is handed -- so they arrive looking like acknowledgements for commands
// sent after the rejoin. Safe to purge: nothing here has been bundled or
// broadcast yet, so every peer still agrees. A player who just dropped should not
// have orders fire seconds later in any case.
//
// Called from BOTH disconnect paths. It first lived only in leaveRoom, which a
// seated player disconnecting from a running game never reaches -- that takes the
// slot-preserving branch of dropClient, which is exactly the rejoin path this is
// meant to protect. The fix ran everywhere except where it was needed.
void Server::dropPendingCommands(Client& c, Room& r) {
    c.cmdQueue.clear();
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
    // A spectator just detaches from the stream -- no slot, nothing to forfeit.
    {
        auto& sp = r->spectators;
        auto it = std::find(sp.begin(), sp.end(), c.id);
        if (it != sp.end()) {
            sp.erase(it);
            c.state = Client::Lobby; c.roomId = 0; c.slot = -1; c.loaded = false;
            std::fprintf(stderr, "client %u stopped spectating game %u (%s)\n", c.id, r->id, reason);
            return;
        }
    }
    if (c.slot >= 0 && c.slot < kMaxSlots) {
        r->slotClient[c.slot] = -1;
        if (!r->running) { r->slots[c.slot].type = 0; r->slots[c.slot].name.clear(); }
        else {
            // Voluntary leave mid-game = immediate forfeit (sequenced event so the
            // sim disposes the units in lockstep).
            r->slots[c.slot].type = 3;
            r->pendingEvents.push_back({ta::net::Event::Kind::Leave, uint8_t(c.slot)});
        }
    }
    uint32_t rid = r->id;
    bool wasHost = (r->hostId == c.id);
    c.state = Client::Lobby; c.roomId = 0; c.slot = -1; c.loaded = false;
    // If the host left in the lobby, pass host to the next human (or dissolve).
    if (!r->running && wasHost) {
        int next = -1;
        for (int i = 0; i < kMaxSlots; ++i)
            if (r->slotClient[i] >= 0) { next = r->slotClient[i]; break; }
        if (next >= 0) r->hostId = uint32_t(next);
        else { rooms_.erase(rid); std::fprintf(stderr, "game %u dissolved\n", rid); return; }
    }
    broadcastLobby(*r);   // reachable only when the room still exists (not dissolved)
    std::fprintf(stderr, "client %u left game %u (%s)\n", c.id, rid, reason);
}

void Server::tryStart(Client& c) {
    Room* r = roomOf(c);
    if (!r || r->hostId != c.id || r->running) return;
    // Validate: >=2 used slots, every human ready, unique colors among used slots.
    // A campaign mission is exempt from the 2-player minimum: its opponents are the
    // mission script's units, not lobby slots, so one seated human is enough.
    if (r->mission.empty() && r->usedSlots() < 2) return;
    bool usedColor[10] = {};
    for (int i = 0; i < kMaxSlots; ++i) {
        const SlotInfo& s = r->slots[i];
        if (s.type != 1 && s.type != 2) continue;
        if (s.type == 1 && !s.ready) return;             // a human isn't ready
        if (s.color < 10) { if (usedColor[s.color]) return; usedColor[s.color] = true; }
    }
    // The referee is MANDATORY, so resolve what it needs before committing to run.
    // Failing here used to leave r->ref null and the game carried on as a relay --
    // silently giving up the canonical hash and, for a mission, the strategic AI.
    // A game that cannot be refereed does not start; the host is told why.
    DataSet& dataSet = dataFor(r->opts.overridePolicy);
    const bool wantMission = !r->mission.empty();
    std::string mapResolved = wantMission ? std::string()
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
    r->running = true;
    r->tick = 0;
    r->nextTickMs = nowMs();
    for (int i = 0; i < kMaxSlots; ++i) r->startSlots[i] = r->slots[i];   // for the replay
    // Build the referee sim (and AI controllers) if we have game data. The world
    // is built by the SAME setupMatch the clients use, so its hash is canonical.
    // Everything the referee reads comes from the data set for THIS game's override
    // tier (retail for none/cosmetic, full for full), so the referee's sim matches
    // the clients that adopted the same tier.
    DataSet* ds = &dataSet;
    const bool isMission = wantMission;
    // A campaign mission builds its own world (placements + the in-sim god script) and
    // needs no map id; a skirmish resolves its map by name (resolved above, where a
    // failure refuses the start).
    const std::string& mapPath = mapResolved;
    {
        r->reg = &registryFor(r->opts.crusades != 0, r->opts.overridePolicy);
        r->ref = std::make_unique<ta::sim::World>();
        r->ref->setVisPlayer(-1);   // headless referee: no fog pass
        if (isMission) {
            // The client builds the SAME world (setupMission is deterministic), so the
            // referee and every peer stay in lockstep.
            int human = 0;
            ta::sim::MissionSetup ms;
            if (!ta::sim::setupMission(*r->ref, *r->reg, ds->vfs, r->mission, human, &ms)) {
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
                // scenery. Mana income is deliberately NOT multiplied here -- a
                // mission is balanced around its placements, not a skirmish curve.
                const ta::ai::Profile& prof =
                    ms.aiProfile.empty() ? aiProfile_
                                         : missionProfile(ds->vfs, ms.aiProfile);
                for (int slot : ms.aiSlots) {
                    std::vector<std::pair<float, float>> enemyStarts;
                    for (size_t j = 0; j < ms.slotPos.size(); ++j) {
                        if (int(j) == slot || r->ref->allied(slot, int(j))) continue;
                        if (ms.slotPos[j].first == 0.0f && ms.slotPos[j].second == 0.0f) continue;
                        enemyStarts.push_back(ms.slotPos[j]);
                    }
                    r->ai.emplace_back(slot, *r->reg, prof, r->seed + uint32_t(slot),
                                       ta::ai::Difficulty::Normal, std::move(enemyStarts));
                }
                if (!ms.aiSlots.empty())
                    std::fprintf(stderr, "taserver: mission '%s' -- %zu strategic AI player(s)%s\n",
                                 r->mission.c_str(), ms.aiSlots.size(),
                                 ms.aiProfile.empty() ? "" : (" profile=" + ms.aiProfile).c_str());
            }
        } else {
            int maxSlot = 0;
            for (int i = 0; i < kMaxSlots; ++i)
                if (r->slots[i].type == 1 || r->slots[i].type == 2) maxSlot = i;
            ta::sim::MatchConfig cfg;
            cfg.vfs = &ds->vfs;
            cfg.mapPath = mapPath;
            cfg.gods = r->opts.gods != 0;
            cfg.unitCap = r->opts.unitCap;
            cfg.monarchExpendable = r->opts.monarchExpendable != 0;
            cfg.stressTest = r->opts.stressTest != 0;
            cfg.benchmark = r->opts.benchmark;
            cfg.randomStarts = r->opts.randomStarts != 0;
            cfg.startSeed = r->seed;   // the seed sent in GameStarting
            cfg.slots.resize(size_t(maxSlot + 1));
            for (int i = 0; i <= maxSlot; ++i) {
                const auto& s = r->slots[i];
                // Absurd AI slots earn double income (incomeMultFor); derived from the
                // shared aiLevel so the client mirror sets the same factor (lockstep).
                float mm = s.type == 2
                    ? ta::ai::incomeMultFor(ta::ai::difficultyFromLevel(s.aiLevel)) : 1.0f;
                cfg.slots[size_t(i)] = {s.type == 1 || s.type == 2, s.faction % 5, s.team, mm};
            }
            auto spots = ta::sim::setupMatch(*r->ref, *r->reg, cfg);
            // setupMatch returns start positions in USED-slot order; remap to slot index.
            std::vector<std::pair<float, float>> slotPos(size_t(maxSlot + 1), {0.f, 0.f});
            for (int i = 0, k = 0; i <= maxSlot; ++i)
                if (r->slots[i].type == 1 || r->slots[i].type == 2) {
                    if (k < int(spots.size())) slotPos[size_t(i)] = spots[size_t(k)];
                    ++k;
                }
            r->ai.reserve(size_t(maxSlot + 1));
            for (int i = 0; i <= maxSlot; ++i)
                if (r->slots[i].type == 2) {
                    // The enemy start positions this AI marches on before it has spotted
                    // any units (fog): every used, non-allied slot's start.
                    std::vector<std::pair<float, float>> enemyStarts;
                    for (int j = 0; j <= maxSlot; ++j) {
                        if (j == i || (r->slots[j].type != 1 && r->slots[j].type != 2)) continue;
                        if (r->ref->allied(i, j)) continue;
                        enemyStarts.push_back(slotPos[size_t(j)]);
                    }
                    auto diff = ta::ai::difficultyFromLevel(r->slots[i].aiLevel);
                    r->ai.emplace_back(i, *r->reg, aiProfile_, r->seed,
                                       diff, std::move(enemyStarts));
                }
        }
    }
    // GameStarting: final slot table + options + seed + a per-slot resume token
    // (used to rejoin the held slot after a disconnect).
    for (int i = 0; i < kMaxSlots; ++i) {
        r->pauseBudgetMs[i] = kPauseBudgetMs;
        if (r->slotClient[i] < 0) continue;
        r->slotToken[i] = randToken();
        auto it = clients_.find(uint32_t(r->slotClient[i]));
        if (it == clients_.end()) continue;
        Writer w; writeSlots(w, *r);
        w.u8(uint8_t(i));                 // your slot
        w.u32(r->seed);       // per-game RNG seed base
        w.u64(r->slotToken[i]);           // resume token
        w.u32(0);                         // fresh game: no history to replay
        it->second->conn.send(Msg::GameStarting, w);
        it->second->loaded = false;
    }
    // Spectators (incl. a host who created the game to watch) get a slot-less
    // GameStarting; they don't gate the first tick, so mark them loaded.
    for (uint32_t sid : r->spectators) {
        auto it = clients_.find(sid);
        if (it == clients_.end()) continue;
        Writer w; writeSlots(w, *r);
        w.u8(0xFF); w.u32(r->seed); w.u64(0);
        w.u32(uint32_t(r->log.size()));   // replay boundary
        it->second->conn.send(Msg::GameStarting, w);
        it->second->loaded = true;
    }
    std::fprintf(stderr, "game %u starting with %d players\n", r->id, r->usedSlots());
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
            if (!rd.ok || slot < 0 || slot >= kMaxSlots || r->running) return;
            bool isHost = (r->hostId == c.id);
            // A player edits only their own slot; the host may edit any.
            if (!isHost && slot != c.slot) return;
            SlotInfo& s = r->slots[slot];
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
            s.faction = faction % 5;
            s.color = color % 10;
            s.team = uint8_t(team % kMaxSlots);
            s.ready = ready ? 1 : 0;
            s.aiLevel = aiLevel > 4 ? 2 : aiLevel;   // 0=passive 1=easy 2=normal 3=hard 4=absurd
            broadcastLobby(*r);
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
                if (it != clients_.end()) { leaveRoom(*it->second, "kicked"); it->second->conn.send(Msg::Bye, Writer{}); }
            }
            break;
        }
        case Msg::SetPause: {
            // A player asking to pause. Only the host may (in single-player the host
            // IS the only human), and only in a running game. Unlike the drop-driven
            // pause this one has NO budget: nobody has disconnected, so nothing should
            // forfeit -- pausePlayer stays at the sentinel so the budget sweep and the
            // rejoin-resume both skip it.
            if (!r || !r->running || r->hostId != c.id) return;
            Reader rd(f.payload.data(), f.payload.size());
            bool want = rd.u8() != 0;
            if (want && !r->paused) {
                r->paused = true; r->pausePlayer = kPauseByRequest; r->pauseStartMs = nowMs();
                Writer pw; pw.u8(1); pw.u8(0);
                broadcastRoom(*r, Msg::Pause, pw);
                std::fprintf(stderr, "game %u: paused by request\n", r->id);
            } else if (!want && r->paused && r->pausePlayer == kPauseByRequest) {
                r->paused = false; r->pausePlayer = -1; r->nextTickMs = nowMs();
                Writer rw; rw.u8(1); rw.u8(0);
                broadcastRoom(*r, Msg::Resume, rw);
                std::fprintf(stderr, "game %u: resumed by request\n", r->id);
            }
            break;
        }
        case Msg::SetGameOptions: {
            if (r->hostId != c.id) return;   // host only
            Reader rd(f.payload.data(), f.payload.size());
            GameOptions o; o.crusades = rd.u8(); o.gods = rd.u8(); o.forfeitSelfDestruct = rd.u8();
            o.overridePolicy = rd.u8(); o.speed = rd.u8(); o.speedUnlock = rd.u8();
            o.unitCap = clampUnitCap(uint16_t(rd.u32())); o.monarchExpendable = rd.u8() ? 1 : 0;
            o.stressTest = rd.u8() ? 1 : 0;
            o.fogExplored = std::min<uint8_t>(rd.u8(), 2);   // 0/1/2, see CreateGame
            o.benchmark = rd.u8();
            o.randomStarts = rd.u8() ? 1 : 0;
            if (!rd.ok) return;
            if (o.speed < 1) o.speed = 1;
            if (o.speed > 40) o.speed = 40;   // clamp 0.1x .. 4.0x
            if (!r->running) {
                // Lobby: adopt the whole option set and rebroadcast the slot table.
                r->opts = o;
                broadcastLobby(*r);
            } else if (r->opts.speedUnlock && r->opts.speed != o.speed) {
                // In-game: only speed can change (re-cadences the sim), and only if the
                // game was created with speed-unlock. Tell every peer to re-pace.
                r->opts.speed = o.speed;
                Writer w; w.u8(o.speed);
                broadcastRoom(*r, Msg::SpeedUpdate, w);
            }
            break;
        }
        case Msg::StartGame: tryStart(c); break;
        case Msg::Loaded: {
            // The client reports its gameplay-data fingerprint at the room's override
            // tier. With referee data we hold it to the tier's canonical hash, so a
            // Full-tier player missing (or differing on) a gameplay override is caught
            // here, before the first tick, instead of desyncing mid-game.
            Reader rd(f.payload.data(), f.payload.size());
            uint64_t clientHash = rd.u64();
            if (rd.ok && clientHash != 0) {
                uint64_t want = dataFor(r->opts.overridePolicy).hash;
                if (clientHash != want) {
                    char msg[176];
                    std::snprintf(msg, sizeof msg,
                        "gameplay-override mismatch (tier %d: server %016llx, you %016llx) -- "
                        "you don't have the same gameplay overrides as the host",
                        int(r->opts.overridePolicy), (unsigned long long)want,
                        (unsigned long long)clientHash);
                    sendReject(c, msg);
                    c.conn.fail("gamedatahash");
                    std::fprintf(stderr, "game %u: client %u rejected at load: %016llx != %016llx (tier %d)\n",
                        r->id, c.id, (unsigned long long)clientHash, (unsigned long long)want,
                        int(r->opts.overridePolicy));
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
        case Msg::LeaveGame: leaveRoom(c, "left"); break;
        case Msg::Chat: {
            Reader rd(f.payload.data(), f.payload.size());
            std::string text = rd.str();
            if (!rd.ok || text.size() > 512) return;
            Writer w; w.str(c.name); w.str(text);
            broadcastRoom(*r, Msg::Chat, w);
            break;
        }
        case Msg::PlayerCommands: {
            if (!r->running) return;
            Reader rd(f.payload.data(), f.payload.size());
            uint32_t n = rd.u32();
            for (uint32_t i = 0; i < n && rd.ok; ++i) {
                Command cmd = rd.cmd();
                if (!rd.ok) break;
                cmd.player = uint8_t(c.slot);   // server stamps ownership
                if (c.cmdQueue.size() >= kCmdQueueMax) {
                    if (++c.cmdDropped % 64 == 1)
                        std::fprintf(stderr, "client %u: command queue full (%zu), "
                                             "dropped %llu so far\n",
                                     c.id, c.cmdQueue.size(),
                                     (unsigned long long)c.cmdDropped);
                    continue;
                }
                c.cmdQueue.push_back(cmd);
            }
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
    // hold this tick yet, in which case wait rather than judging anybody.
    //
    // The client-majority branch that used to sit here belonged to the relay-only
    // mode and is gone with it. It was never a good judge anyway: with two clients
    // it had no majority to find, and with a modded pair against one honest client
    // it would convict the honest one.
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
    // the server may not run more than kMaxLeadTicks past the least-advanced acked
    // tick. Seated humans are the primary constraint (AIs are server-run and always
    // current). When a game has NO seated humans -- an all-AI game watched by
    // spectators -- pace to the slowest spectator instead, otherwise the server
    // outruns a spectator that can't sustain the speed and floods it until the
    // connection breaks. (A spectator NEVER holds up a real match: if any human is
    // seated, spectators lag on their own.)
    uint32_t slowest = r.tick;
    bool anyHuman = false;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (r.slots[i].type != 1 || r.slotClient[i] < 0) continue;
        auto it = clients_.find(uint32_t(r.slotClient[i]));
        if (it == clients_.end() || !it->second->loaded) continue;
        anyHuman = true;
        slowest = std::min(slowest, it->second->ackTick);
    }
    if (anyHuman) return r.tick <= slowest + kMaxLeadTicks;
    bool anySpec = false;
    for (uint32_t sid : r.spectators) {
        auto it = clients_.find(sid);
        if (it == clients_.end()) continue;
        anySpec = true;
        slowest = std::min(slowest, it->second->ackTick);
    }
    if (anySpec) return r.tick <= slowest + kMaxLeadTicks;
    return true;   // no live consumers at all
}

void Server::closeTick(Room& r) {
    // All humans must have loaded before the first tick.
    if (r.tick == 0) {
        for (int i = 0; i < kMaxSlots; ++i)
            if (r.slots[i].type == 1 && r.slotClient[i] >= 0) {
                auto it = clients_.find(uint32_t(r.slotClient[i]));
                if (it == clients_.end() || !it->second->loaded) { r.nextTickMs = nowMs() + 100; return; }
            }
    }
    // The ONE place a client's commands enter a tick: strict FIFO, one budget per
    // client per tick, and the same input-delay rule for all of them.
    //
    // Server-side input delay (TA_SRV_DELAY=K, default 0) buckets commands K ticks
    // into the future so one never "just misses" a tick boundary; costs K ticks of
    // latency.
    static const int srvDelay = [] {
        const char* e = std::getenv("TA_SRV_DELAY"); return e ? std::max(0, std::atoi(e)) : 0;
    }();
    for (int i = 0; i < kMaxSlots; ++i) {
        if (r.slotClient[i] < 0) continue;
        auto ci = clients_.find(uint32_t(r.slotClient[i]));
        if (ci == clients_.end()) continue;
        Client& c = *ci->second;
        for (int taken = 0; taken < kCmdCapPerTick && !c.cmdQueue.empty(); ++taken) {
            if (srvDelay > 0) r.pendingAt[r.tick + uint32_t(srvDelay)].push_back(c.cmdQueue.front());
            else r.pending.push_back(c.cmdQueue.front());
            c.cmdQueue.pop_front();
        }
    }
    // Server-hosted AI: each controller observes the referee world (state after
    // tick-1) and appends its orders to this tick's bundle, exactly like a client.
    // A running room always has a referee -- tryStart refuses to start a game it
    // cannot referee -- so this is unconditional.
    {
        static const bool kAiPhase = std::getenv("TA_AIPHASE") != nullptr;
        auto _a0 = std::chrono::steady_clock::now();
        for (auto& ctl : r.ai)
            ctl.tick(*r.ref, r.tick, [&r](const Command& c) { r.pending.push_back(c); });
        if (kAiPhase) {
            double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - _a0).count();
            static double thr = std::getenv("TA_AIPHASE_MS")
                                    ? atof(std::getenv("TA_AIPHASE_MS")) : 8.0;
            if (ms > thr)
                std::fprintf(stderr, "AIPHASE tick=%u ai=%.1fms controllers=%zu\n",
                             r.tick, ms, r.ai.size());
        }
    }

    // Server input delay: client commands scheduled for THIS tick (received
    // srvDelay ticks ago) join the bundle now.
    if (auto it = r.pendingAt.find(r.tick); it != r.pendingAt.end()) {
        for (auto& c : it->second) r.pending.push_back(c);
        r.pendingAt.erase(it);
    }
    Writer w;
    w.u32(r.tick);
    // Deterministic order: sort by player, stable within a player (arrival order).
    std::stable_sort(r.pending.begin(), r.pending.end(),
                     [](const Command& a, const Command& b) { return a.player < b.player; });
    w.u32(uint32_t(r.pending.size()));
    for (const auto& cmd : r.pending) w.cmd(cmd);
    w.u32(uint32_t(r.pendingEvents.size()));
    for (const auto& e : r.pendingEvents) { w.u8(uint8_t(e.kind)); w.u8(e.player); }
    broadcastRoom(r, Msg::TickBundle, w);
    r.log.push_back(std::move(w.b));   // keep the full bundle log for reconnect/replay
                                       // (moved: broadcastRoom already copied it out)

    // Advance the referee sim by this same bundle, then record its canonical hash --
    // but only at the ticks clients actually REPORT (kHashPeriod): hashing every
    // tick burned ~8ms/s per 2000-unit room on hashes that were never read.
    {
        for (const auto& cmd : r.pending) ta::sim::applyCommand(*r.ref, *r.reg, cmd);
        for (const auto& e : r.pendingEvents) ta::sim::applyEvent(*r.ref, e);
        r.ref->tick(1.0f / kServerHz);
        if (r.tick % uint32_t(kHashPeriod) == 0) {
            const uint64_t h = r.ref->stateHash();
            r.refHash[r.tick] = h;
            // Keep a coarser, UNPRUNED copy for the replay file (see Room::replayChecks).
            // One every 10 hash periods -- fine enough to bracket a divergence, cheap
            // enough to carry for a whole game.
            if (!replayDir_.empty() && (r.tick / uint32_t(kHashPeriod)) % 10 == 0)
                r.replayChecks.emplace_back(r.tick, h);
        }
        // bound the ring
        while (r.refHash.size() > 300) r.refHash.erase(r.refHash.begin());
        // Campaign win/lose: the referee's mission runner is authoritative -- announce
        // the result once (the clients reach the same outcome in their own sims, but
        // this drives the end-of-mission UI and covers all-spectator missions).
        if (!r.missionOutcomeSent) {
            if (int oc = r.ref->missionOutcome()) {
                r.missionOutcomeSent = int8_t(oc);
                Writer mw; mw.u8(uint8_t(int8_t(oc)));
                broadcastRoom(r, Msg::MissionOutcome, mw);
                std::fprintf(stderr, "game %u mission %s: %s\n", r.id, r.mission.c_str(),
                             oc > 0 ? "VICTORY" : "DEFEAT");
            }
        }
    }
    r.pending.clear();
    r.pendingEvents.clear();
    r.tick++;
    // Cadence scales with game speed (dt per tick stays 1/kServerHz): 10 = 1.0x.
    r.nextTickMs += uint64_t(10000 / (kServerHz * std::max<int>(1, int(r.opts.speed))));
}

void Server::dropClient(uint32_t id, const char* reason) {
    auto it = clients_.find(id);
    if (it == clients_.end()) return;
    Client& c = *it->second;
    Room* r = c.roomId ? roomOf(c) : nullptr;
    if (r && r->running && c.slot >= 0 && c.slot < kMaxSlots && r->slots[c.slot].type == 1) {
        // A disconnect from a running game HOLDS the slot: the player may rejoin
        // with their resume token within the grace window. Auto-pause (budget
        // permitting) so nobody is fighting a frozen empire meanwhile.
        int s = c.slot;
        dropPendingCommands(c, *r);   // the slot is HELD; its in-flight orders are not
        r->slotDropped[s] = true;
        r->slotClient[s] = -1;
        r->graceDeadline[s] = nowMs() + kGraceMs;
        if (r->pauseBudgetMs[s] > 0 && !r->paused) {
            r->paused = true; r->pausePlayer = s; r->pauseStartMs = nowMs();
            Writer pw; pw.u8(1 /*drop-grace*/); pw.u8(uint8_t(s));
            broadcastRoom(*r, Msg::Pause, pw);
        }
        Writer w; w.u8(uint8_t(s)); w.u8(1 /*dropped*/); w.u32(0);
        broadcastRoom(*r, Msg::PlayerStatus, w);
        std::fprintf(stderr, "game %u: client %u dropped -- slot %d held (grace %llus)%s\n",
                     r->id, id, s, (unsigned long long)(kGraceMs / 1000),
                     r->paused ? ", paused" : "");
    } else if (r) {
        leaveRoom(c, reason);
    }
    std::fprintf(stderr, "client %u dropped (%s)\n", id, reason);
    clients_.erase(it);
}

void Server::onFrame(Client& c, const Frame& f) {
    c.lastRecvMs = nowMs();
    if (f.kind == Msg::Ping) { c.conn.send(Msg::Pong); return; }
    if (f.kind == Msg::Pong) return;
    if (f.kind == Msg::Bye) { c.conn.fail("bye"); return; }
    switch (c.state) {
        case Client::Handshake: handshake(c, f); break;
        case Client::Auth: authMsg(c, f); break;
        case Client::Lobby: lobbyMsg(c, f); break;
        case Client::InGame: gameMsg(c, f); break;
    }
}

int Server::run() {
    std::string err;
    listenFd_ = listenOn(port_, err, loopbackOnly_);
    if (listenFd_ < 0) { std::fprintf(stderr, "taserver: %s on port %u\n", err.c_str(), port_); return 1; }
    std::fprintf(stderr, "taserver %s listening on %s port %u (protocol v%u)\n",
                 ta::kVersion, loopbackOnly_ ? "loopback" : "all interfaces", port_, kNetVersion);
    // Build id on its own line: the harness greps it to refuse a server built from
    // different source than the client, which otherwise looks exactly like a desync.
    std::fprintf(stderr, "taserver: build %s\n", ta::kBuildId);
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
    for (;;) {
        // Build the pollfd set: listen + every client (POLLOUT when it has pending writes).
        pfds.clear();
        ids.clear();
        // Field-wise (not brace) init: a socket fd is `int` here but `SOCKET`
        // (unsigned) in a Windows pollfd, which brace-init would reject as narrowing.
        pollfd lp{}; lp.fd = listenFd_; lp.events = POLLIN; pfds.push_back(lp);
        ids.push_back(0);
        for (auto& [id, c] : clients_) {
            short ev = POLLIN;
            if (c->conn.wantWrite()) ev |= POLLOUT;
            pollfd cp{}; cp.fd = c->conn.fd(); cp.events = ev; pfds.push_back(cp);
            ids.push_back(id);
        }
        // Timeout = time until the soonest running room's next tick (or 1s idle).
        uint64_t now = nowMs();
        uint64_t soonest = now + 1000;
        // Only rooms ELIGIBLE to tick may pull the poll deadline in. A paused room
        // is skipped by the tick loop below but its nextTickMs still slides into
        // the past, so counting it here pinned the timeout at 0 and span the
        // server at full CPU for as long as the pause lasted -- and a manual
        // pause has no expiry, so that is indefinite.
        for (auto& [rid, r] : rooms_)
            if (r.running && !r.paused && r.nextTickMs < soonest) soonest = r.nextTickMs;
        // A client still streaming catch-up needs servicing regardless of the
        // tick clock -- otherwise resuming into a PAUSED game would feed it one
        // chunk per idle second, since a paused room no longer pulls the
        // deadline in.
        for (const auto& [id, c] : clients_)
            if (c->replaying && now + 10 < soonest) soonest = now + 10;
        int timeout = int(soonest > now ? soonest - now : 0);

        int n = TA_POLL(pfds.data(), (unsigned)pfds.size(), timeout);
        if (n < 0) { if (sockInterrupted(sockErr())) continue; break; }

        throttle_.expire(now);   // forget hosts that have long since behaved

        // Accept new connections.
        if (pfds[0].revents & POLLIN) {
            for (;;) {
                int fd = int(accept(listenFd_, nullptr, nullptr));
                if (fd < 0) break;
                setupSocket(fd);
                auto c = std::make_unique<Client>();
                c->id = nextClientId_++;
                c->conn = Conn(fd);
                c->peer = peerAddress(fd);
                c->lastRecvMs = nowMs();
                clients_[c->id] = std::move(c);
            }
        }
        // Service clients.
        std::vector<uint32_t> dead;
        for (size_t i = 1; i < pfds.size(); ++i) {
            uint32_t id = ids[i];
            auto it = clients_.find(id);
            if (it == clients_.end()) continue;
            Client& c = *it->second;
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (!c.conn.recv()) { dead.push_back(id); continue; }
                Frame fr;
                while (c.conn.poll(fr)) { onFrame(c, fr); if (!c.conn.ok()) break; }
                // A clean close is only final once its trailing frames are drained
                // above -- the last LeaveGame usually arrives in the same segment.
                if (c.conn.peerClosed()) c.conn.fail("peer closed");
            }
            if (!c.conn.ok()) { dead.push_back(id); continue; }
            if (pfds[i].revents & POLLOUT) c.conn.flushWrite();
        }
        // Grace / pause-budget expiry: a held slot that isn't reclaimed in time,
        // or a pause that outlasts its budget, forfeits the player. The forfeit is
        // a SEQUENCED event so every sim (and the replay) disposes their units on
        // the same tick.
        now = nowMs();
        for (auto& [rid, r] : rooms_) {
            if (!r.running) continue;
            bool budgetOut = r.paused && r.pausePlayer >= 0 &&
                             now - r.pauseStartMs >= r.pauseBudgetMs[r.pausePlayer];
            for (int i = 0; i < kMaxSlots; ++i) {
                if (!r.slotDropped[i]) continue;
                bool forfeit = now >= r.graceDeadline[i] ||
                               (budgetOut && i == r.pausePlayer);
                if (!forfeit) continue;
                r.slotDropped[i] = false;
                r.slots[i].type = 3;   // slot closed; the player is out
                r.pendingEvents.push_back({ta::net::Event::Kind::Forfeit, uint8_t(i)});
                std::fprintf(stderr, "game %u: player %d FORFEIT (grace/budget expired)\n", rid, i);
                if (r.paused && r.pausePlayer == i) {
                    r.paused = false; r.pausePlayer = -1; r.nextTickMs = now;
                    Writer rw; rw.u8(0); rw.u8(uint8_t(i));
                    broadcastRoom(r, Msg::Resume, rw);
                }
            }
        }
        // Tear down abandoned running games (everyone left/forfeited): write the
        // replay, then erase.
        std::vector<std::pair<uint32_t, const char*>> doneRooms;
        for (auto& [rid, r] : rooms_) {
            if (!r.running) continue;
            if (!roomOccupied(r)) doneRooms.push_back({rid, "last player left"});
            else if (!roomActive(r)) doneRooms.push_back({rid, "all players gone"});
        }
        for (auto& [rid, why] : doneRooms) {
            Room& r = rooms_.at(rid);
            writeReplay(r);
            // Detach any lingering spectators before the room vanishes: reset their
            // server-side state to Lobby (their client already shows the game's end
            // from the sim, and can browse/leave on its own).
            for (uint32_t sid : r.spectators) {
                auto it = clients_.find(sid);
                if (it == clients_.end()) continue;
                it->second->state = Client::Lobby;
                it->second->roomId = 0; it->second->slot = -1;
            }
            std::fprintf(stderr, "game %u ended (%s)\n", rid, why);
            rooms_.erase(rid);
        }
        // Close ticks for running, unpaused rooms whose deadline passed -- but never
        // get more than kMaxLeadTicks ahead of the slowest seated player (flow control):
        // if one is behind, hold and rebase the clock so we resume without a burst.
        //
        // Games are INDEPENDENT: each closeTick touches only its own Room + referee
        // World (per-room nav/flow/AI) and broadcasts to its own clients (a client is
        // in exactly one room, so the client sets are disjoint). clients_/rooms_ are
        // only READ here (the main thread mutates them before/after, never during),
        // so several games can tick in parallel. The referee reads the shared registry
        // read-only. With >=2 games due we hand one per worker; flow prefetch then
        // stays on-thread (the parallelism is already at the game level). A lone game
        // ticks inline and keeps its intra-tick flow pool. Byte-identical either way.
        // Rooms tick before sockets are serviced again, so in principle one heavy room
        // delays everyone's networking. Measured with a heavy (stress, ~3800 units) room
        // and a light one on the same server: worst socket-service gap 20 ms with two
        // rooms, against 33 ms with a single room -- BETTER with two, because the tick
        // deadlines interleave and the poll wakes more often. The per-room tick is well
        // inside the 33 ms period and rooms tick in parallel, so the batch join is not a
        // bottleneck at this scale. Revisit if a single room's tick ever approaches the
        // period; then the join really would gate everyone.
        now = nowMs();
        std::vector<Room*> due;
        for (auto& [rid, r] : rooms_)
            if (r.running && !r.paused && r.nextTickMs <= now) due.push_back(&r);
        auto tickRoom = [&](Room& r) {
            while (r.nextTickMs <= now) {
                // Pace to the slowest. Back off a few ms rather than rebasing to
                // `now`: an immediate deadline makes the poll above return at
                // once, so the server would spin until the laggard acked.
                if (!canAdvance(r)) { r.nextTickMs = now + kFlowRetryMs; break; }
                closeTick(r);
            }
        };
        bool parallel = due.size() >= 2;
        for (Room* rp : due) if (rp->ref) rp->ref->setSerialThreads(parallel);
        if (parallel)
            tickPool_.run(due.size(), [&](size_t i) { tickRoom(*due[i]); });
        else
            for (Room* rp : due) tickRoom(*rp);
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
            const auto& log = rit->second.log;
            while (c->replayPos < log.size() &&
                   c->conn.txPending() < kReplayChunkBytes)
                c->conn.send(Msg::TickBundle, log[c->replayPos++]);
            if (c->replayPos >= log.size()) c->replaying = false;
        }
        // Flush all pending writes (bundles just queued) + keepalive + timeouts.
        now = nowMs();
        for (auto& [id, c] : clients_) {
            if (!c->conn.flushWrite()) { dead.push_back(id); continue; }
            if (c->state != Client::Handshake && now - c->lastRecvMs > kPingIdleMs &&
                now - c->lastPingMs > kPingIdleMs) {
                c->conn.send(Msg::Ping); c->lastPingMs = now; c->conn.flushWrite();
            }
            // Spectators (in a game, no seat) get a far longer grace than seated players.
            bool spectator = c->state == Client::InGame && c->slot < 0;
            if (now - c->lastRecvMs > (spectator ? kSpectatorTimeoutMs : kTimeoutMs))
                dead.push_back(id);
        }
        for (uint32_t id : dead) dropClient(id, "disconnected");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Test overrides for the reconnect timers (seconds).
    if (const char* g = std::getenv("TA_GRACE_MS")) kGraceMs = uint64_t(std::atoll(g));
    if (const char* b = std::getenv("TA_PAUSE_BUDGET_MS")) kPauseBudgetMs = uint64_t(std::atoll(b));
    uint16_t port = 7677;
    std::string dataRoot, replayDir;
    std::string accountsPath = "taserver-accounts.conf";
    uint32_t fixedSeed = 0;
    bool noAuth = false, loopbackOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = uint16_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) dataRoot = argv[++i];
        else if (!std::strcmp(argv[i], "--replaydir") && i + 1 < argc) replayDir = argv[++i];
        else if (!std::strcmp(argv[i], "--accounts") && i + 1 < argc) accountsPath = argv[++i];
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            fixedSeed = uint32_t(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--no-auth")) noAuth = true;
        else if (!std::strcmp(argv[i], "--local")) loopbackOnly = true;
        else if (!std::strcmp(argv[i], "--version") || !std::strcmp(argv[i], "-v")) {
            std::printf("taserver (TAK engine) %s (build %s)\n", ta::kVersion, ta::kBuildId);
            return 0;
        }
        else if (!std::strcmp(argv[i], "--help")) {
            std::printf("usage: taserver --data <retail-install-dir> [--port N]\n"
                        "                 [--replaydir <dir>] [--accounts <file>]\n"
                        "                 [--no-auth] [--local]\n"
                        "  --data is REQUIRED: it is what the referee sim and the\n"
                        "  server-hosted AI players read. There is no relay-only mode.\n"
                        "  --replaydir writes a .tarep replay file per finished game.\n"
                        "  --seed N pins every game's RNG seed, so a headless run is\n"
                        "  repeatable. Games are otherwise seeded randomly (which is what\n"
                        "  makes Random Start Locations differ game to game).\n"
                        "  --accounts is the account file (default taserver-accounts.conf).\n"
                        "  Players sign in with a name and password; an unused name is\n"
                        "  registered on the spot. No password is stored or transmitted --\n"
                        "  see src/net/auth.h.\n"
                        "  --no-auth serves anyone who connects, with no account at all. Only\n"
                        "  for a private or LAN server; pair it with --local.\n"
                        "  --local binds loopback only, so nothing off this machine connects.\n");
            return 0;
        }
    }
    // --data is mandatory. The server used to run without it as a pure relay, with
    // the clients cross-checking hashes among themselves; that mode is gone. It gave
    // up the canonical referee hash (so a lone client could not be told apart from a
    // desynced one), it could not host AI players at all, and it made a whole second
    // set of paths that no shipped configuration exercised -- single-player launches
    // its private server with --data, and so does every harness.
    if (dataRoot.empty()) {
        std::fprintf(stderr,
            "taserver: --data <retail-install-dir> is required.\n"
            "  It is what the referee sim and the AI players read. Point it at a\n"
            "  TA:Kingdoms install (the folder holding the root *.hpi and Maps/).\n"
            "  Run with --help for the full usage.\n");
        return 1;
    }
    Server s(port, dataRoot);
    if (!replayDir.empty()) s.setReplayDir(replayDir);
    if (fixedSeed) s.setFixedSeed(fixedSeed);
    if (loopbackOnly) s.setLoopbackOnly();
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
