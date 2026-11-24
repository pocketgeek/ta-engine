# Retail campaign and mission system (TotalA.exe v3.1c)

What this covers: how retail Total Annihilation lists campaigns, loads a mission from a
campaign `.tdf` and a mission `.ota`, spawns the pre-placed units and their scripted orders,
evaluates the win and lose conditions, and moves between NEWGAME, MSNBRIEF, the game and
ENDMSN. It describes behaviour for a clean-room reimplementation. No code or data from the
binary goes into the engine.

Sources. Addresses are VAs in `ta_install/TotalA.exe` (base 0x400000). `G` is the game object
`[0x511de8]`. `M` is the map/mission-info object `[G+0x391e9]`, and `C` is the
mission-condition object `[G+0x391ed]`. Data paths are under `assets/tadata/`. The tools are
in `tools/re/camp_*.py`:

- `camp_dis.py START END`: linear disassembly with TDF-helper and string notes.
- `camp_strs.py LO HI`: strings in a range, with the code that references them.
- `camp_xref.py call|disp ADDR…`: call sites of a routine, or code that uses a displacement.
- `camp_opscan.py PAT…`: every instruction whose operand contains a pattern.
- `camp_actions.py`: the order names behind each action id of the resolver at 0x43f0e0.
- `camp_survey.py`: census of the campaign `.tdf` keys and of the condition keys in every
  campaign mission OTA.

TDF helpers (see retail-data.md §1): `TDF_load` 0x4c2f60, `TDF_enter` 0x4c3410 (section
lookup is case-insensitive), `GetInt` 0x4c46c0 (atol), `GetDouble` 0x4c4760, `GetStr`
0x4c48c0, and `GetLocStr` 0x4c58a0, which tries `<LanguagePrefix><key>` first and then
`<key>`.

Confidence tags:
- **[C]** confirmed from code.
- **[D]** confirmed from data.
- **[I]** inferred from usage or naming.
- **[U]** unconfirmed.

---

## 1. Campaign files (`camps\*.tdf`)

### 1.1 Format [C][D]

```
[HEADER]   { campaignside=ARM; }           // ARM, CORE or ALL
[MISSION0] { missionfile=AC01.ota;
             missionname=1: A Hero Returns;
             Germanmissionname=…; Frenchmissionname=…; Italianmissionname=…; Spanishmissionname=…; }
[MISSION1] { … }
…
```

| Key | Where read | Meaning |
|---|---|---|
| `[HEADER] campaignside` | 0x476b5d, GetStr max 64, default "" | Side filter for the campaign list (§1.2). The comparison is a case-**sensitive** byte compare (inline strcmp at 0x476b80) against the side name from sidedata (`G+0x37f3d + side*0x232`), or against the literal `ALL`. The TDF parser trims whitespace, so `ARM; ` works. |
| `[MISSIONn] missionfile` | 0x435f6a, GetStr max 256 | The mission map is `maps\<missionfile>`, loaded as a TDF. The `.ota` is part of the value. The matching `.tnt` is `maps\<missionfile-without-ext>.TNT` (slot 1, 0x43605d). An empty value or a missing key gives the fatal error "Old TED format no longer supported!" (0x4360af). |
| `[MISSIONn] missionname` | 0x435864 (list), 0x435f48 (load); GetLocStr max 256 | Display name. With a language prefix, `Germanmissionname` etc. win. A missing name shows in the list as "Error -- Unnamed Mission" (0x43588e). |

No other key is read. Retail files use only these keys [D] (`camp_survey.py`).

**Mission count.** Missions are `MISSION0`, `MISSION1`, … up to the first missing index
(0x4356f0 and 0x435980 probe `MISSION%d` from 0 upwards). A gap ends the list. `0x435980(i)`
returns `count > i`, meaning "a mission with index i exists".

The retail files [D]:

| File | Side | Missions |
|---|---|---|
| arm campaign | ARM | 25 (AC01–AC25) |
| core campaign | CORE | 25 (CC01–CC25) |
| arm / core campaign - core contingency | ARM / CORE | 12 each |
| battle tactics - {arm,core} {very short,short,medium,long} | | 14/17/10/5 (arm), 15/20/13/6 (core) |
| krogoth encounter | ARM | 1 |

### 1.2 The campaign list on NEWGAME [C]

- `0x476a60(sideIndex, outList)` enumerates `camps\*.tdf` through the file lister 0x4af320.
  Sort mode 2 sorts the list by name [I]. Names are stored without the extension.
- For each file it loads the TDF, enters `[HEADER]` and reads `campaignside`. The campaign is
  listed when that value equals the player's side name exactly, or when it is `ALL`. A file
  with no `[HEADER]` is skipped.
- When a campaign is selected (0x4779e0), the engine loads it (`0x435110(name)`) and fills the
  Missions list from the `missionname`s (0x435760, 0x4a32a0). The Missions list is used only in
  "any mission" mode (`[0x51e668]`, 0x477b16) [I].
- **Any-mission mode.** Typing `DRDEATH` on the SINGLE screen (0x477510) toggles bit 0 of
  `G+0x38d7f`. That bit shows the hidden `AnyMsn` button and is saved as the registry value
  `AllMissions` (0x432b60). The value is read at startup (0x430eaa).
- `0x507b6c != 0` [U] picks a different path: the campaign is forced to "Arm Campaign" or
  "Core Campaign" from player 0's side (0x477bf7).

### 1.3 Starting a campaign (NEWGAME Start, 0x477b6d–0x477cc4) [C]

1. Campaign CD check (0x41d6a0). On failure it shows "Please insert the Campaign CD (Disc 2)…".
   An engine port should skip this.
2. `0x41d4c0` [U], then `0x41da30` resets the result string `G+0x391cf[0..24]` to 25 `'U'`
   followed by a NUL (§4.3).
