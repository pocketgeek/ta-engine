// authrogue: points a REAL MpClient at a HOSTILE server and proves it walks away.
//
//   authrogue
//
// The login's whole claim is that a machine posing as the server gains nothing:
// it cannot sign you in, it cannot learn your password, and it cannot skip the
// proof. Guards that assert this are easy to write and easy to get subtly wrong,
// so this drives the actual client -- src/net/client.cpp, unmodified -- against a
// server that lies, and checks it refuses.
//
// Each scenario is a fake server on a loopback port, speaking the real framing.
// The client must end up NOT signed in for every attack, and signed in for the
// one honest control.
//
// A dev harness; not shipped in a game.

#include "net/auth.h"
#include "net/client.h"
#include "net/conn.h"
#include "net/crypto.h"
#include "net/netcompat.h"
#include "net/protocol.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace ta;
using namespace ta::net;

namespace {

int failures = 0;

void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}

// What the fake server should do once it has the client's AuthBegin.
enum class Attack {
    ResultWithoutChallenge,   // skip the challenge; forge over the zeroed ServerKey
    CreatedInsteadOfOk,       // challenge as an existing account, answer "Created"
    UnsignedOk,               // challenge properly, then send Ok with no signature
    BareWelcome,              // ask for a login, then just let them in
    ChallengeStorm,           // many challenges, to make the client derive in a loop
    Honest,                   // the control: a correct exchange that must succeed
};

const char* kUser = "curtis";
const char* kPass = "correct horse battery";
constexpr uint32_t kIters = 4096;   // the real one is 600k; keep the test quick

// One connection's worth of fake server. Runs on its own thread.
void serveOnce(int listenFd, Attack attack, std::atomic<bool>* done) {
    int fd = -1;
    for (int i = 0; i < 4000 && fd < 0; ++i) {
        fd = int(accept(listenFd, nullptr, nullptr));
        if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (fd < 0) { done->store(true); return; }
    setupSocket(fd);
    Conn c(fd);

    // The credential a real server would hold for this account, so the honest
    // control can be answered correctly.
    std::vector<uint8_t> salt(auth::kSaltLen, 0x5a);
    auth::Keys keys = auth::deriveKeys(kPass, salt.data(), salt.size(), kIters);
    auth::Credential cred = auth::makeCredential(keys, salt.data(), salt.size(), kIters);

    std::vector<uint8_t> cnonce, snonce(auth::kNonceLen, 0x11);
    std::string user;
    auto sendResult = [&](AuthStatus st, const crypto::Digest* sig, const char* msg) {
        Writer w;
        w.u8(uint8_t(st));
        if (sig) w.bytes(sig->data(), sig->size()); else w.bytes(nullptr, 0);
        w.str(msg);
        c.send(Msg::AuthResult, w);
    };
    auto welcome = [&] {
        Writer w; w.u32(7); w.str(user.empty() ? std::string(kUser) : user);
        c.send(Msg::Welcome, w);
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!c.recv()) break;
        Frame f;
        bool any = false;
        while (c.poll(f)) {
            any = true;
            Reader r(f.payload.data(), f.payload.size());
            if (f.kind == Msg::Hello) {
                r.u32(); r.str(); r.u64(); r.str();
                c.send(Msg::AuthRequired);
            } else if (f.kind == Msg::AuthBegin) {
                user = r.str();
                cnonce = r.bytes(auth::kNonceLen);
                if (attack == Attack::ResultWithoutChallenge) {
                    // No challenge at all. pend_ is empty on the client, so the
                    // transcript it will check against is fully known to us and
                    // its ServerKey is still zero -- forge over exactly that.
                    std::vector<uint8_t> am =
                        auth::authMessage(user, cnonce, {}, {}, 0);
                    crypto::Digest sig = auth::serverSignature(crypto::Digest{}, am);
                    sendResult(AuthStatus::Ok, &sig, "signed in");
                    welcome();
                    continue;
                }
                if (attack == Attack::BareWelcome) { welcome(); continue; }
                Writer w;
                w.u8(0);                 // "this account exists" -> client sends a proof
                w.bytes(salt);
                w.u32(kIters);
                w.bytes(snonce);
                c.send(Msg::AuthChallenge, w);
                if (attack == Attack::ChallengeStorm)
                    for (int i = 0; i < 32; ++i) c.send(Msg::AuthChallenge, w);
            } else if (f.kind == Msg::AuthProof) {
                std::vector<uint8_t> proofBytes = r.bytes(crypto::kHashLen);
                std::vector<uint8_t> am = auth::authMessage(user, cnonce, snonce, salt, kIters);
                if (attack == Attack::CreatedInsteadOfOk) {
                    // Claim we made a new account. If the client only verifies the
                    // signature on the Ok path, this skips mutual auth entirely.
                    sendResult(AuthStatus::Created, nullptr, "new account created");
                    welcome();
                } else if (attack == Attack::UnsignedOk) {
                    sendResult(AuthStatus::Ok, nullptr, "signed in");
                    welcome();
                } else {   // Honest
                    crypto::Digest proof{};
                    std::memcpy(proof.data(), proofBytes.data(), proof.size());
                    if (!auth::verifyClientProof(cred, am, proof)) {
                        sendResult(AuthStatus::BadPassword, nullptr, "that password is not right");
                    } else {
                        crypto::Digest sig = auth::serverSignature(cred.serverKey, am);
                        sendResult(AuthStatus::Ok, &sig, "signed in");
                        welcome();
                    }
                }
            }
        }
        c.flushWrite();
        // recv() no longer returns false on a clean close -- it keeps the frames that
        // arrived with the FIN and reports the close here -- so this loop has to ask.
        // Without it a client that hangs up just spins until the 20s deadline and
        // stalls srv.join() for the whole of it.
        if (c.peerClosed()) break;
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!c.ok()) break;
    }
    c.flushWrite();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));   // let the tail drain
    c.closeNow();
    done->store(true);
}

