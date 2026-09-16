#include "server/accounts.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <ctime>
#include <fstream>
#include <sstream>

#ifndef _WIN32
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace ta::srv {

namespace {

int64_t nowUnix() { return int64_t(std::time(nullptr)); }

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return std::string(s.substr(a, b - a));
}

constexpr const char* kHeader =
    "# TA:Kingdoms engine -- multiplayer accounts.\n"
    "#\n"
    "# There are no passwords in this file and none can be recovered from it. Each\n"
    "# account stores a random salt and two SHA-256 verifiers (SCRAM-SHA-256); a\n"
    "# password is only ever hashed on the player's own machine. Verifying a login\n"
    "# needs these values, but they cannot be used to PERFORM one.\n"
    "#\n"
    "# Treat the file as sensitive anyway: it is what an offline password-guessing\n"
    "# attack would run against. It is written owner-read-only, and the server\n"
    "# rewrites it whole whenever an account changes -- edit it only while the\n"
    "# server is stopped.\n"
    "\n"
    "version = 1\n";

}  // namespace

// ---- AccountStore ----------------------------------------------------------

bool AccountStore::load(const std::string& path, std::string* err) {
    path_ = path;
    byFold_.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // No file yet: a fresh server. Not an error -- the first registration
        // creates it. (A file that exists but is unreadable fails below, because
        // ifstream only fails to open here when it is absent or denied, and we
        // distinguish those.)
#ifndef _WIN32
        if (::access(path.c_str(), F_OK) == 0) {
            if (err) *err = "cannot read accounts file '" + path + "': " + std::strerror(errno);
            return false;
        }
#endif
        return true;
    }

    std::string line;
    int lineNo = 0;
    Account cur;
    bool inBlock = false;
    auto flush = [&](std::string* e) -> bool {
        if (!inBlock) return true;
        inBlock = false;
        if (cur.name.empty()) {
            if (e) *e = "accounts file '" + path + "': an [account] block has no name";
            return false;
        }
        if (cur.cred.salt.empty()) {
            if (e) *e = "accounts file '" + path + "': account '" + cur.name + "' has no salt";
            return false;
        }
        byFold_[auth::foldUsername(cur.name)] = cur;
        cur = Account{};
        return true;
    };

    while (std::getline(in, line)) {
        ++lineNo;
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        if (s.front() == '[') {
            if (!flush(err)) return false;
            if (s == "[account]") { inBlock = true; cur = Account{}; }
            // Unknown sections are skipped, so a future server can add its own
            // without an older build refusing to start.
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(std::string_view(s).substr(0, eq));
        std::string val = trim(std::string_view(s).substr(eq + 1));
        for (char& c : key) c = char(std::tolower((unsigned char)c));
        if (!inBlock) continue;             // top-level keys (version) are informational

        auto badHex = [&](const char* what) {
            if (err) *err = "accounts file '" + path + "' line " + std::to_string(lineNo) +
                            ": malformed " + what;
            return false;
        };
        if (key == "name") {
            cur.name = val;
        } else if (key == "iters") {
            cur.cred.iters = uint32_t(std::strtoul(val.c_str(), nullptr, 10));
            if (cur.cred.iters == 0) cur.cred.iters = auth::kPbkdf2Iters;
        } else if (key == "salt") {
            if (val.size() % 2 || val.empty()) return badHex("salt");
            cur.cred.salt.resize(val.size() / 2);
            if (!crypto::fromHex(val, cur.cred.salt.data(), cur.cred.salt.size()))
                return badHex("salt");
        } else if (key == "storedkey") {
            if (!crypto::fromHex(val, cur.cred.storedKey)) return badHex("storedkey");
        } else if (key == "serverkey") {
            if (!crypto::fromHex(val, cur.cred.serverKey)) return badHex("serverkey");
        } else if (key == "created") {
            cur.createdUnix = std::strtoll(val.c_str(), nullptr, 10);
        } else if (key == "lastlogin") {
            cur.lastLoginUnix = std::strtoll(val.c_str(), nullptr, 10);
        } else if (key == "logins") {
            cur.logins = std::strtoull(val.c_str(), nullptr, 10);
        }
        // Unknown keys inside a block are ignored for the same forward-compat
        // reason as unknown sections.
    }
    // getline stops on EOF *or* on a read error, and the loop above cannot tell
    // them apart. A mid-file I/O error would otherwise look like a short but
    // valid file -- the server would come up having silently forgotten every
    // account after the bad byte, and hand those names to whoever asks for them
    // next. Refuse instead.
    if (in.bad()) {
        if (err) *err = "error reading accounts file '" + path + "': " + std::strerror(errno);
        byFold_.clear();
        return false;
    }
    return flush(err);
}

