#pragma once

// Clean shutdown on an OS termination signal. SDL routes SIGTERM/SIGINT to an SDL_QUIT
// event, but the front-end menu deliberately IGNORES SDL_QUIT (the window's X -- you quit
// via the Exit door), so a `kill` (or a `timeout` wrapper, or Ctrl-C) on a headless run
// with no window to click would wedge the process until SIGKILL. Instead we install our
// own handler that sets this flag, and every UI event loop polls termRequested() and
// tears down cleanly -- distinct from the ignorable window-close SDL_QUIT.

#include <atomic>
#include <csignal>

namespace ta {

inline std::atomic<bool> g_termRequested{false};

inline bool termRequested() { return g_termRequested.load(std::memory_order_relaxed); }

// Install SIGTERM/SIGINT handlers. Call once, after SDL_Init, so ours replace SDL's.
inline void installSignalHandlers() {
    auto handler = [](int) { g_termRequested.store(true, std::memory_order_relaxed); };
    std::signal(SIGTERM, handler);
    std::signal(SIGINT, handler);
}

}  // namespace ta
