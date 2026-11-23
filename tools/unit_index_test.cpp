// Unit broad phase (workstream L, client/unitindex.h).
//
//  1. UnitGrid::query against brute force: every unit inside the rectangle is
//     returned, results are strictly ascending, every finite result lies within one
//     cell of the rectangle, non-finite positions are always returned -- over
//     uniform, clustered, coincident, collinear, negative, huge (cell growth),
//     empty and single-unit populations and degenerate/inverted/outside/cell-edge
//     rectangles.
//  2. The GameView cull through anchorBandToWorld: for random zooms, offsets,
//     viewports and per-unit lifts within LiftBounds, the units passing the exact
//     float test `!(sx < L || sx > R || sy < T || sy > B)` are all candidates, and
//     filtering the candidates gives the full scan's accepted list, in order. Units
//     are placed ulps either side of each edge.
//  3. Every unit pass of the renderer (unitpass::*: painter cull, health bar,
//     self-destruct number, squad marker): with random views,
//     lifts and footprint depths, the exact float flip point of each edge of the
//     pass's test is located by bisection and units a few ulps either side of it
//     (plus random ones) that pass the test lie inside the rectangle built from
//     the pass's band, and are grid candidates in full-scan order. The painter
//     pass is also checked against the renderer's expression written out above.
//  4. Randomized movement: a population that moves and churns, rebuilt as a new
//     snapshot every step, against a full scan.
//  5. UnitTypeTable: registered(id) equals the map's key set through random
//     sets/erases, negative and large ids, ids past the flag table.
//  6. Cursor picking (perf follow-up C, pickBand): GameView::unitUnderCursor's
//     exact float test written out (hit box widened to 9 px, or a dot), for random
//     zooms, offsets, hit boxes, dots, lifts, waterline sinks and interpolation
//     offsets up to the bound: every unit the test accepts lies inside the world
//     rectangle built from pickBand and is a grid candidate, and filtering the
//     candidates gives the full scan's hits in order. Units are placed at the
//     exact flip points of the test's edges (bisection) and a few ulps either side.

#include "client/unitindex.h"
#include "client/unittypetable.h"

#include <cmath>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using tak::render::UnitGrid;
using tak::render::LiftBounds;
using tak::render::anchorBandToWorld;

namespace {

int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("\n"); if (failures > 20) std::exit(1); } } while (0)

struct Population { std::vector<float> x, z; };

void checkQuery(const Population& p, const UnitGrid& g, float l, float t, float r, float b,
                std::vector<uint64_t>& bits, const char* what) {
    std::vector<uint32_t> out;
    g.query(l, t, r, b, bits, out);
    for (size_t k = 1; k < out.size(); ++k) CHECK(out[k - 1] < out[k], "%s: not ascending", what);
    std::vector<char> in(p.x.size(), 0);
    for (uint32_t i : out) { CHECK(i < p.x.size(), "%s: index out of range", what); if (i < in.size()) in[i] = 1; }
    const bool nanRect = std::isnan(l) || std::isnan(t) || std::isnan(r) || std::isnan(b);
    for (size_t i = 0; i < p.x.size(); ++i) {
        const float x = p.x[i], z = p.z[i];
        const bool finite = std::isfinite(x) && std::isfinite(z);
        const bool inside = finite && x >= l && x <= r && z >= t && z <= b;
        if (!finite || nanRect) CHECK(in[i], "%s: unit %zu (non-finite or NaN rect) missing", what, i);
        if (inside) CHECK(in[i], "%s: unit %zu at %.9g,%.9g inside [%.9g,%.9g]x[%.9g,%.9g] missing",
                          what, i, double(x), double(z), double(l), double(r), double(t), double(b));
        // Tight to one cell around the rectangle (whole cells are returned).
        if (in[i] && finite && !nanRect && l <= r && t <= b) {
            const float c = g.cell() * 1.0001f;
            CHECK(x >= l - c && x <= r + c && z >= t - c && z <= b + c,
                  "%s: unit %zu at %.9g,%.9g more than a cell outside the rectangle", what, i, double(x), double(z));
        }
    }
    for (uint64_t w : bits) CHECK(w == 0, "%s: scratch bits not cleared", what);
    if (g.covers(l, t, r, b)) CHECK(out.size() == p.x.size(), "%s: covers() but not every unit returned", what);
    bool all = true;
    for (size_t i = 0; i < p.x.size(); ++i)
        all = all && std::isfinite(p.x[i]) && std::isfinite(p.z[i]) && p.x[i] >= l && p.x[i] <= r && p.z[i] >= t && p.z[i] <= b;
    if (g.covers(l, t, r, b)) CHECK(all, "%s: covers() but a unit lies outside", what);
    if (all && !p.x.empty()) CHECK(g.covers(l, t, r, b), "%s: every unit inside but covers() false", what);
}

