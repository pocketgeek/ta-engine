# CLAUDE.md

Orientation for Claude Code working in this repo. `README.md` has the full
build/play/options docs; this file is the quick map plus the rules that are easy
to get wrong.

## What this is

A modern, clean-room C++20 / SDL2 re-implementation of **Total Annihilation**
(Cavedog, 1997 — Commander Pack: base + Core Contingency + Battle Tactics):
asset pipeline, deterministic simulation, an SDL2 renderer, and client-server
multiplayer.

It began as the engine for *TA: Kingdoms* (github.com/pocketgeek/tak-engine, kept
as the `upstream` remote) and is being converted to TA. **Kingdoms is not a
supported target** — where the two games disagree, the Kingdoms behaviour is
removed rather than kept alongside. Anything still carrying Kingdoms' vocabulary
(`mana`, kingdoms/houses, `.tdo`-era assumptions) is a conversion that has not
happened yet, not a feature to preserve. The one thing worth keeping is the
engine's own *additions* over retail — shared radar, the build-menu placement
preferences, replay/spectate, the stats table on the end screen.

Behaviour is reverse-engineered from the retail binary `TotalA.exe` by static
analysis (`pefile` + `capstone`) and by EMULATING individual routines with
Unicorn to check a port against the original. Emulation observes behaviour only —
**the harness lives outside the repo and nothing from the binary is copied into
the engine or its comments.** Findings go in `docs/retail-engine-ta.md`.

## Hard rules

- **Never commit game assets or the retail binary.** The retail install
  (`TotalA.exe`, `*.hpi`, `*.ufo`, `*.ccx`, `*.gp3`, `Maps/`, extracted art) is
  gitignored and normally lives OUTSIDE the repo — this machine keeps it at
  `~/ta_install`. `TotalA.exe` is for **reverse-engineering only**. Never copy its
  code or data into the engine, and never commit any of it.
- **The simulation is deterministic lockstep** — every peer must compute
  byte-identical state:
  - No direct libm transcendentals in `src/sim/`. Use `src/sim/detmath.h`
    (`detmath::sin/cos/atan2/len/...`). `tools/check-detmath.sh` enforces this.
  - `World::stateHash()` is the lockstep checksum; everything it folds in must be
    deterministic. Unit headings use `detmath::atan2`.
  - **Fog of war / visibility (`vis_`) is LOCAL display, not hashed** — the
    headless referee skips `updateVisibility` (`visPlayer_ < 0`). LoS/fog math may
    use plain floats.
  - Verify with `tools/check-determinism.sh` (cross-compiler golden hash, currently
    `ab1ef54ae324bd0e`) and the headless run below.
- **Anything derived from the retail data must be MEASURED, not assumed.** The
  conversion's recurring failure mode is a plausible reading of a field name that
  the data quietly contradicts: `EnergyUse=-20` is a solar panel's *output*,
  `WindGenerator=30` is a cap multiplied by a normalised factor rather than a
  `min`, `TidalGenerator=1` is a flag and not a rating, `.ota` `XPos` is in world
  units and not cells. Check the claim against the shipped files (the `tools/` CLIs
  below) or against the binary, and say in the comment what was measured — and
  where something could NOT be established, say that too rather than implying it
  was.

## Build / run / test

```sh
cmake -B build -G Ninja && cmake --build build      # Release -> ./build/*
```

- **Two build dirs coexist: `build/` (Release) and `build-dbg/` (Debug).** After
  editing code, rebuild **whichever binary you actually run** — a stale build
  silently shows old behaviour (this has caused confusion). Rebuild both if unsure;
  Debug has caught defects Release hid (`-Wformat`, assertions).
- **After a `src/sim`, `src/net`, or `src/ai` change, rebuild ALL targets:**
  `cmake --build build` (no `--target`). Those live in the shared `ta-formats`
  static lib, which is baked into each executable at link time — so `--target
  taclient` alone leaves a **stale `taserver`** (its referee sim then disagrees
  with the freshly-built clients). Building all targets relinks both together.
  **The skirmish AI runs server-side**, so an AI change needs the server rebuilt
  and RESTARTED to have any effect.
- **Release vs debug CLI.** A RELEASE `build/taclient` is hardened: it accepts ONLY
  `--data <dir>` and `--version` (`--help` prints that), reads NO `TA_*` env vars, and
  always launches the front-end menu — every mode keyword (`game`/`map`/`replay`/`model`),
  gameplay/dev/test flag, and env hook is `#ifndef NDEBUG` (see `src/client/dev.h`).
  So all the CLI-driven flows below need the DEBUG `build-dbg/` binaries. The mode
  keyword must be **argv[1]**: `taclient game "<map>" --data ...`, not after the flags.
- Play (debug build): `./build-dbg/taclient game "<map name>" --data ~/ta_install
  [--side arm --aiside core]` — the engine reads a retail install directly (root
  `*.hpi`/`*.ufo`/`*.ccx`/`*.gp3` + `Maps/` + `overrides/`); maps are referenced by
  NAME, resolved via the VFS (e.g. `"Coast To Coast"`).
- Headless determinism / smoke test — DEBUG binaries. The server needs `--data` to
  run the referee + AI and enforces a gameplay-data hash, so client and server must
  point at the same install. **A server requires an account login by default** —
  pass `--no-auth` for a harness run. Wait for its "listening" line before starting
  the client:

  ```sh
  ./build-dbg/taserver --port 7677 --data ~/ta_install --no-auth --seed 1 &
  SDL_VIDEODRIVER=dummy TA_MP_WATCH=1 TA_MP_AIS=2 ./build-dbg/taclient \
      game "Coast To Coast" --data ~/ta_install \
      --server 127.0.0.1 --serverport 7677 --mphost --time 60
  ```

  prints `mp-headless done: tick=... hash=... units=...`. `--mpai` hosts against one
  server-run AI instead; `TA_MP_WATCH` + `TA_MP_AIS=N` makes it an all-AI game the
  host only watches.
