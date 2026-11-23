// Right-click on a finished friendly unit: which order each selected unit gets.
//
// Retail's default-order resolver (0x4de530, action mode 1, right-click
// interface) decides per selected unit: help-build, else repair, else load,
// else Guard (FOLLOW_GROUND) when the type has `canguard`, else a Move to the
// point. It never asks for a
// weapon, so a builder guards a healthy unit as a soldier does. Our right-click
// used to guard only with armed units, so a builder did nothing.
//
// The synthetic cases always run; given a retail install (-DTAK_TEST_DATA) it
// also checks every shipped mobile builder guards a healthy unit and repairs a
// damaged one.

#include "client/friendlyorder.h"
#include "hpi/hpi.h"
#include "sim/sim.h"

#include <cstdio>
#include <string>

using namespace tak;
using client::FriendlyOrder;

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; }
}

FriendlyOrder order(const sim::UnitType& t, bool damaged, bool repeating = false,
                    bool finished = true, bool self = false) {
    return client::friendlyUnitOrder(t, finished, repeating, self, damaged);
}
} // namespace

int main(int argc, char** argv) {
    sim::UnitType builder;            // mobile, unarmed, canguard
    builder.id = "builder";
    builder.isBuilder = true;
    builder.canGuard = true;
    sim::UnitType soldier;            // mobile, armed, canguard
    soldier.id = "soldier";
    soldier.weapon.damage = 10;
    soldier.canGuard = true;
    sim::UnitType avatar = soldier;   // armed, no canguard (the shipped gods)
    avatar.canGuard = false;
    sim::UnitType factory = builder;  // a structure that builds
    factory.maxVel = sim::Fixed();
    factory.canGuard = false;

    check(order(builder, false) == FriendlyOrder::Guard, "builder over a healthy unit guards it");
    check(order(builder, true) == FriendlyOrder::Repair, "builder over a damaged unit repairs it");
    check(order(builder, false, true) == FriendlyOrder::Move, "a repeating builder takes no guard: it rallies (Move)");
    check(order(builder, true, true) == FriendlyOrder::Move, "a repeating builder does not repair: it rallies (Move)");
    check(order(builder, false, false, false) == FriendlyOrder::None, "an unfinished builder does nothing");
    check(order(builder, false, false, true, true) == FriendlyOrder::None, "a unit never guards itself");
    check(order(soldier, false) == FriendlyOrder::Guard, "soldier over a healthy unit guards it");
    check(order(soldier, true) == FriendlyOrder::Guard, "soldier over a damaged unit guards it (per unit)");
    check(order(avatar, false) == FriendlyOrder::Move, "no canguard: no guard, weapon or not -- retail moves to the point");
    sim::UnitType tower = soldier;    // armed structure, no canguard
    tower.maxVel = sim::Fixed();
    tower.canGuard = false;
    check(order(tower, false) == FriendlyOrder::None, "a structure with nothing to do does nothing (no move)");
    check(order(factory, true) == FriendlyOrder::None, "a factory neither repairs nor guards");

    if (argc >= 2) {
        hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
        sim::TypeRegistry reg;
        reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
        reg.loadDir(vfs, "units/");
        int builders = 0;
        for (const auto& [id, t] : reg.types()) {
            if (!t.isBuilder || t.isStructure()) continue;
            ++builders;
            check(order(t, false) == FriendlyOrder::Guard, "shipped builder " + id + " guards a healthy unit");
            check(order(t, true) == FriendlyOrder::Repair, "shipped builder " + id + " repairs a damaged unit");
        }
        check(builders >= 20, "shipped mobile builders found (" + std::to_string(builders) + ")");
        if (const auto* sword = reg.find("arasword"))
            check(order(*sword, false) == FriendlyOrder::Guard, "shipped swordsman guards");
        else check(false, "shipped type arasword missing");
    }

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("PASS: right-click on a friendly unit: builders guard, damaged -> repair, canguard rules\n");
    return 0;
}
