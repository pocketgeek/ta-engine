# Retail engine notes (`TotalA.exe`)

Findings from analysing the retail *Total Annihilation* executable. The sibling
document `retail-engine.md` is the equivalent for *Kingdoms*, and describes the
engine this codebase was originally built against; this one is about the game it
is being retargeted at.

`TotalA.exe` is a 1.15 MB PE32 (i386, ImageBase `0x400000`), ~1 MB of `.text`.
Its imports place it precisely: **DirectDraw** (no Direct3D — a software 2.5D
renderer), **Smacker** (`smackw32`) for video, **DirectPlay** for networking, and
**DirectSound**.

**Rules.** Analysis is observation only: reading strings, disassembling specific
paths, and — where it earns its keep — emulating individual routines with
Unicorn to watch what they compute. The harness lives OUTSIDE this repo. No code
or data from the binary is reproduced here or copied into the engine; this
documents an interface for a clean-room re-implementation.

**A caveat that applies throughout.** Linear disassembly of `.text` is not
trustworthy on x86: one misaligned byte desynchronises the decoder until it
happens to resynchronise, so "there is exactly one reference to X" means only
"one was found" — and this document has already been wrong once that way (see
the metal-extraction section). Prefer byte-pattern search for an instruction's
encoding when asking whether something exists. Positive findings quoted with an
address were each read back in context and are solid.

## The FBI parser, and unit-struct offsets

The unit-definition parser is a long unrolled sequence, one key at a time, in a
fixed shape (around `0x42c290`):

```
push <key string>
call 0x4c4760          ; parse a float-valued key, result in st0
fstp dword [ebp+<off>] ; ...stored one instruction group later
```

The `fstp` for a key appears in the NEXT group, which is what makes the offsets
readable. Confirmed so far:

| Key | UnitDef offset |
| --- | --- |
| `EnergyUse` | `+0x1c6` |
| `MetalMake` | `+0x1ca` |
| `ExtractsMetal` | `+0x1ce` |
| `MakesMetal` | `+0x1d2` |

`0x4c4760` is the float-key reader; `0x4c46c0` appears alongside it for a
different value type.

## Metal extraction — solved

The formula, from the placement routine at `0x437880`-`0x4378e6`:

```
; for each cell of the footprint (def+0x76 = footprintX, def+0x78 = footprintZ)
call 0x481550                 ; look up the map cell
movzx dx, byte [eax + 7]      ; that cell's metal byte
inc   edx                     ; ...PLUS ONE
add   word [esp+0x1a], dx     ; accumulate

; once, after the loop:
mov  eax, [ebx + 0x92]        ; unit->def
fld  dword [eax + 0x1ce]      ; ExtractsMetal
fild dword [esp + 0x18]       ; the accumulated sum
fmulp st(1)
fmul qword [0x4fd278]         ; x 1/65536
fstp dword [ebx + 0x58]       ; -> cached on the unit
```

So:

> **yield = `ExtractsMetal` x SUM over footprint cells of (cellMetal + 1)**

Two details that the data alone would never have given up:

- **The plus one per cell.** A 3x3 mex on a 127-metal patch sums `9 x 128 = 1152`,
  not `9 x 127 = 1143`. More visibly, a mex on *bare* ground still yields
  `ExtractsMetal x footprintCells` rather than nothing — which is why a TA
  extractor off a patch trickles instead of sitting dead.
- **The `1/65536`.** The FBI float parser stores values pre-scaled by 65536 — the
  engine's 16.16 convention kept even in float form — and extraction divides it
  back out. The two cancel, so what survives is the FBI value as written.

Checked against the engine: ARMMEX (`ExtractsMetal=0.001`, 3x3) on a `metal=127`
patch gives **1.152 metal/s**, which our `World::extractorYield` now reproduces
exactly.

### Where the per-cell metal byte comes from — and what `SurfaceMetal` is

