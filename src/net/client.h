#pragma once

// Client-side handler for the multiplayer protocol (docs/multiplayer-design.md).
// Owns the connection to takserver and the lobby/in-game state. The viewer polls
// it, reads the lobby view for its UI, and in game pulls one TickBundle per tick.
// This class does protocol only -- it never touches the sim; the viewer applies
// the bundle's commands and advances the World.

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "net/auth.h"
#include "net/crypto.h"
#include "net/mappackage.h"
#include "net/modpackage.h"
#include "net/pacing.h"
#include "net/conn.h"
#include "net/protocol.h"
#include "net/replayhdr.h"

namespace tak::net {

// A game's setup as the client sees it in the lobby / at start: the server's RoomState
// (net/protocol.h) and this player's seat (-1: none, watching).
struct RoomView : RoomState {
    int mySlot = -1;
};

// The sign-in token a password login on an encrypted connection earned (see
// src/net/auth.h). Its owner keeps it across sessions, each with its own MpClient:
// in memory only, for as long as the program runs, never in a file.
struct SessionToken {
    std::string host;      // the address connect() was given; the only place it may go
    uint16_t port = 0;
    std::string account;
    std::vector<uint8_t> secret;
    uint64_t earnedMs = 0;              // when its password login was (MpClient::clockMs)
    // Tokens no longer used to sign in that may still be good on their server: a newer
    // login replaced them, or this one's fate is unknown. Only for quitting to revoke.
    std::vector<SessionToken> retired;

    SessionToken() = default;
    SessionToken(const SessionToken&) = delete;
    SessionToken& operator=(const SessionToken&) = delete;
    SessionToken(SessionToken&&) = default;
    ~SessionToken() { wipe(); }
    bool heldFor(const std::string& h, uint16_t p) const {
        return !secret.empty() && host == h && port == p;
    }
    void wipe() {
        crypto::wipe(secret.data(), secret.size());
        secret.clear();
    }
    void retire() {
        if (secret.empty()) return;
        SessionToken old;
        old.host = host; old.port = port; old.account = account; old.earnedMs = earnedMs;
        old.secret = std::move(secret);
        secret.clear();
        retired.push_back(std::move(old));
    }
};

class MpClient {
public:
    enum class State { Offline, Connecting, Lobby, InRoom, Starting, InGame, Done };

    // Set the local gameplay-data fingerprint (hpi::gameplayHash) BEFORE connect;
    // it goes in the Hello so the server can reject a peer whose retail gameplay
    // data doesn't match.
    void setDataHash(uint64_t h) { dataHash_ = h; }

