#pragma once

// The server's account file, and the brute-force throttle that guards it.
//
// Accounts live in one plain-text config file -- no database. It holds, per
// account, a random salt and the two SCRAM verifiers; the password itself is not
// in it and cannot be derived from it (see `src/net/auth.h`). The file is still
// sensitive, because it is exactly what an offline guessing attack would run
// against, so it is written 0600 and never logged.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "net/auth.h"

namespace tak::srv {

struct Account {
    std::string name;              // as its owner spelled it; the display name in game
    auth::Credential cred;
    int64_t createdUnix = 0;
    int64_t lastLoginUnix = 0;
    uint64_t logins = 0;
};

// Loads, queries and rewrites the account file. Small by design: a server with
// thousands of accounts is still a few hundred KB of text, and rewriting the
// whole file on each change keeps it trivially consistent.
class AccountStore {
public:
    // Reads `path`. A MISSING file is success with an empty store -- that is a
    // brand-new server, and the first player to sign in creates it. A file that
    // exists but cannot be read or parsed is a failure: refusing to start beats
    // silently running with no accounts and letting anyone claim any name.
    bool load(const std::string& path, std::string* err);

    // Rewrites the file atomically (temp file + rename) so an interrupted save
    // cannot truncate the accounts of everyone who already registered.
    bool save(std::string* err) const;

    // Case-insensitive lookup. Returns nullptr if there is no such account.
    const Account* find(std::string_view user) const;

    // Registers a new account and saves. Fails if the name is taken (case-
    // insensitively) or invalid.
    bool create(const std::string& user, const auth::Credential& cred, std::string* err);

    // Records a successful login and saves. Best-effort: a save failure here is
    // logged by the caller but must not fail the login the player just made.
    bool noteLogin(std::string_view user, std::string* err);

    size_t size() const { return byFold_.size(); }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::map<std::string, Account> byFold_;   // folded username -> account
};

// The sign-in tokens this server has issued (see src/net/auth.h). Only their hashes,
// and only in memory: a restart invalidates them all, which just means a password
// prompt. Per account, because a player may be signed in on two machines.
class SessionTokens {
public:
    static constexpr uint64_t kLifetimeMs = auth::kTokenLifetimeMs;
    // How long a spent token may be sent again. The link can take the reply with the
    // next token, and the client then has only this one to retry with. The server's
    // disconnect wait (kDropWaitMs) is as long as an in-game reconnect keeps trying.
    static constexpr uint64_t kRetryMs = 30 * 1000;
    static constexpr size_t kPerAccount = 8;   // live tokens; oldest goes first
    // Spent hashes kept to recognise a second use. A line rotated further than this
    // forgets its oldest: those are then just refused, and kLifetimeMs still ends it.
    static constexpr size_t kSpentPerAccount = 32;

    // One password login's line of tokens, which rotation carries on.
    struct Chain {
        uint64_t id = 0;
        uint64_t expiresMs = 0;   // the login's, however often it has rotated
    };

    // A new random token for `a`: the start of a line, or with `rotated`, the next in
    // that one. Throws std::runtime_error if the OS refuses entropy.
    std::vector<uint8_t> issue(const Account& a, uint64_t nowMs, const Chain* rotated = nullptr);

    // Spends a token: true if it was good for `a`, with its line in `chain` for the
    // next issue(). A spent token sent again ends its whole line and is refused, unless
    // it is a resend within kRetryMs and nothing later in the line has been used: then
    // whatever followed it dies instead, so the reply that carried that was lost, and
    // whoever turns up with it ends the line. Compares in constant time, against every
    // hash the account holds.
    [[nodiscard]] bool spend(const Account& a, const std::vector<uint8_t>& token, uint64_t nowMs,
                             Chain* chain = nullptr);

    // Ends the line of a token, spent or not: true if it was one of `a`'s.
    [[nodiscard]] bool revoke(const Account& a, const std::vector<uint8_t>& token);

    // Drops the hashes of expired tokens. Cheap to call often: it sweeps once a minute.
    void expire(uint64_t nowMs);

    size_t size() const;

private:
    struct Entry {
        crypto::Digest hash{};
        uint64_t chain = 0;
        uint64_t expiresMs = 0;
        bool spent = false;
        uint64_t retryUntilMs = 0;   // 0 on a spent token no client can have: never retried
    };
    static size_t find(const std::vector<Entry>& held, const crypto::Digest& want);
    static void drop(std::vector<Entry>& held, uint64_t chain);
    std::map<std::string, std::vector<Entry>> byFold_;   // folded username, oldest first
    uint64_t nextChain_ = 0;
    uint64_t nextSweepMs_ = 0;
};

// Brute-force throttle. Keyed by whatever the caller considers an identity --
// the server feeds it the peer address AND the account name, so neither many
// accounts from one host nor one account from many hosts gets a free run.
// Purely in-memory: a restart forgives everyone, which is the right trade for a
// game server.
class LoginThrottle {
public:
    // How hard to clamp down, per kind of key. These differ on purpose.
    struct Policy {
        int freeAttempts;        // failures tolerated before locking at all
        uint64_t baseLockMs;     // first lockout, doubling per further failure
        uint64_t maxLockMs;      // ... up to this
    };
    // An ADDRESS is the thing actually doing the guessing, so it is clamped hard.
    static constexpr Policy kAddress{5, 30 * 1000, 15 * 60 * 1000};
    // An ACCOUNT is deliberately far more forgiving. A strict per-account lockout
    // is a weapon: anyone who knows a name can hold its owner out of the game by
    // failing five logins an hour. The account limit exists only to blunt guessing
    // spread across many addresses, so it takes real persistence to trip and lets
    // go quickly; the address limit is what stops an ordinary attacker.
    static constexpr Policy kAccount{50, 60 * 1000, 5 * 60 * 1000};

    // A key that behaves for this long is forgotten entirely.
    static constexpr uint64_t kForgetMs = 60 * 60 * 1000;

    // Milliseconds the caller must refuse for, or 0 if the attempt may proceed.
    uint64_t lockedFor(const std::string& key, uint64_t nowMs) const;

    void fail(const std::string& key, uint64_t nowMs, const Policy& p);
    // Clear a key's record. Call this ONLY for account-scoped keys: clearing the
    // address on success would let anyone holding one valid account wipe their
    // own failure history between guesses at somebody else's.
    void succeed(const std::string& key);

    // Drops entries that have been quiet for kForgetMs, so a long-lived server
    // does not accumulate one entry per address that ever mistyped a password.
    void expire(uint64_t nowMs);

    size_t size() const { return keys_.size(); }

private:
    struct Entry {
        int fails = 0;
        uint64_t lastMs = 0;
        uint64_t lockedUntilMs = 0;
    };
    std::map<std::string, Entry> keys_;
};

}  // namespace tak::srv
