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

**A caveat that applies throughout.** Most of what follows came from a LINEAR
sweep of `.text`, which on x86 is not trustworthy: a single misaligned byte
desynchronises the decoder until it happens to resynchronise, so "there is
exactly one reference to X" should be read as "one reference was found". Absence
is not evidence here. Positive findings quoted with an address were each read
back in context and are solid.

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

## Metal extraction: `ExtractsMetal` is a PREDICATE, not a multiplier

This is the interesting one, and it contradicts the natural reading of the FBI.

At `0x401480`, in what is evidently the per-unit economy tick (`esi` = the unit,
`[esi+0x92]` = its UnitDef):

```
mov  ecx, [esi + 0x92]          ; unit->def
fld  dword [ecx + 0x1ce]        ; ExtractsMetal
fcomp dword [0x4fc478]          ; ...against 0.0
fnstsw ax
test ah, 0x41                   ; C3|C0 -> "less or equal"
jne  0x4014ca                   ; <= 0: not an extractor
; > 0 falls through to the extractor path, which loads...
fld  dword [esi + 0x58]         ; ...a PER-UNIT float
```

`ExtractsMetal` is loaded exactly once in the whole binary, and only to be
compared with zero. It is never multiplied by anything. The yield an extractor
actually contributes is a float already sitting on the unit at `+0x58`.

So the per-tick formula is not `ExtractsMetal x (metal under the footprint)`.
Retail evaluates the footprint once — presumably when the building is placed or
completed — and caches the result on the unit; the tick just adds it.

**Not yet found:** where `+0x58` is written. No `fstp`/`mov` to `[reg+0x58]`
turned up, which given the caveat above most likely means the sweep desynced
around it rather than that no writer exists. Until that is located, the exact
combining rule — and in particular whether `ExtractsMetal`'s *magnitude* matters
at all, or only its sign — is still open.

**What this means for the engine.** `World::extractorYield` currently computes
`sum(per-cell richness under the footprint) x ExtractsMetal` every time it is
asked, which reproduces the right order of magnitude (ARMMEX's `0.001` over a
3x3 patch of `metal=127` gives ~1.14 metal/s). That matches retail's *output*
but not its *shape*: retail caches, and may not use the magnitude at all. Worth
revisiting once the writer is found.

The non-extractor branch falls through to `MakesMetal` (`+0x1d2`), compared
against 0.0 the same way at `0x401550` — a flat rate for metal makers.

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

## Open questions

- Where unit `+0x58` is written, and whether `ExtractsMetal`'s magnitude is used.
- The stall curve: is a starved consumer slowed strictly proportionally?
- How fast wind varies between the `.ota`'s `minwindspeed` and `maxwindspeed`,
  and whether it interpolates or steps.