The cell lookup the formula calls (`0x481550`) resolves `13 * (z*width + x) +
[global+0x14287]`, so the map is an array of **13-byte cells** and metal is
byte **+7** of one. That byte is not in the `.tnt`: every byte of all 96 shipped
maps is accounted for by the planes we read (the only gaps are the tile plane's
16-byte alignment padding), `MapAttr`'s fourth byte is zero on every cell of
every map, and only `.tnt` and `.ota` ship per map. So it is **painted at load**.

`SurfaceMetal` (the string is at `0x504b24`, read at `0x4366b8` into map
`+0xd30`) is the background **richness** of a cell with no metal feature on it,
and this is settled by the extremes rather than by the name:

| map | SurfaceMetal | metal features placed |
| --- | --- | --- |
| Metal Heck | **255** | **none** |
| Lava Alley | 20 | none |
| Coast To Coast | 5 | 10 (ArchMetal1/2/3) |
| The Pass | 3 | none |

Metal Heck is TA's all-metal map and places not one metal feature — it is metal
everywhere *because* its background is the maximum byte. Coast To Coast is the
opposite arrangement: a poor background with rich patches scattered on it.

So a map with a low figure and no patches really is poor ground, and the numbers
follow: a 3x3 ARMMEX yields `9 x (3+1) x 0.001` = **0.036 metal/sec** on The Pass,
against `9 x (187+1) x 0.001` = **1.69** on an ArchMetal1 patch and `9 x 256 x
0.001` = **2.30** on Metal Heck. The AI was building eight extractors on The Pass
and staying at +1.3 metal/sec — the Commander's own trickle — which is what sent
us looking here.

**Not established:** `+0xd30` is also read at `0x40c166`, immediately after a
`rand(255)`, and at `0x40a5ec` where it is multiplied by the map's width and
height and doubled. Both look like a *density* being used to scatter something at
load. Whether that is the metal painting itself, feature scattering, or something
else is unresolved — what the table above pins down is how the figure behaves at
the two extremes, not the routine that applies it.

**Correcting an earlier reading in this document.** The tick site at `0x401486`
loads `ExtractsMetal` only to compare it with `0.0`, and I wrote that up as
"`ExtractsMetal` is a predicate, not a multiplier". That was right about *that*
site and wrong about the mechanic: the multiply happens once at placement, and
the tick is only asking "is this an extractor?" before adding the cached figure.
The lesson is the obvious one — a single call site is not the mechanic.

**Shape difference we keep deliberately.** Retail evaluates the footprint once
and caches the result on the unit (`+0x58`); we recompute per tick. That costs a
footprint's worth of lookups and gives the same answer for a static map, and it
means a map edit (a reclaimed patch) is picked up for free.

### How this was found, and the method that failed

A linear sweep of `.text` reported *zero* writes to `[reg+0x58]`, which is what
sent the first pass down the predicate reading. A BYTE-PATTERN search for the
encodings of `fstp dword [reg+0x58]` (`D9 5E 58` and its seven register
siblings) and `mov [reg+0x58], reg` found 25, of which exactly one was a float
store — the line above.

The lesson for anything else in this document: on x86, byte-pattern search for
an instruction encoding is reliable where linear disassembly is not, because a
sweep desynchronises on the first misaligned byte and silently skips whatever
follows.

## Income handicap multipliers

The same routine applies one of two multipliers to an income figure and
*subtracts* the result (`fmul` then `fsubp`), selected by a small integer:

```
0x4fc480: -0.7
0x4fc488: -0.5
```

Two accumulators are visible on the unit/player struct: `[esi+0xbc]` and
`[esi+0xd4]`, one per resource. The selector looks like a difficulty or handicap
setting rather than anything data-driven.

## Interface

- `SIDEDATA.TDF` is the whole HUD contract: sides, each side's `commander=`, the
  `[CANBUILD]` build tree, and pixel rects for every readout. See
  `src/tdf/sidedata.{h,cpp}`.
- The in-game command panel is `<PREFIX>GEN.GUI` (`ARMGEN`, `CORGEN`), keyed on
  the side's unit-id prefix, not its name.
