#include "net/client.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <system_error>

#include "net/auth.h"
#include "net/crypto.h"
#include "tnt/mapgen.h"
#include "util/devenv.h"

namespace ta::net {

namespace {
uint64_t nowMs() {   // monotonic wall-clock (pacing/keepalive only; never hashed)
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
constexpr uint64_t kPingIdleMs = 5000, kTimeoutMs = 15000;
constexpr uint64_t kSpectatorTimeoutMs = 120000;   // matches the server's spectator grace
Writer helloFrame(uint64_t dataHash, const std::string& name) {
    Writer w;
    w.u32(kNetVersion);
    w.str("");          // build id (version-gated only)
    w.u64(dataHash);    // gameplay-data fingerprint (enforced by the server)
    w.str(name);
    return w;
}
}  // namespace

bool MpClient::connect(const std::string& host, uint16_t port, const std::string& name) {
    if (derive_.joinable()) derive_.join();
    crypto::wipe(keys_.clientKey.data(), keys_.clientKey.size());
    crypto::wipe(keys_.serverKey.data(), keys_.serverKey.size());
    forgetResume();
    host_ = host; port_ = port;
    auth_ = Auth::None; account_.clear(); pend_ = PendingAuth{}; lanServer_ = false;
    deriveDone_.store(false, std::memory_order_relaxed);
    conn_ = Conn{}; err_.clear();
    resetModTransfer();resetMapTransfer();
    room_=RoomView{};roomRequestPending_=false;expectingSpectate_=false;
    xferSeq_ = 0; roomError_.clear();
    voice_.clear(); voiceListenSent_ = 0;   // a new connection starts unheard: the server default is off
    pingsOut_.clear(); leavePongs_ = 0; voiceBaseRtt_ = UINT32_MAX;
    voiceHeld_ = voiceOpen_ = voiceRestart_ = voiceSlowRtt_ = false;
    name_ = name;
    if (!conn_.connect(host, port)) { err_ = conn_.error(); state_ = State::Done; return false; }
    conn_.allowServerFrames(false,true);   // the server's tick bundles may pass kMaxFrame
    state_ = State::Connecting;
    send(Msg::Hello, helloFrame(dataHash_, name_));
    lastRecvMs_ = nowMs();
    return true;
}

MpClient::~MpClient() {
    if (derive_.joinable()) derive_.join();   // never outlive the key-derivation worker
    crypto::wipe(loginPass_);
    forgetResume();
}

void MpClient::forgetResume() {
    crypto::wipe(resumeKeys_.clientKey.data(), resumeKeys_.clientKey.size());
    crypto::wipe(resumeKeys_.serverKey.data(), resumeKeys_.serverKey.size());
    crypto::wipe(resumeKeys_.storedKey.data(), resumeKeys_.storedKey.size());
    haveResumeKeys_ = false; resumeSalt_.clear(); resumeIters_ = 0;
    lostInGame_ = reconnecting_ = rejoinSent_ = reconnectRefused_ = reseated_ = false;
}

uint64_t MpClient::clockMs() { return nowMs(); }

// The server counts a seat's budget from its last contact, so what is left when the link
// goes is short by the silence since we last heard from the server (the same outage, seen
// from this end): a link that died quietly is not counted down afresh on detection.
uint64_t MpClient::reconnectWindowMs() const {
    const uint64_t now = nowMs();
    const uint64_t spent = ownWaitRunning_ ? now - ownWaitAt_ : now - std::min(lastRecvMs_, now);
    return spent < ownWaitMs_ ? ownWaitMs_ - spent : 0;
}

bool MpClient::canReconnect() const {
    return lostInGame_ && !reconnectRefused_ && !spectator_ && !desynced_ && !host_.empty() &&
           room_.id && resumeToken_ &&
           (loginUser_.empty() || haveResumeKeys_ ||
            (tokenSignIn_ && token_ && token_->heldFor(host_, port_)));
}

bool MpClient::reconnect() {
    if (!canReconnect() || state_ != State::Done) return false;
    if (derive_.joinable()) derive_.join();
    // A new connection for the SAME game: the room, the recording, the received
    // bundles and the resume token stay; only the link and the login start over.
    auth_ = Auth::None; pend_ = PendingAuth{};
    deriveDone_.store(false, std::memory_order_relaxed);
    conn_ = Conn{}; err_.clear();
    voice_.clear(); voiceListenSent_ = 0;
    pingsOut_.clear(); leavePongs_ = 0; voiceBaseRtt_ = UINT32_MAX;
    voiceHeld_ = voiceOpen_ = voiceRestart_ = voiceSlowRtt_ = false;
    // The server re-tells a reseated client the room as it is now: the wait, the host's
    // pause, and (LobbyState) the speed and the host.
    waiting_.clear(); paused_ = false;
    // A short connect timeout: this runs on the render thread, and is retried.
    if (!conn_.connect(host_, port_, 1500)) { err_ = conn_.error(); state_ = State::Done; return false; }
    conn_.allowServerFrames(false,true);
    state_ = State::Connecting;
    reconnecting_ = true; rejoinSent_ = false;
    send(Msg::Hello, helloFrame(dataHash_, name_));
    lastRecvMs_ = nowMs();
    return true;
}

void MpClient::setLogin(const std::string& user, const std::string& password) {
    loginUser_ = user;
    loginPass_ = password;
}

void MpClient::setSessionToken(SessionToken* token, bool signIn) {
    token_ = token;
    tokenSignIn_ = signIn;
}

void MpClient::revokeSessionToken(SessionToken& token, uint64_t dataHash) {
    const uint64_t giveUp = nowMs() + 3000;
    const auto revoke = [&](const SessionToken& t) {
        // Past its 12 hours the server no longer knows it, and would count a failed login.
        if (t.secret.empty() || nowMs() >= giveUp || nowMs() - t.earnedMs >= auth::kTokenLifetimeMs)
            return;
        Conn conn;
        if (!conn.connect(t.host, t.port, 1000) || !conn.encrypted()) return;
        conn.send(Msg::Hello, helloFrame(dataHash, t.account));
        Writer w;
        w.str(t.account);
        w.bytes(t.secret);
        conn.send(Msg::AuthRevoke, w);
        crypto::wipe(w.b.data(), w.b.size());
        // Wait for the answer: closing with its bytes unread could reset the connection
        // ahead of the request.
        const uint64_t deadline = std::min(nowMs() + 1000, giveUp);
        for (bool answered = false; !answered && nowMs() < deadline;) {
            if (!conn.flushWrite() || !conn.recv()) break;
            Frame f;
            while (conn.poll(f)) answered |= f.kind == Msg::AuthResult || f.kind == Msg::Reject;
            if (conn.peerClosed()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    revoke(token);
    for (const SessionToken& old : token.retired) revoke(old);
    token.wipe();
    token.retired.clear();
}

void MpClient::disconnect(const std::string& reason) {
    if (conn_.ok()) { Writer w; w.str(reason); send(Msg::Bye, w); conn_.flushWrite(); }
    conn_.closeNow();
    state_ = State::Done;
    forgetResume();   // a deliberate close is never reconnected
}

void MpClient::leaveRunningGame() {
    if (state_ != State::InGame || !conn_.ok()) return;
    send(Msg::LeaveGame);
    conn_.flushWrite();
}

bool MpClient::poll() {
    if (state_ == State::Offline || state_ == State::Done) return false;
    if (jitterMs_ < 0) {   // one-time init of the test link model
        const char* j = ta::devEnv("TA_NET_JITTER_MS");
        const char* b = ta::devEnv("TA_NET_BASE_MS");
        const char* l = ta::devEnv("TA_NET_LOSS_PCT");
        jitterMs_ = j ? std::max(0, std::atoi(j)) : 0;
        baseMs_ = b ? std::max(0, std::atoi(b)) : 0;
        lossPct_ = l ? std::clamp(std::atoi(l), 0, 100) : 0;
    }
    // recv() no longer fails on a clean close, so the frames that arrived alongside
    // the FIN get drained BEFORE the connection is finished off. That matters here as
    // well as on the server: a Reject sent immediately before the close used to be
    // buffered and then thrown away, leaving only a bare "peer closed" to explain a
    // rejection the server had actually spelled out.
    const uint64_t readAt = nowMs();
    jitter_.beginPoll(readAt);   // a bundle read now came after the previous poll (see ArrivalJitter)
    // A seated player whose link goes now may reconnect (canReconnect) -- unless the
    // server itself ended it (Reject, Bye), which the handlers below record.
    const bool inGame = state_ == State::InGame && !spectator_;
    const bool recvOk = conn_.recv();
    Frame f;
    if (recvOk)
        while (conn_.poll(f)) { onFrame(f); if (!conn_.ok()) break; }
    if (recvOk && conn_.recvDrained()) readAt_ = readAt;   // after the frames: a Pong needs the read before
    if (!recvOk || conn_.peerClosed()) {
        // Keep an existing reason (e.g. the server's Reject text): the socket
        // closing right after a Reject must not overwrite WHY with "peer closed".
        if (err_.empty()) {
            err_ = conn_.error().empty() ? std::string("peer closed") : conn_.error();
            // A close during the handshake (never Welcomed) is almost always a
            // version gate whose Reject reason did not arrive -- say so instead
            // of a bare "peer closed".
            if (state_ == State::Connecting)
                err_ = "server closed during handshake -- likely a protocol version "
                       "mismatch (this client speaks v" + std::to_string(kNetVersion) + ")";
        }
        state_ = State::Done;
        if (inGame) lostInGame_ = true;
        return false;
    }
    if(modPendingAt_ && nowMs()>=modPendingAt_ && !modBusy() && state_==State::InRoom && room_.opts.gameMods)
        offerMods();
    modSend_.pump(conn_,Msg::ModChunk);
    mapSend_.pump(conn_);
    if (mapSerialSent_ && nowMs() - mapSentAt_ >= 5000) {   // the server never answered (it ignored it)
        mapAnswer_ = {mapSerialSent_, RoomMapAnswer::TimedOut, {}};
        dropRoomMapRequest();
    }
    if (mapRetryAt_ && nowMs() >= mapRetryAt_) { mapRetryAt_ = 0; mapOfferedFor_.clear(); }
    if (state_ == State::InRoom && confirmedRoomId_ == room_.id) offerRoomMap();
    if (startRequested_ && !modPendingAt_ && state_ == State::InRoom && room_.mapsReady &&
        (!room_.opts.gameMods || (modPackage_ && modReadyRoom_==room_.id))) {
        send(Msg::StartGame); startRequested_ = false; startSent_ = true;
    }
    pumpDerive();          // the login's PBKDF2 finished on its worker: send the proof
    if (state_ != State::Connecting && int(voiceListen_) != voiceListenSent_) {
        Writer w; w.u8(voiceListen_ ? 1 : 0); send(Msg::VoiceListen, w);
        voiceListenSent_ = int(voiceListen_);
    }
    uint64_t now = nowMs();
    // Release any jitter-held bundles whose delay has elapsed.
    if (!jitterHeld_.empty()) {
        for (auto it = jitterHeld_.begin(); it != jitterHeld_.end();) {
            if (it->releaseMs <= now) { bundles_[it->tick] = std::move(it->bd);
                                        noteBundleArrival(now); it = jitterHeld_.erase(it); }
            else ++it;
        }
    }
    // Active RTT probes, one ping at a time: one a second when a consumer enabled them, and
    // the voice uplink guard's (see voiceProbe).
    if (measureRtt_ && pingsOut_.empty() && now - lastPingMs_ > 1000) sendPing(now);
    voiceProbe(now);
    if (now - lastRecvMs_ > kPingIdleMs && now - lastPingMs_ > kPingIdleMs) sendPing(now);
    // A spectator's render loop can freeze for several seconds on a heavy frame
    // (big supersampled resolution under VRAM pressure) without pumping the socket;
    // give it the same long grace the server extends to spectators so a transient
    // stall doesn't self-terminate a display-only connection.
    uint64_t timeoutMs = spectator_ ? kSpectatorTimeoutMs : kTimeoutMs;
    if (now - lastRecvMs_ > timeoutMs) { if (err_.empty()) err_ = "server timeout"; state_ = State::Done; }
    if (!conn_.flushWrite()) { if (err_.empty()) err_ = conn_.error(); state_ = State::Done; }
    if (!conn_.ok() && err_.empty()) { err_ = conn_.error(); state_ = State::Done; }
    if (state_ == State::Done && inGame) lostInGame_ = true;
    return state_ != State::Done;
}

// ---- account login ---------------------------------------------------------
//
// The exchange, once the server answers our Hello with AuthRequired:
//
//   C->S  AuthBegin      username, client nonce
//   S->C  AuthChallenge  does that account exist?, salt, iterations, server nonce
//   C->S  AuthProof      proof of the password       (existing account)
//     or  AuthRegister   the verifiers to store      (new account)
//   S->C  AuthResult     outcome + the server's own signature
//   S->C  Welcome        ... and we are in the lobby
//
// or, holding a sign-in token, after AuthRequired:
//
//   C->S  AuthToken      username, token
//   S->C  AuthResult     outcome + the next token
//   S->C  Welcome
//
// See src/net/auth.h for what each value is and why the password itself never
// appears anywhere in it.

void MpClient::failAuth(const std::string& why) {
    auth_ = Auth::Failed;
    if (reconnecting_) reconnectRefused_ = true;   // the same login will not do better next time
    if (err_.empty()) err_ = why;
    if (pend_.tokenSent && token_) token_->wipe();  // spent or refused: either way it is finished
    // Settle the key-derivation worker before touching anything it writes. This
    // can cost the tail of one PBKDF2 run, which is a fine price on a path that
    // has already decided to hang up -- and the alternative is a live data race
    // between the worker filling keys_ and us abandoning it.
    if (derive_.joinable()) derive_.join();
    crypto::wipe(loginPass_);
    crypto::wipe(keys_.clientKey.data(), keys_.clientKey.size());
    crypto::wipe(keys_.serverKey.data(), keys_.serverKey.size());
    // Hang up rather than sit on a connection we can never use. The error is
    // already recorded, so the socket closing cannot overwrite why.
    conn_.closeNow();
    state_ = State::Done;
}

void MpClient::sendAuthBegin() {
    if (loginUser_.empty()) {
        failAuth("this server requires an account -- sign in from the multiplayer menu");
        return;
    }
    std::string why;
    if (!auth::validUsername(loginUser_, &why)) { failAuth(why); return; }
    if (tokenSignIn_) { sendAuthToken(); return; }
    try {
        pend_ = PendingAuth{};
        pend_.clientNonce = crypto::randomVec(auth::kNonceLen);
    } catch (const std::exception& e) {
        failAuth(std::string("cannot sign in: ") + e.what());
        return;
    }
    auth_ = Auth::Pending;
    Writer w;
    w.str(loginUser_);
    w.bytes(pend_.clientNonce);
    send(Msg::AuthBegin, w);
}

void MpClient::sendAuthToken() {
    // Unlike a SCRAM proof the token itself crosses the wire: never in the clear, and
    // only to the server it came from, for the account it came from.
    if (!conn_.encrypted() || !token_ || !token_->heldFor(host_, port_) ||
        token_->account != loginUser_) {
        failAuth("sign in with your password");
        return;
    }
    pend_ = PendingAuth{};
    pend_.tokenSent = pend_.proofSent = true;     // proofSent: no key derivation to wait for
    auth_ = Auth::Pending;
    Writer w;
    w.str(loginUser_);
    w.bytes(token_->secret);
    send(Msg::AuthToken, w);
    crypto::wipe(w.b.data(), w.b.size());
}

void MpClient::onAuthChallenge(Reader& r) {
    if (auth_ != Auth::Pending || pend_.tokenSent) { failAuth("unexpected login challenge"); return; }
    // Exactly one challenge per login. Without this a server (or anything posing
    // as one) can send challenges in a loop: each spawns a fresh 600k-iteration
    // derivation, and startDerive joins the previous worker ON THE CALLER'S
    // THREAD -- so a handful of 60-byte frames freeze the client for seconds.
    if (pend_.challenged) { failAuth("the server sent a second login challenge"); return; }
    pend_.newAccount = r.u8() != 0;
    pend_.salt = r.bytes();
    pend_.iters = r.u32();
    pend_.serverNonce = r.bytes(auth::kNonceLen);
    if (!r.ok || pend_.salt.empty()) { failAuth("malformed login challenge"); return; }
    // A hostile server could ask for an absurd work factor to hang the client, or
    // a trivial one to weaken the derivation. Neither is worth entertaining.
    if (pend_.iters < 1000 || pend_.iters > 5000000) {
        failAuth("the server asked for an unreasonable password work factor");
        return;
    }
    if (reconnecting_) {
        // Signing in again for a reconnect: the password is long gone, but the keys it
        // gave are kept for exactly this. Only for the same account, salt and work
        // factor -- anything else is not the login we made.
        if (!haveResumeKeys_ || pend_.newAccount || pend_.salt != resumeSalt_ || pend_.iters != resumeIters_) {
            failAuth("cannot sign in again to reconnect");
            return;
        }
        pend_.challenged = true;
        keys_ = resumeKeys_;
        deriveDone_.store(true, std::memory_order_release);   // pumpDerive sends the proof
        return;
    }
    if (pend_.newAccount) {
        // Creating the account: hold the password to our own rules, not the
        // server's, so a weak one is refused before it is ever committed to.
        std::string why;
        if (!auth::validPassword(loginPass_, &why)) { failAuth(why); return; }
    }
    pend_.challenged = true;
    startDerive();
}

void MpClient::startDerive() {
    if (derive_.joinable()) derive_.join();
    deriveDone_.store(false, std::memory_order_relaxed);
    std::string pass = loginPass_;                 // the worker owns its own copy
    std::vector<uint8_t> salt = pend_.salt;
    uint32_t iters = pend_.iters;
    derive_ = std::thread([this, pass, salt, iters]() mutable {
        keys_ = auth::deriveKeys(pass, salt.data(), salt.size(), iters);
        crypto::wipe(pass);                        // the lambda's copy
        deriveDone_.store(true, std::memory_order_release);   // publishes keys_
    });
    crypto::wipe(pass);   // ...and the one we copied FROM, which the lambda cloned
}

void MpClient::pumpDerive() {
    if (auth_ != Auth::Pending || pend_.proofSent) return;
    if (!deriveDone_.load(std::memory_order_acquire)) return;
    if (derive_.joinable()) derive_.join();
    pend_.proofSent = true;
    crypto::wipe(loginPass_);                      // done with it, for good

    if (pend_.newAccount) {
        // Registration. The server gets the two verifiers and nothing else -- it
        // could not reconstruct the password from them if it wanted to.
        auth::Credential c = auth::makeCredential(keys_, pend_.salt.data(),
                                                  pend_.salt.size(), pend_.iters);
        Writer w;
        w.bytes(c.storedKey.data(), c.storedKey.size());
        w.bytes(c.serverKey.data(), c.serverKey.size());
        send(Msg::AuthRegister, w);
    } else {
        std::vector<uint8_t> am = auth::authMessage(loginUser_, pend_.clientNonce,
                                                    pend_.serverNonce, pend_.salt, pend_.iters);
        crypto::Digest proof = auth::clientProof(keys_, am);
        Writer w;
        w.bytes(proof.data(), proof.size());
        send(Msg::AuthProof, w);
    }
}

void MpClient::onAuthResult(Reader& r) {
    auto status = AuthStatus(r.u8());
    std::vector<uint8_t> sig = r.bytes();
    std::string msg = r.str();
    std::vector<uint8_t> token;                    // optional: the sign-in token this earned
    if (r.ok && r.p != r.end) token = r.bytes(auth::kTokenLen);
    if (!r.ok) { failAuth("malformed login result"); return; }

    if (status != AuthStatus::Ok && status != AuthStatus::Created) {
        // The failure statuses are the only ones a server may legitimately send
        // before a challenge (Throttled / BadUsername / ServerError all come
        // straight out of the AuthBegin handler), so they are answered here,
        // ahead of the state guard below.
        if (status == AuthStatus::Throttled && pend_.tokenSent && token_)
            token_->retire();                      // never looked at: still good there
        failAuth(msg.empty() ? "the server refused the login" : msg);
        return;
    }
    // Only entertain a SUCCESS for an exchange we actually completed, and only
    // the KIND of success we asked for. Without this, a machine posing as the
    // server answers our AuthBegin with AuthResult{Ok} and never sends a
    // challenge at all -- leaving pend_ empty and keys_ still zero-filled, so the
    // signature it has to forge is HMAC over an all-zero key, which anyone can
    // compute. Requiring proofSent also means the derivation thread was joined,
    // so reading keys_ here is safe; it is otherwise a live data race against the
    // worker when a challenge and a result arrive in the same TCP segment.
    if (auth_ != Auth::Pending || !pend_.proofSent ||
        pend_.newAccount != (status == AuthStatus::Created)) {
        failAuth("the server answered a login we never made -- "
                 "do not trust this connection");
        return;
    }
    // Mutual authentication, on BOTH paths. Only something holding this account's
    // ServerKey can produce this signature. Registration is included because the
    // server signs with the key we just gave it -- an unsigned "account created"
    // would otherwise be a free way to skip this check entirely. A token sign-in has
    // no such exchange: there the verified TLS certificate of the one address the
    // token goes to has already shown whose server this is.
    if (!pend_.tokenSent) {
        std::vector<uint8_t> am = auth::authMessage(loginUser_, pend_.clientNonce,
                                                    pend_.serverNonce, pend_.salt, pend_.iters);
        crypto::Digest want = auth::serverSignature(keys_.serverKey, am);
        if (sig.size() != want.size() || !crypto::equalCT(sig.data(), want.data(), want.size())) {
            failAuth("the server failed to prove it knows this account -- "
                     "do not trust this connection");
            return;
        }
    }
    account_ = loginUser_;
    auth_ = status == AuthStatus::Created ? Auth::Created : Auth::Ok;
    // Kept (until disconnect() or the next connect()) so a game whose link drops can
    // sign in again and reconnect inside the disconnect wait without the password.
    // (A token sign-in has no keys: its reconnect spends the next token instead.)
    if (!pend_.tokenSent) {
        resumeKeys_ = keys_; resumeSalt_ = pend_.salt; resumeIters_ = pend_.iters; haveResumeKeys_ = true;
    }
    crypto::wipe(keys_.clientKey.data(), keys_.clientKey.size());
    crypto::wipe(keys_.serverKey.data(), keys_.serverKey.size());
    if (token_) {
        uint64_t earned = nowMs();
        if (pend_.tokenSent) {                     // spent; the next one ends when it would have
            earned = token_->earnedMs;
            token_->wipe();
        }
        if (!token.empty() && conn_.encrypted()) {
            token_->retire();                      // a password login leaves the last one good
            token_->host = host_; token_->port = port_; token_->account = loginUser_;
            token_->secret = token;
            token_->earnedMs = earned;
        }
    }
    crypto::wipe(token.data(), token.size());
    // The server follows this with a Welcome, which moves us into the lobby.
}

static void readSlots(Reader& r, RoomView& v) { get(r, static_cast<RoomState&>(v)); }

// The server sends a room's messages only to the clients it has in that room, and it puts
// a client there with a message of that client's own before any other:
//   CreateGame, JoinGame  JoinResult(ok), then the room's first LobbyState
//   Spectate              (MapOffer, ModOffer,) GameStarting, (Waiting,) then the bundle log
//   Rejoin                Reseated, then LobbyState, (Pause,) Waiting, the missed bundles
//   MoveSeat, a seat the map's capacity moved: JoinResult(ok) then LobbyState, in the room
// (a host succession is a LobbyState to the room; no room outlives its game). So nothing
// of a room is this client's before one of those, nor once it has left: a table the server
// sent before it took the LeaveGame used to put the client back in the room it had left.
bool MpClient::takes(Msg kind) const {
    switch (kind) {
        // The connection's own, whatever room this client is or is not in.
        case Msg::Welcome: case Msg::AuthRequired: case Msg::AuthChallenge: case Msg::AuthResult:
        case Msg::Reject: case Msg::Ping: case Msg::Pong: case Msg::Bye: case Msg::GameList:
            return true;
        default: break;
    }
    // Sent before the server took this client's leave (see leavePongs_): the left room's,
    // or the answer to a request taken back.
    if (leavePongs_) return false;
    switch (kind) {
        // The answer to a CreateGame, JoinGame, Spectate (a refusal) or Rejoin (a refusal)
        // that is waiting for one; in a room, this client's new seat.
        case Msg::JoinResult:
            return state_ == State::InRoom || (state_ == State::Lobby && (roomRequestPending_ || rejoinSent_));
        // This room's game starts; or the answer to a Spectate.
        case Msg::GameStarting:
            return state_ == State::InRoom || (state_ == State::Lobby && expectingSpectate_);
        case Msg::Reseated: return rejoinSent_;
        // The room's table (in its game too: the speed, the host). Never the way into one.
        case Msg::LobbyState: return inRoom();
        case Msg::RoomError: return state_ == State::InRoom;
        // A room's transfers, chat and voice, and its game's stream. A watched game's
        // packages are offered before its GameStarting, which may wait for them
        // (pendingMapStart_) while the rest arrives.
        default: return inRoom() || expectingSpectate_;
    }
}

void MpClient::onFrame(const Frame& f) {
    lastRecvMs_ = nowMs();
    if (!takes(f.kind)) return;
    Reader r(f.payload.data(), f.payload.size());
    switch (f.kind) {
        case Msg::Welcome:
            myId_ = r.u32();
            name_ = r.str();
            lanServer_ = r.u8() == 1;   // 1 = a LAN host's server
            // A Welcome is NOT a login. If we came here with credentials, the
            // only way in is a verified AuthResult -- otherwise a machine posing
            // as the server could skip the whole exchange and answer our Hello
            // with a bare Welcome, and we would happily play on a connection that
            // proved nothing.
            // A player's LOCAL SERVER has no accounts at all, and says so: the name is
            // all it takes, and the password goes nowhere (it was never sent).
            if (!loginUser_.empty() && auth_ != Auth::Ok && auth_ != Auth::Created && lanServer_) {
                loginUser_.clear(); crypto::wipe(loginPass_);
            } else if (!loginUser_.empty() && auth_ != Auth::Ok && auth_ != Auth::Created) {
                failAuth("the server let us in without checking the account -- "
                         "do not trust this connection");
                return;
            }
            // The Welcome name is authoritative: names match case-insensitively,
            // so someone who typed "CURTIS" is signed in to "curtis" and should be
            // shown as its owner spelled it, not as they happened to type it.
            if (auth_ != Auth::None && !name_.empty()) account_ = name_;
            state_ = State::Lobby;
            if (reconnecting_) {
                // Back in: ask for the seat, catching up from the first bundle we lack.
                reconnecting_ = false; rejoinSent_ = true;
                Writer w; w.u32(room_.id); w.u64(resumeToken_); w.u32(nextBundleTick_);
                send(Msg::Rejoin, w);
                break;
            }
            break;
        case Msg::AuthRequired: sendAuthBegin(); break;
        case Msg::AuthChallenge: onAuthChallenge(r); break;
        case Msg::AuthResult: onAuthResult(r); break;
        case Msg::Reject:
            roomRequestPending_=false;
            err_ = r.str();
            state_ = State::Done;
            reconnectRefused_ = true;   // the server ended it: not a link to restore
            break;
        case Msg::Ping: send(Msg::Pong); break;
        case Msg::Pong:
            if (!pingsOut_.empty()) {
                const uint64_t now = nowMs(), sent = pingsOut_.front(), rtt = now - sent;
                // The Pong came in somewhere after the last read that took everything the
                // socket had: rtt is the most the round trip took, and atLeast the least. They
                // differ by the time between the two reads -- a frame, which on a stuttering
                // machine can be a 250 ms one, and that is the client's own delay, not queueing.
                const uint64_t atLeast = readAt_ > sent ? readAt_ - sent : 0;
                pingsOut_.pop_front();
                if (leavePongs_) --leavePongs_;   // at 0 the server has taken the leave: see takes()
                if (measureRtt_) {
                    // Add the modelled return-path delay so RTT reflects the test link.
                    float sample = float(rtt) + float(receiveDelayMs());
                    rttMs_ = rttMs_ > 0 ? 0.7f * rttMs_ + 0.3f * sample : sample;
                }
                const uint32_t ms = uint32_t(std::min<uint64_t>(rtt, 60000));
                voiceBaseRtt_ = voiceBaseRtt_ == UINT32_MAX ? ms : std::min(voiceBaseRtt_ + 1, ms);
                voiceProbe(now, int64_t(atLeast));
            }
            break;
        case Msg::Bye: err_ = r.str(); state_ = State::Done; reconnectRefused_ = true; break;
        case Msg::GameList: {
            games_.clear();
            uint32_t n = r.u32();
            for (uint32_t i = 0; i < n && r.ok; ++i) {
                GameInfo g;
                g.id = r.u32(); g.name = r.str(); g.mapId = r.str();
                g.players = r.u8(); g.capacity = r.u8(); g.running = r.u8();
                g.passworded = r.u8(); g.uptimeSec = r.u32();
                g.allowSpectators = r.u8();
                if (!r.ok) break;
                games_.push_back(g);
            }
            break;
        }
        case Msg::JoinResult: {
            uint8_t ok = r.u8(); uint8_t slot = r.u8(); std::string why = r.str();
            // The room's revision, so a READY sent before the first LobbyState counts.
            const uint32_t rev = r.avail(4) ? r.u32() : 0;
            // A refusal answers a request that is waiting for its answer, and nothing else;
            // in the game list so does a yes (a Rejoin's yes is Reseated).
            if (!roomRequestPending_ && (ok ? state_ == State::Lobby : !rejoinSent_)) break;
            if(!ok) {
                roomRequestPending_=false;
                // Keep the reason for the browser, and drop the pending kind: left
                // set, a refused watch made the NEXT ordinary join's GameStarting
                // read as a spectator start.
                joinError_ = why.empty() ? std::string("the server refused the request") : why;
                expectingSpectate_ = false;
                if (rejoinSent_) {   // the seat is gone (the wait ran out) or never was
                    rejoinSent_ = false; reconnectRefused_ = true;
                    err_ = joinError_; state_ = State::Done;
                    break;
                }
            }
            if (ok) {
                joinError_.clear();
                // A seat move (Msg::MoveSeat) in this room: mirror the server's swap
                // until its LobbyState lands, so nothing drawn or sent in between reads
                // the open seat's old values as this player's.
                const int was = room_.mySlot;
                if (state_ == State::InRoom && was >= 0 && was < kMaxSlots && slot < kMaxSlots && int(slot) != was) {
                    std::swap(room_.slots[was], room_.slots[slot]);
                    room_.slots[was].type = 0; room_.slots[was].name.clear(); room_.slots[was].ready = 0;
                }
                // WATCH: the host's seat opens, as the server opened it.
                if (state_ == State::InRoom && was >= 0 && was < kMaxSlots && slot == kWatchSeat) {
                    room_.slots[was].type = 0; room_.slots[was].name.clear(); room_.slots[was].ready = 0;
                }
                room_.roomRev = rev;
                // 0xFF = watching: the host created the game as a slot-less spectator, or left its seat.
                room_.mySlot = (slot == 0xFF) ? -1 : int(slot);
                spectator_ = (slot == 0xFF);
                state_ = State::InRoom;
            }
            break;
        }
        case Msg::ModOffer: case Msg::ModRequest: case Msg::ModChunk: case Msg::ModError:
            modFrame(f); break;
        case Msg::MapOffer: case Msg::MapRequest: case Msg::MapChunk: case Msg::MapError:
            mapFrame(f); break;
        case Msg::LobbyState: {
            // Only in a room (takes()): the table never opens one. Its JoinResult did.
            const auto oldGameMods=room_.opts.gameMods;
            int keep = room_.mySlot;
            RoomView incoming;
            readSlots(r, incoming);
            if (!r.ok) break;
            if (room_.id && incoming.id != room_.id) break;   // not this room's
            roomRequestPending_=false;
            ++lobbySeq_;
            // The answer to this host's SetRoomMap carries its serial: a map the server took
            // becomes the room's package now (and not before, so a refused or crossed change
            // never touched it).
            if (mapSerialSent_ && incoming.mapSerial == mapSerialSent_) {
                mapAnswer_ = {mapSerialSent_, RoomMapAnswer::Accepted, {}};
                if (pendingPackage_ && pendingPackageFor_ == incoming.mapId) {
                    mapPackage_ = pendingPackage_; mapPackageFor_ = incoming.mapId;
                }
                dropRoomMapRequest();
            }
            // The room's map changed (for host and guests alike): every transfer of the old
            // one stops, and a start waiting on it is off.
            const bool mapChanged = confirmedRoomId_ == incoming.id && incoming.mapId != confirmedMapId_;
            if (mapChanged) {
                mapSend_ = {}; mapReceive_ = {}; mapReadyRoom_ = 0;
                pendingMapStart_.reset(); startRequested_ = false;
                mapStatus_.clear(); startError_.clear(); mapOfferedFor_.clear();
                mapRetryAt_ = 0; mapRetries_ = 0; hostMapMissing_ = false;
                if (mapPackageFor_ != incoming.mapId) { mapPackage_.reset(); mapPackageFor_.clear(); }
            }
            room_ = std::move(incoming);
            room_.mySlot = keep;
            // A room's map and game mods are settled when it starts: a player made the host
            // in a running game (its host left, or reconnected under a new id) keeps the
            // packages its world was built from and offers none.
            const bool hosting = hostingLobby();
            if(oldGameMods!=room_.opts.gameMods && hosting){mapPackage_.reset();mapPackageFor_.clear();mapOfferedRoom_=mapReadyRoom_=0;mapOfferedFor_.clear();}
            if(!room_.opts.gameMods)resetModTransfer();
            else if(hosting && modOfferedRoom_!=room_.id)offerMods();
            gameSpeed_ = room_.opts.speed;
            if (!room_.mission.empty() || mapgen::isGeneratedMapId(room_.mapId)) {
                mapReadyRoom_ = room_.id; mapStatus_ = "MAP READY";
            } else offerRoomMap();
            confirmedRoomId_ = room_.id; confirmedMapId_ = room_.mapId;
            if (!hosting) dropRoomMapRequest();   // no longer the host: nothing will answer it
            break;
        }
        case Msg::RoomError: {
            const auto request = RoomRequest(r.u8());
            const uint32_t serial = r.u32();
            std::string why = r.str();
            if (!r.ok) break;
            roomError_ = why.empty() ? std::string("the server refused the change") : why;
            roomErrorFor_ = request;
            // Nothing changed at the server; the LobbyState that follows shows the room as it is.
            if (request == RoomRequest::Map && mapSerialSent_ && serial == mapSerialSent_) {
                mapAnswer_ = {serial, RoomMapAnswer::Refused, roomError_};
                dropRoomMapRequest();
            }
            break;
        }
        case Msg::SpeedUpdate: { uint8_t s = r.u8(); if (r.ok) { gameSpeed_ = s; room_.opts.speed = s; } break; }
        case Msg::MissionOutcome: { int8_t o = int8_t(r.u8()); if (r.ok) missionOutcome_ = o; break; }
        case Msg::GameOver: {
            const uint32_t ticks = r.u32(); const uint8_t team = r.u8();
            if (!r.ok) break;
            gameOver_ = true; gameOverTicks_ = ticks; gameOverTeam_ = team == 0xFF ? -1 : int(team);
            break;
        }
        case Msg::Chat: {
            std::string who = r.str(), text = r.str();
            if (r.ok) chat_.push_back({who, text});
            break;
        }
        case Msg::VoiceDown: {
            VoiceFrame v;
            v.talker = r.u32(); v.slot = r.u8(); v.seq = r.u16(); v.flags = r.u8();
            const uint16_t len = r.u16();
            if (!r.ok || len > kVoiceMaxOpus || size_t(r.end - r.p) < len) break;
            v.opus.assign(r.p, r.p + len); r.p += len;
            if (v.flags & kVoiceHasName) v.name = r.str();
            if (!r.ok || !voiceListen_) break;
            if (voice_.size() >= kVoiceInbox) { voice_.erase(voice_.begin()); ++voiceDropped_; }
            voice_.push_back(std::move(v));
            break;
        }
        case Msg::GameStarting: {
            RoomView check; Reader header(f.payload.data(), f.payload.size()); readSlots(header, check);
            if (header.ok && state_ == State::InRoom && room_.id && check.id != room_.id) break;   // not this room's
            roomRequestPending_=false;
            if (header.ok && ((check.opts.gameMods && modReadyRoom_!=check.id) || (check.mission.empty() && !mapgen::isGeneratedMapId(check.mapId) && mapReadyRoom_ != check.id))) {
                pendingMapStart_ = f; return;
            }
            int keep = room_.mySlot;
            RoomView incoming;
            readSlots(r, incoming);
            if (!r.ok) break;
            room_ = std::move(incoming);
            startSent_ = false;   // the START was answered: the game is starting
            dropRoomMapRequest(); // and the room is settled
            gameSpeed_ = room_.opts.speed;
            if (room_.mission.empty() && mapgen::isGeneratedMapId(room_.mapId)) {
                try {
                    const auto data = roomDataVfs();
                    if (const auto cache = cacheRoot(); !cache.empty()) maps::saveGenerated(cache, data, room_.mapId);
                } catch (const std::exception& e) {
                    mapStatus_ = std::string("MAP SAVE FAILED: ") + e.what();
                    chat_.push_back({"SYSTEM", mapStatus_});
                    std::fprintf(stderr, "%s\n", mapStatus_.c_str());
                }
            }
            missionOutcome_ = 0;   // fresh game/replay
            slotLoaded_.fill(false);
            slotLoadPct_.fill(0);

            // Snapshot the starting slot table for a replay header, before forfeits
            // and drops rewrite the live one.
            for (int i = 0; i < kMaxSlots; ++i) startSlots_[i] = room_.slots[i];
            uint8_t mySlot = r.u8();
            startSeed_ = r.u32();
            resumeToken_ = r.u64();
            replayTicks_ = r.u32();   // bundles logged before we joined (0 = none)
            nextBundleTick_ = 0; waiting_.clear();
            ownWaitAt_ = 0; ownWaitMs_ = kDropWaitMs; ownWaitRunning_ = false;
            // 0xFF marks a spectator (no slot); map it to -1.
            room_.mySlot = !r.ok ? keep : (mySlot == 0xFF ? -1 : int(mySlot));
            // Record a replay of every game we take part in -- playing or spectating
            // (an all-AI watch game included). Started here rather than when the view
            // sets up, because the backlog a spectate receives arrives
            // immediately after this message -- any later and the recording would be
            // missing its first ticks. A spectator's replay carries no hash
            // checkpoints (see sendHash); the view skips saving for the Benchmark.
            if (recording_ && recordingRestart_) recordingRestart_();
            startRecording();
            // Any 0xFF start is a spectator (a create-as-spectator host, or spectate()).
            spectator_ = expectingSpectate_ || (r.ok && mySlot == 0xFF);
            // Route EVERY spectator through the "set up before the state branches" path
            // (isCatchUp): a spectator never reports Loaded, so the server starts sending
            // TickBundles immediately and the client's state can flip Starting->InGame
            // before the normal starting-branch runs -- which would skip world setup and
            // leave the spectator watching an empty sim. A host-spectator whose game is
            // just starting simply has an empty backlog to "replay".
            catchUp_ = expectingSpectate_ || spectator_;
            expectingSpectate_ = false;
            state_ = State::Starting;
            break;
        }
        case Msg::PlayerStatus: {
            // slot, status (0=connected 1=dropped 2=loaded 3=loading), value. The
            // loading screen's player strip reads loaded and loading (value = percent).
            uint8_t slot = r.u8(), status = r.u8();
            const uint32_t value = r.u32();
            if (r.ok && slot < kMaxSlots && status == 2) slotLoaded_[slot] = true;
            if (r.ok && slot < kMaxSlots && status == 3) slotLoadPct_[slot] = uint8_t(std::min<uint32_t>(value, 100));
            break;
        }
        case Msg::Pause: paused_ = true; break;
        case Msg::Resume: paused_ = false; break;
        case Msg::Waiting: {
            const uint64_t now = nowMs();
            std::vector<Wait> list;
            const uint8_t n = r.u8();
            for (uint8_t i = 0; i < n && r.ok; ++i) {
                Wait w; w.slot = r.u8(); w.untilMs = now + r.u32();
                if (r.ok && w.slot < kMaxSlots) list.push_back(w);
            }
            const bool running = r.u8() != 0;
            const uint32_t own = r.u32();
            if (r.ok) {
                waiting_ = std::move(list);
                ownWaitAt_ = now; ownWaitMs_ = own; ownWaitRunning_ = running;
            }
            break;
        }
        case Msg::Reseated: {
            const uint8_t slot = r.u8(); const uint64_t token = r.u64();
            const uint32_t from = r.u32(), end = r.u32();
            if (!r.ok || !rejoinSent_ || slot >= kMaxSlots) break;
            rejoinSent_ = false; lostInGame_ = false;
            room_.mySlot = slot; resumeToken_ = token;
            reseated_ = true; reseatFrom_ = from; reseatEnd_ = end;
            state_ = State::InGame;
            break;
        }
        case Msg::TickBundle: {
            // Record the payload VERBATIM, before parsing: the server logs this exact
            // buffer, so replaying our copy reproduces the game bit-for-bit. Parsing
            // and re-serializing would risk a format drift between the two writers.
            if (recording_) replayLog_.push_back(f.payload);
            // Not strict (see getBundle): the referee already applied this bundle.
            uint32_t tk = 0;
            Bundle bd;
            if (getBundle(r, tk, bd, /*strict=*/false)) {
                nextBundleTick_ = std::max(nextBundleTick_, tk + 1);
                uint64_t delay = receiveDelayMs();
                if (delay > 0) {   // model the link: hold, then release later
                    jitterHeld_.push_back({nowMs() + delay, tk, std::move(bd)});
                } else {
                    bundles_[tk] = std::move(bd);
                    noteBundleArrival(nowMs());
                }
                if (state_ == State::Starting) state_ = State::InGame;
            }
            break;
        }
        case Msg::InputDropped: {
            InputDrop d;
            d.commands = r.u32();
            d.serial = r.u32();
            if (r.ok) inputDropped_.push_back(d);   // at most one per message this client sent
            break;
        }
        case Msg::Desynced: {
            uint32_t tk = r.u32(); std::string why = r.str();
            desynced_ = true;
            desyncReason_ = why + " (tick " + std::to_string(tk) + ")";
            break;
        }
        default: break;
    }
}

// ---- lobby actions --------------------------------------------------------

void MpClient::listGames() { send(Msg::ListGames); }

void MpClient::createGame(const std::string& name, const std::string& password,
                          const std::string& mapId, const GameOptions& o, uint8_t capacity,
                          bool spectate, bool priv, const std::string& mission) {
    if(roomRequestPending_)return;
    joinError_.clear();createError_.clear();
    gameOver_ = false;
    resetModTransfer();resetMapTransfer();
    mapStatus_ = "CHECKING MAP";
    if (mission.empty() && !mapgen::isGeneratedMapId(mapId)) {
        try {
            mapPackage_ = maps::build(hpi::mountRetailRoot(mapRoot_,
                o.gameMods?hostGameModPacks():std::vector<std::string>{}), mapId);
            mapPackageFor_ = mapId;
        } catch (const std::exception& e) {
            // The map cannot be built here: nothing was sent, and the connection is fine. Say why
            // (the lobby shows it, a headless run ends on it) without poisoning the link's error().
            createError_ = mapStatus_ = e.what(); return;
        }
    }
    // The server refuses a name that is not 1..kRoomNameMax printable-ASCII bytes. A name
    // can come from a remembered setting, a field that took any UTF-8 or a map id, so send
    // what it takes: printable ASCII only, cut to the limit (empty: the server's default).
    std::string sent;
    for (unsigned char ch : name)
        if (ch >= 0x20 && ch <= 0x7e && sent.size() < kRoomNameMax) sent += char(ch);
    Writer w; w.str(sent); w.str(password); w.str(mapId); w.str(mission);
    put(w, o);
    w.u8(capacity);   // map's start-position count (the server has no map data)
    w.u8(spectate ? 1 : 0);   // host watches, taking no slot
    w.u8(priv ? 1 : 0);       // private (single-player): not in the public game list
    roomRequestPending_=true;
    send(Msg::CreateGame, w);
}

void MpClient::joinGame(uint32_t id, const std::string& password) {
    if(roomRequestPending_)return;
    roomRequestPending_=true;
    joinError_.clear();
    gameOver_ = false;
    Writer w; w.u32(id); w.str(password);
    send(Msg::JoinGame, w);
}

void MpClient::leaveGame() {
    // A request still on its way is taken back with it: the server takes this LeaveGame
    // after it, so a room it opens there is left, and its answer is dropped here.
    roomRequestPending_=false; expectingSpectate_ = false;
    send(Msg::LeaveGame);
    sendPing(nowMs());                 // its Pong: the server has taken the leave (takes())
    leavePongs_ = pingsOut_.size();
    resetModTransfer();resetMapTransfer();
    // What the game's stream left here goes with it: the next game on this connection
    // counts its ticks from 0 again, and must find none of these waiting under them. A
    // watched game's stream is taken before its GameStarting is (which may wait for a
    // package, takes()), so a WATCH taken back in that wait leaves one too.
    bundles_.clear(); jitterHeld_.clear(); nextBundleTick_ = 0;
    waiting_.clear(); paused_ = false; gameOver_ = false;
    chat_.clear(); voice_.clear(); inputDropped_.clear();
    room_ = RoomView{};
    state_ = State::Lobby;
}

void MpClient::setSlot(int slot, uint8_t type, uint8_t faction, uint8_t color,
                       uint8_t team, uint8_t ready, uint8_t aiLevel) {
    Writer w; w.u8(uint8_t(slot)); w.u8(type); w.u8(faction); w.u8(color); w.u8(team); w.u8(ready);
    w.u8(aiLevel);
    w.u32(room_.roomRev);   // the room this edit was made on: a READY counts only for it
    send(Msg::SlotUpdate, w);
}

void MpClient::setGameOptions(const GameOptions& o) {
    Writer w; put(w, o);
    send(Msg::SetGameOptions, w);
}

bool MpClient::roomEditable() const {
    return state_ == State::InRoom && room_.id && room_.hostId == myId_ && room_.mission.empty() &&
           !room_.opts.benchmark && !startPending() && !pendingMapStart_;
}

bool MpClient::setRoomMap(const std::string& mapId, uint8_t capacity, bool mapScenario, uint8_t randomStarts) {
    const auto refuse = [&](const std::string& why) { roomError_ = why; roomErrorFor_ = RoomRequest::Map; return false; };
    if (!roomEditable()) return refuse("only the host can change the map, before the game starts");
    if (mapSerialSent_) return refuse("the last map change is still waiting for the server");
    if (mapId.empty()) return refuse("no map was given");
    roomError_.clear();
    const bool generated = mapgen::isGeneratedMapId(mapId);
    // A new file map is built here first, as createGame builds one: a map this player cannot
    // build is never asked for. It becomes the room's package when the server confirms.
    std::shared_ptr<maps::Package> candidate;
    if (!generated && mapId != room_.mapId) {
        try { candidate = maps::build(roomDataVfs(), mapId); }
        catch (const std::exception& e) { return refuse(e.what()); }
    }
    // What the server would make of it (its own SetRoomMap rules): nothing to change is
    // not worth a round trip -- but a map the server has not verified is offered again.
    const uint8_t scenario = mapScenario && !generated && !room_.opts.stressTest && !room_.opts.benchmark ? 1 : 0;
    const uint8_t starts = scenario ? kStartsFixed : normalizeStartMode(randomStarts);
    if (mapId == room_.mapId && scenario == room_.opts.mapScenario && starts == room_.opts.randomStarts) {
        if (mapReadyRoom_ != room_.id) { mapOfferedFor_.clear(); mapRetryAt_ = 0; err_.clear(); }
        return true;
    }
    pendingPackage_ = std::move(candidate);
    pendingPackageFor_ = pendingPackage_ ? mapId : std::string();
    const uint32_t serial = std::max(mapSerialLast_, room_.mapSerial) + 1;
    Writer w; w.u32(serial); w.str(mapId); w.u8(capacity); w.u8(mapScenario ? 1 : 0); w.u8(randomStarts);
    send(Msg::SetRoomMap, w);
    // Nothing else changes until the answer: no transfer state, no room_, no mapStatus_.
    mapSerialSent_ = mapSerialLast_ = serial;
    mapSentAt_ = nowMs();
    return true;
}

void MpClient::setRoomInfo(std::optional<std::string> name, std::optional<std::string> password) {
    const uint8_t flags = uint8_t((name ? kRoomInfoName : 0) | (password ? kRoomInfoPassword : 0));
    if (!flags || state_ != State::InRoom || room_.hostId != myId_) return;
    roomError_.clear();
    Writer w; w.u8(flags); w.str(name ? *name : std::string()); w.str(password ? *password : std::string());
    send(Msg::SetRoomInfo, w);
}

void MpClient::setPause(bool want) { Writer w; w.u8(want ? 1 : 0); send(Msg::SetPause, w); }

void MpClient::kick(int slot) { Writer w; w.u8(uint8_t(slot)); send(Msg::Kick, w); }

void MpClient::moveSeat(int slot) { Writer w; w.u8(uint8_t(slot)); send(Msg::MoveSeat, w); }

void MpClient::chat(const std::string& text, SlotMask recipients, bool spectators) {
    Writer w; w.str(text); w.u16(recipients); w.u8(spectators ? 1 : 0); send(Msg::Chat, w);
}

void MpClient::setVoiceListen(bool on) {
    voiceListen_ = on;
    if (!on) voice_.clear();
}

void MpClient::sendVoice(uint16_t seq, SlotMask recipients, uint8_t flags, const std::vector<uint8_t>& opus) {
    if (state_ != State::InRoom && state_ != State::Starting && state_ != State::InGame) return;
    if (opus.size() > kVoiceMaxOpus || opus.empty() != bool(flags & kVoiceEnd) ||
        (flags & (kVoiceStart | kVoiceEnd)) == (kVoiceStart | kVoiceEnd)) return;   // the server fails these
    const bool end = flags & kVoiceEnd;
    const uint64_t now = nowMs();
    if (!end) voiceTxAt_ = now;
    if (voiceHeld_) {   // the hold already closed the transmission (holdVoice)
        if (!end) ++voiceHeldFrames_;
        return;
    }
    if (end) voiceRestart_ = false;
    else if (voiceRestart_) { flags |= kVoiceStart; voiceRestart_ = false; }   // a fresh start after a hold
    Writer w;
    w.u16(seq); w.u16(recipients); w.u8(flags); w.u16(uint16_t(opus.size()));
    w.b.insert(w.b.end(), opus.begin(), opus.end());
    voiceSeq_ = seq; voiceMask_ = recipients; voiceSpectators_ = flags & kVoiceSpectators; voiceOpen_ = !end;
    conn_.sendLow(Msg::VoiceUp, w.b);
    if (!conn_.flushWrite() && err_.empty()) err_ = conn_.error();   // out now, not a frame later
}

// How far above the quiet round trip a probe may come back before this client's own voice
// counts as queueing ahead of its game traffic: 4 ticks at the current speed, at least
// 100 ms (a few ticks of the 16-tick reserve every lead keeps, flowLeadTicks).
uint64_t MpClient::voiceSlackMs() const {
    return std::max<uint64_t>(100, 40000 / (30 * uint64_t(std::max<uint8_t>(gameSpeed_, 1))));
}

// The voice uplink guard (sendVoice): probe the round trip, hold voice when it shows
// queueing, release the hold once the back-off is over and the probe is quiet. Run from
// poll(), and on each Pong with the least round trip it shows (rtt, else -1).
//
// Only evidence of queueing trips it: a frame loop that stutters must not read as a slow
// uplink. So a probe is judged by the least it took -- from its Ping to the last read that
// found no Pong (readAt_), not to the read that found it -- while the quiet round trip it
// is judged against comes from the most they took, which can only loosen the guard.
void MpClient::voiceProbe(uint64_t now, int64_t rtt) {
    const bool inRoom = state_ == State::InRoom || state_ == State::Starting || state_ == State::InGame;
    const bool talking = inRoom && voiceTxAt_ && now - voiceTxAt_ < 1000;
    if (rtt < 0 && (talking || (inRoom && voiceListen_)) && pingsOut_.empty() &&
        now - lastPingMs_ >= (talking ? 250u : 2000u))
        sendPing(now);
    if (voiceBaseRtt_ == UINT32_MAX) return;
    // Queueing shows as a probe back past the limit, or one still out past it. Acting on the
    // first is deliberate: waiting for a second costs another probe interval of voice
    // queued ahead of the game (a 2500 B/s uplink: worst Progress delay 0.4 s -> 0.8 s),
    // and a blip on the route that trips it costs only a few seconds of voice.
    const uint64_t limit = voiceBaseRtt_ + voiceSlackMs();
    if (rtt >= 0) voiceSlowRtt_ = uint64_t(rtt) > limit;
    const bool queueing = voiceSlowRtt_ || (!pingsOut_.empty() && readAt_ > pingsOut_.front() &&
                                            readAt_ - pingsOut_.front() > limit);
    if (voiceHeld_) {
        if (now >= voiceHeldUntil_ && !queueing) voiceHeld_ = false;
    } else if (talking && queueing) {
        holdVoice(now);
    }
}

void MpClient::holdVoice(uint64_t now) {
    if (now - voiceTripAt_ > 120000) voiceStrikes_ = 0;
    voiceTripAt_ = now;
    voiceHeldUntil_ = now + std::min<uint64_t>(5000ull << std::min(voiceStrikes_, 4u), 60000);
    ++voiceStrikes_;
    voiceHeld_ = true;
    conn_.dropLow();
    if (voiceOpen_) {
        // Close the transmission now, as an ordinary frame: it follows whatever voice
        // already left, and the listeners stop waiting for more.
        Writer w; w.u16(uint16_t(voiceSeq_ + 1)); w.u16(voiceMask_); w.u8(uint8_t(kVoiceEnd | voiceSpectators_)); w.u16(0);
        send(Msg::VoiceUp, w);
        voiceOpen_ = false;
        voiceRestart_ = true;
    }
}

void MpClient::startGame() { startRequested_ = true; startError_.clear(); }

// ---- game play ------------------------------------------------------------

void MpClient::reportLoaded(uint64_t dataHash) { Writer w; w.u64(dataHash); send(Msg::Loaded, w); }
void MpClient::reportLoadProgress(int percent) {
    if (spectator_ || room_.mySlot < 0) return;
    Writer w; w.u8(uint8_t(std::clamp(percent, 0, 100)));
    send(Msg::LoadProgress, w);
    conn_.flushWrite();   // the load blocks poll(): push it now (non-blocking)
}

void MpClient::spectate(uint32_t gameId, const std::string& password) {
    if(roomRequestPending_)return;
    roomRequestPending_=true;
    joinError_.clear();
    gameOver_ = false;
    expectingSpectate_ = true;
    Writer w; w.u32(gameId); w.str(password);
    send(Msg::Spectate, w);
}

bool MpClient::takeBundle(uint32_t tick, Bundle& out) {
    auto it = bundles_.find(tick);
    if (it == bundles_.end()) return false;
    out = std::move(it->second);
    bundles_.erase(it);
    return true;
}

void MpClient::sendCommands(const std::vector<Command>& cmds) {
    if (cmds.empty()) return;
    Writer w; w.u32(uint32_t(cmds.size()));
    for (const auto& c : cmds) w.cmd(c);
    send(Msg::PlayerCommands, w);
}

// One explicit selection order as its transaction (net/protocol.h):
// Begin, the members in chunks of at most kOrderChunkMembers, Commit. The frames
// go out back to back on this connection, so nothing the client sends later can
// come between them.
void MpClient::sendOrder(const SelectionOrder& o) {
    {
        Writer w; w.u32(o.serial); w.cmd(o.tmpl); w.u8(uint8_t(o.shape)); w.u32(uint32_t(o.members.size()));
        send(Msg::OrderBegin, w);
    }
    int32_t prev = -1;
    for (size_t at = 0; at < o.members.size(); at += kOrderChunkMembers) {
        const size_t n = std::min(o.members.size() - at, size_t(kOrderChunkMembers));
        Writer w; w.u32(o.serial);
        putMembers(w, o.members.data() + at, n, prev);
        send(Msg::OrderMembers, w);
    }
    Writer w; w.u32(o.serial); w.u32(uint32_t(o.members.size())); w.u64(orderDigest(o.members));
    send(Msg::OrderCommit, w);
}

void MpClient::sendHash(uint32_t tick, uint64_t hash, uint64_t posDigest) {
    // Keep our own trail while recording, so the replay carries what this client
    // actually computed at each checkpoint. A spectator's StateHash is only a
    // progress ack carrying 0 (never a computed hash), so it is not a checkpoint.
    if (recording_ && !spectator_) hashLog_.push_back({tick, hash, posDigest});
    Writer w; w.u32(tick); w.u64(hash);
    send(Msg::StateHash, w);
}

void MpClient::sendProgress(uint32_t tick) {
    Writer w; w.u32(tick);
    w.u32(progressRttMs(rttMs_));   // flowBufferLimit sizes the jitter target from the same value
    w.u32(bundles_.empty() ? tick : std::max(tick, bundles_.rbegin()->first));   // arrived, not yet run
    send(Msg::Progress, w);
}

}  // namespace ta::net

namespace ta::net {
void MpClient::acceptMap(std::shared_ptr<maps::Package> package, uint32_t room) {
    mapPackage_ = std::move(package); mapPackageFor_ = room_.mapId; mapReadyRoom_ = room; mapReceive_ = {};
    mapRetries_ = 0; mapRetryAt_ = 0;
    mapStatus_ = "MAP VERIFIED";
    try { if (const auto cache = cacheRoot(); !cache.empty()) maps::saveCache(cache, *mapPackage_); }
    catch (const std::exception& e) {
        mapStatus_ = std::string("MAP VERIFIED; SAVE FAILED: ") + e.what();
        chat_.push_back({"SYSTEM", mapStatus_}); std::fprintf(stderr, "%s\n", mapStatus_.c_str());
    }
    Writer w; w.u32(room); w.str(mapPackage_->digest); send(Msg::MapReady, w);
    if (pendingMapStart_) {
        auto start = std::move(*pendingMapStart_); pendingMapStart_.reset(); onFrame(start);
    }
}
void MpClient::mapFrame(const Frame& f) {
    try {
        Reader r(f.payload.data(), f.payload.size());
        if (f.kind == Msg::MapOffer) {
            auto id = r.u32(); auto mapId = r.str(), digest = r.str(); auto size = r.u32();
            if (!r.ok || r.p != r.end) throw std::runtime_error("invalid map offer");
            if (room_.id && room_.id != id && !expectingSpectate_) return;
            if (room_.id == id && mapId != room_.mapId) return;   // a map the room no longer plays
            if (mapReceive_.room == id && mapReceive_.digest == digest) return;
            if (mapPackage_ && mapPackage_->digest == digest) { acceptMap(mapPackage_, id); return; }
            mapReadyRoom_ = 0; mapReceive_.begin(id, 0, size, digest);
            if (auto cached = cacheRoot().empty() ? nullptr : maps::loadCache(cacheRoot(), digest)) { acceptMap(std::move(cached), id); return; }
            try {
                auto local = maps::build(roomDataVfs(), mapId);
                if (local->digest == digest) { acceptMap(std::move(local), id); return; }
            } catch (const std::exception&) {} // Missing/different map: request the host's copy.
            // A transfer id of our own: chunks of an earlier request for this room are told apart.
            mapReceive_.xfer = ++xferSeq_;
            Writer request; request.u32(id); request.str(digest); request.u32(mapReceive_.xfer);
            send(Msg::MapRequest, request);
            mapStatus_ = "DOWNLOADING MAP 0%";
        } else if (f.kind == Msg::MapRequest) {
            // The server asks this host for the package it offered: the upload starts, or
            // starts over from offset 0 for a newer request. One for a package this host no
            // longer holds (the map changed since) is stale.
            auto id = r.u32(); auto digest = r.str(); auto xfer = r.u32();
            if (!r.ok || r.p != r.end) throw std::runtime_error("invalid map upload request");
            if (id != room_.id || room_.hostId != myId_ || !mapPackage_ || mapPackage_->digest != digest) return;
            mapSend_ = {id, xfer, 0, mapPackage_}; mapStatus_ = "UPLOADING MAP TO SERVER";
        } else if (f.kind == Msg::MapChunk) {
            const auto got = mapReceive_.append(r);
            if (got == maps::Append::Stale) return;   // left the old room, or an earlier request's
            if (got == maps::Append::Done) {
                const auto id = mapReceive_.room;
                acceptMap(maps::decode(std::move(mapReceive_.bytes), mapReceive_.digest), id);
            } else mapStatus_ = "DOWNLOADING MAP " + std::to_string(100 * mapReceive_.bytes.size() / mapReceive_.size) + "%";
        } else if (f.kind == Msg::MapError) {
            auto id = r.u32(); auto why = r.str();
            if (r.ok && id == room_.id) {
                // The answer to our START: the room shows it under its buttons (its map status
                // line is hidden once every map is verified, which is exactly when a START is
                // answered). It is not a map or connection error, so neither is left behind:
                // a stale "MAP ERROR" would come back whenever a joiner's map check hides the
                // room's readiness, and a later disconnect would report it as its reason.
                if (startSent_) { startError_ = why; startSent_ = false; }
                else if ((why.find("retry later") != std::string::npos || why.find("budget") != std::string::npos) &&
                         hostingLobby() && mapRetries_ < 3) {
                    // The server was busy (validation capacity, map memory): offer again after
                    // 2, 4 and 8 s. Only the status says so; it is not an error yet.
                    mapRetryAt_ = nowMs() + (2000ull << mapRetries_++);
                    mapStatus_ = "MAP ERROR: " + why + " (TRYING AGAIN)";
                }
                else { mapStatus_ = "MAP ERROR: " + why; err_ = mapStatus_; }
            }
        }
    } catch (const std::exception& e) {
        mapStatus_ = std::string("MAP ERROR: ") + e.what(); mapReadyRoom_ = 0;
        Writer w; w.u32(mapReceive_.room ? mapReceive_.room : room_.id); w.str(mapStatus_);
        send(Msg::MapError, w); mapReceive_ = {}; mapSend_ = {}; err_ = mapStatus_;
    }
}
}

namespace ta::net {
void MpClient::resetModTransfer() {
    modPackage_.reset();modReceive_={};modSend_={};modOfferedRoom_=modReadyRoom_=0;modStatus_.clear();modPendingAt_=0;
}
void MpClient::resetMapTransfer() {
    mapPackage_.reset(); mapPackageFor_.clear(); mapReceive_ = {}; mapSend_ = {};
    mapReadyRoom_ = mapOfferedRoom_ = 0; mapOfferedFor_.clear();
    startRequested_ = false; pendingMapStart_.reset(); mapStatus_.clear();
    startSent_ = false; startError_.clear();
    dropRoomMapRequest(); confirmedRoomId_ = 0; confirmedMapId_.clear();
    mapRetryAt_ = 0; mapRetries_ = 0; hostMapMissing_ = false;
}
void MpClient::offerRoomMap() {
    if (!hostingLobby() || !room_.id || room_.mapId.empty() || !room_.mission.empty() ||
        mapgen::isGeneratedMapId(room_.mapId))
        return;
    // Once per room and map; never while a map change is unanswered (only the room's map
    // as the server last said it is offered), nor while a busy server's retry waits.
    if ((mapOfferedRoom_ == room_.id && mapOfferedFor_ == room_.mapId) || mapSerialSent_ || mapRetryAt_) return;
    mapOfferedRoom_ = room_.id; mapOfferedFor_ = room_.mapId;
    try {
        if (!mapPackage_ || mapPackageFor_ != room_.mapId) {
            mapPackage_ = maps::build(roomDataVfs(), room_.mapId); mapPackageFor_ = room_.mapId;
        }
        send(Msg::MapOffer, maps::offer(room_.id, room_.mapId, *mapPackage_));
        mapStatus_ = "CHECKING MAP WITH SERVER"; hostMapMissing_ = false;
    } catch (const std::exception& e) {
        // This host cannot build the room's map (a successor without it): the guests learn why
        // from the server's chat line, and the room says to change the map.
        mapPackage_.reset(); mapPackageFor_.clear(); hostMapMissing_ = true;
        mapStatus_ = e.what(); Writer error; error.u32(room_.id); error.str(mapStatus_);
        send(Msg::MapError, error);
    }
}
void MpClient::setGameModPacks(std::vector<std::string> names) {
    // Only packs still on disk: a saved selection naming a pack folder since
    // removed or renamed must not stop every game from building its package
    // (the room lists only the packs that exist, so it could not be unticked).
    // modPacks lists ordinary packs only, so a tick an always-enabled pack hides
    // (same name) drops out too: that pack applies anyway.
    const auto oldAlways=alwaysGameModPacks_;rescanAlwaysGameMods();
    if(!gameModsAllowed_)names.clear();
    const auto have=hpi::modPacks(mapRoot_,hpi::ModKind::Game);
    std::erase_if(names,[&](const auto& n){return !std::binary_search(have.begin(),have.end(),n);});
    std::sort(names.begin(),names.end());names.erase(std::unique(names.begin(),names.end()),names.end());
    // One package carries at most 64 packs, the always-enabled ones first.
    names.resize(std::min(names.size(),size_t(64)-alwaysGameModPacks_.size()));
    if(names==gameModPacks_ && oldAlways==alwaysGameModPacks_)return;
    if(modBusy()){alwaysGameModPacks_=oldAlways;return;}
    gameModPacks_=std::move(names);modOfferedRoom_=0;
    if(state_==State::InRoom && room_.hostId==myId_ && room_.opts.gameMods) {
        // Coalesce checkbox clicks into one transfer once selection settles.
        modPendingAt_=nowMs()+500;modOfferedRoom_=room_.id;
        room_.mapsReady=false;modStatus_="APPLYING PACK SELECTION";
    }
}
void MpClient::rescanAlwaysGameMods() {
    alwaysGameModPacks_.clear();
    if(!gameModsAllowed_ || mapRoot_.empty())return;
    try {alwaysGameModPacks_=hpi::alwaysEnabledPacks(mapRoot_,hpi::ModKind::Game);}catch(const std::exception&){}
    if(alwaysGameModPacks_.size()>64)alwaysGameModPacks_.resize(64);
}
std::vector<std::string> MpClient::hostGameModPacks() const {
    if(!gameModsAllowed_)return {};
    auto names=alwaysGameModPacks_;names.insert(names.end(),gameModPacks_.begin(),gameModPacks_.end());
    std::sort(names.begin(),names.end());names.erase(std::unique(names.begin(),names.end()),names.end());
    return names;
}
void MpClient::offerMods() {
    modPendingAt_=0;room_.mapsReady=false;
    modOfferedRoom_=room_.id;modReadyRoom_=0;mapPackage_.reset();mapPackageFor_.clear();mapOfferedRoom_=mapReadyRoom_=0;mapOfferedFor_.clear();
    try {modPackage_=mods::build(mapRoot_,hostGameModPacks());
        send(Msg::ModOffer,maps::offer(room_.id,"mods",*modPackage_));modStatus_="VERIFYING GAME MODS";
    }catch(const std::exception& e){modPackage_.reset();modStatus_=std::string("GAME MODS: ")+e.what();}
}
hpi::Vfs MpClient::modVfs(const std::vector<std::string>& cosmetic,bool withCosmetic,std::vector<std::string>* skipped) const {
    hpi::Vfs::Files game;
    if(room_.opts.gameMods){if(!modPackage_)throw std::runtime_error("game mods not verified");game=*modPackage_->files;}
    // This player's own cosmetic mods on top (they cannot change the gameplay
    // hash): the always-enabled packs in every room, the ticked pack folders in
    // every room but a campaign one. A guest's own always-enabled GAME packs
    // never apply here: the host's package is the room's game data.
    const bool campaign=!room_.mission.empty();
    return hpi::mountPlayVfs(mapRoot_,std::move(game),campaign?std::vector<std::string>{}:cosmetic,withCosmetic,skipped);
}
hpi::Vfs MpClient::roomDataVfs() const {
    // The room's game data for a map package or a generated map: the host's own
    // selection, or for a guest the host's verified game-mod package. A guest's
    // saved packs (from its own single-player games) never stand in for the host's.
    if(!room_.opts.gameMods)return hpi::mountRetailRoot(mapRoot_);
    if(room_.hostId==myId_)return hpi::mountRetailRoot(mapRoot_,hostGameModPacks());
    auto vfs=hpi::mountRetailRoot(mapRoot_);
    if(modPackage_)vfs.setModFiles(modPackage_->files);
    return vfs;
}
void MpClient::acceptMods(std::shared_ptr<mods::Package> package,uint32_t room) {
    modPackage_=std::move(package);modReceive_={};modReadyRoom_=room;
    modStatus_="GAME MODS VERIFIED";
    // Like acceptMap: the package is verified, so a cache failure (full disk)
    // is reported but must not reject it.
    try {if(const auto cache=cacheRoot();!cache.empty())mods::saveCache(cache,*modPackage_);}
    catch(const std::exception& e){modStatus_=std::string("GAME MODS VERIFIED; SAVE FAILED: ")+e.what();chat_.push_back({"SYSTEM",modStatus_});std::fprintf(stderr,"%s\n",modStatus_.c_str());}
    Writer w;w.u32(room);w.str(modPackage_->digest);send(Msg::ModReady,w);
    if(pendingMapStart_){auto start=std::move(*pendingMapStart_);pendingMapStart_.reset();onFrame(start);}
}
void MpClient::modFrame(const Frame& f) {
    try {
        Reader r(f.payload.data(),f.payload.size());
        if(f.kind==Msg::ModOffer){
            auto id=r.u32();auto label=r.str(),digest=r.str();auto size=r.u32();
            if(!r.ok || r.p!=r.end || !id || label!="mods" || (room_.id && id!=room_.id))throw std::runtime_error("invalid game mod offer");
            modReceive_.begin(id,++xferSeq_,size,digest);modReadyRoom_=0;
            if(modPackage_ && modPackage_->digest==digest && modPackage_->bytes.size()==size){acceptMods(modPackage_,id);return;}
            if(auto cached=cacheRoot().empty()?nullptr:mods::loadCache(cacheRoot(),digest)){if(cached->bytes.size()!=size)throw std::runtime_error("game mod size mismatch");acceptMods(std::move(cached),id);return;}
            Writer w;w.u32(id);w.str(digest);w.u32(modReceive_.xfer);send(Msg::ModRequest,w);modStatus_="DOWNLOADING GAME MODS";
        }else if(f.kind==Msg::ModRequest){
            // Starts the upload, or starts it over from offset 0 for a newer request (xfer);
            // a request for another package than this host's is stale.
            auto id=r.u32();auto digest=r.str();auto xfer=r.u32();
            if(!r.ok || r.p!=r.end)throw std::runtime_error("invalid game mod upload request");
            if(id!=room_.id || room_.hostId!=myId_ || !modPackage_ || modPackage_->digest!=digest)return;
            modSend_={id,xfer,0,modPackage_};modStatus_="UPLOADING GAME MODS";
        }else if(f.kind==Msg::ModChunk){
            const auto got=modReceive_.append(r);
            if(got==maps::Append::Done){auto id=modReceive_.room;acceptMods(mods::decode(std::move(modReceive_.bytes),modReceive_.digest),id);}
        }else if(f.kind==Msg::ModError){auto id=r.u32();auto why=r.str();if(r.ok && id==room_.id){modStatus_="GAME MODS: "+why;modReceive_={};modSend_={};modReadyRoom_=0;}}
    }catch(const std::exception& e){modStatus_=std::string("GAME MODS: ")+e.what();modReceive_={};modSend_={};modReadyRoom_=0;Writer w;w.u32(room_.id);w.str(modStatus_);send(Msg::ModError,w);}
}
}
