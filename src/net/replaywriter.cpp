#include "net/replaywriter.h"

#include "net/replayhdr.h"  // kMaxReplayBytes: the loader's size cap

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "util/u8path.h"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/file.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace tak::net {
namespace {

constexpr const char* kPartSuffix = ".part";       // <name>.takrep.part
constexpr const char* kChecksSuffix = ".chk";      // <name>.takrep.part.chk

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::filesystem::path fsPath(const std::string& utf8) { return tak::u8path(utf8); }

bool exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(fsPath(path), ec);
}

void removeFile(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(fsPath(path), ec);
}

void putU32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
uint32_t getU32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i);
    return v;
}
void putCheck(std::vector<uint8_t>& out, const ReplayCheck& c) {
    Writer w;
    w.u32(c.tick); w.u64(c.hash); w.u64(c.posDigest);
    out.insert(out.end(), w.b.begin(), w.b.end());
}

// ---- the platform file layer ---------------------------------------------------

enum class Opened { Ok, Busy, Missing, Failed };

#ifdef _WIN32
HANDLE handle(intptr_t f) { return reinterpret_cast<HANDLE>(f); }

// Shared for READING only: no other handle may write the file, rename it or delete
// it while this one is open. That is the Windows half of the recording lock.
intptr_t createLocked(const std::string& path) {
    HANDLE h = CreateFileW(fsPath(path).wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? -1 : reinterpret_cast<intptr_t>(h);
}

Opened openLocked(const std::string& path, intptr_t& out) {
    HANDLE h = CreateFileW(fsPath(path).wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) return Opened::Busy;
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return Opened::Missing;
        return Opened::Failed;
    }
    out = reinterpret_cast<intptr_t>(h);
    return Opened::Ok;
}

bool writeAt(intptr_t f, uint64_t at, const uint8_t* p, size_t n) {
    while (n > 0) {
        OVERLAPPED ov{};
        ov.Offset = DWORD(at & 0xFFFFFFFFu);
        ov.OffsetHigh = DWORD(at >> 32);
        const DWORD chunk = DWORD(std::min<size_t>(n, 1u << 30));
        DWORD done = 0;
        if (!WriteFile(handle(f), p, chunk, &done, &ov) || done == 0) return false;
        p += done; n -= done; at += done;
    }
    return true;
}

bool readAll(intptr_t f, std::vector<uint8_t>& out, uint64_t cap) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle(f), &size) || size.QuadPart < 0 || uint64_t(size.QuadPart) > cap) return false;
    out.resize(size_t(size.QuadPart));
    uint64_t at = 0;
    while (at < out.size()) {
        OVERLAPPED ov{};
        ov.Offset = DWORD(at & 0xFFFFFFFFu);
        ov.OffsetHigh = DWORD(at >> 32);
        DWORD done = 0;
        const DWORD chunk = DWORD(std::min<uint64_t>(out.size() - at, 1u << 30));
        if (!ReadFile(handle(f), out.data() + at, chunk, &done, &ov) || done == 0) return false;
        at += done;
    }
    return true;
}

bool truncateTo(intptr_t f, uint64_t size) {
    LARGE_INTEGER at{};
    at.QuadPart = LONGLONG(size);
    return SetFilePointerEx(handle(f), at, nullptr, FILE_BEGIN) && SetEndOfFile(handle(f));
}

bool syncFile(intptr_t f) { return FlushFileBuffers(handle(f)) != 0; }
void closeFile(intptr_t f) { CloseHandle(handle(f)); }

// A Windows file cannot be renamed while this process holds it unshared, so the
// caller closes it first. MOVEFILE_WRITE_THROUGH returns once the rename is on disk;
// without MOVEFILE_REPLACE_EXISTING it refuses to overwrite. A virus scanner or the
// search indexer that opened the just-closed file without FILE_SHARE_DELETE makes
// the rename fail for a moment, so a sharing failure is retried briefly; should it
// persist, the .part stays and the next recovery pass finishes it.
bool renameClosed(const std::string& from, const std::string& to) {
    const std::wstring f = fsPath(from).wstring(), t = fsPath(to).wstring();
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(f.c_str(), t.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        const DWORD e = GetLastError();
        if (attempt >= 5 || (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION))
            return false;
        Sleep(100);
    }
}
#else
intptr_t createLocked(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    // flock, not fcntl: an flock lock belongs to the open file description, so it
    // excludes a second open in THIS process too (the replay picker's recovery pass).
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); return -1; }
    return fd;
}

