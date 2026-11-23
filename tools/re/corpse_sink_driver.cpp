// Text driver for tools/re/check_corpse_sink.py: runs the engine's water
// grave (512ee0's predicate, src/sim/retailheight.h) and sinking
// (src/sim/retailfeaturesink.h) on maps the checker also loads into the
// original routines. Built by the checker.
//
// In: W H sea gravity isfeature x y z reclaim wake reload, then W*H heights,
// then the tick count; reclaim is the tick whose units reclaim the body (-1:
// none), wake the tick whose units reload its decay with reload (-1: none),
// which World does only while the record is still in the moving list.
// Out: one line per tick, "y vy fading alpha-bits state" (state 0 moving,
// 1 settled, 2 removed, 3 reclaimed away), ending at the first tick that is
// not moving -- after a reclaim, the first that is gone. After a reclaim the
// alpha is the reclaim fade's (0.03 a pass, kReclaimFadeTicks passes).
#include "sim/retailfeaturesink.h"
#include "sim/retailheight.h"
#include "sim/retailreclaim.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <iostream>
#include <vector>

using namespace tak::sim;

int main() {
    int w, h, sea, gravity, isFeature, ticks, reclaim, wake, reload;
    int32_t x, y, z;
    while (std::cin >> w >> h >> sea >> gravity >> isFeature >> x >> y >> z >> reclaim >> wake >> reload) {
        std::vector<int> heights(size_t(w) * size_t(h));
        for (auto& v : heights) std::cin >> v;
        std::cin >> ticks;
        auto at = [&](int cx, int cz) {
            return heights[size_t(std::min(cz, h - 1)) * size_t(w) + size_t(std::min(cx, w - 1))];
        };
        const bool water = retailTerrainHeight(x, z, w, h, at) <= sea;
        RetailFeatureSink f{x, y, z, 0, water && !isFeature ? kRetailSinkVelocity : 0, 0};
        auto ground = [&](int32_t px, int32_t pz) {
            return retailCellMidHeight(px, pz, w, h, [&](int cx, int cz) {
                const int a = at(cx, cz), b = at(cx + 1, cz), c = at(cx, cz + 1), d = at(cx + 1, cz + 1);
                return std::pair{std::min({a, b, c, d}), std::max({a, b, c, d})};
            });
        };
        bool reclaimed = false, settled = false;
        int fade = 0;
        for (int t = 0; t < ticks; ++t) {
            if (t == wake && !settled) f.decay = reload;
            if (t == reclaim) { retailFeatureSinkReclaim(f); reclaimed = true; }
            if (reclaimed) {
                // The reclaim fade's pass (495b35), then a record with no
                // velocity and no fade of its own does nothing more.
                f.alpha = f.alpha - 0.03f;
                const bool gone = ++fade >= int(kReclaimFadeTicks);
                if (gone) f.alpha = 0.0f;   // 495b58: set to 0 before 494f00
                std::printf("%d %d %d %u %d\n", f.y, f.vy, int(f.fading), std::bit_cast<uint32_t>(f.alpha), gone ? 3 : 0);
                if (gone) break;
                continue;
            }
            const auto r = settled ? RetailSinkResult::Settled : retailFeatureSinkStep(f, ground, uint8_t(sea), gravity);
            settled = r == RetailSinkResult::Settled;
            std::printf("%d %d %d %u %d\n", f.y, f.vy, int(f.fading), std::bit_cast<uint32_t>(f.alpha), int(r));
            if (r != RetailSinkResult::Moving && reclaim < 0) break;
            if (r == RetailSinkResult::Removed) break;
        }
        std::printf("end\n");
        std::fflush(stdout);
    }
}
