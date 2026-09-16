#include "net/client.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "net/auth.h"
#include "net/crypto.h"

namespace ta::net {

namespace {
uint64_t nowMs() {   // monotonic wall-clock (pacing/keepalive only; never hashed)
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
constexpr uint64_t kPingIdleMs = 5000, kTimeoutMs = 15000;
constexpr uint64_t kSpectatorTimeoutMs = 120000;   // matches the server's spectator grace
}  // namespace

bool MpClient::connect(const std::string& host, uint16_t port, const std::string& name) {
    name_ = name;
    if (!conn_.connect(host, port)) { err_ = conn_.error(); state_ = State::Done; return false; }
    state_ = State::Connecting;
    Writer w;
    w.u32(kNetVersion);
    w.str("");          // build id (version-gated only)
    w.u64(dataHash_);   // gameplay-data fingerprint (enforced by the server)
    w.str(name_);
    send(Msg::Hello, w);
    lastRecvMs_ = nowMs();
    return true;
}

MpClient::~MpClient() {
    if (derive_.joinable()) derive_.join();   // never outlive the key-derivation worker
    crypto::wipe(loginPass_);
}

void MpClient::setLogin(const std::string& user, const std::string& password) {
    loginUser_ = user;
    loginPass_ = password;
}

void MpClient::disconnect(const std::string& reason) {
    if (conn_.ok()) { Writer w; w.str(reason); send(Msg::Bye, w); conn_.flushWrite(); }
    conn_.closeNow();
    state_ = State::Done;
}

bool MpClient::poll() {
    if (state_ == State::Offline || state_ == State::Done) return false;
    if (jitterMs_ < 0) {   // one-time init of the test link model
        const char* j = std::getenv("TA_NET_JITTER_MS");
        const char* b = std::getenv("TA_NET_BASE_MS");
        const char* l = std::getenv("TA_NET_LOSS_PCT");
        jitterMs_ = j ? std::max(0, std::atoi(j)) : 0;
        baseMs_ = b ? std::max(0, std::atoi(b)) : 0;
        lossPct_ = l ? std::clamp(std::atoi(l), 0, 100) : 0;
    }
    // recv() no longer fails on a clean close, so the frames that arrived alongside
    // the FIN get drained BEFORE the connection is finished off. That matters here as
    // well as on the server: a Reject sent immediately before the close used to be
    // buffered and then thrown away, leaving only a bare "peer closed" to explain a
    // rejection the server had actually spelled out.
    const bool recvOk = conn_.recv();
    Frame f;
    if (recvOk)
        while (conn_.poll(f)) { onFrame(f); if (!conn_.ok()) break; }
    if (!recvOk || conn_.peerClosed()) {
        // Keep an existing reason (e.g. the server's Reject text): the socket
        // closing right after a Reject must not overwrite WHY with "peer closed".
        if (err_.empty()) {
            err_ = conn_.error().empty() ? std::string("peer closed") : conn_.error();
            // A close during the handshake (never Welcomed) is almost always a
            // version gate on a server too old to flush its Reject reason --
            // say so instead of a bare "peer closed".
            if (state_ == State::Connecting)
                err_ = "server closed during handshake -- likely a protocol version "
                       "mismatch (this client speaks v" + std::to_string(kNetVersion) + ")";
        }
        state_ = State::Done;
        return false;
    }
    pumpDerive();          // the login's PBKDF2 finished on its worker: send the proof
    uint64_t now = nowMs();
    // Release any jitter-held bundles whose delay has elapsed.
    if (!jitterHeld_.empty()) {
        for (auto it = jitterHeld_.begin(); it != jitterHeld_.end();) {
            if (it->releaseMs <= now) { bundles_[it->tick] = std::move(it->bd);
                                        noteBundleArrival(now); it = jitterHeld_.erase(it); }
            else ++it;
        }
    }
    // Active RTT probe (only when a consumer enabled it): one ping/sec.
    if (measureRtt_ && !pingSentMs_ && now - lastPingMs_ > 1000) {
        send(Msg::Ping); lastPingMs_ = now; pingSentMs_ = now;
    }
    if (now - lastRecvMs_ > kPingIdleMs && now - lastPingMs_ > kPingIdleMs) {
        send(Msg::Ping); lastPingMs_ = now;
    }
    // A spectator's render loop can freeze for several seconds on a heavy frame
    // (big supersampled resolution under VRAM pressure) without pumping the socket;
    // give it the same long grace the server extends to spectators so a transient
    // stall doesn't self-terminate a display-only connection.
    uint64_t timeoutMs = spectator_ ? kSpectatorTimeoutMs : kTimeoutMs;
    if (now - lastRecvMs_ > timeoutMs) { if (err_.empty()) err_ = "server timeout"; state_ = State::Done; }
    if (!conn_.flushWrite()) { if (err_.empty()) err_ = conn_.error(); state_ = State::Done; }
    if (!conn_.ok() && err_.empty()) { err_ = conn_.error(); state_ = State::Done; }
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
// See src/net/auth.h for what each value is and why the password itself never
// appears anywhere in it.

void MpClient::failAuth(const std::string& why) {
    auth_ = Auth::Failed;
    if (err_.empty()) err_ = why;
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

void MpClient::onAuthChallenge(Reader& r) {
    if (auth_ != Auth::Pending) { failAuth("unexpected login challenge"); return; }
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
    if (!r.ok) { failAuth("malformed login result"); return; }

    if (status != AuthStatus::Ok && status != AuthStatus::Created) {
        // The failure statuses are the only ones a server may legitimately send
        // before a challenge (Throttled / BadUsername / ServerError all come
        // straight out of the AuthBegin handler), so they are answered here,
        // ahead of the state guard below.
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
    // would otherwise be a free way to skip this check entirely.
    {
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
    crypto::wipe(keys_.clientKey.data(), keys_.clientKey.size());
    crypto::wipe(keys_.serverKey.data(), keys_.serverKey.size());
    // The server follows this with a Welcome, which moves us into the lobby.
}

static void readSlots(Reader& r, RoomView& v) {
    v.id = r.u32();
    v.name = r.str();
    v.mapId = r.str();
    v.mission = r.str();
    v.opts.forfeitSelfDestruct = r.u8();
    v.opts.overridePolicy = r.u8();
    v.opts.speed = r.u8(); v.opts.speedUnlock = r.u8();
    v.opts.unitCap = uint16_t(r.u32());
    v.opts.commanderExpendable = r.u8();
    v.opts.stressTest = r.u8();
    v.opts.fogExplored = r.u8();
    v.opts.benchmark = r.u8();
    v.opts.randomStarts = r.u8();
    v.hostId = r.u32();
    for (int i = 0; i < kMaxSlots; ++i) {
        SlotInfo& s = v.slots[i];
        s.type = r.u8(); s.faction = r.u8(); s.color = r.u8(); s.team = r.u8();
        s.ready = r.u8(); s.aiLevel = r.u8(); s.name = r.str();
    }
}

void MpClient::onFrame(const Frame& f) {
    lastRecvMs_ = nowMs();
    Reader r(f.payload.data(), f.payload.size());
    switch (f.kind) {
        case Msg::Welcome:
            myId_ = r.u32();
            name_ = r.str();
            // A Welcome is NOT a login. If we came here with credentials, the
            // only way in is a verified AuthResult -- otherwise a machine posing
            // as the server could skip the whole exchange and answer our Hello
            // with a bare Welcome, and we would happily play on a connection that
            // proved nothing.
            if (!loginUser_.empty() && auth_ != Auth::Ok && auth_ != Auth::Created) {
                failAuth("the server let us in without checking the account -- "
                         "do not trust this connection");
                return;
            }
            // The Welcome name is authoritative: names match case-insensitively,
            // so someone who typed "CURTIS" is signed in to "curtis" and should be
            // shown as its owner spelled it, not as they happened to type it.
            if (auth_ != Auth::None && !name_.empty()) account_ = name_;
            state_ = State::Lobby;
            break;
        case Msg::AuthRequired: sendAuthBegin(); break;
        case Msg::AuthChallenge: onAuthChallenge(r); break;
        case Msg::AuthResult: onAuthResult(r); break;
        case Msg::Reject:
            err_ = r.str();
            state_ = State::Done;
            break;
        case Msg::Ping: send(Msg::Pong); break;
        case Msg::Pong:
            if (pingSentMs_) {
                // Add the modelled return-path delay so RTT reflects the test link.
                float sample = float(nowMs() - pingSentMs_) + float(receiveDelayMs());
                rttMs_ = rttMs_ > 0 ? 0.7f * rttMs_ + 0.3f * sample : sample;
                pingSentMs_ = 0;
            }
            break;
        case Msg::Bye: err_ = r.str(); state_ = State::Done; break;
        case Msg::GameList: {
            games_.clear();
            uint32_t n = r.u32();
            for (uint32_t i = 0; i < n && r.ok; ++i) {
                GameInfo g;
                g.id = r.u32(); g.name = r.str(); g.mapId = r.str();
                g.players = r.u8(); g.capacity = r.u8(); g.running = r.u8();
                g.passworded = r.u8(); g.uptimeSec = r.u32();
                games_.push_back(g);
            }
            break;
        }
        case Msg::JoinResult: {
            uint8_t ok = r.u8(); uint8_t slot = r.u8(); r.str();
            if (ok) {
                // 0xFF = the host created the game as a slot-less spectator.
                room_.mySlot = (slot == 0xFF) ? -1 : int(slot);
                spectator_ = (slot == 0xFF);
                state_ = State::InRoom;
            }
            break;
        }
        case Msg::LobbyState: {
            int keep = room_.mySlot;
            readSlots(r, room_);
            room_.mySlot = keep;
            gameSpeed_ = room_.opts.speed;
            if (state_ == State::Lobby) state_ = State::InRoom;
            break;
        }
        case Msg::SpeedUpdate: { uint8_t s = r.u8(); if (r.ok) { gameSpeed_ = s; room_.opts.speed = s; } break; }
        case Msg::MissionOutcome: { int8_t o = int8_t(r.u8()); if (r.ok) missionOutcome_ = o; break; }
        case Msg::Chat: {
            std::string who = r.str(), text = r.str();
            if (r.ok) chat_.push_back({who, text});
            break;
        }
        case Msg::GameStarting: {
            int keep = room_.mySlot;
            readSlots(r, room_);
            gameSpeed_ = room_.opts.speed;
            missionOutcome_ = 0;   // fresh game/replay
            slotLoaded_.fill(false);

            // Snapshot the starting slot table for a replay header, before forfeits
            // and drops rewrite the live one.
            for (int i = 0; i < kMaxSlots; ++i) startSlots_[i] = room_.slots[i];
            uint8_t mySlot = r.u8();
            startSeed_ = r.u32();
            resumeToken_ = r.u64();
            replayTicks_ = r.u32();   // bundles logged before we joined (0 = none)
            // 0xFF marks a spectator (no slot); map it to -1.
            room_.mySlot = !r.ok ? keep : (mySlot == 0xFF ? -1 : int(mySlot));
            // Record a replay if we are PLAYING this game. Started here rather than
            // when the view sets up, because the backlog a rejoin/spectate receives
            // arrives immediately after this message -- any later and the recording
            // would be missing its first ticks. Spectators (slot -1) do not record:
            // the request is that each human PLAYER keeps its own copy.
            if (room_.mySlot >= 0) startRecording(); else stopRecording();
            // Any 0xFF start is a spectator (a create-as-spectator host, or spectate()).
            spectator_ = expectingSpectate_ || (r.ok && mySlot == 0xFF);
            // Route EVERY spectator through the "set up before the state branches" path
            // (isRejoin): a spectator never reports Loaded, so the server starts sending
            // TickBundles immediately and the client's state can flip Starting->InGame
            // before the normal starting-branch runs -- which would skip world setup and
            // leave the spectator watching an empty sim. A host-spectator whose game is
            // just starting simply has an empty backlog to "replay".
            rejoin_ = expectingRejoin_ || expectingSpectate_ || spectator_;
            expectingRejoin_ = expectingSpectate_ = false;
            state_ = State::Starting;
            break;
        }
        case Msg::PlayerStatus: {
            // slot, status (0=connected 1=dropped 2=loaded), ping. Only the loaded
            // flag is consumed today -- it fills that player's loading-screen bar.
            uint8_t slot = r.u8(), status = r.u8();
            if (r.ok && slot < kMaxSlots && status == 2) slotLoaded_[slot] = true;
            break;
        }
        case Msg::Pause: paused_ = true; break;
        case Msg::Resume: paused_ = false; break;
        case Msg::TickBundle: {
            // Record the payload VERBATIM, before parsing: the server logs this exact
            // buffer, so replaying our copy reproduces the game bit-for-bit. Parsing
            // and re-serializing would risk a format drift between the two writers.
            if (recording_) replayLog_.push_back(f.payload);
            uint32_t tk = r.u32();
            Bundle bd;
            uint32_t nc = r.u32();
            for (uint32_t i = 0; i < nc && r.ok; ++i) bd.cmds.push_back(r.cmd());
            uint32_t ne = r.u32();
            for (uint32_t i = 0; i < ne && r.ok; ++i) {
                Event e; e.kind = Event::Kind(r.u8()); e.player = r.u8();
                bd.events.push_back(e);
            }
            if (r.ok) {
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
    Writer w; w.str(name); w.str(password); w.str(mapId); w.str(mission);
    w.u8(o.forfeitSelfDestruct); w.u8(o.overridePolicy);
    w.u8(o.speed); w.u8(o.speedUnlock); w.u32(o.unitCap); w.u8(o.commanderExpendable);
    w.u8(o.stressTest); w.u8(o.fogExplored); w.u8(o.benchmark); w.u8(o.randomStarts);
    w.u8(capacity);   // map's start-position count (the server has no map data)
    w.u8(spectate ? 1 : 0);   // host watches, taking no slot
    w.u8(priv ? 1 : 0);       // private (single-player): not in the public game list
    send(Msg::CreateGame, w);
}

void MpClient::joinGame(uint32_t id, const std::string& password) {
    Writer w; w.u32(id); w.str(password);
    send(Msg::JoinGame, w);
}

void MpClient::leaveGame() {
    send(Msg::LeaveGame);
    room_ = RoomView{};
    state_ = State::Lobby;
}

void MpClient::setSlot(int slot, uint8_t type, uint8_t faction, uint8_t color,
                       uint8_t team, uint8_t ready, uint8_t aiLevel) {
    Writer w; w.u8(uint8_t(slot)); w.u8(type); w.u8(faction); w.u8(color); w.u8(team); w.u8(ready);
    w.u8(aiLevel);
    send(Msg::SlotUpdate, w);
}

void MpClient::setGameOptions(const GameOptions& o) {
    Writer w; w.u8(o.forfeitSelfDestruct);
    w.u8(o.overridePolicy); w.u8(o.speed); w.u8(o.speedUnlock); w.u32(o.unitCap); w.u8(o.commanderExpendable);
    w.u8(o.stressTest); w.u8(o.fogExplored); w.u8(o.benchmark); w.u8(o.randomStarts);
    send(Msg::SetGameOptions, w);
}

void MpClient::setPause(bool want) { Writer w; w.u8(want ? 1 : 0); send(Msg::SetPause, w); }

void MpClient::kick(int slot) { Writer w; w.u8(uint8_t(slot)); send(Msg::Kick, w); }

void MpClient::chat(const std::string& text) { Writer w; w.str(text); send(Msg::Chat, w); }

void MpClient::startGame() { send(Msg::StartGame); }

// ---- game play ------------------------------------------------------------

void MpClient::reportLoaded(uint64_t dataHash) { Writer w; w.u64(dataHash); send(Msg::Loaded, w); }

void MpClient::rejoin(uint32_t gameId, uint64_t token) {
    expectingRejoin_ = true;
    Writer w; w.u32(gameId); w.u64(token);
    send(Msg::Rejoin, w);
}

void MpClient::spectate(uint32_t gameId, const std::string& password) {
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

void MpClient::sendHash(uint32_t tick, uint64_t hash) {
    // Keep our own trail while recording, so the replay carries what this client
    // actually computed at each checkpoint.
    if (recording_) hashLog_.push_back({tick, hash});
    Writer w; w.u32(tick); w.u64(hash);
    send(Msg::StateHash, w);
}

}  // namespace ta::net