void testGrid(std::mt19937& rng) {
    std::vector<uint64_t> bits;
    std::vector<uint32_t> scratch;
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    for (int trial = 0; trial < 400; ++trial) {
        Population p;
        const int kind = trial % 9;
        const size_t n = kind == 7 ? 0 : kind == 8 ? 1 : 1 + rng() % 3000;
        for (size_t i = 0; i < n; ++i) {
            float x = 0, z = 0;
            switch (kind) {
            case 0: x = u01(rng) * 10240; z = u01(rng) * 10240; break;                       // uniform map
            case 1: x = 5000 + (u01(rng) - 0.5f) * 300; z = 7000 + (u01(rng) - 0.5f) * 200; break;  // cluster
            case 2: x = 1234.5f; z = 4321.25f; break;                                       // coincident
            case 3: x = u01(rng) * 20000; z = 512.0f; break;                                // collinear
            case 4: x = -3000 + u01(rng) * 2000; z = -50 - u01(rng) * 700; break;             // negative
            case 5: x = (u01(rng) - 0.5f) * 4.0e6f; z = (u01(rng) - 0.5f) * 3.0e5f; break;    // huge: cell growth
            case 6: x = float(rng() % 41) * 256.0f; z = float(rng() % 41) * 256.0f; break;   // exact cell edges
            default: x = u01(rng) * 10240; z = u01(rng) * 10240; break;
            }
            if (trial % 13 == 5 && i % 97 == 3) x = (i & 1) ? nan : inf;                   // never produced; still safe
            p.x.push_back(x); p.z.push_back(z);
        }
        UnitGrid g;
        g.build(p.x, p.z, scratch);
        CHECK(g.size() == n, "size");
        CHECK(g.width() <= UnitGrid::kMaxSide && g.height() <= UnitGrid::kMaxSide, "grid too large");
        for (int q = 0; q < 40; ++q) {
            float l, t, r, b;
            const int qk = q % 10;
            if (qk < 4 && n) {   // around a unit, possibly degenerate
                const size_t i = rng() % n;
                const float w = qk == 0 ? 0.0f : u01(rng) * 2000, h = qk == 0 ? 0.0f : u01(rng) * 1500;
                l = p.x[i] - w * u01(rng); t = p.z[i] - h * u01(rng); r = l + w; b = t + h;
                if (qk == 1 && std::isfinite(p.x[i])) { l = r = p.x[i]; t = b = p.z[i]; }
            } else if (qk == 4) {   // cell edges, one ulp either side
                l = std::nextafter(float(rng() % 40) * 256.0f, (rng() & 1) ? inf : -inf);
                t = std::nextafter(float(rng() % 40) * 256.0f, (rng() & 1) ? inf : -inf);
                r = l + 256.0f * float(rng() % 6); b = t + 256.0f * float(rng() % 6);
            } else if (qk == 5) { l = -1e9f; t = -1e9f; r = 1e9f; b = 1e9f; }              // everything
            else if (qk == 6) { l = 1e8f; t = 1e8f; r = 2e8f; b = 2e8f; }                  // outside
            else if (qk == 7) { l = 900; r = 100; t = 0; b = 500; }                        // inverted
            else if (qk == 8) { l = -inf; t = -inf; r = inf; b = inf; }
            else if (qk == 9 && q % 20 == 9) { l = nan; t = 0; r = 10; b = 10; }
            else { l = (u01(rng) - 0.2f) * 12000; t = (u01(rng) - 0.2f) * 12000; r = l + u01(rng) * 4000; b = t + u01(rng) * 3000; }
            checkQuery(p, g, l, t, r, b, bits, "grid");
        }
    }
}

