# Cartographer port

A clean-room 1:1 re-implementation of **Cartographer**, the retail TA:Kingdoms
map editor (`Cartographer.exe`, Cavedog, May 1999). Behaviour and file formats
are reverse-engineered from the retail binary by **static analysis only** — same
rules as the engine (`docs/retail-engine.md`): never copy its code or ship the
binary/its assets; reimplement observed behaviour.

## What it is (from the PE)

MFC + DirectDraw + COMCTL32 GUI app, 4 sections, imagebase 0x400000:
`.text 0x1000`, `.rdata 0xfa000`, `.data 0x11c000`, `.rsrc 0x174000`
(raw 0x135000). MFC document/view: one map document, a scrolling tile canvas,
modal property dialogs. Credits (About dlg): James Loe (programming + design),
Ron Gilbert (design).

## The complete feature map (from resources)

### Main menu (MENU 128)
- **File**: New (Ctrl+N), Open (Ctrl+O), Save (Ctrl+S), Save As (Ctrl+A),
  Recent, Exit.
- **Edit**: Land Lasso (Ctrl+L, #32789), Clear Area (#32796).
- **View**: Toolbar, Status Bar, Triggers (#32791), Grid (#32794).
- **Scenario**: Properties (#32780), Use Only (#32782), Check Map (#32798),
  Resize (#32787), Scripting (#32790).
- **Zoom**: In (`+` #32785), Out (`-` #32786), 100/75/50/25/12.5%
  (#32771-74, #32795).
- **Help**: About (#57664).

### Context menu (MENU 139) — right-click on a placed object
- **Unit**: Delete (#32775), Properties (#32776).
- **Feature**: Delete (#32777).
- **Start Pos**: Delete (#32779).
- **Tree Unit**: Default Properties (#32781).
- **Trigger**: Delete (#32792), Properties (#32793).

### Dialogs (the data model)
- **New** (30721): fresh-map size + world.
- **Scenario** (133): Width, Height "(in Units {512})", World Type combo,
  Load... — the new-map / world setup. 1 Unit = 512 (16.16 world units? = 32px
  block). So map dims are entered in "units", 512 per block.
- **Scenario Properties** (143): Scenario Name, Scenario Description.
- **Resize** (145): Width, Height (in Units).
- **Use Only** (144): a listbox — the buildable-unit restriction set.
- **Unit Properties** (140): Unit Type combo, Unique Name, Player ID, Health%,
  Veteran, X/Y/Z Pos, Armor%, Weapon%, Angle.
- **Default Unit Properties** (142): Unit Type, Veteran, Armor%, Weapon% — the
  "Tree Unit" (feature) default brush.
- **Scenario Scripting** (146): Conditions list + Actions list per rule,
  Add/Remove Condition/Action, Copy/Paste Rule, Copy/Paste Player Rules, Done.
- **Conditions** (147) / **Actions** (148): pick a condition/action type.
- **Rule editor** (150): 5 combo params + 5 spinners ("Rule"/"Keys" group) —
  the condition/action argument editor.
- **Trigger Properties** (151): Trigger Name.
- **About** (100), **Initializing** (131) splash.

## The editable objects

1. **Terrain**: tile mosaic (paint the `.tnt` tile plane) + heightmap +
   the per-cell feature/road/blocker plane (0xFFFB road, 0xFFFC block).
2. **Features** ("Tree Units"): trees/rocks/mana/etc. with a default brush
   (type/veteran/armor/weapon) — stored in the tnt feature plane by name.
3. **Units**: placed with full props (type, name, player, HP%, veteran, XYZ,
   armor%, weapon%, angle) — scenario data (OTA/side file).
4. **Start positions**: numbered spawn points (OTA `specialwhat=StartPosN`).
5. **Triggers**: named rules of Conditions + Actions, per-player rule sets,
   copy/paste — the scenario scripting system (OTA + a script blob).

## File formats to master (RE targets)

- **`.tnt`** WRITE format — DONE (`Map::save()`, RE'd from Cartographer 0x41ba70):
  52-byte header (13 dwords, magic 0x4000) then sections in physical order:
  heights(u8/cell) · features(u16/cell) · feature-name table(count x 132B:
  {u32 seq, 128B name}) · tile keys(u32/block) · cols(u8) · rows(u8) ·
  small minimap(126x126 {u32 w,u32 h,bytes}) · large overview minimap
  ({u32 w,u32 h,bytes}). All row-major. Minimap regeneration (from tiles +
  the 256-colour terrain palette) is still needed for NEW/resized maps --
  round-trip preserves the loaded ones verbatim.
- A shipped map is a **`.kmp` = HPI archive** of 5 members (kmap\<name>.{tnt,
  ota,tdf,crt,txt}); only .tnt + .ota are needed for a game-loadable map. The
  .crt holds placed game units as raw 568-byte binary structs (editor-internal).
- **`.ota`** — scenario TDF; the editor emits (in order) [GlobalHeader]:
  Copyright, missionname, missiondescription, kingdom, numplayers, size
  (`W x H`, cells/32), memory=`32 MB`, useonlyunits, hasscenario; nested
  [Map Data]: Type=`Network 1`, aiprofile=`DEFAULT`; nested [specials] /
  [special%d]: specialwhat=`StartPos%d`, XPos, ZPos. Start positions are
  16-byte records (XPos, ZPos, number). WRITER still to build (phase 4).
- Tile sections come from the retail install's `terrain/<hexkey>.jpg` +
  `sections.hpi` prefabs — reuse `terrain::Compositor` + the section prefab loader.

## Port architecture (planned)

`src/cartographer/` — an SDL2/C++20 app (`cartographer` target), reusing the
engine's `tnt`, `terrain::Compositor`, `hpi::Vfs`, `client/mapview` (render),
`client/font`, `jpeg`. New pieces: a **TNT writer**, an **OTA scenario writer**,
the **tile/section palette + brush tools**, the **height tools**, the
**feature/unit/start/trigger object model + property dialogs**, and the
**trigger (condition/action) system**. Immediate-mode UI in our own widgets
(we have font + input already), matching Cartographer's panel layout 1:1.

### Build phases
0. Skeleton: window, open a `.tnt`, render terrain (reuse MapView), pan/zoom. ✅ this session
1. TNT writer + New/Save/Save As round-trip (load→save→byte-compare).
2. Tile palette + paint brush; height tools; Grid overlay; zoom levels.
3. Feature/unit/start-pos placement + selection + the property dialogs. ✅
   [start positions + FEATURES + UNITS all DONE. Units read/written through the
    SHARED ta::crt (parse/write; also used by taclient/taserver). Place/move/
    delete + a Unit Properties dialog (double-click: player/health/armor/weapon/
    veteran/angle); Save writes the map's .crt, preserving the trigger rules,
    regions, and custom types the unit tool doesn't touch.]
4. Scenario Properties / Resize / Use Only / Check Map. ✅
   [Scenario Properties + Resize (modal widget layer); Use Only checklist overlay
    (toggle allowed types; writes the sibling <map>.tdf `[TYPE]\t{}` list + the
    OTA `useonlyunits=<name>.tdf` ref, empty=unrestricted); Check Map message box
    (warns which placed unit types are restricted — the one retail validation,
    also fired at save). Keys: P/R/U/C.]
5. The trigger system (Scripting): condition/action opcode tables (RE), rule
   editor, per-player rules, script write. ✅
   [Scripting overlay (key T): per-player rule groups, each a list of conditions
    + actions rendered human-readably from the 26+26 opcode templates
    (src/cartographer/triggers). Navigate players/rules; +/- RULE, +/- COND,
    +/- ACT (26-item opcode picker); double-click a condition/action to edit its
    parameters (fields labelled by param kind). Saves through ta::crt::write in
    the .crt -- verified: Ulin's Folly's 30 rules load, render correctly ("I
    control more than 2 ARAAT at hill"), and save back byte-identical.]
6. Land Lasso + Clear Area; the .kmp bundle writer; polish to 1:1. ✅
   [Clear Area (key K): arm, drag a box, confirm -> removes units + clears the
    feature plane inside. Land Lasso (Ctrl+L): land-paint vs object-mode toggle.
    .kmp bundle: SHARED ta::hpi::pack writes an HPI holding kmap/<name>/
    <name>.{tnt,ota,crt,txt}(+.tdf) -- the retail distributable-map format the
    engine mounts directly (Ctrl+B, or headless --bundle). Verified: a bundled
    map loads through the engine's real path (MountSet -> findMap -> Map::load ->
    28 units); hpitool pack round-trips. Recent Files is N/A -- the editor opens
    maps by name via the VFS, not a file-open dialog.]

The Cartographer port is FEATURE-COMPLETE: all editor tools, the full Scenario
menu, and the .kmp writer. Remaining work is engine-side (playing scenario maps:
apply .crt unit stats + a typed trigger evaluator) and cosmetic polish.

## Deep RE findings (tools + triggers, confirmed from the binary)

### Tile painting = section-PREFAB stamp (not per-cell, not flood)
- Left panel is a palette (`CItemView`) with tabs: **Map Sections, Special,
  Features, Trigger, Units, Buildings**. Map Sections load from
  `sections/<world>/<category>/*.tnt` (each a prefab `.tnt`; a section is
  256px = 8x8 tiles = 16x16 cells). We ALREADY load section prefabs for the map
  generator (reuse that).
- Left-click (land mode) snaps to a **256px grid** and copies the selected
  section's cells `{tileKey u32, col u8, row u8}` verbatim into the tile plane.
  Height rides in the prefab -- there is **NO separate height tool** (painting a
  section sets art + height together; confirms the flat-mosaic model).
- **Land Lasso** (Ctrl+L) is a MODE TOGGLE (land-editing on = stamp sections;
  off = place/select palette objects), not a marquee.
- **Clear Area**: drag a region -> confirm -> removes all units+features inside.

### Zoom = 5 discrete levels only
1.0 / 0.75 / 0.5 / 0.25 / 0.125 (In/Out step 0.25, half-step at 0.125<->0.25).

### World Type = kingdoms from `gamedata/sidedata.tdf`
SIDE0 ARAMON, SIDE1 TAROS, SIDE2 VERUNA, SIDE3 ZHON (SIDE4-6 non-playable;
CREON only in Iron Plague `IPData.hpi`). Selecting a world sets OTA
`kingdom=<lower>` and drives per-world paths: `sections/<world>/...`,
`features/<world>/*.tdf` (+ `features/all worlds`, `features/corpses`),
palettes `palettes/<world>{,_features,_textures}.pcx`, and `waterheight`
(ARAMON=40, others ~58) from that SIDE. **Minimap palette = `palettes/<world>.pcx`.**

### Coordinate model (fully reconciled)
1 cell = 16px; 1 tile/block = 32px = 2 cells; **1 "Unit" = 512px = 32 cells =
16 tiles**. TNT header dims are in cells. OTA `size = cells>>5` (Units).
Unit/StartPos XPos/ZPos are in **cells** (world px = xpos*16).

### Check Map (cmd 32798) = ONE validation
Warns if any placed unit's type is on the use-only restriction list ("...they
have been restricted. They will not show up in the game."). No pathing/overlap
checks. Same warning also fires at save.

### The scenario (placed units + triggers) is a BINARY `.crt`, NOT the OTA
`.crt` writer 0x40d8d0 / loader ~0x40de42. Full layout **PROVEN byte-exact**
against every shipped `.crt` (empty 56-byte files through 120 KB Savannah Hunt)
and implemented in the SHARED `src/crt/` (`ta::crt::parse`/`write`, used by
taclient/taserver AND cartographer). Top-level, no padding anywhere:

```
f32 version = 1.0
i32 numCustomTypes;  CustomType[272] x N
i32 numUnits;        UnitRecord[568]  x N
i32 numPlayers (=9);
    per player: i32 numGroups
        per group: i32 numConditions; Rule[324] x n
                   i32 numActions;    Rule[324] x n
i32 numRegions;      RegionDef[272] x N
```

- **UnitRecord (568/0x238):** objectName char[256]@0x000 · uniqueName
  char[256]@0x100 · i32 X@0x200 (16px cells) · i32 Y@0x204 (=200) · i32 Z@0x208
  · i32 player@0x20c (0..8) · i32 health%@0x210 · i32 armor%@0x214 ·
  i32 weapon%@0x218 · i32 angle@0x21c (deg 0..359) · i32 veteran@0x220. Bytes
  0x224..0x238 are a type-derived cache + heap ptr = in-memory residue; a clean
  writer zero-fills them and the game loads identically. Coords are **16px
  cells** (NOT the old 2px-unit guess; old offsets 0x160/0x1f8/0x1fa are zero in
  every file — that was the bug in the pre-RE reader).
- **CustomType (272/0x110):** name char[256]@0x000 · i32 stat[4]@0x100 =
  default {health,armor,weapon,?} — a type is "custom" only when its stats
  differ from {100,100,100,0} (e.g. Cairbray's VERBALL = {100,0,0,0}).
- **Rule (324):** i32 opcode@0x000 · 5 x char[64] operand slots@0x004. A "group"
  is one condition-set + one action-set; conditions and actions share this
  record format but use INDEPENDENT opcode spaces. Numeric spinner values are
  stored as ASCII in the slots (no separate int array). In memory the record is
  0x244 with slots at 0x100.. and opcode at 0x240; on disk it is 324.
- **RegionDef (272/0x110):** name char[64]@0x000 · i32 x1,z1,x2,z2@0x100 (cells);
  0x040..0x0FF is in-memory junk (zero-filled on a clean write). Includes the
  per-player start zones ("Player 1"...).
- There is **no per-player header** — team/color/AI live in the `.ota`, not the
  `.crt`; player identity is purely the index 0..8.

The 1328-byte RuleRecord in an earlier note was wrong (it is 324 on disk).

### Trigger opcodes: 26 CONDITIONS + 26 ACTIONS (tables at 0x51c190)
Param types: value, unit type, player, location, text string, flag. Conditions
are opcode==display-index (identity remap). ACTIONS: display order != internal
opcode -- a remap permutation (0x51c3b0); the 4 team/opponent victory variants +
the 4-arg Display were appended later, so **serialize the INTERNAL opcode**.
String-param defaults: unit type="Any Unit", player="All Players",
location="Anywhere"; text string = literal 256-byte string; flag = int index.
Full 26+26 template list captured in the RE task output (session d39a8c26,
task ac01eaf629a4e233d).

### Engine: `.crt` scenario RUNNER is now in the sim (DONE, kNetVersion 36)
`ta::crt::parse` reads the full record; the engine's scenario branch spawns the
placed units at the corrected cells, APPLYING each unit's health% and veteran,
and honouring its facing angle. The 26+26 trigger opcodes now run in the sim:
`src/sim/scenario.{h,cpp}` (`ScenarioScript`) mirrors `MissionScript` — built on
every peer, ticked in `World::tick`, folded into `stateHash`, so flag/timer
state, spawns, and win/lose stay in lockstep. Rules are EDGE-triggered (fire
once on the rising edge of "all conditions true", the retail model); conditions
cover gametime/timers/flags/control-counts-in-region/opponents-left/resources/
kills+losses/random, actions cover flags/timers/create/destroy/own/heal/damage/
move/resources/victory+defeat(me|teammates|opponents)/display. Victory/Defeat
force per-player defeat via `World::forceDefeat` (respected by `updateOutcome`,
hashed); "Display" actions surface as HUD notices for the viewing player.
Verified: deterministic (stateHash reproduces + cross-build golden), edge-timing
correct (a "set countdown timer to N" fires once then counts down), victory
defeats opponents. `src/sim/mission.cpp` remains the separate CAMPAIGN runner.

The old client-side heuristic trigger code was removed (the superseded
`loadTriggers`/`Triggers`/`TrigRecord`/`Placement`/`load` in ta::crt, the
gameview spawn-rule/scoring/message machinery, and the `--hilltest` harness);
ta::crt is now just the typed `parse`/`write`. Minor remaining: "Move all"
uses a plain move order; kill/loss credit is by last-hitter.

### Command ID -> handler VAs (for follow-up RE)
ScenProps 0x401890 · UseOnly 0x401910 · zoom 0x401990/a10/a90/b10 · 12.5%
0x402ed0 · ZoomIn 0x402460 · ZoomOut 0x4024e0 · Resize 0x402560 · LandLasso
0x402700 · Scripting 0x4027a0 · Triggers-view 0x4028b0 · Grid 0x402910 ·
ClearArea 0x402f50 · CheckMap 0x403030 · Unit Del/Props 0x40b5c0/0x40b690 ·
Feature Del 0x40b460 · StartPos Del 0x40b750 · Trigger Del/Props
0x40bfa0/0x40bfe0. Writers: TNT 0x41ba70 · OTA 0x41bfd0 · UseOnly 0x41c400 ·
TXT 0x41c4e0 · CRT 0x40d8d0 · bundle 0x418b80. GetCell 0x419120 · SectionStamp
0x41d2d0 · SetZoom 0x419790.

## RE status
- Resource map + command handlers + tool behaviours + trigger opcode tables +
  TNT/OTA write formats + coordinate model + world types: **DONE**.
- `.crt` full binary layout (UnitRecord 568B, Rule 324B, CustomType 272B,
  RegionDef 272B, per-player group structure): **DONE** — proven byte-exact,
  implemented as `ta::crt::parse`/`write` in the shared lib, round-trip tested
  by `tnttool crt` across all 27 shipped scenario maps.
- Pending deep RE (for later phases): minimap-generation exact downsample;
  HPI/.kmp bundle writer; whether the loader consumes UnitRecord 0x224/0x228
  (type cache) or re-derives from objectName (almost certainly the latter).
