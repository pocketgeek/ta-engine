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

## Open questions

- What accumulates into build `work` per tick.
- How `WindGenerator`/`TidalGenerator` ratings convert to energy per second.
- Whether a unit's standing `EnergyUse`/`MetalUse` is billed per unit (retail) or
  in bulk (ours) -- see the stall section.
