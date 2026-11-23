// Launch-failure routing (client/sessionfailure.h): a failed launch reopens the menu
// screen it was picked from, and only a CLI launch exits. Single-player once reopened
// the multiplayer sign-in dialog -- the same misroute a refused replay made before.
#include "client/sessionfailure.h"
#include <cstdio>

int main() {
    int failed = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", what); } };
    using tak::FailTo; using tak::LaunchOrigin;
    expect(tak::failDestination(LaunchOrigin::Cli) == FailTo::Exit, "CLI launch exits");
    expect(tak::failDestination(LaunchOrigin::SinglePlayer) == FailTo::Notice, "single-player -> title notice");
    expect(tak::failDestination(LaunchOrigin::Campaign) == FailTo::CampaignPicker, "campaign -> campaign picker");
    expect(tak::failDestination(LaunchOrigin::Benchmark) == FailTo::BenchmarkMenu, "benchmark -> benchmark menu");
    expect(tak::failDestination(LaunchOrigin::Multiplayer) == FailTo::ServerSelect, "multiplayer -> server select");
    expect(tak::failDestination(LaunchOrigin::Replay) == FailTo::ReplayPicker, "replay -> replay picker");
    // Nothing but multiplayer may land in the sign-in dialog.
    for (LaunchOrigin o : {LaunchOrigin::Cli, LaunchOrigin::SinglePlayer, LaunchOrigin::Campaign,
                           LaunchOrigin::Benchmark, LaunchOrigin::Replay})
        expect(tak::failDestination(o) != FailTo::ServerSelect, "only multiplayer reopens the sign-in dialog");

    tak::SessionFailure f;
    expect(!f, "empty failure is false");
    expect(!tak::routeFailure(LaunchOrigin::Cli, "boom", f) && !f, "CLI failure is not queued for a menu");
    expect(tak::routeFailure(LaunchOrigin::SinglePlayer, "no server", f) && f &&
           f.to == FailTo::Notice && f.message == "no server", "single-player failure queued as a notice");
    expect(tak::routeFailure(LaunchOrigin::Multiplayer, "refused", f) && f.to == FailTo::ServerSelect &&
           f.message == "refused", "multiplayer failure queued for the sign-in dialog");

    expect(tak::localServerExitMessage(127, "/x/takserver").find("/x/takserver") != std::string::npos,
           "exec failure names the binary");
    expect(tak::localServerExitMessage(3, "/x/takserver").find("exit code 3") != std::string::npos,
           "other exits report the code");
    return failed ? 1 : 0;
}
