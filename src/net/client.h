#pragma once

// Client-side handler for the multiplayer protocol (docs/multiplayer-design.md).
// Owns the connection to takserver and the lobby/in-game state. The viewer polls
// it, reads the lobby view for its UI, and in game pulls one TickBundle per tick.
// This class does protocol only -- it never touches the sim; the viewer applies
// the bundle's commands and advances the World.

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "net/auth.h"
#include "net/conn.h"
#include "net/protocol.h"

namespace tak::net {

// The commands + sequenced events the server assigned to one tick.
struct Bundle {
    std::vector<Command> cmds;
    std::vector<Event> events;
};

// A game's setup as the client sees it in the lobby / at start.
struct RoomView {
    uint32_t id = 0;
    std::string name, mapId;
    std::string mission;   // campaign mission stem (empty = ordinary skirmish/MP)
    GameOptions opts;
    uint32_t hostId = 0;
    SlotInfo slots[kMaxSlots];
    int mySlot = -1;
};

class MpClient {
public:
    enum class State { Offline, Connecting, Lobby, InRoom, Starting, InGame, Done };

    // Set the local gameplay-data fingerprint (hpi::gameplayHash) BEFORE connect;
    // it goes in the Hello so the server can reject a peer whose retail gameplay
    // data (or, under Full overrides, gameplay overrides) doesn't match.
    void setDataHash(uint64_t h) { dataHash_ = h; }

    bool connect(const std::string& host, uint16_t port, const std::string& name);
    void disconnect(const std::string& reason = "bye");
    ~MpClient();

    // ---- account login -----------------------------------------------------
    // Credentials for a server that requires an account. Set BEFORE connect().
    // The password is used to derive the login keys and then wiped: it is not
    // kept, and it never goes on the wire in any form (see src/net/auth.h).
    // Against a server that asks for no account these are simply unused.
    void setLogin(const std::string& user, const std::string& password);

    enum class Auth {
        None,       // the server never asked -- an open server (LAN, single-player)
        Pending,    // asked; the exchange (or the slow key derivation) is in flight
        Ok,         // signed in to an existing account
        Created,    // there was no such account, so one was made; signed in on it
        Failed,     // refused -- error() says why, in words fit to show the player
    };
    Auth auth() const { return auth_; }
    // True once the handshake has settled either way, so a caller can pump poll()
    // until the answer is in rather than guessing at a delay.
    bool handshakeSettled() const {
        return state_ != State::Connecting || auth_ == Auth::Failed;
    }
    // The account we are signed in as -- empty on a server that asks for none.
    // This, not the local settings name, is who the player is in a lobby.
    const std::string& account() const { return account_; }

    // Pump the socket: read frames, dispatch, flush writes. Call every frame.
    // Returns false once the connection is gone (see error()).
    bool poll();

    State state() const { return state_; }
    const std::string& error() const { return err_; }
    uint32_t myClientId() const { return myId_; }

    // ---- lobby actions -----------------------------------------------------
    void listGames();
    // spectate=true: create the game but DON'T seat the host -- it watches as a
    // slot-less spectator, freeing all capacity slots (e.g. for an all-AI game).
    // priv=true: a private (single-player) game -- omitted from the public game
    // list so other players don't see it. Still hosted on the server (AIs run
    // there); the host just doesn't advertise it.
    // mission: a campaign mission stem (e.g. "takmission01_mt"). When set, the server
    // builds the world from the mission (placements + in-sim god script) instead of a
    // skirmish, and reports MissionOutcome; see docs/campaign-design.md.
    void createGame(const std::string& name, const std::string& password,
                    const std::string& mapId, const GameOptions& opts, uint8_t capacity,
                    bool spectate = false, bool priv = false, const std::string& mission = "");
    void joinGame(uint32_t id, const std::string& password);
    void leaveGame();
    void setSlot(int slot, uint8_t type, uint8_t faction, uint8_t color,
                 uint8_t team, uint8_t ready, uint8_t aiLevel = 2);
    void kick(int slot);
    void chat(const std::string& text);
    void startGame();
    // Host: change room options. In the lobby the server rebroadcasts them; in-game
    // only `speed` takes effect (re-cadences the sim), if speedUnlock was set.
    void setGameOptions(const GameOptions& opts);
    // Ask the server to pause/resume a running game (host only; in single-player
    // the host is the only human). The server stops issuing ticks, so every peer
    // freezes on the same tick -- a real pause, not a local one.
    void setPause(bool want);
    // Current game speed in tenths (10 = 1.0x), tracking in-game SpeedUpdate broadcasts.
    uint8_t gameSpeed() const { return gameSpeed_; }
    // Campaign mission result the server reported: 0 = running, +1 = victory, -1 = defeat.
    int missionOutcome() const { return missionOutcome_; }

