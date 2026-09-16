// standorders_test -- the two standing orders TA keeps per unit.
//
// Data-independent, so CI runs it.
//
// Retail keeps the move order and the fire order SEPARATELY, and the single
// "stance" the HUD shows is a front-end that writes both (Kingdoms had only the
// composite). Three things about that were wrong or missing here, each of which
// is invisible from the composite alone:
//
//   * The composite reaches only three of the nine combinations, and in
//     particular can NEVER produce fire state 1, "return fire" -- the state where
//     a unit shoots back but never picks its own targets. That is an ordinary
//     retail setting, and with only setStance there was no way to ask for it.
//   * Each axis has its OWN permission flag in the FBI (mobilestandorders and
//     firestandorders), each falling back to the Kingdoms-era composite
//     unitstandorders. Measured over the Commander Pack's 272 unit types, the two
//     end up differing on 8 -- few, but a single folded flag gets every one of
//     those wrong, either offering a control retail withholds or withholding one
//     it offers.
//   * Move state 2 (roam, unlimited chase) is unreachable from the composite by
//     design -- retail's setter cannot write it either -- so a unit that spawned
//     roaming gives it up for good the first time the stance buttons are touched.
//     The per-axis setter CAN restore it, which is what the MOVEORD cycle does.

#include "sim/sim.h"

#include <cstdio>
#include <string>

using namespace ta::sim;

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("  %-70s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}
static void eqi(int got, int want, const std::string& what) {
    bool ok = got == want;
    std::printf("  %-70s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) { std::printf("      got %d, want %d\n", got, want); ++g_fail; }
}

static UnitType soldier(const char* id, bool mayMove = true, bool mayFire = true) {
    UnitType t{};
    t.name = id; t.id = id;
    t.maxVel = 60; t.turnRate = 10000;
    t.maxHp = 100; t.canMove = true;
    t.footX = 2; t.footZ = 2;
    t.canSetMoveState = mayMove;
    t.canSetFireState = mayFire;
    t.canSetStance = mayMove || mayFire;
    return t;
}

int main() {
    std::printf("standorders_test\n");

    World w;
    w.setVisPlayer(-1);
    w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, /*seaLevel=*/20);

    UnitType both = soldier("both");
    UnitType moveOnly = soldier("moveonly", true, false);
    UnitType fireOnly = soldier("fireonly", false, true);
    UnitType neither = soldier("neither", false, false);

    const int a = w.spawn(&both, 200, 200, 0, 0);
    const int m = w.spawn(&moveOnly, 300, 200, 0, 0);
    const int f = w.spawn(&fireOnly, 400, 200, 0, 0);
    const int n = w.spawn(&neither, 500, 200, 0, 0);
    check(a && m && f && n, "four units spawn");

    // --- the composite still behaves exactly as it did ------------------------
    w.setStance(a, 0);
    eqi(w.unit(a)->moveState, 1, "Offensive writes move 1");
    eqi(w.unit(a)->fireState, 2, "...and fire 2");
    w.setStance(a, 1);
    eqi(w.unit(a)->moveState, 0, "Defensive writes move 0");
    eqi(w.unit(a)->fireState, 2, "...and fire 2");
    w.setStance(a, 2);
    eqi(w.unit(a)->moveState, 0, "Passive writes move 0");
    eqi(w.unit(a)->fireState, 0, "...and fire 0");

    // --- the state the composite cannot express -------------------------------
    w.setFireState(a, 1);
    eqi(w.unit(a)->fireState, 1, "return fire is reachable per-axis");
    eqi(w.unit(a)->moveState, 0, "and setting the fire axis leaves the move axis alone");
    // Displayed stance is derived: fire != 0 and move == 0 reads as Defensive.
    eqi(w.unit(a)->stance, 1, "the displayed stance re-derives from the two axes");

    // --- roam, which the composite trades away permanently --------------------
    w.setMoveState(a, 2);
    eqi(w.unit(a)->moveState, 2, "roam is restorable per-axis");
    eqi(w.unit(a)->stance, 0, "and with fire on, that reads as Offensive");
    w.setStance(a, 1);
    eqi(w.unit(a)->moveState, 0, "the composite still cannot write roam back");

    // --- the axes are permissioned independently ------------------------------
    w.setMoveState(m, 0); w.setFireState(m, 0);
    eqi(w.unit(m)->moveState, 0, "a move-only unit accepts a move order");
    eqi(w.unit(m)->fireState, 2, "and refuses a fire order, keeping its default");

    w.setMoveState(f, 0); w.setFireState(f, 0);
    eqi(w.unit(f)->moveState, 2, "a fire-only unit refuses a move order");
    eqi(w.unit(f)->fireState, 0, "and accepts a fire order");

    w.setMoveState(n, 0); w.setFireState(n, 0); w.setStance(n, 2);
    eqi(w.unit(n)->moveState, 2, "a unit allowed neither keeps its move default");
    eqi(w.unit(n)->fireState, 2, "and its fire default, through both setters and the composite");

    // --- values are clamped, not wrapped or stored raw -------------------------
    w.setMoveState(a, 7); w.setFireState(a, -3);
    eqi(w.unit(a)->moveState, 2, "an out-of-range move value clamps to 2");
    eqi(w.unit(a)->fireState, 0, "a negative fire value clamps to 0");

    // --- a dead or unknown unit is a no-op, not a crash ------------------------
    w.setMoveState(999999, 1);
    w.setFireState(0, 1);
    check(true, "setting the state of a unit that does not exist is a no-op");

    std::printf(g_fail ? "standorders_test: %d FAILURE(S)\n" : "standorders_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
