# Retail AI comparison harness

Plays whole retail TA:Kingdoms skirmishes headless, records what the retail AI
does, records the same thing from our engine, and compares the two as
distributions. Retail AI openings vary a lot with the RNG, so compare many
seeds, not single games.

Since 2026-10-22 this is **optional tooling, not a gate** for the CLASSIC AI
level: the retail game is no longer run for it. Each ported routine is checked
by an emulation oracle (`check_ai_*.py`) and by our own game tests
(`docs/classic-ai-plan.md` §5); the PrintWeights dumps captured earlier are
still read by `check_ai_printweights.py`.

Nothing here contains code or data from `KINGDOMS.icd`. The scripts read and
poke a running retail process through `/proc/<pid>/mem` and GDB, at addresses
documented in `docs/retail-engine.md` and the other `tools/re` decoders, and
`livesample.verify_code` refuses any executable that is not the v4.0BB
reference. Captures, PrintWeights dumps and screenshots stay under the run
directory (default `~/tak-tmp/retail-ai`). They are never committed.

| Script | What it does |
|---|---|
| `retail_ai_harness.py setup` | Creates the template Proton prefix (one time, about a minute) |
| `retail_ai_harness.py run` | Plays one recorded retail skirmish and tears it down |
| `retail_ai_poll.py` | The JSONL poller the harness uses; it also works on its own against a PID |
| `tak_ai_run.py` | One headless tak `--mpai` game with the debug roster dump on |
| `compare_ai.py` | Metrics and distribution tests over two sets of captures |
| `check_ai_printweights.py` | Checks the CLASSIC brain's pref, limit and priority tables against captured PrintWeights dumps and the polls around them, and emulates retail's 0x410810 on the same tables (no retail run) |
| `check_ai_squad_ctor.py` | Emulates retail's AI manager constructor 0x40fed0 (squads, base quotas, the three wave-size rolls) on random seeds and player tables against the CLASSIC brain's `classicConstructSquads` |
| `check_ai_assignment.py` | Emulates retail's dying-unit cleanup 0x40fe80 and assignment pass 0x40eeb0, with retail's own set-squad, nearest-base, centre and radius code under them, on random players against the CLASSIC brain's `classicDropDying` / `classicAssign` |
| `check_ai_placement.py` | Emulates retail's block-grid draw 0x4110f0 and site search 0x412f80 (nearest free deposit 0x411510, block-grid scatter 0x411200 with its own trig and cell lookup) on random seeds, feature planes and sacred-site tables against the CLASSIC brain's placers (`classicDrawBlockGrids`, `classicPlaceSite`) |
| `check_ai_constructor.py` | Emulates retail's constructor planner 0x40c890, with its centre, radius, counts, firepower, shot cost, menu draw, busy-deposit test and placers running, on random bases, builders and Monarchs against the CLASSIC brain's `classicPlanConstructor` |
| `check_ai_static_grade.py` | Emulates the navigator-grid build 0x4dfee0 on random ratings and footprints and reads back each cell's static bit (what 0x4e1570's mode 1 tests) against the CLASSIC brain's `classicStaticPlane`, which its constructor planner's reachability walks |

## What it needs

- A retail install to copy. The default is `assets/game` (v4.0BB, the build
  every RE address was verified against). Each run reflink-copies it into its
  own directory. The user's own install and Faugus prefixes are never touched.
- `umu-run` (default `~/.local/share/faugus-launcher/umu-run`) and a Proton
  build (default `~/.local/share/Steam/compatibilitytools.d/Proton-CachyOS Latest`).
  Override them with `--umu` and `--proton`. Plain Wine does not work here.
- `kwin_wayland` (with `--virtual --xwayland`), `wmctrl`, ImageMagick `import`
  (for screenshots; `--no-screenshots` skips them), `gdb` (for `--seed` and
  `--aivai`), and `x86_64-w64-mingw32-gcc`. `retail_driver.py` compiles the
  Win32 input bridge `retail_input.c` with it on first use.
