# TA Engine

A clean-room C++20 / SDL2 engine for **Total Annihilation** (Cavedog, 1997), built on
the TAK Engine code base and re-targeted to Total Annihilation's data and rules. Game
behaviour is reverse-engineered from the retail `TotalA.exe` (GOG Commander Pack,
v3.1c) by static analysis and by emulating individual routines (unicorn); the specs
live in `docs/retail-*.md` with the addresses they come from.

> **You need your own copy of Total Annihilation.** No game assets are included. The
> GOG *Total Annihilation: Commander Pack* supplies everything; point the engine at
> its installation folder (the one holding `totala1.hpi` and `TotalA.exe`).

## What works

- **Game data straight from the install**: HPI v1 archives (`*.hpi`, `*.ufo`, `*.ccx`,
  `rev31.gp3`) with retail's precedence, TNT 0x2000 maps with their own tile art, 3DO
  models with TA's palette and shade table, COB scripts, GAF/PCX art, `.GUI` screens and
  `.FNT` fonts.
- **Units and weapons as TotalA.exe parses them** (`docs/retail-data.md`): FBI keys and
  scales, `weapons/*.tdf` by ID and section name, build menus from `guis/<unit>N.gui`
  plus `download/*.tdf`, sides from `gamedata/sidedata.tdf`.
- **Metal and energy** (`docs/retail-economy.md`): once-a-second settle with retail's
  debt stalls, extractors on ore features, metal makers, wind, tidal, reclaim, build
  and repair costs, weapon shot costs.
- **Front end and interface from the retail GUIs** (`docs/retail-gui.md`): MAINMENU,
  SINGLE, SKIRMISH (+ map select), MULTI (SELPROV / TCP); in game the side's build and
  order pages, ORDERS/BUILD tabs, paging, and the top bar's metal/energy readouts.
- **Computer players** (`docs/retail-ai.md`): Total Annihilation's own AI ported from
  TotalA.exe (RETAIL EASY / RETAIL / RETAIL HARD, with retail's 0.5/0.7/1.0 income
  multipliers); the engine's other levels run the same brain on the matching plan.
- **Music by battle status** (`docs/retail-music.md`): retail's 30-second battle monitor
  switches between battle and building tracks from `music/*.mp3`.
- **Two pathfinding modes**: Retail and Legion; deterministic lockstep multiplayer
  through a server, replays, and the headless harness.

Deliberate differences from retail are listed in `docs/ta-port-plan.md`.

## Build

```sh
./tools/build-static-deps.sh       # once: static zlib, libjpeg-turbo, SDL2
./tools/build-ffmpeg-bink.sh       # once: static FFmpeg (Smacker decode, streaming)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Run `./build/taclient --data <your Total Annihilation folder>`. Debug builds
(`-DCMAKE_BUILD_TYPE=Debug`) add the CLI game/harness modes described in `CLAUDE.md`.

## Tools

`hpitool`, `tnttool`, `gaftool`, `cobtool`, `modeltool`, `tdftool`, `registry_dump`
(what the unit registry made of the data) and `guitool` (renders a `.GUI` screen to a
PNG). `tools/re/` holds the reverse-engineering scripts (venv with unicorn, capstone,
pefile).

## License

GPL-3.0 (see `LICENSE`). Bundled third-party code keeps its own licences
(`*-LICENSE*.txt`).