- **Under `--mpai` the `p0-*` figures are the IDLE HUMAN slot**, not the AI: p0 is
  the headless client, which builds nothing by design, so `p0 mix: armcomx1` and a
  flat `p0-metal` are the expected result and not a regression. Only the total
  `units=` reflects AI behaviour there. Use `TA_MP_WATCH=1 TA_MP_AIS=N` when you
  want p0 itself to be an AI. Run ONE harness at a time, too — two runs sharing a
  port kill each other's server and end `err=peer closed` mid-game.
- **`--seed` is REQUIRED for any comparison.** Without it the server rolls a fresh
  seed per game, and two runs of the SAME binary give different hashes and
  different `units=`. That count is noisy across seeds by more than a factor of two
  (Coast To Coast, 2 AI, 120 s: 12 / 11 / 9 for seeds 11 / 22 / 33), so a
  before/after figure from a single unseeded run measures the seed, not the change.
  See `docs/ta-port.md` §5a.
- `ctest` in either build dir runs the suite (28 tests). The data-backed
  `unitdata_test` needs an install: `./build-dbg/unitdata_test ~/ta_install`.
- Debug-only diagnostics for the failure modes that are otherwise SILENT:
  `TA_TERRAIN=1` (what a loaded map yielded: walkable fraction, water, slope),
  `TA_PLACE=1` (which test refused a building site), `TA_AI_PICK=1` (why a producer
  chose nothing — set it on the SERVER, which is where the AI runs),
  `TA_LOBBY=1` (the create/seat/start handshake — set it on BOTH the client and
  the server; a campaign launch that lands in a plain skirmish shows up here as a
  room reporting `mission=''`), and `TA_FEATART=1` (why a feature drew nothing).
  `TA_FEATART=audit` goes further: it primes EVERY feature def's art at map load
  instead of only the ones the map placed, and names the ones that fail. A map
  exercises a few dozen defs of 1644, so a clean load proves little on its own —
  the audit is what surfaced the HPI precedence bug (`docs/ta-port.md` §2).
- Asset-inspection CLIs (in `tools/`, built into `build/`): `hpitool`, `tnttool`,
  `gaftool`, `tdftool`, `cobtool`, `modeltool`, plus the `cartographer` map editor.
  Use them to verify claims about the shipped data instead of guessing.

## Architecture

- `src/sim/` — deterministic sim (movement, A* nav, combat, economy). The
  authority for gameplay state; guard determinism carefully here.
- `src/client/main.cpp` — the SDL2 app (`taclient`): rendering + input, and the
  **COB animation VM runs here**, so animation never affects the sim hash.
- `src/net/` + `src/server/` — client-server MP (lockstep relay, referee,
  server-run AI). `src/ai/` — the skirmish AI (emits commands).
- Asset loaders: `src/{hpi,tnt,sct,tdo,cob,gaf,tdf,fnt,gui,terrain,crt}`. Full
  table in the README; deeper notes in `docs/` — start with `ta-port.md` (the
  conversion roadmap and what is done) and `retail-engine-ta.md` (RE findings).

## Recurring gotchas

- **"Is it a building?"** use `isStructure(type) = type->maxVel <= 0` — the FBI
  `canmove` flag is unreliable.
- **A TA map carries its own art.** A `.tnt` (version `0x2000`) embeds a tile
  library and indexes it per 32px tile, so a tile INDEX means nothing outside the
  map that owns it — copying one between maps (the editor's stamp brush) must
  intern by content. Kingdoms' shared, content-addressed `0x4000` sections are
  gone; `.sct` prefabs in `worlds.hpi` are a near-relative of the `.tnt`, decoded
  in `src/sct/`.
- **Terrain is a FLAT tile mosaic**: cliff/height relief is baked into the tile
  art, and mobile units are *lifted on screen* to sit on it (`terrainLift` /
  `terrainLiftX`; buildings exempt). Any screen↔world interaction (selection,
  placement, fog) must be height-aware — use `pickWorld` (screen→world, inverts
  the lift) and `unitScreen` (world→screen, includes flyer altitude). Don't
  hand-roll a flat `offX + sx/zoom`.
- **Models** (3DO v1) are authored front=−z / right=−x (a mirrored basis):
  `scriptRot` negates piece X and Y, and all movers (flyers included) face
  `−heading`. See `docs/model-rendering-plan.md`.
- **TA's data is not consistent about CASE.** The same install holds
  `features/archi/METAL.TDF` next to `features/all worlds/DragonsTeeth.tdf`. Test
  extensions with `ta::iendsWith` (`src/util/strcase.h`), never
  `path.extension() == ".tdf"` — that mistake loads zero features and reports no
  error.
- **`gh` defaults to the wrong remote here.** `origin` is `pocketgeek/ta-engine`;
  `upstream` is the Kingdoms repo, and bare `gh run list` reports THAT one. Pass
  `-R pocketgeek/ta-engine`.
- Match the surrounding code's style, naming, and comment density. C++20.
- Put temporary/scratch files in the system temp dir, never in the repo.
- **Run long or blocking commands in the background** (`run_in_background=true`):
  dev servers, watch tasks, builds, test suites, the headless harness. Prefer
  backgrounding over waiting whenever the command does not have to finish before
  the next step.
