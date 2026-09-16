# Porting the TAK engine to Total Annihilation

This repo is a fork of [tak-engine](https://github.com/pocketgeek/tak-engine) at
`v0.6.7`, retargeted from *Total Annihilation: Kingdoms* (Cavedog, 1999) to
*Total Annihilation* (Cavedog, 1997) — the **Commander Pack**: base game + *The
Core Contingency* + *Battle Tactics*.

TA is Kingdoms' **predecessor**, and Kingdoms was built on a revision of the same
engine. That is what makes this tractable: most of the asset pipeline is not a
rewrite but a *de-revision* — going back to the older, usually simpler, version
of a format the loader already understands.

This document is the map of the delta. It is organised by how much work each
subsystem needs, because that ordering is also the build order.

**Status key** — ✅ carries over as-is · 🟡 small delta · 🟠 substantial rework ·
🔴 rewrite · ⬜ new subsystem with no TAK counterpart.

---

## 1. What carries over untouched

These are the reason the fork is worth doing. None of it is Kingdoms-specific.

| Subsystem | Why it survives |
| --- | --- |
| ✅ `src/net/`, `src/server/` | Wire format, framed TCP, SCRAM-SHA-256 accounts, lockstep relay, referee, reconnect, spectate, replays. Game-agnostic. |
| ✅ `src/sim/detmath` | The determinism shim and its cross-build golden-hash gate. Contract is unchanged. |
| ✅ `src/sim/pathsearch` | A*, the nav grid, the bounded search scheduler, incremental relabelling. TA's movement classes plug into the same `NavGrid`. |
| ✅ `src/util/`, `src/gui/`, `src/video/` | Helpers, `.gui` gadget layout (TA uses the same format), Bink decode for menu clips. |
| ✅ `src/tdo/` | **3DO is already classic-TA version 1** — *verified*: `modeltool` reads `ARMCOM.3DO` with its full piece tree (pelvis → torso → biggun / nanolath / head) and 25 textures. Zero work. |
| ✅ `src/gaf/` | Already decodes GAF encodings 0 (raw) and 1 (RLE) — the classic-TA pair. *Verified*: `gaftool` lists `ARMINT.GAF` against the retail `PALETTE.PAL`. TAF's 4/5 become dead code to delete, not port. |
| ✅ `src/tdf/` | TDF/FBI/OTA is the same line-oriented `[section]{key=value;}` text format in both games. *Verified* on `SIDEDATA.TDF`, `ARMCOM.FBI` and map `.ota`s. |
| ✅ CI + test harness | All four workflows, the 17 ctest targets, the static-link gates, the 7-distro packaging matrix. Renamed, not redesigned. |

## 2. Asset pipeline — the delta

### 🟡 HPI (`src/hpi/`)

TA ships **HPI version 1**; `Archive` currently hard-rejects anything but v2 at
`hpi.cpp:161`. The v1 format is already *named* in `describe()`, just not read.

What v1 adds over v2:

- A different header: `{ "HAPI", u32 version, u32 dirSize, u32 headerKey, u32 start }`.
- **Whole-archive obfuscation.** When `headerKey != 0`, the key is
  `~((headerKey * 4) | (headerKey >> 6))` and every byte is unmasked against its
  own file offset. v2 has nothing equivalent.
- **Compression method 1 = LZ77**, alongside the zlib method 2 that v2 uses. The
  `SQSH` chunk header itself is shared, so this is one new decompressor behind an
  existing switch.
- A flat `{nameOffset, dataOffset, flag}` directory rather than v2's separate
  name/dir blocks.

TA also spreads its data across **four extensions** — `.hpi` (base game),
`.ccx` (**both** expansions: `ccdata.ccx`/`ccmaps.ccx`/`ccmiss.ccx` for Core
Contingency, `btdata.ccx`/`btmaps.ccx` for Battle Tactics), `.gp3` (the 3.1
patch, `rev31.gp3`) and `.ufo` (mods) — all the same container, mounted in that
order so a later group overrides an earlier one.

#### The tie-break, and why it silently disabled both expansions

Slotting the new extensions into the existing precedence chain was **not**
enough, and the reason is worth recording because the failure was completely
invisible.

The chain resolved a same-path collision by newest entry date, with a strict
`>` so that a tie kept the FIRST-mounted archive. That rule was inherited from
Kingdoms, and **it cannot decide anything on TA data**: an HPI v1 file record is
nine bytes — offset, size, compression — and carries *no timestamp at all*. So
every TA entry's date is 0, every collision is a tie, every tie went to the
first mount, and the first group is `.hpi`. The base game therefore beat
`ccdata.ccx`, `btdata.ccx` and the 3.1 patch `rev31.gp3` on every path all of
them ship.

Measured on a Commander Pack install, that shadowed **1082 files whose
expansion/patch copy differs from the base** (a further 626 were byte-identical,
and 2430 expansion files were new paths and so came through fine):

| dir | shadowed | dir | shadowed |
|---|---|---|---|
| `units` | 471 | `textures` | 30 |
| `anims` | 329 | `scripts` | 24 |
| `guis` | 105 | `features` | 22 |
| `unitpics` | 41 | `gamedata` | 15 |
| `weapons` | 15 | `ai` | 12 |

Three measurements make the consequences concrete, and each is a thing a player
would notice:

* `gamedata/sidedata.tdf` goes from **283 `canbuild` lines in the base to 474**
  in all three expansion archives. The Commander's own menu goes from 12 entries
  to 19 — the extra seven are its naval and underwater construction. The
  expansion units themselves loaded fine (their FBIs are new paths, not
  collisions), but **every builder's menu was the pre-expansion one, so much of
  the added roster could not actually be built**.
* `ARM_DISINTEGRATOR` (the D-gun) is `default=5500` in the base and **`30000` in
  all three expansion archives**. The engine was firing the base number.
* Base `features/lava/VENTS.TDF` declares `[Lavavent003]` with
  `seqname=tvent03`, **a sequence that does not exist** in `anims/lavastuff2.gaf`
  (which has `tvent001`..`tvent010`); all three expansion archives carry
  `seqname=tvent003`, which does. This one is decisive on its own: retail draws
  that vent, so retail is not resolving the collision in the base's favour, and
  a patch that lost every conflict to the file it patches would do nothing.

The fix is one character — `>` becomes `>=`, so a tie goes to the LATER mount —
but it only became findable because the whole-table feature-art audit (§5c) put
a number on something that had always been reported as fine. It is safe inside
the base group too: across all thirteen root `.hpi` archives exactly **one** path
collides with differing content, `installres/install.inf`, an installer leftover.

An archive that does carry dates (HPI v2) is unaffected — the date still
dominates and only the tie changed.

With the tie flipped, the *group order* now decides every collision, which makes
the one unverified assumption in that order worth bounding. The rank of `.ccx`
against `.gp3` comes from the community modding record, not from `TotalA.exe`.
Measured, its reach is small: `btdata.ccx` and `ccdata.ccx` disagree on 112
paths, but `rev31.gp3` — mounted last on either reading — ships 87 of them and
therefore decides those regardless. The remaining **25 are all in `anims/`**:
cosmetic GAF art, no gameplay data. So on a full Commander Pack install this
assumption cannot change what the simulation computes; confirming it against the
binary would only settle which of two art variants is drawn.

#### Water-only structures, and what the AI harness did and did not show

Letting the expansion menus through exposed a second defect. 32 TA structures
declare their own `MinWaterDepth`; 24 of those are shoreline buildings whose
yardmap carries `w` slipway cells, which `canPlace` already has a branch for.
The other **8 are fully submerged** — both sides' underwater metal extractor,
energy store, metal store and fusion plant — and carry a plain `ooooooooo`
yardmap, so they fell through to the generic footprint test. That test uses
`navFor(type)`, and a structure has no `movementclass`, so they were checked
against the GROUND grid: refused over water (not walkable) and refused on land
(they are water buildings). **They could not be built anywhere, on any map.**
The fix reads the structure's own `MinWaterDepth` and gives it
`Domain::Water`, which is all `navFor` needs; `canPlace` is untouched.

The AI harness is worth a word of caution here, because it misled this work
once. Under `--mpai` the `p0-*` figures in the `mp-headless` line belong to the
**idle human slot**, not to the AI — so "p0 built nothing" is the expected
result, not a regression, and only the total `units=` says anything about AI
behaviour. Measured on Coast To Coast at 60 s over seeds 1/2/3:

| build | s1 | s2 | s3 |
|---|---|---|---|
| before the HPI and water fixes | 6 | 6 | 6 |
| with both | 5 | 4 | 6 |

Those totals include p0's lone commander. The spreads overlap and the gap is
inside the seed noise §5a documents, so this says only that nothing broke — it
is NOT evidence either way about whether the expansion roster helps or hurts the
build-up. A longer horizon would be needed for that; the 300 s runs attempted
here were repeatedly invalidated by concurrent harness processes sharing a port
(`err=peer closed` partway through), so they are not reported.

**This is why `unitdata_test` now branches on whether `ccdata.ccx` is present.**
Its numbers were honestly measured, but measured from the base archive, which is
what the engine was serving; asserting them unconditionally is part of what let
the bug stand. It now expects the base values on a base install and the
expansion values on a Commander Pack.

### 🟡 COB (`src/cob/`)

**Already accepts version 4** — TA's version — next to TAK's 6 (`cob.cpp:44`),
and the opcode set is shared. *Verified*: `cobtool` reads `ARMCOM.COB` — 20
scripts (`Create`, `StartMoving`, `QueryNanoPiece`, `AimFromPrimary`,
`StartBuilding`, …), 14 pieces, 1821 code words. Downgraded from 🟡 to ✅ on the
evidence; expect at most TA-only opcode gaps found by running scripts, not a port.

### 🔴 TNT + terrain (`src/tnt/`, `src/terrain/`)

The one genuine rewrite. The two games' map formats share a name and nothing else:

Header confirmed against `maps/Coast to Coast.tnt`: version `0x2000`, 210×126
cells.

| | TAK (0x4000) | TA (0x2000) |
| --- | --- | --- |
| Terrain source | content-addressed JPG sections in `terrain.hpi`, keyed per 32px block | a **tile library inside the `.tnt`** — 32×32 8-bit indexed tiles, indexed per 16px cell |
| Height | one byte per cell | packed into a per-cell `MapAttr` record alongside special flags |
| Palette | JPG, truecolor | 8-bit indexed through the game palette |
| Extras | minimap + overview | minimap, tile-anim (`TileAnim`) table, sea level |

#### The TA header, decoded

Read out of `maps/Coast To Coast.tnt` and cross-checked so that every offset in
the file is accounted for — 16 little-endian u32 words, then the planes:

| # | Field | Value here | Checks out as |
| --- | --- | --- | --- |
| 0 | version | `0x2000` | |
| 1-2 | width, height | 210 × 126 | in 16px cells |
| 3 | `mapDataOffset` | 64 | u16 tile index per **32px** tile, (w/2)×(h/2) = 105×63 |
| 4 | `mapAttrOffset` | 13296 | 4 bytes per 16px cell — 105840 / 26460 = **exactly 4** |
| 5 | `tileGfxOffset` | 119136 | `numTiles` × 1024 (32×32, 8-bit indexed) — **exactly** 1292 × 1024 |
| 6 | `numTiles` | 1292 | |
| 7 | `numTileAnims` | 19 | |
| 8 | `tileAnimOffset` | 1442144 | 132 bytes each — **exactly** 19 × 132. Entry 0 is `ArchMetal3`. |
| 9 | `seaLevel` | 85 | heights run 0–195 |
| 10 | `minimapOffset` | 1444652 | `{u32 w, u32 h, w*h}` = 252×252 — **exactly** the file tail |
| 11 | ? | 1 | |
| 12-15 | pad | 0 | |

`MapAttr`, per 16px cell, is `{ u8 height; u16 feature; u8 unused }`:

- `height` 0–195 against a sea level of 85.
- `feature` is a u16 index with `0xFFFF` = none (26260 of 26460 cells here).
  `0xFFFE` occurs 80 times and is a sentinel of some kind — note this is a value
  the *Kingdoms* loader's comment says never appears in a file, so the two games
  use the spare range differently.
- The fourth byte is 0 across every cell of this map, so its meaning is still open.

**Where the metal map lives — answered.** There is no separate metal plane, and
there does not need to be: word 8's table is the FEATURE name table, and metal
patches are *features*. Coast To Coast's 19 names are `ArchMetal1/2/3`,
`Palm01-06` and `Frond01-07`, and its 10 `ArchMetal` placements are each a 3x3
block — one anchor cell holding the index, the other eight holding `0xFFFE`.

So an extractor's yield is a property of the feature under its footprint, not of
a density field. What the `.ota` schema's `SurfaceMetal`/`MohoMetal` contribute
on top of that is still open, and is a question for dynamic analysis.

`terrain::Compositor` is written entirely against the JPG path and gets replaced.
`tnt::Map`'s *consumers* (14 files) mostly touch `heights` / `features` /
`width` / `height`, so keeping that surface stable contains the blast radius to
the loader and the compositor.

`src/tnt/mapgen.cpp` (the procedural generator) is ported but **not faithful**:
it synthesizes a small dithered tile library from palette ramps, because a TA map
must bring its own pixels and there is no shared art to point at. Drawing real
tiles from `worlds.hpi` is outstanding. Its Kingdoms coastline-prefab pass is
deleted rather than left to resolve nothing.

`cartographer`'s stamp brush is ported and now interns tiles by content (a tile
index means nothing outside the map that owns the library). Its section palette
reads TA's prefabs, which live in `worlds.hpi` under the same
`sections/<World>/<Category>/` layout but in a `.sct` container rather than a
TNT — decoded in `src/sct/` (169 sections for Archipelago, 837 shipped in all).

