# Cross-build determinism: the `detmath` shim — scoping

**Status: IMPLEMENTED** (commit `ae54375`). `src/sim/detmath.{h,cpp}` ships the
`sin`/`cos`/`atan2`/`len` described below; all ~10 sim call sites route through
it; `tools/detmath_test` checks accuracy + emits a golden hash, and
`tools/check-detmath.sh` guards against regressions. Verified: accuracy vs libm
sin/cos ≤ 3e-8, atan2 ≤ 1.25e-7; golden hash identical across g++
`-O0`/`-O2`/`-O3`; sim stays bit-identical (`--mpai` 1800 ticks × 2, 1v1).
Resolved open questions: (1) `double` intermediates rounded once; (2) `sqrt`/
`fmod` left as `std::` (already deterministic) — not wrapped; (3) clang and
x86-vs-ARM CI legs left for when a target needs them. The rest of this doc is
the original scoping, kept for the rationale.

This was the last open item of the multiplayer plan (see
`docs/multiplayer-design.md`). Everything else — lobby, referee sim, server AI,
reconnect/forfeit, replays, spectators, chat, scoreboard, allied economy — is
done and verified.

**Goal:** make the simulation produce **bit-identical** state on peers built with
*different* compilers, compiler versions, C libraries, or CPU architectures — not
just the same binary. That is what lets two players on, say, Linux/gcc and
macOS/clang (or x86 and ARM) share one lockstep game.

---

## 1. What already holds, and why

Same-binary determinism is in place and heavily tested (headless 2-client runs
are bit-identical over hundreds of ticks; the referee bit-matches clients). The
contract today:

- **No RNG in the sim.** All variation comes from ordered state.
- **Fixed timestep** (1/30 s), fixed iteration orders, `mixf` hashes the *exact*
  float bits (`src/sim/sim.cpp` `stateHash`).
- **`-ffp-contract=off`** on `tak-formats` (which holds `sim.cpp`,
  `matchsetup.cpp`, `ai.cpp`), so the compiler never fuses `a*b+c` into an FMA
  that rounds differently on different CPUs. **No `-ffast-math`.**

Why that is *not yet* enough for cross-build play:

> IEEE-754 requires the basic operations `+ - * /` **and `sqrt`** to be
> *correctly rounded* — every conforming platform returns the same bits for the
> same inputs and rounding mode. `fmod` is exact. So those are already
> deterministic across builds (given no contraction and the default
> round-to-nearest-even mode, which we never change).
>
> **The transcendental functions are not.** `sin`, `cos`, `atan2`, `pow`, `exp`,
> `log`, `hypot` are *not* required to be correctly rounded; each libm / compiler
> builtin picks its own last-bit result. Two builds calling `std::sin` on the
> same angle can differ by 1 ULP — and because `mixf` hashes exact bits, and a
> unit that is 1 ULP off this tick is *more* off next tick, that difference
> compounds and desyncs the game. Quantizing the hash would only *hide* the first
> tick's divergence, not stop it compounding — so it is not a real fix.

**Therefore the shim's job is narrow: replace the non-correctly-rounded
transcendentals used by the sim with portable, fixed-algorithm implementations
that yield identical bits everywhere.** Basic ops, `sqrt`, and `fmod` stay as-is.

---

## 2. The divergence surface (complete inventory)

Grepped across the entire deterministic path (`sim.cpp`, `matchsetup.cpp`,
`ai.cpp`); confirmed nothing hides in `terrain.cpp`, `tnt.cpp`, `lockstep.cpp`,
or the COB VM, and that **COB/piece animation state is not in `stateHash`**
(animation is cosmetic and out of scope).

**This table is the original audit, not a live index.** It records which call
sites drove the shim decision at the time it was taken; the line numbers have
drifted since and the "flow field" entry names a system that no longer exists
(steering now aims straight at the front order's point, and routing is the
boundary tracer in `sim/pathsearch.h`). The verdicts still hold, and the live
check is `tools/check-detmath.sh`, which fails the build on any direct libm
transcendental in `src/sim/` — that, not this table, is what keeps the
inventory complete.

