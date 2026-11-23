// hidden_units_test -- the HIDDEN UNITS policy (client/hiddenunits.h).
//
// The option's persisted names, the painter layer that puts flat features (TDF height
// below 10) under every unit, the pieces the DOTS test is built from (the sprite
// opacity mask with the sway shear, the model triangle test, the screen bins the frame
// walks backwards) and FADE's cover tests and fades. The draw itself (cover order,
// fog, faded pixels) is checked through the real renderer by the hidden_units harness
// test (TAK_HIDDEN_UNITS_TEST).

#include "client/hiddenunits.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace hu = tak::hiddenunits;

static int g_fail = 0;

static void check(const char* what, bool ok) {
    std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

struct Point { float x, y; };
struct Vertex { Point position; };

int main() {
    std::printf("hidden_units_test: the HIDDEN UNITS policy\n");

    // ---- the option ----
    check("default is FADE", hu::kDefault == hu::kFade);
    check("names round trip", hu::parse(hu::name(hu::kOff)) == hu::kOff &&
                              hu::parse(hu::name(hu::kDots)) == hu::kDots &&
                              hu::parse(hu::name(hu::kFade)) == hu::kFade);
    check("saved by name", std::string(hu::name(hu::kOff)) == "off" && std::string(hu::name(hu::kDots)) == "dots" &&
                           std::string(hu::name(hu::kFade)) == "fade");
    check("three modes", hu::kModeCount == 3 && hu::kFade == hu::kModeCount - 1);
    check("numbers read", hu::parse("0") == hu::kOff && hu::parse("1") == hu::kDots &&
                          hu::parse("2") == hu::kFade && hu::parse("3") == hu::kFade);
    check("unknown value reads as the default", hu::parse("") == hu::kDefault && hu::parse("halo") == hu::kDefault);
    check("other names (shadow, outline) read as FADE", hu::parse("shadow") == hu::kFade &&
                                                         hu::parse("outline") == hu::kFade);

    // ---- the low layer ----
    check("height 0 and 9 are low, 10 and 226 are not",
          hu::lowFeature(0) && hu::lowFeature(9) && !hu::lowFeature(10) && !hu::lowFeature(226));
    {
        // The painter list's order: layer, then depth, stable. A smudge south of a
        // unit (larger z) still goes first; a tree south of it goes after it.
        struct Item { float z; int layer; char what; };
        std::vector<Item> items = {{120, hu::featureLayer(0), 's'}, {100, hu::kLayerGround, 'u'},
                                   {110, hu::featureLayer(226), 't'}, {90, hu::kLayerAir, 'f'},
                                   {80, hu::featureLayer(9), 'm'}};
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.layer != b.layer) return a.layer < b.layer;
            return a.z < b.z;
        });
        std::string order;
        for (const Item& i : items) order += i.what;
        check("flat features first, then ground by depth, flyers last", order == "msutf");
    }

    // ---- sprite cover ----
    {
        // 4x2 sprite: only column 2 of the top row and column 0 of the bottom row opaque.
        std::vector<uint8_t> px(4 * 2 * 4, 0);
        px[(0 * 4 + 2) * 4 + 3] = 255;
        px[(1 * 4 + 0) * 4 + 3] = 255;
        px[(1 * 4 + 1) * 4 + 3] = 100;   // below the cut: transparent
        hu::AlphaMask m;
        m.build(px.data(), 4, 2);
        check("mask: opaque texels", m.opaque(2, 0) && m.opaque(0, 1) && !m.opaque(1, 1) && !m.opaque(0, 0));
        check("mask: outside is clear", !m.opaque(-1, 0) && !m.opaque(4, 0) && !m.opaque(0, 2));
        // Drawn at (100, 50), 40x20 on screen: a texel is 10x10.
        check("sprite: point on an opaque texel", hu::spriteCovers(m, 100, 50, 40, 20, 0, 125, 55));
        check("sprite: point on a clear texel", !hu::spriteCovers(m, 100, 50, 40, 20, 0, 105, 55));
        check("sprite: point outside", !hu::spriteCovers(m, 100, 50, 40, 20, 0, 99, 55) &&
                                       !hu::spriteCovers(m, 100, 50, 40, 20, 0, 125, 70));
        // Swayed: the top edge moved 8 px right, the bottom edge stays.
        check("sway: the crown moves with the shear", hu::spriteCovers(m, 100, 50, 40, 20, 8, 132, 51) &&
                                                     !hu::spriteCovers(m, 100, 50, 40, 20, 0, 132, 51));
        check("sway: the base stays put", hu::spriteCovers(m, 100, 50, 40, 20, 8, 101, 69));
        check("sprite: degenerate quad covers nothing", !hu::spriteCovers(m, 100, 50, 0, 20, 0, 100, 55));
    }

    // ---- structure triangles ----
    {
        const std::vector<Vertex> tris = {{{0, 0}}, {{10, 0}}, {{0, 10}},      // lower-left half
                                          {{20, 20}}, {{30, 20}}, {{20, 30}}};
        check("triangle: inside", hu::trianglesCover(std::span<const Vertex>(tris), 2, 2));
        check("triangle: edge counts", hu::trianglesCover(std::span<const Vertex>(tris), 5, 5));
        check("triangle: outside", !hu::trianglesCover(std::span<const Vertex>(tris), 8, 8));
        check("triangle: second one", hu::trianglesCover(std::span<const Vertex>(tris), 22, 22));
        check("triangle: either winding", hu::pointInTriangle(2, 2, 0, 0, 0, 10, 10, 0));
        const std::vector<Vertex> partial = {{{0, 0}}, {{10, 0}}};   // not a whole triangle
        check("triangle: a partial triple is ignored", !hu::trianglesCover(std::span<const Vertex>(partial), 1, 0));
    }

    // ---- screen bins (the backwards walk) ----
    {
        hu::CoverGrid g;
        g.reset(640, 480, 64);
        // Painter order: unit A, tree T1 (over A), unit B, tree T2 (far away).
        // Walked backwards: T2, B, T1, A.
        const uint32_t t2 = g.insert({500, 300, 560, 400});
        size_t seen = 0;
        g.query({100, 100, 140, 160}, [&](uint32_t) { ++seen; return true; });
        check("B: the far tree is no cover", seen == 0);
        const uint32_t t1 = g.insert({90, 60, 200, 200});   // spans many cells
        std::vector<uint32_t> ids;
        g.query({100, 100, 140, 160}, [&](uint32_t id) { ids.push_back(id); return true; });
        check("A: the tree after it found once", ids == std::vector<uint32_t>{t1});
        ids.clear();
        g.query({0, 0, 640, 480}, [&](uint32_t id) { ids.push_back(id); return true; });
        std::sort(ids.begin(), ids.end());
        check("whole screen: every cover once", ids == (std::vector<uint32_t>{t2, t1}) && g.size() == 2);
        size_t visits = 0;
        g.query({0, 0, 640, 480}, [&](uint32_t) { ++visits; return false; });
        check("a visitor can stop the query", visits == 1);
        g.query({-50, -50, -10, -10}, [&](uint32_t) { ++visits; return true; });
        check("off screen: nothing", visits == 1);
        g.reset(320, 240, 64);
        g.query({0, 0, 320, 240}, [&](uint32_t) { ++visits; return true; });
        check("reset empties the bins", visits == 1 && g.size() == 0);
    }

    // ---- rectangles ----
    {
        hu::Rect a{0, 0, 10, 10}, b{10, 0, 20, 10};
        check("touching rectangles do not overlap", !a.overlaps(b));
        check("a rectangle overlaps itself, an empty one nothing", a.overlaps(a) && !a.overlaps(hu::Rect{}));
        check("clip", a.clipped({5, 5, 50, 50}).x0 == 5 && hu::Rect{20, 20, 30, 30}.clipped(a).empty());
    }

    // ---- FADE ----
    check("fade default 50%", hu::kDefaultFadePercent == 50 && hu::fadeOpacity(50) == 128);
    check("fade opacity ends", hu::fadeOpacity(0) == 0 && hu::fadeOpacity(100) == 255 &&
                               hu::fadeOpacity(-3) == 0 && hu::fadeOpacity(140) == 255 && hu::fadeOpacity(50) == 128);
    check("alpha scaling", hu::scaleAlpha(255, 115) == 115 && hu::scaleAlpha(0, 200) == 0 &&
                           hu::scaleAlpha(128, 255) == 128 && hu::scaleAlpha(200, 128) == 100);
    check("cover keys apart", hu::featureCover(5) != hu::structureCover(5) &&
                              hu::structureCover(5) == hu::structureCover(5));
    {
        // The triangle (0,0) (100,0) (0,100): its hypotenuse runs x + y = 100.
        check("tri-rect: inside", hu::triangleOverlapsRect(0, 0, 100, 0, 0, 100, {10, 10, 20, 20}));
        check("tri-rect: past the hypotenuse", !hu::triangleOverlapsRect(0, 0, 100, 0, 0, 100, {60, 60, 80, 80}));
        check("tri-rect: across the hypotenuse", hu::triangleOverlapsRect(0, 0, 100, 0, 0, 100, {45, 45, 80, 80}));
        check("tri-rect: rect holds the triangle", hu::triangleOverlapsRect(10, 10, 12, 10, 10, 12, {0, 0, 50, 50}));
        check("tri-rect: apart", !hu::triangleOverlapsRect(0, 0, 100, 0, 0, 100, {120, 0, 140, 20}));
        struct V { struct { float x, y; } position; };
        const std::vector<V> tris = {{{0, 0}}, {{100, 0}}, {{0, 100}}};
        check("tris cover rect", hu::trianglesCoverRect(std::span<const V>(tris), {10, 10, 20, 20}) &&
                                 !hu::trianglesCoverRect(std::span<const V>(tris), {60, 60, 80, 80}));
    }
    check("fades out faster than back in", hu::kFadeOutMs >= 150 && hu::kFadeOutMs <= 250 &&
                                           hu::kFadeInMs >= 300 && hu::kFadeInMs <= 400);
    {
        constexpr uint8_t kOp = hu::fadeOpacity(hu::kDefaultFadePercent);
        hu::CoverFades f;
        const auto frame = [&](float ms, std::initializer_list<uint64_t> hits, std::initializer_list<uint64_t> view) {
            f.begin();
            for (uint64_t id : view) f.inView(id);
            for (uint64_t id : hits) f.cover(id);
            f.advance(ms);
        };
        check("fade: nothing fading", f.empty() && f.alpha(7, kOp) == 255);
        frame(16, {7, 7, 3}, {3, 7, 9});
        check("fade: covers over a unit start fading", f.size() == 2 && f.alpha(7, kOp) < 255 && f.alpha(7, kOp) > 200 &&
                                                       f.alpha(9, kOp) == 255);
        frame(hu::kFadeOutMs, {3, 7}, {3, 7, 9});
        check("fade: fully faded after the fade-out time", f.alpha(7, kOp) == kOp &&
                                                            f.alpha(3, kOp) == kOp);
        frame(hu::kFadeInMs * 0.5f, {3}, {3, 7, 9});
        check("fade: the unit gone, coming back", f.alpha(7, kOp) > kOp && f.alpha(7, kOp) < 255 &&
                                                   f.alpha(3, kOp) == kOp);
        frame(hu::kFadeInMs * 0.6f, {3}, {3, 7, 9});
        check("fade: fully back, dropped", f.alpha(7, kOp) == 255 && f.size() == 1);
        frame(16, {}, {7});
        check("fade: out of view, dropped", f.empty() && f.alpha(3, kOp) == 255);
        frame(16, {5}, {});
        frame(0, {5}, {});
        const uint8_t still = f.alpha(5, kOp);
        frame(0, {5}, {5});
        check("fade: no time, no change", f.alpha(5, kOp) == still);
        check("fade: 100% never fades", f.alpha(5, 255) == 255);
    }
    {
        // The sprite: 4x2 texels, only the top-right one opaque, drawn 40x20 at (100, 50).
        hu::AlphaMask m;
        uint8_t px[4 * 2 * 4] = {};
        px[3 * 4 + 3] = 255;
        m.build(px, 4, 2);
        check("rect: on the opaque texel", hu::spriteCoversRect(m, 100, 50, 40, 20, 0, {128, 52, 134, 58}));
        check("rect: only clear texels", !hu::spriteCoversRect(m, 100, 50, 40, 20, 0, {100, 50, 128, 70}));
        check("rect: outside the sprite", !hu::spriteCoversRect(m, 100, 50, 40, 20, 0, {150, 50, 200, 70}));
        check("rect: thinner than a step", hu::spriteCoversRect(m, 100, 50, 40, 20, 0, {135, 51, 135.5f, 51.5f}));
    }

    {
        // FADE's per-frame bookkeeping against testing every cover of every unit: the
        // same covers fade (each reported once), every unit gets the same answer, and
        // fewer exact tests run. Units walk in frame order; each sees some covers.
        uint32_t seed = 12345;
        const auto rnd = [&](uint32_t n) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) % n; };
        bool same = true, once = true, fewer = true, saved = false;
        for (int round = 0; round < 300; ++round) {
            const uint32_t covers = 1 + rnd(12), units = 1 + rnd(20);
            std::vector<std::vector<uint32_t>> sees(units);
            std::vector<std::vector<char>> reach(units, std::vector<char>(covers, 0));
            for (uint32_t u = 0; u < units; ++u) {
                for (uint32_t c = 0; c < covers; ++c)
                    if (rnd(3) == 0) { sees[u].push_back(c); reach[u][c] = char(rnd(2)); }
                for (size_t i = sees[u].size(); i > 1; --i) std::swap(sees[u][i - 1], sees[u][rnd(uint32_t(i))]);
            }
            std::vector<char> refFaded(covers, 0), refHit(units, 0);
            size_t refTests = 0;
            for (uint32_t u = 0; u < units; ++u)
                for (uint32_t c : sees[u]) {
                    ++refTests;
                    if (reach[u][c]) refFaded[c] = refHit[u] = 1;
                }
            hu::CoverMarks marks;
            marks.add();   // storage from an earlier frame
            marks.clear();
            for (uint32_t c = 0; c < covers; ++c) marks.add();
            std::vector<int> reported(covers, 0);
            size_t tests = 0;
            for (uint32_t u = 0; u < units; ++u) {
                const bool hit = marks.unit(
                    [&](auto&& visit) {
                        for (uint32_t c : sees[u])
                            if (!visit(c)) return;
                    },
                    [&](uint32_t c) { ++tests; return reach[u][c] != 0; }, [&](uint32_t c) { ++reported[c]; });
                same = same && hit == (refHit[u] != 0);
            }
            for (uint32_t c = 0; c < covers; ++c) {
                same = same && (reported[c] > 0) == (refFaded[c] != 0) && marks.found(c) == (refFaded[c] != 0);
                once = once && reported[c] <= 1;
            }
            fewer = fewer && tests <= refTests;
            saved = saved || tests < refTests;
        }
        check("marks: same fades and answers as testing every cover", same);
        check("marks: each cover reported once a frame", once);
        check("marks: never more exact tests, sometimes fewer", fewer && saved);
        // A unit whose only cover was already found over another unit, seen after a
        // found cover that does not reach it: still covered.
        hu::CoverMarks m;
        for (int i = 0; i < 3; ++i) m.add();
        int fades = 0;
        const auto unit = [&](std::vector<uint32_t> ids, std::vector<uint32_t> reaching) {
            return m.unit([&](auto&& visit) { for (uint32_t c : ids) if (!visit(c)) return; },
                          [&](uint32_t c) { return std::find(reaching.begin(), reaching.end(), c) != reaching.end(); },
                          [&](uint32_t) { ++fades; });
        };
        check("marks: first unit fades both covers", unit({0, 1}, {0, 1}) && fades == 2);
        check("marks: covered by an already-found cover", unit({0, 1}, {1}) && fades == 2);
        check("marks: found covers that miss: not covered", !unit({0, 1}, {}) && fades == 2);
        check("marks: a new cover still fades", unit({0, 2}, {0, 2}) && fades == 3 && m.found(2));
    }

    std::printf(g_fail ? "hidden_units_test: %d FAILURE(S)\n" : "hidden_units_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
