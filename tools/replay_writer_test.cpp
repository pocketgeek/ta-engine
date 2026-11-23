// The streamed replay writer (net/replaywriter.h): a finished recording is the
// finished file byte for byte, a rejoin's re-sent log is not written twice, and
// what a dead process leaves behind (.part + .chk, possibly torn) is recovered into
// a replay loadReplayFile accepts -- while a live recording is never touched.
#include "client/replayfile.h"
#include "net/crypto.h"
#include "net/replaywriter.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include "util/u8path.h"

#ifndef _WIN32
  #include <csignal>
  #include <sys/resource.h>
#endif

using namespace tak::net;
namespace fs = std::filesystem;
namespace {
int checks = 0;
void check(bool ok, const std::string& message) { ++checks; if (!ok) throw std::runtime_error(message); }

ReplayHeader header() {
    ReplayHeader h;
    h.mapId = "Synthetic map"; h.unitCap = 2000; h.seed = 77;
    h.slotType[0] = h.slotType[1] = 1; h.slotFaction[1] = 1; h.slotTeam[1] = 1;
    return h;
}
std::vector<uint8_t> bundle(uint32_t tick) {   // a tick with no orders and no events
    Writer w; w.u32(tick); w.u32(0); w.u32(0);
    return w.b;
}
std::vector<std::vector<uint8_t>> ticks(uint32_t n) {
    std::vector<std::vector<uint8_t>> log;
    for (uint32_t t = 0; t < n; ++t) log.push_back(bundle(t));
    return log;
}
std::vector<ReplayCheck> trail(uint32_t n) {   // a checkpoint every 30 ticks, like the client's
    std::vector<ReplayCheck> c;
    for (uint32_t t = 29; t < n; t += 30) c.push_back({t, 0x1000u + t, 0x2000u + t});
    return c;
}
// The one-shot layout the server writes (and a finished stream matches).
std::vector<uint8_t> oneShot(const ReplayHeader& h, const std::vector<std::vector<uint8_t>>& log,
                             const std::vector<ReplayCheck>& c) {
    Writer w;
    writeReplayHeader(w, h);
    w.u32(uint32_t(log.size()));
    for (const auto& b : log) { w.u32(uint32_t(b.size())); w.b.insert(w.b.end(), b.begin(), b.end()); }
    w.u32(uint32_t(c.size()));
    for (const auto& k : c) { w.u32(k.tick); w.u64(k.hash); w.u64(k.posDigest); }
    return w.b;
}
std::vector<uint8_t> bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void put(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
std::string u8(const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); }
size_t count(const fs::path& dir, const std::string& tail) {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string name = u8(e.path().filename());
        if (name.size() >= tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) ++n;
    }
    return n;
}
ReplayFile load(const std::string& path) {
    ReplayFile r;
    check(loadReplayFile(path, r), "replay refused: " + path + " " + r.error);
    return r;
}

