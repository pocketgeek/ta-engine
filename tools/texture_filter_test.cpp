// texture_filter_test -- the Texture Filtering policy (client/texturefilter.h).
//
// Which art samples LINEAR is decided by two small functions: units follow the level
// alone, and Everything's world art (terrain, features, effects) follows a zoom gate
// with hysteresis. The gate is the part that can go subtly wrong -- an inverted band
// flickers every wheel notch, a missing band flickers on a slow benchmark zoom, and a
// gate that ignores the level smooths terrain for players who chose Off or Units -- so
// each case below walks the zoom the way the camera does and checks every frame.

#include "client/texturefilter.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace tf = tak::texturefilter;

static int g_fail = 0;

static void check(const char* what, bool ok) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

// Feed a zoom sequence through the gate frame by frame, as GameView does.
static std::vector<bool> walk(int level, const std::vector<float>& zooms, bool start = false) {
    std::vector<bool> out;
    bool on = start;
    for (float z : zooms) out.push_back(on = tf::worldLinear(level, z, on));
    return out;
}

int main() {
    std::printf("texture_filter_test: levels and the Everything zoom gate\n");

    check("levels clamp to Off..Everything", tf::clampLevel(-3) == tf::Off &&
          tf::clampLevel(1) == tf::Units && tf::clampLevel(7) == tf::Everything);
    check("units: Off nearest, Units and Everything linear",
          !tf::unitsLinear(tf::Off) && tf::unitsLinear(tf::Units) && tf::unitsLinear(tf::Everything));
    check("the gate opens above kSmoothZoom = 1.5", tf::kSmoothZoom == 1.5f);

    // Off and Units never smooth world art, at any zoom or prior state.
    {
        bool any = false;
        for (int level : {int(tf::Off), int(tf::Units)})
            for (float z = 0.05f; z <= 4.0f; z *= 1.118f)
                any |= tf::worldLinear(level, z, false) || tf::worldLinear(level, z, true);
        check("Off and Units: world art always nearest", !any);
    }

    // Everything at the zooms people actually play: game start is 1.25x, default
    // retail-like views are 1.0x. Neither may be smoothed (texels are 1-2 px there).
    check("Everything: nearest at 1.0x and 1.25x",
          !tf::worldLinear(tf::Everything, 1.0f, false) && !tf::worldLinear(tf::Everything, 1.25f, false));
    check("Everything: exactly 1.5x does not yet switch on", !tf::worldLinear(tf::Everything, 1.5f, false));
    check("Everything: linear at 2x and 4x",
          tf::worldLinear(tf::Everything, 2.0f, false) && tf::worldLinear(tf::Everything, 4.0f, false));

    // Hysteresis: inside the band [1.4, 1.5] the previous answer holds.
    check("band holds ON at 1.45 and 1.4",
          tf::worldLinear(tf::Everything, 1.45f, true) && tf::worldLinear(tf::Everything, 1.4f, true));
    check("band holds OFF at 1.45", !tf::worldLinear(tf::Everything, 1.45f, false));
    check("below the band switches off (1.39)", !tf::worldLinear(tf::Everything, 1.39f, true));

    // Wheel walk from game start: 1.25 -> x1.118 notches in, then back out. The 1.25
    // -> 1.3975 -> 1.5625 steps turn it on at the second notch; on the way out it stays
    // on through 1.5625 and goes off at 1.3975, below the band.
    {
        std::vector<float> zooms;
        float z = 1.25f;
        for (int i = 0; i < 3; ++i) { zooms.push_back(z); z *= 1.118f; }
        for (int i = 0; i < 3; ++i) { zooms.push_back(z); z /= 1.118f; }
        const auto got = walk(tf::Everything, zooms);
        const std::vector<bool> want{false, false, true, true, true, false};
        check("wheel notches in and out from 1.25x", got == want);
    }

    // Jitter around the threshold (a slow scripted zoom, or float noise) must not
    // flicker: once on, wobbling between 1.42 and 1.55 keeps it on; once off,
    // wobbling between 1.41 and 1.50 keeps it off.
    {
        std::vector<float> wobble;
        for (int i = 0; i < 40; ++i) wobble.push_back(1.485f + 0.065f * std::sin(float(i)));
        int flips = 0;
        bool on = true;
        for (float z : wobble) { bool next = tf::worldLinear(tf::Everything, z, on); flips += next != on; on = next; }
        check("no flicker while on and wobbling 1.42..1.55", flips == 0 && on);
        std::vector<float> low;
        for (int i = 0; i < 40; ++i) low.push_back(1.455f + 0.045f * std::sin(float(i)));
        const auto got = walk(tf::Everything, low, false);
        bool anyOn = false;
        for (bool b : got) anyOn |= b;
        check("no flicker while off and wobbling 1.41..1.50", !anyOn);
    }

    // Leaving Everything turns world art off immediately, whatever the zoom.
    check("switching to Units at 4x turns world art off", !tf::worldLinear(tf::Units, 4.0f, true));

    std::printf(g_fail ? "texture_filter_test: %d FAILURE(S)\n" : "texture_filter_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
