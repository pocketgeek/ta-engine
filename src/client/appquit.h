#pragma once

// Clean shutdown on an OS termination signal. SDL routes SIGTERM/SIGINT to an SDL_QUIT
// event, but the front-end menu deliberately IGNORES SDL_QUIT (the window's X -- you quit
// via the Exit door), so a `kill` (or a `timeout` wrapper, or Ctrl-C) on a headless run
// with no window to click would wedge the process until SIGKILL. Instead we install our
// own handler that sets this flag, and every UI event loop polls termRequested() and
// tears down cleanly -- distinct from the ignorable window-close SDL_QUIT.

#include <atomic>
#include <csignal>
#ifndef _WIN32
#include <signal.h>   // sigaction: query SIGHUP's disposition
#endif

namespace tak {

inline std::atomic<bool> g_termRequested{false};

inline bool termRequested() { return g_termRequested.load(std::memory_order_relaxed); }

// Install SIGTERM/SIGINT (and SIGHUP: the terminal or SSH session closed) handlers.
// Call once, after SDL_Init, so ours replace SDL's. A clean teardown is what finishes
// the game's replay file; a process killed outright leaves it to the next start's
// recovery (net/replaywriter.h). SIGHUP is left alone when it is IGNORED (`nohup`,
// a launcher that detaches us): that is a request to outlive the terminal.
inline void installSignalHandlers() {
    auto handler = [](int) { g_termRequested.store(true, std::memory_order_relaxed); };
    std::signal(SIGTERM, handler);
    std::signal(SIGINT, handler);
#ifndef _WIN32
    struct sigaction hup{};
    if (::sigaction(SIGHUP, nullptr, &hup) == 0 && hup.sa_handler != SIG_IGN) std::signal(SIGHUP, handler);
#endif
}

}  // namespace tak
