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

TA also spreads its data across **four extensions** — `.hpi`, `.ufo` (mods),
`.ccx` (Core Contingency), `.gp3` (Battle Tactics / patches) — all the same
container. The VFS already layers `.hpi` then `.ufo` by newest-entry-date; `.ccx`
and `.gp3` slot into that same precedence chain, so this is a mount-list change,
not a new mechanism.

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

### 🟠 Construction: nanolathe

Kingdoms builders place a building and it appears. TA builders *stream* a unit
into existence, several can assist one job, and build power is additive with
metal/energy drawn continuously at a rate set by `WorkerTime / BuildTime`. The
existing `buildTime`/`workerTime` fields are the right shape; the tick logic is
not.

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
