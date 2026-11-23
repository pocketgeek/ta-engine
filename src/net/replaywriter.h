#pragma once

// Writes a .takrep WHILE the game is played, so a replay survives however the game
// ends -- a crash, a kill, a power cut, or simply play continuing after the result.
//
// It used to be written in one go at the end, from memory. Anything that ended the
// process without running that code (SIGKILL, a segfault, the OOM killer, Task
// Manager, a power cut) lost the whole game, and a replay written at the player's
// result dropped everything that was played after it.
//
// The file on disk is the finished format byte for byte (net/replayhdr.h), with
// two differences while it is being written:
//   * it is named <name>.takrep.part, so the replay picker (which lists *.takrep)
//     never offers half a file;
//   * the bundle count is kReplayOpenCount and the hash trail is missing; the trail
//     goes to a small sidecar, <name>.takrep.part.chk, in its own 20-byte records.
// finish() writes the trail after the bundles, fills in the count, makes it durable
// and renames it to <name>.takrep. A .part left behind by a process that never got
// that far is finished by recoverReplayParts(), which keeps every complete bundle.
//
// A live recording is LOCKED, so recovery in another process (or a second client
// on the same profile) never touches it: flock(LOCK_EX) on POSIX; on Windows the
// file is opened without FILE_SHARE_WRITE/FILE_SHARE_DELETE, so nobody else can
// open it for writing or rename it.
//
// Presentation/IO only: nothing here is read back by the simulation.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/replayhdr.h"

namespace tak::net {

// The count field of a file still being written. A real count is bounded far below
// it by the replay size cap, so it can never be mistaken for one.
inline constexpr uint32_t kReplayOpenCount = 0xFFFFFFFFu;

class ReplayWriter {
public:
    ReplayWriter() = default;
    ~ReplayWriter();   // finish()es a recording still open
    ReplayWriter(const ReplayWriter&) = delete;
    ReplayWriter& operator=(const ReplayWriter&) = delete;

    // Creates <dir>game-<gameId>-<stamp>.takrep.part (a later stamp when that name
    // is taken) and writes the header. `dir` ends in a path separator. UTF-8 path.
    bool open(const std::string& dir, const ReplayHeader& h, uint32_t gameId, uint64_t stamp);
    bool isOpen() const { return file_ != -1; }

    // Queues log[bundles()..] and every checkpoint newer than the last one queued.
    // Both vectors may be cleared and refilled from tick 0 (a rejoin re-sends the
    // same game): what is already queued is skipped, so nothing is written twice.
    // Buffers only; flush() hands the bytes to the OS.
    void append(const std::vector<std::vector<uint8_t>>& log, const std::vector<ReplayCheck>& checks);
    size_t buffered() const { return buf_.size() + chkBuf_.size(); }
    // After this returns true the data survives the process being killed. On a
    // failed write (disk full) it returns false and keeps the bytes queued; the next
    // flush, and finish(), write them again. Nothing is dropped while it fails.
    bool flush();
    bool failing() const { return failed_; }   // the last flush failed
    // Ask for the flushed data to reach the disk (survives a power cut). Runs on a
    // helper thread: an fsync on a busy disk can take long enough to drop frames.
    void syncAsync();
    // Writes the hash trail, fills in the count, syncs and renames the file to
    // .takrep. Returns that path; "" when no bundle was recorded (the .part is
    // removed) or on an I/O failure (the .part is kept for recovery).
    std::string finish();

    uint32_t bundles() const { return bundles_; }
    const std::string& partPath() const { return part_; }
    const std::string& error() const { return error_; }

private:
    void stopSync();
    void closeFiles();

    intptr_t file_ = -1, chk_ = -1;   // fd, or a Windows HANDLE
    std::string part_;
    uint64_t countAt_ = 0, fileEnd_ = 0, chkEnd_ = 0;
    uint32_t bundles_ = 0;
    std::vector<uint8_t> buf_, chkBuf_;
    std::vector<ReplayCheck> checks_;
    bool failed_ = false;
    std::string error_;
    std::thread syncThread_;
    std::mutex syncMu_;
    std::condition_variable syncCv_;
    bool syncWanted_ = false, syncStop_ = false;
};

// Finishes every <dir>*.takrep.part that no live process is still recording:
// keeps each complete bundle (a torn final write is cut off), adds the hash trail
// recorded so far, and renames it to .takrep. A .part with no complete bundle is
// removed, as is a sidecar whose recording is gone. Returns the replays it wrote.
struct RecoveredReplay {
    std::string path;
    uint32_t bundles = 0;
};
std::vector<RecoveredReplay> recoverReplayParts(const std::string& dir);

}  // namespace tak::net