| Function | Sites | Where / what | Verdict |
|---|---|---|---|
| `sin` / `cos` | 6 | `sim.cpp:1744-45` & `:1809` (heading→move vector, stuck-nudge), `matchsetup.cpp:224` (start-ring fallback), `ai.cpp:125,152` (build-placement probes) | **SHIM** |
| `atan2` | 3 | `sim.cpp:1133` (face target), `:1711,:1718` (steer toward waypoint / flow field) | **SHIM** |
| `hypot` | 1 | `sim.cpp:1798` (stuck-watchdog travel distance) | **rewrite** as `sqrt(dx*dx+dz*dz)` |
| `sqrt` | 8 | distances / magnitudes throughout | keep (correctly rounded) |
| `fmod` | 1 | `sim.cpp:35` `angleDiff` (angle wrap) | keep (exact) |
| `abs` | many | sign only | keep (exact) |

Every trig call reduces to one of two shapes:

- **heading → unit vector:** `(sin θ, cos θ)` — movement, ring/AI placement.
- **vector → heading:** `atan2(dx, dz)` — desired facing.

So the shim needs exactly **three** functions — `sin`, `cos`, `atan2` — plus a
one-line `hypot` rewrite. That is the whole job.

---

## 3. Options considered

**A. Full fixed-point sim.** Rewrite positions/velocities/angles/stats as
integers. Maximally robust, but touches every sim field, every FBI-loaded float
stat, and the hash — an enormous, high-risk rewrite for a surface this small.
Rejected.

**B. Deterministic transcendental shim (float in/out).** Keep IEEE float storage
and the already-deterministic basic ops; replace *only* `sin`/`cos`/`atan2` with
fixed-algorithm code built from correctly-rounded primitives. Tiny, isolated,
low-risk; the standard approach for lockstep RTS engines. **Recommended.**

**C. Soft-float everything.** Software IEEE for all ops. Robust but a large perf
hit and pointless — the basic ops are already deterministic. Rejected.

---

## 4. Recommended design — `src/sim/detmath.{h,cpp}`

```cpp
namespace tak::detmath {
    float sin(float x);              // fixed range-reduction + fixed polynomial
    float cos(float x);              // = sin(x + pi/2), shared core
    float atan2(float y, float x);   // quadrant dispatch + fixed atan polynomial
    inline float len(float a, float b) { return std::sqrt(a*a + b*b); } // hypot repl.
}
```

**Determinism argument.** If every function is a *fixed sequence of correctly-
rounded operations* (`+ - * /`, optionally `sqrt`) in a *fixed evaluation order*,
with intermediates in a fixed IEEE type, then by the IEEE correctness guarantee
each op yields identical bits on every conforming platform — so the whole
function does too. The rules that make this hold:

- Build **only** from `+ - * /` (and `sqrt`); **never** call the platform
  `std::sin`/`cos`/`atan`. That is the entire point.
- Pin intermediate precision. Compute in `double` and round once to `float` at
  return (x86-64 SSE and ARM NEON both keep `double` in 64-bit registers — no
  x87 80-bit extended precision to leak). Never rely on extended precision.
- Keep `-ffp-contract=off` and round-to-nearest-even (already true; we never
  touch the rounding mode). The detmath TU lives in `tak-formats`, so it inherits
  the flag automatically.
- Write Horner evaluation with explicit parenthesization so the op order is fixed
  in source, not left to the optimizer.

**Implementation sketch.**

- `sin`: reduce `x` to `[-pi, pi]` with the existing exact wrap (`fmod`-based,
  already deterministic), fold to `[-pi/2, pi/2]` by symmetry, then a minimax
  polynomial (≈ degree 7, odd terms) evaluated by fixed Horner. `cos(x) =
  sin(x + pi/2)` through the same reduction.