`.sct` is a close relative of the TNT: the same 32px tile library, the same
per-16px-cell attribute plane, plus a fixed 128×128 palette thumbnail. Two
versions ship and they are **not** the same layout with a different number —
version 2 (607 files) puts the tile graphics straight after the header with the
cell planes behind them, version 3 (230 files) puts the planes first and moves
the graphics to the back. Both headers are pointer-led, so reading one with the
other's field meaning yields plausible in-range offsets rather than an error.

Two things about it were worth establishing empirically rather than assuming:

- **The attribute plane is flat and row-major over the whole section**, not
  grouped per tile. Per-tile grouping is exactly the same size in bytes, so the
  file's shape cannot distinguish them; what does is that the flat reading yields
  terrain with a little under half the total height variation of either per-tile
  ordering, across every shipped section.
- **The thumbnail is always 128×128**, whatever the section's size or aspect — a
  16×48-tile section is drawn into it letterboxed, filling 42 of the 128 columns.

The cell record is 4 bytes in version 3, byte-identical to the TNT's `MapAttr`
(feature `u16` unaligned at byte 1, holding `0xFFFF` throughout, as befits a
prefab that places none). In version 2 it is 8 bytes and **only byte 0, the
height, is determinable**: the other seven hold `01 FF 00 00 00 00 00` in all
664,144 cell records of all 607 v2 sections, so nothing in the shipped data
distinguishes their meaning. They are read as opaque rather than guessed at.

