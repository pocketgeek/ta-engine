# TotalA.exe fog of war, line of sight and radar, and how the engine differs

Scope: Total Annihilation 3.1c (`TotalA.exe`, GOG Commander Pack, base 0x400000).
All addresses are **TotalA.exe** unless marked otherwise. The engine's fog code
(`src/client/retailfog.h`, `src/sim/retailexploration.h`) cites KINGDOMS.icd
addresses (0x4c6800, 0x498270, 0x498b60, 0x425ef0, ...). Those describe
Kingdoms, not TA. This audit found that several of those behaviours are not in
TA. Behaviour was observed only; nothing from the binary is copied into engine
source.

Evidence tags: **[S]** read from the disassembly, **[I]** inferred from the
code around it or from data, **[U]** unknown or not traced. Nothing here was
run under unicorn. §8 lists the formulas worth checking that way.

Notation: `G` = `[0x511de8]`. `P(i)` = the player record `G+0x1b63+i*0x14b`.
Its fields are: `+0x7c` view-count buffer pointer, `+0x80`/`+0x84` its
width/height, `+0x88` its size, `+0x146` the player index, `+0x108[j]` the
alliance flags, `+0x27` the info record (option word `+0x97`/`+0x9b`). `V` =
`G+0x2a43`, the **viewed** player. Every display path below keys on `V`, not
on the local player `G+0x2a42`.

## 1. State

| Where | What | Notes |
|---|---|---|
| `G+0x14281` word | bit 0 **Mapping** (shroud on), bit 1 **LOS on**, bit 2 **True LOS** (clear = Circular), bit 3 fog overlay grid valid | [S] Bits 0-2 are set at game start (§6). Bit 3 is cleared whenever the viewed player's view or explored state changes (0x481d2d, 0x481d73, 0x482293, 0x481911). |
| `G+0x14273` | **explored** plane: one u16 per 32-px cell, bit *i* = player *i* has seen it | [S] Allocated as "MAPPED MEMORY", size `W16*H16/2` bytes (0x483cf6). The plane is W16/2 × H16/2. |
| `P(i)+0x7c` | **view counts**: one byte per 32-px cell, the number of player *i*'s sight stamps covering it | [S] There is one plane per player. Stamps add one (0x482270) or remove one (0x481d50). |
| `G+0x1428f` {ptr,w,h,n} | **True-LOS height plane**: two bytes per 32-px *projected* cell, `{hi, lo}` | [S] Built by 0x482c20 from the 16-px height bytes: per cell max/min of the projected heights, then `hi'=(lo+2hi)/3`, `lo'=(hi+2lo)/3`, both at least sea level (0x4831a6..0x483205). The engine's `retailExplorationHeights` (`src/sim/retailexploration.h:31`) already computes this. |
| `G+0x1427b`, count `G+0x14277` | "EYEBALL MEMORY": up to **20** temporary sight stamps of 0x24 bytes | [S] 0x483d8d, 0x482926. |
| `G+0x1485b` | `anims/vismasks.gaf` sequence `vismask`: **10 frames**, 11×11 … 29×29, transparent index 9 | [S] Loaded at 0x429b9e. These are the Circular sight masks (§2.2). |
| `0x51e6a0` | ray tables from `gamedata/los.tdf` (`[TABLEINFO] numtables`, `[TABLEn] numlines, lineK=count, dx,dz, …`) | [S] Loaded by 0x433130 (strings "los", "TABLEINFO", "numtables"). The shipped file says `numtables=9` and defines TABLE1..TABLE12. |
| `G+0x1485f..0x1486b` / `G+0x1486f..0x1487b` | `anims/fog.gaf` sequences **Black1..Black4** and **Gray1..Gray4**, 14 frames each | [S] Loaded at 0x429bcb..0x429c61. Each frame is a mask: palette 0 = fog pixel, 9 = transparent (§3). |
| unit `+0x110` bits 8 / 9 / 10 | relative to `V`: **detected** (radar, or centre in LOS) / **sonar contact** / jammed | [S] Recomputed by 0x467440 (§4.2). |
| unit `+0x7a` (x,z i16), `+0xf8` (u8) | the cached footprint: its corner cell, and the mask index (Circular) or eye height (True) | [S] |
| display `[0x51fbd0]+0xcc` | **GRAY TABLE**, 256 bytes | [S] Allocated at 0x4ba6b0 and built by 0x4bad30: for each palette entry, the nearest palette colour to the grey `(r+g+b)/3`. It exists only when display flag `+0xf1` bit 0 is set. |
| `G+0xdcb[k]` | guipal→palette.pal colour map (retail-data.md) | Entry 0 = black (unexplored fill). 0xa = green and 0xc = red (minimap radar and jammer rings). 0xe = yellow (minimap shot dots). |