    // The game data folder: read only, never written.
    void setMapRoot(const std::filesystem::path& root) { mapRoot_ = root; rescanAlwaysGameMods(); }
    // Where verified map and game-mod downloads (MapCache/, ModCache/) and saved
    // generated maps (Maps/) go. Default: hpi::userMapRoot(); none when both are empty.
    void setCacheRoot(const std::filesystem::path& root) { cacheRoot_ = root; }
    std::filesystem::path cacheRoot() const { return cacheRoot_.empty() ? hpi::userMapRoot() : cacheRoot_; }
    const std::shared_ptr<maps::Package>& mapPackage() const { return mapPackage_; }
    // The host's ticked mods/game packs. A room this player hosts sends them, with
    // the always-enabled ones (mods/game/always_enabled/), as one package to every
    // player whenever the room has game mods on (every non-campaign room a host
    // creates, unless a debug --mods turned them off).
    void setGameModPacks(std::vector<std::string> names);
    const std::vector<std::string>& gameModPacks() const { return gameModPacks_; }
    // False (debug --mods none|cosmetic) drops every game pack, always-enabled ones too.
    void setGameModsAllowed(bool allowed) { gameModsAllowed_ = allowed; rescanAlwaysGameMods(); if (!allowed) setGameModPacks({}); }
    // This install's always-enabled game packs (empty when game mods are not allowed):
    // applied only in rooms this player hosts. A guest uses the host's package alone.
    const std::vector<std::string>& alwaysGameModPacks() const { return alwaysGameModPacks_; }
    // What this player's hosted room applies: always-enabled + ticked, sorted
    // (disjoint: an always-enabled pack hides its ordinary namesake), at most 64.
    std::vector<std::string> hostGameModPacks() const;
    const std::shared_ptr<mods::Package>& modPackage() const {return modPackage_;}
    const std::string& modStatus() const {return modStatus_;}
    // The game-mod status a room shows: a transfer in progress or an error always, but
    // the plain "GAME MODS VERIFIED" only when the verified package carries a pack. An
    // empty selection is verified too (so every peer agrees there are none), yet saying
    // so would read as if game packs were in play.
    std::string roomModStatus() const {return roomModStatus(modStatus_,modPackage_.get());}
    static std::string roomModStatus(const std::string& status,const mods::Package* package) {
        // A package's mapPath names its packs ("A, B"); it is empty when it has none.
        if(status=="GAME MODS VERIFIED" && (!package || package->mapPath.empty()))return {};
        return status;
    }
    bool modPending() const {return modPendingAt_!=0;}
    bool modBusy() const {return modSend_.package || modReceive_.size;}
    // The VFS a game in this room mounts: the room's verified game-mod package
    // (when game mods are on), then the always-enabled cosmetic packs of
    // mods/cosmetic/always_enabled/ (every room), then this player's own ticked cosmetic packs (every room but
    // a campaign mission; packs no longer on disk are skipped).
    // `withCosmetic` false (debug --mods none|game) leaves every cosmetic layer out.
    // A cosmetic layer that cannot load or would change the gameplay hash is left
    // out and named in `skipped` (hpi::mountPlayVfs).
    hpi::Vfs modVfs(const std::vector<std::string>& cosmeticPacks, bool withCosmetic = true,
                    std::vector<std::string>* skipped = nullptr) const;
    // The room's game data without any cosmetic layer (map packages, generated
    // maps): the host's game packs, or a guest's copy of the host's package.
    hpi::Vfs roomDataVfs() const;
    const std::string& mapStatus() const { return mapStatus_; }
    bool connect(const std::string& host, uint16_t port, const std::string& name);
    void disconnect(const std::string& reason = "bye");
    ~MpClient();

    // ---- account login -----------------------------------------------------
    // Credentials for a server that requires an account. Set BEFORE connect().
    // The password is used to derive the login keys and then wiped: it is not
    // kept, and it never goes on the wire in any form (see src/net/auth.h).
    // Against a server that asks for no account these are simply unused.
    void setLogin(const std::string& user, const std::string& password);
    // Where a login keeps the sign-in token it earns (see SessionToken), which must
    // outlive this client. `signIn`: connect() signs in by spending the token held
    // there, for the setLogin() user, with no password. If it cannot, auth() ends up
    // Failed like any refused login, and the password is what is left to try.
    void setSessionToken(SessionToken* token, bool signIn = false);
    // Quitting is signing out: tell the server each token held, retired ones included,
    // is no longer good, on a connection of its own, and wipe them. Best effort, and
    // bounded to a few seconds: one that could not be revoked still dies with its 12 hours.
    static void revokeSessionToken(SessionToken& token, uint64_t dataHash);

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
    // The server is a player's LOCAL SERVER (takserver --lan): the Welcome said so.
    bool lanServer() const { return lanServer_; }

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
    // name: sent as printable ASCII only, cut to kRoomNameMax bytes; an empty result asks
    // for the server's "<player>'s Game". A bad password is the server's to refuse.
    void createGame(const std::string& name, const std::string& password,
                    const std::string& mapId, const GameOptions& opts, uint8_t capacity,
                    bool spectate = false, bool priv = false, const std::string& mission = "");
    void joinGame(uint32_t id, const std::string& password);
    // Leave the room; or, in the game list, take back the CreateGame, JoinGame or Spectate
    // still on its way (LeaveGame follows it to the server, so a room it opens there is left
    // at once). Either way this client is in no room from here: what the server sent it as
    // a member of that room, or in answer to that request, is dropped as it arrives -- the
    // room's tables and transfers, a late yes or no, a watched game's start (see takes()).
    // What was already taken of a game's stream goes too (its bundles, pause, wait, chat
    // and voice: a watch still loading, watchLoading(), has taken them before its start).
    // The next request may go out at once; its answer is told apart from those.
    void leaveGame();
    void setSlot(int slot, uint8_t type, uint8_t faction, uint8_t color,
                 uint8_t team, uint8_t ready, uint8_t aiLevel = 2);
    void kick(int slot);
    // Take another OPEN seat in the lobby (Msg::MoveSeat). The server answers with
    // a JoinResult naming the new seat, so mySlot follows it, then the LobbyState.
    void moveSeat(int slot);
    // recipients: the seated players (slot bits); spectators: whether spectators read it
    // too. Before the game starts the server sends every line to the whole room.
    void chat(const std::string& text, uint8_t recipients, bool spectators = false);
    void chat(const std::string& text) { chat(text, 0xff, true); }   // everyone
    void startGame();
    // START was clicked and the server has neither started the game nor refused.
    bool startPending() const { return startRequested_ || startSent_; }
    // Host: change room options. In the lobby the server rebroadcasts them; in-game
    // only `speed` takes effect (re-cadences the sim), if speedUnlock was set.
    void setGameOptions(const GameOptions& opts);