3. `0x435110(campaignName)` loads `camps\<name>.tdf` into `M+0xa08`. A missing file gives
   "The requested campaign file, %s, does not exist."
4. The start index is 0. In any-mission mode it is the selected row of the Missions list.
   `0x435c00(i)` sets `M+0xc18 = i` and loads that mission (0x435da0, §2).
5. Player 0's lobby record gets `+0x96 = 0` and player 1's gets `+0x96 = 1` (0x477c79).
6. Settings are saved (0x430f00). Front-end state becomes 0xF (campaign) or 0x10 (any
   mission). Both lead to MSNBRIEF (retail-gui.md).

### 1.4 Progression and unlocking [C]

Retail has **no persistent unlock**.

- The registry readers and writers for `ArmCamp` / `CoreCamp` (0x432a80 and 0x432b00, a
  26-byte `U`/`W`/`L` string) have no callers. They are dead code.
- A new campaign always starts at mission 0, unless the player is in any-mission mode.
- Progress lives only in the current session and in save games, where the result string is
  saved as `Thumbs` (0x432869) and loaded at 0x4927f5. A loaded value whose length is not 25
  is reset to `U`×25.
- In ENDMSN, the Missions list shows **every** mission of the campaign, each with a W/L/U
  glyph (§4.3). The player may start any of them. Nothing is locked.
- The default selection is the next mission after a win and the same mission after a loss.

---

## 2. Mission loading (0x435da0, campaign path)

### 2.1 Sequence [C]

1. Delete the old condition object `C` (0x48dfb0) and create a new one (0x48df90, 0x8c
   bytes, `enabled=1`).
2. Reset the mission fields of `M`:
   - `+0xd30..+0xd3c = -1`
   - `+0xd40 = -1.0f`
   - `+0xd44 = +0xd48 = 0`
   - the Planet and description strings are emptied
   - the unit, special and feature arrays and the brief buffer are freed
3. Enter `[MISSION<cur>]` (cur = `M+0xc18`) and read `missionname` into `M+0xa14`
   (GetLocStr) and `missionfile`.
4. Load `maps\<missionfile>`. If it fails: "Hey, joker! There is no mission defintion for
   this mission: %s".
5. `[GlobalHeader]` → `maxunits` (default 200) → `G+0x37ee6`. Only the campaign path reads it.
   Then set path slot 1 to `maps\<file>.TNT`.
6. Read the GlobalHeader keys (§2.2) in the order listed there.
7. Parse the conditions: `C.Parse(GlobalHeader)` at 0x48e010 (§3.1).
8. Read `killmul` and `timemul`.
9. Pick the schema with `0x436860(mode, root, out)` (retail-ai.md §1). Campaign mode tries
   `type=` in this order:
   - Easy, Medium, Hard for difficulty 0
   - Medium, Easy, Hard for difficulty 1
   - Hard, Medium, Easy for difficulty 2

   Difficulty is `G+0x37eee`. If nothing matches: "No suitable schema type in mission file!".
10. Read the schema keys (§2.3). Then load `[units]`, `[specials]` and `[features]` (0x436c30,
    §2.4).

Path slots (`M+0x104 + slot*0x100`, built by `0x435430(slot, dir, name, ext)` and read back by
`0x4356c0(slot)`). An empty name gives an empty slot.

| Slot | Built from | Path |
|---|---|---|
| 0 | campaign | `camps\<campaign>.TDF` |
| 1 | — | `maps\<map>.TNT` |
| 2 | `brief` | `camps\briefs\<brief>.TXT` |
| 3 | `narration` | `camps\briefs\<narration>.WAV` |
| 4 | `missionhint` | `camps\hints\<hint>.TXT` |
| 5 | `glamour` | `<glamour>.PCX` |
| 6 | `UseOnlyUnits` | `camps\useonly\<file>.TDF` |
| 7 | `aiprofile` | `ai\<profile>.txt` (default `ai\default.txt`) |
| 8 | `glamoursound` | `camps\briefs\<sound>.WAV` |

Slot 4 has no reader. No `camps\hints` directory ships [D], so `missionhint` is effectively
unused [C].

### 2.2 `[GlobalHeader]` keys the game reads [C]

| Key | Read at | Type / default | Effect |
|---|---|---|---|
| `maxunits` | 0x43602c | int, 200 | Unit cap per player (`G+0x37ee6`). |
| `brief` | 0x4361f5 | GetLocStr, "" | The briefing text is loaded whole into `M+0xc14` (0x436248). The node CRC of GlobalHeader is kept at `M+0xc20`. |
| `narration` | 0x436297 | GetLocStr | Briefing voice WAV. It plays on MSNBRIEF entry and on replay (0x476c86, 0x476e6c, 0x478dc7) through 0x47f090 at volume 0x3c [I], unless `G+0x391f1 == 6`. |
| `missionhint` | 0x4362cd | GetLocStr | Slot 4 only. Never used. |
| `glamour` | 0x436302 | str | Victory picture `bitmaps\glamour\<slot5>` (0x41db50). The fallback is `glamour\Arm01.PCX`. |
| `glamoursound` | 0x436337 | str | Slot 8 WAV, played by 0x476ca0 [I: with the glamour screen]. |
| `UseOnlyUnits` | 0x43636c | str | Build whitelist file (§2.5). |
| `mapping` | 0x436393 | int, 0 | `G+0x3921d` (the "SingleMapping" setting). |
| `lineofsight` | 0x4363ae | int, 0 | `G+0x39221` (the "SingleLineOfSight" setting). |
| (forced) | 0x4363d9 | | `G+0x39225 = 1` (SingleLOSType) and `G+0x39219 = 0` (SingleCommanderDeath = game continues). Commander loss is therefore handled only by the mission's conditions. |
| `memory` | 0x4363f4 | str[128] | `M+0xdc4`. Display only [I]. |
| `numplayers` | 0x436413 | str[128] | `M+0xe44`. Display only [I]. |
| `Planet` | 0x436432 | str[128] | `M+0xca4`. Picks the briefing animation (§4.2). |
| `nomovie` | 0x436441 | int, 0 | `G+0x39073`. When set, the end-of-campaign movie after winning the **last** mission is skipped (0x41fc66). |
| `missiondescription` | 0x43646d | str, "No description available" | Upper-cased and passed through the translation table 0x4c5740. Stored at `M+0xc24`. |
| `minwindspeed` / `maxwindspeed` / `gravity` | 0x436521.. | int, 0 | `M+0xd34/+0xd38/+0xd3c`. |
| `tidalstrength` | 0x436565 | double → float | `M+0xd40`. |
| `lavaworld` / `nosealeveltrigger` / `waterdoesdamage` / `waterdamage` | 0x43657b.. | int | `M+0xd44/+0xd48/+0xd4c/+0xd50`. |
| condition keys | 0x48e010 | | §3. |
| `killmul` / `timemul` | 0x4365eb / 0x436603 | double → float | `M+0xd54 / +0xd58`. Used by the score screen [I]. |

