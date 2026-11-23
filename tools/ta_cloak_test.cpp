// TA cloaking (docs/retail-economy.md section 2, 0x4017d9..0x401872): at each
// once-a-second settle a cloak-wanting unit pays trunc(cloakcost) -- or
// trunc(cloakcostmoving) while moving -- from stored energy at once, and is
// cloaked; when storage cannot pay it the cloak drops. Work blocks it.
#include "sim/sim.h"

#include <cstdio>
#include <string>
#include <vector>

namespace ta::sim {
struct RetailReplayProbe {   // a friend of World
    static void block(World& w, int id, uint32_t ticks) { w.blockCloak(*w.unit(id), ticks); }
};
}
using namespace ta;
static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += !ok;
}

static sim::UnitType cloaker() {
    sim::UnitType t;
    t.id = "cloaker"; t.maxHp = 100; t.canMove = true; t.upright = true;
    t.maxVel = sim::Fixed::fromInt(1); t.footX = t.footZ = 2;
    t.canCloak = true; t.cloakCost = 100.7f; t.cloakCostMove = 300;
    return t;
}

int main() {
    auto run = [](float energy, int ticks, sim::World& w, int& id, const sim::UnitType& t) {
        w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        w.setPlayerCount(1);
        w.setStartResources(0, energy, 1000);
        id = w.spawn(&t, 400, 400, 0.0f, 0);
        w.setCloak(id, true);
        for (int i = 0; i < ticks; ++i) w.tick(1.f / 30.f);
    };
    const auto t = cloaker();
    {
        sim::World w; int id = -1;
        run(1000, 61, w, id, t);
        const float stored = w.books(0).stored[0];
        check(w.unit(id) && w.unit(id)->cloaked, "a unit with energy in store cloaks");
        // Two or three settles have run by tick 61; each took trunc(100.7) = 100.
        check(stored <= 1000 - 200 + 0.01f && stored >= 1000 - 300 - 0.01f,
              "each settle takes trunc(cloakcost) energy (stored " + std::to_string(stored) + ")");
        check(int(stored) % 100 == 0, "the cost is truncated to whole energy");
    }
    {
        sim::World w; int id = -1;
        run(50, 61, w, id, t);
        check(w.unit(id) && !w.unit(id)->cloaked, "storage that cannot pay keeps the unit visible");
    }
    {
        sim::World w; int id = -1;
        run(1000, 35, w, id, t);
        sim::RetailReplayProbe::block(w, id, 300);
        w.tick(1.f / 30.f);
        check(!w.unit(id)->cloaked, "work (blockCloak) decloaks at once");
        const float before = w.books(0).stored[0];
        for (int i = 0; i < 60; ++i) w.tick(1.f / 30.f);
        check(!w.unit(id)->cloaked && w.books(0).stored[0] >= before,
              "a blocked unit neither cloaks nor pays");
    }
    std::printf(failures ? "FAILED (%d)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