    // ---- room editing: the host, in its room's lobby (never a mission or Benchmark room) ----
    // True when this client may edit its room now: InRoom, the host, not a mission or Benchmark
    // room, no START pending or starting.
    bool roomEditable() const;
    // Change the room's map (Msg::SetRoomMap). Returns false and sends nothing (roomError()
    // says why) unless roomEditable(), when a SetRoomMap is still unanswered, or when a file
    // map cannot be built here. A file map is built into a pending package first (from the
    // room's data, as createGame builds); the room's own package is untouched until the
    // server confirms. randomStarts: the host's chosen start mode (it applies when the
    // scenario ends up off).
    bool setRoomMap(const std::string& mapId, uint8_t capacity, bool mapScenario, uint8_t randomStarts);
    // The unanswered SetRoomMap's serial, 0 = none. It clears when the LobbyState or RoomError
    // carrying it arrives, on GameStarting, on leaving, when this client stops being host, or
    // 5 s after sending.
    uint32_t roomMapPending() const { return mapSerialSent_; }
    struct RoomMapAnswer {
        uint32_t serial = 0;
        enum Kind : uint8_t { None, Accepted, Refused, TimedOut } kind = None;
        std::string why;   // Refused: the server's reason
    };
    const RoomMapAnswer& roomMapAnswer() const { return mapAnswer_; }   // the last one; the view acts when serial changes
    // Msg::SetRoomInfo. Only the fields given are sent and applied.
    void setRoomInfo(std::optional<std::string> name, std::optional<std::string> password);
    void watch() { moveSeat(kWatchSeat); }   // JoinResult 0xFF (isSpectator())
    // The last RoomError (or a local refusal of setRoomMap); cleared by the next room edit.
    const std::string& roomError() const { return roomError_; }
    RoomRequest roomErrorFor() const { return roomErrorFor_; }
    uint64_t lobbySeq() const { return lobbySeq_; }   // +1 per LobbyState
    // The host's own build of the room's file map failed (a lobby successor without the map).
    bool hostMapMissing() const { return hostMapMissing_; }
    // Ask the server to pause/resume a running game (host only; in single-player
    // the host is the only human). The server stops issuing ticks, so every peer
    // freezes on the same tick -- a real pause, not a local one.
    void setPause(bool want);
    // Current game speed in tenths (10 = 1.0x), tracking in-game SpeedUpdate broadcasts.
    uint8_t gameSpeed() const { return gameSpeed_; }
    // Campaign mission result the server reported: 0 = running, +1 = victory, -1 = defeat.
    int missionOutcome() const { return missionOutcome_; }
    // The server ended the game this client is watching (Msg::GameOver): it closes no more
    // ticks, so the game is its first gameOverTicks() bundles and is over here once those
    // are played. gameOverTeam(): the winning team then, -1 = nobody. Cleared by the next
    // room request and by leaveGame() -- not by GameStarting, which a watcher may still be
    // holding back for a package when the game ends (watchLoading()).
    bool gameOver() const { return gameOver_; }
    uint32_t gameOverTicks() const { return gameOverTicks_; }
    int gameOverTeam() const { return gameOverTeam_; }

