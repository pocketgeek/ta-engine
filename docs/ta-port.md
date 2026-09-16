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

`terrain::Compositor` is written entirely against the JPG path and gets replaced.
`tnt::Map`'s *consumers* (14 files) mostly touch `heights` / `features` /
`width` / `height`, so keeping that surface stable contains the blast radius to
the loader and the compositor.

`src/tnt/mapgen.cpp` (the procedural generator, 36 KB) emits TAK-format maps and
has to be retargeted at the TA tile set too.

### ⬜ New: the metal map

TA's `.tnt` carries a per-cell **metal density** plane that has no Kingdoms
counterpart — it is what makes an extractor's income a property of *where* it is
built. New data, new sim input, new minimap overlay.

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

- **Stall.** When a resource hits zero, TA does not halt consumers — it slows
  *every* one of them proportionally. This is load-bearing for how the game
  feels, and it is a hashed sim path, so it has to be deterministic.
- **Wind and tidal income**, which are map properties (`MinWindSpeed` /
  `MaxWindSpeed` / `TidalStrength` from the `.ota`) and, for wind, time-varying.

Also new: **ally resource sharing** and the share/ratio sliders.

### 🟠 Construction: nanolathe

Kingdoms builders place a building and it appears. TA builders *stream* a unit
into existence, several can assist one job, and build power is additive with
metal/energy drawn continuously at a rate set by `WorkerTime / BuildTime`. The
existing `buildTime`/`workerTime` fields are the right shape; the tick logic is
not.

### 🟠 Sides: four Houses become two

`UnitType::side` is `ARA/TAR/VER/ZON/CRE` today. TA has **ARM and CORE**, read
from `SIDEDATA.TDF` — which also supplies per-side build menus, unit-icon
mapping, and the side's starting commander. Fewer sides, but a different source
of truth.

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

### 🗑 Kingdoms-only sim features to drop

Gods and god favour (`attractsgods`), the mana-per-shot caster economy, status
weapons (freeze / petrify / paralyze) and their immunities, resurrect, crusades.
Some of these (status weapons, resurrect) are worth keeping as **dormant
data-driven paths** rather than deletions — nothing in TA's retail data sets
them, so they cost nothing and they keep the door open for mods.

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
| 3 | **Formats**: confirm 3DO/GAF/COB against real assets; rewrite TNT + compositor; metal map. | **yes** |
| 4 | **Unit data**: FBI/TDF/weapons/`SIDEDATA`/`MOVEINFO` → `UnitType`. `tdftool` dumps it. | **yes** |
| 5 | **Two-resource economy** + stall + nanolathe construction. | yes |
| 6 | **Skirmish playable**: ARM vs CORE, AI, HUD. | yes |
| 7 | **Sensors** (radar/sonar/jammer), water layer, air depth. | yes |
| 8 | **Campaigns** + Battle Tactics missions. | yes |
| 9 | **CC/BT content**: nukes, shields, the expansion unit sets. | yes |

Milestone 1 is the only substantial piece that can be finished before the retail
data lands, so it goes first.

## 6. Open questions, pending the retail data

Answers to these come from the install itself and from static analysis of
`TotalA.exe`, under the same rules as `docs/retail-engine.md`: **observe only,
copy nothing**.

- Exact metal-extraction income formula. The inputs are now known: the map's
  per-cell metal plane, the FBI `ExtractsMetal`, and the `.ota` schema's
  `SurfaceMetal` / `MohoMetal`. The combining rule is not.
- The stall curve: is the slowdown strictly proportional, or stepped?
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