Never read: `missionname` (in the OTA), `planet` (lower case works because lookup is
case-insensitive), `size`, `SCHEMACOUNT`, `solarstrength` (that string is not in the exe) [C].

### 2.3 Schema keys [C]

From the selected `[Schema N]`:

| Key | Read at | Effect |
|---|---|---|
| `HumanMetal` | 0x436646 | Starting metal for player index 0 (`M+0xd5c`). |
| `ComputerMetal` | 0x43667c | Starting metal for player index 1 (`M+0xd60`). |
| `HumanEnergy` | 0x43665e | Starting energy for player index 0 (`M+0xd84`). |
| `ComputerEnergy` | 0x43669a | Starting energy for player index 1 (`M+0xd88`). |
| `SurfaceMetal` | 0x4366b8 | `M+0xd30`. |
| `aiprofile` | 0x4366e1 | AI profile path in slot 7. |
| `MeteorWeapon` (str ≤31), `MeteorRadius` (int), `MeteorDensity`, `MeteorDuration`, `MeteorInterval` (double) | 0x436743–0x4367ac | Meteor storm. The storm is armed (0x437d40) only when the weapon name is non-empty and radius, density, duration and interval are all non-zero. Otherwise 0x437d50 / 0x438320 [I]. |
| `MohoMetal` | — | Not read in this routine [C]. |

The starting resources are applied in 0x465e30: for each player i in 0..9,
`stored metal = M[+0xd5c+4i]` and `stored energy = M[+0xd84+4i]`, through 0x496e90. Only
indices 0 and 1 are filled by the loader. Players 2–9 get whatever is in the object [U];
retail campaign maps use only Player=1 and Player=2 [D].

### 2.4 `[units]`, `[specials]`, `[features]` (0x436c30) [C]

**`[units]`** holds child sections in file order (`[unit0]`, `[unit1]`, …; the section names
are not checked). Each becomes a 0x24-byte record in `M+0xdac`, with the count in `M+0xdb0`:

| Key | Default | Record field / conversion |
|---|---|---|
| `Unitname` | — | +0 string (unit short name) |
| `Ident` | none | +4 string. Name used by `g`/`i`/`wa` orders (§2.6). |
| `InitialMission` | none | +8 string (§2.6) |
| `XPos`, `YPos`, `ZPos` | 0 | +0xc/+0x10/+0x14 = value << 16 (pixels to 16.16) |
| `Angle` | 0 | +0x18 u16 = trunc((deg<<16)/360) (the 0xb60b60b7 reciprocal at 0x436ef9) |
| `Player` | 0 → 1 | +0x22 byte. 0 is changed to 1. |
| `HealthPercentage` | 100 | +0x1a |
| `BuildPriority` | 0 | +0x20 (**unused**) |
| `CreationCountdown` | 0 | +0x1c (**unused**) |
| `MissionCriticalUnit` | 0 | +0x23 bit4 (**unused**) |
| `AiIgnore` | 0 | +0x23 bit5 (**unused**) |
| `AiPriorityTarget` | 0 | +0x23 bit6 (**unused**) |
| `InitialGroup` | 0 | +0x23 bits0–3 (**unused**) |
| `Immunity` | 0 | +0x23 bit7 → unit flag 0x8000 (§2.6) |
| `Kills` | — | **not read** |

"Unused" means no code reads the record field: every access to `M+0xdac` is in 0x436c30,
0x488310, 0x487af0 or the free paths [C].

**`[specials]`** (0x43700c): each child gives a 12-byte record in `M+0xdb4`, with the count in
`M+0xdb8`.

- If `specialwhat` starts with `StartPos` (strncmp over 8 characters), the record gets
  `valid=1`, `x=XPos` (word, px) and `z=ZPos` (word, px).
- The index comes from the digits after `StartPos`: `StartPosN` gives N−1. Without digits, a
  running counter is used (1, 2, … then −1).
- Any other `specialwhat` gives `valid=0`.

**`[features]`** (0x43717f): records of 0x88 bytes in `M+0xdbc`, count `M+0xdc0`.

- `Featurename` (≤127), `XPos`, `ZPos`, both defaulting to −1.
- A negative coordinate clears the name, so the feature is skipped.
- The coordinates are in 16-px map cells [I/D]: values up to 288 on 9×9 maps.

### 2.5 UseOnly unit whitelist (0x431740, from game start 0x4974de) [C]

- If slot 6 loads as a TDF:
  1. Clear the type flag `0x800000` ("available") at `type+0x241` on every unit type except
     index 0.
  2. Set it again for each **top-level section name** that matches a unit type's short name
     (`type+0x20`, case-insensitive). Section bodies are ignored; retail files are `[ARMCOM] {}` …