    const std::vector<GameInfo>& games() const { return games_; }
    const RoomView& room() const { return room_; }
    // Why the server refused the last create / join / watch / reconnect ("wrong password",
    // "spectators are not allowed in this game", "server game capacity reached", ...);
    // empty once a later request is sent or one succeeds. The connection stays in the
    // lobby, so the browser shows it. A request taken back (leaveGame) leaves none.
    const std::string& joinError() const { return joinError_; }
    // Why the last createGame() could not even be requested (the map failed to build locally);
    // cleared by the next createGame(). The connection stays up in the lobby.
    const std::string& createError() const { return createError_; }
    // Why the server refused this client's last START (MAP SCENARIO's missing seat, a map
    // without a scenario, setupMatch's refusal, maps not verified). Cleared by startGame().
    const std::string& startError() const { return startError_; }
    // Ticks of HISTORY the server is replaying to us after a spectate.
    // The authoritative end of the replay: local buffer depth cannot tell history
    // from a gap between chunks.
    uint32_t replayTicks() const { return replayTicks_; }
    // Drain chat lines received since the last call (sender, text).
    std::vector<std::pair<std::string, std::string>> takeChat() { auto c = std::move(chat_); chat_.clear(); return c; }

    // ---- voice chat (presentation only, see net/protocol.h) ------------------------
    // Whether this connection wants voice relayed to it (voice chat switched on). Sent
    // only when it changes, and again after every new connection; the server sends no
    // voice to a connection that never said yes.
    void setVoiceListen(bool on);
    // One captured frame, as a LOW-PRIORITY message (Conn::sendLow): it waits behind game
    // traffic queued in this program and in the kernel, and is dropped when the link is
    // backed up. Ignored outside a room.
    // recipients: the diplomacy VOICE mask (0xff = everyone, as for chat).
    // The uplink is guarded too, since what the kernel already holds still goes ahead of
    // the Progress and commands queued after it: while this client transmits it pings the
    // server four times a second (and every two seconds while voice is on, for a quiet
    // baseline), and a round trip more than max(100 ms, 4 ticks) above that baseline (back,
    // or still out) means its own voice is queueing on the link. Voice is then held -- captured frames are
    // dropped, an END closes the transmission at once, and the queued voice is thrown
    // away -- for a back-off of 5 s that doubles with each repeat (to 60 s, forgotten after
    // 2 quiet minutes), and resumes (with a START) once the probe is quiet again.
    void sendVoice(uint16_t seq, uint8_t recipients, uint8_t flags, const std::vector<uint8_t>& opus);
    bool voiceHeld() const { return voiceHeld_; }
    uint64_t voiceHeldFrames() const { return voiceHeldFrames_; }   // captured frames the hold dropped
    struct VoiceFrame {
        uint32_t talker = 0;      // the talker's connection id
        uint8_t slot = 0xFF;      // their player slot, 0xFF = spectator
        uint16_t seq = 0;
        uint8_t flags = 0;
        std::string name;         // only on frames that carry it (kVoiceHasName)
        std::vector<uint8_t> opus;
    };
    // Drain the voice frames received since the last call. Bounded: past kVoiceInbox
    // frames the oldest are dropped, so a host that stops draining holds no backlog.
    static constexpr size_t kVoiceInbox = 512;
    std::vector<VoiceFrame> takeVoice() { auto v = std::move(voice_); voice_.clear(); return v; }
    uint64_t voiceDropped() const { return voiceDropped_; }