The editor's world list is now read off the `sections/` tree rather than
hardcoded — TA's worlds are Archipelago, GreenWorld, Lava, Mars, Metal and MOON,
where Kingdoms' were the five houses, and a fixed list makes the editor unusable
against the other game and a mod's world invisible.

## 3. Simulation — the delta

### 🟠 Economy: one resource becomes two

Kingdoms has **mana**. TA has **metal and energy**, each with independent
income, drain, and storage cap, and the two are not interchangeable.

The good news is that TAK's economy is already shaped like TA's — the concepts
map one-to-one, they just need doubling:

| TAK | TA |
| --- | --- |
| `Player::mana` / `storage` / `income` | `metal`/`energy` × current/storage/income/drain |
| `UnitType::buildCost` (mana) | `BuildCostMetal` + `BuildCostEnergy` |
| `income` (`mogriumincome`) | `MetalMake` / `EnergyMake` — plus `ExtractsMetal`, `TidalGenerator`, `WindGenerator` |
| `storage` (`mogriumstorage`) | `MetalStorage` / `EnergyStorage` |
| `onMana` — build on a yardmap `'S'` sacred stone | build on **metal**: income scales with the map's metal density under the footprint |
| reclaim yields mana | reclaim yields **both** metal and energy |

Two TA behaviours have no Kingdoms analogue and are new logic:

- **Stall.** When a consumer cannot afford this tick's draw it gets *nothing* and
  retries next tick — all-or-nothing, not a proportional slowdown (confirmed
  against the binary; see `docs/retail-engine-ta.md`). Load-bearing for how the
  game feels, and a hashed sim path, so it has to be deterministic.
- **Wind and tidal income**, which are map properties (`MinWindSpeed` /
  `MaxWindSpeed` / `TidalStrength` from the `.ota`) and, for wind, time-varying.

Also new: **ally resource sharing** and the share/ratio sliders.

### ✅ Construction: nanolathe — done

Kingdoms builders place a building and it appears. TA builders *stream* a unit
into existence, several can assist one job, and build power is additive with
metal/energy drawn continuously at a rate set by `WorkerTime / BuildTime`.

This entry sat at 🟠 ("the tick logic is not [the right shape]") after the tick
logic had in fact been written, which is its own small lesson about trusting a
status line over the code. `tools/nanolathe_test.cpp` now pins the behaviour, so
the claim is checkable rather than asserted — twelve checks, all passing, on a
synthetic world (no retail data, so CI runs it):

* a lone builder takes `buildTime / workerTime` seconds (3000/300 = 10 s, not
  3000 and not 300);
* **a second builder assisting halves it** — build power adds rather than the
  two re-targeting or fighting over the site;
* the job still costs ONE unit's metal and energy however many builders worked
  it (paying per builder would double an assisted job);
* the cost is drawn continuously — half spent at the half-way point, not billed
  on completion;
* a starved job HOLDS: `spendBuild` is all-or-nothing, so an empty treasury
  makes no progress rather than free progress or a cancelled site;
* an abandoned site decays at the rate it was being built and vanishes, leaving
  no wreck — it was never finished.

Construction was also SILENT until now. `loadBuildFx` reads Kingdoms'
`aramonbuild_4444.taf` / `tarosbuild` / `verunabuild` sparkle sheets, none of
which exist in a TA install, so every load threw, `buildFx_` stayed empty and
`sprinkleBuildFx` returned immediately: the sim streamed the unit up and nothing
drew the lathe.

The beam now reuses the existing weapon `BeamFx` renderer (three additive
passes — outer halo, body, hot core) rather than adding a second beam path. One
short beam is emitted per frame while the work is actually happening, so it
starts and stops with the job: a builder still walking to its site, or starved
and making no progress, draws nothing.

Two details worth keeping:

* **Do not gate the beam on `buildProgress`.** That field counts *seconds of
  work on the builder's QUEUE front*, not the site's completion, so it reads 0
  for a building under construction — gating on it suppressed the beam
  entirely. The condition is the site being `underConstruction` with the builder
  inside its `buildDist`.
* The emit piece names are **measured**: of the 53 builders in the Commander
  Pack, 29 carry a nanolathe piece, spelled `nano1`/`nano2` (14 each),
  `nanospray` (5), `nanogun` (4), `nanopoint` (3), `nano` (3), `nanolath` (2),
  `nozzle` (2), plus `l`/`r`-prefixed pairs. The other 24 have none and emit
  from the body, which is why the piece is a preference and not a requirement.

**The colours are NOT measured from retail** — a pale green-white chosen to read
as a lathe rather than a weapon. The shape is right; the ramp is a guess, and is
the first thing to check against the binary if it ever matters.

Verified by emission rather than by screenshot: `TA_FXLOG=1` with `--testbuild`
reports `nanolathe: builder 1 (armcom) -> site 3 (armsolar) from (3243,247) to
(3326,247)` — the endpoints are right and the emit point sits forward of the
body, so the piece lookup resolved. A single-frame `--shot` does not land on a
frame with that scene in view (it shows neither the builder nor the site), so
there is no captured image of it here; the beam feeds the same renderer that
draws every hitscan weapon in the game.

### ✅ Sides: four Houses become two — done

`gamedata/SIDEDATA.TDF` turns out to carry, in one file, four things Kingdoms
kept in four places — so `src/tdf/sidedata.{h,cpp}` reads all of them:

| | |
| --- | --- |
| **Sides** | `[SIDE0]` ARM, `[SIDE1]` CORE, each with its `commander=`. Kingdoms had no equivalent — its five monarchs were a hardcoded table in the engine — so the roster now comes from the install, not from source. |
| **The build tree** | `[CANBUILD]`, one block per builder, `canbuild1..N` **in menu order**. Kingdoms used a directory of `canbuild/<builder>/<buildable>.tdf` marker files with a `[Menu] priority` that had to be sorted, because a directory listing has no inherent order. |
| **The HUD panel layout** | exact pixel rects for both resource bars, their numbers and caps, the production/consumption readouts, the unit footer and the reload bars. This is what a faithful two-resource panel gets drawn from. |
| **Per-side cosmetics** | the interface GAF, fonts, and the palette indices the bars are drawn in. |

This forced a **TDF parser change**. A value used to run to end of line — a
Kingdoms accommodation, since its data had values legitimately containing `;`.
SIDEDATA packs all four keys of a rect onto one line 240 times over, so every
rect was reading as its `x1` and three zeros. A scan of all 1819 TDF/FBI/OTA/GUI
files in the Commander Pack found exactly one line where text follows a `;` —
`ARMSCORP.FBI`'s malformed `ItalianDescription=;Scorpione` — so `;` now
terminates, and a stray token is skipped rather than fatal (failing on it
abandoned the file and silently dropped the Core Contingency Scorpion).

### 🟠 Commander

TAK's `commander=1` Monarch already carries the loss condition, and TA's
*Commander Ends Game* is the same rule — so the plumbing exists. The commander
itself gains TA-specific behaviour: the **D-Gun**, and a death explosion
proportional to what it was carrying.

### ⬜ New sim subsystems with no TAK counterpart

- **Radar / sonar / jammer / seismic** as distinct sensor layers. TAK has a
  single `radardistance` folded into the sight radius — TA needs jamming
  (which *subtracts* coverage), sonar for the water layer, and radar blips that
  show position without identity.
- **Underwater as a third movement layer.** TAK water is a surface; TA has
  submarines, torpedoes, and units that walk the sea floor.
