// hudcorner_test -- the map view's shared top-left column (client/hudcorner.h).
//
// For window sizes x F4 SCORECARD SCALEs x player counts x stream banner widths: the
// scoreboard never overlaps the stream banner (the user's report: the banner covered
// its Name header) nor the GAME TIME clock, stays inside the map view (not under the
// minimap strip), still fits above the information bar where it fits at all, and with
// no banner it is exactly the table it always was wherever that fitted across.

#include "client/hudcorner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hc = tak::hudcorner;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::printf("  FAIL %s\n", what); ++g_fail; }
}

int main() {
    std::printf("hudcorner_test\n");
    const float base = 1080.0f / 700.0f;   // GameView::kHudBase
    const int sizes[][2] = {{640, 480}, {1024, 768}, {1600, 1000}, {1920, 1080}, {3840, 2160}, {7680, 2160}};
    const float scorecard[] = {0.5f, 1.0f, 1.5f, 2.0f};
    const float bars[] = {0, 90, 160};
    int cases = 0;
    for (const auto& sz : sizes)
    for (float sc : scorecard)
    for (float bar : bars)
    for (int players = 1; players <= 10; ++players)
    for (int teams = 0; teams < 2; ++teams)
    for (int banner = 0; banner < 2; ++banner)
    for (int clock = 0; clock < 2; ++clock) {
        ++cases;
        const float w = float(sz[0]) - 200, floor = float(sz[1]) - bar;
        hc::Column column;
        hc::Rect bannerBox{};
        if (banner) {
            bannerBox = hc::streamBanner(450, sz[0]);
            column = hc::Column(bannerBox.y + bannerBox.h + hc::kGap);
        }
        float clockBottom = 0;
        if (clock) { const float y = column.place(32, 28); clockBottom = y + 28; check(!banner || y >= bannerBox.y + bannerBox.h, "clock below the banner"); }
        const auto b = hc::scoreboard(sc * base, teams, players, w, floor, column.next());
        const float tableH = 32 * b.scale * float(players + 1);
        const float tableW = (484.0f + (teams ? 64.0f : 0.0f)) * b.scale;
        check(b.scale >= 0.1f - 1e-4f && b.scale <= sc * base + 1e-4f, "scale within 0.1 .. requested");
        if (banner) check(b.top >= bannerBox.y + bannerBox.h, "scoreboard header below the stream banner");
        if (clock) check(b.top >= clockBottom, "scoreboard below the GAME TIME clock");
        check(24 * b.scale + tableW <= w + 1e-3f, "scoreboard within the map view, margins included");
        if (b.top + 12 * 0.1f + 32 * 0.1f * float(players + 1) <= floor)   // it can fit at all
            check(b.top + tableH <= floor + 1e-3f, "scoreboard above the information bar");
        if (!banner && !clock && sc * base * (508.0f + (teams ? 64.0f : 0.0f)) <= w) {
            // Unchanged without anything above it, where it fits across: the old formula.
            const float old = std::max(0.1f, std::min({sc * base, w / (484.0f + (teams ? 64.0f : 0.0f)),
                                                       std::max(1.0f, floor) / (24.0f + 32.0f * (players + 1))}));
            check(std::fabs(b.scale - old) < 1e-5f && std::fabs(b.top - 12 * old) < 1e-4f, "no banner: the table as before");
        }
    }
    // The column itself: an overlay with nothing above keeps its own top; the next one
    // starts kGap below the last.
    hc::Column c;
    check(c.place(32, 28) == 32, "clock alone keeps y 32");
    check(c.place(10, 20) == 32 + 28 + hc::kGap, "later overlay goes below the clock");
    hc::Column r(44);
    check(r.place(32, 28) == 44, "clock moves below a reserved banner");
    check(r.place(86, 30) == 86, "objectives keep y 86 when there is room");
    // The banner never exceeds the window.
    check(hc::streamBanner(5000, 640).w == 620, "banner clipped to the window");
    std::printf("  %d layouts checked\n", cases);
    std::printf(g_fail ? "FAILED (%d)\n" : "ok\n", g_fail);
    return g_fail ? 1 : 0;
}