    // ---- game start / play -------------------------------------------------
    // After GameStarting the viewer reads these, loads the map, then calls
    // reportLoaded(). The server begins ticking once every human has loaded.
    bool starting() const { return state_ == State::Starting; }
    const RoomView& startRoom() const { return room_; }   // final slots
    uint32_t startSeed() const { return startSeed_; }
    // ---- replay recording -------------------------------------------------
    //
    // Every TickBundle payload this client received, in tick order, byte-for-byte as
    // the server broadcast it -- which is the SAME buffer the server appends to its
    // own replay log, so a client-written .takrep is identical to a server-written
    // one. Filled only while recording (see startRecording).
    //
    // A spectator is sent the whole backlog before the live stream, so a recording
    // that starts late still ends up complete; a reconnect continues it from the first
    // bundle the old connection did not bring.
    const std::vector<std::vector<uint8_t>>& replayLog() const { return replayLog_; }
    // The (tick, hash) pairs this client reported while playing. Recorded alongside
    // the bundles so a replay can be checked against the game AS PLAYED: two
    // identical reruns only prove the reruns agree with each other.
    const std::vector<ReplayCheck>& hashLog() const { return hashLog_; }
    uint64_t dataHash() const { return dataHash_; }
    void startRecording() { recording_ = true; replayLog_.clear(); hashLog_.clear(); }
    // Called when a GameStarting is about to restart the recording, BEFORE the log is
    // cleared: the room and seed are already the new game's, the log still the old
    // one's. A writer streaming the old game finishes it here.
    void setRecordingRestartHook(std::function<void()> hook) { recordingRestart_ = std::move(hook); }
    void stopRecording() { recording_ = false; replayLog_.clear(); hashLog_.clear(); }
    bool recording() const { return recording_; }
    // Slot table as it was when the game STARTED. The live room_.slots mutates as
    // players forfeit or drop, and a replay has to rebuild the world the game began
    // with, so the header needs this rather than the current table.
    const SlotInfo* startSlots() const { return startSlots_; }
    // Has this slot reported its map/data loaded? Broadcast by the server as each
    // peer reports in, so the loading screen can show who the game is waiting on.
    bool slotLoaded(int slot) const {
        return slot >= 0 && slot < kMaxSlots && slotLoaded_[size_t(slot)];
    }
    uint32_t gameId() const { return room_.id; }
    uint64_t resumeToken() const { return resumeToken_; }
    // `dataHash` = this client's gameplay-data fingerprint with the ROOM's game
    // mods mounted; the server rejects it if it doesn't match the room's expected
    // data, so a game-mod mismatch is caught
    // before the game starts, not as a mid-game desync.
    void reportLoaded(uint64_t dataHash = 0);
    // ---- the disconnect wait --------------------------------------------------
    // The seated players the server is waiting for, each with the local time its seat
    // goes to an AI (Msg::Waiting). Empty while the game runs.
    struct Wait { uint8_t slot = 0; uint64_t untilMs = 0; };   // untilMs on clockMs()
    const std::vector<Wait>& waiting() const { return waiting_; }
    static uint64_t clockMs();   // the monotonic clock the client paces by
    // How long the server will still wait for our seat if the link goes now: our
    // running countdown, else what is left of our seat's budget for the game.
    uint64_t reconnectWindowMs() const;
    // This seated player's link dropped mid-game (not a Reject, a Bye, a desync or a
    // deliberate disconnect()), so reconnect() may get the seat back inside the wait.
    bool canReconnect() const;
    // A new connection to the same server for the same game: Hello, the login again
    // (with the keys kept from the first one -- the password is long gone), then
    // Rejoin from the first tick whose bundle has not arrived. The room, recording and
    // received bundles stay. False if it could not even connect; poll() as usual, and
    // state() is InGame again once the server reseats us (takeReseated). A refusal
    // (the wait ran out and an AI has the seat) leaves state() Done, error() saying
    // why, and canReconnect() false.
    bool reconnect();
    // One-shot: the server reseated us after a reconnect. `from` is the first tick it
    // sends, `end` where the log stood (the history the view catches up through).
    bool takeReseated(uint32_t& from, uint32_t& end) {
        if (!reseated_) return false;
        reseated_ = false; from = reseatFrom_; end = reseatEnd_; return true;
    }
    // A deliberate exit from a running game: tell the server it is a Leave (no wait,
    // the units go inert) before the connection closes.
    void leaveRunningGame();
    // Close the socket with no Bye, as a failed link would (the debug harness's
    // TAK_DROP_LINK_AT_TICK). The next poll() reports the connection lost.
    void dropLink() { conn_.closeNow(); }
    // Watch a running game (no slot): connect() first, then spectate(). The server
    // replies with GameStarting (slot 0xFF -> mySlot -1) and the bundle log, then
    // streams live bundles. A spectator sends no commands or hashes.
    void spectate(uint32_t gameId, const std::string& password);
    // A watch the server has taken whose game cannot open here yet: its GameStarting waits
    // for the packages it names (the map, the game mods), which mapStatus() and modStatus()
    // follow. The server has this client in the game meanwhile; leaveGame() takes it back.
    bool watchLoading() const { return state_ == State::Lobby && pendingMapStart_.has_value(); }
    bool isSpectator() const { return spectator_; }
    // True when the last GameStarting was a spectator's (the world must be rebuilt and
    // the bundle log replayed). Cleared once consumed.
    bool isCatchUp() const { return catchUp_; }
    void clearCatchUp() { catchUp_ = false; }
    bool paused() const { return paused_; }

