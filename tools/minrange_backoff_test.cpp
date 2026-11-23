// minrange_backoff_test: a unit with a minimum range backs off from every enemy
// that comes too close, not only from the first three.
//
//   minrange_backoff_test <retail-install-dir>
//
// Engine deviation (owner, 2026-11-12): retail's 52fe30 refuses to aim inside a
// weapon's minrange and the unit stands there for as long as the order lasts;
// ours walks to a spot in its range band (World::repositionForShot) and fires.
// The walks come out of the blocked-shot budget, three per target between hits
// on it (kMaxRepositions). The minimum-range path spent them but never took its
// target into that bookkeeping, so neither a new target nor a hit gave them
// back: after three back-offs in a unit's life an Attack on any enemy inside the
// minimum range was dropped the tick it was given, and the unit stood idle.
//
// A shipped Tortoise (cretort, minrange 64), ordered onto
// six enemies in turn, every one placed inside its minimum range: each order
// must hold, the unit must walk out of the minimum range, and it must fire.
// The enemies are a shipped Beast Tamer that cannot move, fight or die here.
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ta;

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void makeWorld(sim::World& w, const sim::TypeRegistry& reg) {
    w.setSerialThreads(true);
    w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(256 * 256, 100), 256, 256, 20);
    w.setMapPlacementFeatures(std::vector<uint16_t>(256 * 256, 0xffff), {});
    w.buildNavClasses(reg);
    w.setPathService(true);
}

bool holds(const sim::World& w, int a, int b) {
    const sim::Unit* u = w.unit(a);
    return u && std::any_of(u->orders.begin(), u->orders.end(), [&](const auto& o) { return o.targetId == b; });
}

float distance(const sim::World& w, int a, int b) {
    const sim::Unit* u = w.unit(a);
    const sim::Unit* t = w.unit(b);
    return std::hypot(u->x.toFloat() - t->x.toFloat(), u->z.toFloat() - t->z.toFloat());
}

void tick(sim::World& w, int n) { for (int i = 0; i < n; ++i) w.tick(1.f / 30); }

void backOffSixTimes(const sim::TypeRegistry& reg, const char* balance, const char* shooterId) {
    const sim::UnitType* shooter = reg.find(shooterId);
    const sim::UnitType* tamer = reg.find("zontrain");
    if (!shooter || shooter->weapons.empty() || !tamer) {
        check(false, std::string(balance) + " " + shooterId + ": shipped types present");
        return;
    }
    // Stands still, has no weapon, outlives the test.
    sim::UnitType victim = *tamer;
    victim.maxVel = {};
    victim.weapons.clear();
    victim.weapon.damage = 0;
    victim.maxHp = 30000;
    const int minRange = shooter->weapons.front().minRange;
    std::printf("%s %s: minrange %d, range %d\n", balance, shooterId, minRange, shooter->weapons.front().range);
    check(minRange > 0, std::string(balance) + " " + shooterId + " has a minimum range");

    sim::World w;
    makeWorld(w, reg);
    const int a = w.spawn(shooter, 2000, 2000, 0, 0);
    tick(w, 30);
    for (int round = 1; round <= 6; ++round) {
        const sim::Unit* u = w.unit(a);
        const float gap = std::max(40.f, float(minRange) * 0.5f);
        const int b = w.spawn(&victim, u->x.toFloat() + gap, u->z.toFloat(), 0, 1);
        w.attack(a, b, false);
        tick(w, 1);
        const bool heldAtOnce = holds(w, a, b);
        int shots = 0, held = 0;
        for (int k = 0; k < 1500; ++k) {
            w.tick(1.f / 30);
            if (w.unit(a)->justFired) ++shots;
            if (holds(w, a, b)) ++held;
        }
        const float end = distance(w, a, b);
        std::printf("  round %d: enemy %.0f px away; order held after one tick %d, for %d of 1500 ticks; "
                    "%d shots; ends %.0f px away; walks %d\n",
                    round, gap, int(heldAtOnce), held, shots, end, int(w.unit(a)->repositions));
        const std::string what = std::string(balance) + " " + shooterId + " enemy " + std::to_string(round);
        check(heldAtOnce && held == 1500, what + ": the order holds");
        check(end > float(minRange), what + ": backs out of the minimum range");
        check(shots > 0, what + ": fires at it");
        w.stop(a);
        if (sim::Unit* v = w.unit(b)) v->hp = sim::Fixed();
        tick(w, 90);
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: minrange_backoff_test <retail-install-dir>\n");
        return 2;
    }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry standard;
    sim::setupRegistry(standard, vfs);
    backOffSixTimes(standard, "standard", "cretort");
    if (failures) {
        std::printf("minrange_backoff: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("minrange_backoff: all passed\n");
    return 0;
}
