# CLAUDE.md

Orientation for Claude Code working in this repo. `README.md` has the full
build/play/options docs; this file is the quick map plus the rules that are easy
to get wrong.

## What this is

A modern, clean-room C++20 / SDL2 re-implementation of **Total Annihilation:
Kingdoms** (Cavedog, 1999): asset pipeline, deterministic simulation, an SDL2
renderer, and client-server multiplayer. Behaviour is reverse-engineered from
the retail binary `KINGDOMS.icd` by static analysis, and since 2026-09-12 also
by EMULATING individual routines to check a port against the original (see
`docs/retail-engine.md`). Emulation observes behaviour only -- the harness lives
outside the repo and nothing from the binary is copied into the engine.

## Hard rules

- **Never commit game assets or the retail binary.** `assets/` (which holds
  `KINGDOMS.icd`, `ironplague.icd`, and the extracted art) and root `*.hpi/*.HPI`
  are gitignored — keep it that way. `KINGDOMS.icd` is for **reverse-engineering
  only** — static analysis, and emulating routines to compare behaviour against a
  port. Never copy its code/data into the engine, and never commit any of it.
- **The simulation is deterministic lockstep** — every peer must compute
  byte-identical state:
  - No direct libm transcendentals in `src/sim/`. Use `src/sim/detmath.h`
    (`detmath::sin/cos/atan2/len/...`). `tools/check-detmath.sh` enforces this.
  - `World::stateHash()` is the lockstep checksum; everything it folds in must be
    deterministic. Unit headings use `detmath::atan2`.
  - **Fog of war / visibility (`vis_`) is LOCAL display, not hashed** — the
    headless referee skips `updateVisibility` (`visPlayer_ < 0`). LoS/fog math may
    use plain floats.
  - Verify with `tools/check-determinism.sh` (cross-compiler golden hash) and the
    headless `--mpai` run below (its `hash=...` must be reproducible run-to-run;
    it only *changes* when you deliberately alter hashed sim state).

## Build / run / test

```sh
cmake -B build -G Ninja && cmake --build build      # Release -> ./build/*
```

- **Two build dirs coexist: `build/` (Release) and `build-dbg/` (Debug).** After
  editing code, rebuild **whichever binary you actually run** — a stale build
  silently shows old behaviour (this has caused confusion). Rebuild both if unsure.
- **After a `src/sim`, `src/net`, or `src/ai` change, rebuild ALL targets:**
  `cmake --build build` (no `--target`). Those live in the shared `ta-formats`
  static lib, which is baked into each executable at link time — so `--target
  taclient` alone leaves a **stale `taserver`** (its referee sim then disagrees
  with the freshly-built clients and trips the referee-suspect check). Building
  all targets relinks `taclient` AND `taserver` together.
- **Release vs debug CLI.** A RELEASE `build/taclient` is hardened: it accepts ONLY
  `--data <dir>` and `--version` (`--help` prints that), reads NO `TA_*` env vars, and
  always launches the front-end menu — every mode keyword (`game`/`map`/`replay`/`model`),
  gameplay/dev/test flag, and env hook is `#ifndef NDEBUG` (see `src/client/dev.h` for the
  env wrapper). So all the CLI-driven flows below need the DEBUG `build-dbg/` binaries.
- Play (debug build): `./build-dbg/taclient game "<map name>" --data <retail-install-dir> [--side X --aiside Y]`
  — the engine reads a retail install directly (root `*.hpi` + `Maps/` + `Music/`
  + `overrides/`); maps are referenced by NAME, resolved via the VFS. `--overrides
  none|cosmetic|full` picks which of `overrides/` are mounted. README lists all options.
  (A release `build/taclient --data <dir>` plays the same games through the menu.)
- Headless determinism / smoke test — DEBUG binaries (the harness flags + `TA_HEADLESS`
  are debug-only). The server needs `--data` to run the referee + AI, and enforces a
  gameplay-data hash, so client and server must point at the same install. **A server
  now requires an account login by default** — pass `--no-auth` for a harness run (or
  give the client `--user NAME --pass PASSWORD`; an unused name registers itself). The
  server takes a couple seconds to mount + load, so wait for its "listening" line
  before the client:
  `./build-dbg/taserver --port 7677 --data <install> --no-auth --seed 1 &` then
  `TA_HEADLESS=1 SDL_VIDEODRIVER=dummy ./build-dbg/taclient game "<map>" --data <install> --server 127.0.0.1 --serverport 7677 --mpai --time 60` —
  prints a state `hash=`. **`--seed` is required for a repeatable hash**: a game is
  otherwise seeded randomly (so Random Start Locations differ game to game), and the
  harness exists to produce the same hash twice. (Debug and release sims are
  bit-identical, so the hash matches a release run.)
- Asset-inspection CLIs (in `tools/`, built into `build/`): `cobtool`,
  `modeltool`, `gaftool`, `tnttool`, `tdftool`, `hpitool`. Handy for verifying
  claims about the shipped data instead of guessing.

## Architecture

- `src/sim/` — deterministic sim (movement, A* nav, combat, economy). The
  authority for gameplay state; guard determinism carefully here.
- `src/client/main.cpp` — the SDL2 app (`taclient`): rendering + input, and the
  **COB animation VM runs here**, so animation never affects the sim hash.
- `src/net/` + `src/server/` — client-server MP (lockstep relay, referee,
  server-run AI). `src/ai/` — the skirmish AI (emits commands).
- Asset loaders: `src/{hpi,tnt,tdo,cob,gaf,tdf,terrain,crt}`. Full table in the
  README; deeper notes in `docs/` (`retail-engine.md`, `detmath-scope.md`,
  `multiplayer-design.md`, `model-rendering-plan.md`, `sp-mp-equivalence.md`,
  `height-and-fog.md`).

## Recurring gotchas

- **"Is it a building?"** use `isStructure(type) = type->maxVel <= 0` — the FBI
  `canmove` flag is unreliable (the Keep sets `canmove=1` with no velocity).
- **Terrain is a FLAT tile mosaic**: cliff/height relief is baked into the tile
  art, and mobile units are *lifted on screen* to sit on it (`terrainLift` /
  `terrainLiftX`; buildings exempt). Any screen↔world interaction (selection,
  placement, fog) must be height-aware — use `pickWorld` (screen→world, inverts
  the lift) and `unitScreen` (world→screen, includes flyer altitude). Don't
  hand-roll a flat `offX + sx/zoom`.
- **Models** are authored front=−z / right=−x (a mirrored basis): `scriptRot`
  negates piece X and Y, and all movers (flyers included) face `−heading`. See
  `docs/model-rendering-plan.md`.
- Match the surrounding code's style, naming, and comment density. C++20.
- Put temporary/scratch files in the system temp dir, never in the repo.
- **Run long or blocking commands in the background** (`run_in_background=true`):
  dev servers, watch tasks, builds, test suites, the headless `--mpai` harness.
  Prefer backgrounding over waiting whenever the command does not have to finish
  before the next step.