- `atan2`: dispatch on the signs/magnitudes of `y,x` into the eight octants,
  reduce to `atan(t)` on `t ∈ [0,1]` (using `atan(1/t) = pi/2 − atan(t)`), then a
  minimax polynomial; reassemble with the exact quadrant offsets (`±pi`, `±pi/2`)
  and handle the `x=y=0` and infinity edge cases explicitly.
- Table-plus-polynomial is an option if a pure polynomial can't hit the accuracy
  target cheaply, but a fixed table of exact float constants is equally portable —
  the constraint is "fixed constants + correctly-rounded ops," not "no table."

Accuracy target: within ~1e-5 of `std::sin`/`atan2` so unit motion, facing, and
AI placement are visually and behaviorally unchanged. (The sim already turns
units by `clamp(angleDiff, ±maxTurn)` each tick, so it is not sensitive to the
last few bits — it just needs *everyone to agree* on those bits.)

---

## 5. Migration plan

1. Land `detmath.{h,cpp}` with the three functions + `len`.
2. Add a compile-time guard so the mistake can't recur: inside the sim TUs, make
   bare `std::sin`/`cos`/`atan2`/`hypot`/`pow`/`exp`/`log` a **hard error**
   (a poison macro in a sim-only header, or a small CI grep-lint). New sim code
   then *must* route through `detmath`.
3. Swap the ~10 call sites (§2). Rewrite the one `hypot` as `detmath::len`.
4. Re-baseline: the same-binary lockstep hashes **change once** (detmath sin ≠
   libm sin bit-for-bit). Record the new golden hashes; confirm two runs still
   match (determinism intact) and unit counts / a screenshot look unchanged
   (gameplay intact).

`sqrt` and `fmod` may optionally be routed through `detmath` too (thin wrappers)
so "no bare `std::<math>` in the sim" is a single clean rule — cosmetic, decide
at implementation time.

---

## 6. Verification

- **Golden-vector unit test.** Evaluate `detmath::{sin,cos,atan2}` over a fixed
  input set; assert exact hex bit patterns checked into the test. Runs in CI on
  every platform/compiler — any drift fails loudly. This is the core guarantee.
- **Accuracy test.** Assert `|detmath − std|` under tolerance across the domain,
  so the shim stays gameplay-equivalent.
- **Cross-build lockstep test (the real proof).** Build twice with different
  toolchains — cheap first pass: gcc vs clang and/or `-O0` vs `-O3` on one
  machine; ultimate pass: x86 vs ARM — run the *same* headless MP game and assert
  identical final hash. Without the shim this diverges; with it, it must match.
- **Regression.** Existing same-binary headless MP hash comparisons still pass
  (against the re-baselined goldens), 0 desyncs.

---

## 7. Effort, risk, open questions

- **Effort:** ~one focused session. `detmath.{h,cpp}` ≈ 150–250 lines, ~10 call
  swaps, tests, one hash re-baseline.
- **Risk:** low–medium. Surface is tiny and isolated. Real risks:
  - Polynomial accuracy low enough to *visibly* shift AI/movement → mitigated by
    the accuracy test + a before/after screenshot and unit-count check.
  - A transcendental we missed slips into the sim later → mitigated by the poison
    guard / CI lint (step 2).
- **Open questions to settle at implementation time:**
  1. `double` intermediates rounded once, vs pure `float` throughout — pick one
     and state it. (Leaning `double`-then-round: easier accuracy, still portable.)
  2. Route `sqrt`/`fmod` through `detmath` for a single audited surface, or leave
     them as `std::`? (Leaning: wrap them, for the one-rule lint.)
  3. Do we want the ultimate x86-vs-ARM CI leg now, or ship with the gcc-vs-clang
     leg and add ARM when a target actually needs it? (Cross-arch is the true
     test but needs a second runner.)
