// authrogue: points a REAL MpClient at a HOSTILE server and proves it walks away.
//
//   authrogue
//   authrogue --live <tls://host> <port> <data hash in hex>
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
// --live instead drives the client's sign-in token against a REAL takserver over TLS
// (account_auth_network_test.py runs it; SSL_CERT_FILE must trust the server).
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
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace tak;
using namespace tak::net;

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
    TokenInTheClear,          // honest, and offers a sign-in token on this plaintext link
};

const char* kUser = "curtis";
const char* kPass = "correct horse battery";
constexpr uint32_t kIters = 4096;   // the real one is 600k; keep the test quick

// One connection's worth of fake server. Runs on its own thread.
void serveOnce(int listenFd, Attack attack, std::atomic<bool>* done, std::atomic<bool>* sawToken) {
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
        if (attack == Attack::TokenInTheClear) w.bytes(std::vector<uint8_t>(auth::kTokenLen, 0x77));
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
            } else if (f.kind == Msg::AuthToken) {
                sawToken->store(true);
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
    bool sawToken = false;       // a sign-in token reached the fake server
};

// `token` (optional): the client's sign-in token; `tokenSignIn` spends it.
Outcome run(Attack attack, uint16_t port, SessionToken* token = nullptr, bool tokenSignIn = false) {
    std::string err;
    int lf = listenOn(port, err, /*loopbackOnly=*/true);
    if (lf < 0) { Outcome o; o.err = "listen: " + err; return o; }

    std::atomic<bool> done{false}, sawToken{false};
    std::thread srv(serveOnce, lf, attack, &done, &sawToken);

    Outcome o;
    {
        MpClient cl;
        cl.setLogin(kUser, tokenSignIn ? "" : kPass);
        cl.setSessionToken(token, tokenSignIn);
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
    o.sawToken = sawToken.load();
    return o;
}

bool signedIn(const Outcome& o) {
    return o.auth == MpClient::Auth::Ok || o.auth == MpClient::Auth::Created;
}

// One real login against a real server: `token` is where it keeps what it earns, and
// with `password` empty it signs in by spending that.
MpClient::Auth login(const std::string& host, uint16_t port, uint64_t dataHash,
                     const std::string& password, SessionToken& token) {
    MpClient cl;
    cl.setDataHash(dataHash);
    cl.setLogin(kUser, password);
    cl.setSessionToken(&token, password.empty());
    if (!cl.connect(host, port, kUser)) return MpClient::Auth::Failed;
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!cl.handshakeSettled() && std::chrono::steady_clock::now() < limit) {
        if (!cl.poll()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto result = cl.auth();
    if (result != MpClient::Auth::Failed) check(cl.state() == MpClient::State::Lobby, "reaches the lobby");
    cl.disconnect();
    return result;
}

int live(const std::string& host, uint16_t port, uint64_t dataHash) {
    SessionToken token;
    std::printf("the sign-in token against a real server\n");
    check(login(host, port, dataHash, kPass, token) == MpClient::Auth::Created,
          "a password login registers");
    check(token.heldFor(host, port), "and earns a token");
    const std::vector<uint8_t> first = token.secret;

    check(login(host, port, dataHash, "", token) == MpClient::Auth::Ok, "the token signs in");
    check(token.heldFor(host, port) && token.secret != first, "and is replaced by the next");

    // A client whose link took the reply still holds the token it sent.
    SessionToken lost;
    lost.host = host; lost.port = port; lost.account = kUser; lost.secret = first;
    check(login(host, port, dataHash, "", lost) == MpClient::Auth::Ok,
          "a token whose reply was lost can be sent again");
    check(lost.heldFor(host, port) && lost.secret != first, "and earns the next");
    check(login(host, port, dataHash, "", token) == MpClient::Auth::Failed,
          "what the lost reply carried is refused");
    check(token.secret.empty(), "and the client forgets it");
    check(login(host, port, dataHash, "", lost) == MpClient::Auth::Failed,
          "and that used the line up: the resend's token goes too");

    check(login(host, port, dataHash, kPass, token) == MpClient::Auth::Ok, "the password signs in");
    SessionToken elsewhere;
    elsewhere.host = "tls://elsewhere.invalid"; elsewhere.port = port;
    elsewhere.account = kUser; elsewhere.secret = token.secret;
    check(login(host, port, dataHash, "", elsewhere) == MpClient::Auth::Failed,
          "a token from another address is never sent");
    check(login(host, port, dataHash, "", token) == MpClient::Auth::Ok,
          "the current one did not go (it still signs in)");

    SessionToken stale;
    stale.host = host; stale.port = port; stale.account = kUser; stale.secret = token.secret;
    MpClient::revokeSessionToken(token, dataHash);
    check(token.secret.empty(), "quitting wipes the token");
    check(login(host, port, dataHash, "", stale) == MpClient::Auth::Failed,
          "and the server no longer takes it");

    check(login(host, port, dataHash, kPass, token) == MpClient::Auth::Ok,
          "the password still signs in");
    check(token.heldFor(host, port), "and earns a new token");

    // A second password login replaces the token; quitting has to sign that one out too.
    stale.secret = token.secret;
    check(login(host, port, dataHash, kPass, token) == MpClient::Auth::Ok, "so does another");
    check(token.heldFor(host, port) && token.secret != stale.secret && token.retired.size() == 1,
          "and the token it replaces is kept to revoke");
    MpClient::revokeSessionToken(token, dataHash);
    check(token.secret.empty() && token.retired.empty(), "quitting wipes them all");
    check(login(host, port, dataHash, "", stale) == MpClient::Auth::Failed,
          "and the replaced one no longer signs in");

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    netStartup();
    if (argc == 5 && std::string(argv[1]) == "--live")
        return live(argv[2], uint16_t(std::atoi(argv[3])), std::strtoull(argv[4], nullptr, 16));
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

    std::printf("a sign-in token never crosses a plaintext link\n");
    {
        SessionToken token;
        Outcome o = run(Attack::TokenInTheClear, port++, &token);
        check(o.auth == MpClient::Auth::Ok, "a plaintext password login still signs in", o.err);
        check(token.secret.empty(), "but a token offered on it is not taken");
    }
    {
        SessionToken token;
        token.host = "127.0.0.1"; token.port = port; token.account = kUser;
        token.secret.assign(auth::kTokenLen, 0x42);
        Outcome o = run(Attack::Honest, port++, &token, true);
        check(!signedIn(o), "a token held for a plaintext address cannot sign in", o.err);
        check(!o.sawToken, "because the client never sends it");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
