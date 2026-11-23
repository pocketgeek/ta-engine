# TA Engine port plan

Total Annihilation engine built from the TAK Engine code base (imported without
history). Every retail behaviour is checked against `TotalA.exe` (GOG Commander
Pack 3.1c), statically and by emulating routines with unicorn (`tools/re/`,
venv `~/venvs/ta-re`). The retail specs live in `docs/retail-*.md`.

## Done

- Repo, build, rename (`ta::`, `taclient`, `taserver`, `TA_*`).
- HPI v1 archives (key, LZ77, zlib), TA mount precedence: loose > rev31.gp3 >
  *.ccx > *.ufo > *.hpi, first mount wins (0x41d4c0, 0x4bb320).
- TNT 0x2000 maps (0x483610) load/save, all 275 shipped maps round-trip;
  terrain drawn from the map's own tiles through palette.pal.
- GAF without version check (0x429700); COB v4 and 3DO parse for all shipped files.
- Sides from `gamedata/sidedata.tdf` (ARM, CORE) replace the five Kingdoms factions.
- Map features placed from TNT feature names (`features/<world>/*.tdf`).
- In-game music by battle status (`docs/retail-music.md`, `client/battlemusic.h`).
- Headless server+client game runs in lockstep with ARMCOM vs CORCOM.

## Port steps (all seven landed)

| Step | State |
| --- | --- |
| 1. Units and weapons (`retail-data.md`) | done: 278 units, weapons by section, build menus from guis/download |
| 2. Metal / energy economy (`retail-economy.md`) | done: 1 s settle, debt stalls, extractors on ore, wind/tidal, reclaim (`reclaim_test`) |
| 3. Kingdoms gameplay stripped | done (gods, auras, sacred sites, mana build areas, monarchs); see "Known gaps" |
| 4. GUI runtime, front end, sidebar (`retail-gui.md`) | done: MAINMENU, SINGLE, SKIRMISH, SELMAP, SELPROV/TCP, NEWGAME, MSNBRIEF, ENDMSN, in-game sidebar and top bar |
| 5. AI (`retail-ai.md`) | done: every level runs the retail brain (RETAIL EASY/HARD plus the engine's levels) |
| 6. Pathfinding (`retail-pathfinding.md`) | Retail A* verified 613/613 against TotalA.exe; goals per 0x44cf60/0x44d3b0/0x44dc60; Legion kept |
| 7. Rendering, campaigns, Cartographer, random maps, MP, packaging | done: 3DO textures; TA campaigns (`retail-campaign.md`); Cartographer stamps .sct and saves missions losslessly; random maps from .sct sections; MP failures return to SELPROV; CPack TGZ builds |

## Pathfinding status (docs/retail-pathfinding.md)

Applied (verified 613/613 against TotalA.exe in emulation): TA's fixed A* costs,
no weight ramp, no octant march / visit limit, no node limit; navigator holds 20
points. Pending from the change list: per-player initial heuristic weight (5),
search grade for unexplored / out-of-plane cells (6), 30-tick body grades (7),
60-tick re-path gate (9), scheduler credits (10), underwater speed cap (14), ground
scan (15), cell-entry refusal (16). Goal metrics (11-13) applied.

## Known deviations from retail (deliberate)

- All *.hpi are mounted (retail stops after 10, hiding totala3/4 and worlds.hpi on the GOG layout).
- Battle-music monitor resets each game (retail carries it across games); default
  track table used on GOG installs, where retail plays only building music.

## Known gaps

- Campaign: no save/load (ENDMSN's Load/Save are greyed), narration WAVs and the
  glamour picture / ending movie are not played, ENDMSN's statistics table is empty,
  the briefing's `&X` colour letters beyond Y/R/G/B are unmapped [U].
- Tests still written against Kingdoms data (ai_behavior, classic_ai, placement,
  map_transfer, cartographer_ui, retailgap weapons/rules/orders/fireatwill,
  ai_orders' Crusades roster) compile but fail at runtime; they need TA fixtures.
- Kingdoms storm rendering and per-unit mana fields remain in the client/sim (inert).
