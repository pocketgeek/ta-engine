// Shift-drag build-line geometry (client/buildline.h): every site sits on the
// cells canPlace tests, consecutive sites never share a cell in any drag
// direction, gap 0 is footprints touching and gap N leaves exactly N empty cells
// on the axis that separates them. Also the wheel's one-step-per-notch rule.
#include "client/buildline.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace tak::client;
using tak::sim::footprintOrigin;

static void check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

struct Cell { int x, z; };
static Cell origin(const std::pair<float, float>& p, int fx, int fz) {
    return {footprintOrigin(p.first, fx), footprintOrigin(p.second, fz)};
}

int main() try {
    const int foots[][2] = {{1, 1}, {2, 2}, {3, 3}, {4, 2}, {2, 5}, {6, 4}, {7, 7}};
    int lines = 0, sites = 0;
    for (const auto& f : foots) {
        const int fx = f[0], fz = f[1];
        for (int gap = 0; gap <= 4; ++gap)
            for (int deg = 0; deg < 360; deg += 5) {
                // Unaligned start: the drag never begins on a cell boundary.
                const float x0 = 1003.3f, z0 = 997.7f, len = 900.0f;
                const float a = float(deg) * 3.14159265f / 180.0f;
                const float x1 = x0 + std::cos(a) * len, z1 = z0 + std::sin(a) * len;
                const auto line = buildLineSites(fx, fz, gap, x0, z0, x1, z1);
                const std::string at = std::to_string(fx) + "x" + std::to_string(fz) + " gap " +
                                       std::to_string(gap) + " at " + std::to_string(deg) + " deg: ";
                check(line.size() >= 2, at + "a 900px drag gave fewer than two sites");
                for (size_t i = 0; i < line.size(); ++i) {
                    const auto c = origin(line[i], fx, fz);
                    // The Build order's own snap keeps the same cells.
                    const float sx = tak::sim::footprintWaypoint(tak::sim::footprintCell(line[i].first, fx), fx).toFloat();
                    const float sz = tak::sim::footprintWaypoint(tak::sim::footprintCell(line[i].second, fz), fz).toFloat();
                    check(origin({sx, sz}, fx, fz).x == c.x && origin({sx, sz}, fx, fz).z == c.z,
                          at + "a site moves cells under the Build order's snap");
                    check(sx == line[i].first && sz == line[i].second, at + "a site is not already on the snapped point");
                    for (size_t j = 0; j < i; ++j) {
                        const auto o = origin(line[j], fx, fz);
                        check(std::abs(c.x - o.x) >= fx || std::abs(c.z - o.z) >= fz,
                              at + "sites " + std::to_string(j) + " and " + std::to_string(i) + " share a cell");
                    }
                    if (!i) continue;
                    const auto p = origin(line[i - 1], fx, fz);
                    const int dx = std::abs(c.x - p.x), dz = std::abs(c.z - p.z);
                    // The empty cells between neighbours, on the axis that separates them.
                    check(std::max(dx - fx, dz - fz) == gap,
                          at + "neighbours are " + std::to_string(std::max(dx - fx, dz - fz)) + " cells apart");
                    // The line follows the drag: each step points along it.
                    const float ux = float(c.x - p.x), uz = float(c.z - p.z);
                    check(ux * std::cos(a) + uz * std::sin(a) > 0, at + "a step points away from the drag");
                }
                // The far end stays within one site of the cursor.
                const auto last = line.back();
                const float rest = std::hypot(x1 - last.first, z1 - last.second);
                check(rest <= 16.0f * float(std::max(fx, fz) + gap) * 1.5f + 16.0f,
                      at + "line stops " + std::to_string(rest) + "px short of the cursor");
                ++lines; sites += int(line.size());
            }
    }
    {   // Straight runs: count and exact positions. A 2x2 dragged 10 cells right from
        // cell (10,10) touches every 2 cells: sites at origin x 10,12,..,20.
        const auto line = buildLineSites(2, 2, 0, 10 * 16 + 16, 10 * 16 + 16, 20 * 16 + 16, 10 * 16 + 16);
        check(line.size() == 6, "2x2 over 10 cells: " + std::to_string(line.size()) + " sites, expected 6");
        for (size_t i = 0; i < line.size(); ++i) {
            const auto c = origin(line[i], 2, 2);
            check(c.x == 10 + 2 * int(i) && c.z == 10, "2x2 row is not edge to edge from the start cell");
        }
        const auto wide = buildLineSites(2, 2, 3, 10 * 16 + 16, 10 * 16 + 16, 20 * 16 + 16, 10 * 16 + 16);
        check(wide.size() == 3 && origin(wide[1], 2, 2).x == 15 && origin(wide[2], 2, 2).x == 20,
              "gap 3 does not step 5 cells");
        // A vertical drag of a 4x2 (wide, shallow) steps 2 rows: the short side.
        const auto down = buildLineSites(4, 2, 0, 400, 400, 400, 400 + 16 * 8);
        check(down.size() == 5 && origin(down[1], 4, 2).z - origin(down[0], 4, 2).z == 2 &&
              origin(down[1], 4, 2).x == origin(down[0], 4, 2).x, "vertical 4x2 does not step its depth");
        // 45 degrees with squares: corner to corner, one footprint on each axis.
        const auto diag = buildLineSites(3, 3, 0, 500, 500, 500 + 16 * 9, 500 + 16 * 9);
        check(diag.size() == 4, "45-degree 3x3 over 9 cells gave " + std::to_string(diag.size()) + " sites");
        for (size_t i = 1; i < diag.size(); ++i) {
            const auto c = origin(diag[i], 3, 3), p = origin(diag[i - 1], 3, 3);
            check(c.x - p.x == 3 && c.z - p.z == 3, "45-degree 3x3 is not corner to corner");
        }
    }
    {   // A zero-length drag (a single shift-click) is one site on the clicked cells.
        const auto one = buildLineSites(3, 2, 5, 777.f, 555.f, 777.f, 555.f);
        check(one.size() == 1, "a shift-click is not one site");
        const auto c = origin(one[0], 3, 2);
        check(c.x == footprintOrigin(777.f, 3) && c.z == footprintOrigin(555.f, 2), "the click's cells moved");
        // A drag shorter than one step is still just the first site.
        check(buildLineSites(3, 3, 0, 500, 500, 530, 500).size() == 1, "a sub-step drag added a site");
        check(buildLineSites(1, 1, 99, 0, 0, 1000, 0).size() ==
              buildLineSites(1, 1, kBuildLineMaxGap, 0, 0, 1000, 0).size(), "gap above the maximum is not clamped");
        check(buildLineSites(1, 1, -4, 0, 0, 160, 0).size() == 11, "negative gap is not clamped to touching");
    }
    {   // The wheel: every whole notch is a step; trackpad fractions add up, at most
        // one step per kBuildLineTrackpadMs however much a swipe sends.
        float carry = 0;
        uint32_t last = 0;
        check(buildLineWheelSteps(1.0f, carry, 10, last) == 1 && buildLineWheelSteps(-1.0f, carry, 11, last) == -1,
              "a notch is one step");
        check(buildLineWheelSteps(3.0f, carry, 12, last) == 3, "three notches in one event are not three steps");
        int sum = 0;
        for (int i = 0; i < 10; ++i) sum += buildLineWheelSteps(0.1f, carry, 100 + i, last);
        check(sum == 1, "ten tenth-notches are not one step");
        carry = 0;
        sum = 0;   // a 600 ms swipe of big momentum deltas, one event per 10 ms
        for (uint32_t t = 1000; t < 1600; t += 10) sum += buildLineWheelSteps(3.7f, carry, t, last);
        check(sum == 4, "a trackpad swipe moved " + std::to_string(sum) + " steps, not one per 150 ms");
        carry = 0;
        check(buildLineWheelSteps(0.4f, carry, 5000, last) == 0 && buildLineWheelSteps(0.4f, carry, 5001, last) == 0 &&
              buildLineWheelSteps(0.4f, carry, 5002, last) == 1, "fractions do not accumulate");
    }
    std::printf("PASS: %d build lines, %d sites: snapped, never sharing a cell, gap exact; wheel steps\n", lines, sites);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
