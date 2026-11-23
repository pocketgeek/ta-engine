# Single-player vs multiplayer: what's shared, what differs

A fair worry about a lockstep engine: could single-player and multiplayer *do
things differently*? Here is the precise answer for this engine, with the
guardrail that keeps it honest.

Single-player launches a private local `takserver` (`--local --no-auth`) and
plays through exactly the same server-sequenced bundles, fixed 1/30 step and
server-run AI as multiplayer, so there are no latency, substep, ordering,
AI-tick or AI-seed differences between the two. A room's RNG seed is rolled per
room, or pinned with `takserver --seed`. The one remaining split is inside every
game, single-player included: the server referee applies commands directly,
while every client applies them after the wire. The regression test below
guards exactly that.

## The rules are one code path

Every game reaches the world only through the same shared functions:

- `tak::sim::setupMatch` — builds the initial world (`matchsetup.h`)
- `tak::sim::applyCommand` / `applyEvent` — apply every order/event
- `World::tick` — the simulation step

`GameView::apply` is a one-line wrapper over `applyCommand`, and the server
referee calls the same free function. Same code, same registry, same math
(`-ffp-contract=off` on every target, plus the `detmath` shim for cross-build
determinism). So combat, economy, pathfinding, and AI *decisions* cannot
diverge in their rules — there is no separate "single-player sim."

## Why single-player goes through the server

An in-process single-player driver (immediate apply, local AI) would not be
bit-identical to a networked game, for these reasons:

| Difference | Why |
|---|---|
| **Command latency** | Applied in-process, an order lands before the next tick. Networked, it ships to the server and returns in a later bundle — so the same click lands on a later tick. |
| **Substep dt** | An in-process driver would scale `dt` by the game-speed control and substep it; the server path is always exactly one `tick(1/30)` per bundle. |
| **Intra-tick order** | The server `stable_sort`s a tick's commands by player (`src/server/roomtick.cpp`); an in-process driver would apply human orders pre-tick and AI orders post-tick, in controller order. |
| **AI observation tick** | The referee runs the AI while closing a tick; a driver running it after the tick would see the world one activation later. |
| **AI seed** | The AI's draws come from the room's seed; a separate local seed would give a different AI command stream from the first activation. |

Running single-player through a private server removes all five: one clock, one
canonical command order, one AI. Game speed is a server-side pacing control, so
solo play keeps it.

## The load-bearing invariant, and its regression test

What actually keeps every peer in lockstep — and is the honest thing to guard —
is:

> Given the **same** commands and events at the **same** ticks with a fixed 1/30
> step, the sim produces the **same** `World::stateHash` whether the commands are
> applied **directly** (the referee) or transported through the **multiplayer
> wire format** (`Writer` → bytes → `Reader`) first (every client).

`tools/sim_driver_equiv_test.cpp` checks exactly this:

- **Part 1 (ctest `sim_driver_equiv`, no game data, CI-gated):** the wire format
  round-trips commands, events, and a full tick bundle byte-for-byte (a case for
  every `Cmd` kind plus edge-value floats/ids), the server's per-tick command
  sort is stable and player-ordered, and leave events agree between the referee,
  direct and wire paths.
- **Part 2 (ctest `sim_driver_equiv_data`, needs the game data):** builds two
  real worlds and drives an identical schedule — including a same-tick
  two-player command pair and a forfeit event — through the direct path vs the
  wire path, asserting equal `stateHash` **every tick**.

It is deliberately non-tautological: Part 2's second path runs the real
`Writer`/`Reader` code, so a serialization bug or a sim-affecting side effect on
one path is caught. Injecting a bug (a dropped coordinate in `Reader::cmd`)
makes the test fail at tick 0 with every round-trip case mismatching.

**Bottom line:** the *rules* are identical shared code, single-player and
multiplayer share one server-sequenced timing, and the contract that ties the
referee to the clients — apply the same inputs, get the same state — is a
regression test.
