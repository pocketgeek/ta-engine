// takserver: the central multiplayer server (docs/multiplayer-design.md, M3).
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
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "version.h"

using namespace tak::net;

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
using tak::net::kCmdCapPerTick;
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
// How many over-budget commands one client may have waiting. A few ticks' worth:
// enough that an honest burst (a big selection, or two render steps landing in one
// server tick) always survives, small enough that a flooder cannot make the server
// hold an unbounded queue on its behalf.
constexpr size_t kCmdOverflowMax = 8 * size_t(kCmdCapPerTick);
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
    // ~0u, not 0: the budget refreshes when this differs from the room tick, and a
    // room's FIRST tick IS 0 -- initialising both to zero meant the refresh never
    // fired there and every command issued on tick 0 was dropped.
    uint32_t cmdTick = ~0u;      // tick cmdBudget refers to
    int cmdBudget = 0;           // commands still accepted from this client this tick
    // Commands past this tick's budget wait their turn instead of being thrown
    // away. The cap is flood protection, and a legitimate client can exceed it
    // through no fault of its own: it sends up to kCmdCapPerTick per RENDER step,
    // so at 120fps against a 30Hz tick -- or whenever TCP hands the server two
    // messages in one pass -- several honest batches land inside one server tick.
    // Dropping them lost orders the client had already cleared from its outbox.
    // Bounded, so a flooder still cannot buy unbounded queueing.
    std::deque<Command> cmdOverflow;
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
    std::unique_ptr<tak::sim::World> ref;
    const tak::sim::TypeRegistry* reg = nullptr;   // the balance this game uses
    std::vector<tak::ai::Controller> ai;        // one per AI slot
    std::map<uint32_t, uint64_t> refHash;       // tick -> referee hash (bounded ring)
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
    void setNoAuth() { requireAuth_ = false; }
    void setLoopbackOnly() { loopbackOnly_ = true; }
    // Load (or start) the account file. Returns false with `err` set if it exists
    // but cannot be read -- starting anyway would mean running an open server
    // while looking like a closed one.
    bool loadAccounts(const std::string& path, std::string& err) {
        return accounts_.load(path, &err);
    }
    size_t accountCount() const { return accounts_.size(); }
    Server(uint16_t port, const std::string& dataRoot) : port_(port), dataRoot_(dataRoot) {
        if (!dataRoot_.empty()) {
            // The referee reads the retail install directly, per the ROOM's override
            // tier. Build the pure-retail set eagerly (the connection-level Hello
            // check + the common none/cosmetic game); the Full set is built lazily
            // the first time a Full game starts.
            buildDataSet(retail_, tak::hpi::OverridePolicy::None);
            aiProfile_ = tak::ai::loadProfile(retail_.vfs);
            aiNames_ = loadAiNames(retail_.vfs);
            haveCb_ = retail_.haveCb;
            haveData_ = true;
            std::fprintf(stderr, "takserver: loaded game data from %s (referee sim + AI enabled%s), "
                         "retail gameplay hash %016llx\n",
                         dataRoot_.c_str(), haveCb_ ? ", +Crusades" : "",
                         (unsigned long long)retail_.hash);
        }
    }
    int run();

