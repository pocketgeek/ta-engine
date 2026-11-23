// Launch-failure routing (client/sessionfailure.h): a failed launch reopens the menu
// screen it was picked from, and only a CLI launch exits. Single-player once reopened
// the multiplayer sign-in dialog -- the same misroute a refused replay made before.
#include "client/sessionfailure.h"
#include <cstdio>

int main() {
    int failed = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", what); } };
    using ta::FailTo; using ta::LaunchOrigin;
    expect(ta::failDestination(LaunchOrigin::Cli) == FailTo::Exit, "CLI launch exits");
    expect(ta::failDestination(LaunchOrigin::SinglePlayer) == FailTo::Notice, "single-player -> title notice");
    expect(ta::failDestination(LaunchOrigin::Campaign) == FailTo::CampaignPicker, "campaign -> campaign picker");
    expect(ta::failDestination(LaunchOrigin::Benchmark) == FailTo::BenchmarkMenu, "benchmark -> benchmark menu");
    expect(ta::failDestination(LaunchOrigin::Multiplayer) == FailTo::ServerSelect, "multiplayer -> server select");
    expect(ta::failDestination(LaunchOrigin::Replay) == FailTo::ReplayPicker, "replay -> replay picker");
    // Nothing but multiplayer may land in the sign-in dialog.
    for (LaunchOrigin o : {LaunchOrigin::Cli, LaunchOrigin::SinglePlayer, LaunchOrigin::Campaign,
                           LaunchOrigin::Benchmark, LaunchOrigin::Replay})
        expect(ta::failDestination(o) != FailTo::ServerSelect, "only multiplayer reopens the sign-in dialog");

    ta::SessionFailure f;
    expect(!f, "empty failure is false");
    expect(!ta::routeFailure(LaunchOrigin::Cli, "boom", f) && !f, "CLI failure is not queued for a menu");
    expect(ta::routeFailure(LaunchOrigin::SinglePlayer, "no server", f) && f &&
           f.to == FailTo::Notice && f.message == "no server", "single-player failure queued as a notice");
    expect(ta::routeFailure(LaunchOrigin::Multiplayer, "refused", f) && f.to == FailTo::ServerSelect &&
           f.message == "refused", "multiplayer failure queued for the sign-in dialog");

    expect(ta::localServerExitMessage(127, "/x/taserver").find("/x/taserver") != std::string::npos,
           "exec failure names the binary");
    expect(ta::localServerExitMessage(3, "/x/taserver").find("exit code 3") != std::string::npos,
           "other exits report the code");
    return failed ? 1 : 0;
}
