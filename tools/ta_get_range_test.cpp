// ta_get_range_test -- TotalA.exe's COB GET answers ids 1..20 only.
//
// The unit script host's GET (vtable +0x44, 0x480770) indexes its jump table
// with id - 1 up to 0x13 (0x480781); 0, a negative id and every id past 20
// return 0 (0x480abb). Kingdoms' 27..46 (heading, terrain, animation speeds,
// veteran level, standing order) used to answer here, in the simulation's
// host (World::ScriptHost::get) and in the client's display VM
// (client/displayget.h). Both now answer 0 there, whatever the unit does; both run
// the one routine (sim/retailscriptget.h), so the display VM's 1..20 answer as
// the sim's do.

#include "client/displayget.h"
#include "cob/cob.h"
#include "sim/sim.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ta::sim {
struct RetailReplayProbe {
    static std::vector<uint32_t> run(World& world, int id, const char* script) {
        world.notifyUnitScript(*world.unit(id), script);
        return world.unitScripts_.at(id).peek().vm.statics;
    }
};
}

static int g_fail = 0;
static void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  %-64s %s%s%s\n", what, ok ? "ok" : "FAIL", detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_fail;
}

int main() {
    using namespace ta::sim;
    // GET_UNIT_VALUE id; store into static i, for each id.
    const int ids[] = {4, 0, -1, 20, 21, 27, 28, 29, 30, 32, 33, 34, 46, 100};
    constexpr unsigned n = sizeof ids / sizeof ids[0];
    auto file = std::make_shared<ta::cob::File>();
    file->numStatics = n; file->scripts = {{"Read", 0}};
    for (unsigned i = 0; i < n; ++i)
        file->code.insert(file->code.end(), {0x10021001, uint32_t(ids[i]), 0x10042000, 0x10023004, i});
    file->code.push_back(0x10065000);
    UnitType type; type.simulationScript = file; type.maxHp = 100; type.maxVel = Fixed::fromInt(4);
    type.turnRate = 3000; type.turnInPlaceRate = 2000;
    World world;
    world.setTerrain(std::vector<uint8_t>(32 * 32, 100), 32, 32, 20);
    const int id = world.spawn(&type, 200, 200);
    Unit* u = world.unit(id);
    // A moving, turning veteran on a road, standing order set.
    u->baseSpeed = Fixed::fromInt(4); u->speed = Fixed::fromInt(2); u->heading = Bam(0x2000);
    u->animationTurnBam = 1200; u->kills = 10; u->groundTerrainFlags = 0x800; u->hp = Fixed::fromInt(50);
    const auto got = RetailReplayProbe::run(world, id, "Read");
    std::string row;
    bool zero = true;
    for (unsigned i = 1; i < n; ++i) { zero &= got[i] == 0; row += std::to_string(int32_t(got[i])) + " "; }
    std::printf("simulation host (World::ScriptHost::get):\n");
    check(got[0] == 50, "HEALTH (4) answers: the table's ids work", std::to_string(got[0]));
    check(zero, "0, -1, 20 (ARMORED, not stored), 21, 27..34, 46, 100 answer 0", row);

    std::printf("display VM (client/displayget.h):\n");
    // The snapshot of the same unit: the display host answers every id as the sim's.
    u->underConstruction = true; u->buildStarted = true; u->buildRemaining = 0.25f;
    u->moveState = 1; u->fireState = 2;
    UnitR r;
    r.type = u->type; r.deadFor = -1; r.scriptWords = world.scriptGetWords(id);
    r.worldPosition = {u->x.v, u->groundY.v, u->z.v};
    UnitR other = r; other.worldPosition = {Fixed::fromInt(300).v, Fixed::fromInt(12).v, Fixed::fromInt(90).v};
    const std::vector<uint8_t> heights(32 * 32, 100);
    auto find = [&](int which) -> const UnitR* { return which == 7 ? &other : which == id ? &r : nullptr; };
    ta::client::DisplayGetHost host{r, find, {}, &heights, 32, 32};
    auto display = [&](int which, std::vector<int32_t> args = {}) { return ta::client::displayScriptGet(which, args, host); };
    check(display(4) == 50, "HEALTH answers from the sim's words");
    check(display(17) == ta::sim::retailConstructionPercent(0.25f) && display(2) == 1 && display(3) == 2,
          "BUILD_PERCENT_LEFT is the remaining work; standing orders");
    const auto words = world.scriptGetWords(id);
    check(display(1) == int32_t(words.activated) && display(6) == int32_t(words.busy) && display(5) == int32_t(words.ready),
          "ACTIVATION, INBUILDSTANCE and BUSY are the script's (not the queue or motion)");
    check(uint32_t(display(9, {7})) == ta::sim::retailPackXZ(other.worldPosition[0], other.worldPosition[2]) &&
          display(10, {7}) == other.worldPosition[1] && display(9, {99}) == 0,
          "UNIT_XZ / UNIT_Y read the unit the argument names");
    check(display(16, {int32_t(ta::sim::retailPackXZ(Fixed::fromInt(40).v, Fixed::fromInt(40).v))}) == 100 << 16,
          "GROUND_HEIGHT reads the map");
    check(display(15, {3 << 16, 4 << 16}) == 5 << 16, "HYPOT");
    bool none = true;
    for (int v : {0, -1, 20, 21, 27, 28, 29, 30, 32, 33, 34, 46, 100}) none &= display(v) == 0;
    check(none, "ids past 20 (and 0, -1, ARMORED) answer 0, as the simulation's do");
    if (g_fail) { std::printf("ta_get_range_test: %d FAILED\n", g_fail); return 1; }
    std::printf("ta_get_range_test: all passed\n");
    return 0;
}