    const std::vector<GameInfo>& games() const { return games_; }
    const RoomView& room() const { return room_; }
    // Ticks of HISTORY the server is replaying to us after a rejoin or spectate.
    // The authoritative end of the replay: local buffer depth cannot tell history
    // from a gap between chunks.
    uint32_t replayTicks() const { return replayTicks_; }
    // Drain chat lines received since the last call (sender, text).
    std::vector<std::pair<std::string, std::string>> takeChat() { auto c = std::move(chat_); chat_.clear(); return c; }

    // ---- game start / play -------------------------------------------------
    // After GameStarting the viewer reads these, loads the map, then calls
    // reportLoaded(). The server begins ticking once every human has loaded.
    bool starting() const { return state_ == State::Starting; }
    const RoomView& startRoom() const { return room_; }   // final slots
    uint32_t startSeed() const { return startSeed_; }
    // Has this slot reported its map/data loaded? Broadcast by the server as each
    // peer reports in, so the loading screen can show who the game is waiting on.
    bool slotLoaded(int slot) const {
        return slot >= 0 && slot < kMaxSlots && slotLoaded_[size_t(slot)];
    }
    uint32_t gameId() const { return room_.id; }
    uint64_t resumeToken() const { return resumeToken_; }
    // `dataHash` = this client's gameplay-data fingerprint at the ROOM's tier
    // (after adopting its override policy); the server rejects it if it doesn't
    // match the room's expected data, so a Full-tier override mismatch is caught
    // before the game starts, not as a mid-game desync.
    void reportLoaded(uint64_t dataHash = 0);
    // Reconnect to a held slot: connect() first, then rejoin() with the game id
    // and the resume token saved from the original GameStarting.
    void rejoin(uint32_t gameId, uint64_t token);
    // Watch a running game (no slot): connect() first, then spectate(). The server
    // replies with GameStarting (slot 0xFF -> mySlot -1) and the bundle log, then
    // streams live bundles. A spectator sends no commands or hashes.
    void spectate(uint32_t gameId, const std::string& password);
    bool isSpectator() const { return spectator_; }
    // True when the last GameStarting was a REJOIN (the world must be rebuilt and
    // the bundle log replayed). Cleared once consumed.
    bool isRejoin() const { return rejoin_; }
    void clearRejoin() { rejoin_ = false; }
    bool paused() const { return paused_; }

    // In game: is bundle `tick` available? If so, consume it (once).
    bool haveBundle(uint32_t tick) const { return bundles_.count(tick) != 0; }
    bool takeBundle(uint32_t tick, Bundle& out);
    size_t bufferedBundles() const { return bundles_.size(); }   // backlog depth
    void sendCommands(const std::vector<Command>& cmds);
    void sendHash(uint32_t tick, uint64_t hash);

    // Set once the server flags us desynced (fatal in M3; reconnect is M5).
    bool desynced() const { return desynced_; }
    std::string desyncReason() const { return desyncReason_; }

private:
    void onFrame(const Frame& f);
    void sendAuthBegin();            // Hello answered with AuthRequired -> start SCRAM
    void onAuthChallenge(Reader& r);
    void onAuthResult(Reader& r);
    void startDerive();              // spin the PBKDF2 off this thread
    void pumpDerive();               // ... and send the proof once it lands
    void failAuth(const std::string& why);
    void send(Msg kind, const Writer& w) { conn_.send(kind, w); }
    void send(Msg kind) { conn_.send(kind); }

    Conn conn_;
    State state_ = State::Offline;
    std::string err_;
    std::string name_;
    uint32_t myId_ = 0;