- **Nukes and anti-nukes**, with interception geometry.
- **Shields** (Core Contingency) — area damage absorption with its own energy drain.
- **Air layer depth**: landing pads, fuel, proper transports, `VTOL` vs fixed-wing.
- **Kamikaze, self-destruct, `ExplodeAs`/`SelfDestructAs`** weapon-driven deaths.

### 🗑 Kingdoms-only sim features — removed

Taken out rather than left dormant. An earlier draft of this document argued for
keeping the unused paths "for mods"; that was wrong. Nothing in TA's data reaches
them, so they would have been untested code that still has to compile, still has
to be reasoned about in every refactor, and still shows up in every grep.

| Removed | |
| --- | --- |
| ✅ Gods | favour, priests (`attractsgods`), appear time, `summonReadyGods`, the client announcer |
| ✅ The single mana pool | `Player::mana`/`storage`/`income` → `Resource metal, energy` |
| ✅ Per-unit caster pools | `maxmana`, `manarechargerate`, `manapershot`, the recharge tick |
| ✅ Veterancy | `veteranmodel`, `noveteran`, `xp`/`veteran`, `vetMul()`, the gold tint, the HUD pips |
| ✅ Petrify & freeze | statue deaths, `stone=`/`frozen=` features, `cantbestoned`/`cantbefrozen` |
| ✅ Mind control | and the charm roll that scaled off the victim's veterancy |
| ✅ Resurrect & animate | the whole revive channel, and corpse `resurrectable` |
| ✅ Mana deposits | Sacred Stones → TA metal patches (`category=metal`) |

**Paralyze stayed**, because it is not a Kingdoms mechanic: TA's paralyzer is
damage type 4 (the Core Contingency Immobilizer) and 8 shipped types carry
`ImmuneToParalyzer` against it.

Still to remove: the monarch/house vocabulary, and crusades.

`tools/retailgap_test.cpp` is the casualty of all this. It is 1903 lines of
Kingdoms regression coverage whose ~50 unit lookups are all `ara*`/`tar*`/
`ver*`/`zon*` types, each section guarded by `if (reg.find(...))` — so against a
TA install it runs to completion, asserts nothing, and reports success. It now
bails loudly instead. The behaviours it covers (queued orders, stances, VTOL
standby, area reclaim, self-destruct, per-category damage,
`canMove`-vs-structure) all matter for TA, so the scaffold is kept and the gate
comes off section by section as each is re-pointed at TA units.

## 4. TAK extras to preserve

The brief is to keep as many as possible. Every one of these is game-agnostic
and survives the port:

**Multiplayer** — shared team vision **and radar**, dedicated client-server (no
NAT/port-forwarding), server-side AI, referee sim, gameplay-data fingerprint at
join, reconnect with resume token, live spectate, `.tarep` replays, accounts with
SCRAM-SHA-256 and lockout, lobby with browser/teams/colours/password, host-set
unit cap, unlockable in-game speed.

**Gameplay** — five AI difficulties including Absurd's doubled income,
formations (distinct from control groups, slowest-member pacing, stragglers
rejoin), area-reclaim drag, minimap orders, rebindable controls.

**Presentation & ops** — the benchmark mode and its stats screen, stress test,
smooth-GUI-art upscaling, Bink deblocking, the self-calibrating VRAM budget,
terrain streaming, unit shadows, health bars, the full options screen, the
cosmetic/gameplay override tiering, `cartographer`, and the disco/headbang
emotes.

**Engineering** — cross-build determinism via `detmath`, fully static binaries,
the 7-distro packaging matrix, and the CI shape.

Two need renaming rather than porting: *Monarch Expendable* is TA's **Commander
Ends Game**, and the god toggle has no TA meaning and goes.

TA has native options the engine does not yet model — **Line of Sight**
(circular / permanent / true), and **limited vs unlimited resources** — which
belong in the same lobby panel.

## 5. Build order

Each milestone ends green: builds, `ctest` passes, determinism gate agrees.

| # | Milestone | Gated on retail data? |
| --- | --- | --- |
| 0 | **Fork + rebrand.** ✅ done — 137 targets, 17/17, golden hash agrees. | no |
| 1 | **HPI v1**: header, obfuscation, LZ77. ✅ done — 21 synthetic checks, then validated against the real install: **30 archives, 7,890 files extracted, zero failures.** | no |
| 2 | **Mount a real install**: VFS over `.hpi`/`.ufo`/`.ccx`/`.gp3`. ✅ done — extension-ranked mount resolves `gamedata/SIDEDATA.TDF` → `totala1.hpi`. | done |
| 3 | **Formats**: 3DO/GAF/COB confirmed; TNT + compositor rewritten. ✅ done — all 96 shipped maps round-trip **byte-identically**, and real maps render. | done |
| 4 | **Unit data**: FBI → `UnitType`. ✅ done — key set surveyed across all 815 shipped FBIs; 278 types load, ARM 137 / CORE 141, checked against real values by `unitdata_test`. Weapons and `SIDEDATA` still to come. | done |
| 5 | **Two-resource economy**: metal+energy, stall, map-driven wind/tidal/metal. 🔨 sim side done; HUD shows metal only, and the Kingdoms mechanics still need removing. | in progress |
| 6 | **Skirmish playable**: ARM vs CORE, AI, HUD. | yes |
| 7 | **Sensors** (radar/sonar/jammer), water layer, air depth. | yes |
| 8 | **Campaigns** + Battle Tactics missions. | yes |
| 9 | **CC/BT content**: nukes, shields, the expansion unit sets. | yes |

Milestone 1 is the only substantial piece that can be finished before the retail
data lands, so it goes first.

## 5a. Measuring the AI build-up — use a fixed seed

`taserver --seed <n>` pins the per-game seed; without it the server rolls a fresh
one per game (`randToken()`), and two runs of the *same* binary on the same map
produce different world hashes and different unit counts. A headless run is
otherwise fully deterministic: with `--seed` fixed, repeated runs reproduce the
world hash exactly.

This matters because `units=` at a fixed wall time is the obvious progress metric
and it is **noisy across seeds by a factor of two or more**. Numbers quoted from a
single unseeded run — including some in this repo's earlier commit messages, which
compared before/after economy changes that way — carry seed variance, not just the
change under test. Compare across the same fixed seeds, or not at all.

Current baseline, Coast To Coast, 2 AI, 120 s of simulated time:

| seed | units |
| --- | --- |
| 11 | 12 |
| 22 | 11 |
| 33 | 9 |

Three seeds spanning 9..12 on identical code is the point: a single run landing on
9 or on 12 says nothing about a change on its own.

The harness line also reports `kills=`, because `units=` alone cannot tell two
armies grinding each other down from two that built up peacefully and never met —
and at 120 s on this map it is still `kills=0`, so that figure is measuring
build-up only.

**A hash that does not move is a result too.** Re-running these three seeds after
the AI's economy ladder was rebased onto a measured factory appetite reproduced
all three world hashes *byte for byte*, at 120 s **and** at 300 s. That is worth
recording rather than quietly re-running at a length that flatters the change —
and it is what sent the investigation to the real constraint below.

### What the AI's economy was actually doing

The harness line reports `p0-metal=` and `p0-energy=` with their rates, and a
`p0 mix:` line of the most numerous unit types, because `units=` answers neither
"is the economy growing" nor "growing on which resource". Against those:

> `units=8 p0-metal=630(+1.0/s) p0-energy=0(+45.0/s)`
> `p0 mix: armcomx1,armlltx1,armsolarx1,armwinx1`

