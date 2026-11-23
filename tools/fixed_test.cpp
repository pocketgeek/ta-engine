// fixed_test -- the arithmetic the sim's positions will be built on.
//
// This is the net for a conversion that touches ~240 sites of HASHED state, where a
// single wrong shift is a desync rather than a visible bug. Every property here is one
// the port depends on: exact representation of the values routes are built from,
// rounding that matches on both signs, cell indexing that floors rather than truncates
// toward zero, and a multiply that cannot overflow before it is shifted back.
//
// The tangency case has its own check, because it is the reason this type exists: a
// unit one cell clear of a body must be EXACTLY clear, with no drift to wedge on.

#include "sim/fixed.h"

#include <cstdio>
#include <cstdint>
#include <string>

using namespace tak::sim;

static int failures = 0;
static void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("  %-58s %s%s%s\n", what.c_str(), ok ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}

int main() {
    std::printf("fixed_test -- deterministic 16.16 for sim positions\n");

    // --- out-of-range float->int conversions are defined and platform-independent ---
    {
        check(Fixed::fromFloat(32767.5f).v == int32_t(32767.5 * 65536), "fromFloat in range exact");
        check(Fixed::fromFloat(-1.5f).v == -98304 && Fixed::fromFloat(0.5f / 65536.f).v == 1,
              "fromFloat rounds half away from zero");
        check(Fixed::fromFloat(32768.0f).v == INT32_MAX && Fixed::fromFloat(1e30f).v == INT32_MAX,
              "fromFloat saturates high");
        check(Fixed::fromFloat(-32769.0f).v == INT32_MIN && Fixed::fromFloat(-1e30f).v == INT32_MIN,
              "fromFloat saturates low");
        check(Fixed::fromFloat(std::nanf("")).v == 0, "fromFloat(NaN) is 0");
        check(Fixed::fromRetailNumber(1.99999) .v == int32_t(1.99999 * 65536) &&
              Fixed::fromRetailNumber(-2.5).v == -163840, "fromRetailNumber truncates toward zero in range");
        check(Fixed::fromRetailNumber(32768.0).v == INT32_MAX && Fixed::fromRetailNumber(1e300).v == INT32_MAX,
              "fromRetailNumber saturates high");
        check(Fixed::fromRetailNumber(-32768.0).v == INT32_MIN && Fixed::fromRetailNumber(-1e300).v == INT32_MIN,
              "fromRetailNumber saturates low");
        check(truncSat32(3e9) == INT32_MAX && truncSat32(-3e9) == INT32_MIN && truncSat32(-2.9) == -2 &&
              truncSat32(std::nan("")) == 0, "truncSat32");
        check(roundSat32(2147483647.0) == INT32_MAX && roundSat32(-1e12) == INT32_MIN &&
              roundSat32(2.5) == 3 && roundSat32(-2.5) == -3, "roundSat32");
        check(bamFromRadians(1e30f) == bamFromRadians(1e30f) && bamFromRadians(0.0f).v == 0,
              "bamFromRadians huge input is defined");
    }

    // --- exactness of the values positions actually take ---------------------
    check(Fixed::fromInt(568).toFloat() == 568.0f, "whole pixels are exact");
    check((Fixed::fromInt(600) - Fixed::fromInt(568)).toFloat() == 32.0f,
          "a 32px footprint gap is exact (the tangency case)");
    // Half a cell, a quarter, an eighth: all the fractions a 16px grid produces.
    check(Fixed::raw(Fixed::kOne / 2).toFloat() == 0.5f,   "1/2 px exact");
    check(Fixed::raw(Fixed::kOne / 16).toFloat() == 0.0625f, "1/16 px exact");
    check(Fixed::raw(Fixed::fromInt(16).v / 16).toFloat() == 1.0f, "one cell / 16 is one pixel");

    // --- the wedge this type exists to prevent -------------------------------
    {
        // The route's corner waypoint, one cell clear of a parked body. In float the
        // unit arrived at 568.04 and sat 0.04px inside, refused for ever.
        const Fixed ux = Fixed::fromInt(568), ox = Fixed::fromInt(600);
        const Fixed sep = Fixed::fromInt(32);
        const Fixed margin = sep - fxAbs(ux - ox);
        check(margin.v == 0, "tangent waypoint has EXACTLY zero margin, not 0.04px",
              "margin raw=" + std::to_string(margin.v));
        // ...and stays exact after a round trip through arithmetic, which is what the
        // mover does to it every tick.
        Fixed acc = ux;
        for (int i = 0; i < 1000; ++i) { acc += Fixed::fromInt(1); acc -= Fixed::fromInt(1); }
        check(acc == ux, "1000 add/subtract round trips do not drift");
    }

    // --- rounding is the same on both signs ----------------------------------
    {
        const Fixed a = Fixed::raw(Fixed::fromInt(3).v / 2);     // +1.5
        const Fixed b = -Fixed::raw(Fixed::fromInt(3).v / 2);  // -1.5
        check(a.floorInt() == 1,  "floorInt(+1.5) == 1");
        check(b.floorInt() == -2, "floorInt(-1.5) == -2 (floors, not truncates)",
              "got " + std::to_string(b.floorInt()));
        // A cell index must not fold -0.5 and +0.5 into the same cell.
        check(Fixed::raw(-1).floorInt() == -1, "the smallest negative is cell -1, not 0");
    }

    // --- multiply: int64 intermediate, no overflow before the shift ----------
    {
        const Fixed big = Fixed::fromInt(4000);          // beyond any map edge
        const Fixed half = Fixed::raw(Fixed::kOne / 2);
        check((big * half).toFloat() == 2000.0f, "4000 * 0.5 == 2000 (no overflow)");
        const Fixed step = Fixed::raw(Fixed::fromInt(70).v / 30);      // 70px/s at 30Hz
        check(step.v > 0 && step.toFloat() > 2.3f && step.toFloat() < 2.34f,
              "a 70px/s step at 30Hz lands near 2.333px",
              std::to_string(step.toFloat()));
    }

    // --- length, integer-only ------------------------------------------------
    {
        check(fxLen(Fixed::fromInt(3), Fixed::fromInt(4)) == Fixed::fromInt(5),
              "3,4,5 triangle is exact");
        check(fxLen(Fixed::fromInt(0), Fixed::fromInt(0)) == Fixed(), "len(0,0) == 0");
        const Fixed d = fxLen(Fixed::fromInt(2560), Fixed::fromInt(2560));
        check(d.toFloat() > 3620.0f && d.toFloat() < 3622.0f,
              "a full map diagonal is in range and sane", std::to_string(d.toFloat()));
    }

    // --- isqrt agrees with the real thing over a wide sweep ------------------
    {
        int bad = 0;
        for (uint64_t n = 0; n < 4000000; n += 997) {
            const uint64_t r = isqrt64(n);
            if (r * r > n || (r + 1) * (r + 1) <= n) ++bad;
        }
        check(bad == 0, "isqrt64 is the exact integer square root over 4014 samples",
              bad ? std::to_string(bad) + " wrong" : "");
    }

    // --- integer trig: accuracy, and the identities that matter ---------------
    {
        double worst = 0;
        for (int32_t ai = 0; ai < kBamFullV; ai += 7) { const Bam a{ai};          // 9363 angles
            const SinCos sc = fxSinCos(a);
            const double want = double(a.v) * (2.0 * 3.14159265358979323846 / 65536.0);
            worst = std::max(worst, std::fabs(sc.s.toFloat() - std::sin(want)));
            worst = std::max(worst, std::fabs(sc.c.toFloat() - std::cos(want)));
        }
        check(worst < 2e-3, "CORDIC sin/cos within 2e-3 of libm over 9363 angles",
              "worst " + std::to_string(worst));
    }
    {
        // The quadrant folding is where a sign error hides, so pin the cardinals.
        // EXACT, not close. A residual here is a sideways push on every axis-aligned
        // step, and those are the common case.
        check(fxSin(Bam(0)).v == 0,                              "sin(0) is exactly 0");
        check(fxCos(Bam(0)) == Fixed::fromInt(1),                "cos(0) is exactly 1");
        check(fxSin(kBamQuarter) == Fixed::fromInt(1),      "sin(90) is exactly 1");
        check(fxCos(kBamQuarter).v == 0,                    "cos(90) is exactly 0");
        check(fxSin(kBamHalf).v == 0,                       "sin(180) is exactly 0");
        check(fxCos(kBamHalf) == -Fixed::fromInt(1),        "cos(180) is exactly -1");
        check(fxSin(kBamHalf + kBamQuarter) == -Fixed::fromInt(1), "sin(270) is exactly -1");
        check(fxCos(kBamHalf + kBamQuarter).v == 0,         "cos(270) is exactly 0");
        // ...and a unit walking due east for a five-minute game gains no sideways drift.
        Fixed side;
        for (int i = 0; i < 10000; ++i) side += fxSin(Bam(0)) * Fixed::fromInt(2);
        check(side.v == 0, "10000 axis-aligned steps drift sideways by exactly nothing");
    }
    {
        // Wrapping is the property radians cannot give us: adding angles for ever
        // must not drift, and must land exactly back where it started.
        Bam a{12345};
        for (int i = 0; i < 100000; ++i) a = a + Bam(999);
        check(a == bamWrap(12345 + 100000 * 999),
              "100k angle additions wrap exactly, with no drift");
        check(fxSin(Bam(5)) == fxSin(bamWrap(5 + kBamFullV)),
              "sin is exactly periodic across a wrap");
    }
    {
        // sin^2 + cos^2 == 1, the standard CORDIC gain check.
        double worstId = 0;
        for (int32_t ai = 0; ai < kBamFullV; ai += 101) { const Bam a{ai};
            const SinCos sc = fxSinCos(a);
            const double id = double(sc.s.toFloat()) * sc.s.toFloat()
                            + double(sc.c.toFloat()) * sc.c.toFloat();
            worstId = std::max(worstId, std::fabs(id - 1.0));
        }
        check(worstId < 5e-3, "sin^2 + cos^2 == 1 (the gain is right)",
              "worst " + std::to_string(worstId));
    }

    // --- angle difference: exact, and correct across the wrap ----------------
    {
        check(bamDiff(Bam(10), Bam(10)) == 0,                    "diff(a,a) == 0");
        check(bamDiff(Bam(100), Bam(50)) == 50,                  "a simple difference");
        check(bamDiff(Bam(50), Bam(100)) == -50,                 "and its negation");
        // The case a float angleDiff gets wrong when rounding lands it past pi.
        check(bamDiff(Bam(10), bamWrap(kBamFullV - 10)) == 20,        "across the zero wrap, the SHORT way");
        check(bamDiff(bamWrap(kBamFullV - 10), Bam(10)) == -20,       "and back the other way");
        check(bamDiff(Bam(0), kBamHalf) == kBamHalfV,        "exactly opposite resolves to +180");
    }
    // --- atan2, against libm, and round-tripped through sin/cos --------------
    {
        double worst = 0;
        int checked = 0;
        for (int yi = -2000; yi <= 2000; yi += 37)
            for (int xi = -2000; xi <= 2000; xi += 37) {
                if (xi == 0 && yi == 0) continue;
                const Bam got = fxAtan2(Fixed::fromInt(yi), Fixed::fromInt(xi));
                double want = std::atan2(double(yi), double(xi));
                if (want < 0) want += 2.0 * 3.14159265358979323846;
                double gotR = double(got.v) * (2.0 * 3.14159265358979323846 / 65536.0);
                double d = std::fabs(gotR - want);
                if (d > 3.14159265358979323846) d = 2.0 * 3.14159265358979323846 - d;
                worst = std::max(worst, d); ++checked;
            }
        check(worst < 2e-3, "fxAtan2 within 2e-3 rad of libm over " + std::to_string(checked)
                            + " points", "worst " + std::to_string(worst));
    }
    {
        // The identity the mover relies on: aim at a point, step that way, arrive.
        int bad = 0;
        for (int i = 0; i < 360; i += 7) {
            const Bam a = bamWrap(int32_t(int64_t(i) * kBamFullV / 360));
            const SinCos sc = fxSinCos(a);
            const Bam back = fxAtan2(sc.s, sc.c);
            if (std::abs(bamDiff(back, a)) > 40) ++bad;   // 40/65536 = 0.2 degrees
        }
        check(bad == 0, "atan2(sin(a), cos(a)) round-trips to within 0.2 degrees",
              bad ? std::to_string(bad) + " of 52 off" : "");
    }

    std::printf(failures ? "fixed_test: FAILURES\n" : "fixed_test: all passed\n");
    return failures ? 1 : 0;
}