// GameView's unit cull, in the same float expression order (gameview_render.cpp).
bool exactCull(float x, float z, float liftX, float liftY, float offX, float offY, float zm,
               float L, float T, float R, float B) {
    const float sx = (x - offX) * zm - liftX * zm;
    const float sy = (z - offY) * zm - liftY * zm;
    return !(sx < L || sx > R || sy < T || sy > B);
}

void testCull(std::mt19937& rng) {
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    std::vector<uint64_t> bits;
    std::vector<uint32_t> scratch, out;
    size_t accepted = 0, edge = 0;
    for (int trial = 0; trial < 1500; ++trial) {
        const float zm = trial % 7 == 0 ? 0.05f + u01(rng) * 0.2f : 0.2f + u01(rng) * 3.8f;
        const int mvw = 300 + int(rng() % 7400), winH = 200 + int(rng() % 2000);
        const float offX = (u01(rng) - 0.1f) * 12000, offY = (u01(rng) - 0.1f) * 12000;
        const float L = -160, T = -260, R = float(mvw + 160), B = float(winH + 120);
        // Lift bounds as GameView builds them: [0, (top - datum + 1) * scale], the
        // scales possibly negative or zero (debug TAK_HSCALE/TAK_HSCALEX).
        const float above = float(rng() % 256) + 1.0f;
        const float sy = trial % 11 == 0 ? -0.5f : trial % 5 == 0 ? 0.0f : 0.5f;
        const float sxs = trial % 3 == 0 ? 0.25f : trial % 17 == 0 ? -0.3f : 0.0f;
        const LiftBounds lift{std::min(0.0f, above * sxs), std::max(0.0f, above * sxs),
                              std::min(0.0f, above * sy), std::max(0.0f, above * sy)};
        Population p;
        std::vector<float> lx, ly;
        const size_t n = 200 + rng() % 1500;
        for (size_t i = 0; i < n; ++i) {
            const float liftX = lift.minX + (lift.maxX - lift.minX) * ((i % 5 == 0) ? float(i % 2) : u01(rng));
            const float liftY = lift.minY + (lift.maxY - lift.minY) * ((i % 7 == 0) ? float(i % 2) : u01(rng));
            float x, z;
            if (i % 3 == 0) {
                // On an edge of the band: solve for the edge, then step a few ulps.
                const float ex = (i % 2 ? L : R) / zm + offX + liftX;
                const float ez = ((i / 2) % 2 ? T : B) / zm + offY + liftY;
                x = ex; z = ez;
                const int steps = int(rng() % 9) - 4;
                for (int s = 0; s < std::abs(steps); ++s) x = std::nextafter(x, steps > 0 ? 1e30f : -1e30f);
                if (i % 6 == 0) z = offY + (T + u01(rng) * (B - T)) / zm + liftY;   // on the x edge only
                ++edge;
            } else {
                x = offX + (L - 300 + u01(rng) * (R - L + 600)) / zm;
                z = offY + (T - 300 + u01(rng) * (B - T + 600)) / zm;
            }
            p.x.push_back(x); p.z.push_back(z); lx.push_back(liftX); ly.push_back(liftY);
        }
        UnitGrid g;
        g.build(p.x, p.z, scratch);
        const auto rect = anchorBandToWorld(L, T, R, B, offX, offY, zm, lift);
        g.query(rect.left, rect.top, rect.right, rect.bottom, bits, out);
        std::vector<char> cand(n, 0);
        for (uint32_t i : out) cand[i] = 1;
        std::vector<uint32_t> full, indexed;
        for (size_t i = 0; i < n; ++i)
            if (exactCull(p.x[i], p.z[i], lx[i], ly[i], offX, offY, zm, L, T, R, B)) {
                full.push_back(uint32_t(i));
                // The rectangle itself must hold the unit (not just its grid cell).
                CHECK(p.x[i] >= rect.left && p.x[i] <= rect.right && p.z[i] >= rect.top && p.z[i] <= rect.bottom,
                      "cull: unit %zu passes the exact test outside the world rectangle", i);
                CHECK(cand[i], "cull: unit %zu passes the exact test but is no candidate (zoom %.6g, lift %.6g/%.6g)",
                      i, double(zm), double(lx[i]), double(ly[i]));
            }
        for (uint32_t i : out)
            if (exactCull(p.x[i], p.z[i], lx[i], ly[i], offX, offY, zm, L, T, R, B)) indexed.push_back(i);
        CHECK(full == indexed, "cull: filtered candidates differ from the full scan");
        accepted += full.size();
    }
    // A zoom that is not positive and finite: every unit is a candidate.
    Population p{{1, 2, 3}, {4, 5, 6}};
    UnitGrid g;
    g.build(p.x, p.z, scratch);
    for (float zm : {0.0f, -1.0f, std::numeric_limits<float>::infinity()}) {
        const auto rect = anchorBandToWorld(-160, -260, 900, 800, 0, 0, zm, LiftBounds{});
        g.query(rect.left, rect.top, rect.right, rect.bottom, bits, out);
        CHECK(out.size() == 3, "degenerate zoom must return every unit");
    }
    CHECK(accepted > 10000 && edge > 10000, "cull test exercised too little (%zu accepted, %zu on edges)", accepted, edge);
}