    // In game: is bundle `tick` available? If so, consume it (once).
    bool haveBundle(uint32_t tick) const { return bundles_.count(tick) != 0; }
    bool takeBundle(uint32_t tick, Bundle& out);
    size_t bufferedBundles() const { return bundles_.size(); }   // backlog depth
    void sendCommands(const std::vector<Command>& cmds);
    // An explicit selection order (Cmd::Order) as its uplink transaction.
    void sendOrder(const SelectionOrder& o);
    // Input the server dropped (Msg::InputDropped): commands of a PlayerCommands
    // message, or one order transaction (serial, 0 = none). Taken by the view,
    // which releases them from its send windows.
    struct InputDrop { uint32_t commands = 0, serial = 0; };
    std::vector<InputDrop> takeInputDropped() { return std::exchange(inputDropped_, {}); }
    // posDigest rides only into the recorded replay trail, never onto the wire.
    void sendHash(uint32_t tick, uint64_t hash, uint64_t posDigest = 0);
    // Flow-control progress (WE S3): the last tick processed and the smoothed RTT.
    void sendProgress(uint32_t tick);

    // Set once the server flags us desynced (fatal: the client cannot recover the state).
    bool desynced() const { return desynced_; }
    std::string desyncReason() const { return desyncReason_; }

private:
    void onFrame(const Frame& f);
    // Is this frame this client's to act on? A room's messages count only while the server
    // has this client in that room: from its own JoinResult(ok) (CreateGame, JoinGame),
    // GameStarting (Spectate) or Reseated (Rejoin) until it leaves. Anything else of a room
    // was sent before the server took this client's leave, or is not this client's at all,
    // and is dropped unread: it changes no state, room, transfer or pending request.
    bool takes(Msg kind) const;
    bool inRoom() const { return state_ == State::InRoom || state_ == State::Starting || state_ == State::InGame; }
    // leaveGame's mark in the stream. The server answers a connection's messages in order,
    // and LeaveGame has no answer of its own, so a Ping goes out right behind it: everything
    // of a room that arrives before that Ping's Pong was sent before the server took the
    // leave (the left room's, or the answer to a request taken back), and what follows the
    // Pong answers what was sent after it. Counts the Pongs still to come before that one.
    size_t leavePongs_ = 0;
    void sendAuthBegin();            // Hello answered with AuthRequired -> start SCRAM
    void sendAuthToken();            // ... or spend the sign-in token instead
    void onAuthChallenge(Reader& r);
    void onAuthResult(Reader& r);
    void startDerive();              // spin the PBKDF2 off this thread
    void pumpDerive();               // ... and send the proof once it lands
    void failAuth(const std::string& why);
    void send(Msg kind, const Writer& w) { conn_.send(kind, w); }
    void send(Msg kind) { conn_.send(kind); }