    std::vector<GameInfo> games_;
    RoomView room_;
    uint8_t gameSpeed_ = 10;   // live game speed (from opts + in-game SpeedUpdate)
    int missionOutcome_ = 0;   // campaign mission result (0 running / +1 win / -1 loss)
    std::vector<std::pair<std::string, std::string>> chat_;

    uint64_t dataHash_ = 0;      // local gameplay-data fingerprint (sent in Hello)
    uint32_t startSeed_ = 0;
    uint32_t replayTicks_ = 0;   // history the server replays after a rejoin/spectate
    std::array<bool, kMaxSlots> slotLoaded_{};   // per-slot "reported Loaded"
    uint64_t resumeToken_ = 0;
    bool rejoin_ = false;
    bool expectingRejoin_ = false;
    bool spectator_ = false;
    bool expectingSpectate_ = false;
    bool paused_ = false;
    std::map<uint32_t, Bundle> bundles_;
    bool desynced_ = false;
    std::string desyncReason_;
    uint64_t lastRecvMs_ = 0, lastPingMs_ = 0;

    // ---- account login state ------------------------------------------------
    std::string loginUser_, loginPass_;
    Auth auth_ = Auth::None;
    std::string account_;
    struct PendingAuth {
        std::vector<uint8_t> clientNonce, serverNonce, salt;
        uint32_t iters = 0;
        bool newAccount = false;     // the server said there is no such account yet
        bool challenged = false;     // a challenge arrived; a second one is refused
        bool proofSent = false;
    } pend_;
    // PBKDF2 at the real work factor takes a few hundred milliseconds. Running it
    // on the caller's thread would stall the front-end mid-login, so it goes to a
    // worker and poll() picks the result up.
    std::thread derive_;
    std::atomic<bool> deriveDone_{false};
    auth::Keys keys_;

    // Artificial receive jitter for testing (TAK_NET_JITTER_MS): hold each bundle
    // then release it after a random 0..N ms delay, modelling uneven server->client
    // delivery. Test-only; does not touch command/sim state.
    struct HeldBundle { uint64_t releaseMs; uint32_t tick; Bundle bd; };
    std::vector<HeldBundle> jitterHeld_;
    int jitterMs_ = -1;                 // <0 = read env once; then 0 = off
    int baseMs_ = 0;                    // TAK_NET_BASE_MS: constant one-way delay
    int lossPct_ = 0;                   // TAK_NET_LOSS_PCT: retransmit-spike probability
    uint32_t rng_ = 0x2545F491u;        // xorshift (non-sim, non-determinism OK)
    uint32_t xorshift() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }
    // Modelled one-way receive delay for a message: base + uniform jitter, plus an
    // occasional ~2*base retransmit spike (TCP loss). 0 when no model is configured.
    uint64_t receiveDelayMs() {
        if (baseMs_ <= 0 && jitterMs_ <= 0 && lossPct_ <= 0) return 0;
        uint64_t d = uint64_t(baseMs_) + (jitterMs_ > 0 ? xorshift() % uint32_t(jitterMs_ + 1) : 0);
        if (lossPct_ > 0 && int(xorshift() % 100) < lossPct_)
            d += uint64_t(baseMs_) * 2 + uint64_t(jitterMs_);   // retransmit + head-of-line
        return d;
    }
    // Round-trip time, measured with active pings when a consumer asks for it
    // (the adaptive jitter buffer). Off by default -> no extra traffic.
    bool measureRtt_ = false;
    uint64_t pingSentMs_ = 0;           // outstanding RTT ping send time (0 = none)
    float rttMs_ = 0;                   // smoothed round-trip time
    // Bundle-arrival jitter: how much later than the 30 Hz schedule bundles land,
    // as a slowly-decaying recent max. This is what the jitter buffer must cover.
    uint64_t lastBundleMs_ = 0;
    float arrivalJitterMs_ = 0;
    void noteBundleArrival(uint64_t now) {
        if (lastBundleMs_) {
            float late = float(now - lastBundleMs_) - 1000.0f / 30.0f;
            arrivalJitterMs_ = std::max(std::max(0.0f, late), arrivalJitterMs_ - 0.4f);
        }
        lastBundleMs_ = now;
    }
public:
    float rttMs() const { return rttMs_; }
    float arrivalJitterMs() const { return arrivalJitterMs_; }
    void enableRttProbe() { measureRtt_ = true; }
private:
};

}  // namespace tak::net