void run() {
    // A non-ASCII folder: the writer and recovery take UTF-8 paths on every platform.
    const fs::path root = fs::temp_directory_path() / tak::u8path("tak-replay-writer-\xc3\xa9-" +
                                                                tak::crypto::toHex(tak::crypto::randomVec(8)));
    fs::create_directories(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{root};
    const std::string dir = u8(root) + "/";
    const ReplayHeader h = header();

    // 1. Streamed in pieces, finished: the same bytes as the one-shot writer.
    {
        const auto log = ticks(200);
        const auto c = trail(200);
        ReplayWriter w;
        check(w.open(dir, h, 5, 1000), "open: " + w.error());
        check(fs::exists(tak::u8path(w.partPath())) && w.partPath().find(".takrep.part") != std::string::npos,
              "recording is not a .part");
        for (size_t at : {0u, 1u, 31u, 120u, 200u}) {
            const std::vector<std::vector<uint8_t>> sofar(log.begin(), log.begin() + ptrdiff_t(at));
            std::vector<ReplayCheck> csofar;
            for (const auto& k : c) if (k.tick < at) csofar.push_back(k);
            w.append(sofar, csofar);
            check(w.flush(), "flush");
        }
        // The live .part is locked: recovery (even from this process) leaves it alone.
        check(recoverReplayParts(dir).empty() && fs::exists(tak::u8path(w.partPath())), "recovery touched a live recording");
        const std::string path = w.finish();
        check(path == dir + "game-5-1000.takrep", "finished name: " + path);
        check(bytes(tak::u8path(path)) == oneShot(h, log, c), "streamed file differs from the one-shot layout");
        check(count(root, ".part") == 0 && count(root, ".chk") == 0, "finish left a .part or sidecar");
        const auto r = load(path);
        check(r.bundles.size() == 200 && r.checks.size() == c.size() && r.checks.back().tick == c.back().tick,
              "finished replay lost bundles or checkpoints");
    }
    // 2. A rejoin clears the log and re-sends it from tick 0: written once, not twice.
    //    And the name of a finished replay is never reused.
    {
        ReplayWriter w;
        check(w.open(dir, h, 5, 1000), "open 2");
        check(w.partPath() == dir + "game-5-1001.takrep.part", "reused a finished replay's name: " + w.partPath());
        w.append(ticks(50), trail(50));
        w.flush();
        w.append({}, {});                       // the log cleared at the rejoin's GameStarting
        w.append(ticks(30), trail(30));         // catch-up, part way
        w.append(ticks(90), trail(90));         // caught up and beyond
        const std::string path = w.finish();
        check(bytes(tak::u8path(path)) == oneShot(h, ticks(90), trail(90)), "rejoin duplicated or lost ticks");
    }
    // 3. Nothing recorded: nothing kept.
    {
        ReplayWriter w;
        check(w.open(dir, h, 6, 1000), "open 3");
        const std::string part = w.partPath();
        check(w.finish().empty() && !fs::exists(tak::u8path(part)) && !fs::exists(tak::u8path(part + ".chk")),
              "an empty recording left a file");
    }
#ifndef _WIN32
    // 3b. A write that fails (the disk full) keeps its bytes and is written again
    //     once there is room: the finished replay is still complete. RLIMIT_FSIZE
    //     stands in for the full disk; the first failing write lands part way.
    {
        std::signal(SIGXFSZ, SIG_IGN);   // a write past the limit fails with EFBIG instead
        rlimit saved{};
        getrlimit(RLIMIT_FSIZE, &saved);
        auto limitTo = [&](rlim_t cap) {
            rlimit l = saved;
            l.rlim_cur = cap;
            check(setrlimit(RLIMIT_FSIZE, &l) == 0, "setrlimit");
        };
        ReplayWriter w;
        check(w.open(dir, h, 12, 1000), "open 3b");
        const fs::path part = tak::u8path(w.partPath());
        limitTo(rlim_t(fs::file_size(part) + 30));
        w.append(ticks(60), trail(60));
        check(!w.flush() && w.failing(), "a write past the limit did not fail");
        w.append(ticks(100), trail(100));        // the game goes on while the disk is full
        check(!w.flush() && w.failing(), "the retry should still fail");
        check(setrlimit(RLIMIT_FSIZE, &saved) == 0, "restore rlimit");
        check(w.flush() && !w.failing(), "the retry did not recover once there was room");
        w.append(ticks(150), trail(150));
        const std::string path = w.finish();
        check(!path.empty() && bytes(tak::u8path(path)) == oneShot(h, ticks(150), trail(150)),
              "a write failure lost or duplicated ticks");

        // Still failing at the end: finish keeps what reached the disk as a .part
        // (complete ticks only once recovered), never a half-file under .takrep.
        ReplayWriter f;
        check(f.open(dir, h, 13, 1000), "open 3b-2");
        f.append(ticks(40), trail(40));
        check(f.flush(), "flush 3b-2");
        const fs::path part2 = tak::u8path(f.partPath());
        limitTo(rlim_t(fs::file_size(part2) + 7));
        f.append(ticks(80), trail(80));
        const std::string none = f.finish();
        check(setrlimit(RLIMIT_FSIZE, &saved) == 0, "restore rlimit 2");
        check(none.empty() && fs::exists(part2), "a failed finish did not keep its .part");
        const fs::path keep = root / "full";
        fs::create_directories(keep);
        fs::rename(part2, keep / part2.filename());
        fs::rename(fs::path(part2.native() + fs::path(".chk").native()),
                   keep / fs::path(part2.filename().native() + fs::path(".chk").native()));
        const auto rec = recoverReplayParts(u8(keep) + "/");
        check(rec.size() == 1 && rec[0].bundles == 40 && bytes(tak::u8path(rec[0].path)) == oneShot(h, ticks(40), trail(40)),
              "the .part of a failed finish did not recover to its complete ticks");
    }
#endif
    // 4. A process that died mid-game: the .part and its sidecar, as it left them.
    const fs::path dead = root / "dead";
    fs::create_directories(dead);
    const std::string deadDir = u8(dead) + "/";
    auto leftovers = [&](uint32_t gameId, uint32_t n) {
        ReplayWriter w;
        check(w.open(dir, h, gameId, 2000), "open leftovers");
        w.append(ticks(n), trail(n));
        w.flush();
        const fs::path part = tak::u8path(w.partPath());
        const fs::path to = dead / part.filename();
        // Read, not copy_file: the live recording is shared for reading only (Windows).
        put(to, bytes(part));
        put(fs::path(to.native() + fs::path(".chk").native()), bytes(fs::path(part.native() + fs::path(".chk").native())));
        w.finish();
        return to;
    };
    {
        // Killed between flushes: the last write torn part way through a bundle, and
        // a checkpoint record torn too.
        const fs::path part = leftovers(7, 100);
        auto b = bytes(part);
        const auto torn = bundle(100);
        Writer len; len.u32(uint32_t(torn.size()));
        b.insert(b.end(), len.b.begin(), len.b.end());
        b.insert(b.end(), torn.begin(), torn.begin() + 5);
        put(part, b);
        const fs::path chk = fs::path(part.native() + fs::path(".chk").native());
        auto cb = bytes(chk);
        cb.insert(cb.end(), {1, 2, 3, 4, 5, 6, 7});
        put(chk, cb);
        // A recording that died before its first tick, and a sidecar whose .part is gone.
        const fs::path empty = leftovers(8, 0);
        put(dead / "game-9-1.takrep.part.chk", {1, 2, 3});
        const auto rec = recoverReplayParts(deadDir);
        check(rec.size() == 1 && rec[0].bundles == 100 && rec[0].path == deadDir + "game-7-2000.takrep",
              "recovery did not keep exactly the complete ticks");
        check(!fs::exists(empty) && count(dead, ".part") == 0 && count(dead, ".chk") == 0,
              "recovery left a .part or sidecar behind");
        check(bytes(tak::u8path(rec[0].path)) == oneShot(h, ticks(100), trail(100)),
              "recovered replay is not the finished layout of its complete ticks");
        const auto r = load(rec[0].path);
        check(r.bundles.size() == 100 && r.checks.size() == trail(100).size(), "recovered replay lost its checkpoints");
        check(recoverReplayParts(deadDir).empty(), "a second recovery pass found work");
    }
    {
        // Killed after the count was filled in but before the rename: already complete.
        const fs::path part = leftovers(10, 64);
        const auto finished = oneShot(h, ticks(64), trail(64));
        put(part, finished);
        // ...and a finished replay already holds its name: recovery picks another.
        put(dead / "game-10-2000.takrep", {'x'});
        const auto rec = recoverReplayParts(deadDir);
        check(rec.size() == 1 && rec[0].path == deadDir + "game-10-2000-1.takrep" && bytes(tak::u8path(rec[0].path)) == finished,
              "a finished-but-unrenamed recording was changed or overwrote a replay");
        check(bytes(dead / "game-10-2000.takrep") == std::vector<uint8_t>{'x'}, "recovery overwrote an existing replay");
    }
    {
        // Garbage under a .part name (a power cut before the header reached the disk).
        put(dead / "game-11-1.takrep.part", std::vector<uint8_t>(40, 0));
        check(recoverReplayParts(deadDir).empty() && !fs::exists(dead / "game-11-1.takrep.part"),
              "a recording with no header was kept");
    }
    {
        // Another build's recording (other format, or other protocol): not this build's
        // to judge or finish, so it is left exactly as it is, sidecar and all.
        for (const auto& [gameId, at, value] : {std::tuple{12u, size_t(4), kReplayFormat - 1},
                                                std::tuple{13u, size_t(8), kNetVersion - 1},
                                                std::tuple{14u, size_t(8), kNetVersion + 1}}) {
            const fs::path part = leftovers(gameId, 50);
            auto b = bytes(part);
            for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(value >> (8 * i));
            put(part, b);
            const fs::path chk = fs::path(part.native() + fs::path(".chk").native());
            const auto sidecar = bytes(chk);
            check(recoverReplayParts(deadDir).empty() && bytes(part) == b && bytes(chk) == sidecar,
                  "recovery touched another build's recording (game " + std::to_string(gameId) + ")");
            fs::remove(part); fs::remove(chk);
        }
    }
}
}  // namespace

int main() {
    try { run(); std::cout << "PASS: " << checks << " replay writer checks\n"; return 0; }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
