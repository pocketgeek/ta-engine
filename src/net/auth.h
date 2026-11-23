#pragma once

// Account login for multiplayer: binary SCRAM-SHA-256.
//
// WHY SCRAM. The requirements are that the password never travels in the clear
// and the server never stores it. A naive "hash the password and send the hash"
// meets neither in practice: the hash IS the password on the wire (replay it and
// you are in), and the stored copy is then a password-equivalent secret, so one
// leaked config file logs an attacker into every account.
//
// SCRAM (RFC 5802, SHA-256 variant RFC 7677) is the standard answer and gives us:
//
//   * The password is never transmitted, in any form. What crosses the wire is a
//     proof computed over a fresh server nonce, so capturing it replays nowhere.
//   * The server stores StoredKey = SHA256(ClientKey) and ServerKey. Proving you
//     know the password requires ClientKey, and recovering ClientKey from
//     StoredKey is a SHA-256 preimage. Verifiers still permit offline guessing
//     and server impersonation; combining them with a captured login proof
//     exposes the ClientKey. Protect the account file and use verified TLS.
//   * Mutual authentication: the server returns a signature only someone holding
//     ServerKey can produce, so a machine that impersonates the server cannot
//     convince the client it knows the account.
//
// We speak it over our own binary framing rather than SASL's base64 text, since
// the protocol is already binary and the encoding carries no security.
//
// TRANSPORT. SCRAM alone does not encrypt or authenticate subsequent game
// messages. Native TLS in Conn protects public sessions, including first
// registration. Explicit plaintext/LAN connections retain those limitations;
// login proofs are not a substitute for TLS against interception or relaying.
//
// THE SIGN-IN TOKEN. The password is wiped straight after a login, so coming back
// to the game list after a result screen would mean typing it again. A password
// login on an ENCRYPTED connection therefore also earns a long random token (the
// tail of AuthResult), which the client holds in memory and spends once, in
// Msg::AuthToken in place of AuthBegin, for a sign-in with no password prompt. Every
// use earns the next token, in a line that ends 12 hours after the password login
// however often it rotates. A token used a second time ends its whole line, because
// one of the two users is a thief; the one exception is a resend straight after, when
// the link took the reply with the next token (kRetryMs in src/server/accounts.h).
// Unlike a SCRAM proof the token IS the credential on the wire and replays until
// spent, so it exists on TLS only: a plaintext server never issues or accepts one,
// and those players keep the password prompt. The client sends it only to the address
// it came from, and quitting revokes every token it still holds (Msg::AuthRevoke, best
// effort). The server keeps tokenHash() per account, in memory (SessionTokens in
// src/server/accounts.h), so a restart invalidates them.
// Whatever goes wrong with one, the player just gets the normal password prompt.
//
// Unknown usernames are reported as unknown, by design -- the client has to be
// told in order to offer to create the account. That does let someone probe
// which usernames exist; it is the explicit cost of self-service registration.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "net/crypto.h"

namespace tak::auth {

// PBKDF2 work factor. The CLIENT pays this on each login (the server does zero
// PBKDF2 work, which is one of SCRAM's nice properties -- a login flood cannot
// be turned into a server-side CPU attack). 600k is the current OWASP figure for
// PBKDF2-HMAC-SHA256; it costs a few hundred milliseconds once, at a screen the
// player expects to wait at. Stored per account so it can be raised later
// without invalidating existing credentials.
constexpr uint32_t kPbkdf2Iters = 600000;

constexpr size_t kNonceLen = 32;      // per-login randomness, both directions
constexpr size_t kSaltLen = 16;       // per-account, server-chosen at registration
constexpr size_t kTokenLen = 32;      // a sign-in token (see above)
constexpr uint64_t kTokenLifetimeMs = 12 * 60 * 60 * 1000;   // from the password login
constexpr size_t kMinPassword = 8;    // new accounts only; login accepts any length
constexpr size_t kMaxPassword = 128;
constexpr size_t kMinUsername = 3;
constexpr size_t kMaxUsername = 20;

// The server's stored record for one account. This is exactly what lands in the
// accounts file -- note that the password is not recoverable from any of it.
struct Credential {
    uint32_t iters = kPbkdf2Iters;
    std::vector<uint8_t> salt;
    crypto::Digest storedKey{};   // SHA256(ClientKey) -- verifies the client
    crypto::Digest serverKey{};   // proves the server knows the account
};

// Everything the client derives from the password. Lives for one login and is
// wiped afterwards.
struct Keys {
    crypto::Digest clientKey{};
    crypto::Digest storedKey{};
    crypto::Digest serverKey{};
};

// The slow step: SaltedPassword = PBKDF2(password, salt, iters), then the two
// keys. Takes a few hundred milliseconds at kPbkdf2Iters -- call it off the
// render thread.
Keys deriveKeys(std::string_view password, const void* salt, size_t saltLen, uint32_t iters);

// The credential to REGISTER for a brand-new account, derived client-side so the
// password never leaves the machine.
Credential makeCredential(const Keys& k, const void* salt, size_t saltLen, uint32_t iters);

// The canonical transcript both sides sign. Binding the proof to the username,
// both nonces, the salt and the iteration count means a proof captured from one
// login cannot be replayed into another, nor into a downgraded one with a weaker
// work factor. Every field is length-prefixed so no two different exchanges can
// serialise to the same bytes.
std::vector<uint8_t> authMessage(std::string_view user,
                                 const std::vector<uint8_t>& clientNonce,
                                 const std::vector<uint8_t>& serverNonce,
                                 const std::vector<uint8_t>& salt, uint32_t iters);

// ClientProof = ClientKey XOR HMAC(StoredKey, authMessage). The server recovers
// ClientKey from it and checks SHA256(ClientKey) against what it stored.
crypto::Digest clientProof(const Keys& k, const std::vector<uint8_t>& am);

// Server side of the above. Constant-time.
[[nodiscard]] bool verifyClientProof(const Credential& c, const std::vector<uint8_t>& am,
                                     const crypto::Digest& proof);

// ServerSignature = HMAC(ServerKey, authMessage): the server's half of mutual
// auth. The client checks it against the ServerKey it derived from the password.
crypto::Digest serverSignature(const crypto::Digest& serverKey, const std::vector<uint8_t>& am);

// What the server keeps of a sign-in token. Keyed by the account's StoredKey and
// bound to its name, so a token is good for that account and credential only.
crypto::Digest tokenHash(const Credential& c, std::string_view user,
                         const std::vector<uint8_t>& token);

// ---- account naming --------------------------------------------------------

// Usernames are 3-20 characters of ASCII letters, digits, '_', '-' or '.',
// starting with a letter or digit. Narrow on purpose: the name is also the
// player's display name in lobbies and chat, it goes in log lines and the
// accounts file, and homoglyph games with unicode would let one player pose as
// another. `why` (optional) receives a message fit to show the player.
[[nodiscard]] bool validUsername(std::string_view u, std::string* why = nullptr);

// Rejects new passwords that are too short, too long, or contain control
// characters. LOGIN does not apply this -- it checks only the stored proof, so a
// rule change never locks out an existing account.
[[nodiscard]] bool validPassword(std::string_view p, std::string* why = nullptr);

// Lookup key: usernames are unique case-insensitively, so "Curtis" cannot be
// registered alongside "curtis", but the spelling the owner chose is what gets
// displayed.
std::string foldUsername(std::string_view u);

}  // namespace tak::auth
