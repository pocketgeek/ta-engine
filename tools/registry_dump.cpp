// registry_dump <install-dir> [unit...]: load the unit registry the way a game does
// and print what TA data produced (counts, and per-unit economy + weapons). A data
// check against docs/retail-data.md, not a test of game behaviour.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sides.h"
#include "sim/sim.h"
#include "sim/taweapons.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: registry_dump <install-dir> [unit...]\n"); return 2; }
    auto vfs = ta::hpi::mountRetailRoot(argv[1]);
    ta::sim::TypeRegistry reg;
    ta::sim::setupRegistry(reg, vfs, false);
    size_t armed = 0, builders = 0;
    for (const auto& [id, t] : reg.types()) {
        if (!t.weapons.empty()) ++armed;
        if (t.isBuilder) ++builders;
    }
    std::printf("units %zu (armed %zu, builders %zu), sides %d\n", reg.types().size(), armed, builders,
                ta::sides::count());
    for (int i = 0; i < ta::sides::count(); ++i)
        std::printf("side %d %s prefix %s commander %s -> %s\n", i, ta::sides::at(i).name.c_str(),
                    ta::sides::at(i).prefix.c_str(), ta::sides::commander(i).c_str(),
                    reg.find(ta::sides::commander(i)) ? "found" : "MISSING");
    for (int a = 2; a < argc; ++a) {
        const auto* t = reg.find(argv[a]);
        if (!t) { std::printf("%s: not loaded\n", argv[a]); continue; }
        std::printf("%s '%s' side=%s model=%s hp=%d cost m%.0f e%.0f buildtime %.0f worker %.0f\n",
                    t->id.c_str(), t->name.c_str(), t->side.c_str(), t->objectName.c_str(), t->maxHp,
                    t->buildCostMetal, t->buildCostEnergy, t->buildTime, t->workerTime);
        std::printf("  make e%.2f m%.2f use e%.2f extract %.4f makesmetal %d wind %.1f tidal %.1f store e%.0f m%.0f\n",
                    t->energyMake, t->metalMake, t->energyUse, t->extractsMetal, t->makesMetal,
                    t->windGenerator, t->tidalGenerator, t->energyStorage, t->metalStorage);
        std::printf("  foot %dx%d yard '%s' sight %d cloak %.0f/%.0f mincloak %d selfd %d commander %d builds %zu\n",
                    t->footX, t->footZ, t->yardMap.c_str(), int(t->sight), t->cloakCost, t->cloakCostMove,
                    int(t->minCloakDist), t->selfDestructCountdown, int(t->commander), reg.buildable(t->id).size());
        for (const auto& w : t->weapons)
            std::printf("  weapon [%s] id %d '%s' range %d reload %u dmg %d aoe %d vel %.0f aim %d e/shot %.0f\n",
                        w.section.c_str(), w.taId, w.name.c_str(), w.range, w.reloadTicks, w.damage, w.aoe,
                        w.projVel, int(w.taAim), w.energyPerShot);
        if (t->hasExplodeAs) std::printf("  explodeas [%s] dmg %d aoe %d\n", t->explodeAs.section.c_str(),
                                         t->explodeAs.damage, t->explodeAs.aoe);
    }
    return 0;
}