    void modFrame(const Frame& f);
    void offerMods();
    void acceptMods(std::shared_ptr<mods::Package> package,uint32_t room);
    void resetModTransfer();   // drop all mod package/transfer state
    void resetMapTransfer();        // ... and the map package/transfer state
    std::vector<std::string> gameModPacks_, alwaysGameModPacks_;
    bool gameModsAllowed_ = true;
    void rescanAlwaysGameMods();
    std::shared_ptr<mods::Package> modPackage_;
    maps::Receiver modReceive_;
    maps::Sender modSend_;
    bool roomRequestPending_=false;
    uint32_t modOfferedRoom_=0,modReadyRoom_=0;
    uint64_t modPendingAt_=0;
    std::string modStatus_;
    void mapFrame(const Frame& f);
    void acceptMap(std::shared_ptr<maps::Package> package, uint32_t room);
    // The host offers the room's file map to the server (building it first when needed),
    // when it has not offered this room's map yet (mapOfferedRoom_ / mapOfferedFor_).
    void offerRoomMap();
    bool hostingLobby() const { return room_.hostId == myId_ && (state_ == State::Lobby || state_ == State::InRoom); }
    std::filesystem::path mapRoot_,cacheRoot_;
    std::shared_ptr<maps::Package> mapPackage_;
    std::string mapPackageFor_;          // the map id mapPackage_ was built or received for
    maps::Receiver mapReceive_;
    maps::Sender mapSend_;
    uint32_t mapReadyRoom_ = 0, mapOfferedRoom_ = 0;
    std::string mapOfferedFor_;          // the map id this host last offered, beside mapOfferedRoom_
    std::string mapStatus_;
    // Room editing (see setRoomMap). confirmed*: the last LobbyState's room and map.
    // pendingPackage_: setRoomMap's candidate, promoted when the server confirms it.
    uint32_t confirmedRoomId_ = 0;
    std::string confirmedMapId_;
    std::shared_ptr<maps::Package> pendingPackage_;
    std::string pendingPackageFor_;
    uint32_t mapSerialSent_ = 0, mapSerialLast_ = 0;   // the outstanding request; the last one sent
    uint64_t mapSentAt_ = 0;
    RoomMapAnswer mapAnswer_;
    uint32_t xferSeq_ = 0;               // this connection's transfer ids (MapRequest/ModRequest)
    // A transient refusal of the host's offer (map validation capacity, the map memory
    // budget): it offers again after 2, 4 and 8 s.
    uint64_t mapRetryAt_ = 0;
    int mapRetries_ = 0;
    std::string roomError_;
    RoomRequest roomErrorFor_ = RoomRequest::Map;
    uint64_t lobbySeq_ = 0;
    bool hostMapMissing_ = false;
    void dropRoomMapRequest() { mapSerialSent_ = 0; pendingPackage_.reset(); pendingPackageFor_.clear(); }
    bool startRequested_ = false;
    std::optional<Frame> pendingMapStart_;
    Conn conn_;
    State state_ = State::Offline;
    std::string err_;
    std::string name_;
    uint32_t myId_ = 0;

    std::vector<GameInfo> games_;
    RoomView room_;
    std::string createError_;   // see createError()
    std::string joinError_;   // the last JoinResult refusal (see joinError())
    std::string startError_;  // the last START refusal (see startError())
    bool startSent_ = false;  // a StartGame went out and its answer is still to come
    uint8_t gameSpeed_ = 10;   // live game speed (from opts + in-game SpeedUpdate)
    int missionOutcome_ = 0;   // campaign mission result (0 running / +1 win / -1 loss)
    bool gameOver_ = false;    // Msg::GameOver arrived (see gameOver())
    uint32_t gameOverTicks_ = 0;
    int gameOverTeam_ = -1;
    std::vector<std::pair<std::string, std::string>> chat_;
    std::vector<VoiceFrame> voice_;
    uint64_t voiceDropped_ = 0;
    bool voiceListen_ = false;       // what the host wants
    int voiceListenSent_ = 0;        // what this connection last told the server (0 = its default, off)
    // The uplink guard (sendVoice). voiceTxAt_: the last captured audio frame (transmitting
    // while recent). voiceBaseRtt_: the lowest probe round trip, allowed to creep up 1 ms a
    // sample so a route that got slower is followed. voiceOpen_: the last frame sent was
    // audio, so a hold owes the server an END; voiceRestart_: a hold cut a transmission, so
    // the next audio frame carries START.
    uint64_t voiceTxAt_ = 0, voiceHeldUntil_ = 0, voiceTripAt_ = 0, voiceHeldFrames_ = 0;
    uint32_t voiceBaseRtt_ = UINT32_MAX;
    unsigned voiceStrikes_ = 0;
    // voiceSlowRtt_: the last probe came back past the limit.
    bool voiceHeld_ = false, voiceOpen_ = false, voiceRestart_ = false, voiceSlowRtt_ = false;
    uint16_t voiceSeq_ = 0;
    uint8_t voiceMask_ = 0xff;
    uint8_t voiceSpectators_ = 0;   // kVoiceSpectators when the open transmission reaches spectators
    uint64_t voiceSlackMs() const;
    void voiceProbe(uint64_t now, int64_t rtt = -1);
    void holdVoice(uint64_t now);

