// gpuvram_test -- the GPU memory cap's policy (client/gpuvrampolicy.h), driven with a
// fake clock: the Options limit (live both ways, floor, no-op re-apply), the 3 s bake
// backoff, the post-failure tightening, and that tightening lifting again (60 s, then
// doubling per re-tightening up to 8 min). Before 2026-10 a failure clamped the cap for
// the rest of the session; the "relaxes" cases are what catch that coming back.

#include "client/gpuvrampolicy.h"

#include <cstdio>

using namespace gpuvram;

static int g_fail = 0;
static void check(const char* what, bool ok) {
    std::printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

int main() {
    std::printf("gpuvram_test: cap policy\n");
    constexpr size_t M = kMiB;
    {
        Policy p;
        check("default: the default limit is the cap", p.limit == kDefaultLimit && p.cap == kDefaultLimit && !p.tightened());
        check("fits up to the cap, not past it", p.fits(kDefaultLimit - M, M) && !p.fits(kDefaultLimit, 1));
        check("fits is overflow-safe", !p.fits(SIZE_MAX - 1, 2) && !p.fits(2, SIZE_MAX));
        check("not blocked before any failure", !p.blocked(0) && !p.blocked(123456));
    }
    {
        Policy p;
        check("lower limit applies at once", p.setLimit(512 * M) && p.cap == 512 * M && !p.fits(500 * M, 20 * M));
        check("raise limit applies at once", p.setLimit(2048 * M) && p.cap == 2048 * M && p.fits(1500 * M, 500 * M));
        check("limit never below the floor", p.setLimit(100 * M) && p.limit == kFloor && p.cap == kFloor);
        p.setLimit(kUnlimited);
        check("unlimited fits anything that adds up", p.fits(size_t(1) << 40, size_t(1) << 40));
        check("unlimited still overflow-safe", !p.fits(SIZE_MAX / 2 + 1, SIZE_MAX / 2 + 1));
    }
    {
        Policy p;
        check("small failure: no tightening", !p.fail(300 * M, 1000) && p.cap == kDefaultLimit && !p.tightened());
        check("...but bakes back off 3 s", p.blocked(1000) && p.blocked(3999) && !p.blocked(4000));
    }
    {
        Policy p;
        check("failure tightens to 87.5% held", p.fail(800 * M, 1000) && p.cap == 700 * M && p.tightened());
        check("tightened cap refuses growth", !p.fits(690 * M, 20 * M));
        check("not relaxed before 60 s", !p.relax(1000 + kRelaxMs - 1) && p.cap == 700 * M);
        check("relaxes to the limit after 60 s", p.relax(1000 + kRelaxMs) && p.cap == kDefaultLimit && !p.tightened());
        check("relax is one-shot", !p.relax(1000 + 2 * kRelaxMs));
        // Second and later tightenings wait twice as long each, up to the maximum.
        uint64_t t = 200000;
        check("2nd tightening", p.fail(800 * M, t) && p.relaxAt == t + 2 * kRelaxMs);
        p.relax(t + 2 * kRelaxMs);
        t += 1000000;
        check("3rd tightening waits 4x", p.fail(800 * M, t) && p.relaxAt == t + 4 * kRelaxMs);
        p.relax(t + 4 * kRelaxMs);
        for (int i = 0; i < 6; ++i) { t += 10000000; p.fail(800 * M, t); p.relax(t + kRelaxMaxMs); }
        t += 10000000;
        check("wait capped at 8 min", p.fail(800 * M, t) && p.relaxAt == t + kRelaxMaxMs);
    }
    {
        Policy p;
        p.fail(800 * M, 1000);
        check("failure while tightened postpones relax", !p.fail(900 * M, 30000) && p.cap == 700 * M &&
                                                          p.relaxAt == 30000 + kRelaxMs);
        check("deeper failure tightens further", p.fail(500 * M, 40000) && p.cap == 437 * M + 512 * 1024);
        check("tightening never below the floor", p.fail(400 * M, 50000) && p.cap == kFloor);
    }
    {
        Policy p;
        p.fail(800 * M, 1000);
        check("re-applying the same limit keeps a tightening", !p.setLimit(kDefaultLimit) && p.cap == 700 * M);
        check("a new limit replaces a tightening", p.setLimit(3072 * M) && p.cap == 3072 * M && !p.tightened());
        p.fail(1000 * M, 5000);
        check("relax returns to the NEW limit", p.relax(5000 + 2 * kRelaxMs) && p.cap == 3072 * M);
        p.setLimit(kUnlimited);
        check("tightening from unlimited", p.fail(1600 * M, 9000) && p.cap == 1400 * M);
        check("relax back to unlimited", p.relax(9000 + 4 * kRelaxMs) && p.cap == kUnlimited);
    }
    std::printf(g_fail ? "gpuvram_test: %d FAILURE(S)\n" : "gpuvram_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
