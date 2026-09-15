// navincr_test -- the incremental component update must be INDISTINGUISHABLE from a
// full relabel.
//
// blockCells() carries a cached component labelling across a pure-block edit when it can
// prove locally that nothing was severed (World::tryIncrementalBlock). That is a
// performance shortcut on a structure the deterministic sim reads: pathExists() decides
// whether an order is reachable, so a labelling that disagrees with the truth by even one
// cell is a DESYNC, not a slow frame. This test exists to make that impossible to ship
// quietly.
//
// Method: two worlds, identical terrain, identical block sequences. The WARM world is
// queried between every edit, so its cache is live and the incremental path runs on each
// block. The COLD world is queried only at the end, so its cache is built once, from
// scratch, against the final obstacle set. Their answers must agree on every pair.
//
// The generated walls deliberately include ones that SEVER the map (the fast path must
// refuse those and fall back) as well as ones that merely dent it.

#include "sim/sim.h"

#include <cstdio>
#include <vector>

using namespace tak::sim;

static int g_fail = 0;
static void check(bool cond, const std::string& what, const std::string& detail = {}) {
    std::printf("  %-56s %s%s%s\n", what.c_str(), cond ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!cond) ++g_fail;
}

static UnitType mover(int foot) {
    UnitType t{};
    t.name = "m"; t.id = "m";
    t.maxVel = 70; t.turnRate = 10000; t.maxHp = 100; t.canMove = true;
    t.footX = foot; t.footZ = foot;
    return t;
}

// A deterministic 32-bit PRNG: the sequence must be identical in both worlds, and
// std::rand()/mt19937 portability is not worth the risk in a determinism test.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int range(int lo, int hi) { return lo + int(next() % uint32_t(hi - lo + 1)); }
};

struct Edit { int x, z, w, h; bool blocked; };

// Mix of small footprints (buildings), long walls (corridor severing), and CLEARS of
// previously blocked ground -- an unblock merges components, which is the other half of
// the incremental path and the half a block-only test would never exercise.
static std::vector<Edit> makeEdits(uint32_t seed, int n, int W, int H) {
    Rng r(seed);
    std::vector<Edit> out;
    for (int i = 0; i < n; ++i) {
        // Clear something already placed about a third of the time, so merges happen
        // against real geometry rather than empty ground.
        if (!out.empty() && r.range(0, 2) == 0) {
            const Edit& prev = out[size_t(r.range(0, int(out.size()) - 1))];
            out.push_back({prev.x, prev.z, prev.w, prev.h, false});
            continue;
        }
        const int kind = r.range(0, 9);
        if (kind < 6) {                                  // a building
            const int s = r.range(1, 5);
            out.push_back({r.range(0, W - s - 1), r.range(0, H - s - 1), s, s, true});
        } else if (kind < 8) {                           // a long horizontal wall
            const int len = r.range(20, W / 2);
            out.push_back({r.range(0, W - len - 1), r.range(0, H - 2), len, r.range(1, 3), true});
        } else {                                         // a long vertical wall
            const int len = r.range(20, H / 2);
            out.push_back({r.range(0, W - 2), r.range(0, H - len - 1), r.range(1, 3), len, true});
        }
    }
    return out;
}

static bool runSeed(uint32_t seed, int foot, bool verbose) {
    const int W = 128, H = 128;
    const UnitType t = mover(foot);
    const auto edits = makeEdits(seed, 40, W, H);

    auto build = [&](bool warm) {
        auto w = std::make_unique<World>();
        w->setVisPlayer(-1);
        w->setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, /*seaLevel=*/20);
        for (const auto& e : edits) {
            w->blockCells(e.x, e.z, e.w, e.h, e.blocked);
            // Warm: force the labelling to exist BEFORE the next edit, so the next
            // blockCells has a live cache to carry across incrementally.
            if (warm) (void)w->pathExists(&t, 32.0f, 32.0f, float(W * 16 - 32), float(H * 16 - 32));
        }
        return w;
    };

    auto warm = build(true);
    auto cold = build(false);

    // Probe a lattice of pairs: every combination of a fixed set of points, so the
    // comparison covers cells inside walled-off pockets as well as open ground.
    std::vector<std::pair<float, float>> pts;
    for (int i = 1; i < 8; ++i)
        for (int j = 1; j < 8; ++j)
            pts.emplace_back(float(i * W * 16 / 8), float(j * H * 16 / 8));

    int mismatches = 0, reachable = 0;
    for (size_t a = 0; a < pts.size(); ++a)
        for (size_t b = 0; b < pts.size(); ++b) {
            const bool pw = warm->pathExists(&t, pts[b].first, pts[b].second,
                                             pts[a].first, pts[a].second);
            const bool pc = cold->pathExists(&t, pts[b].first, pts[b].second,
                                             pts[a].first, pts[a].second);
            if (pw != pc) ++mismatches;
            if (pc) ++reachable;
        }
    if (verbose || mismatches)
        std::printf("    seed=%u foot=%d pairs=%zu reachable=%d mismatches=%d\n",
                    seed, foot, pts.size() * pts.size(), reachable, mismatches);
    return mismatches == 0;
}

int main() {
    std::printf("navincr_test -- incremental component update vs full relabel\n");
    int bad = 0;
    for (uint32_t seed = 1; seed <= 40; ++seed)
        for (int foot : {1, 2, 3, 5})
            if (!runSeed(seed, foot, false)) ++bad;
    check(bad == 0, "warm (incremental) == cold (full rebuild) over 40 seeds x 4 footprints",
          bad ? std::to_string(bad) + " seed/foot combinations disagree" : "");

    // A wall that genuinely severs the map must be seen as severing it -- otherwise the
    // test above could pass simply by never proving anything.
    {
        const int W = 128, H = 128;
        const UnitType t = mover(2);
        World w;
        w.setVisPlayer(-1);
        w.setTerrain(std::vector<uint8_t>(size_t(W) * size_t(H), 100), W, H, 20);
        const bool before = w.pathExists(&t, 32.0f, 32.0f, float(W * 16 - 32), float(H * 16 - 32));
        for (int x = 0; x < W; ++x) w.blockCells(x, H / 2, 1, 3, true);
        const bool after = w.pathExists(&t, 32.0f, 32.0f, float(W * 16 - 32), float(H * 16 - 32));
        check(before, "open map: corner to corner reachable");
        check(!after, "wall across the full width: NOT reachable (fast path must refuse)");
        // Punching a hole back through must MERGE the two halves again -- the union-find
        // path, queried with a warm cache so the incremental route is the one under test.
        for (int z = H / 2; z < H / 2 + 3; ++z) w.blockCells(W / 2, z, 4, 1, false);
        const bool healed = w.pathExists(&t, 32.0f, 32.0f, float(W * 16 - 32), float(H * 16 - 32));
        check(healed, "gate reopened in that wall: reachable again (unblock must merge)");
    }

    std::printf(bad || g_fail ? "navincr_test: FAILURES\n" : "navincr_test: all passed\n");
    return g_fail ? 1 : 0;
}
