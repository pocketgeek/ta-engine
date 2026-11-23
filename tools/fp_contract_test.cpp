// Guard for the project-wide -ffp-contract=off (CMakeLists.txt, next to -Wall).
//
// Floating-point contraction turns a*b+c into one fused multiply-add: one rounding
// instead of two, so a different last bit. Lockstep needs every build of every
// peer -- and the server's AI, whose commands every peer applies -- to round the
// same way. GCC on baseline x86-64 has no FMA instruction to fuse into, so this
// test cannot fail there; Apple Clang on arm64 contracts by DEFAULT, and it is the
// build that fails here if the flag is ever lost or scoped to fewer targets again.
//
// Every operand is read through a volatile so nothing is constant-folded: the
// expressions below are compiled exactly the way sim/AI code is.
//
// The first case is the one that split Mac-hosted games from Linux/Windows ones:
// the AI's door-clearing Move, `factory.z + sin(a) * r`, at tick 3271 of the
// 300 s Inner Circle Legion --mpai game (fused: 912.33337, unfused: 912.333).

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

volatile float vfz = 720.0f, vsin = 0x1.6a09e6p-1f, vr = 272.0f;
volatile float va = 1.0f + 0x1p-12f, vc = -(1.0f + 0x1p-11f);
volatile double vda = 1.0 + 0x1p-27, vdc = -(1.0 + 0x1p-26);

uint32_t bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
uint64_t bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

int failures = 0;
template <class T> void expect(const char* what, T got, T want) {
    if (bits(got) == bits(want)) return;
    ++failures;
    std::printf("FAIL %s: got %a, want %a (two roundings) -- a*b+c was contracted "
                "into an FMA; is -ffp-contract=off still project-wide?\n",
                what, double(got), double(want));
}

}  // namespace

int main() {
    // The AI expression, written as ai.cpp writes it.
    {
        const float fz = vfz, s = vsin, r = vr;
        const float z = fz + s * r;
        expect("ai door-clear  fz + sin(a)*r", z, 0x1.c82aa0p+9f);
    }
    // a*a + c: a*a = 1 + 2^-11 + 2^-24 rounds (tie to even) to 1 + 2^-11, so the
    // unfused sum is exactly 0 and the fused one is 2^-24.
    {
        const float a = va, c = vc;
        expect("float a*a + c", a * a + c, 0.0f);
    }
    {
        const double a = vda, c = vdc;
        expect("double a*a + c", a * a + c, 0.0);
    }
    if (failures) return 1;
    std::printf("fp_contract: OK (no contraction)\n");
    return 0;
}