- ptrace permission over your own processes, as GDB needs (`kernel.yama.ptrace_scope` 0 or 1).
- Python 3 with no extra packages. `compare_ai.py` does its own KS test.
- For our side: a DEBUG build (`build-o2`, i.e. `-O2 -g` Debug, is the usual
  one). A release `takserver` compiles the roster dump out.

## Retail side

```sh
python3 tools/re/retail_ai_harness.py setup
python3 tools/re/retail_ai_harness.py run --label taros-s1 --seed 1 --minutes 20 \
    --side aramon --enemy taros [--map "Alchemist Glen"] [--aivai] [--printweights 9000]
```

Output goes to `~/tak-tmp/retail-ai/runs/<label>/`: `retail.jsonl` (the
capture), `meta.json` (settings, the selected map, the poke result, seat
factions, why the poll ended), `printweights-p<N>.txt` with `--printweights`,
and the `title`/`lobby`/`end` screenshots, logs and `gdb.log`. The run's game
and prefix copies are deleted at the end unless you pass `--keep-files`.
`--root` (or `TAK_RETAIL_AI_ROOT`) moves everything. A 10-minute game takes
about 2-3 minutes of wall time at the default game speed 0, including about 45 s
of start-up. Runs can go in parallel: each
one has its own display, prefix and game copy. `--cores` (default `4-23`)
pins them.

How a run works:

1. `kwin_wayland --virtual --xwayland` starts an off-screen X display. It never
   touches the desktop.
2. `umu-run` starts `Kingdoms.exe -AutoStop=999999`. Retail's `-AutoStop`
   benchmark switch stops pacing ticks to real time: about 140 ticks/s at game
   speed 0 and 280 at +10, against 30 normally. That flag is latched once at
   start-up. While the switch's cached argument is set, it also exits after N
   wall seconds (hence the huge N) and reseeds both the game RNG and the C
   runtime's `rand` with 1234 at the start of every tick (see step 5).