private:
    uint16_t port_;
    std::string dataRoot_;
    // Accounts. requireAuth_ is the default; --no-auth turns it off for a private
    // or LAN server (single-player launches one of those).
    bool requireAuth_ = true;
    bool loopbackOnly_ = false;
    tak::srv::AccountStore accounts_;
    tak::srv::LoginThrottle throttle_;
    // A mounted data set at one override tier: the VFS, its base + Crusades
    // registries, and the gameplay-data fingerprint peers are held to.
    struct DataSet {
        tak::hpi::Vfs vfs;
        tak::sim::TypeRegistry reg, regCb;
        bool haveCb = false;
        uint64_t hash = 0;
        bool built = false;
    };
    DataSet retail_, full_;            // none/cosmetic use retail_; full uses full_
    uint64_t relayHash_ = 0;           // pure-relay: the first client's hash (peers must match)
    bool relayHashSet_ = false;
    bool haveData_ = false, haveCb_ = false;
    tak::ai::Profile aiProfile_;
    // Per-mission build profiles (ai/<name>.txt), cached by name -- a Controller
    // holds a reference to its Profile, so these must outlive the room.
    std::map<std::string, tak::ai::Profile> missionProfiles_;
    const tak::ai::Profile& missionProfile(const tak::hpi::Vfs& vfs, const std::string& name) {
        auto it = missionProfiles_.find(name);
        if (it != missionProfiles_.end()) return it->second;
        return missionProfiles_.emplace(name, tak::ai::loadProfile(vfs, name)).first->second;
    }
    std::vector<std::string> aiNames_;   // retail's gamedata/ainames.tdf pool

    static std::vector<std::string> loadAiNames(const tak::hpi::Vfs& vfs) {
        std::vector<std::string> out;
        try {
            auto b = vfs.read("gamedata/ainames.tdf");
            auto root = tak::tdf::parseText(std::string(b.begin(), b.end()), "ainames.tdf");
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
    void buildDataSet(DataSet& ds, tak::hpi::OverridePolicy pol) {
        ds.vfs = tak::hpi::mountRetailRoot(dataRoot_, pol);
        tak::sim::setupRegistry(ds.reg, ds.vfs, false);
        if (!ds.vfs.list("unitscb").empty()) {
            tak::sim::setupRegistry(ds.regCb, ds.vfs, true);
            ds.haveCb = true;
        }
        ds.hash = tak::hpi::gameplayHash(ds.vfs);
        ds.built = true;
    }
    // The data set a game runs under, by its override policy (0/1 = retail, 2 = full).
    DataSet& dataFor(uint8_t policy) {
        DataSet& ds = (policy == 2) ? full_ : retail_;
        if (!ds.built) buildDataSet(ds, policy == 2 ? tak::hpi::OverridePolicy::Full
                                                    : tak::hpi::OverridePolicy::None);
        return ds;
    }
    const tak::sim::TypeRegistry& registryFor(bool crusades, uint8_t policy) {
        DataSet& ds = dataFor(policy);
        return (crusades && ds.haveCb) ? ds.regCb : ds.reg;
    }
    std::string replayDir_;
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
        return "user:" + tak::auth::foldUsername(user);
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
    void sendAuthResult(Client& c, AuthStatus st, const tak::crypto::Digest* sig,
                        const std::string& msg);
    void lobbyMsg(Client& c, const Frame& f);
    void gameMsg(Client& c, const Frame& f);

    void sendReject(Client& c, const std::string& why);
    void sendGameList(Client& c);
    void broadcastLobby(Room& r);
    void broadcastRoom(Room& r, Msg kind, const Writer& w, uint32_t exceptClient = 0);
    Room* roomOf(Client& c) { auto it = rooms_.find(c.roomId); return it == rooms_.end() ? nullptr : &it->second; }
    void leaveRoom(Client& c, const char* reason);
    void tryStart(Client& c);
    void closeTick(Room& r);
    bool canAdvance(const Room& r) const;   // false = wait for a lagging player (flow control)
    void checkHashes(Room& r, uint32_t tick);
    void dropClient(uint32_t id, const char* reason);
    void writeSlots(Writer& w, Room& r);
    void writeReplay(Room& r);
};

// A running game is abandoned once nobody is left to watch it. Normally that means
// no human slot has a live or held (dropped-within-grace) client. A game created by
// a SPECTATOR host to watch the AIs fight (single-player spectate / TAK_MP_WATCH) has
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

void Server::writeReplay(Room& r) {
    if (replayDir_.empty() || r.log.empty()) return;
    // Self-contained replay: header (format, map, options, final slot table,
    // seed) + every tick bundle. A viewer can rebuild the world and play it back.
    Writer w;
    for (char ch : {'T', 'A', 'K', 'R'}) w.u8(uint8_t(ch));
    w.u32(5);                 // replay format (5: + stressTest u8; 4: + monarchExpendable u8; 3: + unitCap u32; 2: + overridePolicy)
    w.u32(kNetVersion);
    w.str(r.mapId);
    w.u8(r.opts.crusades); w.u8(r.opts.gods); w.u8(r.opts.forfeitSelfDestruct);
    w.u8(r.opts.overridePolicy);
    w.u32(r.opts.unitCap); w.u8(r.opts.monarchExpendable); w.u8(r.opts.stressTest);
    w.u32(0x7a6b0000u + r.id);
    w.u8(uint8_t(kMaxSlots));
    for (int i = 0; i < kMaxSlots; ++i) {
        const SlotInfo& s = r.startSlots[i];   // start config, not the forfeited end state
        w.u8(s.type); w.u8(s.faction); w.u8(s.color); w.u8(s.team);
    }
    w.u32(uint32_t(r.log.size()));
    for (const auto& b : r.log) { w.u32(uint32_t(b.size())); w.b.insert(w.b.end(), b.begin(), b.end()); }
    std::string path = replayDir_ + "/game-" + std::to_string(r.id) + "-" +
                       std::to_string(r.createdMs) + ".takrep";
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
    // share the same overrides). With --data we hold the canonical referee hash;
    // as a pure relay we adopt the first client's and hold the rest to it.
    // The Hello hash is the client's PURE-RETAIL gameplay fingerprint (mounted with
    // no overrides), so the base game files must match regardless of anyone's
    // override tier. (Full-tier gameplay overrides are checked per game at Loaded.)
    uint64_t want = haveData_ ? retail_.hash : (relayHashSet_ ? relayHash_ : dataHash);
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
    if (!haveData_ && !relayHashSet_) { relayHash_ = dataHash; relayHashSet_ = true; }
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

void Server::sendAuthResult(Client& c, AuthStatus st, const tak::crypto::Digest* sig,
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
        std::vector<uint8_t> cnonce = r.bytes(tak::auth::kNonceLen);
        if (!r.ok) { sendReject(c, "malformed login"); c.conn.fail("bad auth"); return; }

        // Rate-limit on BOTH the name and the address, so neither hammering one
        // account from many hosts nor many accounts from one host gets a free run.
        // The address is checked first and always, which is also what stops this
        // endpoint being used to sweep for which usernames exist.
        if (uint64_t wait = loginLocked(c, user, now)) { sendThrottled(c, wait); return; }
        std::string why;
        if (!tak::auth::validUsername(user, &why)) {
            throttle_.fail(ipKeyFor(c), now, tak::srv::LoginThrottle::kAddress);
            sendAuthResult(c, AuthStatus::BadUsername, nullptr, why);
            return;
        }

        c.pendAuth = Client::PendingAuth{};
        c.pendAuth.user = user;
        c.pendAuth.clientNonce = std::move(cnonce);
        try {
            c.pendAuth.serverNonce = tak::crypto::randomVec(tak::auth::kNonceLen);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "takserver: %s\n", e.what());
            sendAuthResult(c, AuthStatus::ServerError, nullptr, "the server cannot sign you in");
            return;
        }
        const tak::srv::Account* a = accounts_.find(user);
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
            c.pendAuth.iters = tak::auth::kPbkdf2Iters;
            try {
                c.pendAuth.salt = tak::crypto::randomVec(tak::auth::kSaltLen);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "takserver: %s\n", e.what());
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
        std::vector<uint8_t> proofBytes = r.bytes(tak::crypto::kHashLen);
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
        const tak::srv::Account* a = accounts_.find(c.pendAuth.user);
        if (!a) { sendAuthResult(c, AuthStatus::BadPassword, nullptr, "that account no longer exists"); return; }

        tak::crypto::Digest proof{};
        std::memcpy(proof.data(), proofBytes.data(), proof.size());
        std::vector<uint8_t> am = tak::auth::authMessage(c.pendAuth.user, c.pendAuth.clientNonce,
                                                    c.pendAuth.serverNonce, a->cred.salt,
                                                    a->cred.iters);
        const std::string ipKey = ipKeyFor(c), userKey = userKeyFor(c.pendAuth.user);
        if (!tak::auth::verifyClientProof(a->cred, am, proof)) {
            throttle_.fail(ipKey, now, tak::srv::LoginThrottle::kAddress);
            throttle_.fail(userKey, now, tak::srv::LoginThrottle::kAccount);
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
        tak::crypto::Digest sig = tak::auth::serverSignature(a->cred.serverKey, am);
        c.account = a->name;
        c.name = a->name;
        std::string err;
        if (!accounts_.noteLogin(a->name, &err))
            std::fprintf(stderr, "takserver: could not record login: %s\n", err.c_str());
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
        std::vector<uint8_t> stored = r.bytes(tak::crypto::kHashLen);
        std::vector<uint8_t> serverKey = r.bytes(tak::crypto::kHashLen);
        if (!r.ok) { sendReject(c, "malformed registration"); c.conn.fail("bad auth"); return; }

        // Registration is the one unauthenticated operation that WRITES, and each
        // one rewrites the whole account file on the tick thread -- so a flood is
        // both a disk-filling attack and a way to stall every running game. Meter
        // it per address on the same escalating curve as a failed password.
        const std::string ipKey = ipKeyFor(c);
        if (uint64_t wait = throttle_.lockedFor(ipKey, now)) { sendThrottled(c, wait); return; }
        throttle_.fail(ipKey, now, tak::srv::LoginThrottle::kAddress);

        tak::auth::Credential cred;
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
            std::fprintf(stderr, "takserver: registration of '%s' failed: %s\n",
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
        std::vector<uint8_t> am = tak::auth::authMessage(c.pendAuth.user, c.pendAuth.clientNonce,
                                                         c.pendAuth.serverNonce, c.pendAuth.salt,
                                                         c.pendAuth.iters);
        tak::crypto::Digest sig = tak::auth::serverSignature(cred.serverKey, am);
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

void Server::writeSlots(Writer& w, Room& r) {
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
        const SlotInfo& s = r.slots[i];
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
                c.state = Client::InGame; c.roomId = room.id; c.slot = -1;
                Writer jr; jr.u8(1); jr.u8(0xFF); jr.str("");   // ok, spectator
                c.conn.send(Msg::JoinResult, jr);
            } else {
                // Host takes slot 0 (human).
                room.slots[0].type = 1; room.slots[0].faction = 0; room.slots[0].color = 0;
                room.slots[0].team = 0; room.slots[0].name = c.name;
                room.slotClient[0] = int(c.id);
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
            c.state = Client::InGame; c.roomId = gid; c.slot = slot; c.loaded = true;
            // GameStarting rebuilds the client's world; then the whole bundle log
            // replays it up to now, after which it receives live bundles.
            Writer w; writeSlots(w, room);
            w.u8(uint8_t(slot)); w.u32(0x7a6b0000u + room.id); w.u64(room.slotToken[slot]);
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
            c.state = Client::InGame; c.roomId = gid; c.slot = -1; c.loaded = true;
            // GameStarting (slot 0xFF = spectator) rebuilds the world; the whole
            // bundle log replays it to now, then live bundles stream via broadcast.
            Writer w; writeSlots(w, room);
            w.u8(0xFF); w.u32(0x7a6b0000u + room.id); w.u64(0);
            c.conn.send(Msg::GameStarting, w);
            c.replaying = true; c.replayPos = 0;   // streamed below, paced by txPending()
            std::fprintf(stderr, "game %u: client %u SPECTATING (replaying %zu ticks)\n",
                         room.id, c.id, room.log.size());
            break;
        }
        default: break;   // ignore other messages while in the lobby
    }
}

void Server::leaveRoom(Client& c, const char* reason) {
    Room* r = roomOf(c);
    if (!r) return;
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
            r->pendingEvents.push_back({tak::net::Event::Kind::Leave, uint8_t(c.slot)});
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
    // AI slots require the server to have game data (referee sim).
    bool anyAi = false;
    for (int i = 0; i < kMaxSlots; ++i) if (r->slots[i].type == 2) anyAi = true;
    if (anyAi && !haveData_) return;   // can't host AI without --data
    r->running = true;
    r->tick = 0;
    r->nextTickMs = nowMs();
    for (int i = 0; i < kMaxSlots; ++i) r->startSlots[i] = r->slots[i];   // for the replay
    // Build the referee sim (and AI controllers) if we have game data. The world
    // is built by the SAME setupMatch the clients use, so its hash is canonical.
    // Everything the referee reads comes from the data set for THIS game's override
    // tier (retail for none/cosmetic, full for full), so the referee's sim matches
    // the clients that adopted the same tier.
    DataSet* ds = haveData_ ? &dataFor(r->opts.overridePolicy) : nullptr;
    const bool isMission = !r->mission.empty();
    // A campaign mission builds its own world (placements + the in-sim god script) and
    // needs no map id; a skirmish resolves its map by name.
    std::string mapPath = (ds && !isMission) ? tak::hpi::findMap(ds->vfs, r->mapId) : std::string();
    if (haveData_ && !isMission && mapPath.empty())
        std::fprintf(stderr, "takserver: map '%s' not found in data; no referee sim\n",
                     r->mapId.c_str());
    if (ds && (isMission || !mapPath.empty())) {
        r->reg = &registryFor(r->opts.crusades != 0, r->opts.overridePolicy);
        r->ref = std::make_unique<tak::sim::World>();
        r->ref->setVisPlayer(-1);   // headless referee: no fog pass
        if (isMission) {
            // The client builds the SAME world (setupMission is deterministic), so the
            // referee and every peer stay in lockstep.
            int human = 0;
            tak::sim::MissionSetup ms;
            if (!tak::sim::setupMission(*r->ref, *r->reg, ds->vfs, r->mission, human, &ms)) {
                std::fprintf(stderr, "takserver: mission '%s' not found; no referee sim\n",
                             r->mission.c_str());
                r->ref.reset();
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
                const tak::ai::Profile& prof =
                    ms.aiProfile.empty() ? aiProfile_
                                         : missionProfile(ds->vfs, ms.aiProfile);
                for (int slot : ms.aiSlots) {
                    std::vector<std::pair<float, float>> enemyStarts;
                    for (size_t j = 0; j < ms.slotPos.size(); ++j) {
                        if (int(j) == slot || r->ref->allied(slot, int(j))) continue;
                        if (ms.slotPos[j].first == 0.0f && ms.slotPos[j].second == 0.0f) continue;
                        enemyStarts.push_back(ms.slotPos[j]);
                    }
                    r->ai.emplace_back(slot, *r->reg, prof, 0x7a6b0000u + r->id + uint32_t(slot),
                                       tak::ai::Difficulty::Normal, std::move(enemyStarts));
                }
                if (!ms.aiSlots.empty())
                    std::fprintf(stderr, "takserver: mission '%s' -- %zu strategic AI player(s)%s\n",
                                 r->mission.c_str(), ms.aiSlots.size(),
                                 ms.aiProfile.empty() ? "" : (" profile=" + ms.aiProfile).c_str());
            }
        } else {
            int maxSlot = 0;
            for (int i = 0; i < kMaxSlots; ++i)
                if (r->slots[i].type == 1 || r->slots[i].type == 2) maxSlot = i;
            tak::sim::MatchConfig cfg;
            cfg.vfs = &ds->vfs;
            cfg.mapPath = mapPath;
            cfg.gods = r->opts.gods != 0;
            cfg.unitCap = r->opts.unitCap;
            cfg.monarchExpendable = r->opts.monarchExpendable != 0;
            cfg.stressTest = r->opts.stressTest != 0;
            cfg.benchmark = r->opts.benchmark;
            cfg.randomStarts = r->opts.randomStarts != 0;
            cfg.startSeed = 0x7a6b0000u + r->id;   // the seed sent in GameStarting
            cfg.slots.resize(size_t(maxSlot + 1));
            for (int i = 0; i <= maxSlot; ++i) {
                const auto& s = r->slots[i];
                // Absurd AI slots earn double income (incomeMultFor); derived from the
                // shared aiLevel so the client mirror sets the same factor (lockstep).
                float mm = s.type == 2
                    ? tak::ai::incomeMultFor(tak::ai::difficultyFromLevel(s.aiLevel)) : 1.0f;
                cfg.slots[size_t(i)] = {s.type == 1 || s.type == 2, s.faction % 5, s.team, mm};
            }
            auto spots = tak::sim::setupMatch(*r->ref, *r->reg, cfg);
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
                    auto diff = tak::ai::difficultyFromLevel(r->slots[i].aiLevel);
                    r->ai.emplace_back(i, *r->reg, aiProfile_, 0x7a6b0000u + r->id,
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
        w.u32(0x7a6b0000u + r->id);       // per-game RNG seed base
        w.u64(r->slotToken[i]);           // resume token
        it->second->conn.send(Msg::GameStarting, w);
        it->second->loaded = false;
    }
    // Spectators (incl. a host who created the game to watch) get a slot-less
    // GameStarting; they don't gate the first tick, so mark them loaded.
    for (uint32_t sid : r->spectators) {
        auto it = clients_.find(sid);
        if (it == clients_.end()) continue;
        Writer w; writeSlots(w, *r);
        w.u8(0xFF); w.u32(0x7a6b0000u + r->id); w.u64(0);
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
            if (haveData_ && rd.ok && clientHash != 0) {
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
            if (c.cmdTick != r->tick) { c.cmdTick = r->tick; c.cmdBudget = kCmdCapPerTick; }
            // Server-side input delay (TAK_SRV_DELAY=K, default 0): bucket incoming
            // commands K ticks into the future instead of the very next tick, so a
            // command never "just misses" a tick boundary. Costs K ticks of latency.
            static const int srvDelay = [] {
                const char* e = std::getenv("TAK_SRV_DELAY"); return e ? std::max(0, std::atoi(e)) : 0;
            }();
            for (uint32_t i = 0; i < n && rd.ok; ++i) {
                Command cmd = rd.cmd();
                if (!rd.ok) break;
                cmd.player = uint8_t(c.slot);   // server stamps ownership
                if (c.cmdBudget <= 0) {         // over this tick's budget: defer
                    if (c.cmdOverflow.size() < kCmdOverflowMax) c.cmdOverflow.push_back(cmd);
                    continue;                   // past the backlog cap it IS dropped
                }
                --c.cmdBudget;
                if (srvDelay > 0) r->pendingAt[r->tick + uint32_t(srvDelay)].push_back(cmd);
                else r->pending.push_back(cmd);
            }
            break;
        }
        case Msg::StateHash: {
            if (!r->running) return;
            Reader rd(f.payload.data(), f.payload.size());
            uint32_t tk = rd.u32(); uint64_t h = rd.u64();
            if (!rd.ok) return;
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

    // The canonical hash: the referee's for this tick if we ran a referee sim,
    // else the clients' majority (relay-only mode).
    bool haveRef = r.ref && r.refHash.count(tick);
    uint64_t canon = 0;   // set from the referee hash or the client majority below
    if (haveRef) {
        canon = r.refHash[tick];
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
    } else {
        std::map<uint64_t, int> tally;
        for (auto& [cid, h] : it->second) ++tally[h];
        int best = -1;
        for (auto& [h, cnt] : tally) if (cnt > best) { best = cnt; canon = h; }
    }
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
    // Deferred commands first: anything a client sent past last tick's budget gets
    // this tick's allowance before anything new does, so a big order arrives in
    // order and merely late rather than partially.
    for (int i = 0; i < kMaxSlots; ++i) {
        if (r.slotClient[i] < 0) continue;
        auto ci = clients_.find(uint32_t(r.slotClient[i]));
        if (ci == clients_.end()) continue;
        Client& c = *ci->second;
        if (c.cmdOverflow.empty()) continue;
        c.cmdTick = r.tick;
        c.cmdBudget = kCmdCapPerTick;
        while (!c.cmdOverflow.empty() && c.cmdBudget > 0) {
            r.pending.push_back(c.cmdOverflow.front());
            c.cmdOverflow.pop_front();
            --c.cmdBudget;
        }
    }
    // Server-hosted AI: each controller observes the referee world (state after
    // tick-1) and appends its orders to this tick's bundle, exactly like a client.
    if (r.ref) {
        static const bool kAiPhase = std::getenv("TAK_AIPHASE") != nullptr;
        auto _a0 = std::chrono::steady_clock::now();
        for (auto& ctl : r.ai)
            ctl.tick(*r.ref, r.tick, [&r](const Command& c) { r.pending.push_back(c); });
        if (kAiPhase) {
            double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - _a0).count();
            static double thr = std::getenv("TAK_AIPHASE_MS")
                                    ? atof(std::getenv("TAK_AIPHASE_MS")) : 8.0;
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
    if (r.ref) {
        for (const auto& cmd : r.pending) tak::sim::applyCommand(*r.ref, *r.reg, cmd);
        for (const auto& e : r.pendingEvents) tak::sim::applyEvent(*r.ref, e);
        r.ref->tick(1.0f / kServerHz);
        if (r.tick % uint32_t(kHashPeriod) == 0)
            r.refHash[r.tick] = r.ref->stateHash();
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
    if (listenFd_ < 0) { std::fprintf(stderr, "takserver: %s on port %u\n", err.c_str(), port_); return 1; }
    std::fprintf(stderr, "takserver %s listening on %s port %u (protocol v%u)\n",
                 tak::kVersion, loopbackOnly_ ? "loopback" : "all interfaces", port_, kNetVersion);
    if (requireAuth_)
        std::fprintf(stderr, "takserver: accounts required -- %zu in %s\n",
                     accounts_.size(), accounts_.path().c_str());
    else
        std::fprintf(stderr, "takserver: NO ACCOUNTS REQUIRED (--no-auth)%s\n",
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

        int n = TAK_POLL(pfds.data(), (unsigned)pfds.size(), timeout);
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
                r.pendingEvents.push_back({tak::net::Event::Kind::Forfeit, uint8_t(i)});
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
    if (const char* g = std::getenv("TAK_GRACE_MS")) kGraceMs = uint64_t(std::atoll(g));
    if (const char* b = std::getenv("TAK_PAUSE_BUDGET_MS")) kPauseBudgetMs = uint64_t(std::atoll(b));
    uint16_t port = 7677;
    std::string dataRoot, replayDir;
    std::string accountsPath = "takserver-accounts.conf";
    bool noAuth = false, loopbackOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = uint16_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) dataRoot = argv[++i];
        else if (!std::strcmp(argv[i], "--replaydir") && i + 1 < argc) replayDir = argv[++i];
        else if (!std::strcmp(argv[i], "--accounts") && i + 1 < argc) accountsPath = argv[++i];
        else if (!std::strcmp(argv[i], "--no-auth")) noAuth = true;
        else if (!std::strcmp(argv[i], "--local")) loopbackOnly = true;
        else if (!std::strcmp(argv[i], "--version") || !std::strcmp(argv[i], "-v")) {
            std::printf("takserver (TAK engine) %s\n", tak::kVersion);
            return 0;
        }
        else if (!std::strcmp(argv[i], "--help")) {
            std::printf("usage: takserver [--port N] [--data <retail-install-dir>]\n"
                        "                 [--replaydir <dir>] [--accounts <file>]\n"
                        "                 [--no-auth] [--local]\n"
                        "  --data enables the referee sim + server-hosted AI; without it the\n"
                        "  server is a pure relay (clients cross-check hashes among themselves).\n"
                        "  --replaydir writes a .takrep replay file per finished game.\n"
                        "  --accounts is the account file (default takserver-accounts.conf).\n"
                        "  Players sign in with a name and password; an unused name is\n"
                        "  registered on the spot. No password is stored or transmitted --\n"
                        "  see src/net/auth.h.\n"
                        "  --no-auth serves anyone who connects, with no account at all. Only\n"
                        "  for a private or LAN server; pair it with --local.\n"
                        "  --local binds loopback only, so nothing off this machine connects.\n");
            return 0;
        }
    }
    Server s(port, dataRoot);
    if (!replayDir.empty()) s.setReplayDir(replayDir);
    if (loopbackOnly) s.setLoopbackOnly();
    if (noAuth) {
        s.setNoAuth();
    } else {
        std::string err;
        if (!s.loadAccounts(accountsPath, err)) {
            // Refuse to start rather than come up looking like a server with
            // accounts while actually having none of them.
            std::fprintf(stderr, "takserver: %s\n", err.c_str());
            return 1;
        }
    }
    return s.run();
}