bool AccountStore::save(std::string* err) const {
    if (path_.empty()) { if (err) *err = "no accounts file path set"; return false; }
    const std::string tmp = path_ + ".tmp";
    {
        // Create the temp file OWNER-ONLY FROM THE START. Writing it with the
        // default mask and chmod'ing afterwards leaves the verifiers readable by
        // every account on the box for the duration of the write -- short, but a
        // window that need not exist at all.
        std::string text = kHeader;
        for (const auto& [fold, a] : byFold_) {
            text += "\n[account]\n";
            text += "name = " + a.name + "\n";
            text += "iters = " + std::to_string(a.cred.iters) + "\n";
            text += "salt = " + crypto::toHex(a.cred.salt.data(), a.cred.salt.size()) + "\n";
            text += "storedkey = " + crypto::toHex(a.cred.storedKey) + "\n";
            text += "serverkey = " + crypto::toHex(a.cred.serverKey) + "\n";
            text += "created = " + std::to_string(a.createdUnix) + "\n";
            text += "lastlogin = " + std::to_string(a.lastLoginUnix) + "\n";
            text += "logins = " + std::to_string(a.logins) + "\n";
        }
#ifndef _WIN32
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            if (err) *err = "cannot write '" + tmp + "': " + std::strerror(errno);
            return false;
        }
        // An existing temp file keeps its old mode, so state it explicitly too.
        if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
            if (err) *err = "cannot secure '" + tmp + "': " + std::strerror(errno);
            ::close(fd); std::remove(tmp.c_str());
            return false;
        }
        size_t off = 0;
        while (off < text.size()) {
            ssize_t n = ::write(fd, text.data() + off, text.size() - off);
            if (n <= 0) {
                if (errno == EINTR) continue;
                if (err) *err = "error writing '" + tmp + "': " + std::strerror(errno);
                ::close(fd); std::remove(tmp.c_str());
                return false;
            }
            off += size_t(n);
        }
        // Get the bytes on the platter BEFORE the rename. Without this a crash
        // can leave the rename durable and its contents not -- an empty accounts
        // file where everyone's credentials used to be.
        if (::fsync(fd) != 0 || ::close(fd) != 0) {
            if (err) *err = "error flushing '" + tmp + "': " + std::strerror(errno);
            std::remove(tmp.c_str());
            return false;
        }
#else
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err) *err = "cannot write '" + tmp + "': " + std::strerror(errno);
            return false;
        }
        out << text;
        out.flush();
        if (!out) {
            if (err) *err = "error writing '" + tmp + "': " + std::strerror(errno);
            out.close(); std::remove(tmp.c_str());
            return false;
        }
#endif
    }
#ifdef _WIN32
    // Windows rename() refuses an existing target, so the old file has to move
    // aside first -- but DELETING it was wrong: if the rename then failed we
    // destroyed the accounts and the replacement both. Move it to a backup we can
    // put back, and only drop the backup once the new file is in place.
    const std::string bak = path_ + ".bak";
    std::remove(bak.c_str());
    bool hadOld = std::rename(path_.c_str(), bak.c_str()) == 0;
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        if (err) *err = "cannot replace '" + path_ + "': " + std::strerror(errno);
        std::remove(tmp.c_str());
        if (hadOld) std::rename(bak.c_str(), path_.c_str());   // put it back
        return false;
    }
    if (hadOld) std::remove(bak.c_str());
    return true;
#else
    // POSIX rename() replaces atomically: a concurrent reader sees either the
    // whole old file or the whole new one, never a truncated mix.
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        if (err) *err = "cannot replace '" + path_ + "': " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
#endif
}

const Account* AccountStore::find(std::string_view user) const {
    auto it = byFold_.find(auth::foldUsername(user));
    return it == byFold_.end() ? nullptr : &it->second;
}

bool AccountStore::create(const std::string& user, const auth::Credential& cred,
                          std::string* err) {
    std::string why;
    if (!auth::validUsername(user, &why)) { if (err) *err = why; return false; }
    const std::string fold = auth::foldUsername(user);
    if (byFold_.count(fold)) { if (err) *err = "that name is already taken"; return false; }
    if (cred.salt.empty()) { if (err) *err = "missing credential salt"; return false; }

    Account a;
    a.name = user;
    a.cred = cred;
    a.createdUnix = nowUnix();
    a.lastLoginUnix = a.createdUnix;
    a.logins = 1;
    byFold_[fold] = a;
    if (!save(err)) {
        byFold_.erase(fold);   // don't pretend an unsaved account exists
        return false;
    }
    return true;
}

bool AccountStore::noteLogin(std::string_view user, std::string* err) {
    auto it = byFold_.find(auth::foldUsername(user));
    if (it == byFold_.end()) { if (err) *err = "no such account"; return false; }
    it->second.lastLoginUnix = nowUnix();
    ++it->second.logins;
    return save(err);
}

// ---- LoginThrottle ---------------------------------------------------------

uint64_t LoginThrottle::lockedFor(const std::string& key, uint64_t nowMs) const {
    auto it = keys_.find(key);
    if (it == keys_.end()) return 0;
    return nowMs < it->second.lockedUntilMs ? it->second.lockedUntilMs - nowMs : 0;
}

void LoginThrottle::fail(const std::string& key, uint64_t nowMs, const Policy& p) {
    Entry& e = keys_[key];
    if (e.lastMs && nowMs - e.lastMs > kForgetMs) e.fails = 0;   // long-quiet: start over
    ++e.fails;
    e.lastMs = nowMs;
    if (e.fails > p.freeAttempts) {
        // base, 2x, 4x, ... capped. Shift by the count PAST the free attempts.
        int over = e.fails - p.freeAttempts - 1;
        uint64_t lock = p.baseLockMs;
        for (int i = 0; i < over && lock < p.maxLockMs; ++i) lock *= 2;
        if (lock > p.maxLockMs) lock = p.maxLockMs;
        e.lockedUntilMs = nowMs + lock;
    }
}

void LoginThrottle::succeed(const std::string& key) { keys_.erase(key); }

void LoginThrottle::expire(uint64_t nowMs) {
    for (auto it = keys_.begin(); it != keys_.end();) {
        const Entry& e = it->second;
        bool quiet = e.lastMs + kForgetMs < nowMs;
        bool unlocked = nowMs >= e.lockedUntilMs;
        it = (quiet && unlocked) ? keys_.erase(it) : std::next(it);
    }
}

}  // namespace ta::srv
