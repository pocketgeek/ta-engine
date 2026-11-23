// client/localserver.h: the single-player takserver child. A child that dies on
// startup must be seen at once (the connect poll used to wait out its whole 10 s
// timeout on a missing binary), and stop()/the destructor must end a live one.
// The test spawns ITSELF as the child: `--child-exit N` exits N, `--child-sleep`
// idles until killed.
#include "client/localserver.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
int failed = 0;
void expect(bool ok, const char* what) { if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", what); } }
// Poll exited() the way the client's connect loop does; returns the ms it took (-1 = never).
long waitExit(tak::LocalServer& s, int* code, int limitMs) {
    const auto t0 = Clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < limitMs) {
        if (s.exited(code)) return long(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
}
// Is process `pid` gone (exited and reaped)? Checked AFTER a stop/destructor, so a
// LocalServer that merely forgot the child (returned without killing it) fails.
bool processGone(long pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h) return true;
    const bool done = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    CloseHandle(h);
    return done;
#else
    return kill(pid_t(pid), 0) != 0 && errno == ESRCH;
#endif
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--child-exit") return std::atoi(argv[2]);
    if (argc >= 2 && std::string(argv[1]) == "--child-sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    const std::string self = argv[0];

    {   // a child that exits on startup is reported promptly, with its code
        tak::LocalServer s;
        expect(s.start(self, {"--child-exit", "7"}), "start self --child-exit");
        int code = -1;
        const long ms = waitExit(s, &code, 5000);
        expect(ms >= 0 && ms < 1000, "early exit seen in under a second");
        expect(code == 7, "exit code reported");
        expect(s.exited(&code) && code == 7, "exited() stays true once reaped");
        s.stop();
        expect(!s.started() && s.pid() == 0, "stop() releases an exited child");
    }
    {   // a missing binary: Windows refuses at start(); POSIX exits 127 from the child
        tak::LocalServer s;
        const bool started = s.start(self + "-does-not-exist", {"--port", "1"});
        if (started) {
            int code = -1;
            const long ms = waitExit(s, &code, 5000);
            expect(ms >= 0 && ms < 1000, "missing binary seen in under a second");
            expect(code == 127, "missing binary exits 127");
        }
    }
    {   // a live child is running until stopped, and stop() ends it promptly
        tak::LocalServer s;
        expect(s.start(self, {"--child-sleep"}), "start self --child-sleep");
        const long pid = s.pid();
        expect(pid > 0, "live child has a pid");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        expect(!s.exited(), "live child is not reported exited");
        const auto t0 = Clock::now();
        s.stop();
        expect(Clock::now() - t0 < std::chrono::seconds(3), "stop() ends a live child promptly");
        expect(!s.started(), "stop() releases a live child");
        expect(pid > 0 && processGone(pid), "stop() ends a live child");
        s.stop();   // idempotent
    }
    {   // the destructor stops a live child (the session's `continue`/return paths)
        long pid = 0;
        const auto t0 = Clock::now();
        {
            tak::LocalServer s;
            expect(s.start(self, {"--child-sleep"}), "start self for destructor");
            pid = s.pid();
        }
        expect(pid > 0 && Clock::now() - t0 < std::chrono::seconds(3), "destructor returns promptly");
        expect(pid > 0 && processGone(pid), "destructor ends a live child");
    }
    if (!failed) std::printf("local_server: ok\n");
    return failed ? 1 : 0;
}