    uint64_t dataHash_ = 0;      // local gameplay-data fingerprint (sent in Hello)
    uint32_t startSeed_ = 0;
    std::vector<std::vector<uint8_t>> replayLog_;   // raw TickBundle payloads (recording)
    std::vector<ReplayCheck> hashLog_;              // (tick, hash) we reported
    bool recording_ = false;
    std::function<void()> recordingRestart_;
    SlotInfo startSlots_[kMaxSlots] = {};
    uint32_t replayTicks_ = 0;   // history the server replays after a spectate
    std::array<bool, kMaxSlots> slotLoaded_{};   // per-slot "reported Loaded"
    uint64_t resumeToken_ = 0;
    bool catchUp_ = false;
    bool spectator_ = false;
    // Reconnecting (the disconnect wait). host_/port_: where connect() went.
    // nextBundleTick_: one past the newest bundle received (what Rejoin asks from).
    // lostInGame_: the link went while seated in a game; reconnectRefused_: the server
    // said no (or ended it), so there is nothing to retry. reconnecting_: a reconnect's
    // handshake is under way; rejoinSent_: its Rejoin awaits the answer.
    std::string host_;
    uint16_t port_ = 0;
    uint32_t nextBundleTick_ = 0;
    std::vector<Wait> waiting_;
    // Our own seat's wait budget from the last Msg::Waiting (reconnectWindowMs).
    uint64_t ownWaitAt_ = 0, ownWaitMs_ = kDropWaitMs;
    bool ownWaitRunning_ = false;
    bool lostInGame_ = false, reconnectRefused_ = false, reconnecting_ = false, rejoinSent_ = false;
    bool reseated_ = false;
    uint32_t reseatFrom_ = 0, reseatEnd_ = 0;
    // The login's keys, kept for a reconnect's sign-in (see onAuthResult); wiped by
    // forgetResume (disconnect, the next connect, the destructor).
    auth::Keys resumeKeys_;
    std::vector<uint8_t> resumeSalt_;
    uint32_t resumeIters_ = 0;
    bool haveResumeKeys_ = false;
    void forgetResume();
    bool expectingSpectate_ = false;
    bool paused_ = false;
    std::map<uint32_t, Bundle> bundles_;
    std::vector<InputDrop> inputDropped_;
    bool desynced_ = false;
    std::string desyncReason_;
    uint64_t lastRecvMs_ = 0, lastPingMs_ = 0;
    // When the last read that took everything the socket had began (poll): a frame that was
    // not in by then arrived after it.
    uint64_t readAt_ = 0;
    // Send time of every Ping still waiting for its Pong, oldest first (the server answers
    // in order). Probes go out only while this is empty.
    std::deque<uint64_t> pingsOut_;
    void sendPing(uint64_t now) { send(Msg::Ping); lastPingMs_ = now; pingsOut_.push_back(now); }

    // ---- account login state ------------------------------------------------
    std::string loginUser_, loginPass_;
    Auth auth_ = Auth::None;
    std::string account_;
    bool lanServer_ = false;
    struct PendingAuth {
        std::vector<uint8_t> clientNonce, serverNonce, salt;
        uint32_t iters = 0;
        bool newAccount = false;     // the server said there is no such account yet
        bool challenged = false;     // a challenge arrived; a second one is refused
        bool proofSent = false;
        bool tokenSent = false;      // this login is a sign-in token, not a SCRAM exchange
    } pend_;
    SessionToken* token_ = nullptr;  // see setSessionToken
    bool tokenSignIn_ = false;
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
    float rttMs_ = 0;                   // smoothed round-trip time
    // Bundle-arrival jitter (net/pacing.h): stamped per poll, so the client's own stalls are
    // not read as network jitter. This is what the jitter buffer must cover.
    ArrivalJitter jitter_;
    void noteBundleArrival(uint64_t now) { jitter_.noteBundle(now, gameSpeed_); }
public:
    float rttMs() const { return rttMs_; }
    float arrivalJitterMs() const { return jitter_.jitterMs; }
    void enableRttProbe() { measureRtt_ = true; }
private:
};

}  // namespace tak::net
