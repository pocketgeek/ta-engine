// ta_get_range_test -- TotalA.exe's COB GET answers ids 1..20 only.
//
// The unit script host's GET (vtable +0x44, 0x480770) indexes its jump table
// with id - 1 up to 0x13 (0x480781); 0, a negative id and every id past 20
// return 0 (0x480abb). Kingdoms' 27..46 (heading, terrain, animation speeds,
// veteran level, standing order) used to answer here, in the simulation's
// host (World::ScriptHost::get) and in the client's display VM
// (client/displayget.h). Both now answer 0 there, whatever the unit does.

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
    UnitR r;
    r.scriptHealthPercent = 50; r.yardOpen = true; r.constructionPercentLeft = 7;
    r.kills = 10; r.standingOrder = 1; r.movementTerrainFlags = 0x1800; r.heading = 1.0f;
    r.turnSpeedPercent = 40; r.verticalSpeedPercent = -50; r.horizontalSpeedPercent = 75;
    check(ta::client::displayScriptGet(r, 4) == 50 && ta::client::displayScriptGet(r, 18) == 1 &&
          ta::client::displayScriptGet(r, 17) == 7, "HEALTH, YARD_OPEN, BUILD_PERCENT_LEFT answer");
    bool none = true;
    for (int v : {0, -1, 21, 27, 28, 29, 30, 32, 33, 34, 46, 100}) none &= ta::client::displayScriptGet(r, v) == 0;
    check(none, "ids past 20 (and 0, -1) answer 0, as the simulation's do");
    if (g_fail) { std::printf("ta_get_range_test: %d FAILED\n", g_fail); return 1; }
    std::printf("ta_get_range_test: all passed\n");
    return 0;
}