namespace up = tak::render::unitpass;
using tak::render::AnchorView;
using tak::render::ScreenBand;
using tak::render::ScreenPoint;

struct Pass {
    const char* name;
    bool footprint;
    ScreenPoint (*pos)(float, float, float, float, int, const AnchorView&);
    bool (*inView)(ScreenPoint, const AnchorView&);
    ScreenBand (*band)(const AnchorView&, int, int);
};
const Pass kPasses[] = {
    {"painter", false, [](float x, float z, float lx, float ly, int, const AnchorView& v) { return up::painter(x, z, lx, ly, v); },
     up::painterInView, [](const AnchorView& v, int, int) { return up::painterBand(v); }},
    {"health", false, [](float x, float z, float lx, float ly, int, const AnchorView& v) { return up::healthBar(x, z, lx, ly, v); },
     up::healthBarInView, [](const AnchorView& v, int, int) { return up::healthBarBand(v); }},
    {"selfdestruct", false, [](float x, float z, float lx, float ly, int, const AnchorView& v) { return up::selfDestruct(x, z, lx, ly, v); },
     up::selfDestructInView, [](const AnchorView& v, int, int) { return up::selfDestructBand(v); }},
    {"squad", true, up::squadMarker, up::squadMarkerInView, up::squadMarkerBand},
};

// The float at which `inside(f)` flips between a (inside) and b (outside): the last
// inside value walking from a towards b. Bisection in double, then ulp steps.
template<class F> float flipPoint(float a, float b, F inside) {
    double lo = a, hi = b;
    for (int i = 0; i < 80; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (inside(float(mid))) lo = mid; else hi = mid;
    }
    float f = float(lo);
    const float dir = b > a ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
    while (!inside(f)) f = std::nextafter(f, -dir);
    for (float n = std::nextafter(f, dir); inside(n); n = std::nextafter(n, dir)) f = n;
    return f;
}

