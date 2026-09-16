// Test + golden-vector tool for the deterministic math shim (docs/detmath-scope.md).
//
//   detmath_test           -> accuracy check vs libm + a golden hash of all
//                             detmath output bits; exit nonzero if inaccurate.
//   detmath_test --hash     -> print only the golden hash line.
//
// Cross-build determinism check: build this with two toolchains (e.g. gcc and
// clang, or -O0 and -O3) and confirm the "detmath golden" hash line is
// identical. The hash mixes the EXACT output bits of every detmath call over a
// fixed input grid, so any last-bit divergence changes it.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "sim/detmath.h"

namespace {

uint64_t g_hash = 1469598103934665603ULL;   // FNV-1a
void mixBits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    for (int i = 0; i < 4; ++i) { g_hash ^= (b >> (i * 8)) & 0xFF; g_hash *= 1099511628211ULL; }
}

}  // namespace

int main(int argc, char** argv) {
    bool hashOnly = argc > 1 && std::string(argv[1]) == "--hash";

    double sinMax = 0, cosMax = 0, atanMax = 0;

    // sin / cos over several full turns, fine step (covers reduction + all folds).
    for (int i = -20000; i <= 20000; ++i) {
        float x = float(i) * 0.001f;               // [-20, 20] rad, step 0.001
        float ds = ta::detmath::sin(x), dc = ta::detmath::cos(x);
        mixBits(ds); mixBits(dc);
        sinMax = std::max(sinMax, std::fabs(double(ds) - std::sin(double(x))));
        cosMax = std::max(cosMax, std::fabs(double(dc) - std::cos(double(x))));
    }

    // atan2 over a grid across all quadrants and both axes (incl. 0,0).
    for (int iy = -50; iy <= 50; ++iy)
        for (int ix = -50; ix <= 50; ++ix) {
            float y = float(iy) * 0.37f, x = float(ix) * 0.41f;
            float da = ta::detmath::atan2(y, x);
            mixBits(da);
            double ref = (x == 0.0f && y == 0.0f) ? 0.0 : std::atan2(double(y), double(x));
            atanMax = std::max(atanMax, std::fabs(double(da) - ref));
        }

    std::printf("detmath golden %016llx\n", (unsigned long long)g_hash);
    if (hashOnly) return 0;

    std::printf("accuracy vs libm: sin<=%.3e cos<=%.3e atan2<=%.3e\n", sinMax, cosMax, atanMax);
    const double kTol = 1e-5;
    if (sinMax > kTol || cosMax > kTol || atanMax > kTol) {
        std::printf("FAIL: exceeds tolerance %.1e\n", kTol);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