- All interface art is one shared GAF, `anims/commongui.gaf`, addressed by
  sequence name: the panel plate is what the `.gui` root's `panel=` names, and
  each button's art is named after its gadget.
- The HUD frame is the side's `intgaf` (`anims/ARMINT.GAF`): `PANELSIDE`
  129x480, `PANELTOP` 513x32, `PANELBOT` 513x33 — 129 + 513 = the 640-wide
  screen.
- Interface art is indexed against the ORDINARY game palette
  (`palettes/PALETTE.PAL`), not `GUIPAL.PAL` — despite the name, that one maps
  these indices to unrelated saturated colours. The panel chrome is genuinely
  dark (`PANELSIDE` peaks at luminance 36) while `STAR` and parts of `PANELTOP`
  are bright, so the darkness is the art, not a decode or palette error.

## Wind — solved

From the tick at `0x4787a5`. `[0x51e654]` is the current wind and `[0x51e670]` a
countdown, both globals:

```
if (--countdown > 0) return;      ; most ticks do nothing at all
wind += rand() % 5 - 2;           ; a +/-2 random walk
wind = clamp(wind, minWind, maxWind);
countdown = rand() % 63;          ; next change in 0..62 ticks
```

So wind **steps and never interpolates**, and it *wanders* rather than sweeping:
two maps with identical min/max feel different because the walk spends its time
near wherever it started. Implemented in `World::tickWind`, drawing on the sim's
own RNG and folded into the state hash, because it feeds energy income.

Our first attempt was a smooth ~40s sine between the two bounds — the same
numbers describing a different mechanic.

### What a wind generator earns from that — solved, and it is not a clamp

The wind speed above is not the energy. Retail normalises it against a **fixed
5000** and *multiplies* the unit's rating by the result:

```
factor = min(currentWindSpeed / 5000.0, 1.0)
energy = WindGenerator * factor
```

Traced from the `.ota` keys inward. The strings `minwindspeed` / `maxwindspeed`
sit lowercased at `0x504c18` / `0x504c08` and are read at `0x43651c` / `0x43652c`
into the map object. The live wind speed lives at `+0x37eda`; `0x490d40` does
`fild [+0x37eda]; fidiv [+0x37ec8]` and stores the quotient as a factor at
`+0x37ede`; `+0x37ec8` is written exactly once, with the literal `0x1388` = 5000
(`0x4918ed`); and `0x490d5e` compares that factor against the double `1.0` at
`0x4fda10`, overwriting it with `1.0f` when it is larger. The consumer at
`0x401550` loads the factor and does `fmul [unit+0x1d2]`, the unit's
`WindGenerator`.

**This contradicts the obvious reading, and the data hides the contradiction.**
`min(WindGenerator, windSpeed)` is what the two field names invite, and it is
what we implemented. Both sides' wind generators rate `WindGenerator=30` while
maps advertise wind speeds in the *thousands* — Coast To Coast 0..3500, Fox Holes
100..6000, Etorrep Glacier 500..5000 — so that min always selected 30. Every wind
generator on every map with any wind at all produced its full rating, flat,
for ever: no weak-wind map, no gusting, and no reason for a map to state a range.

The 5000 is what makes the range mean something: it is the speed at which a
generator reaches its rating. Coast To Coast's 3500 ceiling is 70% of rating;
only a map advertising 5000 or more ever reaches the full 30; Dark Side ships
`0/0` and its wind generators are ornaments.

`TidalGenerator` is the same shape — `fld [factor]; fmul [unit+0x1d6]` at
`0x4015df` — but both tidal generators declare `TidalGenerator=1`, so the output
*is* the factor and the multiply cannot be told apart from an assignment. The
factor's global (`+0x14267`) is read at `0x4015df` and `0x488f92` and written
nowhere in `.text` that a displacement scan can see, so whether it is
`tidalstrength` verbatim or scaled is **not** established here. We use it
verbatim, which matches the shape and the one map value we can check.

## The map header's own defaults

The loader at `0x483793` validates each `.ota` field and substitutes a default
when it is missing or the file predates version `0x2000`:

