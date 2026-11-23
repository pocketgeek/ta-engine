// authtest: proves the multiplayer login crypto and the account store.
//
//   authtest [scratch-dir]
//
// The primitives are checked against the PUBLISHED test vectors -- FIPS 180-4
// for SHA-256, RFC 4231 for HMAC-SHA-256, RFC 7914 §11 for PBKDF2-HMAC-SHA256 --
// because "I wrote a hash function and it looks right" is worth nothing. Then the
// SCRAM exchange is driven end to end, including the attacks it is supposed to
// stop: replaying a captured proof, logging in with a stolen accounts file, and
// a server that does not actually know the account.
//
// A dev harness; not shipped in a game.

#include "net/auth.h"
#include "net/crypto.h"
#include "server/accounts.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace tak;

namespace {

int failures = 0;

void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}

void checkHex(const std::string& got, const char* want, const char* what) {
    check(got == want, what, got == want ? "" : "got " + got + ", want " + std::string(want));
}

std::vector<uint8_t> bytes(std::string_view s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> repeated(uint8_t b, size_t n) { return std::vector<uint8_t>(n, b); }

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";

    // ---- 1. SHA-256, FIPS 180-4 --------------------------------------------
    std::printf("SHA-256 (FIPS 180-4)\n");
    checkHex(crypto::toHex(crypto::sha256("")),
             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
             "empty string");
    checkHex(crypto::toHex(crypto::sha256("abc")),
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
             "\"abc\"");
    checkHex(crypto::toHex(crypto::sha256(
                 "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
             "56-byte multi-block message");
    checkHex(crypto::toHex(crypto::sha256(
                 "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                 "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
             "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
             "112-byte message");
    {   // the classic length-padding edge cases: exactly 55, 56, 63, 64 bytes
        std::string a(1000000, 'a');
        checkHex(crypto::toHex(crypto::sha256(a)),
                 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                 "one million 'a' (streaming, crosses every block boundary)");
        struct { size_t n; const char* want; } kLens[] = {
            {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
            {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
            {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
            {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        };
        for (auto& t : kLens)
            checkHex(crypto::toHex(crypto::sha256(std::string(t.n, 'a'))), t.want,
                     ("padding edge case: " + std::to_string(t.n) + " bytes").c_str());
    }

    // ---- 2. HMAC-SHA-256, RFC 4231 -----------------------------------------
    std::printf("HMAC-SHA-256 (RFC 4231)\n");
    {
        auto k1 = repeated(0x0b, 20);
        checkHex(crypto::toHex(crypto::hmacSha256(k1.data(), k1.size(), "Hi There", 8)),
                 "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
                 "case 1");
        checkHex(crypto::toHex(crypto::hmacSha256("Jefe", "what do ya want for nothing?")),
                 "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
                 "case 2");
        auto k3 = repeated(0xaa, 20);
        auto d3 = repeated(0xdd, 50);
        checkHex(crypto::toHex(crypto::hmacSha256(k3.data(), k3.size(), d3.data(), d3.size())),
                 "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
                 "case 3");
        auto k6 = repeated(0xaa, 131);   // key longer than the block: hashed down first
        checkHex(crypto::toHex(crypto::hmacSha256(
                     k6.data(), k6.size(),
                     "Test Using Larger Than Block-Size Key - Hash Key First", 54)),
                 "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
                 "case 6 (131-byte key)");
        auto k7 = repeated(0xaa, 131);
        const char* m7 = "This is a test using a larger than block-size key and a larger "
                         "than block-size data. The key needs to be hashed before being "
                         "used by the HMAC algorithm.";
        checkHex(crypto::toHex(crypto::hmacSha256(k7.data(), k7.size(), m7, std::strlen(m7))),
                 "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
                 "case 7 (long key and long message)");
    }

    // ---- 3. PBKDF2-HMAC-SHA256, RFC 7914 §11 -------------------------------
    std::printf("PBKDF2-HMAC-SHA256 (RFC 7914)\n");
    {
        auto hex = [](const std::vector<uint8_t>& v) { return crypto::toHex(v); };
        checkHex(hex(crypto::pbkdf2Sha256("passwd", "salt", 4, 1, 64)),
                 "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                 "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
                 "c=1, dkLen=64");
        checkHex(hex(crypto::pbkdf2Sha256("Password", "NaCl", 4, 80000, 64)),
                 "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
                 "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d",
                 "c=80000, dkLen=64 (the slow one)");
        // RFC 6070's SHA-1 cases re-run for SHA-256; these are the values widely
        // published alongside it and are what most SCRAM implementations test.
        checkHex(hex(crypto::pbkdf2Sha256("password", "salt", 4, 1, 32)),
                 "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b",
                 "\"password\"/\"salt\" c=1");
        checkHex(hex(crypto::pbkdf2Sha256("password", "salt", 4, 4096, 32)),
                 "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a",
                 "\"password\"/\"salt\" c=4096");
        checkHex(hex(crypto::pbkdf2Sha256("passwordPASSWORDpassword",
                                          "saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, 40)),
                 "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1"
                 "c635518c7dac47e9",
                 "long password and salt, dkLen=40 (spans two output blocks)");
    }

    // ---- 4. helpers ---------------------------------------------------------
    std::printf("helpers\n");
    {
        uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 4}, c[4] = {1, 2, 3, 5};
        check(crypto::equalCT(a, b, 4), "equalCT: identical buffers compare equal");
        check(!crypto::equalCT(a, c, 4), "equalCT: a differing last byte compares unequal");
        crypto::Digest d{};
        check(crypto::fromHex(std::string(64, 'a'), d), "fromHex: 64 hex digits parse");
        check(!crypto::fromHex("abcd", d), "fromHex: a short string is refused");
        check(!crypto::fromHex(std::string(63, 'a') + "z", d),
              "fromHex: a non-hex digit is refused");
        std::vector<uint8_t> r1(32), r2(32);
        check(crypto::randomBytes(r1.data(), 32) && crypto::randomBytes(r2.data(), 32),
              "randomBytes: the OS CSPRNG answers");
        check(r1 != r2, "randomBytes: two draws differ");
        std::string secret = "hunter2hunter2";
        crypto::wipe(secret);
        check(secret.empty(), "wipe: clears the string");
    }

    // ---- 5. the SCRAM exchange, end to end ---------------------------------
    std::printf("SCRAM-SHA-256 login exchange\n");
    const std::string kUser = "curtis";
    const std::string kPass = "correct horse battery staple";
    const uint32_t kIters = 4096;          // the real one is 600k; keep the test quick
    std::vector<uint8_t> salt = bytes("0123456789abcdef");
    auth::Credential stored;
    {
        // REGISTRATION: the client derives everything; the password stays home.
        auth::Keys k = auth::deriveKeys(kPass, salt.data(), salt.size(), kIters);
        stored = auth::makeCredential(k, salt.data(), salt.size(), kIters);
        check(stored.storedKey == crypto::sha256(k.clientKey.data(), k.clientKey.size()),
              "registration: StoredKey is SHA256(ClientKey)");
    }
    {
        // A GOOD LOGIN.
        auto cnonce = bytes("client-nonce-0000000000000000000");
        auto snonce = bytes("server-nonce-1111111111111111111");
        auto am = auth::authMessage(kUser, cnonce, snonce, stored.salt, stored.iters);
        auth::Keys k = auth::deriveKeys(kPass, stored.salt.data(), stored.salt.size(),
                                        stored.iters);
        crypto::Digest proof = auth::clientProof(k, am);
        check(auth::verifyClientProof(stored, am, proof), "the right password verifies");
        crypto::Digest sig = auth::serverSignature(stored.serverKey, am);
        check(sig == auth::serverSignature(k.serverKey, am),
              "mutual auth: the client recomputes the server's signature");

        // A WRONG PASSWORD.
        auth::Keys bad = auth::deriveKeys("Correct horse battery staple", stored.salt.data(),
                                          stored.salt.size(), stored.iters);
        check(!auth::verifyClientProof(stored, am, auth::clientProof(bad, am)),
              "a one-character-different password is refused");

        // REPLAY: the same proof against a different server nonce.
        auto snonce2 = bytes("server-nonce-2222222222222222222");
        auto am2 = auth::authMessage(kUser, cnonce, snonce2, stored.salt, stored.iters);
        check(!auth::verifyClientProof(stored, am2, proof),
              "a captured proof does not replay against a fresh nonce");

        // TRANSCRIPT BINDING: same secrets, different username.
        auto amUser = auth::authMessage("curtis2", cnonce, snonce, stored.salt, stored.iters);
        check(!auth::verifyClientProof(stored, amUser, proof),
              "a proof is bound to the username it was made for");

        // DOWNGRADE: an attacker lowering the advertised work factor.
        auto amIters = auth::authMessage(kUser, cnonce, snonce, stored.salt, 1000);
        check(!auth::verifyClientProof(stored, amIters, proof),
              "a proof is bound to the iteration count (no work-factor downgrade)");

        // STOLEN ACCOUNTS FILE: the attacker has StoredKey and ServerKey and
        // tries to authenticate with them. This is the property that makes the
        // whole scheme worth the trouble.
        crypto::Digest forgedSig = crypto::hmacSha256(stored.storedKey, am);
        check(!auth::verifyClientProof(stored, am, forgedSig),
              "the stored verifier alone cannot produce a valid proof");
        crypto::Digest zeroProof{};
        check(!auth::verifyClientProof(stored, am, zeroProof), "an empty proof is refused");

        // A SERVER THAT DOES NOT KNOW THE ACCOUNT cannot fake the signature.
        crypto::Digest wrongSig = crypto::hmacSha256(crypto::Digest{}, am);
        check(!(wrongSig == sig), "a server without ServerKey cannot fake its signature");
    }

    // ---- 6. name and password rules ----------------------------------------
    std::printf("account name and password rules\n");
    {
        struct { const char* u; bool ok; const char* what; } kNames[] = {
            {"curtis", true, "a plain name"},
            {"Cur.tis_9-x", true, "dots, underscores and dashes"},
            {"ab", false, "too short"},
            {"abcdefghijklmnopqrstuvwxyz", false, "too long"},
            {"_curtis", false, "cannot start with punctuation"},
            {"cur tis", false, "no spaces"},
            {"curtis!", false, "no punctuation outside _ - ."},
            {"", false, "empty"},
        };
        for (auto& t : kNames)
            check(auth::validUsername(t.u) == t.ok, t.what, t.u);
        check(auth::foldUsername("CuRtIs") == "curtis", "names fold to lower case for lookup");

        check(auth::validPassword("password1"), "an 8+ character password is accepted");
        check(!auth::validPassword("short7c"), "a 7-character password is refused");
        check(!auth::validPassword(std::string(129, 'x')), "a 129-character password is refused");
        check(!auth::validPassword("bad\tpassword"), "a control character is refused");
    }

    // ---- 7. the account store ----------------------------------------------
    std::printf("account store\n");
    {
        const std::string path = dir + "/authtest-accounts.conf";
        std::remove(path.c_str());
        srv::AccountStore s;
        std::string err;
        check(s.load(path, &err), "a missing file loads as an empty store", err);
        check(s.size() == 0, "the empty store has no accounts");
        check(s.find("curtis") == nullptr, "an unknown name is not found");

        check(s.create("Curtis", stored, &err), "an account is created", err);
        check(s.find("curtis") != nullptr, "and is found case-insensitively");
        check(s.find("CURTIS")->name == "Curtis", "with the spelling its owner chose");
        check(!s.create("curtis", stored, &err), "a duplicate name is refused (case-folded)");
        check(!s.create("no", stored, &err), "an invalid name is refused");

        srv::AccountStore s2;
        check(s2.load(path, &err), "the file reloads", err);
        check(s2.size() == 1, "with the account still in it");
        const srv::Account* a = s2.find("curtis");
        check(a && a->cred.storedKey == stored.storedKey, "StoredKey survives the round trip");
        check(a && a->cred.serverKey == stored.serverKey, "ServerKey survives the round trip");
        check(a && a->cred.salt == stored.salt, "the salt survives the round trip");
        check(a && a->cred.iters == stored.iters, "the iteration count survives the round trip");

        // The file must not contain the password in any form.
        std::FILE* f = std::fopen(path.c_str(), "rb");
        std::string text;
        if (f) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        check(!text.empty(), "the accounts file has content");
        check(text.find(kPass) == std::string::npos, "the password is not in the file");
        check(text.find("horse") == std::string::npos, "no fragment of it either");

        check(s2.noteLogin("CURTIS", &err), "a login is recorded", err);
        srv::AccountStore s3;
        check(s3.load(path, &err) && s3.find("curtis")->logins == 2,
              "the login counter persists");
        std::remove(path.c_str());
    }

    // ---- 8. the brute-force throttle ---------------------------------------
    std::printf("login throttle\n");
    {
        using T = srv::LoginThrottle;
        srv::LoginThrottle t;
        uint64_t now = 1000;
        for (int i = 0; i < T::kAddress.freeAttempts; ++i) t.fail("ip:a", now, T::kAddress);
        check(t.lockedFor("ip:a", now) == 0, "an address gets 5 free failures");
        t.fail("ip:a", now, T::kAddress);
        check(t.lockedFor("ip:a", now) == T::kAddress.baseLockMs,
              "the 6th failure locks the address for 30s");
        t.fail("ip:a", now, T::kAddress);
        check(t.lockedFor("ip:a", now) == T::kAddress.baseLockMs * 2, "the 7th doubles it");
        for (int i = 0; i < 20; ++i) t.fail("ip:a", now, T::kAddress);
        check(t.lockedFor("ip:a", now) == T::kAddress.maxLockMs, "the lockout is capped");
        check(t.lockedFor("ip:b", now) == 0, "another address is unaffected");
        // The lockout expires on its own -- it is a delay, not a ban.
        check(t.lockedFor("ip:a", now + T::kAddress.maxLockMs) == 0,
              "the lockout lets go once it elapses");

        // An ACCOUNT is far more forgiving, so knowing someone's name is not
        // enough to hold them out of the game.
        srv::LoginThrottle u;
        for (int i = 0; i < T::kAddress.freeAttempts * 2; ++i) u.fail("user:bob", now, T::kAccount);
        check(u.lockedFor("user:bob", now) == 0,
              "10 failures do not lock an account (an address would be locked by now)");
        for (int i = 0; i < 41; ++i) u.fail("user:bob", now, T::kAccount);
        check(u.lockedFor("user:bob", now) > 0, "but sustained guessing does lock it");
        check(u.lockedFor("user:bob", now) <= T::kAccount.maxLockMs,
              "and an account lockout is short");
        u.succeed("user:bob");
        check(u.lockedFor("user:bob", now) == 0, "a successful login clears the account");

        // succeed() must never be handed an address key: one valid account would
        // otherwise wipe the guesser's own record between attempts at another.
        srv::LoginThrottle v;
        for (int i = 0; i < 6; ++i) v.fail("ip:c", now, T::kAddress);
        v.succeed("user:whoever");
        check(v.lockedFor("ip:c", now) > 0,
              "signing in to one account does not clear the address's record");

        srv::LoginThrottle w;
        w.fail("ip:d", now, T::kAddress);
        w.expire(now + T::kForgetMs + 1);
        check(w.size() == 0, "a quiet key is eventually forgotten");
        srv::LoginThrottle x;
        for (int i = 0; i < 6; ++i) x.fail("ip:e", now, T::kAddress);
        x.expire(now + 1);
        check(x.size() == 1, "a currently-locked key is NOT forgotten early");
    }

    // ---- 9. sign-in tokens ---------------------------------------------------
    std::printf("sign-in tokens\n");
    {
        using T = srv::SessionTokens;
        std::vector<uint8_t> salt2 = bytes("fedcba9876543210");
        srv::Account alice, bob;
        alice.name = "Alice";
        alice.cred = stored;
        bob.name = "Bob";
        bob.cred = auth::makeCredential(auth::deriveKeys("another password", salt2.data(),
                                                         salt2.size(), kIters),
                                        salt2.data(), salt2.size(), kIters);
        const uint64_t now = 1000;

        // The next token in a line, or nothing if `from` is refused.
        auto rotate = [&](srv::SessionTokens& s, const std::vector<uint8_t>& from, uint64_t when) {
            T::Chain chain;
            return s.spend(alice, from, when, &chain) ? s.issue(alice, when, &chain)
                                                      : std::vector<uint8_t>{};
        };

        srv::SessionTokens t;
        std::vector<uint8_t> tok = t.issue(alice, now), other = t.issue(alice, now);
        check(tok.size() == auth::kTokenLen && tok != other, "tokens are long and random");
        check(!t.spend(bob, tok, now), "a token is no good for another account");
        srv::Account bobWithHerKeys = alice;
        bobWithHerKeys.name = "Bob";
        check(!t.spend(bobWithHerKeys, tok, now), "nor for another name on the same keys");
        check(t.spend(alice, tok, now), "the right account can use it");
        check(!t.spend(alice, tok, now + T::kRetryMs), "only once: a spent token is refused");
        check(t.spend(alice, other, now), "and spending one leaves the account's others alone");

        // The hash is bound to the name as folded, like every other lookup.
        srv::Account lower = alice;
        lower.name = "ALICE";
        std::vector<uint8_t> folded = t.issue(alice, now);
        check(t.spend(lower, folded, now), "the account name matches case-insensitively");

        // The account's credential is what keys the hash: re-created, it starts clean.
        std::vector<uint8_t> old = t.issue(alice, now);
        srv::Account remade = alice;
        remade.cred = bob.cred;
        check(!t.spend(remade, old, now), "a token does not survive its credential");
        check(t.spend(alice, old, now), "(the failed try above did not burn it)");

        std::vector<uint8_t> e1 = t.issue(alice, now), e2 = t.issue(alice, now);
        check(t.spend(alice, e1, now + T::kLifetimeMs - 1), "a token is good for 12 hours");
        check(!t.spend(alice, e2, now + T::kLifetimeMs), "and expired after that");

        // Rotation never adds to the 12 hours.
        {
            srv::SessionTokens s;
            auto a = s.issue(alice, now);
            auto b = rotate(s, a, now + T::kLifetimeMs - 2);
            auto c = rotate(s, b, now + T::kLifetimeMs - 1);
            check(!c.empty(), "a line rotates for its 12 hours");
            check(!s.spend(alice, c, now + T::kLifetimeMs), "and ends 12 hours after the password");
            check(s.size() == 0, "and nothing is left of it");
        }
        // A token used after its successor has been: one of the two is a thief.
        {
            srv::SessionTokens s;
            auto a = s.issue(alice, now), mine = s.issue(alice, now);
            auto b = rotate(s, a, now);
            auto c = rotate(s, b, now + 1);
            check(!s.spend(alice, a, now + 2), "a token used after its successor is refused");
            check(!s.spend(alice, c, now + 2), "and ends the whole line");
            check(s.spend(alice, mine, now + 2), "but not the account's other logins");
        }
        // Sent again when nothing followed it, but too late to be a lost reply.
        {
            srv::SessionTokens s;
            auto a = s.issue(alice, now);
            auto b = rotate(s, a, now);
            check(!s.spend(alice, a, now + T::kRetryMs), "a late second use is refused");
            check(!s.spend(alice, b, now + T::kRetryMs), "and ends the line too");
        }
        // The link took the reply: the only thing left to send is the spent token.
        {
            srv::SessionTokens s;
            auto a = s.issue(alice, now);
            auto lost = rotate(s, a, now);
            auto lost2 = rotate(s, a, now + 1);
            auto kept = rotate(s, a, now + 2);
            check(!lost2.empty() && !kept.empty() && kept != lost2 && kept != lost,
                  "a token whose reply was lost can be sent again, more than once");
            check(!s.spend(alice, lost, now + 3), "what the lost reply carried is dead");
            check(!s.spend(alice, kept, now + 3), "and whoever shows it up ends the line");

            auto d = s.issue(alice, now);
            auto e = rotate(s, d, now);
            auto f = rotate(s, d, now + 1);
            auto g = rotate(s, f, now + 2);
            check(!e.empty() && !g.empty(), "a resend's token carries the line on");
            check(!s.spend(alice, d, now + 3), "and the resend is over once that is used");
            check(!s.spend(alice, g, now + 3), "(the line ends)");
        }
        // A retry never buys time either.
        {
            srv::SessionTokens s;
            auto a = s.issue(alice, now);
            T::Chain c1, c2;
            check(s.spend(alice, a, now + T::kLifetimeMs - 2, &c1) &&
                  s.spend(alice, a, now + T::kLifetimeMs - 1, &c2) &&
                  c1.id == c2.id && c2.expiresMs == now + T::kLifetimeMs,
                  "a resend stays on the login's clock");
            check(!s.spend(alice, a, now + T::kLifetimeMs), "and stops with it");
        }
        // Quitting ends the line, whichever of its tokens the client still has.
        {
            srv::SessionTokens s;
            auto a = s.issue(alice, now);
            auto b = rotate(s, a, now);
            check(s.revoke(alice, a), "a spent token can still sign its line out");
            check(!s.spend(alice, b, now), "and what followed it goes too");
            check(!s.revoke(alice, a), "after which it is unknown");
            check(!s.revoke(bob, s.issue(alice, now)), "a token cannot revoke another account's");
            check(s.size() == 1, "revoking leaves nothing but the other line");
        }

        {
            srv::SessionTokens s;
            std::vector<uint8_t> first = s.issue(alice, now);
            for (size_t i = 1; i < T::kPerAccount; ++i) s.issue(alice, now);
            std::vector<uint8_t> newest = s.issue(alice, now);
            check(s.size() == T::kPerAccount, "an account holds a bounded number of tokens");
            check(!s.spend(alice, first, now), "the oldest goes first");
            check(s.spend(alice, newest, now), "the newest stays");

            srv::SessionTokens r;
            auto line = r.issue(alice, now);
            for (size_t i = 0; i < 2 * T::kSpentPerAccount; ++i) line = rotate(r, line, now);
            check(!line.empty() && r.size() == T::kSpentPerAccount,
                  "and a bounded number of spent hashes");
        }

        srv::SessionTokens u;
        auto swept = u.issue(alice, now);
        check(u.spend(alice, swept, now), "(spent, not yet expired)");
        u.issue(bob, now + T::kLifetimeMs / 2);
        u.expire(now + T::kLifetimeMs);
        check(u.size() == 1, "expired hashes are swept, spent ones too, live ones kept");
        check(!t.spend(alice, std::vector<uint8_t>(auth::kTokenLen - 1, 1), now),
              "a short token is refused");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
