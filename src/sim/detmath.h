#pragma once

// Deterministic transcendentals for the simulation. See docs/detmath-scope.md.
//
// Lockstep needs bit-identical sim state across peers built with DIFFERENT
// compilers, C libraries, or CPU architectures -- not just the same binary.
// IEEE-754 makes + - * / and sqrt correctly-rounded (so they already agree
// bit-for-bit everywhere, given -ffp-contract=off and round-to-nearest, which
// the sim never changes), and fmod is exact. The libm transcendentals are NOT
// correctly rounded -- each platform's sin/cos/atan2 may differ in the last bit,
// and because the state hash mixes exact float bits, a 1-ULP difference
// compounds tick over tick into a desync.
//
// These replace the sin/cos/atan2 the sim uses with fixed-algorithm code built
// only from correctly-rounded primitives, so every conforming platform produces
// identical bits. Sim code MUST route trig through here and never call
// std::sin/cos/tan/atan2/atan/hypot/pow/exp/log on hash-affecting values.
// (std::sqrt and std::fmod are fine: correctly-rounded / exact respectively.)

#include <cmath>

namespace ta::detmath {

// Bit-identical across builds. Accurate to well under 1e-5 vs libm.
float sin(float x);
float cos(float x);
float atan2(float y, float x);   // same argument order and range as std::atan2

// hypot replacement: sqrt is correctly rounded, so sqrt(a*a+b*b) is portable
// (unlike libm hypot, which is not correctly rounded).
inline float len(float a, float b) { return std::sqrt(a * a + b * b); }

}  // namespace ta::detmath
