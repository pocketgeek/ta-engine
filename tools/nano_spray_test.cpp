// The nanolathe spray (client/retailnano.h) against what tools/re/nano_emu.py
// reads off TotalA.exe's spray object (init 0x473b50, emit 0x473d50, tick
// 0x472eb0) and its callers (0x4720d0 / 0x472200).
#include "client/retailnano.h"
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

namespace nano = ta::client::nano;
static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main() {
    // 0x473b50: the box 100..144 x 0..22 x 50..94 keeps 116+12, 8+6, 66+12.
    {
        const auto x = nano::middle(100, 144), y = nano::middle(0, 22), z = nano::middle(50, 94);
        check(x.lo == 116 && x.size == 12, "middle x");
        check(y.lo == 8 && y.size == 6, "middle y");
        check(z.lo == 66 && z.size == 12, "middle z");
        const auto p = nano::middle(7, 7);
        check(p.lo == 7 && p.size == 0, "a point stays a point");
    }
    // 0x473ebd..0x473ed5: four pixels a tick, whole ticks; the emulated
    // particles of 144.75 and 135.84 px live 36 and 33 ticks; under 4 px none.
    check(nano::travelTicks(144.75f) == 36, "ticks for 144.75 px");
    check(nano::travelTicks(135.84f) == 33, "ticks for 135.84 px");
    check(nano::travelTicks(3.99f) == 0, "no particle under 4 px");
    // 0x473f41 / 0x4739b0: 0xa1 + i at birth, one green further each tick, 0xa7 -> 0xa1.
    check(nano::colour(0, 0) == 0xa1 && nano::colour(4, 0) == 0xa5, "birth colours");
    check(nano::colour(0, 1) == 0xa2 && nano::colour(4, 1) == 0xa6, "one tick on");
    check(nano::colour(4, 3) == 0xa1 && nano::colour(3, 3) == 0xa7, "wraps after 0xa7");
    // The callers.
    check(!nano::reverse(nano::Work::Build) && !nano::reverse(nano::Work::Repair) &&
          !nano::reverse(nano::Work::Resurrect), "build/repair/resurrect spray piece -> box");
    check(nano::reverse(nano::Work::ReclaimUnit) && nano::reverse(nano::Work::ReclaimFeature) &&
          nano::reverse(nano::Work::Capture), "reclaim/capture spray box -> piece");
    check(nano::callsPerWake(nano::Work::ReclaimFeature) == 2 && nano::callsPerWake(nano::Work::Build) == 1,
          "feature reclaim calls 0x472200 twice");
    check(nano::wakeTicks(nano::Work::Build) == 1 && nano::wakeTicks(nano::Work::ReclaimUnit) == 2,
          "wake cadence");

    // One call: five particles at once, five more on the next tick, none
    // after; all from the point to the box's middle, at 4..8 px a tick.
    {
        nano::Sprayer s;
        std::vector<nano::Dot> dots;
        uint32_t seed = 1;
        auto rand = [&] { seed = seed * 0x343fdu + 0x269ec3u; return (seed >> 16) & 0x7fffu; };
        auto sink = [&](const nano::Dot& d) { dots.push_back(d); };
        auto drop = [](uint32_t) {};
        const nano::Box src = nano::point(0, 10, 0), dst{100, 0, 50, 144, 22, 94};
        s.call(1000, src, dst, rand, sink, drop);
        check(dots.size() == 5, "first batch: five");
        s.tick(1000, rand, sink);
        check(dots.size() == 5, "no second batch the same tick");
        s.tick(1001, rand, sink);
        check(dots.size() == 10, "second batch on the next tick");
        s.tick(1002, rand, sink);
        s.tick(1003, rand, sink);
        check(dots.size() == 10, "no third batch");
        for (size_t i = 0; i < dots.size(); ++i) {
            const auto& d = dots[i];
            check(d.birth == (i < 5 ? 1000u : 1001u), "birth ticks");
            check(d.index == i % 5, "batch index");
            check(d.x == 0 && d.y == 10 && d.z == 0, "starts at the point");
            float x, y, z;
            d.at(d.birth + uint32_t(d.ticks), x, y, z);
            check(x >= 116 - 0.01f && x <= 128 + 0.01f && y >= 8 - 0.01f && y <= 14 + 0.01f &&
                  z >= 66 - 0.01f && z <= 78 + 0.01f, "ends in the box's middle");
            const float speed = std::sqrt(d.vx * d.vx + d.vy * d.vy + d.vz * d.vz);
            check(speed >= 4.0f && speed < 8.0f, "4..8 px a tick");
            check(!d.dead(d.birth + uint32_t(d.ticks)) && d.dead(d.birth + uint32_t(d.ticks) + 1),
                  "gone the tick after it arrives (0x473b30)");
        }
    }
    // The reverse: box -> point.
    {
        nano::Sprayer s;
        std::vector<nano::Dot> dots;
        uint32_t seed = 7;
        auto rand = [&] { seed = seed * 0x343fdu + 0x269ec3u; return (seed >> 16) & 0x7fffu; };
        s.call(10, nano::Box{100, 0, 50, 144, 22, 94}, nano::point(0, 10, 0), rand,
               [&](const nano::Dot& d) { dots.push_back(d); }, [](uint32_t) {});
        for (const auto& d : dots) {
            check(d.x >= 116 && d.x <= 128 && d.z >= 66 && d.z <= 78, "reverse starts in the box");
            float x, y, z; d.at(d.birth + uint32_t(d.ticks), x, y, z);
            check(std::fabs(x) < 8 && std::fabs(z) < 8, "reverse ends at the point");
        }
    }
    // 0x47219f: past 400 live objects the oldest is deleted with its particles.
    {
        nano::Sprayer s;
        std::set<uint32_t> dropped;
        uint32_t seed = 3;
        auto rand = [&] { seed = seed * 0x343fdu + 0x269ec3u; return (seed >> 16) & 0x7fffu; };
        auto sink = [](const nano::Dot&) {};
        auto drop = [&](uint32_t o) { dropped.insert(o); };
        const nano::Box src = nano::point(0, 0, 0), dst{400, 0, 400, 432, 20, 432};
        for (int i = 0; i < 401; ++i) s.call(5, src, dst, rand, sink, drop);
        check(dropped.empty() && s.objects() == 401, "401 objects fit");
        s.call(5, src, dst, rand, sink, drop);
        check(dropped.size() == 1 && *dropped.begin() == 1 && s.objects() == 401, "the 402nd drops the oldest");
        // Long after, every object is finished: the cap counts none of them.
        s.tick(6, rand, sink);   // their second batches
        s.call(5000, src, dst, rand, sink, drop);
        check(s.objects() == 1 && dropped.size() == 1, "finished objects are gone");
    }
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("nano_spray: ok");
    return 0;
}
