// metalspots_test -- metal deposits as extractor build spots (client/metalspots.h).
//
// A lone 3x3 patch is one spot for a 2x2 extractor, centred on its richest 2x2 and on
// the sim's footprintSnap grid; a 3x3 extractor takes a 3x3 patch whole; a field of
// metal takes as many as fit without overlapping; a metal planet (high SurfaceMetal, or
// no deposit) has none; the snap finds the nearest spot within reach and nothing past it; a box takes the
// spots inside it, nearest-first from the builder.

#include "client/metalspots.h"

#include <cstdio>
#include <vector>

namespace ms = ta::client::metalspots;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

struct Map {
    int w, h, surface;
    std::vector<uint8_t> m;
    Map(int w_, int h_, int s) : w(w_), h(h_), surface(s), m(size_t(w_) * size_t(h_), uint8_t(s)) {}
    void patch(int x0, int z0, int n, int v) {
        for (int z = z0; z < z0 + n; ++z)
            for (int x = x0; x < x0 + n; ++x) m[size_t(z) * size_t(w) + size_t(x)] = uint8_t(v);
    }
    ms::Cells cells() const { return {m.data(), w, h, surface}; }
};

int main() {
    std::printf("metalspots_test\n");
    {
        Map map(64, 64, 3);
        map.patch(10, 10, 3, 187);
        const auto s = ms::find(map.cells(), 2, 2);
        check(s.size() == 1, "a lone 3x3 patch is one 2x2 spot");
        check(!s.empty() && s[0].ox >= 10 && s[0].ox <= 11 && s[0].oz >= 10 && s[0].oz <= 11,
              "the spot lies inside the patch");
        check(!s.empty() && s[0].yield == 4 * 188, "it covers four deposit cells");
        check(!s.empty() && s[0].x == float(s[0].ox * 16 + 16) && s[0].z == float(s[0].oz * 16 + 16),
              "its build point is footprintSnap's centre");
        const auto big = ms::find(map.cells(), 3, 3);
        check(big.size() == 1 && big[0].ox == 10 && big[0].oz == 10, "a 3x3 extractor takes the patch whole");
        check(!ms::metalPlanet(map.cells()), "a patched map with SurfaceMetal 3 is no metal planet");
    }
    {
        Map map(64, 64, 3);
        map.patch(10, 10, 3, 187);
        map.patch(13, 10, 3, 187);   // touching: one cluster
        const auto s = ms::find(map.cells(), 2, 2);
        check(s.size() == 3, "two touching 3x3 patches (6x3 of metal) take three 2x2 extractors");
        bool apart = true;
        for (size_t i = 0; i < s.size(); ++i)
            for (size_t j = i + 1; j < s.size(); ++j)
                if (s[i].ox < s[j].ox + 2 && s[j].ox < s[i].ox + 2 && s[i].oz < s[j].oz + 2 && s[j].oz < s[i].oz + 2) apart = false;
        check(apart && !s.empty() && s.back().yield == 4 * 188, "none overlaps, each fully on metal");
    }
    {
        Map map(64, 64, 255);
        check(ms::metalPlanet(map.cells()) && ms::find(map.cells(), 2, 2).empty(), "SurfaceMetal 255: a metal planet, no spots");
        Map lava(64, 64, 20);
        lava.patch(5, 5, 3, 187);
        check(ms::metalPlanet(lava.cells()), "SurfaceMetal 20 is a metal planet");
        Map bare(64, 64, 3);
        check(ms::metalPlanet(bare.cells()), "no deposit anywhere is a metal planet");
    }
    {
        Map map(128, 128, 0);
        map.patch(10, 10, 3, 187);
        map.patch(40, 10, 3, 187);
        map.patch(10, 40, 3, 187);
        map.patch(100, 100, 3, 187);
        const auto s = ms::find(map.cells(), 2, 2);
        check(s.size() == 4, "four patches, four spots");
        const auto* n = ms::nearest(s, 10 * 16 + 30, 10 * 16 + 30);
        check(n && n->ox >= 10 && n->ox <= 11 && n->oz >= 10 && n->oz <= 11, "the snap takes the nearest");
        check(!ms::nearest(s, 70 * 16, 70 * 16), "nothing within reach: no snap");
        const auto box = ms::inBox(s, 0, 0, 60 * 16, 60 * 16, 50 * 16, 0);
        check(box.size() == 3, "the box takes the three spots inside it");
        check(box.size() == 3 && box[0].ox >= 40 && box[1].ox <= 11 && box[1].oz <= 11 && box[2].oz >= 40,
              "nearest first from the builder");
    }
    std::printf(g_fail ? "metalspots_test: %d FAILED\n" : "metalspots_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
