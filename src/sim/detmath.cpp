#include "sim/detmath.h"

// Belt-and-suspenders: forbid a*b+c -> FMA fusion in THIS translation unit
// regardless of the build flags, so the op sequence below is exactly what runs.
// (ta-formats also builds with -ffp-contract=off; this makes the guarantee
// local to the file that most depends on it.)
// Clang honours this pragma; GCC doesn't implement it (and warns), but the whole
// library is compiled -ffp-contract=off, so GCC already has the guarantee.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

// Everything here is a FIXED sequence of correctly-rounded IEEE double ops
// (+ - * / and an exact truncating cast) in a FIXED evaluation order. By the
// IEEE-754 correctness guarantee -- with round-to-nearest-even (the default,
// never changed) and no contraction -- each op yields identical bits on every
// conforming platform, so the whole function does too. We compute in double and
// round once to float on return; x86-64 (SSE2) and ARM (VFP/NEON) both keep
// double in 64-bit registers, with no 80-bit x87 extended precision to leak.

namespace ta::detmath {
namespace {

constexpr double kPi      = 3.141592653589793115997963468544185161590576171875;
constexpr double kHalfPi  = 1.5707963267948965579989817342720925807952880859375;
constexpr double kQtrPi   = 0.78539816339744827899949086713604629039764404296875;
constexpr double kTwoPi   = 6.28318530717958623199592693708837032318115234375;
constexpr double kInvTwoPi = 0.15915494309189534560822210096995416097342967987060546875;

// sin, Taylor (odd terms) on the folded range [-pi/2, pi/2]. Truncating after
// x^13 leaves < 1e-9 absolute error there -- far under the 1e-5 target, and the
// series is a fixed, portable set of ratios.
constexpr double kS3  = -0.16666666666666665741480812812369549646973609924316406250;
constexpr double kS5  =  0.00833333333333333287074040640404421836603432893753051758;
constexpr double kS7  = -0.00019841269841269841252631711547849185101946070790290833;
constexpr double kS9  =  0.00000275573192239858882111660477199294099677121500484645;
constexpr double kS11 = -0.00000002505210838544171877505252248329474013452020846307;
constexpr double kS13 =  0.00000000016059043836821613341070835748664826140090047169;

// atan on the reduced range [-tan(pi/8), tan(pi/8)] ~ [-0.414, 0.414]
// (Cody/Cephes single-precision minimax; ~1e-7 there).
constexpr double kTanPi8 = 0.41421356237309514547462185873882845044136047363281250;
constexpr double kA1 = -0.3333294915390000;
constexpr double kA2 =  0.1997771064780000;
constexpr double kA3 = -0.1387768560320000;
constexpr double kA4 =  0.0805374449538000;

// sin over the full line. Range-reduce to [-pi, pi] with a truncating cast (no
// dependence on the FP rounding mode), fold to [-pi/2, pi/2], then the polynomial.
double sinCore(double x) {
    double t = x * kInvTwoPi;
    double k = double(static_cast<long long>(t >= 0.0 ? t + 0.5 : t - 0.5));
    x -= k * kTwoPi;                       // x now in [-pi, pi]
    if (x > kHalfPi)       x = kPi - x;    // sin(pi - x)  = sin x
    else if (x < -kHalfPi) x = -kPi - x;   // sin(-pi - x) = sin x
    double z = x * x;
    double p = kS13;
    p = p * z + kS11;
    p = p * z + kS9;
    p = p * z + kS7;
    p = p * z + kS5;
    p = p * z + kS3;
    p = p * z + 1.0;
    return x * p;
}

}  // namespace

float sin(float x) { return float(sinCore(double(x))); }
float cos(float x) { return float(sinCore(double(x) + kHalfPi)); }

float atan2(float yf, float xf) {
    double y = double(yf), x = double(xf);
    double ax = x < 0.0 ? -x : x;
    double ay = y < 0.0 ? -y : y;
    if (ax == 0.0 && ay == 0.0) return 0.0f;   // define atan2(0,0) = 0
    // Work with the ratio in [0,1] (avoids the large-argument atan branch), then
    // reflect. q = min(|x|,|y|) / max(|x|,|y|).
    bool swap = ay > ax;
    double q = swap ? ax / ay : ay / ax;
    double base = 0.0;
    if (q > kTanPi8) { base = kQtrPi; q = (q - 1.0) / (q + 1.0); }  // -> [-0.414, 0.414]
    double z = q * q;
    double p = kA4;
    p = p * z + kA3;
    p = p * z + kA2;
    p = p * z + kA1;
    p = p * z * q + q;                 // atan(q)
    double ang = base + p;             // angle of (max,min) in [0, pi/2]
    if (swap) ang = kHalfPi - ang;     // undo the min/max swap
    if (x < 0.0) ang = kPi - ang;      // 2nd/3rd quadrant
    if (y < 0.0) ang = -ang;           // below the x axis
    return float(ang);
}

}  // namespace ta::detmath
