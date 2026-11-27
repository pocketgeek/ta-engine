// COB EXPLODE debris (client/retaildebris.h) against TotalA.exe: the launch
// 0x481140, the whole-piece tick 0x4213b0 and the SHATTER fragment's tick
// (inline in 0x420f30). docs/retail-weapon-scripts.md "Debris".
#include "client/retaildebris.h"
#include <cstdio>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main() {
    using M = ta::RetailDebrisMotion;
    // 0x4811d0..0x481238: the descriptor flags.
    check(M::descriptorFlags(1) == M::kShatter, "SHATTER");
    check(M::descriptorFlags(3) == (M::kShatter | M::kShatterExplodeOnHit), "SHATTER|EXPLODE_ON_HIT");
    check(M::descriptorFlags(2 | 4 | 8 | 16) == (M::kExplodeOnHit | M::kFall | M::kSmoke | M::kFire),
          "EXPLODE_ON_HIT|FALL|SMOKE|FIRE");
    // Launch: six draws in order, spins (r2, r3, r1), speeds << 14 / << 16.
    {
        std::vector<unsigned> bounds;
        const unsigned values[] = {1, 2, 3, 0, 9, 39};
        size_t next = 0;
        M m;
        check(M::launch(4, [&](unsigned b) { bounds.push_back(b); return values[next++]; }, m), "launch");
        check(bounds == std::vector<unsigned>({3000, 3000, 3000, 40, 10, 40}), "draw order and bounds");
        check(m.spin[0] == 2 && m.spin[1] == 3 && m.spin[2] == 1, "spin mapping");
        check(m.velocity[0] == 20 * 16384 && m.velocity[1] == 9 * 65536 && m.velocity[2] == -19 * 16384,
              "launch speeds");
        check(m.remaining == 900 && m.flags == M::kFall, "lifetime and FALL");
        check(!M::launch(0x20 | 4, [](unsigned) { return 0u; }, m), "BITMAPONLY makes no debris");
    }
    const auto flat = [](int h) { return [h](const std::array<int32_t, 3>&) { return h; }; };
    // A FALL piece goes up, comes down under gravity, bounces at half speed and
    // ends with the burst once a bounce is under 2 px a tick.
    {
        M m;
        M::launch(4 | 2, [](unsigned b) { return b == 10 ? 9u : 20u; }, m);   // vy 9 px/tick, no drift
        m.position = {100 << 16, 50 << 16, 100 << 16};
        int ticks = 0, bounces = 0, top = 0;
        int32_t lastVy = m.velocity[1];
        M::Result r{true};
        while ((r = m.tick(0, 8155, false, false, flat(20))).alive && ticks < 2000) {
            ++ticks;
            top = std::max(top, m.position[1] >> 16);
            if (m.velocity[1] > 0 && lastVy <= 0) ++bounces;
            lastVy = m.velocity[1];
        }
        check(ticks < 300, "a falling piece comes back down");
        check(top > 50 + 30, "it arcs up first");
        check(bounces >= 1, "it bounces");
        check(r.effect == ta::RetailFx::Explosion && r.light == 0, "EXPLODE_ON_HIT bursts on landing");
    }
    // Without FALL a piece keeps its speed (retail too) until its 900 updates.
    {
        M m;
        M::launch(0, [](unsigned b) { return b == 10 ? 5u : 20u; }, m);
        m.position = {0, 50 << 16, 0};
        int ticks = 0;
        while (m.tick(0, 8155, false, false, flat(20)).alive) ++ticks;
        check(ticks == 900, "lifetime: 900 updates, the 901st ends it");
    }
    // Under the sea line: splash (lava's on a lavaworld), none with nosealeveltrigger.
    {
        M m;
        M::launch(2, [](unsigned) { return 20u; }, m);
        m.position = {0, 30 << 16, 0};
        auto r = m.tick(30, 8155, false, false, flat(10));
        check(!r.alive && r.effect == ta::RetailFx::WaterSplash && r.light == -1, "water splash");
        M l = m; l.remaining = 5;
        r = l.tick(30, 8155, true, false, flat(10));
        check(r.effect == ta::RetailFx::LavaSplash, "lava splash");
        M q = m; q.remaining = 5;
        r = q.tick(30, 8155, false, true, flat(10));
        check(!r.alive && r.effect == ta::RetailFx::None, "nosealeveltrigger: no splash");
    }
    // SHATTER fragments always fall: vy starts with 30 * gravity, and they land.
    {
        std::vector<unsigned> bounds;
        M f = M::fragment(M::kShatter | M::kShatterExplodeOnHit, 8155, {0, 1, 0},
                          [&](unsigned b) { bounds.push_back(b); return b / 2; });
        check(bounds == std::vector<unsigned>({160, 160, 160, 1600, 1600, 1600, 200, 200}),
              "fragment draw order");
        check(f.velocity[1] == 8155 * 30 && f.velocity[0] == 0 && f.velocity[2] == 0, "fragment speeds");
        f.position = {100 << 16, 40 << 16, 100 << 16};
        int ticks = 0;
        M::Result r{true};
        while ((r = f.tick(0, 8155, false, false, flat(20))).alive && ticks < 2000) ++ticks;
        check(ticks < 300, "a fragment comes back down");
        check(r.effect == ta::RetailFx::Explosion && r.light == 0, "fragment bursts when it explodes on hit");
    }
    if (failures) return 1;
    std::printf("debris_motion: ok\n");
    return 0;
}
