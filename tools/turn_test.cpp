// turn_test -- retail's TurnDirection(deg) engine callin (icd 0x4d9550), which we
// now dispatch (gameview_impl.cpp) on a mover's per-tick heading change, edge-keyed
// on the turn-direction sign. On the archer araarch, TurnDirection stashes its arg in
// static 7, and the MoveWatcher ambient loops `static0 = (speed>5) OR static7` -- so a
// turning-but-stationary unit still gates its walk gait ON (the turn-in-place gait,
// instead of sliding round). This pins that mechanism deterministically: with speed
// forced to 0, static0 is driven purely by TurnDirection.
//   turn_test <retail-install-dir>
#include "hpi/hpi.h"
#include "cob/cob.h"
#include "cob/vm.h"
#include <cstdio>
#include <memory>
using namespace tak;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: turn_test <install>\n"); return 2; }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    auto f = std::make_shared<const cob::File>(
        cob::load(vfs.read("scripts/araarch.cob"), "araarch.cob"));
    check(f->scriptIndex("TurnDirection") >= 0, "araarch defines TurnDirection");

    cob::Vm vm(f);
    vm.onGet = [](int32_t, const std::vector<int32_t>&) { return int32_t(0); };  // speed 0: stationary
    vm.start("Create");
    auto settle = [&]{ for (int i = 0; i < 8; ++i) vm.tick(1.0f / 30.0f); };  // >100ms: MoveWatcher runs
    settle();
    check(vm.getStatic(0) == 0, "idle + not turning -> move gate (static0) clear");

    vm.start("TurnDirection", {20});   // began turning right
    settle();
    check(vm.getStatic(0) != 0, "turning in place -> move gate engaged (gait plays, no slide)");

    vm.start("TurnDirection", {0});    // stopped turning
    settle();
    check(vm.getStatic(0) == 0, "turn stopped + still idle -> move gate clears again");

    std::printf("turn_test: %s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