| Field | Map offset | Default |
| --- | --- | --- |
| `minwindspeed` | `+0xd34` (int) | from the caller |
| `maxwindspeed` | `+0xd38` (int) | from the caller |
| `gravity` | `+0xd3c` (int) | `0x1fdb` (8155), after scaling by two constants |
| `tidalstrength` | `+0xd40` (float) | `0.5` |

## The stall — solved, and it is not a curve

TA has three resource-spending primitives, and every one is the same shape:

| | |
| --- | --- |
| `0x401220` | spend energy |
| `0x401260` | spend metal |
| `0x4012a0` | spend both |

```
if (player->resource < amount) return 0;    ; spend NOTHING
player->resource -= amount;
consumedAccumulator += amount;
return 1;
```

There is **no division anywhere in them** — no fraction, no scaling, no curve. A
consumer that cannot afford this tick's draw does not advance slowly; it does not
advance at all, and tries again next tick.

So "a stall slows everything down" is an emergent description, not a mechanic.
Each consumer independently succeeds or fails depending on what is left when its
turn comes, which is why a stalled TA base *stutters* — nanolathe beams flicker
on and off — rather than easing smoothly to half speed.

We had implemented the intuitive reading: pay what you can afford, credit that
fraction of the work. It produces a visibly different game, and it is now gone.
`Resource::share` went with it (it was only ever written).

**Not yet matched:** a unit's standing `EnergyUse`/`MetalUse` still settles in
bulk here, where retail bills each unit through the same all-or-nothing call.
The difference only shows in *which* units go dark first when a base browns out,
and it belongs with the on/off toggle work.

## The player struct

From the stat serialiser at `0x4660fe`:

| Field | Offset | Type |
| --- | --- | --- |
| Energy | `+0x8c` | float |
| Metal | `+0x98` | float |
| TotalEnergyProduced | `+0xac` | double |
| TotalMetalProduced | `+0xb4` | double |
| TotalEnergyConsumed | `+0xbc` | double |
| TotalMetalConsumed | `+0xc4` | double |
| EnergyWasted | `+0xcc` | double |
| MetalWasted | `+0xd4` | double |
| PlayerEnergyStorage | `+0xdc` | float |
| PlayerMetalStorage | `+0xe0` | float |

Retail tracks produced, consumed AND wasted per resource as running doubles —
so overflow is measured, not merely discarded.

## Building

| Key | UnitDef offset | Type |
| --- | --- | --- |
| `BuildTime` | `+0x1ea` | int32 |
| `WorkerTime` | `+0x1fe` | **int16** |
| `BuildDistance` | `+0x210` | int16 |

**`sizeof(UnitDef)` is 585 bytes**, from the index arithmetic at `0x404f98`:
`id << 6` then `+ id` (= `id * 65`), then `lea edx, [ecx + ecx*8]` (`* 9`). The
definitions live in one flat array reached through the game-state pointer at
`0x511de8`.

Build PROGRESS is a plain fraction (`0x41bacd`): `work / BuildTime`, clamped to
[0, 1], then used to interpolate two further UnitDef floats at `+0x186` and
`+0x18a`. That matches the model the engine already uses, where `BuildTime` is a
work quantity rather than a duration.

**Still open:** what accumulates into `work` per tick. Our `buildTime /
workerTime` seconds is the community formula and is consistent with the fraction
above, but it has not been read out of the binary.

### A false lead worth recording

`0x404fb0` also reads `BuildTime` next to `WorkerTime` and computes

```
round( (BuildTime * 0.3) / (WorkerTime / 30) )      ; /30 via the 0x88888889 magic
```

into a countdown at `unit+0x3a` — which looks exactly like a build timer, and is
not one. For ARMLAB built by a commander it gives ~203, where the unit takes
around 22 s in play. The routine turns out to be RESURRECTION: a few instructions
earlier it bails to an error string, `"Ressurection failed"`.

Identify the routine before trusting the arithmetic. The numbers alone were
perfectly plausible.

### TA has a resurrection code path