## 2. How sight is stamped

### 2.1 When [S]
- **Rebuild, 0x4816a0(resetExplored).** With the argument set, every explored
  word is set to 0 if Mapping is on, else to 0xffff. Every live player's
  (`+0x73` ∈ 1..3, `+0x146 ≠ 10`) view-count plane is then filled with
  `LOS on ? 0 : 1`. Then every unit with `+0xa6 ≠ 0` is stamped from scratch
  (0x481796..0x4818ff). Last, it calls the minimap rebuilds 0x466c20 and
  0x466dc0. Callers: +LOSType, +LOS, +Mapping, +NowISee, game start
  (0x497ae5), watch mode (0x4656ce), MakePoster (0x495b61 / 0x495e58).
- **Unit created**: 0x482ac0 (0x486178, 0x486312). **Unit moved**: 0x4827b0,
  called from the movers (0x43da59, 0x464dbf, 0x465053, 0x48aaa0, 0x48b6bf).
  It goes to 0x4825b0, which re-stamps only when something changed (below).
- **Unit died**: 0x482090 unstamps (0x486845, only when LOS is on). In the kill
  packet handler 0x4866d0, if the dead unit belongs to `V`, 0x482910 adds a
  **60-tick** "eyeball" at its position with its sight and height. The eyeball
  is added only when LOS is on and fewer than 20 exist, and it always belongs
  to `V`. 0x482130 expires eyeballs whose tick `+0x1c` < `G+0x38a47`.
- The eye height is clamped to at least `(sea+1)<<16` (0x4817f8 and the same
  code in each wrapper).
- A footprint stamps only its **owner's** plane (info `+0` = the unit's `+0x96`
  player). No ally sharing was found in the stamp path (see §7, item 11).

### 2.2 Circular (LOS on, True off) [S]
- Mask index `k = clamp(sight/32 − 5, 0, frames−1)`, with sight = type `+0x202`
  (0x481840..0x481868). The frame is `vismask[k]`, 11+2k cells square, so the
  radius is **5+k cells = sight/32 cells, clamped to 5..14** (160..448 px).
  There is **no terrain test**: a cell is stamped when its mask byte is not
  the frame's transparent byte (0x482580 / 0x481cc5).
- Centre cell = `(x>>21, (z>>21) − y_px/64)`, i.e. the unit's position
  **projected** (z − y/2) in 32-px cells. The frame's xoff/yoff are subtracted
  to get the corner (0x48186b..0x4818d1).
- A re-stamp happens only when the corner cell or the mask index changes
  (0x48272e..0x482746).

### 2.3 True (LOS on, True on) [S]
- Eye height `e = clamp(y_px + modelHeight, 0, 255)`. modelHeight is the low
  byte of type `+0x170` (the integer part of `+0x16e`, the model height).
- Centre cell = `(x_px>>5, (z_px − e/2)>>5)` (0x4825d4..0x48261c). The re-stamp
  rule is: same cell and |Δe| ≤ 5 → keep (0x48261f..0x48262e). Off the plane →
  no stamp, and the eye byte is set to 0.