void testPasses(std::mt19937& rng) {
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    std::vector<uint64_t> bits;
    std::vector<uint32_t> scratch, out;
    size_t edgeUnits[std::size(kPasses)] = {}, accepted[std::size(kPasses)] = {};
    for (int trial = 0; trial < 1200; ++trial) {
        const Pass& pass = kPasses[size_t(trial) % std::size(kPasses)];
        const AnchorView v{(u01(rng) - 0.1f) * 12000, (u01(rng) - 0.1f) * 12000,
                           trial % 7 == 0 ? 0.05f + u01(rng) * 0.2f : 0.2f + u01(rng) * 3.8f,
                           300 + int(rng() % 7400), 200 + int(rng() % 2000)};
        const float above = float(rng() % 256) + 1.0f;
        const float sy = trial % 11 == 0 ? -0.5f : trial % 5 == 0 ? 0.0f : 0.5f;
        const float sx = trial % 3 == 0 ? 0.25f : 0.0f;
        const LiftBounds lift{std::min(0.0f, above * sx), std::max(0.0f, above * sx),
                              std::min(0.0f, above * sy), std::max(0.0f, above * sy)};
        const int footMin = pass.footprint ? int(rng() % 3) : 1, footMax = footMin + (pass.footprint ? int(rng() % 20) : 0);
        Population p;
        std::vector<float> lx, ly;
        std::vector<int> foot;
        const auto add = [&](float x, float z, float a, float b, int f) {
            p.x.push_back(x); p.z.push_back(z); lx.push_back(a); ly.push_back(b); foot.push_back(f);
        };
        for (int u = 0; u < 120; ++u) {
            const float a = lift.minX + (lift.maxX - lift.minX) * (u % 4 == 0 ? float(u % 2) : u01(rng));
            const float b = lift.minY + (lift.maxY - lift.minY) * (u % 6 == 0 ? float(u % 2) : u01(rng));
            const int f = footMin + int(rng() % unsigned(footMax - footMin + 1));
            // A point well inside: the band's centre, mapped back through this unit's lift.
            const ScreenBand band = pass.band(v, f, f);
            const float cx = v.offX + 0.5f * (band.left + band.right) / v.zoom + a;
            const float cz = v.offY + 0.5f * (band.top + band.bottom) / v.zoom + b;
            const auto in = [&](float x, float z) { return pass.inView(pass.pos(x, z, a, b, f, v), v); };
            if (!in(cx, cz)) { add(cx, cz, a, b, f); continue; }   // still a valid random unit
            const float far = 1.0e5f / v.zoom;
            const float edges[4] = {flipPoint(cx, cx - far, [&](float x) { return in(x, cz); }),
                                    flipPoint(cx, cx + far, [&](float x) { return in(x, cz); }),
                                    flipPoint(cz, cz - far, [&](float z) { return in(cx, z); }),
                                    flipPoint(cz, cz + far, [&](float z) { return in(cx, z); })};
            for (int e = 0; e < 4; ++e)
                for (int step = -3; step <= 3; ++step) {
                    float t = edges[e];
                    for (int k = 0; k < std::abs(step); ++k) t = std::nextafter(t, step > 0 ? 1e30f : -1e30f);
                    if (e < 2) add(t, cz, a, b, f); else add(cx, t, a, b, f);
                    ++edgeUnits[size_t(&pass - kPasses)];
                }
            add(cx + (u01(rng) - 0.5f) * 3 * (band.right - band.left) / v.zoom,
                cz + (u01(rng) - 0.5f) * 3 * (band.bottom - band.top) / v.zoom, a, b, f);
        }
        UnitGrid g;
        g.build(p.x, p.z, scratch);
        const ScreenBand band = pass.band(v, footMin, footMax);
        const auto rect = anchorBandToWorld(band.left, band.top, band.right, band.bottom, v.offX, v.offY, v.zoom, lift);
        g.query(rect.left, rect.top, rect.right, rect.bottom, bits, out);
        std::vector<char> cand(p.x.size(), 0);
        for (uint32_t i : out) cand[i] = 1;
        std::vector<uint32_t> full, indexed;
        for (size_t i = 0; i < p.x.size(); ++i) {
            if (!pass.inView(pass.pos(p.x[i], p.z[i], lx[i], ly[i], foot[i], v), v)) continue;
            full.push_back(uint32_t(i));
            CHECK(p.x[i] >= rect.left && p.x[i] <= rect.right && p.z[i] >= rect.top && p.z[i] <= rect.bottom,
                  "%s: unit %zu (foot %d of %d..%d) passes its test outside the world rectangle",
                  pass.name, i, foot[i], footMin, footMax);
            CHECK(cand[i], "%s: unit %zu passes its test but is no candidate", pass.name, i);
        }
        for (uint32_t i : out)
            if (pass.inView(pass.pos(p.x[i], p.z[i], lx[i], ly[i], foot[i], v), v)) indexed.push_back(i);
        CHECK(full == indexed, "%s: filtered candidates differ from the full scan", pass.name);
        accepted[size_t(&pass - kPasses)] += full.size();
    }
    for (size_t k = 0; k < std::size(kPasses); ++k)
        CHECK(edgeUnits[k] > 5000 && accepted[k] > 2000, "%s exercised too little (%zu edge units, %zu accepted)",
              kPasses[k].name, edgeUnits[k], accepted[k]);
    // The painter helper is the renderer's former inline expression.
    for (int i = 0; i < 100000; ++i) {
        const AnchorView v{u01(rng) * 9000, u01(rng) * 9000, 0.05f + u01(rng) * 4, 100 + int(rng() % 8000), 100 + int(rng() % 3000)};
        const float x = u01(rng) * 12000, z = u01(rng) * 12000, a = u01(rng) * 128, b = u01(rng) * 128;
        const ScreenPoint q = up::painter(x, z, a, b, v);
        CHECK(up::painterInView(q, v) == exactCull(x, z, a, b, v.offX, v.offY, v.zoom, -160, -260,
                                                   float(v.viewW + 160), float(v.viewH + 120)),
              "painter helper differs from the renderer's expression");
    }
}