Opened openLocked(const std::string& path, intptr_t& out) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? Opened::Missing : Opened::Failed;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        ::close(fd);
        return e == EWOULDBLOCK ? Opened::Busy : Opened::Failed;
    }
    // The writer renames its file while holding the lock and releases it after, so a
    // lock won after a wait may belong to a file that has since been finished under
    // its final name. Only the file still at `path` is a recording to recover.
    struct stat a{}, b{};
    if (::fstat(fd, &a) != 0 || ::stat(path.c_str(), &b) != 0 || a.st_ino != b.st_ino || a.st_dev != b.st_dev) {
        ::close(fd);
        return Opened::Missing;
    }
    out = fd;
    return Opened::Ok;
}

bool writeAt(intptr_t f, uint64_t at, const uint8_t* p, size_t n) {
    while (n > 0) {
        const ssize_t done = ::pwrite(int(f), p, n, off_t(at));
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return false;
        p += done; n -= size_t(done); at += uint64_t(done);
    }
    return true;
}

bool readAll(intptr_t f, std::vector<uint8_t>& out, uint64_t cap) {
    struct stat st{};
    if (::fstat(int(f), &st) != 0 || st.st_size < 0 || uint64_t(st.st_size) > cap) return false;
    out.resize(size_t(st.st_size));
    size_t at = 0;
    while (at < out.size()) {
        const ssize_t done = ::pread(int(f), out.data() + at, out.size() - at, off_t(at));
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return false;
        at += size_t(done);
    }
    return true;
}

bool truncateTo(intptr_t f, uint64_t size) { return ::ftruncate(int(f), off_t(size)) == 0; }

bool syncFile(intptr_t f) {
#if defined(__APPLE__)
    return ::fsync(int(f)) == 0;
#else
    return ::fdatasync(int(f)) == 0;
#endif
}

void closeFile(intptr_t f) { ::close(int(f)); }

// The rename itself must reach the disk too, or a power cut can bring back the
// .part beside a finished file. Recovery would then finish it a second time.
void syncDirOf(const std::string& path) {
    const std::string dir = fsPath(path).parent_path().string();
    const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}
#endif

// <name>.takrep for <name>.takrep.part, or a numbered name beside it when a file
// already holds that one: a replay is never overwritten.
std::string finalNameFor(const std::string& part) {
    const std::string base = part.substr(0, part.size() - std::char_traits<char>::length(kPartSuffix));
    if (!exists(base)) return base;
    const std::string stem = base.substr(0, base.size() - 7);   // drop ".takrep"
    for (int i = 1; i < 1000; ++i) {
        std::string alt = stem + "-" + std::to_string(i) + ".takrep";
        if (!exists(alt)) return alt;
    }
    return {};
}

// Renames a finished, synced recording to its .takrep name and releases it.
// POSIX renames while still holding the lock, so no recovery pass can claim the
// file between the two; Windows has to close it first (see renameClosed). Either
// way the outcome is the same file: recovery of a finished .part only renames it.
std::string publish(intptr_t& f, const std::string& part) {
    std::string final = finalNameFor(part);
    bool ok = !final.empty();
#ifdef _WIN32
    closeFile(f); f = -1;
    if (ok && !renameClosed(part, final)) {
        // Another process finishing the same file beat us to the name.
        ok = !exists(part) && exists(final);
    }
#else
    ok = ok && ::rename(part.c_str(), final.c_str()) == 0;
    if (ok) syncDirOf(final);
    closeFile(f); f = -1;
#endif
    if (!ok) return {};
    removeFile(part + kChecksSuffix);
    return final;
}

bool validBundle(const uint8_t* p, uint32_t len, uint32_t index) {
    if (len < 4 || getU32(p) != index) return false;
    Reader r(p, len);
    uint32_t tick = 0;
    Bundle bd;
    return getBundle(r, tick, bd, /*strict=*/true) && tick == index;
}