Metal pinned at **+1.0/sec** — the Commander's own `MetalMake=1`, and nothing
else — for the whole match, while energy climbed past +55. **The AI never built a
metal extractor.** No threshold was stopping it: the planner had a single
`Economy` category, inherited from Kingdoms' one-resource mana economy, so an
extractor was one interchangeable draw among the eleven economy buildings on the
Commander's menu and simply lost the dice most of the time. Three fixes, each
measured on the same fixed seed:

| | metal rate @60 s | mix |
| --- | --- | --- |
| before | +1.0/s | commander, LLT, solar, wind |
| split metal vs power | +2.9/s | **2× mex**, 2× storage, commander, solar |
| + storage gated on capping out | +2.2/s | mex, maker, storage, commander, solar |
| + makers gated on spare energy | **+5.0/s** | **4× mex**, commander, storage |

A fourth followed from watching the same seed at 300 s: metal had reached
+6.1/sec, past the threshold for a first factory, and the AI still built none. The
energy rule was treating a low energy *stock* as urgency, and a stock near zero
while income exceeds drain just means construction is spending it — which is what
it is for. That pinned power at priority 96, above Factory's 90, for ever. A
stall is `income < drain` and nothing else. With that corrected, seed 11 at 300 s:

> `units=16 p0-metal=1227(+6.1/s) p0-energy=538(+55.0/s)`
> `p0 mix: armmexx4,armradx2,armcomx1,`**`armlabx1`**`,armmstorx1,armsolarx1`

— four extractors, a power base, and a Kbot Lab: the first factory to appear in
any of these runs, against +1.0/sec and no extractor at all before.

`kills=` is still 0 throughout. That is the map, not the planner: Coast To Coast
puts the two starts on opposite sides of open water, so nothing built so far can
reach the other player. Testing whether the AI *fights* needs a land-connected
map, and is the next thing to measure rather than something to infer from here.

### What a metal-poor map showed

Running the same AI on **The Pass** — all land, so a fair combat test — instead
exposed two more things.

It built **eight extractors and stayed at +1.3 metal/sec**. The Pass declares
`SurfaceMetal=3` and places no metal feature at all, so a 3x3 mex there earns
`9 x (3+1) x 0.001` = 0.036/sec and takes over twenty minutes to repay its own 50
metal. (That `SurfaceMetal` is a per-cell *richness* is settled in
[`retail-engine-ta.md`](retail-engine-ta.md) by the extremes: Metal Heck, TA's
all-metal map, declares 255 and places no metal feature either.) The planner now
asks `World::extractorYieldAt` what a site would actually pay — the same function
that will pay it — and declines ground that never repays the building. Coast To
Coast is untouched by that rule, byte for byte; The Pass stops throwing metal
away.

And then it built **nothing at all** for five minutes, sitting on ~1000 of each
resource. `TA_AI_PICK` named it: `armlab ... afford=Y usable=0`. The store rule
above was testing "does this declare a storage field", and **nearly every TA
building carries a small buffer** — a Kbot Lab declares `MetalStorage=100` and
`EnergyStorage=100` — so it had classified every *factory* as a store and refused
to build one unless the player happened to be near their cap. Scoped to the
economy categories, both maps reach production: Coast To Coast 5 extractors,
+6.1/sec and an Aircraft Plant at 300 s; The Pass a Kbot Lab inside 45 s.

The lesson is the one the diagnostics exist for: a rule keyed on a *field being
present* rather than on what the thing is FOR will quietly catch everything that
happens to carry the field.

With both fixed, The Pass at 300 s answers the question Coast To Coast could not:

> `units=9 kills=2 p0-metal=23(+1.0/s) p0-energy=1100(+25.9/s)`
> `p0 mix: armpwx3,armcomx1,armlabx1,armmstorx1`

A Kbot Lab, three Peewees out of it, and **kills** — the whole chain, economy to
factory to army to contact, on an AI that six commits earlier built four
buildings and never moved off the Commander's own +1.0 metal/sec.

The middle two are the sort of thing a single-resource planner cannot see: a
metal *store* produces nothing but shares a category with the things that do, and
a metal *maker* burns 60 energy/sec for 1 metal/sec, so built without that
surplus it never produces at all (upkeep is billed per unit, all or nothing) and
is pure spent metal. Retail's own profile damps makers to weight 0.1, which makes
that unlikely; gating them on the energy to run them makes it correct.

## 5b. Campaign missions

A TA mission is a map's `.ota` under `maps/`, not a separate file type: extra
GlobalHeader keys, the opening board as placed units inside each schema, and the
objectives as plain keys. `camps/Arm Campaign.tdf` lists them (`missionfile=
AC01.ota`), and `camps/useonly/<name>.tdf` restricts the build menu — the `.ota`
names that file in `useonlyunits=`.

Two things are load-bearing and were read off the data:

- **A schema is a DIFFICULTY.** AC01 ships Easy / Medium / Hard, each with its
  own board and its own opening treasuries (Easy gives the human 1000 metal
  against the computer's 100; Hard reverses it). A skirmish map's schemas vary
  the *player count* instead, so a reader written for one silently mis-serves the
  other — reading only the first schema locks the campaign to Easy.
- **`.ota` player 1 is the human.** Checked across all 51 shipped ARM and CORE
  campaign missions: player 1 owns the campaign's own side and player 2 the
  enemy. AC01's player 1 holds ARMFAV/ARMPW/ARMGATE and player 2 the CORAK/CORFAV.

The **campaign spine** resolves too: all **175 missions across 13 campaigns** —
ARM and CORE at 25 each, both Core Contingency twelves, all eight Battle Tactics
sets and the Krogoth Encounter — now find their `.tnt` and `.ota`. The blocker was
one key. Each `[MISSIONn]` lists both `missionfile` and `missionname`, and they do
not mean the same thing:

| | `missionfile` | `missionname` |
| --- | --- | --- |
| TA | `AC01.ota` | `1: A Hero Returns` |
| Kingdoms | `takmission01_mt.ota` | `takmission01_mt` |

Kingdoms repeats the stem in `missionname`, so reading the stem from there worked
— and silently asked a TA install for a file called "1: A Hero Returns". Every TA
campaign resolved to nothing. The stem comes from `missionfile`; `missionname` is
a display title in TA, and is left to `translate/missions.tdf` in Kingdoms.

**Briefing text** comes through too — all **175** missions, where before the fix
below only 70 did. TA keeps it in `camps/briefs/<brief>.txt`, named by the
mission's own `.ota` (`brief=ArmCampaign1`), as prose rather than Kingdoms'
bulleted `missions/<stem>.txt`. Two details:

- The `brief` key **sometimes carries the extension and sometimes does not** —
  the campaign missions write `brief=ArmCampaign1`, the Battle Tactics scenarios
  `brief=I09Brief.txt`. Appending `.txt` unconditionally asked for
  `I09Brief.txt.txt`, which is why only the campaign proper had any text.
- Colour runs are written `&X … &`: the **opening** delimiter carries a
  one-letter colour and the closing one does not. Across the 50 shipped brief
  files the opens are exactly R (38), Y (31) and G (13) and the closes are the
  other 82 occurrences — they balance — so stripping only the `&` leaves the
  letter glued to the text ("RExpect Core patrols").

AC01's briefing is also an independent confirmation of the win/lose split: *"Take
your units to the Gate and secure it. Our Commander must return safely or we are
lost."* — `MoveUnitToRadius` as the victory, the Gate's destruction as the defeat,
which is exactly what the two objective arrays say.

`setupMission` now takes the TA path when `maps/<stem>.ota` places units, and
falls back to the Kingdoms arrangement (`missions/` + a `.cob` god script + a
`.crt`) otherwise, so a Kingdoms install still runs. AC01 loads: 34 units over 2
players on Easy, the enemy gets a brain from the schema's `aiprofile=MISSIONS`,
the conjure menu restricts to its 14 allowed types, and the two sides fight.

**Open, and flagged rather than papered over.** The Galactic Gate the player owns
in AC01 declares `EnergyUse=3000` and is not `onoffable` — six unit types carry
an upkeep at or above 100, and the two gates are the only ones that cannot be
switched off. Under the per-unit all-or-nothing billing established in
[`retail-engine-ta.md`](retail-engine-ta.md), the gate drains the mission's
opening 1000 energy in about a third of a second and then simply never pays
again. Whether retail charges a campaign gate at all is not established here.

Objectives now **evaluate**. The win/lose split is settled in
[`retail-engine-ta.md`](retail-engine-ta.md): the engine keeps two arrays on the
mission object and which one a key lands in is its meaning. It is not guessable
from the names — `CommanderKilled` (123 missions) is the DEFEAT and
`KillEnemyCommander` (7) the victory, two different keys on opposite lists, and
`AllUnitsKilledOfType=ARMGATE` in AC01 means *protect* the gate, the only one on
that map being the player's.

The evaluator itself was already here and already correct: the inherited
`MissionScript::parseConditions` classifies all twelve keys it knew about exactly
as the objective factory does, which is worth stating because it was arrived at
independently. A TA mission attaches it in its data-only form — no `.cob`, just
the `.ota` conditions — and the condition keys are the same names in both games,
so it reads a TA header unchanged. Three keys the shipped TA data uses were
missing and are now implemented: `CaptureUnitType` (32 missions),
`BuildUnitType` (8) and `AnyUnitPasses{X,Z}` (3).

## 5c. Feature art: 432 of 1644 defs are 3DO models, not GAF sequences

TAK's features are all sprites: a `.gaf` file plus a sequence name. The loader
took that as the only shape, so `featureArtFor` returned nothing whenever a def
named neither, and those features were placed in the sim but never drawn.

Counting every top-level section in `features/**/*.tdf` across the merged
archives (`*.hpi`, `*.ufo`, `*.ccx`, `*.gp3`) gives **1644 unique defs, which
is exactly the count the engine's own loader reports** — so the corpus below is
the same set the engine sees. Of those, **432 (26%) carry `object=<3do>` and
neither `filename=` nor `seqname=`**; 424 of the 432 are the `*_dead` /
`*_heap` wreckage every unit leaves behind, and the remaining 8 are standalone
props like `DragonsTeeth` (`object=armdrag`). So all unit wreckage drew
nothing, and AC01 reported `10 no art` before the count was understood.

(Counting this requires a CASE-INSENSITIVE file match: TA ships
`features/archi/METAL.TDF` alongside `features/all worlds/DragonsTeeth.tdf`,
and a case-sensitive `*.tdf` sweep silently drops 22 files and 338 defs — the
same trap the loader itself hit, which is why it uses `ta::iendsWith`.)

`GameView::featureModelArt()` renders such a model ONCE into a texture sized to
its own bounds and feeds it into the existing `FeatArt`/`FeatureInst` sprite
pipeline, so placement, sorting and shadows are unchanged — only the source of
the pixels differs.

One TAK assumption had to be scoped to make it work. `pieceMetaFor` sets
`m.skip = isRoot || ...`: in Kingdoms a model's root IS a flat ground-reference
plate, and `modeltool info` confirms that holds for TA units too (armcom's and
armpw's roots are a single 4-vertex quad with the real geometry in children).
It does NOT hold for a feature model, which is standalone and often a single
piece: **armdrag is a root named `base` carrying all 37 of its primitives with
no children at all**, so the root skip discarded the entire model. The fix
passes `isRoot=false` from `featureModelArt` only; unit rendering is untouched.

Measured after: AC01 places `360/360 (1644 defs; 0 no def, 0 no art)`, and the
skirmish path is unchanged at `120/120`.

A map only ever exercises a few dozen of the 1644 defs, though, so a clean load
proves very little. `TA_FEATART=audit` primes **every** def at map load instead
and names what fails — which is how the HPI precedence bug in §2 was found: the
audit reported `1643/1644`, and the single hold-out was the `Lavavent003` whose
base-archive `seqname` does not exist. With precedence fixed it reports
`1644/1644 defs have art (0 fail; 432 are object= models, 0 of those fail)`.

### Wreck models: already working, and how that was got wrong

An earlier revision of this section claimed corpses drew as the intact unit
lying flat and listed it as an open gap. **That was wrong**, and the way it went
wrong is the point: `drawUnit` resolves its model as
`visuals_.find(unitType_.at(u.id))`, which reads like the live type — but
`maybeSwapCorpseModel` (`gameview_impl.cpp`) REPOINTS `unitType_[id]` at the
corpse feature's `object=` model when a body finishes dying, so that lookup has
already become the wreck. Reading the resolution site without following what
mutates its input produced a confident claim about a gap that did not exist.

Measured, on a staged kill: `corpse model swap: unit 7 (corak) -> wreck 3do
'corak_dead'`, and the projection then collects **46 triangles** from it. The
same number comes out with the root-skip argument either way, because the
swapped visual carries an EMPTY `PieceMeta` map rather than a null one, and
`collect` only computes the skip live when it is handed no meta at all. So the
`isRoot` trap that §5c hit through `featureModelArt` does not arise here.

What is genuinely worth recording:

* `corak_dead` is a single piece -- `deadak`, 38 prims, no children -- against
  live `corak`'s 1-prim `ground` root with children. TA wreck models are
  standalone, and they use dedicated `wreck*`/`noise*` textures no live unit
  references.
* **228 of 278 unit types declare `Corpse=`, and all 228 resolve** to a feature
  def with `object=` and a `.3do` that loads (161 chain on to a further
  `featuredead` heap stage). `TA_FEATART=audit` now reports this as a second
  line, because a map load never exercises a corpse def and so proves nothing
  about it.

### No unit ever spoke

`SoundClasses::load` read `gamedata/soundclasses/*.tdf` — the Kingdoms layout,
NESTED and weighted: `[CLASS] { [event] { wav=weight; } }`. A TA install has no
such directory at all (0 files); it ships **one flat `gamedata/SOUND.TDF`**,
31 KB, 120 classes, `[ARM_KBOT] { select1=kbarmsel; ok1=kbarmmov; … }`. So every
class map came back empty, `pick` returned null for every unit, and no unit ever
acknowledged a selection or an order with its voice — they only ever produced
the fallback click tone.

The EVENT NAMES differ too. TA spells them `select1` / `ok1` / `arrived1` /
`cant1` / `underattack` / `working` / `build` / `count0..5` / `canceldestruct`,
while the call sites ask for `select` / `move` / `attack` / `guard`. The loader
now registers both the TA key and an alias. The aliases are inferred from the
key names and the WAVs they point at (`ARM_KBOT`: `select1=kbarmsel`,
`ok1=kbarmmov`, `arrived1=kbarmstp` — select, move, stop), **not read from the
binary**; mapping all three order events onto TA's single `ok1` is the part most
worth re-checking.

Measured after: 120 classes, 1930 events. **267 of 278 unit types resolve a
class** (267 answer `select`, 196 answer `move` — the difference is buildings,
which have no `ok1`). Before: none of them did.

The 11 that resolve nothing are all explained by the shipped data, not by the
engine:

* **6 declare `SoundCategory=none` outright** — `armdrag`, `cordrag`,
  `armfdrag`, `corfdrag`, `armfort`, `corfort`: dragon's teeth and forts, inert
  walls that correctly have no voice.
* **5 are typos in Cavedog's data.** `SOUND.TDF` is itself inconsistent about
  the CORE prefix — it defines `COR_KBOT`, `COR_MEX` and `CORE_TANK` — and these
  five FBIs name the other spelling: `corfast`, `corfhlt` and `corspy` ask for
  `core_kbot`, `cormex` for `core_mex`, `corsent` for `cor_tank`. No section
  answers, so **those five are voiceless in retail too**. Not papered over here:
  a forgiving COR_/CORE_ fallback would hand them voices the real game does not.

`TA_SNDLOG=1` reports the class/event totals, the coverage line above, and each
`voice()` lookup with what it resolved.

### The game played no music at all

`startMusic` looked for `music/track<N>.wav` — Kingdoms' layout, which SDL loads
directly. TA ships **`music/<N>.mp3`**: 18 tracks numbered 0..17 on a GOG
install, in a format SDL cannot decode. So the playlist came back empty
(`music: 0 faction tracks`) and the game was silent from the menu onward.

Rather than add a second media dependency, this reuses the FFmpeg already
vendored and static-linked for the Bink menu videos: the build now enables the
mp3 demuxer/decoder alongside bink, and `src/video/audiodec.cpp` decodes a whole
file to interleaved S16 through the same avformat → avcodec → swresample path
`BinkVideo` uses for binkaudio. It feeds memory, not a path, because music comes
through the VFS. Whole-file decode is deliberate: a track is a couple of minutes
of stereo, decoded once when it starts, and streaming would buy memory this does
not need at the cost of a decode thread feeding the mixer callback.

Measured after: `music: 18 faction tracks, audio=yes` / `now playing
music/3.mp3`.

**A trap worth knowing before touching the codec list.**
`tools/build-ffmpeg-bink.sh` SKIPS the build when its prefix already exists, and
CI restores that prefix from a cache keyed by FFmpeg *version*. Enabling a codec
without also bumping `ffmpeg-bink-*` in `.github/workflows/*.yml` therefore
restores the OLD libraries, skips the rebuild, and produces a **green build
whose decoder is quietly missing** — the exact silent-success pattern this
section of the document keeps describing. All five key occurrences were bumped
with the change, and the script now says so at the configure call.

A build whose FFmpeg lacks the decoder degrades rather than breaks:
`avcodec_find_decoder` returns null, `decodeToS16` returns empty, and the player
logs `music: cannot decode …` instead of crashing.

### 38 of 197 maps had no extractable metal

`FeatType::isMetal` was `category == "metal"`. That misses seven metal-patch
defs filed under `category=rocks` — `rockmetal`, `rockmetal1/2/3`,
`greenaquaore1/2/3` — each a 3x3 footprint with `metal=` between 86 and 250,
each placed on 10 to 35 of the shipped maps. They contributed nothing to the
metal plane, so **38 of the 197 shipped maps had no extractable metal at all**.
The Pass is one: it places four `RockMetal3` and the engine reported
`metal patches: 0`.

Quantified on The Pass, whose `[Schema 0]` declares `SurfaceMetal=3`: every cell
fell back to that background richness, so a 3x3 extractor scored
`(3 + 1) x 9 = 36`. On a `RockMetal3` patch it now scores
`(223 + 1) x 9 = 2016` — **56x** the metal, which is the difference between a
mex being worth building and not.

**The obvious rule is the wrong one, and it is worth saying why.** `metal > 0`
looks like the natural test and would be badly wrong: **232 of the 254
`category=rocks` defs carry `metal>0`, and 224 of those are described plainly as
"Rock"** — ordinary boulders, reclaimable but not extraction sites. Their values
overlap the real patches exactly (`slaterock09` is `metal=249` against
`rockmetal3`'s 223), so no threshold separates them either. Keying on the value
would have painted metal richness under nearly every rock on every map while
looking like a fix.

Magnitude does not work on its own either. Reclaim scrap runs far higher —
`building06` is `metal=11000`, `comstat02` is 8997 — and every genuine patch sits
in an 84..250 richness band; but cars (20..37), pipes (25..75) and trucks
(50..65) share that band.

What separates them is the DESCRIPTION. Measured: `description=Metal` gives 62
defs, `category=metal` gives 82, and their union is 89, of which 81 are actually
placed by some map. That union is the rule now, in both the sim
(`matchsetup.cpp`, the metal plane and extractor yield) and the client
(`addFeature`, the patch list the HUD and AI use) — they had drifted, which is
why fixing only the sim left the client still reporting zero.

### Build buttons showed rendered models, not TA's artwork

`iconFor` asked for `anims/buildpic/<id>.jpg` — Kingdoms' layout. A TA install
has **no `anims/buildpic` at all** (0 files); it ships **`unitpics/<UNITNAME>.PCX`,
282 hand-drawn portraits**, one per unit. So every lookup missed and every build
button fell back to `modelIconTex`, rendering the unit's 3DO as an icon. Not
broken — the menu was perfectly usable — just never the artwork the game ships.

There was no PCX *image* decoder: `gaf::Palette::fromBytes` reads a PCX's
trailing colour table and nothing else. `src/util/pcx.cpp` adds one, handling
only the shape TA ships — RLE, one plane of 8 bits, 256-colour palette appended
after the pixels — and returning an empty image for anything else rather than
guessing.

Measured: **all 282 portraits decode, 0 failures**; `armcom.pcx` is 96x96 and is
the Commander's familiar picture. Both art paths remain, JPEG first, then PCX,
then the model fallback, so no data set loses what it had.

### Impacts drew particles instead of the authored explosion art

`Weapon::explosionClass` is parsed from `explosionclass`, which a Kingdoms
weapon resolves through `gamedata/explosions/explosions.tdf` into a list of
effect animations. **A TA install has neither**: `gamedata/` holds 13 files and
`gamedata/explosions/` does not exist, and across the 620 weapon sections
`explosionclass` appears **0** times. TA names the art on the weapon instead —
`explosiongaf=<file>` plus `explosionart=<sequence>`, 297 sections — with a
matching water pair.

So `explosionClass` was always empty and every impact fell through to
`spawnImpact`, the procedural-particle fallback. Not invisible, which is why it
went unnoticed: just never the real thing.

`Weapon::explosionAnim` / `waterExplosionAnim` now carry TA's pair in the
client's existing `"file:sequence"` form, which `effectFor` already understood,
and the impact path tries them before the class lookup and the particles. The
Kingdoms class path is left in place beneath, so nothing regresses for data that
does declare one.

Measured after, with `TA_FXLOG=1`: a fight over land plays `fx:explode5` (26) and
`fx:explode2` (3); over water, `fx:h2o` (12 frames). Zero impacts fell through to
the class path or the particles.

Display-only and never hashed — `check-determinism` still agrees on golden
`ab1ef54ae324bd0e`.

### Weapons fired in silence

Same file, a second failure. The sim parses a weapon's impact sound from
`soundhitclass` and already falls back to TA's `soundhit`, so impacts were fine.
But **`soundstart` — the FIRING sound — was never parsed at all**, and the
client's fire branch fell through to a chain of Kingdoms stems: `bow2`,
`firedrag`, `fireflsh`, `lightng<N>`, `ahitfl0<N>`. Every one of those is ABSENT
from a TA install, so any weapon whose COB script did not play a sound of its
own fired silently.

`Weapon::soundStart` is now parsed and played ahead of that fallback chain.
Measured over the weapons units actually carry: **167 weapons, all 167 declare
`soundstart`, and all 167 have the WAV present** (all 167 also have a playable
`soundhit`). Across the raw tables it is 298 of 620 sections, the remainder
being weapons no unit fields.

It is display-only and never hashed, like `soundHit` and the projectile art
beside it — `check-determinism` still agrees on golden `ab1ef54ae324bd0e` with
the field added.

### The lobby map preview was blank for every TA map

Same shape of fault again, and it took two independent fixes because it was
broken twice over.

1. **Wrong header field.** The preview read TNT header field 12, then 11 — the
   Kingdoms hi-res overview and minimap. A TA TNT (version `0x2000`) keeps its
   minimap pointer at byte 40, i.e. **field 10** (`src/tnt/tnt.cpp`,
   `pMinimap = u32(&d[40])`). Probed across the shipped maps: field 10 gives a
   good 252x252 image on every one, and fields 11 and 12 give pointers the
   bounds checks reject. So the preview returned before doing anything.
2. **No palette.** Even with the image, it asked for `palettes/<kingdom>.pcx`
   named by the `.ota`'s `kingdom=`, falling back to `"aramon"`. A TA `.ota`
   carries no `kingdom=` and a TA install ships no `<kingdom>.pcx` — its
   `palettes/` holds one game palette (`PALETTE.PAL`) plus the GUI's. Both
   lookups returned null and the builder gave up again.

A third thing had to be measured rather than assumed: a TA minimap is **always**
252x252, with the map in the top-left proportional to its own aspect and the
remainder filled with palette index **100**. Measured: a 450x392 map uses
252x216, 386x264 uses 252x168, 194x296 uses 168x252, and a near-square 386x392
fills the whole 252x252. Drawing the padding would letterbox every non-square
map in grey, so the preview crops to the used region. (Index 9, which the old
code treated as transparent, is the KINGDOMS transparent index and means nothing
here.)

Verified by reproducing the pipeline standalone against the install: Coast To
Coast yields `252x252 -> cropped 252x142` and renders as a recognisable
coastline.

### The local harnesses were spawning nothing

The staged-combat harness was still a Kingdoms scene: it spawned `araarch`,
`tararch`, `vertower`, `tardrag`, `zonbasil` and `tarpries`, and checked
basilisk petrification and a necromancer raising a ghoul from a corpse. **None
of those unit ids exist in a TA install and none of those mechanics exist in
TA**, so `spawn` found nothing every time and the flag did nothing at all —
silently, because a missing id is not an error there.

Rebuilt around the checks that still mean something: a shooter kills a target so
the death leaves a TA wreck, a tower auto-acquires something off-axis (the aim
pipeline's heading sign), splash beside a flamable feature ignites it, one unit
is killed outright and left alone so a wreck persists to be looked at, and a
builder is sent to reclaim another. Units are chosen by probing the registry
(`armpw`/`corak`/... ) rather than hard-coded, so a data set with different
sides still stages a fight instead of silently staging nothing. The camera is
pinned too — follow mode re-centres on moving friendlies every tick and drags
the view off the scene, and off a wreck, which does not move.

`--firetest` was not alone. Sweeping `src/` for hard-coded Kingdoms unit ids
turned up the same fault in every other local harness:

| flag | spawned | outcome |
|---|---|---|
| `--navy` | `verflag`, `verman`, `verharp`, `vertre`, `npcbotl`, `monpiran` | converted to ARM vs CORE ships |
| `--amphib` | `vertrans`, `araarch`, `arasword` | converted to the Hulk/Envoy + kbots |
| `--facetest` | `araarch` | converted |
| `--soundtest` | `araarch` | converted |
| `--testbuild` | `aralode`, anchored on `keepId_` | converted: the commander builds a solar collector |
| `--creon` | `cregod`, `creiron`, … | **removed** — Creon is a Kingdoms faction |
| `--misstest` | `verat`, `versword` | **removed** — Kingdoms mission06 coordinates |
| `--lodetest` (+ `--lodeunit`) | `zonlode` | **removed** — mana lodestones have no TA counterpart |

`--amphib` is the one worth singling out. It did not merely spawn nothing: it
opened with `registry_.find("vertrans")`, and a null type makes `navFor()` fall
back to the GROUND grid, so the beach search that follows ran in the wrong
domain before spawning nothing anyway. It now picks a real sea transport first
and bails with a message if the data set has none, and it finds a beach:
`amphib: embark beach (1421,1135) landing (1898,562)`.

The general lesson is in how these failed. `spawn()` on an unknown id is not an
error, so a harness whose entire cast is missing runs to completion and reports
success. Converted harnesses now pick their units by PROBING the registry and
say so on stderr when they find nothing, so the next data-set change makes noise
instead of quietly doing nothing.

One harness property to know before using `--firetest` to look at deaths: the
virtual clock outruns the corpse window. A corpse is only in `corpsePhase` between
`deadFor` 4 and its decompose time, and at `--time 30` the first rendered frame
already reports `deadFor=1023.5`. Use a SHORT `--time` (5-8) to land inside the
window.

## 6. Open questions, pending the retail data

Answers come from the install itself and from analysing `TotalA.exe` — static
reading plus Unicorn emulation of individual routines — under the same rules:
**observe only, copy nothing**, and the harness lives outside the repo. Findings
are collected in [`docs/retail-engine-ta.md`](retail-engine-ta.md).

- Exact metal-extraction income formula. The inputs are now known: the map's
  per-cell metal plane, the FBI `ExtractsMetal`, and the `.ota` schema's
  `SurfaceMetal` / `MohoMetal`. The combining rule is not.
- ~~The stall curve~~ — **solved**, see `docs/retail-engine-ta.md`: there is no
  curve. Spending is ALL OR NOTHING per consumer per tick, so a stalled base
  stutters rather than slowing smoothly. The proportional model is gone.
- ~~The extractor formula~~ — **solved**, see `docs/retail-engine-ta.md`:
  `yield = ExtractsMetal × Σ(cellMetal + 1)` over the footprint. The `+1` per
  cell is why a TA mex off a patch trickles rather than sitting dead, and it is
  implemented.
- ~~Wind's cadence and interpolation~~ — **solved**, see
  `docs/retail-engine-ta.md`: a clamped ±2 random walk re-rolled every 0..62
  ticks. It steps, never interpolates, and is implemented.
- Wind income's cadence and interpolation between the `.ota`'s `minwindspeed`
  and `maxwindspeed` (Ashap Plateau: 0 and 4000).
- Whether `.ccx`/`.gp3` sit at fixed priorities or fall through to mount order.
  Currently moot — v1 records carry no date, so every collision ties — but it
  decides what happens when a `.ufo` mod and a `.ccx` ship the same path.
- ~~Whether TA's COB header is a 13-word v4~~ — answered, it parses.

### The install, as it actually is

Confirmed contents of the GOG Commander Pack root (all HPI v1):

| Archives | |
| --- | --- |
| base | `totala1.hpi` (units, scripts, sounds, gamedata), `totala2.hpi` (maps), `totala3.hpi`, `totala4.hpi` |
| patch 3.1 | `rev31.gp3` |
| Core Contingency | `ccdata.ccx`, `ccmaps.ccx`, `ccmiss.ccx` |
| Battle Tactics | `btdata.ccx`, `btmaps.ccx`, `tactics1-8.hpi` |
| other | `worlds.hpi`, and 11 shipped `.ufo` mods (Fark, Flea, Scarab, Necro, …) |

Plus `TotalA.exe` (1.15 MB) for static analysis, and `camps/`, `Bitmaps/`,
`music/`, `Data/` as loose directories.

The `.ota` schemas also answer where the economy's map inputs live:
`tidalstrength`, `solarstrength`, `minwindspeed`/`maxwindspeed`, `gravity`,
`SurfaceMetal`, `MohoMetal`, `lineofsight`, `killmul`.