void testMovement(std::mt19937& rng) {
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    std::vector<uint64_t> bits;
    std::vector<uint32_t> scratch, out;
    Population p;
    for (int i = 0; i < 4000; ++i) { p.x.push_back(u01(rng) * 10240); p.z.push_back(u01(rng) * 10240); }
    UnitGrid g;   // one grid object reused across snapshots, as a ring slot is
    for (int step = 0; step < 300; ++step) {
        for (size_t i = 0; i < p.x.size(); ++i) {
            p.x[i] += (u01(rng) - 0.5f) * 12.0f; p.z[i] += (u01(rng) - 0.5f) * 12.0f;
            if (rng() % 500 == 0) { p.x[i] = u01(rng) * 10240; p.z[i] = u01(rng) * 10240; }   // teleport / respawn
        }
        if (step % 10 == 0) {   // churn: deaths and spawns change the live list
            const size_t kill = rng() % 50;
            for (size_t k = 0; k < kill && !p.x.empty(); ++k) {
                const size_t i = rng() % p.x.size();
                p.x.erase(p.x.begin() + long(i)); p.z.erase(p.z.begin() + long(i));
            }
            for (size_t k = 0; k < kill; ++k) { p.x.push_back(u01(rng) * 10240); p.z.push_back(u01(rng) * 10240); }
        }
        g.build(p.x, p.z, scratch);
        const float cx = 5120 + 4000 * std::sin(float(step) * 0.05f), cz = 5120 + 3000 * std::cos(float(step) * 0.031f);
        checkQuery(p, g, cx - 700, cz - 500, cx + 700, cz + 500, bits, "movement");
        checkQuery(p, g, cx - 4000, cz - 2000, cx + 4000, cz + 2000, bits, "movement wide");
    }
}

}  // namespace

void testTypeTable(std::mt19937& rng) {
    tak::render::UnitTypeTable table;
    std::unordered_map<int, std::string> reference;
    for (int step = 0; step < 200000; ++step) {
        const int id = int(rng() % 5000) - 200 + (step % 1000 == 0 ? 100000 : 0);
        if (rng() % 4 == 0) { table.erase(id); reference.erase(id); }
        else { const std::string v = "t" + std::to_string(rng() % 7); table.set(id, v); reference[id] = v; }
        const int probe = int(rng() % 6000) - 500;
        CHECK(table.registered(probe) == (reference.count(probe) != 0), "type table: registered(%d) wrong", probe);
        CHECK(table.count(probe) == reference.count(probe), "type table: count(%d) wrong", probe);
        if (reference.count(probe)) CHECK(table.at(probe) == reference.at(probe), "type table: at(%d) wrong", probe);
    }
    for (const auto& [id, v] : reference) CHECK(table.registered(id) && table.find(id) != table.end(), "type table: %d lost", id);
    CHECK(!table.registered(1 << 30), "type table: id past the flags");
}