struct Outcome {
    MpClient::Auth auth = MpClient::Auth::None;
    MpClient::State state = MpClient::State::Offline;
    std::string err;
    double seconds = 0;
};

Outcome run(Attack attack, uint16_t port) {
    std::string err;
    int lf = listenOn(port, err, /*loopbackOnly=*/true);
    if (lf < 0) { Outcome o; o.err = "listen: " + err; return o; }

    std::atomic<bool> done{false};
    std::thread srv(serveOnce, lf, attack, &done);

    Outcome o;
    {
        MpClient cl;
        cl.setLogin(kUser, kPass);
        const auto t0 = std::chrono::steady_clock::now();
        if (!cl.connect("127.0.0.1", port, kUser)) {
            o.err = "connect: " + cl.error();
        } else {
            const auto limit = t0 + std::chrono::seconds(15);
            while (!cl.handshakeSettled() && std::chrono::steady_clock::now() < limit) {
                if (!cl.poll()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            o.err = cl.error();
        }
        o.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        o.auth = cl.auth();
        o.state = cl.state();
    }
    srv.join();
    sockClose(lf);
    return o;
}

bool signedIn(const Outcome& o) {
    return o.auth == MpClient::Auth::Ok || o.auth == MpClient::Auth::Created;
}

}  // namespace

int main() {
    netStartup();
    uint16_t port = 8390;

    std::printf("a hostile server must not be able to sign us in\n");
    {
        Outcome o = run(Attack::ResultWithoutChallenge, port++);
        check(!signedIn(o),
              "AuthResult with no challenge (forged over the zeroed ServerKey) is refused",
              o.err);
    }
    {
        Outcome o = run(Attack::CreatedInsteadOfOk, port++);
        check(!signedIn(o),
              "answering a sign-in with an unsigned \"account created\" is refused", o.err);
    }
    {
        Outcome o = run(Attack::UnsignedOk, port++);
        check(!signedIn(o), "an AuthResult carrying no signature is refused", o.err);
    }
    {
        Outcome o = run(Attack::BareWelcome, port++);
        check(!signedIn(o), "a bare Welcome with no login at all is refused", o.err);
        check(o.state != MpClient::State::Lobby, "and it does not reach the lobby");
    }
    {
        // A second challenge must be refused outright rather than starting another
        // key derivation -- each one is deliberately expensive, and the join
        // happens on the caller's thread.
        Outcome o = run(Attack::ChallengeStorm, port++);
        check(!signedIn(o), "a storm of login challenges is refused", o.err);
        check(o.seconds < 8.0, "and does not hang the client",
              "took " + std::to_string(o.seconds) + "s");
    }

    std::printf("...but an honest server still works\n");
    {
        Outcome o = run(Attack::Honest, port++);
        check(o.auth == MpClient::Auth::Ok, "a correct exchange signs in", o.err);
        check(o.state == MpClient::State::Lobby, "and reaches the lobby");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
