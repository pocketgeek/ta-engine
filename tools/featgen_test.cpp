// featgen_test -- the contract behind World::featGeneration().
//
// The renderer's feature sync (GameView::syncBurningFeatures) used to take simMutex_
// and rescan every feature every cosmetic step to discover changes that, measured over
// 45s, happened on 0% of steps: ~1ms of pure LOCK WAIT per step for nothing (the scan
// itself was 0.012ms). It now skips both locked scans while featGeneration() is
// unchanged, which makes this counter load-bearing in two directions:
//
//   * A MISSED bump silently freezes feature visuals -- a tree that ignites, burns to
//     its next stage or is reclaimed away would never update on screen.
//   * A SPURIOUS bump (e.g. bumping every tick) puts the lock wait straight back and
//     the optimization is worth nothing.
//
// So both directions are asserted here. Only the add path is reachable from outside the
// class -- igniteFeature/swapFeature are private -- so the ignite path is covered
// empirically instead (verified by driving a real game: 5 forced ignitions produced 5
// renderer-side burn-vis transitions).

#include "sim/sim.h"

#include <cstdio>

using ta::sim::World;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  %-64s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}

int main() {
    std::printf("featgen_test\n");

    World w;
    // Headless: no fog work, no renderer. Matches how the referee runs.
    w.setVisPlayer(-1);

    const uint32_t atStart = w.featGeneration();

    // Ticking an empty world must not move the counter. This is the property that makes
    // the render-side skip worth anything -- if idle ticks bumped it, the sync would
    // take the lock every step exactly as before.
    for (int i = 0; i < 30; ++i) w.tick(1.0f / 30.0f);
    check(w.featGeneration() == atStart,
          "an idle world does not bump the generation (no spurious rescans)");

    // Adding a feature must move it: the renderer has to notice a new feature to give
    // it a visual instance at all.
    w.addFeature(1, 100.0f, 100.0f, 10.0f, 5.0f, 1, 1, false, -1);
    const uint32_t afterOne = w.featGeneration();
    check(afterOne != atStart, "adding a feature bumps the generation");
    check(w.features().size() == 1, "and the feature is actually there");

    // Distinct adds must be distinguishable, not collapsed into one edge.
    w.addFeature(2, 200.0f, 200.0f, 10.0f, 5.0f, 1, 1, false, -1);
    check(w.featGeneration() != afterOne, "a second add bumps it again");

    // ...and ticking after the adds must go quiet again, so the sync settles back to
    // the lock-free path instead of staying permanently dirty.
    const uint32_t afterAdds = w.featGeneration();
    for (int i = 0; i < 30; ++i) w.tick(1.0f / 30.0f);
    check(w.featGeneration() == afterAdds,
          "ticking with features present still does not bump it");

    std::printf(g_fail ? "featgen_test: %d FAILURE(S)\n" : "featgen_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