- The flag is global per type, so it limits every player (human and AI) [I].
- A missing or empty `UseOnlyUnits` leaves the normal availability unchanged.

### 2.6 Spawning the pre-placed units (0x488310) [C]

For each record, in order:

1. `type = FindUnitType(Unitname)` (0x488a50). If there is none, the slot is null and the unit
   is skipped.
2. `p = Player − 1`. It must be 0..9 and an active player (`player+0x73 ∈ {1,2,3}`,
   `+0x146 != 10`). Otherwise the fatal error is "Player number %d invalid for unit %s".
   **Player=1 is player index 0 (the human). Player=2 is index 1, the first computer.**
3. `0x47ddc0(type, &pos)` adjusts the position [I: snaps Y to the ground or sea surface].
4. `unit = CreateUnit(p, typeId, pos, 1, 1, 0)` (0x485f50) creates the unit fully built.
5. If Immunity is set: `unit.flags(+0x110) |= 0x8000` [I: cannot be damaged].
6. `unit.health(+0x108) = type.maxdamage(+0x1fa) * HealthPercentage / 100`, unsigned integer
   arithmetic.
7. `unit.heading(+0x66) = Angle`.

Then, for every spawned unit that has `InitialMission`, `0x487bf0(unit, string, spawnedArray)`
runs. Finally, **if the mission has no `[units]` at all, `C.enabled = 0`** (0x488541), so no
win or lose condition is ever evaluated.

**`InitialMission` grammar** (0x487bf0):
- The string is split on commas (strcspn `,`). Leading blanks are skipped.
- The first letter (case-insensitive, jump table 0x4882cc/0x488270) selects the command.
- Coordinates are pixel floats ×65536. Times are seconds ×30 (ticks).
- Each command queues an order (0x43adc0) with the action id or order name below.

