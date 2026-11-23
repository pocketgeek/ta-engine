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
#include "sim/retailaim.h"
#include "sim/retailmotion.h"
#include "sim/retailexploration.h"

namespace {

uint64_t g_hash = 1469598103934665603ULL;   // FNV-1a
void mixBits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    for (int i = 0; i < 4; ++i) { g_hash ^= (b >> (i * 8)) & 0xFF; g_hash *= 1099511628211ULL; }
}

}  // namespace

namespace tak::sim {
inline Bam referenceDirection(Fixed y, Fixed x) {
    if (!x.v && !y.v) return Bam(0);
    int64_t vx = int64_t(x.v) * (int64_t(1) << 28);
    int64_t vy = int64_t(y.v) * (int64_t(1) << 28);
    int64_t angle = 0;
    if (vx < 0) {
        angle = (vy >= 0 ? int64_t(32768) : -int64_t(32768)) * (int64_t(1) << 32);
        vx = -vx;
        vy = -vy;
    }
    for (unsigned i = 0; i < detail::directionAngles.size() && vy; ++i) {
        const int64_t dx = vx >> i, dy = vy >> i;
        if (vy > 0) {
            vx += dy; vy -= dx; angle += detail::directionAngles[i];
        } else {
            vx -= dy; vy += dx; angle -= detail::directionAngles[i];
        }
    }
    return bamWrap(int32_t((angle + (int64_t(1) << 31)) >> 32));
}

}

