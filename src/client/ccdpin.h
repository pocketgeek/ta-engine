#pragma once
// PIN TO CCDS (Options > GAME, Settings::ccdPin): which CCD the game and its local server
// run on, and how many simulation threads each then gets. Decided when a game session
// opens, like SIMULATION THREADS, which it replaces while on. Scheduling only: it never
// changes a game's outcome.

#include "client/settings.h"
#include "sim/simcpus.h"

#include <algorithm>
#include <vector>

namespace tak::client {

// Offered: two or more CCDs, on a system that pins threads.
inline bool ccdPinningAvailable(const std::vector<sim::Ccd>& ccds, const sim::CpuTopology& cpus) {
    return ccds.size() >= 2 && cpus.canPin;
}

// The CCDs a choice names (-1 = AUTO): the game on the largest L3 (else the first), the
// server on the first other one. A saved index past this machine's CCDs is AUTO.
struct CcdChoice { int game = 0, server = 0; };
inline CcdChoice resolveCcds(const std::vector<sim::Ccd>& ccds, int game, int server) {
    const int n = int(ccds.size());
    CcdChoice c;
    if (n == 0) return c;
    c.game = game >= 0 && game < n ? game : std::max(sim::largestL3(ccds), 0);
    c.server = server >= 0 && server < n ? server : (c.game == 0 && n > 1 ? 1 : 0);
    return c;
}

// A session's plan. on: the setting is on and offered. The game's threads all go on
// `game`, its local server's on `server`; each simulation gets a thread per core of its
// CCD (sim::ccdThreads), split as SIMULATION THREADS splits them (the game the odd one)
// when both share a CCD.
struct CcdPlan {
    bool on = false;
    int gameCcd = 0, serverCcd = 0;
    std::vector<int> game, server;
    unsigned gameThreads = 0, serverThreads = 0;
};
inline CcdPlan planCcds(const Settings& s, bool localServer) {
    CcdPlan p;
    if (!s.ccdPin) return p;
    const auto ccds = sim::detectCcds();
    if (!ccdPinningAvailable(ccds, sim::detectCpus())) return p;
    const CcdChoice c = resolveCcds(ccds, s.ccdGame, s.ccdServer);
    p.on = true;
    p.gameCcd = c.game; p.serverCcd = c.server;
    p.game = ccds[size_t(c.game)].cpus;
    p.server = ccds[size_t(c.server)].cpus;
    p.gameThreads = sim::ccdThreads(ccds[size_t(c.game)]);
    p.serverThreads = sim::ccdThreads(ccds[size_t(c.server)]);
    if (localServer && c.game == c.server) {
        const unsigned t = p.gameThreads;
        p.gameThreads = std::max((t + 1) / 2, 1u);
        p.serverThreads = std::max(t / 2, 1u);
    }
    return p;
}

// Puts this process (the game) on its CCD, or back on every CPU it started with when the
// plan is off and an earlier session pinned it.
inline void applyGameCcd(const CcdPlan& plan) {
    static bool pinned = false;
    if (plan.on) pinned = sim::setProcessCpus(plan.game) || pinned;
    else if (pinned) { sim::setProcessCpus({}); pinned = false; }
}

}  // namespace tak::client
