# Cross-build determinism: the `detmath` shim

`src/sim/detmath.{h,cpp}` provides `sin`, `cos`, `atan2` and `len` (plus a
double-precision `atan`, used for retail ballistic angles); every sim and AI
call site routes through it. `tools/detmath_test` (ctest `detmath`) checks
accuracy and emits a golden hash, and `tools/check-detmath.sh` guards against a
direct libm call creeping back in. Measured accuracy vs libm: sin/cos ≤ 3e-8,
atan2 ≤ 1.25e-7 (the test's tolerance is 1e-5).

**Goal:** the simulation produces **bit-identical** state on peers built with
*different* compilers, compiler versions, C libraries, or CPU architectures — not
just the same binary. That is what lets two players on, say, Linux/gcc and
macOS/clang (or x86 and ARM) share one lockstep game. See
[multiplayer-design.md](multiplayer-design.md) for the lockstep contract this
serves.

---

## 1. What the contract rests on

- **No unseeded randomness in the sim.** The sim uses retail's seeded game RNG
  (`sim/retailrng.h`, `World::setGameSeed`), whose seed every peer and replay
  receives, so its draws are ordered state like everything else.
- **Fixed timestep** (1/30 s), fixed iteration orders, `mixf` hashes the *exact*
  float bits (`src/sim/sim.cpp` `stateHash`).
- **`-ffp-contract=off`** on EVERY target (directory-wide in CMakeLists.txt), so
  the compiler never fuses `a*b+c` into an FMA that rounds differently on
  different CPUs. **No `-ffast-math`.** Setting it per library is not enough: it
  would miss `src/ai/ai.cpp` (a `takserver` source, not part of the library), and
  Apple Clang on arm64 contracts by default, so a Mac-hosted server's AI would
  issue a door-clearing Move one ulp off (`factory.z + sin(a)*r`) and the game
  would diverge from the same game hosted on Linux/Windows. `fp_contract_test`
  (ctest `fp_contract`) guards it.
- **Transcendentals through `detmath`.** This is the shim's job:

> IEEE-754 requires the basic operations `+ - * /` **and `sqrt`** to be
> *correctly rounded* — every conforming platform returns the same bits for the
> same inputs and rounding mode. `fmod` is exact. So those are deterministic
> across builds (given no contraction and the default round-to-nearest-even
> mode, which the engine never changes).
>
> **The transcendental functions are not.** `sin`, `cos`, `atan2`, `pow`, `exp`,
> `log`, `hypot` are *not* required to be correctly rounded; each libm / compiler
> builtin picks its own last-bit result. Two builds calling `std::sin` on the
> same angle can differ by 1 ULP — and because `mixf` hashes exact bits, and a
> unit that is 1 ULP off this tick is *more* off next tick, that difference
> compounds and desyncs the game. Quantizing the hash would only *hide* the first
> tick's divergence, not stop it compounding — so it is not a real fix.

**So the shim's job is narrow: replace the non-correctly-rounded
transcendentals used by the sim with portable, fixed-algorithm implementations
that yield identical bits everywhere.** Basic ops, `sqrt`, and `fmod` stay as
`std::`.

Unit COB threads and piece motion run in `World` and are hashed; they use only
the shim too.

---

## 2. The divergence surface

Every trig call in the sim reduces to one of two shapes:

- **heading → unit vector:** `(sin θ, cos θ)` — movement, ring/AI placement.
- **vector → heading:** `atan2(dx, dz)` — desired facing (unit headings use
  `detmath::atan2`).

Distances use `detmath::len(a, b)` = `sqrt(a*a + b*b)` instead of libm `hypot`.
`sqrt`, `fmod` and `abs` stay as `std::` (correctly rounded / exact).

The live inventory is `tools/check-detmath.sh`: it fails (in CI, via
`tools/check-determinism.sh`) on any direct `sin`/`cos`/`tan`/`atan2`/`atan`/
`asin`/`acos`/`hypot`/`pow`/`exp`/`log` call in the sim and AI sources it lists
(`retailaim.h`, `sim.{cpp,h}`, `matchsetup.{cpp,h}`, `ai.{cpp,h}` and the
classic-AI files). Comments are stripped before matching, so a comment that
describes a formula does not trip it.

---

## 3. Why a shim

**A. Full fixed-point sim.** Rewrite positions/velocities/angles/stats as
integers. Maximally robust, but touches every sim field, every FBI-loaded float
stat, and the hash — an enormous rewrite for a surface this small. Not chosen
(the sim does use fixed-point and tick integers where retail does; the shim
covers the floats that remain).

**B. Deterministic transcendental shim (float in/out).** Keep IEEE float storage
and the already-deterministic basic ops; replace *only* the transcendentals with
fixed-algorithm code built from correctly-rounded primitives. Tiny, isolated;
the standard approach for lockstep RTS engines. **This is what the engine
does.**

**C. Soft-float everything.** Software IEEE for all ops. Robust but a large perf
hit and pointless — the basic ops are already deterministic. Not chosen.

---

## 4. Design — `src/sim/detmath.{h,cpp}`

```cpp
namespace tak::detmath {
    float sin(float x);              // fixed range-reduction + fixed polynomial
    float cos(float x);              // shared core with sin
    double atan(double x);           // double-precision, retail ballistic BAM angles
    float atan2(float y, float x);   // quadrant dispatch + fixed atan polynomial
    inline float len(float a, float b) { return std::sqrt(a*a + b*b); } // hypot repl.
}
```

**Determinism argument.** Every function is a *fixed sequence of correctly-
rounded operations* (`+ - * /`, optionally `sqrt`) in a *fixed evaluation order*,
with intermediates in a fixed IEEE type, so by the IEEE correctness guarantee
each op yields identical bits on every conforming platform — and so does the
whole function. The rules that make this hold:

- Build **only** from `+ - * /` (and `sqrt`); **never** call the platform
  `std::sin`/`cos`/`atan`.
- Pin intermediate precision: compute in `double` and round once to `float` at
  return (x86-64 SSE and ARM NEON both keep `double` in 64-bit registers — no
  x87 80-bit extended precision to leak). Never rely on extended precision.
- Keep `-ffp-contract=off` and round-to-nearest-even. The detmath TU lives in
  `tak-formats`, so it inherits the flag.
- Horner evaluation with explicit parenthesization, so the op order is fixed in
  source, not left to the optimizer.

**Implementation.**

- `sin`: reduce `x` to `[-pi, pi]` with an exact wrap, fold to `[-pi/2, pi/2]` by
  symmetry, then a minimax polynomial (odd terms) evaluated by fixed Horner.
  `cos` goes through the same reduction.
- `atan2`: dispatch on the signs/magnitudes of `y,x` into the eight octants,
  reduce to `atan(t)` on `t ∈ [0,1]` (using `atan(1/t) = pi/2 − atan(t)`), then a
  minimax polynomial; reassemble with the exact quadrant offsets (`±pi`, `±pi/2`)
  and handle the `x=y=0` and infinity edge cases explicitly.
- A fixed table of exact float constants would be equally portable — the
  constraint is "fixed constants + correctly-rounded ops," not "no table."

Accuracy target: well under 1e-5 of `std::sin`/`atan2`, so unit motion, facing,
and AI placement behave like libm. (The sim turns units by `clamp(shortest
angular delta, ±maxTurn)` each tick, so it is not sensitive to the last few bits
— it just needs *everyone to agree* on those bits.)

---

## 5. Verification

- **Golden hash.** `detmath_test` evaluates `detmath::{sin,cos,atan,atan2}` over
  a fixed input set and prints one golden hash of the exact bits.
  `tools/check-determinism.sh` (run by the `determinism.yml` workflow) builds the
  shim with every available compiler × optimization level and asserts they all
  print the same golden; the Windows and Apple-silicon macOS CI jobs check the
  same golden (`dcef618cd2e4d558`) natively.
- **Accuracy.** `detmath_test` asserts `|detmath − std|` under its tolerance
  across the domain, so the shim stays gameplay-equivalent.
- **Lint.** `tools/check-detmath.sh` (above).
- **Lockstep.** Same-binary headless MP runs (`--mpai`, see CLAUDE.md) produce a
  reproducible `hash=`, and debug and release sims are bit-identical.