void recoverOne(const std::string& part, std::vector<RecoveredReplay>& out) {
    intptr_t f = -1;
    if (openLocked(part, f) != Opened::Ok) return;   // live (Busy), finished meanwhile, or unreadable
    std::vector<uint8_t> d;
    // A .part over the size cap can never be loaded; leave it rather than spend the memory.
    if (!readAll(f, d, kMaxReplayBytes + (1u << 20))) { closeFile(f); return; }
    auto discard = [&](const char* why) {
        closeFile(f);
        removeFile(part);
        removeFile(part + kChecksSuffix);
        std::fprintf(stderr, "replay: removed unfinished recording %s (%s)\n", part.c_str(), why);
    };
    if (d.size() < 12 || d[0] != 'T' || d[1] != 'A' || d[2] != 'K' || d[3] != 'R')
        return discard("no complete header");
    // Another build's recording (other format or protocol): not ours to judge or finish.
    if (getU32(d.data() + 4) != kReplayFormat || getU32(d.data() + 8) != kNetVersion) { closeFile(f); return; }
    Reader r(d.data() + 4, d.size() - 4);
    ReplayHeader h;
    uint32_t fmt = 0, proto = 0;
    if (!readReplayHeader(r, h, fmt, proto)) return discard("no complete header");
    const size_t countAt = size_t(r.p - d.data());
    const uint32_t count = r.u32();
    if (!r.ok) return discard("no complete header");
    // Keep every complete bundle, in order. A crash leaves at most one torn write
    // at the end (bytes are only ever appended), and that is where this stops.
    size_t at = countAt + 4;
    uint32_t n = 0;
    while (d.size() - at >= 4) {
        const uint32_t len = getU32(d.data() + at);
        if (d.size() - at - 4 < len || !validBundle(d.data() + at + 4, len, n)) break;
        at += 4 + size_t(len);
        ++n;
    }
    if (n == 0) return discard("no complete tick");
    // Finished but never renamed (stopped between the count fill-in and the rename):
    // the count is real and the hash trail runs exactly to the end of the file.
    bool complete = false;
    if (count == n && d.size() - at >= 4) {
        const uint64_t nchecks = getU32(d.data() + at);
        complete = d.size() - at - 4 == nchecks * kReplayCheckBytes;
    }
    bool ok = true;
    if (!complete) {
        // The hash trail recorded so far: whole records only, for ticks the replay
        // reaches, in order.
        std::vector<uint8_t> trail;
        {
            std::ifstream in(fsPath(part + kChecksSuffix), std::ios::binary);
            std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            Writer t;
            t.u32(0);
            uint32_t kept = 0;
            int64_t last = -1;
            for (size_t i = 0; i + kReplayCheckBytes <= raw.size(); i += kReplayCheckBytes) {
                const uint32_t tick = getU32(raw.data() + i);
                if (tick >= n || int64_t(tick) <= last) continue;
                last = tick;
                t.b.insert(t.b.end(), raw.begin() + ptrdiff_t(i), raw.begin() + ptrdiff_t(i + kReplayCheckBytes));
                ++kept;
            }
            putU32(t.b.data(), kept);
            trail = std::move(t.b);
        }
        uint8_t cnt[4];
        putU32(cnt, n);
        ok = truncateTo(f, at) && writeAt(f, at, trail.data(), trail.size()) && writeAt(f, countAt, cnt, 4);
    }
    ok = ok && syncFile(f);
    const std::string final = ok ? publish(f, part) : std::string();
    if (f != -1) closeFile(f);
    if (final.empty()) {
        std::fprintf(stderr, "replay: could not finish unfinished recording %s\n", part.c_str());
        return;
    }
    std::fprintf(stderr, "replay: recovered %s (%u ticks) from an unfinished recording\n", final.c_str(), n);
    out.push_back({final, n});
}

}  // namespace

// ---- ReplayWriter ----------------------------------------------------------------

ReplayWriter::~ReplayWriter() {
    if (isOpen()) finish();
}

bool ReplayWriter::open(const std::string& dir, const ReplayHeader& h, uint32_t gameId, uint64_t stamp) {
    if (isOpen() || dir.empty()) return false;
    Writer w;
    writeReplayHeader(w, h);
    countAt_ = w.b.size();
    w.u32(kReplayOpenCount);
    for (uint64_t k = 0; k < 64; ++k) {
        const std::string base = dir + "game-" + std::to_string(gameId) + "-" + std::to_string(stamp + k) + ".takrep";
        if (exists(base)) continue;   // never reuse a finished replay's name
        const std::string part = base + kPartSuffix;
        const intptr_t f = createLocked(part);
        if (f == -1) {
            if (exists(part)) continue;   // another recording (or a crashed one) holds it
            error_ = "cannot create " + part;
            return false;
        }
        // A sidecar with no .part is left over from a recording that was finished
        // (the sidecar goes last); it belongs to nobody.
        intptr_t c = createLocked(part + kChecksSuffix);
        if (c == -1) { removeFile(part + kChecksSuffix); c = createLocked(part + kChecksSuffix); }
        if (c == -1 || !writeAt(f, 0, w.b.data(), w.b.size())) {
            if (c != -1) closeFile(c);
            closeFile(f);
            removeFile(part + kChecksSuffix);
            removeFile(part);
            error_ = "cannot write " + part;
            return false;
        }
        file_ = f; chk_ = c; part_ = part;
        fileEnd_ = w.b.size(); chkEnd_ = 0; bundles_ = 0;
        buf_.clear(); chkBuf_.clear(); checks_.clear();
        failed_ = false; error_.clear();
        return true;
    }
    error_ = "no free replay file name in " + dir;
    return false;
}

