#pragma once

// Where a failed launch sends the player. A launch picked in the front-end that
// cannot start (the local server would not run, the data would not load, the
// server refused the sign-in) returns to the menu screen it was picked from with
// the reason shown there; a debug CLI launch has no menu to go back to, so it
// exits with an error code instead. One channel, so a failure cannot be shown in
// the wrong place: single-player once answered "could not start the local server"
// with the multiplayer sign-in dialog, the same mistake an old replay failure made.
// SDL-free so the routing is unit-testable (tools/session_failure_test.cpp).

#include <string>
#include <utility>

namespace tak {

// What the player picked to get here. Cli = no menu (a direct debug launch).
enum class LaunchOrigin { Cli, SinglePlayer, Campaign, Benchmark, Multiplayer, Replay };

// Which menu screen reopens to show the failure. Exit = no menu: quit with an error.
enum class FailTo { Exit, Notice, ServerSelect, ReplayPicker, BenchmarkMenu, CampaignPicker };

struct SessionFailure {
    FailTo to = FailTo::Notice;
    std::string message;
    explicit operator bool() const { return !message.empty(); }
};

// The screen each launch came from: the single-player door is the title screen
// itself (a notice over it), the others are the overlay the pick was made in.
constexpr FailTo failDestination(LaunchOrigin o) {
    switch (o) {
        case LaunchOrigin::Cli:          return FailTo::Exit;
        case LaunchOrigin::SinglePlayer: return FailTo::Notice;
        case LaunchOrigin::Campaign:     return FailTo::CampaignPicker;
        case LaunchOrigin::Benchmark:    return FailTo::BenchmarkMenu;
        case LaunchOrigin::Multiplayer:  return FailTo::ServerSelect;
        case LaunchOrigin::Replay:       return FailTo::ReplayPicker;
    }
    return FailTo::Exit;
}

// Record a failure for `origin`. Returns false for a CLI launch (the caller exits
// with an error code); true when the menu will show it (the caller loops back).
inline bool routeFailure(LaunchOrigin origin, std::string message, SessionFailure& out) {
    const FailTo to = failDestination(origin);
    if (to == FailTo::Exit) return false;
    out = {to, std::move(message)};
    return true;
}

// The reason shown when the local takserver died before the game could connect.
// 127 is the POSIX shell/exec convention for "could not run the program" (our own
// child exits with it when exec fails), so it names the binary instead of a code.
inline std::string localServerExitMessage(int code, const std::string& serverBin) {
    if (code == 127) return "could not run the local server (" + serverBin + ")";
    return "the local server stopped while starting (exit code " + std::to_string(code) + ")";
}

}  // namespace tak
