#pragma once
// Deterministic fixed-point scalar for the simulation's positions.
//
// WHY. Lockstep requires every peer to compute byte-identical state. Float positions
// get there only by discipline: detmath for the transcendentals, -ffp-contract=off so
// no compiler fuses a*b+c differently, and a guard script that greps for stray libm
// calls. That is a rule maintained by vigilance, and it leaks -- a traced route's
// corner waypoint past a parked body is EXACTLY tangent, footprints touching at 32px
// with a margin of 0.000, and float drift of four hundredths of a pixel was enough to
// wedge a unit against it permanently. Integers do not drift.
//
// FORMAT. 16.16 in int32: 65536 units per pixel. That is retail's own resolution --
// its mover steps in 0x100000 per 16px cell, which is the same 65536 per pixel -- so
// a waypoint one cell clear of a body is an exact integer here, as it is there, and
// the tangency that needed a sub-pixel slack cannot arise.
//
// RANGE. +/-32768 px, and the largest map (2048 cells of 16 px) is exactly 32768 px
// a side: its east and south edges sit at the top of the range, with no room past
// them. So a sum or a difference never wraps: + and - SATURATE at the ends of the
// range (satAdd32 / satSub32 below). In range that is the plain add it replaces, bit
// for bit; out of range a plain int32 add is undefined behaviour, and in practice it
// put a flyer that crossed the east edge of such a map 65536 px to the west. Products
// use an int64 intermediate, so a multiply cannot overflow before it is shifted back
// down.
//
// CONVERSIONS ARE EXPLICIT, deliberately. An implicit float conversion would let a
// stray double creep back into the hashed path and be invisible; making every crossing
// spelled out is what lets the compiler find the sites during the port, and what keeps
// the boundary (rendering, FBI data, the AI's scratch maths) visible afterwards.

#include <cstdint>
#include <bit>
#include <cmath>