- Ray table: `r = clamp(sight>>5, 0, numtables−1)`; table `r` is used, since
  0x433500 returns element r−1 (0x481e09..0x481e52). With the shipped
  `numtables=9` the radius is capped at **8 cells (256 px)** [S for the clamp;
  [I] that the loader keeps exactly `numtables` tables and mirrors each
  octant's lines]. sight < 32 indexes element −1 [U].
- Each ray walks its cells in order, starting from the centre. Let `j` be the
  1-based step, and track the steepest horizon `best = (h, d)`, starting at
  `(−1, 0)`. A cell is stamped when `(hi − e)·d > h·j`, i.e. the top of the cell
  rises above the horizon. If `(lo − e)·d > h·j`, the horizon becomes
  `(lo − e, j)` (0x4823fc..0x4824b5). Cells off the plane are skipped.
- Exploration (0x481930) walks the same rays and the same masks (§2.4). So in
  True mode hills hide what lies behind them, both from view and from the map.

### 2.4 Explored [S]
0x481930 runs after every stamp when **Mapping** is on (0x482688). It ORs bit
`1<<owner` into every cell the stamp covers. If a new bit was set and the owner
is `V`, it clears `G+0x14281` bit 3 and sets the dirty bit `G+0x142f1` bit 2.
Exploration is never removed.

### 2.5 Which flags change what [S]
| Mapping | LOS | Explored plane | View counts | Result on screen |
|---|---|---|---|---|
| on | on | stamped | stamped | black / grey / clear (standard) |
| on | off | stamped | all 1 | "Permanent": black until explored, then always in view |
| off | on | all set | stamped | no black; grey out of LOS |
| off | off | all set | all 1 | everything visible (NowISee) |

When LOS is off, the unit, feature and shot tests read the explored bit of `V`
instead of the counts: 0x465ac0 → 0x408090, 0x4658e0, 0x49bf2f.

## 3. What the main view draws

### 3.1 The fog overlay: 0x4848e0, called once per frame at 0x469d8e [S]
1. If `G+0x14281` bit 3 is clear, 0x4843c0 rebuilds the window grid and then
   sets bit 3. The grid has one **vertex per 32-px cell centre** over the view:
   the camera is rounded so that vertices fall at cell centres, and the window
   is one cell larger. Each vertex holds two 4-bit masks of which of its 4
   neighbouring cells are dark:
   - byte 1, **grey**: view count 0 while LOS is on (0x4844d7..0x4845a2);
   - byte 0, **black**: the explored bit of `V` is clear (0x4845a5..0x484678).

   At the map edges the masks are copied outward, so the map border does not
   draw a fog edge (0x4846a1..0x4848cf).
2. Each vertex covers a 32×32 screen square, offset 16 px from the cells.
   - black mask 15 → solid fill with `G+0xdcb[0]` (black) (0x484a06).
   - Otherwise, grey mask 1..14 → Gray frame `mask−1`; grey 15 → the whole
     square. Then black 1..14 → Black frame `mask−1`, blitted with
     transparency, so palette 0 draws black (0x484ad8..0x484b0f).
   - The variant is `Black/Gray[(col + row + camera cell sum) & 3]`, which is
     anchored to the world. The four shapes per frame index differ slightly, so
     edges do not repeat.
3. How grey is drawn depends on **DitheredFog**: `G+0x37f06` bit 6, registry
   "DitheredFog", toggled by `+Dither` (0x416590).
   - **Dithered**: a black checkerboard over the area, i.e. every other pixel
     set to palette 0. The parity is `(x + y + camX + camY) & 1` (0x4bff20,
     0x4b88d0).
   - **Not dithered**: the pixels are remapped through the GRAY TABLE
     (0x4bfe10 → 0x4cced5; 0x4b86e0 for masked frames), giving
     **desaturated, not darkened** terrain. This needs display flag `+0xf1`
     bit 0. Without it the square is left untouched [I].
4. There is no animation, wave, tint or alpha in either mode.
5. Order: the profiler slots are "Render Static", "Render Stuff", "Render Fog",
   "SFX", "Weapon", "Misc" (0x46a35d..0x46a3b2). So the fog is drawn over
   terrain, units and features, i.e. over anything that has passed its own
   visibility test [I for the exact draw order of SFX and shots relative to
   the fog].

### 3.2 Units: 0x465ac0(player, unit) [S]
- Own unit → visible.
- Cloaked (`+0x10e` bit 2) → not visible.
- **Submerged**: if the unit is not a sonar contact (`+0x110` bit 9 clear) and
  its top (`y + type+0x16e`) is below sea level, it is not visible.
- Otherwise it is visible if any of the 4 footprint-box corners lies on a cell
  in view, testing each corner's (x, z − y/2) cell. The corners are
  (x±fx·8, top, z−fz·8) and (x±fx·8, y, z+fz·8). The engine's `playerSees`
  uses the same corners.
- Callers: hover text (0x46af3d), box select (0x48bc56), right-click info
  (0x494396), auto-targeting (0x40ab11), and an order's last-seen position
  (0x439761).
- Radar contacts are **not drawn on the main view**. The only readers of bits
  8/9 are the minimap (0x466e6a), the hover text (0x46b6fe), targeting
  (0x40ab45) and the submerged test. Hovering a radar contact gives
  **"R: Unidentified object"**, or "S: …" for a sonar contact (0x46b6f3) [I:
  the pick probably comes from the minimap].

### 3.3 Features: 0x4658e0, called from the feature pass 0x46990f / 0x469a9c [S]
- Only features whose def `+0xff` bit 3 is set — `nodrawundergray`, flags word
  `+0xfe` bit 11 (dragon's teeth, fortifications) — are tested. They are drawn
  only when either footprint corner (x,z) or (x+fx·16, z+fz·16), projected by
  the feature's height, lies in view. A cell whose owner field (`+0xc` bits
  3-6) is the viewer skips the test [I].
- **Every other feature is drawn under grey**, in its current state: trees,
  rocks, wrecks and corpses. Under black it is hidden only by the black fill.
- An unexplored cell's feature cannot be targeted by the cursor (0x43e6cd).

### 3.4 Shots: 0x49be60 [S]
A projectile is drawn only if its (x, z − y/2) cell is in view (or explored
with LOS off). Explosions, smoke and sounds were not traced [U].
docs/retail-music.md: the battle music has no view test.

## 4. Radar, sonar, jammers

### 4.1 Per-frame flags: 0x467440, only when `G+0x2a3c` ≥ 2 [S]
1. Every live unit: clear bit 12.
   - Own (`+0xff == V`) → set bits 8|9.
   - Allied with `V` **and** that ally's info `+0x97` bit 6 (**ShareRadar**) is
     set → 8|9.
   - `V`'s info `+0x9b` bit 6 (watching) → 8|9.
   - Otherwise clear bits 8|9|10.
2. **Radar and sonar**: each unit of `V` (from the `P(V)` unit range) that is
   live, without bit 14, **activated** (`+0x10e` bit 0), and has radar or
   sonar runs 0x47e890 with callback 0x467840. The radar radius is
   `radardistance + 2·y_px` of the radar unit (**altitude extends radar**);
   the sonar radius is `sonardistance`. The test is `(dx²>>32)+(dz²>>32) < R²`.
   The callback skips `V`'s own units and **stealth** units (type `+0x241`
   bit 8). A target whose `y ≤ sea` and is inside the sonar radius gets bit 9.
   A target whose top `≥ sea` and is inside the radar radius gets bit 8.
