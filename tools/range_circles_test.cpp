// Retail's Shift range circles and area-order colours (client/orderoverlay.h).
// With --rows / --ring it prints the port's answers for tools/re/check_range_circles.py,
// which runs retail's 0x4d6530 / 0x4d5c40 under emulation on the same inputs.
#include "client/orderoverlay.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace tak::client;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main(int argc, char** argv) try {
    if (argc > 1 && std::strcmp(argv[1], "--rows") == 0) {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::istringstream in(line);
            RangeCircleInput u;
            int cloaked = 0, kamikaze = 0, explode = 0, mobile = 0;
            uint32_t tick = 0;
            in >> u.minCloakDistance >> cloaked >> kamikaze >> explode >> u.explodeAoe >>
                u.kamikazeDistance >> u.sightDistance >> mobile >> tick;
            u.cloaked = cloaked; u.kamikaze = kamikaze; u.hasExplodeAs = explode; u.mobile = mobile;
            std::string out;
            for (const auto& c : retailRangeCircles(u, tick)) {
                char b[32];
                std::snprintf(b, sizeof b, "%s%d:%08x", out.empty() ? "" : " ", c.radius, c.argb);
                out += b;
            }
            std::printf("%s\n", out.c_str());
        }
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--ring") == 0) {
        int r;
        while (std::cin >> r) {
            const int n = retailCircleSegments(r);
            std::printf("%d", n);
            for (int k = 0; k <= n && n > 0; ++k) {
                const auto o = retailCirclePoint(r, n, k);
                std::printf(" %.3f,%.3f", o.dx, o.dz);
            }
            std::printf("\n");
        }
        return 0;
    }

    // One segment per 8 world units of circumference, truncated.
    check(retailCircleSegments(0) == 0 && retailCircleSegments(1) == 0, "a ring under 8 units round draws nothing");
    check(retailCircleSegments(2) == 1 && retailCircleSegments(50) == 39, "segments = trunc(r * 2pi / 8)");
    check(retailCircleSegments(80) == 62 && retailCircleSegments(100) == 78, "the shipped mincloak radii");
    check(retailCircleSegments(-5) == 0, "a negative radius draws nothing");
    {   // Point 0 is due +z; a quarter of the way round is +x.
        const auto a = retailCirclePoint(80, 4, 0), b = retailCirclePoint(80, 4, 1);
        check(std::abs(a.dx) < 1e-3f && std::abs(a.dz - 80) < 1e-3f, "point 0 at +z");
        check(std::abs(b.dx - 80) < 1e-3f && std::abs(b.dz) < 1e-3f, "point 1 of 4 at +x");
    }
    // The kamikaze pulse: 8 -> aoe/2 over 30 ticks, held for the next 30.
    check(kamikazePulseRadius(0, 203) == 8 && kamikazePulseRadius(2, 203) == 8, "pulse floor is 8");
    check(kamikazePulseRadius(15, 203) == 50, "pulse halfway");
    check(kamikazePulseRadius(30, 203) == 101 && kamikazePulseRadius(59, 203) == 101, "pulse holds at aoe/2");
    check(kamikazePulseRadius(60, 203) == 8, "pulse restarts every 60 ticks");
    check(kamikazePulseRadius(5, 10) == 5, "a blast under 16 caps the floor at aoe/2");

    RangeCircleInput spy;
    spy.minCloakDistance = 80; spy.cloaked = true; spy.mobile = true; spy.sightDistance = 200;
    auto c = retailRangeCircles(spy, 0);
    check(c.size() == 1 && c[0].radius == 80 && c[0].argb == 0xffffffffu, "a cloaked unit: one white mincloak ring");
    spy.cloaked = false;
    check(retailRangeCircles(spy, 0).empty(), "an uncloaked unit shows no mincloak ring");
    RangeCircleInput kam;
    kam.kamikaze = true; kam.hasExplodeAs = true; kam.explodeAoe = 203; kam.kamikazeDistance = 64;
    kam.sightDistance = 100; kam.mobile = true;
    c = retailRangeCircles(kam, 30);
    check(c.size() == 2 && c[0].radius == 101 && c[1].radius == 64 && c[0].argb == 0xffff0000u &&
              c[1].argb == 0xffff0000u, "kamikaze: red pulse then red kamikazedistance");
    kam.mobile = false;
    check(retailRangeCircles(kam, 30)[1].radius == 100, "a kamikaze without a mover rings its sight distance");
    kam.hasExplodeAs = false;
    check(retailRangeCircles(kam, 30).empty(), "kamikaze without [EXPLODEAS] shows nothing");

    // Mission kinds: an empty queue is the standby; a queue of only flagless kinds is not.
    check(missionsShowRangeCircles(0, [](size_t) { return true; }), "standby shows the rings");
    check(!missionsShowRangeCircles(2, [](size_t) { return true; }), "only load/unload/area reclaim: no rings");
    check(missionsShowRangeCircles(2, [](size_t i) { return i == 0; }), "any flagged mission shows them");

    // Area-order rectangles (0x4d5180): white, yellow when the ordering unit is selected.
    check(retailAreaOrderArgb(false) == 0xffffffffu && retailAreaOrderArgb(true) == 0xffffff00u,
          "area rectangle colours");
    std::printf("range_circles: ok\n");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