namespace ta::sim {

// Defined float->int conversions. A bare int32_t(double) of an out-of-range value is
// undefined behaviour (x86 yields INT32_MIN, ARM saturates), and std::lround returns a
// 32-bit long on Windows. These saturate (NaN -> 0), so every compiler and platform
// agrees; in range they are exactly the truncation / round-half-away they replace.
inline int32_t truncSat32(double d) {
    if (!(d == d)) return 0;
    if (d >= 2147483648.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return int32_t(d);
}
inline int32_t roundSat32(double d) {
    if (!(d == d)) return 0;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return int32_t(std::llround(d));   // long long is 64-bit everywhere
}
// Round-half-away to int64 (not saturated to 32 bits), then the modulo-2^32 wrap that
// C++20 defines: for angles in radians, where wrapping past a full turn is the intent.
inline int32_t roundWrap32(double d) {
    if (!(d == d)) return 0;
    if (d > 4.0e18) d = 4.0e18;
    if (d < -4.0e18) d = -4.0e18;
    return int32_t(std::llround(d));
}

// Saturating add and subtract of two raw 16.16 values: the result stops at INT32_MAX or
// INT32_MIN instead of wrapping. The one helper for every sum that may pass the end of the
// range: Fixed's own + and -, and the retail ports that work on raw position words. Retail
// adds and subtracts those in 32-bit registers, where they wrap; none of its maps is wide
// enough for that to happen, so wherever retail's result is in range these give the same
// one. Past the range they are ours, not retail: a point stays at the edge it would have
// crossed, and a difference keeps its sign.
constexpr int32_t satAdd32(int32_t a, int32_t b) {
    int32_t r = 0;
    if (__builtin_add_overflow(a, b, &r)) return b < 0 ? INT32_MIN : INT32_MAX;
    return r;
}
constexpr int32_t satSub32(int32_t a, int32_t b) {
    int32_t r = 0;
    if (__builtin_sub_overflow(a, b, &r)) return b < 0 ? INT32_MAX : INT32_MIN;
    return r;
}

struct Fixed {
    static constexpr int kBits = 16;
    static constexpr int32_t kOne = 1 << kBits;

    int32_t v = 0;

    constexpr Fixed() = default;
    static constexpr Fixed raw(int32_t r) { Fixed f; f.v = r; return f; }
    // Saturating like + and -: 32768, the far edge of a 2048-cell map in pixels, is the top
    // of the range (the bare shift made it the bottom).
    static constexpr Fixed fromInt(int i) {
        return raw(i > 32767 ? INT32_MAX : i < -32768 ? INT32_MIN : int32_t(i) << kBits);
    }
    // From float ONLY at a boundary: unit data read from FBI, the renderer, a test.
    // Never inside the tick. std::lround is round-half-away-from-zero and identical
    // everywhere, unlike a bare cast's truncation-toward-zero.
    static Fixed fromFloat(float f) { return raw(roundSat32(double(f) * kOne)); }
    // FBI fixed-point reader 0x5431f0 parses a double, scales, then truncates.
    // Keep separate from rounded position/UI boundaries above.
    static Fixed fromRetailNumber(double value) { return raw(truncSat32(value * kOne)); }

    constexpr float toFloat() const { return float(v) / float(kOne); }
    // NO IMPLICIT CONVERSION TO FLOAT, and it is worth saying why this note exists
    // rather than just the absence of an operator.
    //
    // The commit that claimed to delete it (b60cd13) did convert all 484 call sites --
    // that work was real -- but left the operator itself in place: it had been put
    // back temporarily to bisect a hash difference and was never taken out again. So
    // for six commits the port was finished and the guard was missing, and every site
    // written in that window was free to convert silently. Removing it for real turned
    // up 158 of them.
    //
    // What its absence costs is a compile error at every crossing, which is the entire
    // point: a float reaching stored state has to be spelled fromFloat(), and a read
    // has to be spelled toFloat(), so both are reviewable in a diff.
    // Truncates toward NEGATIVE infinity, like a floor, so cell indexing is correct
    // left of the origin. A bare v/kOne would round toward zero and put x=-0.5 in
    // cell 0 alongside x=+0.5.
    constexpr int32_t floorInt() const { return v >> kBits; }

    // Saturating (see RANGE above): never a wrap, never signed-overflow UB.
    constexpr Fixed operator+(Fixed o) const { return raw(satAdd32(v, o.v)); }
    constexpr Fixed operator-(Fixed o) const { return raw(satSub32(v, o.v)); }
    constexpr Fixed operator-() const { return raw(satSub32(0, v)); }
    // int64 intermediate, then an ARITHMETIC shift -- guaranteed since C++20, so the
    // rounding of a negative product is the same on every compiler. Truncates toward
    // negative infinity, consistently for both signs.
    constexpr Fixed operator*(Fixed o) const {
        return raw(int32_t((int64_t(v) * int64_t(o.v)) >> kBits));
    }
    constexpr Fixed operator/(Fixed o) const {
        return o.v == 0 ? raw(0) : raw(int32_t((int64_t(v) << kBits) / int64_t(o.v)));
    }
    // NO operator*(int) / operator/(int), and that is a scar. They existed as a
    // convenience, and combined with the implicit conversion to float above they
    // silently hijacked every float multiply: in `speed * dt`, float->int is a standard
    // conversion while Fixed->float is a user-defined one, so the INT overload wins and
    // dt (0.0333) truncates to 0. Every step became zero-length and the whole sim
    // stopped moving -- crowdbench went to 0/32 across the board with the build green
    // and the compiler emitting only an "ISO C++ says these are ambiguous" warning.
    //
    // Scaling by a whole number goes through fromInt, which cannot be misread.

    constexpr Fixed& operator+=(Fixed o) { v = satAdd32(v, o.v); return *this; }
    constexpr Fixed& operator-=(Fixed o) { v = satSub32(v, o.v); return *this; }

    constexpr bool operator==(Fixed o) const { return v == o.v; }
    constexpr bool operator!=(Fixed o) const { return v != o.v; }
    constexpr bool operator< (Fixed o) const { return v <  o.v; }
    constexpr bool operator<=(Fixed o) const { return v <= o.v; }
    constexpr bool operator> (Fixed o) const { return v >  o.v; }
    constexpr bool operator>=(Fixed o) const { return v >= o.v; }
};

constexpr Fixed operator*(int i, Fixed f) { return Fixed::raw(f.v * int32_t(i)); }
constexpr Fixed fxAbs(Fixed a) { return a.v < 0 ? -a : a; }
constexpr Fixed fxMin(Fixed a, Fixed b) { return a.v < b.v ? a : b; }
constexpr Fixed fxMax(Fixed a, Fixed b) { return a.v > b.v ? a : b; }

// Integer square root of a 64-bit value (Newton, seeded by bit length). Used by the
// length below; no float anywhere, so it cannot differ between builds.
constexpr uint64_t isqrt64(uint64_t n) {
    if (n == 0) return 0;
    // Start above the root, within a factor of two. Starting at n needed
    // roughly half its bit width in divisions before Newton converged.
    uint64_t x = uint64_t(1) << ((std::bit_width(n) + 1) / 2);
    uint64_t y = (x + n / x) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

// hypot in fixed point. The squares are computed in int64 at 32 fractional bits, so
// the sqrt of that is back at 16 -- no intermediate rounding. The sum is unsigned: two
// saturated differences square to 2^62 apiece, one more than an int64 sum can hold.
// The result is the low 32 bits of the length: the length itself below 32768 px, and
// past that (two points on a map from 1449 cells a side can be that far apart) a
// wrapped word, negative from 32768 px, as retail's ground mover reads its own square
// root (4d9bc8).
inline Fixed fxLen(Fixed a, Fixed b) {
    const uint64_t aa = uint64_t(int64_t(a.v) * int64_t(a.v));
    const uint64_t bb = uint64_t(int64_t(b.v) * int64_t(b.v));
    return Fixed::raw(int32_t(uint32_t(isqrt64(aa + bb))));
}

// ---------------------------------------------------------------------------------
// Angles, and integer trig.
//
// A BINARY ANGLE: 65536 == 360 degrees, held in int32 and wrapped by truncation, so
// 0x4000 is a right angle. That is retail's own convention (heading at navigator +0x7e,
// with the mover comparing headings against 0x4000 for its 90-degree test), and it has
// the property radians never will -- adding two angles cannot drift, and wrapping is
// free rather than a fmod that has to agree across libms.
// A DISTINCT TYPE, not an alias for int32_t. That was the first attempt, and it
// compiled the whole heading conversion with ZERO errors -- because every float
// radian silently truncated to an integer and the build stayed green while the sim
// was nonsense. A struct with explicit conversions makes the compiler name every
// site instead, which is the only reason a change of this size is reviewable.
struct Bam {
    int32_t v = 0;
    constexpr Bam() = default;
    constexpr explicit Bam(int32_t raw) : v(raw) {}
    constexpr bool operator==(Bam o) const { return v == o.v; }
    constexpr bool operator!=(Bam o) const { return v != o.v; }
};
constexpr int32_t kBamFullV = 65536, kBamHalfV = 32768, kBamQuarterV = 16384;
constexpr Bam kBamFull{kBamFullV}, kBamHalf{kBamHalfV}, kBamQuarter{kBamQuarterV};
// Wrap to [0, 360). Masking is exact -- this is what radians cannot do.
constexpr Bam bamWrap(int32_t raw) { return Bam(raw & (kBamFullV - 1)); }
constexpr Bam operator+(Bam a, Bam b) { return bamWrap(a.v + b.v); }
constexpr Bam operator-(Bam a, Bam b) { return bamWrap(a.v - b.v); }

// CORDIC, because it is the only way to get sin and cos with no float anywhere. Sixteen
// rotations of shift-and-add drive the residual angle to zero; the table is atan(2^-i)
// in BAM and the constant is the accumulated gain. Both were computed in the open (see
// the generator in the commit message) rather than read out of the retail binary, which
// is for observation only.
//
// Accurate to about 1e-4 of full scale, which is finer than the 1/65536 px the result is
// stored at, and -- the point of the exercise -- IDENTICAL on every machine, because it
// is integer shifts and adds with no rounding mode to disagree about.
namespace detail {
constexpr int32_t kCordicAtan[16] = {
    8192, 4836, 2555, 1297, 651, 326, 163, 81,
    41, 20, 10, 5, 3, 1, 1, 0,
};
constexpr int32_t kCordicGain = 39797;   // 0.6072529351 in 16.16
}  // namespace detail

// sin and cos together: CORDIC produces both, and every caller that wants one wants the
// other a line later.
struct SinCos { Fixed s, c; };

inline SinCos fxSinCos(Bam ang) {
    int32_t a = ang.v & (kBamFullV - 1);       // wrap: free, and exact
    // THE FOUR CARDINALS EXACTLY. CORDIC converges to about 6/65536 at 0 and 90, and
    // that residual is not harmless here: it is 0.0002px of sideways push per step, so
    // a unit ordered straight along an axis slides ~2px off its line over a five-minute
    // walk. Axis-aligned movement is the common case (waypoints sit on cell centres),
    // and drift is the entire reason positions became integers, so these are answered
    // outright rather than approximated.
    switch (a) {
        case 0:                            return {Fixed(), Fixed::fromInt(1)};
        case kBamQuarterV:                 return {Fixed::fromInt(1), Fixed()};
        case kBamHalfV:                    return {Fixed(), -Fixed::fromInt(1)};
        case kBamHalfV + kBamQuarterV:     return {-Fixed::fromInt(1), Fixed()};
        default: break;
    }
    // Fold to the first quadrant and remember the signs, so the rotation only ever has
    // to cover 90 degrees where CORDIC converges.
    bool negS = false, negC = false;
    if (a >= kBamHalfV) { a -= kBamHalfV; negS = !negS; negC = !negC; }
    if (a >= kBamQuarterV) { a = kBamHalfV - a; negC = !negC; }
    int64_t x = detail::kCordicGain, y = 0;
    int32_t z = a;
    for (int i = 0; i < 16; ++i) {
        const int64_t dx = x >> i, dy = y >> i;
        if (z >= 0) { const int64_t nx = x - dy; y = y + dx; x = nx; z -= detail::kCordicAtan[i]; }
        else        { const int64_t nx = x + dy; y = y - dx; x = nx; z += detail::kCordicAtan[i]; }
    }
    SinCos r;
    r.s = Fixed::raw(int32_t(negS ? -y : y));
    r.c = Fixed::raw(int32_t(negC ? -x : x));
    return r;
}
inline Fixed fxSin(Bam a) { return fxSinCos(a).s; }
inline Fixed fxCos(Bam a) { return fxSinCos(a).c; }

// Shortest signed difference a-b, in (-180, +180] degrees. Exact and branch-free in
// binary angles: mask to the circle, then fold the top half negative. The float version
// this replaces needed a loop or an fmod to normalise, and could land either side of pi
// depending on rounding.
constexpr int32_t bamDiff(Bam a, Bam b) {
    int32_t d = (a.v - b.v) & (kBamFullV - 1);
    return d > kBamHalfV ? d - kBamFullV : d;
}

// atan2 as a binary angle, CORDIC in vectoring mode: rotate (x,y) onto the +x axis and
// accumulate the angle it took. Same family as fxSinCos, same table, no float. Argument
// order matches std::atan2.
inline Bam fxAtan2(Fixed y, Fixed x) {
    if (x.v == 0 && y.v == 0) return Bam(0);
    int64_t vx = x.v, vy = y.v;
    int32_t extra = 0;
    // Fold into the right half-plane; vectoring converges only there.
    if (vx < 0) {
        if (vy >= 0) { const int64_t t = vx; vx = vy;  vy = -t; extra =  kBamQuarterV; }
        else         { const int64_t t = vx; vx = -vy; vy =  t; extra = -kBamQuarterV; }
    }
    int32_t z = 0;
    for (int i = 0; i < 16; ++i) {
        const int64_t dx = vx >> i, dy = vy >> i;
        if (vy > 0) { vx += dy; vy -= dx; z += detail::kCordicAtan[i]; }
        else        { vx -= dy; vy += dx; z -= detail::kCordicAtan[i]; }
    }
    return bamWrap(z + extra);
}

// Boundary helpers: unit data, map files and the renderer all still speak radians.
inline Bam bamFromRadians(float r) {
    return bamWrap(roundWrap32(double(r) * (65536.0 / (2.0 * 3.14159265358979323846))));
}
constexpr float radiansFromBam(Bam a) {
    return float(double(a.v) * (2.0 * 3.14159265358979323846 / 65536.0));
}

}  // namespace ta::sim