3. Seat 0 is an idle human and seat 1 a retail AI. Their factions come from the
   registry values `Side` and `EnemySide` under `Skirmish GameOptions`, written
   into the run's `user.reg` before launch, together with `RandomStartLocations=0`,
   the unit cap (`--unit-cap`, default 150, v4.0's own default) and the game speed.
   The harness checks the monarchs' type prefixes after the start and warns if
   they don't match.
4. The input bridge clicks into the skirmish battle room. With `--map` it finds
   the map in the alphabetical list: it scrolls, selects the top row and reads
   the selection back from the map object in game memory, galloping and then
   bisecting over the scroll position. This takes about 30-60 s. Without `--map`
   it plays the lobby default, Abnar's Terrace.
5. GDB attaches in the battle room before Start and sets a hardware watchpoint
   on the tick counter. On the write of tick `--poke-tick` (default 1) it
   clears `-AutoStop`'s cached argument (`0x6411cc`), which stops the per-tick
   reseed and the wall-clock exit but keeps the unpaced ticks. It then writes
   `--seed` (default 1, a Park-Miller state in 1..2^31-2) into the game RNG and,
   for `--aivai`, the two controller bytes that retail's debug `+AI` command
   writes, so seat 0 becomes an AI. Then it detaches. From there the game runs
   one continuous RNG stream from the seed, like a normal game does from its
   start-up seed. `--reseed-every-tick` keeps the per-tick reseed for
   debugging, which is the mode the 2026-10-22 probe ran in: see
   "Reproducibility" below.
6. `retail_ai_poll.py` samples every `--every` ticks (default 30). It spins on
   the tick counter and keeps a read only if the tick did not change during it.
   AI squads are added every `--squads-every` ticks. It stops at `--minutes`,
   when only one seat still owns units, or when the tick stalls for 20 s.
7. `--printweights T`: at tick T it sets the debug-console byte and types
   `+PrintWeights <seat> pw<seat>.txt` for every seat. That gives retail's baked
   limit, preference and priority table per unit type. The game keeps running
   while this is typed, so the dump's own `gametime` header is the real time.
8. Teardown kills only this run's PIDs: every process whose
   `WINEPREFIX`/`STEAM_COMPAT_DATA_PATH` names this run's prefix, GDB, the
   display-holder `sleep` by the PID it wrote to `display-holder.pid` (it is
   not in kwin's process group, and it outlived kwin whenever kwin had already
   exited: before this fix every run left one behind), the kwin it started, and
   last anything still carrying the run's private `XDG_RUNTIME_DIR`.
   `retail_ai_harness_test.py` (ctest `retail_ai_harness_teardown`) checks this
   against a fake kwin, without retail.

`--stop-at-lobby` leaves the game in the battle room so you can inspect it.
Touch `<run>/release` to tear it down.

## Our side

```sh
python3 tools/re/tak_ai_run.py --out ~/tak-tmp/retail-ai/tak/taros-s1.jsonl \
    --map "Abnar's Terrace" --side aramon --enemy taros --seconds 1200 --seed 1 [--level N]
```

This starts a fresh debug `takserver --no-auth --local --seed S` with
`TAK_ROSTER_DUMP=<out>` (and `TAK_ROSTER_EVERY`, default 30), then a headless
debug `takclient ... --mpai` in scratch `HOME`/`XDG_DATA_HOME` directories. It
prints the client's `mp-headless done ... hash=` line. The dump
(`src/server/rosterdump.cpp`) runs after each referee tick, only reads the
World, and exists only in builds without `NDEBUG`. A state hash is the same with
the dump on and off. The first room writes to the given path, and any later
room to `<path>.room<id>`. Squads are not dumped yet: the CLASSIC brain will
add them in the retail layout.

The CLASSIC brain's own weight table, for `--printweights` comparisons: with
`TAK_CLASSIC_WEIGHTS=<path>` (and `TAK_CLASSIC_WEIGHTS_AT=<tick>`, default 600)
in the takserver's environment, each Classic seat writes `<path>.p<seat>` once,
at the first tick at or past T, one row per unit type in PrintWeights' column
layout (`<limit|n/a> - <priority> = <preference> - '<UNIT>\t\t:<name>'`). It is
debug-only as well, and only writes a file. Two differences are expected: our
table holds every type of the data (retail's held only the sides in play, plus
MONGHOUL, in the dumps seen so far), and the cost-penalty draws of the
priorities come from a different random stream, so only the deterministic rows
(and distributions over seeds) compare. For an exact comparison, use
`check_ai_printweights.py`: it runs our brain's own facts, profile and priority
pass over retail's table, in retail's order, on the state and seed polled
around each dump (`--dumps`, default `~/tak-tmp/retail-ai/baseline/printweights`,
read only). On the 20 baseline dumps every row matches and every dump is
reproduced exactly.

## The capture schema (both engines)

```
{"kind":"meta","schema":1,"engine":"retail"|"tak","tick_rate":30,"every":30,...}
{"kind":"sample","tick":T,
 "players":[{"p":0,"controller":"human"|"ai","mana":M,"storage":S},...],
 "units":[[id,player,"type",remaining,x,z],...],
 "squads":[{"p":1,"squads":[{"slot":k,"kind":"base"|"strike"|"backup"|"vtol"|"homeless",
            "deadline":t,"params":[...],"members":[ids]}]}]}      (retail, every --squads-every)
{"kind":"end","tick":T,"reason":"max-ticks"|"one-player-left"|"stalled"|"process-gone"}
```

`remaining` is retail's construction-remaining fraction: 1 when placed, 0 when
finished. `x` and `z` are world pixels. `storage` is the mana capacity. Retail
reuses entity slots after deaths, so the tools key units by (id, type).

## Comparing

```sh
python3 tools/re/compare_ai.py --a ~/tak-tmp/retail-ai/runs/taros-*/retail.jsonl --label-a retail \
    --b ~/tak-tmp/retail-ai/tak/taros-*.jsonl --label-b tak [--by-faction] [--json report.json]
```

Every AI seat in every file is one observation. For each metric it prints the
median and IQR per set, and the two-sample KS D with its p-value. Metrics:
build order (modal sequence, per-position frequencies, edit distances within
and across sets), first-completion timings (1st/2nd/4th/7th lodestone, first
static building, first mobile unit), composition per type and per class at
3/6/10/15/20 minutes, mana per minute and the broke/full shares, lodestones
over time, waves (strike squads gaining members; retail only until our brain
dumps squads), a squad-free "sortie" proxy for both engines (mobile units more
than `--sortie` px from the start), placement distance and angle relative to the
nearest enemy start, and outcome and length. A type counts as mobile if any of
its units moved, and a lodestone is any type whose name ends in `lode`.

Plan per PLAN §5 C: N >= 20 runs with distinct seeds, on Abnar's Terrace and
two more maps, each faction against idle, plus mirror AI-vs-AI. Pass a distinct
`--seed` to every run on both sides.

## Reproducibility (measured 2026-10-22)

- **With the per-tick reseed (`--reseed-every-tick`, i.e. plain `-AutoStop`)**,
  runs repeat decision for decision, but `--seed` has no lasting effect: four
  10-minute Taros-vs-idle runs (speed +10) with seeds 777 (twice), 123456789 and 987654321
  made the identical 27-event build sequence. Every tick starts from the same
  draws, so one map and faction setup is a single sample however many runs you
  make. **Do not use it for distributions.**
- **Normal mode (default)**: different seeds give different games. Same-seed
  runs usually agree only up to the first building. Taros-vs-idle on Abnar's
  Terrace: seed 4242 x8 at speed 0 (four in parallel, two one after the other,
  one pair), seed 4242 x2 at speed +10, and seed 777 x4. Two pairs, one at each
  speed, matched in game RNG and full roster at every sample for 10 minutes.
  The other runs matched until the first building was placed (tick about 150).
  That building then got a different entity slot from run to run (166..296).
  The AI walks units in slot order, so its draws drift from about tick
  180-300. At 3 minutes all 8 seed-4242 speed-0 runs still had the same build
  sequence (lodestone, then a hellhound pit at tick 360-364). Of the four
  seed-777 runs, two had the same sequence and two did not. The slot choice
  does not come from the game RNG, and it is not CPU contention: runs made one
  after another also differed. It follows the C runtime's `rand` stream. In
  `--reseed-every-tick` mode, which also reseeds that stream every tick, two
  runs with seeds 5 and 6 got the same slots and matched at every sample.
  Something consumes that stream a varying number of times per run. A traced
  window of 200 calls showed only tick-path callers on the main thread, so
  what varies is still open (U9). So a seed is not a replay key. It only makes runs differ,
  and comparisons need distributions over many runs.
- **Game speed changes retail's sim.** Same seed at speed 0 and at +10 diverged
  from tick 240. Keep `--game-speed` fixed across a set. The default is 0.

## Known limits

- 1v1 only: seat 0 is idle or `--aivai`, and seat 1 is the AI. More seats would
  need clicks on the battle room's name and side cells.
- A sample can land a few ticks after its target, and each record carries the
  tick it actually read. The tick counter moves at the start of a tick step, so
  a read can overlap that tick's AI and sim. The poller drops reads during which
  the RNG moved, which catches most of these, not all.
- The C runtime's `rand` stream is not seeded. It continues from `-AutoStop`'s
  `srand(1234)` at the poke tick.
- Seat 1's faction comes from `EnemySide`, so `--aivai` mirrors need both sides set.
