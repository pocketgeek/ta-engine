# Single-player vs multiplayer: what's shared, what differs

A recurring, fair worry about a lockstep engine: could single-player and
multiplayer *do things differently*? Here is the precise answer for this engine,
with the guardrail that keeps it honest.

## The rules are one code path

Single-player (offline, immediate apply, in-process AI) and multiplayer
(server-sequenced bundles, referee AI) both reach the world only through the
same shared functions:

- `ta::sim::setupMatch` — builds the initial world (`matchsetup.h`)
- `ta::sim::applyCommand` / `applyEvent` — apply every order/event
- `World::tick` — the simulation step

`GameView::apply` is a one-line wrapper over `applyCommand` used by *both* paths,
and the server referee calls the same free function. Same code, same registry,
same math (`-ffp-contract=off` on the whole sim library, plus the `detmath` shim
for cross-build determinism). So combat, economy, pathfinding, and AI *decisions*
cannot diverge in their rules — there is no separate "single-player sim."

## What legitimately differs (by design, not bugs)

A **full** single-player game and a **full** multiplayer game are *not*
bit-identical, and shouldn't be:

| Difference | Why |
|---|---|
| **Command latency** | Offline, an order applies immediately (before the next tick). Networked, it ships to the server and returns in a later bundle — so the same click lands on a later tick. |
| **Substep dt** | Single-player scales `dt` by the game-speed control and substeps at ≤ 1/30 (up to 16 per frame); multiplayer is always exactly one `tick(1/30)` per bundle. |
| **Intra-tick order** | The server `stable_sort`s a tick's commands by player; single-player applies human orders pre-tick and AI orders post-tick, in controller order. |
| **AI observation tick** | Single-player runs the AI *after* the tick (first activation sees tick 1); the referee runs it while closing tick 0 — a one-activation offset for player 0. |
| **AI seed** | Single-player seeds `0x1234567`; the server seeds `0x7a6b0000 + room id`. Different draws → a different AI command stream from the first activation. |

These are intentional: multiplayer needs a shared clock and a canonical command
order across peers; single-player wants zero input latency and variable game
speed. (If you ever want single-player and multiplayer to match exactly, the fix
is an in-process *sequencer* that applies commands at tick boundaries with no
network — not running the real server for solo play, which would add latency and
kill speed control.)

## The load-bearing invariant, and its regression test

What actually keeps every multiplayer peer in lockstep — and is the honest thing
to guard — is narrower than "SP == MP":

> Given the **same** commands and events at the **same** ticks with a fixed 1/30
> step, the sim produces the **same** `World::stateHash` whether the commands are
> applied **directly** (single-player) or transported through the **multiplayer
> wire format** (`Writer` → bytes → `Reader`) first.

`tools/sim_driver_equiv_test.cpp` checks exactly this:

- **Part 1 (CI-gated, no game data):** the wire format round-trips commands,
  events, and a full tick bundle byte-for-byte (1040 command cases across every
  `Cmd` kind and edge-value floats/ids), and the server's per-tick command sort
  is stable and player-ordered.
- **Part 2 (local / anyone with the assets):** builds two real worlds and drives
  an identical schedule — including a same-tick two-player command pair and a
  forfeit event — through the direct path vs the wire path, asserting equal
  `stateHash` **every tick**.

It is deliberately non-tautological: Part 2's second path runs the real
`Writer`/`Reader` code, so a serialization bug or a sim-affecting side effect on
one path is caught. Verified by injecting a bug (a dropped coordinate in
`Reader::cmd`): the test fails at tick 0 with 1040/1040 round-trip mismatches;
with the bug removed it passes (150 ticks, direct == wire).

**Bottom line:** the *rules* are identical shared code; the *timing/ordering*
differs between solo and networked play on purpose; and the sim-core contract
that ties them together — apply the same inputs, get the same state — is now a
regression test.
