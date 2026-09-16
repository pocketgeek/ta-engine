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

## Open questions

- The stall curve: is a starved consumer slowed strictly proportionally?