3. **Jammers**: each activated unit **not owned by `V`** with
   `radardistancejam` / `sonardistancejam` clears bit 8 / bit 9 and sets bit 10
   on the units in range (0x467960 / 0x467980). This includes allies' jammers.
4. (bit 12: a type `+0x245` bit 13 proximity rule with `+0x208`. It is not a
   vision rule [U].)
5. **LOS**: every unit not yet at bit 8, not cloaked, whose **centre** cell
   (x, z − y/2) is in view of `V` (explored with LOS off) gets bit 8. This
   step runs after jamming, so a jammer never hides a unit in LOS.

### 4.2 What the bits do
Bit 8 or 9 → minimap dot (§5). Bit 9 lets a submerged unit pass 0x465ac0, so a
submerged unit is drawn when it is a sonar contact **and** in LOS. The bits
feed the hover text. Targeting reads `(+0x110>>8) & mask` (0x40ab3f) [I].

## 5. Minimap

### 5.1 Terrain: 0x466c20, rebuilt when `G+0x142f1` bit 2 is set [S]
Each minimap pixel maps to a 32-px cell:
- not explored by `V` → `G+0xdcb[0]` (black);
- explored, view count of `V` > 0 → the map picture's pixel;
- explored, out of view → that pixel through the **GRAY TABLE**.

The minimap is never dithered and has no edge pieces.

### 5.2 Dots: 0x466dc0 [S]
- Show-all happens when both Mapping and LOS are off, or with the **+Radar**
  cheat (`G+0x37f2f` bit 9, 0x466e23). Otherwise a unit is shown if it has bit
  8 or 9, or belongs to `V`.
- Position: `(x·mmW/mapW, (z − y/2)·mmH/mapH)`.
- Every dot is a frame of **`radlogo`** from `logos.gaf`, chosen by the owner's
  colour (`G+0x147df`, info `+0x96`). The unit `G+0x2cba` gets **`radlogohigh`**
  (`G+0x147e3`) [U: which unit that is]. A unit with `+0xfa ≠ 0` blinks with
  `G+0x142f1` bit 0.
- A selected unit (`+0x110` bit 4) draws its radar and sonar circles in
  `G+0xdcb[0xa]` (green) and its jammer circles in `[0xc]` (red), 0x4c0070.
  An onoffable unit draws them only while activated. An `antiweapons` type
  draws its weapon coverage (0x46707c..).
