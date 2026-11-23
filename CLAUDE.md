# CLAUDE.md

Orientation for Claude Code in this repo. `README.md` is the user-facing overview;
`docs/ta-port-plan.md` tracks what is ported and what remains.

## What this is

A C++20 / SDL2 engine for **Total Annihilation** (1997), built on the TAK Engine code
base (imported without history) and re-targeted to TA. Retail behaviour is checked
against `TotalA.exe` (GOG Commander Pack 3.1c): static analysis plus unicorn emulation
of single routines (`tools/re/`, venv `~/venvs/ta-re`). Specs: `docs/retail-*.md`.

## Hard rules

- **Check retail, don't assume.** New gameplay rules cite a `TotalA.exe` address (or a
  `docs/retail-*.md` section). Hex addresses in older comments without `TotalA.exe`
  are KINGDOMS.icd references inherited from TAK Engine: unverified for TA.
- **Never commit game assets or the retail binary.** `assets/` (extracted reference data
  in `assets/tadata`) is gitignored.
- **No /tmp.** Scratch files go in `~/ta-tmp`; export `TMPDIR=~/ta-tmp` for builds/tools.
- **Run Windows binaries with Proton** (`~/.local/share/faugus-launcher/umu-run`,
  `PROTONPATH=.../Proton-GE Latest`), never plain wine, and reap leftover processes.
- **The simulation is deterministic lockstep**: no libm transcendentals in `src/sim`
  (use `detmath`), everything `World::stateHash()` folds must be bit-identical on every
  peer. The TA economy books (`sim/taeconomy.h`) are float32 in a fixed order.

## Build / run / test

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
cmake -B build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build-dbg
```

- After a `src/sim`, `src/net` or `src/ai` change rebuild both `taclient` and
  `taserver` (the shared `ta-formats` lib).
- Data: `--data ~/ta_install` (the GOG install). Reference extraction: `assets/tadata`.
- Headless harness (debug): `XDG_DATA_HOME=$(mktemp -d -p ~/ta-tmp) ./build-dbg/taserver
  --port 7677 --data ~/ta_install --no-auth --local --seed 1 &` then
  `TA_HEADLESS=1 SDL_VIDEODRIVER=dummy ./build-dbg/taclient game "Ashap Plateau" --data
  ~/ta_install --server 127.0.0.1 --serverport 7677 --mpai --time 60` (prints `hash=`).
  `TA_AI_LEVEL=9` (RETAIL HARD) etc., `TA_ECONLOG=1` economy, `TA_AILOG=1` AI decisions,
  `--shot out.png` + `TA_SHOT_SELECT=1` for a screenshot with a unit selected.
- Front end scripted (debug): `TA_FRONT_SCRIPT=SINGLE,Skirmish,Start ./build-dbg/taclient
  --data ~/ta_install` with `SDL_VIDEODRIVER=dummy`.
- Data checks: `registry_dump <install> armcom ...`, `guitool <install> <screen> out.png
  [bitmaps/<backdrop>.pcx]`, `tnttool roundtrip`, `hpitool list`.

## Map of the code

- `src/hpi` HPI v1/v2 + VFS (TA mount precedence), `src/tnt` TNT 0x2000 + OTA,
  `src/gaf` GAF/PCX, `src/tdf` TDF (retail rules: first section wins, values end at `;`).
- `src/sim` deterministic sim: `taweapons` (weapons/*.tdf), `taeconomy` (metal/energy),
  `sides` (sidedata.tdf), TA build menus in `TypeRegistry::loadBuildTree`.
- `src/ai/taretail` TA's own AI (all levels run it), `src/server`, `src/net`.
- `src/gui/tagui` + `src/client/taguiview` the .GUI runtime; `tafrontend` the menus;
  `gameview_tahud` the in-game sidebar/top bar; `battlemusic.h` battle-status music.

## Gotchas

- `vpath::extension()` is lowercased: TA archives mix case (`METAL.TDF`).
- A TA map carries its own tiles: tile index doubles as the compositor's section key.
- Start positions live in the OTA's `[Schema N]\[specials]`, in pixels.
- `Order` and `Unit` have cache-line layout asserts: append new Unit fields at the end;
  keep `Order`'s pad when removing a field.
