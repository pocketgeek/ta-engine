// Pins the terrain-height lift (src/client/terrainlift.h) to retail's projection:
//     screenY = z - (y >> 1) - cameraY     (KINGDOMS.icd 0x4fcd1f units, 0x421dad features)
// with y the ABSOLUTE height from retail's sampler 0x511170 (corner-anchored
// bilinear, each step truncated toward zero). The formula is written out here a
// second time, by hand, so a change to either copy shows.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "client/terrainlift.h"

namespace lift = tak::client::lift;

static int failures = 0;
static void check(bool ok, const char* what, double got, double want) {
    if (ok) return;
    std::printf("FAIL: %s: got %g want %g\n", what, got, want);
    ++failures;
}

// 0x511170 by hand: cell = px >> 4, fraction = px & 15, height at +4 of the
// 14-byte cell record, the next column at +0x12, C division (toward zero).
static int retailSample(const std::vector<uint8_t>& h, int w, int px, int pz) {
    const int cx = px >> 4, cz = pz >> 4, fx = px & 15, fz = pz & 15;
    const auto at = [&](int x, int z) { return int(h[size_t(z) * size_t(w) + size_t(x)]); };
    const int top = at(cx, cz) + (at(cx + 1, cz) - at(cx, cz)) * fx / 16;
    const int bot = at(cx, cz + 1) + (at(cx + 1, cz + 1) - at(cx, cz + 1)) * fx / 16;
    return top + (bot - top) * fz / 16;
}

int main() {
    // The y >> 1 shift: odd heights round down.
    check(lift::screenLift(220) == 110, "lift(220)", lift::screenLift(220), 110);
    check(lift::screenLift(221) == 110, "lift(221)", lift::screenLift(221), 110);
    check(lift::screenLift(1) == 0, "lift(1)", lift::screenLift(1), 0);
    check(lift::screenLift(255) == 127, "lift(255)", lift::screenLift(255), 127);
    check(lift::kScaleY == 0.5f, "scale", lift::kScaleY, 0.5);

    // A map that is all one height lifts by that height / 2: no per-map datum.
    // (0.9.19 subtracted the modal height and drew this at 0, 110px south.)
    {
        const int w = 8, d = 8;
        std::vector<uint8_t> flat(size_t(w * d), 220);
        for (float x : {0.0f, 37.5f, 64.0f, 111.9f})
            for (float z : {0.0f, 50.25f, 100.0f}) {
                const float y = lift::terrainHeight(flat.data(), w, d, x, z);
                check(y == 220.0f, "flat plateau height", y, 220);
                check(lift::screenLift(int(y)) == 110.0f, "flat plateau lift", lift::screenLift(int(y)), 110);
            }
    }

    // Slopes in both directions against the hand-written sampler, every pixel.
    {
        const int w = 6, d = 5;
        const uint8_t cells[] = {
              0,  40, 220, 220, 100,  23,
             23, 200, 255, 193,   0,   0,
            220, 220,  25,  62, 185,   1,
              7, 130, 221, 219,   2, 250,
              0,   0,   0, 255, 255, 255};
        const std::vector<uint8_t> h(std::begin(cells), std::end(cells));
        for (int pz = 0; pz < (d - 1) * 16; ++pz)
            for (int px = 0; px < (w - 1) * 16; ++px) {
                const int want = retailSample(h, w, px, pz);
                // A sub-pixel position samples its whole pixel, as retail's >> 16.
                const float got = lift::terrainHeight(h.data(), w, d, float(px) + 0.75f, float(pz) + 0.25f);
                check(got == float(want), "slope sample", got, want);
                if (got != float(want)) { std::printf("  at px=%d pz=%d\n", px, pz); return 1; }
            }
        // The height of cell (cx, cz) sits on its north-west corner, not its centre.
        for (int cz = 0; cz < d - 1; ++cz)
            for (int cx = 0; cx < w - 1; ++cx)
                check(lift::terrainHeight(h.data(), w, d, float(cx * 16), float(cz * 16)) ==
                          float(h[size_t(cz * w + cx)]),
                      "corner anchor", lift::terrainHeight(h.data(), w, d, float(cx * 16), float(cz * 16)),
                      h[size_t(cz * w + cx)]);
        // Off the map the edge holds instead of retail's -1 sentinel.
        const float edge = lift::terrainHeight(h.data(), w, d, 1e5f, 1e5f);
        check(edge == float(retailSample(h, w, (w - 1) * 16 - 1, (d - 1) * 16 - 1)), "clamped edge", edge,
              retailSample(h, w, (w - 1) * 16 - 1, (d - 1) * 16 - 1));
        check(lift::terrainHeight(h.data(), w, d, -50.0f, -3.0f) == 0.0f, "clamped origin",
              lift::terrainHeight(h.data(), w, d, -50.0f, -3.0f), 0);
    }

    // Bodies on water (0x51b220 canhover, 0x51b2fa floater).
    check(lift::bodyHeight(10, 100, 20, false, false) == 10, "walker on seabed", lift::bodyHeight(10, 100, 20, false, false), 10);
    check(lift::bodyHeight(10, 100, 20, true, false) == 80, "wader over deep water", lift::bodyHeight(10, 100, 20, true, false), 80);
    check(lift::bodyHeight(150, 100, 20, true, false) == 150, "wader on land", lift::bodyHeight(150, 100, 20, true, false), 150);
    check(lift::bodyHeight(10, 100, 20, false, true) == 80, "floater", lift::bodyHeight(10, 100, 20, false, true), 80);
    check(lift::bodyHeight(95, 100, 20, false, true) == 80, "floater on a shoal", lift::bodyHeight(95, 100, 20, false, true), 80);

    if (failures) return 1;
    std::puts("PASS: terrain lift is retail's z - (y >> 1) with the 0x511170 sampler, no datum");
    return 0;
}