| Cmd | Arguments | Order |
|---|---|---|
| `o` | ` %d %d` (a b) | No order. Sets the **move state** `(flags>>18)&3 = a&3` and the **fire state** `(flags>>20)&3 = b&3` (0x487c9d: both are pre-loaded from the unit's own bits, scanned, masked `&3` and written back as `((b&3)<<2 \| (a&3))<<18`; bit meanings per retail-weapon-scripts.md §8). A missing value keeps the current one; there is no range check. |
| `m` | ` %f %f` x z | Action 2 (move). |
| `p` | ` %f %f %f` x z t | Action 9 (patrol) to x,z. The extra parameter is t×30 [I]. |
| `a` | ` %f %f` x z | Action 3 (attack ground) when two numbers parse. |
| `a` | ` <type>` | Otherwise: `ATTACKUTYPE` with that unit-type id. |
| `u` | ` %f %f` | Action 5 (unload at x,z). |
| `g` | ` <ident>` | Action 7 (guard / follow) of the unit whose Ident (or else Unitname) matches (0x487af0, first match). |
| `i` | ` <ident>` | 0x48aac0(unit, target, −1, 0). Put the unit into that transport [I]. |
| `b` | ` <type> %d %f %f` | Mobile unit (`unit+0 != 0`): `MOBILEBUILD` type at x,z. Otherwise (factory): `BUILDINGBUILD` type × count. |
| `bw` | ` %d` | `BUILDWEAPON` × count (missile silos and similar). |
| `w` | ` %f %d` t n | `WAIT` for t×30 ticks. |
| `wa` | ` [ident]` | `WAITFORATTACK`. The trigger unit is the ident if given, else the unit itself. |
| `d` | | `SELFDESTRUCTFG`. |
| `s` | | `MAKESELECTABLE`. |

Action ids follow the resolver 0x43f0e0 (camp_actions.py): 2 move, 3 attack, 5 unload,
7 guard, 9 patrol, 14 mobile build. Unknown letters are ignored. Retail data has typos such
as `P P 502 1224`; the second `P` makes the float scan fail and the order uses
uninitialised coordinates [I].

**Selectability.** If any order was queued, the unit's flag `0x20` is cleared, which makes it
unselectable. Unless the script contained `a`, `p`, `d` or `s`, a final `MAKESELECTABLE` is
appended, so the unit returns to player control when its script ends (0x487e5b). Both points
matter for the conditions: §3.3 requires flag 0x20.

Example (ac01 gate): `o 0 0,w 3600 0,` sets hold position and hold fire, waits one hour,
then makes the unit selectable. The gate stays outside player control and does not count as
"alive" for `AllUnitsKilled`.

---

## 3. Win and lose conditions

### 3.1 Parse (0x48e010) [C]

`C` holds `victory[16]` at +0x00 (count +0x40), `defeat[16]` at +0x44 (count +0x84) and
`enabled` at +0x88.

- Every condition object starts `{vtbl, satisfied(+4)=0, celebrated(+8)=0}`.
- Keys are read from `[GlobalHeader]` in this fixed order. Each one that is present creates
  one condition.

| # | Key | Parse | List | vtable |
|---|---|---|---|---|
| 1 | `KillEnemyCommander` | GetInt ≠ 0 | V | 0x4fd960 |
| 2 | `DestroyAllUnits` | GetInt ≠ 0 | V | 0x4fd948 |
| 3 | `KillAllMobileUnits` | GetInt ≠ 0 | V | 0x4fd928 |
| 4 | `BuildUnitType` | GetStr (type) | V | 0x4fd908 |
| 5 | `CaptureUnitType` | GetStr (type) | V | 0x4fd8e8 |
| 6 | `KillAllOfType` | GetStr (type) | V | 0x4fd8d0 |
| 7 | `KillUnitType` | sscanf `%[a-zA-Z],%i` → type, count | V | 0x4fd8b0 |
| 8 | `MoveUnitToRadius` | sscanf `%[a-zA-Z],%i,%i,%i` → type, x, z, r (px). `ANYTYPE` gives an empty type. Stores x, z raw and r<<16. Y = sentinel 0x12345678. | V | 0x4fd890 |
| 9 | `UnitTypePassesX` | `%[a-zA-Z],%i` → type, X. Stores X>>4 (16-px cells). `ANYTYPE` = any. | V | 0x4fd870 |
| 10 | `UnitTypePassesZ` | same, Z>>4 | V | 0x4fd850 |
| 11 | `VictoryTimerRunsOut` | GetInt > 0 → deadline = n×30 ticks | V | 0x4fd830 |
| 12 | `CommanderKilled` | GetInt ≠ 0 | D | 0x4fd818 |
| 13 | `AllUnitsKilled` | GetInt ≠ 0 | D | 0x4fd800 |
| 14 | `AllUnitsKilledOfType` | GetStr (type) | D | 0x4fd7e0 |
| 15 | `UnitTypeKilled` | `%[a-zA-Z],%i` → type, count | D | 0x4fd7c0 |
| 16 | `DeathTimerRunsOut` | GetInt > 0 → n×30 ticks | D | 0x4fd7a8 |
| 17 | `AnyUnitPassesX` | GetInt(default −1) ≥ 0 → X>>4 | D | 0x4fd790 |
| 18 | `AnyUnitPassesZ` | same → Z>>4 | D | 0x4fd770 |

- **Defaults.** If no victory key exists, `DestroyAllUnits` is added. If no defeat key
  exists, `AllUnitsKilled` is added (0x48e977, and again lazily in 0x490230 / 0x490360).
- **Quirks.**
  - `%[a-zA-Z]` stops at the first digit or underscore, so a type name with digits would be
    cut short. All retail names are letters only [D].
  - A missing `,count` leaves the count uninitialised.
  - Values are taken after `GetStr` trimming. `sscanf %i` skips blanks, so
    `ARMSPID, 2058, 2129, 256` parses.
- Names are compared with `_stricmp` against the unit type's short name (`type+0x20`).
- Each condition saves and loads `Satisfied` / `Celebrated` (and `NumUnits` / `NumLeftToKill`)
  under the section `VictoryCondition_*` / `DefeatCondition_*` in save games. This happens
  only in campaign mode (0x48fdf0 / 0x48fe60).

### 3.2 Who is "enemy" and "own" [C]

Conditions use fixed player indices, not alliances:
- **Human**: player index 0. The unit slots are `G+0x1bca..[G+0x1bce]` (player 0
  `+0x67/+0x6b`), and `unit+0xff == 0`.
- **Enemy**: player index **1** only. The unit slots are `G+0x1d15..[G+0x1d19]`, the unit
  count is word `G+0x1df2` (player 1 `+0x144`), and `unit+0xff == 1`.

`unit+0xff` is the owner's player index [I, consistent everywhere]. A unit slot is live when
`unit+0xa6` (type id) ≠ 0. A third player is never examined by any condition.

### 3.3 Semantics of each condition [C]

Vtable slots: [0] `IsSatisfied()` (poll), [1] `OnUnitKilled(u)`, [2] `OnUnitCaptured(u)`,
[3] `OnUnitCreated(u)` (no-op for every type), [4] Save, [5] Load.

- **Latched** means `satisfied` stays 1 once set.
- **Celebrate** means that the first time the condition is satisfied, the "Victory Condition"
  sound plays once (0x47f1a0) and `celebrated` is set. This happens only for victory
  conditions.

Victory conditions:

| Condition | Driven by | Rule |
|---|---|---|
| KillEnemyCommander | event [1] 0x48ea40 | Killed unit has owner 1 and type name == the commander name of its owner's side (`G+0x37f5f + side*0x232`, the sidedata commander). Latched. Celebrate. |
| DestroyAllUnits | poll 0x48eb40 | Player 1 unit count (`G+0x1df2`) == 0. Recomputed every poll, not latched. Celebrates the first time. Counts every unit, including unfinished ones and units under script. |
| KillAllMobileUnits | event [1] 0x48ec20 | Killed unit has owner 1 and is mobile (`unit+0 != 0`, the same test that `b` uses). Count player 1's live mobile units, which still includes the dying one, stopping at 2. If the count ≤ 1, latch and celebrate. If the enemy never loses a mobile unit, this never fires. |
| BuildUnitType | poll 0x48edb0 | Once latched, return 1. Otherwise find a player-0 unit of the type (id cached at +0x30 via 0x488b10) that is fully built (`+0x104 == 0.0`). Latch and celebrate. |
| CaptureUnitType | event [2] 0x48eeb0 | Called from 0x488570 (unit changes owner) before the owner changes. Old owner is 1 and the type matches. Latch and celebrate. Any ownership transfer counts [I]. |
| KillAllOfType | event [1] 0x48efb0 | Not yet latched, killed unit has owner 1 and the type matches. Count player 1's units of that type id, dying one included, stopping at 2. If ≤ 1, latch and celebrate. |
| KillUnitType | event [1] 0x48f0f0 | Only while `left > 0`. Killed unit has owner 1 and the type matches: `left -= 1`. When `left <= 0`, latch and celebrate. A parsed count of 0 never fires. |
| MoveUnitToRadius | poll 0x48f200 | On the first poll, (x, z) px become a 16.16 world point with ground height (0x484b50, clamped to the map). Then all units within 2-D distance ≤ r of the point are enumerated (0x47e890, 128-px bucket grid; exact test `dx²+dz² ≤ r²`). Filter 0x48f250: owner 0; type matches or ANYTYPE; selectable (flag 0x20); fully built; `unit+0xfb == 0`; and either not carried (`unit+0x86 == 0`) or the carrier has flag 0x40000000 [I]. A hit latches and celebrates. |
| UnitTypePassesX / Z | poll 0x48f3e0 / 0x48f530 | Not latched yet. Any player-0 unit whose type matches (or ANYTYPE) and whose cell column `unit+0x76` (row `+0x78`) is within ±2 of X>>4 (Z>>4). Latch and celebrate. There is no completeness or selectability test. [I: +0x76/+0x78 are the unit's 16-px grid cell.] |
| VictoryTimerRunsOut | poll 0x48f610 | `gameTick (G+0x38a47) >= n*30`. The tick is absolute from game start. Never celebrates. |

Defeat conditions:

| Condition | Driven by | Rule |
|---|---|---|
| CommanderKilled | event [1] 0x48f6b0 | Killed unit has owner 0 and type == the commander of its side. Latched. |
| AllUnitsKilled | poll 0x48f7e0 | Set satisfied=1, then scan player 0's units with filter 0x48f790. The filter clears satisfied when it finds a unit that is selectable (0x20), fully built, has `+0xfb == 0`, and is not carried (or its carrier has flag 0x40000000). Not latched. **Units under an InitialMission script (unselectable) and unfinished units do not keep the player alive.** |
| AllUnitsKilledOfType | event [1] 0x48f9d0 | **Any** owner's unit of the type dies. Count units of that type over players 0 **and** 1, dying one included, stopping at 2. If ≤ 1, latch. |
| UnitTypeKilled | event [1] 0x48f8c0 | **Any** owner's unit of the type dies: `left -= 1`. If `left <= 0`, latch. A count of 0 fires on the first death. |
| DeathTimerRunsOut | poll 0x48fd50 | `gameTick >= n*30`. |
| AnyUnitPassesX / Z | poll 0x48fb60 / 0x48fc70 | Any **player-1** unit with `abs(cell − X>>4) ≤ 2`. Latched. |

### 3.4 Combination and cadence [C]

- `IsVictory()` (0x490230): returns false if `!enabled`. In campaign mode it adds the
  default if the list is empty and returns the **AND** of every victory `IsSatisfied()`,
  stopping at the first false. Skirmish (mode 2): every other non-allied player has 0 units.
  Network (mode 3): 0x490080.
- `IsDefeat()` (0x490360): returns false if `!enabled`. In campaign mode it returns the
  **OR** of every defeat `IsSatisfied()`. Modes 2 and 3: the local player has 0 units.
- **Anti-piracy.** If the "no CD" flag `[0x511de4]` is set (0x41d8b0; it is set when the
  `TOTALA.ID` check fails at 0x41d735), `IsDefeat` returns true once
  `tick ≥ 9000 + rand()*9000/32768`. Do **not** port this.
- **Polls run once per second.** The player loop 0x464f80 runs every tick. When the tick
  reaches a player's settle time (`p+0xf0`, advanced by 30), and that player is the local
  player (`G+0x2a42`), the campaign path (0x4650a9) does:

```
if IsVictory():   countdown(VICTORY)
elif IsDefeat():  countdown(DEFEAT)
countdown(kind):
  if endTimer(G+0x39239, int16) < 0: endTimer = 4
  else: endTimer -= 1
        if endTimer < 0:
            flags(G+0x3923b) |= 4                      // game over
            VICTORY: flags |= 0x10 | 0x20   DEFEAT: flags &= ~0x10; flags |= 0x40
```

- `endTimer` starts at −1 (0x498199, game init and load). The game therefore ends **5 checks,
  about 5 seconds, after the first check that sees the outcome**.
- The timer is shared, and it is not reset if the condition later becomes false. It only
  stops counting on seconds where neither outcome is true.
- Victory is tested before defeat.
- While `endTimer >= 0` or the game-over flag is set, the economy settle (0x401360) is
  skipped for human and AI players (0x46554f).
- **Events fire synchronously.**
  - Unit death: 0x486799 → `C.OnUnitKilled(u)` (0x4904c0), before the unit is removed.
  - Ownership change: 0x4885b1 → 0x490520.
  - Unit creation: 0x4861ae / 0x48634a → 0x490580.
- Event handlers run in every game mode, but `IsVictory`/`IsDefeat` read the lists only in
  campaign mode.
- `C.enabled = 0` is also set by 0x4164b0 [U: a network message].

---

## 4. Screen flow

### 4.1 Front-end states (retail-gui.md)

```
SINGLE --NewCamp--> NEWGAME (state 0xA)
NEWGAME --Start--> load campaign + mission 0 (or the picked one) --> state 0xF/0x10 --> MSNBRIEF
MSNBRIEF --Start--> game (state 2) ; PrevMenu --> previous menu
game over (flags & 4) --> end-of-game processing 0x41dc20 --> [glamour / movie] --> ENDMSN (0x41f630)
ENDMSN --Start / double-click Missions--> load selected mission --> briefing
ENDMSN --MainMenu--> main menu ; LoadGame / SaveGame --> LOADGAME.GUI
```

### 4.2 MSNBRIEF [C]

- **Text.** The text is `camps\briefs\<brief>.txt` loaded raw. In this format:
  - `&X` starts colour X and a lone `&` ends it.
  - An open colour is closed and reopened across each newline (`&\r\n&X`, 0x476cd0).
  - Byte 0xFF ends the text.
  - Retail text begins with a `MISSION n.nnnn` header line [D].
- **Animation.** `Planet` is matched against this list (0x4790dd): "Green planet",
  "Archipelago", "Wet Desert", "Desert", "Lava", "Red Planet", "Lunar", "Metal", "Lunar2",
  "Ice", "Lush", "Slate", "Water World", "Acid", "Crystal". The matching animation is
  `Greenbrief`, `Archibrief`, `WDesertbrief`, `Desertbrief`, `Lavabrief`, `Marsbrief`,
  `Lunarbrief`, `Metalbrief`, `Lunar2brief`, `Icebrief`, `Lushbrief`, `Slatebrief`,
  `Waterbrief`, `Acidbrief` or `Crystalbrief`. The default is Greenbrief [I].
- **Narration.** The narration WAV plays on entry. The SHUTUP toggle mutes it.

### 4.3 End of game and ENDMSN [C]

1. **0x41dc20** sets `G+0x391af = won` (`flags & 0x10`) and `G+0x391ab = cur`. It records
   `results[cur] = won ? 'W' : 'L'` in `G+0x391cf`.
2. **Victory picture** (0x41db1b). In a campaign mission that was won with a `glamour` name,
   `bitmaps\glamour\<glamour>.PCX` is loaded and shown first (state 6, 0x41fd30).
3. **End-of-campaign movie** (0x41fc4c). If the mission was won, it was the last one
   (`!HasMission(cur+1)`), and `nomovie` is 0, a movie plays [U: which file].
4. **ENDMSN setup** (0x41f0a0). `canContinue` = campaign mode and (lost or
   `HasMission(cur+1)`). Then:
   - If canContinue: reload mission `cur` (0x435c00). The backdrop is `bitmaps\outcome1.pcx`
     and focus goes to `Start`. Fill `Missions` with every mission name, each prefixed by a
     glyph and a space: 0xFF for `L`, 0xFE for `W`, 0xFD for `U` (0x41eaa0). Select row
     `cur + won`.
   - Otherwise (won the last mission, or not a campaign): the backdrop is
     `bitmaps\outcome0.pcx` and focus goes to `MainMenu`. Start, LoadGame, SaveGame, KNOB,
     Missions, Difficulty and AdjustDiff are hidden (0x41f400).
5. **Buttons.**
   - Start: CD check, then `0x435c00(selectedRow)`.
   - Difficulty: cycles Easy → Medium → Hard → Easy (`G+0x37eee` and lobby `+0x228`). It
     takes effect at the next mission load, because the schema is chosen then.
   - SaveGame: saves `Mission` as the name of mission `cur+1` (0x435c60 advance, then a
     restore at 0x432962), with `BetweenMissions=1`, `Thumbs`, `Campaign` and `Difficulty`.
     This happens even after a loss.

---

## 5. Engine implementation spec

### 5.1 Data

```cpp
struct CampaignFile {                // camps/<name>.tdf
  std::string name;                  // file stem, shown in the list
  std::string side;                  // [HEADER] campaignside ("" when absent)
  struct Entry { std::string file, name; };  // missionfile (with .ota), localized missionname
  std::vector<Entry> missions;       // MISSION0..k-1, stop at first gap
};
enum class Cond : uint8_t { KillEnemyCommander, DestroyAllUnits, KillAllMobileUnits, BuildUnitType,
  CaptureUnitType, KillAllOfType, KillUnitType, MoveUnitToRadius, UnitTypePassesX, UnitTypePassesZ,
  VictoryTimer, CommanderKilled, AllUnitsKilled, AllUnitsKilledOfType, UnitTypeKilled, DeathTimer,
  AnyUnitPassesX, AnyUnitPassesZ };
struct Condition {
  Cond kind; bool satisfied=false, celebrated=false;
  std::string type;          // short name, "" = any
  int32_t count=0;           // KillUnitType / UnitTypeKilled: left to kill
  int32_t cell=0;            // PassesX/Z: px>>4
  int32_t x=0, z=0; uint32_t radius=0; bool posResolved=false; Fix3 pos;  // MoveUnitToRadius
  uint32_t deadlineTick=0;   // timers: seconds*30
};
struct MissionUnit { std::string unitname, ident, initialMission; int32_t x,y,z; /*px<<16*/
  uint16_t angle; uint8_t player; uint16_t healthPct=100; bool immune; };
struct Mission {
  std::string name, description, planet, briefText, narrationWav, glamourPcx, glamourWav, useOnlyTdf;
  int maxUnits=200, mapping=0, lineOfSight=0, nomovie=0;
  int minWind, maxWind, gravity, lavaworld, noSeaLevelTrigger, waterDoesDamage, waterDamage;
  float tidal, killmul, timemul;
  // schema
  std::string aiProfile; int surfaceMetal; float startMetal[2], startEnergy[2]; Meteor meteor;
  std::vector<MissionUnit> units; std::vector<StartPos> starts; std::vector<FeaturePlace> features;
  std::vector<Condition> victory, defeat; bool condEnabled=true;
};
struct CampaignSession { CampaignFile camp; int cur=0; int difficulty; char results[25]; /* 'U','W','L' */ };
```

### 5.2 Parsing rules

- Use the existing TDF reader (retail-data.md §1): case-insensitive keys, last duplicate
  wins, GetInt is atol, GetStr truncates to max−1. `GetLocStr(key)` tries
  `<Language>key` first and then `key`.
- **Campaign list.**
  - Scan `camps/*.tdf` sorted by name.
  - Keep a file when `side == playerSideName` (case-sensitive) or `side == "ALL"`.
  - Mission count: probe `MISSION0..` and stop at the first missing section.
- **Mission load**, in this order:
  1. GlobalHeader scalars, then the conditions in table order (§3.1), then
     killmul/timemul, then the schema.
  2. The schema is picked by `type` with the difficulty fallback order (§2.1 step 9). If
     none matches, the load fails.
  3. Then units, specials and features.
- Condition keys:
  - A flag key is present when GetInt ≠ 0. Timers need GetInt > 0. PassesX/Z defeat keys
    need GetInt(default −1) ≥ 0.
  - Type parsing: take leading `[A-Za-z]` characters as the type, then
    `,int[,int,int]` with blanks allowed. `ANYTYPE` (case-insensitive) means any.
  - After all keys: an empty victory list gets `DestroyAllUnits` and an empty defeat list
    gets `AllUnitsKilled`.
- Units: `Player 0 → 1`. Player index = Player − 1 and must be an existing slot. Angle =
  trunc(deg·65536/360). Health = maxdamage·pct/100.

### 5.3 Spawn

1. Apply the UseOnly whitelist before anything is built. If the file exists, available =
   the section names present.
2. Spawn every unit fully built at (x, ground-adjusted y, z) with the heading set.
3. Then run each InitialMission in record order (§2.6 grammar). Ident lookups resolve
   against spawned units: first record whose `Ident`, or else `Unitname`, matches
   case-insensitively.
4. When a script queued orders: make the unit unselectable, and append `MAKESELECTABLE`
   unless the script contained `a`/`p`/`d`/`s`.
5. If the mission has zero units: `condEnabled = false`.
6. Starting resources: player 0 = Human*, player 1 = Computer*. Force commander-death to
   "continue" in campaign mode.

### 5.4 Per-tick evaluation

Event hooks run in sim order:

- `OnUnitKilled(u)`, called before the unit is removed from its owner's list:

  | Condition | Requires | Action |
  |---|---|---|
  | KillEnemyCommander | owner 1, type == commander of owner's side | latch |
  | KillAllMobileUnits | owner 1, u mobile | latch if (# live owner-1 mobile units incl. u) ≤ 1 |
  | KillAllOfType | not latched, owner 1, type match | latch if (# owner-1 units of type incl. u) ≤ 1 |
  | KillUnitType | left > 0, owner 1, type match | `--left`; latch when ≤ 0 |
  | CommanderKilled | owner 0, commander of side | latch |
  | AllUnitsKilledOfType | type match, any owner | latch if (# of type over players 0+1 incl. u) ≤ 1 |
  | UnitTypeKilled | type match, any owner | `--left`; latch when ≤ 0 |

- `OnUnitOwnerChange(u, newOwner)`, before the change: CaptureUnitType latches when the old
  owner is 1 and the type matches.
- **Every victory latch** plays the "Victory Condition" sound once.

Once per second (tick ≥ the local player's settle tick, every 30 ticks), in campaign mode,
if `condEnabled`:

```
victory = all(c.poll() for c in victory)     // stop at first false
if victory: step(VICTORY) elif any(c.poll() for c in defeat): step(DEFEAT)
```

| Condition | `poll()` |
|---|---|
| DestroyAllUnits | live unit count of player 1 == 0 (not latched) |
| BuildUnitType | latched, or player 0 has a complete unit of the type → latch |
| MoveUnitToRadius | latched, or a player-0 unit of the type (or any) that is selectable, complete, not dying, and not carried, with `(ux−x)²+(uz−z)² ≤ r²` (px, 2-D) → latch |
| UnitTypePassesX/Z | latched, or a player-0 unit of the type (or any) with `abs(cellX − X/16) ≤ 2` (cellZ for Z) → latch |
| VictoryTimer / DeathTimer | `tick ≥ n·30` |
| AllUnitsKilled | player 0 has no unit that is selectable, complete, not dying and not carried (not latched) |
| AnyUnitPassesX/Z | latched, or any player-1 unit with `abs(cell − X/16) ≤ 2` → latch |
| event conditions | return `satisfied` |

`step(kind)`:
1. If `endTimer < 0`, set `endTimer = 4`.
2. Otherwise decrement it. When it drops below 0, set gameOver plus won or lost.

`endTimer` starts at −1. While it is ≥ 0 or the game is over, stop the economy settle.
Victory and defeat therefore take effect 5 s after first detection.

For determinism, keep the sim-side state (satisfied flags, counts, endTimer) in the
lockstep state and save it with the game (retail saves Satisfied, Celebrated, NumUnits and
NumLeftToKill per condition).

### 5.5 Screen flow

1. NEWGAME:
   - Pick the side, then a campaign from the filtered list, then the difficulty.
   - Start: `results = "U"*25`, `cur` = 0 (or the any-mission pick), load the mission.
2. MSNBRIEF:
   - Show the brief text (`&X` colour codes) and the planet animation from `Planet`.
   - Play the narration WAV. Start launches the game.
3. Game over:
   - `results[cur] = won ? 'W' : 'L'`.
   - If won with `glamour`: show `bitmaps/glamour/<glamour>.pcx` and play glamoursound.
   - If won the last mission and `!nomovie`: play the ending movie.
4. ENDMSN:
   - `canContinue = lost || cur+1 < count`.
   - If canContinue: backdrop `outcome1`, a list of all missions with W/L/U glyphs, and
     selection `cur + (won ? 1 : 0)`. Start loads the selected mission and goes to
     MSNBRIEF. Difficulty cycles. Save/Load are available.
   - Otherwise: backdrop `outcome0` with MainMenu only.
5. Persistence: retail keeps nothing outside save games. The engine may add a profile file
   storing `results` per campaign, but by default it should not lock any mission. Retail
   lets you pick any mission from ENDMSN, and from NEWGAME through the any-mission toggle.

### 5.6 Known retail quirks to keep or decide on

- Only player index 1 is ever "the enemy". A second computer player in a mission is ignored
  by every condition.
- AllUnitsKilled ignores unselectable (scripted) and unfinished units. A player whose only
  units are scripted loses.
- AllUnitsKilledOfType and UnitTypeKilled count deaths on **both** sides.
- KillUnitType with count 0 can never be satisfied. UnitTypeKilled with count 0 fires on the
  first death.
- Event conditions latch, but DestroyAllUnits and AllUnitsKilled are re-evaluated on every
  poll. Victory needs all conditions true **in the same poll**.
- Saving from ENDMSN after a loss records the *next* mission.