That string, and the routine behind it, exist in the retail binary. No shipped
unit reaches them: a survey of all 815 FBIs in the Commander Pack finds no
`canresurrect` key at all. Noted because this engine removed resurrection as a
Kingdoms mechanic — which is still right for retail behaviour, but it is TA
vestigial code rather than something TA never had.

## Mission objectives — solved, including the win/lose split

A mission's objectives are `.ota` GlobalHeader keys, and the engine turns them
into **two lists of small polymorphic objects**. One factory at `0x48e000`
-`0x48e9xx` walks the keys in a fixed order; for each it calls the get-int reader
(`0x4c46c0`), and when the key is present it `new`s an object of a per-key size,
writes a per-key vtable, and appends it to one of two arrays on the owning object:

| | array | count |
| --- | --- | --- |
| **victory** | `[this + 0x00]` | `[this + 0x40]` |
| **defeat** | `[this + 0x44]` | `[this + 0x84]` |

Sixteen pointers each. **Which array a key lands in is the win/lose split**, and
it is not guessable from the names:

| victory — what the player does | defeat — what is done to them |
| --- | --- |
| `KillEnemyCommander` (7) | `CommanderKilled` (123) |
| `DestroyAllUnits` (90) | `AllUnitsKilled` (163) |
| `KillAllMobileUnits` (9) | `AllUnitsKilledOfType` (52) |
| `KillAllOfType` (40) | `UnitTypeKilled` (18) |
| `KillUnitType` (32) | `DeathTimerRunsOut` (21) |
| `CaptureUnitType` (32) | `AnyUnitPassesX` (2) |
| `BuildUnitType` (8) | `AnyUnitPassesZ` (1) |
| `VictoryTimerRunsOut` (4) | |
| `MoveUnitToRadius` (18) | |

(Counts are how many of the 272 shipped `.ota` files declare each.)

Two pairs would trap anyone reading the names. `CommanderKilled` and
`KillEnemyCommander` are **different keys on opposite lists** — the common one,
123 missions, is the defeat. And `AllUnitsKilledOfType` is a defeat: AC01 declares
`AllUnitsKilledOfType=ARMGATE` and the only ARMGATE on that map belongs to the
PLAYER, so the key means *protect the gate*. Reading it as a victory inverts the
mission.

### The objects

Six vtable slots. Slots 1-3 are event hooks that most types leave as the shared
no-op `ret 4` (`0x48ea10`/`0x48ea20`/`0x48ea30`); slot 0 is the "is it satisfied"
query, whose shared implementation (`0x48ea00`) is just `return this->+4`, a
cached flag the hooks set.

| key | size | vtable |
| --- | --- | --- |
| `AllUnitsKilled` | `0x10` | `0x4fd800` |
| `DestroyAllUnits` | `0x0c` | `0x4fd948` |
| `KillAllMobileUnits` | `0x14` | `0x4fd928` |
| `CommanderKilled` | `0x0c` | `0x4fd818` |
| `MoveUnitToRadius` | `0x40` | `0x4fd830` |
| `DeathTimerRunsOut` | `0x10` | `0x4fd7a8` |

`CommanderKilled` is the clearest worked example: it overrides slot 1 (the
unit-died hook) at `0x48f6b0`, which takes the dead unit, reads its def at
`+0x92`, looks up the owner's commander name out of a per-player table, compares
the two by string, and on a match sets its cached flag. `AllUnitsKilled`
overrides slot 0 instead and computes on demand -- assume satisfied, then walk
the live-unit list and clear the flag if anything still qualifies.

## The sound-event table — solved

`TotalA.exe` carries the whole sound-event list as a 24-byte record array at
`.data:0x005086f0`, laid out `{u32 id, u32 a, u32 b, char* key, char* label,
u32}`. Walking it gives all 23 events in id order:

| id | a | b | key | on-screen label |
|---|---|---|---|---|
| 1 | 10 | 0 | `select` | |
| 2 | 9 | 20 | `underattack` | Under Attack |
| 3 | 4 | 2 | `activate` | |
| 4 | 4 | 2 | `deactivate` | |
| 5 | 5 | 1 | `ok` | |
| 6 | 3 | 4 | `arrived` | Arrived |
| 7 | 8 | 1 | `cant` | Cannot Comply |
| 8 | 3 | 3 | `unitcomplete` | Nanolathe Complete |
| 9 | 4 | 2 | `build` | |
| 10 | 3 | 1 | `repair` | |
| 11 | 2 | 1 | `working` | |
| 12 | 7 | 1 | `load` | |
| 13 | 7 | 1 | `unload` | |
| 14 | 7 | 1 | `cloak` | Cloaked |
| 15 | 7 | 1 | `uncloak` | Visible |
| 16 | 4 | 1 | `capture` | |
| 17-22 | 10 | 0 | `count5`..`count0` | five..zero |
| 23 | 10 | 0 | `canceldestruct` | Self destruct terminated |

What this settles for the port:

* **The order-acknowledgement event is `ok`, and there is exactly one of it.**
  The engine asks for `move` / `attack` / `guard` and aliases all three onto
  TA's `ok1`; that produces the right sound, and this confirms retail has no
  per-order distinction to reproduce. The alias was inferred from WAV names
  (`ok1=kbarmmov`) and is now checked against the binary.
* **The keys carry no digit** — `select`, not `select1`. `SOUND.TDF` spells them
  `select1` / `ok1` / `arrived1` / `cant1`, so retail appends a variant index at
  lookup; single-variant events (`underattack`, `count0..5`, `canceldestruct`)
  appear undigited in both.
* The engine plays 4 of these 23. `build`, `repair`, `working`, `load`,
  `unload`, `cloak`, `uncloak`, `capture`, `activate`, `deactivate`,
  `unitcomplete`, `arrived`, `underattack`, the countdown and
  `canceldestruct` all exist in the shipped `SOUND.TDF` and are never triggered.

Columns `a` and `b` were NOT established. `a` runs 2..10 and `b` 0..20, and the
values are suggestive — `select` and the countdown sit at a=10, `working` at
a=2; `underattack` alone has b=20 where most events have b=0..4 — which reads
like a priority and a repeat cooldown. That is a guess from the numbers' shape,
not something traced through the code, and it is recorded here only so the next
person starts from the measurement rather than re-deriving the table.

## The nanolathe emit piece — solved

The COB-callback name table contains **`QueryNanoPiece`**, alongside the
`QueryPrimary` / `AimFromPrimary` / `SweetSpot` entries the engine already uses.
Retail asks the unit's own script which piece the nanolathe emits from, exactly
as `QueryWeapon` supplies a muzzle: the script writes the piece index to its
out-param local 0.

The shipped scripts agree — **51 of the 53 builders in the Commander Pack define
`QueryNanoPiece`** (the exceptions are `armcarry` and `corcarry`). The engine
now calls it. It previously guessed at piece NAMES, a list measured from the
models rather than invented (`nano1`/`nano2`, `nanospray`, `nanogun`,
`nanopoint`, `nano`, `nanolath`, `nozzle`), but one that could only ever cover
the 29 builders whose piece happens to be named predictably.

### The nanolathe COLOUR is still not established

Recorded as unresolved rather than left implied. The engine draws the beam in an
invented pale green-white. It is not authored art: sweeping every shipped GAF
for a nano/lathe sequence turns up only GUI buttons (`ARMBUILD`, `CORBUILD`) and
building scenery, so retail draws the lathe procedurally and its colour is a
constant in the draw routine. That routine has no string to anchor a search on,
and it was not found. The beam's SHAPE — a line from the script's nano piece to
the build site, appearing and stopping with the work — is right; the ramp is
still a guess.

## Open questions

- What accumulates into build `work` per tick.
- How `WindGenerator`/`TidalGenerator` ratings convert to energy per second.
- Whether a unit's standing `EnergyUse`/`MetalUse` is billed per unit (retail) or
  in bulk (ours) -- see the stall section.