void ReplayWriter::append(const std::vector<std::vector<uint8_t>>& log, const std::vector<ReplayCheck>& checks) {
    if (!isOpen()) return;   // still queued after a failed write: the next flush retries it
    for (size_t i = bundles_; i < log.size(); ++i) {
        const auto& b = log[i];
        uint8_t len[4];
        putU32(len, uint32_t(b.size()));
        buf_.insert(buf_.end(), len, len + 4);
        buf_.insert(buf_.end(), b.begin(), b.end());
    }
    if (log.size() > bundles_) bundles_ = uint32_t(log.size());
    // Checkpoints are in tick order; only those after the last one queued are new.
    auto from = checks_.empty() ? checks.begin()
        : std::upper_bound(checks.begin(), checks.end(), checks_.back().tick,
                           [](uint32_t tick, const ReplayCheck& c) { return tick < c.tick; });
    for (; from != checks.end(); ++from) {
        checks_.push_back(*from);
        putCheck(chkBuf_, *from);
    }
}

bool ReplayWriter::flush() {
    if (!isOpen()) return false;
    if (!buf_.empty()) {
        // A failed write (disk full) keeps the bytes queued and the end where it was:
        // the next flush writes them again from there, over whatever part made it.
        if (!writeAt(file_, fileEnd_, buf_.data(), buf_.size())) {
            failed_ = true; error_ = "write failed: " + part_; return false;
        }
        fileEnd_ += buf_.size();
        buf_.clear();
    }
    failed_ = false;
    if (!chkBuf_.empty()) {
        // The trail only adds checking to a replay; losing it never loses the replay.
        if (writeAt(chk_, chkEnd_, chkBuf_.data(), chkBuf_.size())) chkEnd_ += chkBuf_.size();
        chkBuf_.clear();
    }
    return true;
}

void ReplayWriter::syncAsync() {
    if (!isOpen()) return;
    {
        std::lock_guard<std::mutex> lock(syncMu_);
        syncWanted_ = true;
    }
    if (!syncThread_.joinable()) {
        syncStop_ = false;
        syncThread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(syncMu_);
            for (;;) {
                syncCv_.wait(lock, [this] { return syncWanted_ || syncStop_; });
                if (syncStop_) return;
                syncWanted_ = false;
                lock.unlock();
                // The handles stay open until stopSync() has joined this thread.
                syncFile(file_);
                syncFile(chk_);
                lock.lock();
            }
        });
    }
    syncCv_.notify_one();
}

void ReplayWriter::stopSync() {
    if (!syncThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(syncMu_);
        syncStop_ = true;
    }
    syncCv_.notify_one();
    syncThread_.join();
    syncWanted_ = false;
}

void ReplayWriter::closeFiles() {
    if (file_ != -1) closeFile(file_);
    if (chk_ != -1) closeFile(chk_);
    file_ = chk_ = -1;
}

std::string ReplayWriter::finish() {
    if (!isOpen()) return {};
    flush();   // the last retry of anything a failed write left queued
    stopSync();
    if (failed_) {   // keep what reached the disk: the next recovery pass finishes it
        closeFiles();
        return {};
    }
    if (bundles_ == 0) {   // nothing was played: nothing to replay
        closeFiles();
        removeFile(part_);
        removeFile(part_ + kChecksSuffix);
        return {};
    }
    Writer t;
    t.u32(uint32_t(checks_.size()));
    for (const auto& c : checks_) { t.u32(c.tick); t.u64(c.hash); t.u64(c.posDigest); }
    uint8_t cnt[4];
    putU32(cnt, bundles_);
    const bool ok = writeAt(file_, fileEnd_, t.b.data(), t.b.size()) && writeAt(file_, countAt_, cnt, 4) &&
                    syncFile(file_);
    if (chk_ != -1) { closeFile(chk_); chk_ = -1; }
    if (!ok) {
        error_ = "write failed: " + part_;
        closeFiles();
        return {};
    }
    const std::string final = publish(file_, part_);
    closeFiles();
    if (final.empty()) error_ = "could not rename " + part_;
    return final;
}

std::vector<RecoveredReplay> recoverReplayParts(const std::string& dir) {
    std::vector<RecoveredReplay> out;
    if (dir.empty()) return out;
    std::vector<std::string> parts, sidecars;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(fsPath(dir), ec)) {
        const auto u8 = e.path().filename().u8string();
        const std::string name(u8.begin(), u8.end());
        if (endsWith(name, std::string(".takrep") + kPartSuffix)) parts.push_back(dir + name);
        else if (endsWith(name, std::string(".takrep") + kPartSuffix + kChecksSuffix)) sidecars.push_back(dir + name);
    }
    for (const auto& p : parts) recoverOne(p, out);
    // A recording creates its .part before the sidecar and removes the sidecar after
    // the rename, so a sidecar without its .part belongs to nobody. (A live one on
    // Windows cannot be deleted; that is fine, it is not orphaned.)
    for (const auto& s : sidecars)
        if (!exists(s.substr(0, s.size() - std::char_traits<char>::length(kChecksSuffix)))) removeFile(s);
    return out;
}

}  // namespace tak::net
