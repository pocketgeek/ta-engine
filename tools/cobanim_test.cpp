// Script-driven animation contracts, run on the real shipped COBs.
//
// Retail's engine barely drives animation: it calls Create, and the SCRIPT
// starts control threads that poll GET_UNIT_VALUE and pick the animation. So
// what has to be right is the engine's ANSWERS. Each case here pins one answer
// by running the real script and observing what it does with it.
//
// A castle/factory opens its yard through a BLOCKING protocol (see aracastl
// OpenYard): it sets unit value 18 (YARD_OPEN), then loops -- reading 18 and
// leaving only when the engine answers NONZERO, setting 19 (BUGGER_OFF) and
// sleeping 1.5s each time round. Answer 0 and the thread spins for ever; and
// since Go CALLs startbuild and then OpenYard, the state machine never reaches
// Stop/stopbuild, so the yard doors open and never close.
//
// This pins the engine's side of that contract: with the answer the engine
// gives, the build sequence must terminate.
#include "cob/cob.h"
#include "cob/vm.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace tak;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
    if (!ok) ++fails;
}

// How many times the unit re-issues BUGGER_OFF (unit value 19) over ~40s of
// simulated time, having primed it the way the engine does and then activated
// it.
//
// This reads the protocol directly rather than counting threads. Thread counts
// prove nothing here: a factory legitimately holds one open while it is active
// (Creon's Go ends with START_SCRIPT FactoryFun, the machinery loop), and the
// close path has a handshake of its own. But BUGGER_OFF is unambiguous -- it is
// the "get out of my yard" retry, and it is issued ONLY by the refused branch
// of the OpenYard loop, once every 1.5s, for ever.
static int buggerOffs(const std::string& path, int32_t yardGrant) {
    cob::File f = cob::load(path);
    cob::Vm vm(std::move(f), true);
    int count = 0;
    vm.onGet = [&](int32_t id, const std::vector<int32_t>&) -> int32_t {
        return id == 18 ? (yardGrant > 0 ? 1 : 0) : 0;
    };
    vm.onSetUnitValue = [&](int32_t id, int32_t v) { if (id == 19 && v) ++count; };
    vm.start("Create");
    for (int i = 0; i < 30; ++i) vm.tick(1.0f / 30.0f);
    vm.start("InitState");
    for (int i = 0; i < 5; ++i) vm.tick(1.0f / 30.0f);
    vm.start("Activate");
    for (int i = 0; i < 1200; ++i) vm.tick(1.0f / 30.0f);   // 40s
    return count;
}

// Does the wheel spinner see a turn? SuperDynamicWheelSpinner reads unit value
// 33, tests it against +-910, and spins the two sides of the vehicle at
// different rates. Observed through the piece turn speeds it sets.
static int wheelSides(const std::string& path, int32_t turnRate) {
    cob::File f = cob::load(path);
    cob::Vm vm(std::move(f), true);
    vm.onGet = [&](int32_t id, const std::vector<int32_t>&) -> int32_t {
        if (id == 33) return turnRate;
        if (id == 29) return 100;          // at full speed, so the wheels turn
        return 0;
    };
    vm.start("Create");
    for (int i = 0; i < 120; ++i) vm.tick(1.0f / 30.0f);
    // Distinct spin rates across the pieces = the differential engaged. Wheels
    // are driven with SPIN, so the target rate is what carries it.
    std::vector<float> rates;
    for (const auto& ps : vm.pieces())
        for (int ax = 0; ax < 3; ++ax)
            if (std::fabs(ps.spinTarget[ax]) > 1e-4f) rates.push_back(ps.spinTarget[ax]);
    int distinct = 0;
    for (size_t i = 0; i < rates.size(); ++i) {
        bool seen = false;
        for (size_t j = 0; j < i; ++j)
            if (std::fabs(rates[i] - rates[j]) < 1e-3f) { seen = true; break; }
        if (!seen) ++distinct;
    }
    return distinct;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: cobyard_test <scripts-dir>\n"); return 2; }
    const std::string dir = argv[1];
    // Yard units across three factions, so a faction-specific script template
    // cannot make this pass by accident.
    const char* units[] = {"aracastl", "arakeep", "tarcastl", "creacad", "cresmit"};

    std::printf("[build yard: the engine must grant YARD_OPEN (unit value 18)]\n");
    for (const char* u : units) {
        const std::string path = dir + "/" + u + ".cob";
        std::FILE* probe = std::fopen(path.c_str(), "rb");
        if (!probe) { std::printf("  [SKIP] %s (not in this install)\n", u); continue; }
        std::fclose(probe);

        const int granted = buggerOffs(path, 1);
        const int refused = buggerOffs(path, 0);
        check(granted <= 1,
              std::string(u) + ": granting the yard settles the handshake",
              "BUGGER_OFF issued " + std::to_string(granted) + "x in 40s");
        // The counter-case: what the bug looked like, and what regressing the
        // engine's answer to 0 would look like again.
        check(refused > 10,
              std::string(u) + ": refusing it retries for ever (counter-case)",
              "BUGGER_OFF issued " + std::to_string(refused) + "x in 40s");
    }

    std::printf("\n[wheels: unit value 33 is a SIGNED turn rate]\n");
    {
        const std::string path = dir + "/aracan.cob";
        std::FILE* probe = std::fopen(path.c_str(), "rb");
        if (!probe) std::printf("  [SKIP] aracan (not in this install)\n");
        else {
            std::fclose(probe);
            const int straight = wheelSides(path, 0);
            const int turning  = wheelSides(path, 2000);   // past the 910 threshold
            check(turning > straight,
                  "aracan: turning spins the two sides at different rates",
                  "distinct wheel rates straight=" + std::to_string(straight) +
                      " turning=" + std::to_string(turning));
        }
    }

    std::printf("\n%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
