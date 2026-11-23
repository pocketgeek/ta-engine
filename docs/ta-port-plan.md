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

## Retail specs written (implementation pending)

| Spec | Drives |
| --- | --- |
| `retail-data.md` | FBI / weapon / feature / moveinfo / sound / build-menu parsing |
| `retail-economy.md` | metal + energy: 1 s settle, debt stalls, extractors, wind/tidal, reclaim, sharing |
| `retail-ai.md` | RETAIL AI level: profile weights, build pick, nine squads |
| `retail-gui.md` | .GUI gadget runtime, front-end screen graph, in-game sidebar |
| `retail-music.md` | done |

## Next, in order

1. **Unit + weapon loading** per `retail-data.md` (TDF syntax rules, FBI keys and
   scales, weapons by section name with 256 IDs, MovementClass override, yardmap
   rules, Copyright/Version gate, download/*.tdf + guis/<unit>N.gui build menus).
2. **Economy**: replace mana (Player mana, lodestones, sacred sites, monarch
   storage) with the metal/energy settle of `retail-economy.md`; HUD bars from
   sidedata ENERGYBAR/METALBAR.
3. **Strip Kingdoms gameplay**: gods, monarchs-as-concept, mana deposits,
   auras, nimbus, petrify/freeze, crusades balance, conjure/sacred yard,
   KINGDOMS.icd address comments (re-verify or mark "TAK-derived, unverified").
4. **GUI runtime** (`retail-gui.md` P0..P4): .GUI loader, gadget art lookup,
   .FNT fonts, then main menu -> single player -> skirmish -> game, in-game
   sidebar (<SIDE>MAIN2/GEN, build pages), options incl. MUSICRT.
5. **AI**: RETAIL level from `retail-ai.md`; keep TAK Engine's own levels,
   retargeted to metal/energy and TA unit categories.
6. **Pathfinding**: Retail mode re-verified against TotalA.exe movement code;
   Legion mode kept (TA footprints / movement classes).
7. Unit rendering: 3DO textures from textures/*.gaf, team colour, TA effects.
8. Campaigns (camps/*.tdf, .ota missions, briefings), Cartographer for TA maps,
   random maps from worlds.hpi sections, multiplayer polish, packaging/branding.

## Known deviations from retail (deliberate)

- All *.hpi are mounted (retail stops after 10, hiding totala3/4 and worlds.hpi on the GOG layout).
- Battle-music monitor resets each game (retail carries it across games); default
  track table used on GOG installs, where retail plays only building music.