- **Shots**: each projectile whose centre cell is in view, or that belongs to
  `V`, is a pixel of `G+0xdcb[0xe]` (yellow). Weapons with `+0x111` bits 29-30
  instead draw **`nuclogo`** in the owner's colour. `noradar` weapons (bit 6)
  are never drawn (0x4671a0..0x46742f).

## 6. Options and cheats [S]
- Game start, 0x497300..0x4974d7:
  - multiplayer: from the host's option byte `+0x9c` (bit 0 Mapping, bit 1
    LOS, bit 2 True).
  - skirmish (`G+0x29a0` record, `+0x108..`) and campaign (`G+0x39219`
    record): bit 2 = LOSType&1, bit 0 = Mapping&1, bit 1 = LineOfSight&1.
- The front ends' `LineOfSight Permanent|True|Circular` maps to LineOfSight=0
  → Permanent; otherwise LOSType 1 → True, else Circular (retail-gui.md
  0x47a656).
- **Campaign**: the OTA `mapping` → `G+0x3921d` and `lineofsight` →
  `G+0x39221`, and LOSType is **forced to 1 (True)** (retail-campaign.md,
  0x4363d9). So `mapping=1` means **shroud on**, not "pre-mapped". 231 of the
  275 shipped OTAs say `mapping=1; lineofsight=1`, and the "00" ones are fully
  revealed.
- `+LOSType` (0x416690) is in the **always-available group**. It toggles bit 2
  and rebuilds.
- `+LOS` and `+Mapping` toggle bits 1 and 0, then save the settings (0x430f00)
  and rebuild. +Mapping rebuilds with reset=1. +LOS rebuilds with reset=0, so
  the explored plane is kept.
- `+NowISee` clears both bits.
- `+Radar` only affects the minimap.
- `+View n` moves `V`, so the fog, explored state, radar and minimap all switch
  to player n. A rebuild is not forced [U: whether the grid refreshes at once].
- `+Dither` sets the grey style.
- `+ShareMapping` / `+ShareRadar` toggle info `+0x97` bits 5 / 6. Bit 6 is read
  by §4.1. **No reader of bit 5 was found** [U].
- Watch mode ("You're out! Continue watching?") sets info `+0x9b` bit 6, which
  shows all units on the minimap.

## 7. Differences from the engine (most visible first)

**Status (2026-11-27).** Fixed:

- **1, 2:** `client/fogremap.h` adds the grey table, DitheredFog (`TA_FOG_DITHER`) and the fog.gaf edges. Without OpenGL the overlay stays still and black.
- **3:** campaign `mapping` / `lineofsight`.
- **4 (Circular only), 6:** vismasks.gaf stamps at the projected centre (`sim/retailexploration.h`).
- **7:** the minimap uses the grey table.
- **12:** `nodrawundergray`.

Open: 4 (True LOS rays), 5, 8, 9, 10, 11, 13, 14 and 15.

1. **The out-of-view look.** Retail desaturates out-of-view terrain through the
   GRAY TABLE, or covers it with a 50% black checkerboard (DitheredFog), with no
   motion. The engine draws a white (FogColor) mist at alpha 8/15
   (`retailfog.h:209` `0x8f`). That mist's brightness rolls in time and space,
   and its vertices move up to 10 px (`retailfog.h:252` `Wave`,
   `gameview_render.cpp:6712` `drawFog`). RollingFog, WavyFog and FogColor are
   Kingdoms features, and TA's sidedata has no `fogcolor`. *Fix:* draw grey
   cells as the palette remap (precompute the nearest grey for each palette
   colour, as 0x4bad30 does; on the GPU, desaturate to the nearest palette
   grey), with an option for the checkerboard. Remove Wave and FogColor in TA
   mode.
2. **Fog edges.** Retail uses the marching-squares pieces from `fog.gaf`
   (Black1-4 and Gray1-4, 14 shapes, vertices at cell centres, variant
   `(i+j+cam)&3`). The engine bilinear-filters a 2× ARGB4444 upsample
   (`retailfog.h:214` `blendTexel`, `writeTexels`). *Fix:* build the two 4-bit
   vertex masks per cell centre and composite the GAF masks.
3. **Campaign vision is inverted.** `matchsetup.cpp:1615` and `:1785` treat
   `mapping!=0` as revealed. In retail `mapping=1` is shroud on. So nearly
   every mission starts fully mapped in the engine, while retail starts black.
   `lineofsight=0` is Permanent LOS (LOS off). It reveals the map only together
   with `mapping=0`, and the shipped "00" missions have both. Campaign LOS is
   always True. *Fix:* `mapRevealed = mapping==0`, `losOff = lineofsight==0`,
   and the LOS type = True.