// GameView::unitUnderCursor for a unit without altitude, in the same float expression
// order (gameview_hud.cpp / unitScreen in gameview_impl.cpp).
struct PickBox { float x, y, w, h; };
struct PickUnit { float x, z, dx, dz, liftX, liftY, sink, dotHalf; int box; bool dot; };
bool exactPick(float mx, float my, const PickUnit& u, const PickBox& hb, float offX, float offY, float zms) {
    const float ix = u.x + u.dx, iz = u.z + u.dz;
    const float px = (ix - offX) * zms - u.liftX * zms;
    const float py = (iz - offY) * zms - u.liftY * zms + u.sink * zms - 0.0f * zms - 12.0f * zms;
    if (u.dot) {
        const float cx = px, cy = py + 12.0f * zms;
        return !(std::fabs(mx - cx) > u.dotHalf || std::fabs(my - cy) > u.dotHalf);
    }
    const float ax = px, ay = py + 12.0f * zms;
    const float cx = ax + (hb.x + hb.w * 0.5f) * zms;
    const float cy = ay + (hb.y + hb.h * 0.5f) * zms;
    const float hw = std::max(hb.w * zms * 0.5f, 9.0f);
    const float hh = std::max(hb.h * zms * 0.5f, 9.0f);
    return !(std::fabs(mx - cx) > hw || std::fabs(my - cy) > hh);
}