int main(int argc, char** argv) {
    bool hashOnly = argc > 1 && std::string(argv[1]) == "--hash";

    double sinMax = 0, cosMax = 0, atanMax = 0;

    // sin / cos over several full turns, fine step (covers reduction + all folds).
    for (int i = -20000; i <= 20000; ++i) {
        float x = float(i) * 0.001f;               // [-20, 20] rad, step 0.001
        float ds = tak::detmath::sin(x), dc = tak::detmath::cos(x);
        mixBits(ds); mixBits(dc);
        sinMax = std::max(sinMax, std::fabs(double(ds) - std::sin(double(x))));
        cosMax = std::max(cosMax, std::fabs(double(dc) - std::cos(double(x))));
    }

    // atan2 over a grid across all quadrants and both axes (incl. 0,0).
    for (int iy = -50; iy <= 50; ++iy)
        for (int ix = -50; ix <= 50; ++ix) {
            float y = float(iy) * 0.37f, x = float(ix) * 0.41f;
            float da = tak::detmath::atan2(y, x);
            mixBits(da);
            double ref = (x == 0.0f && y == 0.0f) ? 0.0 : std::atan2(double(y), double(x));
            atanMax = std::max(atanMax, std::fabs(double(da) - ref));
        }

    // Exploration's stored binary32 slope feeds hashed movement inputs. Include
    // its terrain producer and admitted cells in the cross-compiler golden.
    const auto heights=tak::sim::retailExplorationHeights(24,28,32,
        [](int x,int z){return uint8_t((x*37+z*13)%256);});
    for (const auto& h:heights) {mixBits(float(h[0]));mixBits(float(h[1]));}
    for (int i=0;i<512;++i) {
        tak::sim::RetailSightFootprint sight{int16_t(i%16-2),int16_t(i/16%18-2),
            i%385-64,int16_t(i*17%512),uint8_t(i),false};
        tak::sim::retailSightFootprint(sight,true,true,12,14,uint8_t(i%16),
            [&](int x,int z){return heights[size_t(z)*12+x];},
            [](int x,int z,int delta,uint16_t mask){
                mixBits(float(x));mixBits(float(z));mixBits(float(delta));mixBits(float(mask));
            });
        mixBits(float(sight.active));
    }

    double preciseAtanMax=0;
    auto sampleAtan=[&](double x) {
        const double value=tak::detmath::atan(x);
        const auto bits=std::bit_cast<uint64_t>(value);
        for(int j=0;j<8;++j) {g_hash^=(bits>>(j*8))&255;g_hash*=1099511628211ULL;}
        preciseAtanMax=std::max(preciseAtanMax,std::abs(value-std::atan(x)));
    };
    for(int i=-20000;i<=20000;++i)sampleAtan(double(i)/10000);
    for(int exponent=-1074;exponent<=1023;++exponent) {
        const double value=std::ldexp(1.0,exponent);
        sampleAtan(value);sampleAtan(-value);
    }
    for(int i=1;i<=4096;++i)
        mixBits(float(tak::retailBallisticPitch(float(i%317)-158,float(i%101)-50,
            float(i%593)-296,float(i%59)+0.25f,float(i%13+1)/8,i%2)));
    if(preciseAtanMax>5e-16) {std::printf("FAIL: double atan error %.17g\n",preciseAtanMax);return 1;}

    std::printf("detmath golden %016llx\n", (unsigned long long)g_hash);
    if (hashOnly) return 0;

    // Integer-root invariants are exact and also exercise UINT64_MAX, where
    // the old n+1 Newton seed overflowed. Division avoids squaring overflow.
    auto rootValid=[](uint64_t n) {
        const uint64_t r=tak::sim::isqrt64(n);
        return n==0 ? r==0 : r>0 && r<=n/r && r+1>n/(r+1);
    };
    uint64_t rootRng=0x987654321abcdefULL;
    for (int i=0;i<200000;++i) {
        rootRng=rootRng*6364136223846793005ULL+1442695040888963407ULL;
        if (!rootValid(rootRng)) {std::puts("FAIL: integer square root");return 1;}
    }
    for (unsigned b=0;b<64;++b) {
        const uint64_t n=uint64_t(1)<<b;
        if (!rootValid(n-1) || !rootValid(n) || !rootValid(n+1)) return 1;
    }
    for (uint64_t r=1;r<=65536;++r) {
        const uint64_t n=r*r;
        if (!rootValid(n-1) || !rootValid(n) || !rootValid(n+1)) return 1;
    }
    for (uint64_t r:{uint64_t(1)<<31,(uint64_t(1)<<32)-1}) {
        const uint64_t n=r*r;
        if (!rootValid(n-1) || !rootValid(n) || !rootValid(n+1)) return 1;
    }
    if (!rootValid(UINT64_MAX)) return 1;

    // Retain the full CORDIC as an independent reference for the early exit.
    // Include arbitrary signed fixed-point values, not just small map vectors.
    uint32_t rng=0x87654321;
    for (int i=0;i<200000;++i) {
        rng=rng*1664525u+1013904223u;
        const auto x=tak::sim::Fixed::raw(std::bit_cast<int32_t>(rng));
        rng=rng*1664525u+1013904223u;
        const auto y=tak::sim::Fixed::raw(std::bit_cast<int32_t>(rng));
        if (tak::sim::retailDirection(y,x)!=tak::sim::referenceDirection(y,x)) {
            std::printf("FAIL: direction early exit differs at %d,%d\n",y.v,x.v);
            return 1;
        }
    }
    for (int y:{0,1,-1,32767,-32768,2147483647,(-2147483647-1)})
        for (int x:{0,1,-1,32767,-32768,2147483647,(-2147483647-1)})
            if (tak::sim::retailDirection(tak::sim::Fixed::raw(y),tak::sim::Fixed::raw(x))!=
                tak::sim::referenceDirection(tak::sim::Fixed::raw(y),tak::sim::Fixed::raw(x))) return 1;


    std::printf("accuracy vs libm: sin<=%.3e cos<=%.3e atan2<=%.3e\n", sinMax, cosMax, atanMax);
    const double kTol = 1e-5;
    if (sinMax > kTol || cosMax > kTol || atanMax > kTol) {
        std::printf("FAIL: exceeds tolerance %.1e\n", kTol);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