4. **Sight radius and shape.** The engine radius is `distance*2/32` cells, so
   it reaches **twice** the FBI sightdistance. It also uses a Kingdoms height
   cone (`retailexploration.h:80`, `:90`). Retail Circular is the vismasks.gaf
   disc with radius `clamp(sight/32, 5, 14)` cells and no terrain. Retail True
   is the los.tdf rays with the horizon test, radius `min(sight/32, 8)`.
   *Fix:* implement both modes and pick by LOSType.
5. **No True/Circular choice.** The engine knows only Mapping and LOS
   (`setRoomVision`). The LOSType option and the always-available `+LOSType`
   are missing. *Fix:* add a hashed LOS-type flag (it is sim state: the counts
   feed `playerSees`).
6. **Sight centre is not projected.** The engine uses `cz = z/32`
   (`retailexploration.h:128`, `:149`). Retail uses `(z − y/2)/32` (Circular)
   or `(z − e/2)/32` (True). On high ground the engine's disc sits several
   cells south of the unit. *Fix:* project the centre.
7. **Minimap fog.** The engine draws explored-out-of-view as 50% black
   (`fogupload.h:70`). Retail uses the same grey remap as the main view, over
   solid black unexplored. *Fix:* reuse the grey LUT.
8. **Minimap dots and markers.** The engine draws 3×3 quads in the player's
   colour (`gameview_hud.cpp:617`). Retail uses the `radlogo` frames, the
   `radlogohigh` highlight, `nuclogo` for nukes, yellow pixels for shots in
   view, and radar/sonar/jammer rings for selected units. *Fix:* use the
   logos.gaf art and add the shot and ring passes.
9. **Radar rules.** The engine's `RadarCover` (`retailfog.h`, filled at
   `gameview_impl.cpp:1717`) uses the local player, not the viewed one. It has
   no altitude bonus, `<=` instead of `<`, no sonar, no jammers, no stealth, no
   activation check and no ShareRadar. Radar reaches submerged units. *Fix:*
   port §4.1 as a display pass keyed on the viewed player.
10. **Submerged units.** Retail hides an enemy whose top is under the sea unless
    it is a sonar contact, even inside LOS. The engine's `unitInView` and
    `playerSees` (`sim.cpp:16012`) have no such rule. *Fix:* add the sea test
    gated on a sonar contact. Note that `playerSees` is used by the sim, so the
    change is hashed.
11. **Allied sight.** The engine shares view and exploration with every ally
    (`viewShareMask`, `sim.cpp:15889`; owner decision D2). Retail stamps each
    player's own plane only and displays `V`'s plane alone. How allies share in
    retail multiplayer (ShareMapping, info `+0x97` bit 5) is **not traced**
    [U]. Keep D2 unless that path is found.
12. **`nodrawundergray` features.** The engine draws every explored feature
    (`gameview_render.cpp:239`). Retail hides `nodrawundergray` features
    (dragon's teeth, fortifications) out of LOS. *Fix:* test LOS for those
    features.
13. **Radar-contact hover.** Retail shows "R:/S: Unidentified object". The
    engine has no such text.
14. **Draw order.** The engine draws the fog last, over effects and aircraft
    (`gameview_render.cpp:2515`). Retail appears to draw the fog before SFX and
    shots [I]. This matters little, since those are filtered by visibility.
15. **Nanoframes.** The engine skips sight for units under construction. In
    retail, 0x482ac0 runs at creation, so a nanoframe probably sees [I].

These already match retail: the projected 32-px plane and the corner test of
`playerSees`; the True-LOS height plane `{(l+2h)/3, (h+2l)/3}`; the 60-tick,
at-most-20 death eyeballs for the viewed player; the minimap dot position at
z − y/2; the minimap-only `+Radar`; and the |Δeye| ≤ 5 re-stamp rule.

## 8. Not determined
- Explosion, smoke and sound visibility (§3.4).
- Retail ally LOS sharing and ShareMapping.
- What `G+0x2cba` is (the radlogohigh unit).
- How los.tdf lines are mirrored, and whether TABLE10..12 are ever read.
  Unicorn-checking 0x433130 plus one 0x482270 call would settle the True
  radius.
- The exact draw order of the fog against SFX and shots.
- The `+0x110` bit-12 proximity rule.
- Whether the GRAY TABLE is ever replaced (0x4bab60 has no static caller).
