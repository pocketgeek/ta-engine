// reclaim_fade_test -- how a reclaimed tree or rock is drawn fading out
// (client/reclaimfade.h): retail's alpha steps (icd 4959c0: 1.0f less 0.03f a
// tick, drawn as ftol(alpha * 255.0f) by 4fd430), the step count that matches
// the sim's kReclaimFadeTicks, the snapshot-tick mapping, and the white
// silhouette.

#include "client/reclaimfade.h"

#include <cstdio>

namespace rf = tak::reclaimfade;

static int g_fail = 0;

static void check(const char* what, bool ok) {
    std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

int main() {
    std::printf("reclaim_fade_test: the reclaim fade\n");

    // Retail's alpha bytes, from the float steps (0.97f -> 247, ... 0.0100004f -> 2).
    check("before the fade: opaque", rf::alphaByte(0) == 255);
    check("first step: 247", rf::alphaByte(1) == 247);
    check("second step: 239", rf::alphaByte(2) == 239);
    check("third step: 232", rf::alphaByte(3) == 232);
    check("step 32: 10", rf::alphaByte(32) == 10);
    check("step 33, the last drawn: 2", rf::alphaByte(33) == 2);
    bool falling = true;
    for (int k = 1; k <= 33; ++k) falling = falling && rf::alphaByte(k) < rf::alphaByte(k - 1);
    check("every step is fainter than the one before", falling);

    // The sim's fade length is the step at which retail's alpha is <= 0.
    int gone = 0;
    for (float a = 1.0f; a > 0.0f; a = a - 0.03f) ++gone;
    check("kReclaimFadeTicks = the steps to alpha <= 0 (34)",
          gone == tak::sim::kReclaimFadeTicks && gone == 34);
    check("step 34 would be transparent", rf::alphaByte(34) == 0);

    // fadeEnd is the same whichever tick the client read it on.
    check("not fading: no end", rf::fadeEnd(100, 0) == 0);
    check("read at the start tick", rf::fadeEnd(100, 33) == 133);
    check("read five ticks later: the same end", rf::fadeEnd(105, 28) == 133);

    // Steps for the snapshot drawn.
    const uint32_t end = rf::fadeEnd(100, 33);   // fadeLeft 33 after tick 100
    check("no fade: draw as usual", rf::fadeSteps(0, 100) == 0);
    check("a snapshot from before the fade: draw as usual", rf::fadeSteps(end, 99) == 0);
    check("the reclaim's own tick: one step", rf::fadeSteps(end, 100) == 1);
    check("ten ticks on: eleven steps", rf::fadeSteps(end, 110) == 11);
    check("the last tick drawn: 33 steps", rf::fadeSteps(end, 132) == 33);
    check("a late snapshot never passes the last step", rf::fadeSteps(end, 200) == 33);

    // The silhouette: white where the frame is opaque, clear elsewhere.
    const uint8_t rgba[] = {10, 20, 30, 255,   0, 0, 0, 0,
                            0, 0, 0, 0,        200, 100, 50, 255};
    tak::hiddenunits::AlphaMask mask;
    mask.build(rgba, 2, 2);
    const auto px = rf::silhouette(mask);
    check("silhouette size", px.size() == 16);
    check("opaque texel turns opaque white",
          px.size() == 16 && px[0] == 255 && px[1] == 255 && px[2] == 255 && px[3] == 255 &&
          px[12] == 255 && px[13] == 255 && px[14] == 255 && px[15] == 255);
    check("transparent texel stays clear (white, for filtered edges)",
          px.size() == 16 && px[7] == 0 && px[11] == 0 && px[4] == 255 && px[10] == 255);

    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