void testPick(std::mt19937& rng) {
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    std::vector<uint64_t> bits;
    std::vector<uint32_t> scratch, out;
    size_t hits = 0, edge = 0, candidates = 0, units = 0;
    const float interp = 201.0f;
    for (int trial = 0; trial < 1500; ++trial) {
        const float zm = trial % 7 == 0 ? 0.05f + u01(rng) * 0.2f : 0.2f + u01(rng) * 3.8f;
        const float offX = (u01(rng) - 0.1f) * 12000, offY = (u01(rng) - 0.1f) * 12000;
        const float mx = (u01(rng) - 0.05f) * 7000, my = (u01(rng) - 0.05f) * 2400;
        const float above = float(rng() % 256) + 1.0f;
        const float sy = trial % 11 == 0 ? -0.5f : trial % 5 == 0 ? 0.0f : 0.5f;
        const float sxs = trial % 3 == 0 ? 0.25f : trial % 17 == 0 ? -0.3f : 0.0f;
        const LiftBounds lift{std::min(0.0f, above * sxs), std::max(0.0f, above * sxs),
                              std::min(0.0f, above * sy), std::max(0.0f, above * sy)};
        const float sinkHeight = trial % 4 == 0 ? 0.0f : float(rng() % 40) * sy;
        const float sinkLo = std::min(0.0f, sinkHeight), sinkHi = std::max(0.0f, sinkHeight);
        // Hit boxes as unitHitBox makes them (-r, minY, 2r, maxY - minY), tiny to huge,
        // plus the fallback box.
        // Every 13th trial: only tiny, off-centre boxes and no fallback, so the 9 px
        // floor (not another type's box or the dot) is what reaches furthest.
        const bool tiny = trial % 13 == 0;
        std::vector<PickBox> boxes;
        if (!tiny) boxes.push_back({-12.0f, -28.0f, 24.0f, 30.0f});
        for (int k = 1 + int(rng() % 6); k-- > 0;) {
            const float r = tiny ? u01(rng) * 3 : 2 + u01(rng) * 120;
            const float cx = tiny ? (u01(rng) - 0.5f) * 200 : 0.0f;
            const float lo = -u01(rng) * 160, hi = lo + (tiny ? u01(rng) * 4 : u01(rng) * 180) + 0.5f;
            boxes.push_back({cx - r, lo, 2 * r, hi - lo});
        }
        tak::render::PickBoxes union_;
        for (const auto& b : boxes) union_.add(b.x, b.y, b.w, b.h);
        const float dotHalf = tiny ? 9.0f : std::max(9.0f, 1.0f + u01(rng) * 40);
        const auto band = tak::render::pickBand(mx, my, zm, union_, dotHalf, interp, sinkLo, sinkHi);
        const auto rect = anchorBandToWorld(band.left, band.top, band.right, band.bottom, offX, offY, zm, lift);
        std::vector<PickUnit> pop;
        const auto randomUnit = [&](size_t i) {
            PickUnit u{};
            u.box = int(rng() % boxes.size());
            u.dot = !tiny && i % 9 == 0;
            u.dotHalf = 9.0f + (dotHalf - 9.0f) * u01(rng);
            // Each input at an extreme of its range half the time, independently.
            const auto pick = [&](float lo, float hi) { return rng() % 2 ? (rng() % 2 ? hi : lo) : lo + (hi - lo) * u01(rng); };
            u.liftX = pick(lift.minX, lift.maxX);
            u.liftY = pick(lift.minY, lift.maxY);
            u.sink = pick(sinkLo, sinkHi);
            u.dx = pick(-200.0f, 200.0f);   // interpPose: within interp (Frame::maxInterpStep + 1)
            u.dz = pick(-200.0f, 200.0f);
            return u;
        };
        const size_t n = 300 + rng() % 900;
        for (size_t i = 0; i < n; ++i) {
            PickUnit u = randomUnit(i);
            // Solve for the snapshot position whose box centre (or dot) is on the pointer.
            const PickBox& hb = boxes[size_t(u.box)];
            const float ox = u.dot ? 0.0f : (hb.x + hb.w * 0.5f), oy = u.dot ? 0.0f : (hb.y + hb.h * 0.5f);
            const float cxw = offX + (mx / zm - ox) + u.liftX - u.dx;
            const float czw = offY + (my / zm - oy) + u.liftY - u.sink - u.dz;
            if (i % 2 == 0) {
                // At an edge of the test: bisect along x or z from the centre outward.
                u.x = cxw; u.z = czw;
                const bool alongX = i % 4 == 0;
                const float dir = (i / 4) % 2 ? 1.0f : -1.0f;
                const float far = (alongX ? cxw : czw) + dir * (400.0f + 2000.0f / zm);
                const auto inside = [&](float v) {
                    PickUnit t = u; (alongX ? t.x : t.z) = v;
                    return exactPick(mx, my, t, boxes[size_t(t.box)], offX, offY, zm);
                };
                if (!inside(alongX ? cxw : czw)) { pop.push_back(u); continue; }
                float f = flipPoint(alongX ? cxw : czw, far, inside);
                const int steps = int(rng() % 9) - 4;
                for (int s = 0; s < std::abs(steps); ++s) f = std::nextafter(f, steps > 0 ? 1e30f : -1e30f);
                (alongX ? u.x : u.z) = f;
                ++edge;
            } else {
                const float spread = 3.0f * (dotHalf + 200) / zm + 600;
                u.x = cxw + (u01(rng) - 0.5f) * spread;
                u.z = czw + (u01(rng) - 0.5f) * spread;
            }
            pop.push_back(u);
        }
        Population p;
        for (const auto& u : pop) { p.x.push_back(u.x); p.z.push_back(u.z); }
        UnitGrid g;
        g.build(p.x, p.z, scratch);
        g.query(rect.left, rect.top, rect.right, rect.bottom, bits, out);
        std::vector<char> cand(pop.size(), 0);
        for (uint32_t i : out) cand[i] = 1;
        std::vector<uint32_t> full, indexed;
        for (size_t i = 0; i < pop.size(); ++i)
            if (exactPick(mx, my, pop[i], boxes[size_t(pop[i].box)], offX, offY, zm)) {
                full.push_back(uint32_t(i));
                CHECK(p.x[i] >= rect.left && p.x[i] <= rect.right && p.z[i] >= rect.top && p.z[i] <= rect.bottom,
                      "pick: unit %zu is under the cursor outside the world rectangle (zoom %.6g)", i, double(zm));
                CHECK(cand[i], "pick: unit %zu is under the cursor but no candidate (zoom %.6g)", i, double(zm));
            }
        for (uint32_t i : out)
            if (exactPick(mx, my, pop[i], boxes[size_t(pop[i].box)], offX, offY, zm)) indexed.push_back(i);
        CHECK(full == indexed, "pick: filtered candidates differ from the full scan");
        hits += full.size(); candidates += out.size(); units += pop.size();
    }
    std::printf("unit_index_test: pick hits=%zu edge=%zu candidates=%zu units=%zu\n", hits, edge, candidates, units);
    CHECK(hits > 100000 && edge > 100000, "pick test exercised too little (%zu hits, %zu on edges)", hits, edge);
    CHECK(candidates < units, "pick test: the band selected every unit");
}

int main() {
    std::mt19937 rng(20261017);
    testGrid(rng);
    testCull(rng);
    testPasses(rng);
    testMovement(rng);
    testTypeTable(rng);
    testPick(rng);
    if (failures) { std::printf("unit_index_test: %d failures\n", failures); return 1; }
    std::printf("unit_index_test: PASS\n");
    return 0;
}
