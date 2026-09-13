# Retail engine notes (`KINGDOMS.icd`)

Findings from a static analysis of the retail *Total Annihilation: Kingdoms*
executable — `Kingdoms.exe` is a small stub loader; `KINGDOMS.icd` is the real
2.27 MB PE32 (i386, MSVC release build, ImageBase `0x400000`). The build kept its
C++ RTTI and a number of debug-`printf` strings, which together expose the class
model, the config-key schema, and several gameplay formulas. `ironplague.icd` is
the Iron Plague expansion on the same engine.

Analysis was static only — strings, RTTI names, and disassembly of specific code
paths read against the game's own data files. No engine code or assets are
reproduced here; this documents an interface for a clean-room recreation.

## Technology stack (imports)

- **DirectDraw** (`DDRAW.dll`) — a *software* 2.5D renderer; no Direct3D. An
  optional **Glide** path (`glide2x/3x`) targets 3dfx Voodoo. The 3DO models are
  CPU-rasterised to sprites, which is the source of the isometric look.
- **DirectPlay** (`DPLAYX`) + WinSock — networking. The sim is deterministic
  lockstep over discrete *game ticks* (`"…sent more than %lu game ticks ago"`).
- **Miles Sound System** (`mss32`) — audio. **Bink** (`binkw32`) — cutscene video.
- A hot-loaded `Fire.dll` renders flame/particle surfaces (`FireSurfHandler`).

## Object model (from RTTI — 226 game classes)

**Weapons** — `WeaponType` maps one subclass per FBI `type`/`subtype`:
`Melee`, `Ballistic`, `DroppedBallistic` (bombs), `Guided` (homing), `Wandering`,
`RemoteEffect`, `LineOfSight` (hitscan: `Fire`/`Lightning`/`MindControl`/
`TurnToFrozen`/`TurnToStone`), `Earthquake`, `Hailstorm`, `AreaMindControl`,
`AreaTurnToFrozen`, and `ATTRIBUTE_ADJUSTER` (stat buffs). **Status effects and
capture are weapon subtypes**, not unit flags (`turntostone`, `turntofrozen`,
`paralyze`, `mindcontrol` are real data values).

**Navigation** — `PathNavigator` / `VTOLNavigator` with `Local*` vs `Remote*`
variants (the lockstep prediction split); `NavGoalCircle/Rect/Ring` are
area-shaped move goals.

**AI** — role-based squads: `AIBaseSquad` (defence), `AIBackupSquad`,
`AIStrikeSquad`, `AIVTOLSquad` (air).

**Particles/SFX** — `BuildEnergy`, `DamageFlame`, `DeadSpray`, `Flame`, `Smoke`,
`Spray`, `Circle`, `Hail`, `Wake` (ship wakes), plus texture emitters.

**Missions** — one class per victory/defeat condition (`KillEnemyCommander`,
`MoveUnitToRadius`, `CaptureUnitType`, `DeathTimerRunsOut`, …).

## Formulas (disassembled)

**Veterancy** (`getLevel @0x519310`, `getMult @0x5193c0`), constants read from
`.data`/`.rdata`:

```
level = min(kills, 10)                 // one level per kill, cap 10
mult  = 1.0 + 0.10 * level             // base 1.0, +10%/level → up to 2.0×
```

The multiplier scales the instance's **attack** and **armour** up and **divides**
a reload/cooldown field (veterans hit harder, tank more, fire faster); max HP is
unchanged. `noveteran` opts out; `veteranmodel` swaps the mesh at max rank.

**Build/repair rate** (`@0x4d4b30`, the `hp=%i buildtime=%i buildrate=%i` system):

```
rate = round( vetMult * targetMaxHP * workertime * param * (1/3000) ), min 1
```

**Damage** has no single formula — each `WeaponType` subclass applies damage
through its own virtual method; all paths route through the attacker's
veteran-scaled attack and the victim's veteran-scaled armour, on top of the
per-target `DAMAGE` category table.

## Config schema

The TDF parser (`readInt @0x543190`, `readFloat @0x5431c0`, `readString @0x5432c0`)
recognises **336 keys**. The recreation's stat model was validated field-for-field
against the engine's own debug dumps (`Damage/Reload/ManaPerShot`,
`Range/Velocity/AreaOfEffect`, `MaxMana/ManaRecharge`,
`MaxVel(water,road)/ManeuverLeash`, `hp/buildtime/buildrate`). Some `UNIT_DEF`
offsets: `maxdamage +0x1be`, `experiencepoints +0x1c2`, `workertime +0x21a`;
weapon `range +0x90`, `damage +0x126`. Keys the shipped data never uses but the
engine supports: `kamikaze`, `digger`, `teleporter`, `amphibious`, `antiweapons`,
`toairweapon`, `immunetoparalyzer`, `burst`/`burstrate`, `weaponswitching`, …

## Data layout the engine reads

`*.hpi` (SQSH archives, checksummed) · `units/*.fbi` · `MOVEINFO.tdf` (movement
classes → the real `maxslope`/`max·minwaterdepth`) · `gamedata/SIDEDATA.tdf` ·
`gamedata/Gods.tdf` (god timing) · `gamedata/{explosions,effects,damageflames,
soundclasses}/*.tdf` · `CanBuild/<builder>/*.tdf` · `Maps/*.ota` + `*.tnt` ·
`*.gaf`/`*.taf`/`*.3do`/`*.cob`.

## Weapon in-range model (icd, 2026-09-10)

The in-range predicate is a per-weapon-subclass virtual (vtable slots 3/4;
MeleeWeapon vtable `0x5f3af8`, base WeaponType `0x5f3710`):

- **Base `WeaponType::inRange` @0x530580** (point variant @0x530500): `range==0`
  → always true; else strictly **2D centre-to-centre** from the two units'
  position dwords (`unit+0x68` x, `+0x70` z, 16.16 fixed; y at `+0x6c` is
  skipped), 64-bit squared compare `(dx²+dz²)>>32 <= range²` (inclusive). Used
  unchanged by Guided/LineOfSight/Wandering weapons. No footprint subtraction,
  no weapon-piece origin.
- **BallisticWeapon @0x52bc60**: solves the launch arc first (helper `0x52bd10`,
  the only user of dy; `0x8000` = no solution → out of range), then falls
  through to the base centre test.
- **MeleeWeapon @0x52b980** (point @0x52b8a0): **never reads `range`**. Each
  unit's footprint box is position ∓ `foot<<19` (∓ 8px·footCells, cell-snapped);
  in range iff on BOTH axes `|centreΔ| − halfA − halfB < 0x80000` (= 8 px) —
  i.e. the boxes are within half a cell of touching. (`range` in melee TDFs —
  10..250 in shipped data — is dead weight.)
- Weapon TDF parse @0x530780: `range` readInt → `[WeaponType+0x90]`, clamped
  min 2; `minrange` → `+0x94`. Unit-def weapon-type pointers at `+0x1aa`
  (3 dwords). Range-ring UI strings @0x4d6823; debug dump @0x4fbf6b.

## Ship / no-walk mover animation (icd + COB, 2026-09-10)

Ships have no `walk` script. The engine drives them with the **`MoveRate`**
callin on move start/stop (arg > 0 = moving); the COB's `Create` ambients
(`MotionControl`, wake/oar controllers) poll the static that `MoveRate` sets
and select `slowrow`/`row`/`fastrow` from **`GET_UNIT_VALUE 29`** (current
speed, thresholds 25/75 ⇒ percent of max). `TurnDirection(deg)` steers the
rudder/sail trim; `WindChange` orients sails/flags (FBI `wind=1`). Resetting
such a unit's VM kills the Create ambients permanently — nothing restarts them.

## Movement: retail has NO global pathfinder (icd, 2026-09-12)

Asked because our own movement had become the dominant sim cost. It turns out
we diverged from retail badly here, and the divergence is what costs us.

**No pathfinding class exists.** Of the 356 RTTI class names in the binary, the
only movement-related ones are `Navigator`, `PathNavigator`,
`RemotePathNavigator`, `VTOLNavigator`, `LocalVTOLNavigator`,
`RemoteVTOLNavigator`, and the goal shapes `NavGoal`, `NavGoalCircle`,
`NavGoalRect`, `NavGoalRing`, `NavGoalVTOL`, `NavGoalVTOLm`. There is no
`PathFinder`, `Route`, `Waypoint`, `Region`, `Sector` or graph class of any
kind. (`Local*`/`Remote*` is the lockstep prediction split.)

**`PathNavigator` contains no search.** Its vftable is at `0x5f2a24`; every
method is small (11-365 instructions) and nearly loop-free. The largest, slot 8
`0x4e5700`, is a bit-stream serialiser (word/bit cursor, `1 << bit`, wrap at 32,
grow buffer) -- navigator state being saved, not a solver.

**The navigator object**, as read off slots 1 and 2:

| offset  | meaning                                                        |
|---------|----------------------------------------------------------------|
| `+0x04` | the `NavGoal` (area goal: circle / rect / ring)                |
| `+0x08` | unit                                                            |
| `+0x08 + 4*i` | a SHORT array of 16-bit (x,z) points, i indexed by +0x10c |
| `+0x10c`| how many points are live (tested against 2 and 3)              |
| `+0x110`| tick stamp; cleared when older than `globalTick - 6`           |
| `+0x114`| flag bits (bit0 = target valid, bit3, bit4)                    |

**Per-tick update** is slot 2, `0x4e5150`: ask the goal via its vtable whether
it is satisfied (`[goal+0x10]`, `[goal+0x2c]`), then

```
movsx edx, word [esi+0x12]   ; target z        movsx eax, word [unit+0x72] ; unit z
movsx ecx, word [unit+0x6a]  ; unit x          sub / imul / imul / add     ; dx^2 + dz^2
cmp   edx, 0x19              ; <= 25  ->  arrived (radius 5)
```

**Steering** is slot 1, `0x4e54e0`: fetch the goal point through the NavGoal
vtable (`[goal+0x20]`), compute float deltas to it, and set the current target
point, stamping `+0x110` with the tick. The stamp's 6-tick expiry is the whole
re-plan cadence.

**So retail steers each unit toward an AREA GOAL, keeping at most two or three
intermediate points, refreshed about every 6 ticks.** No global search, no
precomputed connectivity, nothing shared between units. That is why it ran
hundreds of units on a 1999 Pentium, and it matches the TA-family feel: fluid
local movement that occasionally wedges.

**What we do instead** (and what it costs): full-map Dijkstra FLOW FIELDS per
(domain, footprint, goal block), plus per-unit A*. Flow fields are OUR
invention -- added 2026-09-03 to fix a crowd/corner jam -- not retail. Measured
server-side on a 120s 8-player game they were 68% of all stalled time, and even
after four rounds of optimisation (e3228e1 and before) they remain ~59%, with
A* another ~23%. The crowd jam they were built to fix is very likely a symptom
of OUR movement model (solid units + separation), which retail did not have in
that form.

NOT established: whether some free function (not a class) does a coarse global
search somewhere. None was found, and the absence of any search-shaped routine
in the navigator region is strong negative evidence, but it is not proof.

## Movement: the local steerer (icd, 2026-09-12, PARTIAL)

Follow-on from the section above. Retail has no path search, so everything that
gets a unit around an obstacle lives in the mover, and this is what that mover
is built out of. Addresses are entry points; the direction-choosing logic that
consumes them is only partly mapped, so treat the last section as a lead rather
than a spec.

**`0x507fb0` -- area rater.** Walks a RECTANGLE of map cells (14-byte records,
stride `0xe`) in a nested row/column loop and takes the MINIMUM score over it:

```
test byte [esi+0xd], 0x80    ; per-cell flag (road)
jne  keep                    ; road keeps the higher score
cmp  dword [edi], 6          ; running minimum
jle  keep
mov  dword [edi], 6          ; ordinary ground clamps it to 6
add  esi, 0xe                ; next cell
```

So ordinary ground scores 6 and road 7 -- the 1.2x road preference our
NavGrid::setRoads comment already cites. The score is GRADED, not boolean.

**`0x4db640` -- passability query.** Calls the rater, returns early if it comes
back `-1` (impassable terrain), then walks the live-unit list checking flag
`0x1000000` at `+0x130`. So one call answers "can a body of this size stand
here, and what does it cost", terrain and occupancy together. This is where
"a parked unit is impassable, a moving one merely expensive" is expressed: the
occupant lowers the score rather than vetoing the cell.

**The threshold is 4.** Every caller compares the result against it
(`cmp eax, 4` / `jle` / `jl`), so the scale is: -1 impassable, below 4 refused,
4..5 passable but costly, 6 ground, 7 road.

**`0x4dba80` -- the mover.** Two uses of the query are clear:

  * Forward probe: take the unit's heading from `+0x7e`, project one step of
    `0x100000` through the sin/cos tables (`0x5360bf` / `0x5360f3`), and query
    the resulting position. `cmp eax,4 / jle` -> blocked.
  * A 3x3 neighbourhood scan: outer loop over x offset -1..+1, inner over z
    offset -1..+1 in `0x100000` steps, querying each and bailing on the first
    below threshold.

**Blocked -> re-read own waypoints, clamp, slow.** When the forward probe fails
the mover sets one of two refusal states (flags `0x100` / `0x200` at navigator
`+0x36`, each accumulating a per-type value from `UnitType+0x249` into `+0x30`
-- the clamp-and-slow already ported in 080d288), runs the 3x3 scan, and then
calls navigator vtable slot 3 with `(outBuf, 3)`:

```
mov  ebx, [ebp+0xc]              ; arg2 = 3, how many points wanted
mov  eax, [ecx+0x10c]            ; the waypoint COUNT
cmp  esi, eax / lea edi,[eax-1]  ; clamp the index to count-1
movsx eax, word [ecx+edi*4+0xc]  ; point[i].x, the same 16-bit array
shl  eax, 0x10                   ; -> 16.16 world units
movsx eax, word [ecx+edi*4+0xe]  ; point[i].z
```

That is an ACCESSOR: it exports the navigator's own two-point segment as world
coordinates. `Navigator`'s base version is `ret 8`, a stub, and all four
NavGoal shapes share an unrelated `mov eax,1; ret`. So the goal object is NOT
consulted here and is not an active participant -- an earlier revision of this
document said it was, on a misread of which object the vtable belonged to.

CONCLUSION SO FAR, and it is uncomfortable: no distinct obstacle-avoidance
algorithm has been found. What retail has is the graded passability score, the
forward probe, the 3x3 scan, and clamp-and-slow -- all of which we now have.
Either the avoidance is subtler than these pieces suggest (the 3x3 scan's
result is used somewhere not yet traced), or retail genuinely wedged as much
as we do and the difference we think we remember is not there. Resolving that
needs the rest of 0x4dba80's control flow read properly, not more guessing.

## RETRACTED: the "wall-follower" is not in the movement path (2026-09-12)

An earlier version of this section claimed that `0x4140d4`..`0x414eb0` is
retail's answer to a blocked unit -- a wall-following obstacle tracer -- and
that implementing it would fix units stopping dead at a parked body. **That
claim is withdrawn.** The code exists and does what was described, but it is
not reachable from unit movement, so it is no evidence about how retail moves
anything.

What the routine genuinely is: a contour tracer. `0x5f304c` / `0x5f3054` really
are the 8-direction offset tables
(`dx = {0,-1,-1,-1,0,1,1,1}`, `dz = {-1,-1,0,1,1,1,0,-1}`, compass order),
the direction at `+0x108` really is rotated and masked `& 7`, candidates really
are scored against the threshold 4, and it really does give up on returning to
its start cell facing its start direction, within a time-sliced step budget
(`+0x48` against the cap at `+0x165`, phase at `+0x60`). All of that held up.

What was never checked: whether anything in the movement path calls it. It does
not. An upward reachability sweep of the whole call graph gives `0x4146e0` just
NINE ancestor functions in the entire binary --
`0x415b10, 0x416430, 0x4261f0, 0x4f6c70, 0x526310, 0x526740, 0x527060,
0x527360` -- and the mover (`0x4dba80`), the navigator update (`0x4dc800`) and
the passability query (`0x4db640`) are none of them. `0x4261f0` is a handler in
the CHEAT dispatch table at `0x605c2c`, whose neighbouring entries are the
strings "HalfShot", "NowISee", "MakePoster" and "ManaMe". This is map/poster
rendering or a similar offline analysis, not steering.

The methodological error is worth naming, because it is the second time in one
day: the routine was found by searching for callers of a function believed to be
movement-related, and its shape (direction tables, passability threshold, give-up
test) was so convincingly "obstacle avoidance" that the shape was accepted as
proof of purpose. Shape is not provenance. Establish the CALL PATH from the
subsystem you care about before concluding a routine belongs to it.

Still true: `0x4139d0` is a per-cell query rather than a search, so the old
`sim.h` note calling it "its path search" remains wrong.

### What a second, top-down pass established (same day)

Working DOWN from the navigator instead of up from a suggestive routine:

  * **The navigator holds up to 64 waypoints, not two.** `0x4e4ea0` is its
    point-list setter: it clamps the incoming count at `0x40` and block-copies
    that many dwords into `+0xc`, storing the count at `+0x10c`. So the
    "two-point segment `[position, goal]`" description -- which I restated as
    recently as the retraction above -- is wrong as a statement about the
    navigator. `movl $0x2,0x10c` at `0x4e5635` is just ONE caller installing
    two points. The navigator is a general multi-waypoint follower, and the
    mover's `pop_front(1)`-then-recurse (`0x4dbf36` / `0x4dbf7e`, vtable slot
    11 = `0x4e50a0`) is it advancing along that list.
  * **The mover's refusal flags are write-only.** Nothing in the binary reads
    bit `0x20`, `0x100` or `0x200` of navigator `+0x36`. The mover clears
    `0x07E0` on entry and sets them on refusal, and no consumer exists. They
    are status, not control -- so the response to being blocked is entirely
    inside the mover: clamp, slow (the `+0x30` budget against the global at
    `[0x62d55c+0x19f44]`), return.
  * **A real route producer exists** and feeds the navigator: `0x414450`,
    `0x415040`, `0x415b10` and `0x416430` all call the setter above, and that
    cluster contains the contour tracer retracted earlier. So the tracer IS
    part of route production; it simply is not reached FROM the mover, which
    is why the reachability test that produced the retraction was the wrong
    test.

### RESOLVED: retail HAS a pathfinder, reached through a singleton pointer

The link is not a call at all, which is why a direct-call graph could never
find it. It is a GLOBAL POINTER.

  * `0x4e6060` does `new(0x22b)` and runs `0x415f80` on the result -- a
    constructor, zeroing `+0x0..+0x28` -- then stores the object into the
    global slot `[0x62d55c + 0x19e70]`. That is a SINGLETON of the
    `0x414450 / 0x415040 / 0x415b10 / 0x416430` class, the same class that
    owns the contour tracer at `0x4146e0`.
  * The navigator's `setDestination` (vtable slot 1, `0x4e54e0`) loads that
    same singleton at `0x4e5502` and calls `0x415f30` on it -- cancel/re-register
    the pending request for this navigator. `0x416430` calls `0x415f30` too,
    which is the completion side.
  * The singleton installs results back into a navigator through `0x4e4ea0`,
    the point-list setter that accepts up to 64 waypoints.
  * When no route is available, `setDestination` falls back at `0x4e5632` to
    `count = 2` -- the unit's own cell and the goal -- and that fallback is
    SKIPPED when bit 0 of `+0x114` says a real route is already in hand.

So the shape is: an asynchronous path request against a singleton pathfinder,
a multi-waypoint route installed when it completes, and a straight two-point
segment as the interim fallback.

**This overturns "retail has no path search of any kind."** That claim was
made earlier today, written into this file, and used to justify setting
`pathBudget_ = 0` in our sim -- i.e. no route search at runtime, units steer
straight at the goal. It is wrong. The earlier `sim.h` note, which said retail
bakes per-class grids and "its path search reads that", was closer to the truth
than the claim that replaced it.

The two-point segment reading was not wrong, but it was the FALLBACK, not the
mechanism -- observed at the one call site that installs it and generalised.

Not yet established: what the search actually optimises, how requests are
scheduled and throttled, and how the tracer participates. Those need their own
pass. What IS established is that runtime routing exists, so our
`pathBudget_ = 0` is a deviation from retail rather than fidelity to it.

### The pathfinder, end to end (2026-09-12)

Complete architecture, established by following the singleton rather than the
call graph. Addresses are entry points unless noted.

**The object.** `[0x62d55c + 0x19e70]` holds a singleton, `new(0x22b)` +
constructor `0x415f80`, created at `0x4e6060` and cleared at `0x4e60b6`.

**Configuration** (`0x4252e0`): reads a setting, then
`+0x221` = base work budget, `+0x225` = `base * clamp(pct, 5, 1000) / 100`,
with two booleans at `+0x229` / `+0x22a` for the -2 / -1 / 0 special values.
So pathfinding effort is a user-facing quality knob.

**Per-frame scheduler** (`0x416430`): walks the request lists of all 10
players, counting pending requests into two buckets -- `B` for those flagged at
`+0x24e7`, `A` for the rest. If none, it bumps an idle counter at `+0x1a5` and
returns. Otherwise the per-request quantum is

    quantum = +0x225 / (A + 5*B)

so a flagged request gets FIVE TIMES the share. It zeroes `+0x165`, which is
the work cap the search meters itself against, then iterates the players.

**Per-request step** (`0x415b10`): runs one search step and switches on it --
`-2` = failed (sets `+0x5c`), `-1` = not finished (charges 30 to the work
counter `+0x48`, expands, retries via `0x414450` / `0x415f10`), `0` = done,
build the route and install it.

**The search itself** (`0x4146e0`) is NOT A*. There is no open list, no
priority queue and no cost-to-goal ordering anywhere in it. It is an
incremental obstacle-boundary tracer:

  * state machine at `+0x60` (0 = init, then 1..3);
  * `0x413e50` returns distance to the goal, and `+0xcc` keeps the BEST
    (minimum) distance reached so far -- the characteristic bookkeeping of a
    "bug" algorithm that follows an obstacle outline and leaves it when it can
    improve on its closest approach;
  * movement uses the 8-direction tables `0x5f304c` / `0x5f3054`
    (`dx = {0,-1,-1,-1,0,1,1,1}`, `dz = {-1,-1,0,1,1,1,0,-1}`, compass order)
    with the direction at `+0x108` rotated and masked `& 7`;
  * every candidate cell is scored by `0x4139d0` against the threshold 4;
  * it terminates on returning to its start cell facing its start direction;
  * it is time-sliced: work accumulates into `+0x48` and the step returns once
    it passes the cap the scheduler wrote to `+0x165`, resuming next frame.

**Hand-off to the navigator.** On success the route is installed through
`0x4e4ea0`, which clamps at 64 waypoints. The navigator's `setDestination`
(vtable slot 1, `0x4e54e0`) cancels/registers with the singleton via
`0x415f30`, and installs a straight TWO-POINT segment -- own cell plus goal --
at `0x4e5632` as the interim, skipping it when bit 0 of `+0x114` says a real
route is already in hand. The mover walks the list, popping via slot 11
(`0x4e50a0`).

So: asynchronous, budgeted, incremental boundary-tracing search; straight-line
movement until it returns; up to 64 waypoints when it does.

**Two of my own claims this file carried are now settled.** "Retail has no path
search of any kind" was wrong -- routing exists. But the observation behind it
was right: there is no A*, no open list, no priority queue. Both halves matter,
and conflating "no A*" with "no pathfinding" is what produced the error. And
the retraction that said the tracer at `0x4146e0` is not in the movement path
was itself wrong: it is the search. It is not reached FROM the mover, which is
what I tested; it is reached from the scheduler, and its output is handed to
the navigator that the mover then follows.

### The search algorithm in detail, and the port plan (2026-09-12)

`0x4146e0` is a two-phase incremental "bug" search over the 16px cell grid,
driven by a state machine at `+0x60`.

**State 0 -- init.** `0x413e50(start)` gives distance to the goal, kept as the
running BEST in `+0xcc`; zero means already there. Score the start cell with
`0x4139d0`; below 4 it gives up immediately. Otherwise seed `+0xd0/+0xd4` with
the start cell and go to state 1.

**State 1 -- greedy march** (8 work per step). Take the delta from the current
cell to the goal, convert it to one of 8 compass directions with `0x415040`,
step, and score the destination with `0x4139d0`:

  * `< 4` -- blocked. Record the current cell as the trace origin in
    `+0xf0/+0xf4` and switch to state 2.
  * `== 4` -- passable but occupied. Bump `+0xd8`; on the 3rd, switch to
    tracing as above.
  * `== 7` -- road. Bump `+0xe0`.
  * otherwise -- ordinary ground. Bump `+0xdc`; on the 3rd, switch to tracing.

Reaching the goal (`+0xcc == 0`) writes the cell to `+0x34/+0x36` and returns
-1, which the caller treats as "a waypoint was produced, keep going".

**State 2 -- cardinal march** (7 work per step). NOT a trace setup, which is
how I first read it. From the current origin `+0xf0/+0xf4` it steps one cell in
the cardinal direction toward the goal, and on each cell it lays a BREADCRUMB:
a bit in the map at `+0x2c`, and a 4-byte record at `+0x1c` whose second byte
is the direction it entered from. Reaching a cell flagged `0x4` -- the goal --
ends the whole search. When the next cell is blocked it starts the traces.

**State 3 -- TWIN boundary traces** (9 work per step). Retail runs two cursors
at once, `+0xf8/+0xfc` and `+0x100/+0x104`, with directions `+0x108` and
`+0x10c`, sweeping in OPPOSITE senses: `0x414c52` rotates by -2/-3 and
`0x414e23` by +2/+3. Each probes with the 8-direction tables against the usual
threshold 4 and lays the same breadcrumbs. Whichever cursor first regains the
straight line from the origin to the goal wins: `0x414fde` and `0x414ff8` copy
that cursor's cell into `+0xf0/+0xf4` and drop back to state 2. The M-line test
itself is at `0x414dc4..0x414df4` -- normalise the signs so the goal delta is
positive, then accept a cell lying along the first leg or at the far x with z
in range. Running both hands at once is what lets it round a wall from whichever
end is nearer.

**The route is reconstructed from the breadcrumbs**, not accumulated as it
goes: `0x414450` walks back from the goal through the per-cell direction bytes.
That per-cell "first visit wins" discipline is load-bearing -- overwrite the
direction on a revisit and the parent map grows cycles, which in our port
produced a 4-cell loop repeated out to the 64-waypoint clamp while still
reporting success.

**Two limits.** Work accumulates in `+0x48` and the step returns once it
crosses the per-request cap in `+0x165`, resuming next frame with all state
intact. Separately `+0xe4` counts cells visited and bails against `+0xe8`.

**Return protocol** (consumed by `0x415b10`): `0` done, `-1` a waypoint was
emitted at `+0x34/+0x36` (the caller charges a further 30 work and appends it),
`-2` OUT OF QUANTUM -- suspend and resume next frame. An earlier note in this
file called `-2` "failed"; it is not. `0x415028` returns it from the work-cap
check, and the caller's `+0x5c` is a still-pending marker, not an error.

**The budget is an INTEGER, not wall-clock.** `0x41617b` seeds both `+0x221`
and `+0x225` with `0x2ee0` = 12000 work units per frame, and the quality
setting scales it by a percentage. Nothing here reads a timer. That matters
enormously for us: a faithful port is deterministic by construction and safe
for lockstep, provided the budget is a match-replicated constant and requests
are visited in a fixed (player, unit id) order.

### Port status (2026-09-12): ON by default

`setupMatch` enables it. What it does, measured end to end on Inner Circle with
the service off vs on, same start and goal:

    goal + 40 cells:  35% of the way  ->  97% of the way
    goal + 20 cells:  70%             ->  70%   (unchanged)
    goal + 80 cells:  19%             ->  19%   (unchanged)

So it works where the search succeeds, and changes nothing where it fails --
the unit keeps the straight segment it always had.

**The failures are largely inherent, not bugs.** March-plus-wall-follow cannot
solve NESTED obstacles, and the 20- and 80-cell cases on Inner Circle run into
exactly that: a mazey field where the outline a cursor is following contains
further obstacles. Retail's own `(w+h)*20` visit limit (`0x414797`) abandons
those too. A bug algorithm is not a planner and was never going to be one.

**Wandering routes are retail's too.** The 30-cell route comes back with 64
corners looping well past the goal, which looked like a defect until reading
`0x414450`: the original records a waypoint only on a direction change, exactly
as we do, applies no smoothing, keeps the last 64 in a RING buffer (`& 0x3f`),
and merely sets a "this route is a detour" flag on the unit when the step count
exceeds the manhattan estimate. Retail tolerates wandering routes; so do we, and
the unit still gets there -- 97% of the way on a route that wanders.

**Failure backoff matters more than it sounds.** A search that failed from
roughly here will fail again, so a unit stuck against a maze sat re-requesting
every 30 ticks and ate the whole budget. Backing off 150 ticks after a failure
cut the benchmark from 23.2ms/tick to 22.1 while keeping the +40 result at 97%.

Cost: 22.1ms/tick at 2217 units against a ~19.5ms baseline at ~2000, so roughly
+1 to +2.5ms depending on how you weigh the unit-count difference. That buys
units that actually arrive. The `--mpai` hash moves to 9179fe05b490297b,
reproducible run to run, and the cross-compiler golden is untouched.

Still open: whether our cursors wander further than retail's. The route builder
and the termination test are now confirmed to match, so if there is a remaining
divergence it is inside the trace's stepping, and it would show up as route
quality rather than as failure.

### The unstick pass is gone, and what is still broken (2026-09-12)

Retail has no "unstick" pass, and neither do we any more. Ours nudged any ground
unit standing on a blocked cell toward the nearest walkable one, to rescue
bodies "spawned by a building, shoved by a crowd, or clipped a corner" -- and
two of those three causes were our own (the crowd-shoving one was the separation
pass, also deleted). Worse, the nudge moved a unit every tick, which reads as
progress, which reset the wedged timer, so a unit that could not reach its goal
never gave up.

Three real bugs fell out of chasing a unit that walked for ever without
arriving, and all three are fixed:

  * **The no-headway test compared SQUARED distances** and subtracted 400 for
    "20px closer". At 2900px the squared distance is ~8.6 million, so a
    sub-pixel gain cleared the bar and the timer never accumulated at range.
    Now linear.
  * **A clipped route threw the destination away.** `replaceLeg` marks the last
    waypoint of the installed path as the leg's goal, so a route cut off at the
    64-waypoint limit replaced the player's destination with wherever the route
    happened to stop. The unit walked to each route's end, asked for another, and
    shuffled between them. The true goal is now appended when a route falls
    short.
  * **The final waypoint was snapped to the exact goal unconditionally**, even
    for a clipped route, sending the unit charging at a distant point through
    whatever lay between. Now only when the route really reached the goal cell.

RESOLVED, and my first reading of it was wrong. Appending the destination fixed
the circling; what looked like a livelock afterwards was a 60-SECOND HARNESS
CUTOFF being misread. The unit was converging the whole time -- distance to goal
falling 3282 -> 3220 -> 3126 -> 3023 -> 2931 -> 2861 -> 2741 -- and I read "1873px
travelled, no arrival" as evidence of circling when it was evidence of walking.
Run the same case for 300s and it ARRIVES: 124.3s, 5181px travelled, from a unit
spawned inside rock to the far corner of a 192x192 map.

The lesson worth keeping: total distance travelled is not a stuck-detector. Plot
distance TO THE GOAL over time, or the harness will lie to you.

The retry cadence is also now retail's rather than a guess. `0x4e545b`
re-requests only once the tick counter has passed the stamp at navigator+0x110
by `0x78` -- 120 ticks -- and only when the path did NOT fail, testing the
failed/detour bits first and doing nothing at all if either is set. We had been
re-asking every 30 ticks, four times retail's rate.

Still not attempted: nothing here needs a per-destination timer. I proposed one
while the circling was still unexplained; it was a fix for a bug that turned out
not to exist.

### Clicking somewhere unreachable (2026-09-12)

Reported from play: a Monarch ordered at a mountain shoves at the cliff
indefinitely. My harness said otherwise, because it was testing the wrong thing
-- it ordered a unit at a goal whose whole neighbourhood was rock, and the
reachability test correctly reported "unreachable" and dropped the leg.

A click on a mountain is not that. It lands on a cell no ground unit fits in,
but the reachability test resolves an unstandable goal to the nearest WALKABLE
cell, which is normally on the unit's own side of the mountain -- so it answers
"reachable", quite correctly, and nothing ever declares the order impossible.
The unit then walks up to the rock and pushes.

`order()` now snaps a destination the unit cannot stand on to the nearest cell
it fits in, before the order is queued. The unit walks as close as it can get
and ARRIVES, which is what retail does (observed in play: ordered at a mountain,
retail's Monarch goes as near as it can and stops, with no pause first).

That change exposed a second bug, this one entirely mine. Path requests are
keyed by UNIT ID, so queueing a second move cancelled the pending request for
the leg in progress and then installed the queued destination's route into that
leg -- the unit set off for the last thing you queued and skipped everything
before it. `order()` now only requests a route when the new order is the one
about to be walked; orders behind it get theirs when they become current.
"the unit walks the FIRST queued leg" is the test that catches it.

### Port plan

1. `PathService` owning a request queue, replacing nothing at first -- run it
   alongside the current mover behind a flag.
2. Per-tick scheduler: count pending requests, `quantum = budget / (A + 5*B)`,
   visit in deterministic order. Budget from match config, default 12000.
3. `PathSearch` as a resumable struct: state, current cell, trace origin and
   direction, best distance, the three cell-quality counters, work counter.
   Costs 8 / 7 / 9 / 30 exactly as above.
4. Cell scoring reuses our existing per-class `NavGrid` + `bodyPenetration`
   occupancy, mapped onto retail's grades: impassable / 4 occupied / 6 ground
   / 7 road, threshold 4.
5. Navigator side: extend orders to carry up to 64 waypoints with pop-on-
   arrival, and install the straight two-point segment while a request is
   outstanding -- which is exactly today's behaviour, so this is the fallback
   path we already have.
6. Retire `pathBudget_` and the inline A* once 1-5 are in and the Monarch
   harness passes.

Determinism gates at every step: `tools/check-determinism.sh` plus the `--mpai`
hash must be reproducible, and the state hash only changes when intended.

### Earlier unresolved note, kept for the record

By direct-call graph, that whole route cluster is reachable only from
`0x4261f0` -- which references the strings `"%s\screenshots"` and `"BIGSHOT"`
and is the poster/screenshot handler -- and from `0x527360`, which has no
callers and no pointers to it anywhere in the image. Function sizes along the
chain are all 74..795 instructions, so this is not an artifact of merged
boundaries, and the `0x4261f0 -> 0x526740` edge was confirmed by hand.

A route generator wired only to the screenshot path is not a credible reading.
The likely gap is that the graph follows direct `call 0xADDR` only and misses
an indirect/vtable invocation from the order system. Until that path is found,
HOW GAMEPLAY INVOKES THE ROUTE PRODUCER IS UNKNOWN, and with it the answer to
what retail does when a body is parked in a unit's way.

Three conclusions about this area have now been published and two retracted in
a single day. The next claim here should come with a demonstrated call path
from the order or unit tick, not from a routine's shape or from a partial graph.

## Retail-faithful body collision: sub-cell solidity (2026-09-12)

First attempt reverted, then landed once the real obstacle was named. Kept in
full because the obstacle is the interesting part.

Retail's configuration is three things that only work together:
  1. PARKED bodies impassable, MOVING bodies merely expensive (graded score).
  2. The step test scores the body's whole footprint RECT, not one cell.
  3. No separation pass at all -- bodies share space briefly and nothing shoves.

Ours currently has hard-blocking movers and a full-footprint separation push.
Implementing (1) and (2) is straightforward and passes every test: an
`areaScore()` returning -1 impassable / 4 passable / 6 ground / 7 road,
thresholded at 4, with occupancy stamping only parked bodies.

(3) is where it stops. Demoting separation to de-overlap lets a walker
interpenetrate a parked body -- measured closest approach 13px where the
footprints should touch at 32px. The cause is in our mover: it skips the step
test entirely while a unit stays INSIDE ITS OWN CELL, so sub-cell motion is
unchecked and a unit can creep into a neighbour between cell transitions. The
full-radius separation push was silently providing that containment.

So the blocker for retail-faithful collision is SUB-CELL collision handling,
not the score. Retail does not need separation because its refusal happens on
the rect at whatever resolution the mover steps at; ours only happens on cell
entry. Until the mover tests every step rather than every cell crossing,
removing separation trades visible shoving for units sinking into each other.

### What landed

Sub-cell step testing first, as that order implied.

`World::bodyPenetration(u, nx, nz)` asks the same question in PIXEL space that
`cellFree()` answered in 16px cells: how deep would this body sit inside another
if it stood at (nx,nz)? It reads candidates out of `occ_` -- any body we could
overlap has a stamped cell within our own half-extent, so the scan is a small
fixed rect, and a footprint stamps a run of identical ids that dedupes away.
The mover's `free()` is now `bodyPenetration(...) <= 0`, and the "unit stays
inside its own cell, skip the test" hatch is gone.

Two rules make that usable rather than a freeze:

  * Only PARKED bodies are consulted. A moving one is merely expensive, which
    is retail's graded score. Gate on movers too and any crowd wider than its
    lane deadlocks -- measured 16 of 24 arriving where 24 of 24 should.
  * A body you are ALREADY inside is skipped. Units spawn in tight ranks, a
    building finishes under its builder; phrase the rule on the deepest overlap
    and nobody can satisfy it, so nobody moves, and each frozen (speed 0) body
    then blocks its neighbours in turn. De-overlapping stays separation's job.
    The mover's job is the narrow one: never ENTER a body you are clear of.

`rebuildOccupancy` also stamps in two passes, movers first, so a PARKED body
wins a contested cell. A cell holds one id; let a mover overwrite a parked
stamp and the parked body goes invisible to the test that exists to respect it.

Measured: a walker sent straight through a parked body closes to exactly 32px,
its footprint touching and never overlapping. With the cell hatch restored it
reaches 6px in. That ablation is the evidence the hatch was the whole defect.

### The occupancy predicate, and why retail needs no separation pass

Read off `0x4db640` -- the unit half of the passability query -- at
`0x4db767..0x4db7c9`. For each unit found in the target rect it decides whether
that unit blocks. An occupant is IGNORED, letting the step pass straight
through, only when ALL THREE hold:

  1. it is genuinely under way -- retail dereferences a movement object at its
     `+8` and bails to "blocked" (`0x4db8d4`) when there is none;
  2. it is not slower than us -- `[[+8]+0x20]` compared against a computed floor
     AND against our own `[[+8]+0x20]`;
  3. its heading (`+0x7e`) is within `0x4000` of ours, i.e. 90 degrees.

Anything else falls through to `0x4db893`, which returns 2. The threshold at
every call site is 4, so that is a refusal. The return set is: 0 and 2 blocked,
4 blocked, and the terrain score (6 ground / 7 road) when no occupant objected.

In English: **you may close up behind someone going your way who is not slower
than you.** Head-on traffic blocks. Slower traffic ahead of you blocks. Parked
blocks.

That single rule is why retail ships no separation pass and no "don't come to
rest inside another body" check. Overlap barely forms, and the one case that
does create it -- tucking in behind a faster leader -- unwinds itself as the
leader pulls away. Ours now implements the predicate and the separation pass is
DELETED. Two systems enforcing spacing at different resolutions, which is what
we had, is what read in play as units shoving each other around.

Known and accepted: a follower can still end up overlapping its leader if that
leader stops, and nothing now pushes them apart. Retail behaves the same way.
Do not reintroduce a push to "fix" it.

## Self-destruct: the unit quits, it does not explode (2026-09-12)

Ctrl+Shift+D in retail does not kill a unit the way a weapon does. It leaves
your command and fades, with no explosion and no wreck.

The mission is `SelfDestruct` / `UNITMISSIONCODE_SELFDESTRUCT`, whose handler is
at `0x4017e0` -- found through the command table at `0x5eb650`, which pairs
those two name strings with it. What it does:

  * On the first run it seeds a countdown at mission `+0x52` from the unit
    TYPE's `selfdestructcountdown`, tagged `0xF0000000` so a zero count is still
    distinguishable from "not started". Retail packs that key into three bits of
    `UnitType+0x264` (`0x4c09e8` masks it `& 7` and shifts it 21), so its whole
    range is 0..7.
  * Each run decrements and reschedules itself 30 ticks out (`0x4d6a10` sets
    `+0xa = now + arg`), so the countdown steps once a second.
  * At zero it flags `+0x4e` and reschedules a random 0..14 ticks later; that
    next run applies **30000 damage of TYPE 5** to the unit itself, via the
    damage entry at `0x51a140`.

Type 5 is the interesting part. It is not 3, and 3 is the explosion type -- the
one that makes the death handler refuse a corpse and blow every piece apart. And
in that handler type 5 takes its OWN branch (`0x5126a9`), setting a flag in the
death event that no other damage type sets. Nothing in the shipped data gives
any weapon damage type 5; it is the engine's way of marking "this unit quit"
rather than "this unit was killed".

**`selfdestructcountdown` is never set in the shipped data** -- not one of the
558 FBIs across every archive declares it. Which makes the DEFAULT the only
value that matters, and the default is not zero: when the key is absent the
parser clears bit 23 and sets bit 22 of the three-bit field (`0x4c0a1f`),
leaving **2**. Every unit in the game therefore has a two-second countdown.

Running the mission settles it rather than arguing from the disassembly
(`tools/re/emuself.py`, hooking the reschedule, damage and rand calls):

    selfdestructcountdown = 0
      run 0 -> ret=5   expired=0   ('damage', 30000, 5)

    selfdestructcountdown = 2
      run 0 -> ret=1   1 left      ('reschedule', 30)
      run 1 -> ret=1   0 left      ('reschedule', 30)
      run 2 -> ret=1   expired=1   ('reschedule', 0)
      run 3 -> ret=5               ('damage', 30000, 5)

Two reschedules of 30 ticks, then the damage: two seconds, which is what the
game gives you. At 0 it fires on the spot, so the default is doing real work.

Retail also LABELS it, and the label is the whole explanation. The command table
entry carries two strings, `SelfDestruct` and `UNITMISSIONCODE_SELFDESTRUCT`,
and the second is a key into `translate/unitmissions.tdf` -- the table of what a
unit is currently doing, the text the InfoPanel shows:

    [UNITMISSIONCODE_SELFDESTRUCT]
        English = Leaving Your Command;
        German  = Einheit wird Aufgel"ost;
        French  = Abandonne votre camp.;

So the unit's status while the countdown runs is "Leaving Your Command". It is
not a chat line or a notification -- it is the mission label, sitting where
"Moving" or "Guarding" would. (I first put it in the chat overlay, which was
wrong.)

That table also corrects several of our other labels, which were plausible
paraphrases of missions retail names differently:

    RECLAIM      -> "Clearing"          (we said RECLAIMING)
    RECLAIMAREA  -> "Clearing Area"
    LOAD         -> "Loading"           (we said BOARDING)
    BEINGBUILT   -> "Intangible Mass"   (we said UNDER CONSTRUCTION)
    CLOAK        -> "Cloaking"          (we said CLOAKED)
    SEEKATTACK   -> "Seeking to attack" (we said ADVANCING)

## Retail does not sort 3DO primitives -- it z-buffers them (2026-09-12)

The open question from the projection work, settled: there is no primitive sort
to copy, because retail resolves visibility per pixel.

  * The binary imports the Glide depth API outright -- `_grDepthBufferMode@4`,
    `_grDepthBufferFunction@4`, `_grDepthMask@4`, `_grDepthRange@8`,
    `_grDepthBiasLevel@4`, `_grLfbConstantDepth@4`, alongside `_grBufferClear@12`
    and `_grRenderBuffer@4`. That is a hardware depth buffer, configured and
    written, in the 3dfx path (the install ships glide2x/glide3x and a
    ChooseRenderer.exe to pick it).
  * The DirectDraw path wants one too: the error text
    "...because there is no hardware support for zbuffer blting. (NOZBUFFERHW)"
    is a capability complaint about z-buffered blitting, not about sorting.

Following the draw path corroborates it. `0x4eea20` is the piece-tree walk --
it recurses over child/sibling, applies the 3x3 piece rotation to each vertex,
and hands the result on. There is no depth comparison, no insertion into an
ordered list, no swap loop anywhere in it.

**So our triangle sort is not an approximation of retail's sort -- it is an
approximation of retail's Z-BUFFER**, and no choice of sort weights can match it
exactly, because a single key per triangle cannot express what a per-pixel test
does for interpenetrating geometry.

That reframes `kSortZ`/`kSortY`: they are our own stand-in for a depth buffer,
not a retail constant. But a stand-in can still be WRONG, and ours was.

The sort key has to be depth along the view ray, and the projection hands that
over directly: `screenY = z - y/2` means points differing by `(y,z) = (2,1)`
land on the same pixel, so that direction is the ray and depth along it is
proportional to `2y + z`. Larger y is higher and nearer the camera; larger z is
further down-screen and also nearer -- so both terms carry the SAME sign.

The old weights were `cos`/`sin` of the 0.72 rad tilt, which gave y and z
OPPOSITE signs. Every triangle pair separated mainly in z was ordered
backwards, which is what tore layered pieces like a Monarch's cape. Now
`-(2y + z)`, derived rather than fitted.

A correct key was still not enough, because we were not CULLING. Retail does --
it imports `_grCullMode@4` -- and without it both faces of a two-sided piece are
drawn at almost the same depth, so which one wins is arbitrary: a Monarch's cape
came out as a patchwork of its own front and back. The cape is four segments,
each carrying a front quad and a back quad (`capelogoA1`/`A2` and friends), which
is exactly the shape that fails.

The cull is tested against the VIEW DIRECTION, not a screen-space winding sign,
because the transform negates y and may mirror x and reasoning about the
resulting handedness is how sign errors get in. Two measurements settle the
direction without guessing:

  * 3DO polygons are wound so `(v1-v0) x (v2-v0)` points OUTWARD -- 88-97% of
    primitives agree, across araking, arasword, tarnecro and zonhurt.
  * With the cull as written, kept faces sit toward the camera and culled faces
    away: mean offset along the view ray `+2.0..+2.9` for kept against
    `-1.8..-3.4` for culled, at every facing. An inverted sign would swap those.

56-65% of triangles survive, a little over half because flat pieces favour one
side.

What remains beyond this is two triangles that genuinely interpenetrate, which
one key per triangle cannot resolve. That is the real depth-buffer case, and it
means taking models off `SDL_RenderGeometry` -- worth doing only if artifacts
survive a correct key and a cull.

## Unit shadows are PROJECTED SILHOUETTES -- and the Glide path is the one that matters (2026-09-12)

Reported: units have no shadows, then retail screenshots of an Aramon and a
Taros Monarch on open sand, both plainly casting one, the Aramon shadow carrying
the sword as a long thin streak. A blob cannot do that; a projection can.

**The renderer we target is Glide, not the software one.** The icd ships both,
and they shadow units by completely different means. Chasing the software path
first cost a full pass of wrong conclusions, so start here: `Glide3x.dll` is
resolved by name at runtime (hence nothing in the import table), 83 entry points
from a string block at `0x621fe0` into a driver object -- `grDrawTriangle` at
`+0xbc`, `grCullMode` at `+0x120`, and so on, resolved by the stride of stores
at `0x5b8617`.

The giveaway that shadows are GEOMETRY, not sprites, is the renderer's own
counter: `"Max polys rendered : %d ( %d shadow )"`. The shadow tally is the
global `0x62a800`, incremented at `0x4edc67`, inside a rasteriser reached only
from the model draw:

    0x4ee700  draw unit
      0x4ee620   build the piece rotation, walk the piece tree (0x4eea20) ONCE
      0x4ec720 -> 0x4ec7b0 -> 0x4ede50 -> 0x4eda80   the shadow polys

`0x4ede50` projects the same geometry through `0x4ec250` and hands it to the
rasteriser. That routine is the whole answer. It works in 16.16 fixed point and
takes a flag choosing between two shears:

    ecx = -z,  edx = y,  esi = x                     (each >>16, sign-extended)
    flag 0:  edx = y>>1;  ecx -= edx             ->  ( x,        -z - y/2 )
    flag 1:  edx = y>>2;  esi += edx; ecx -= edx ->  ( x + y/4,  -z - y/4 )

`0x4ecf21` passes 0 for the model; `0x4ede50` passes 1 for the shadow. So:

  * Flag 0 independently re-derives our model projection and `kProjY = 0.5`.
  * **The light constants are both a quarter**: `kShadowLX = kShadowLZ = 0.25`.
    We shipped `0.55 / 0.35` first, fitted by eye to a screenshot; they were
    wrong and are now read out of the binary.
  * The shadow is NOT flattened to y=0. It is a second shear of the same
    vertices, so a vertex sits exactly `(+y/4, +y/4)` from its own body vertex
    in screen space -- down and to the right. A flyer at altitude A therefore
    casts its silhouette A/4 right of and A/4 below its body, which is the
    offset shadow every TA-engine flyer has.

The exclusions are only two, both bits of `UnitDef+0x260`: `noshadow`
(`0x2000000`, tested at `0x4ec8d8`) and `floater` (`0x80000`, at `0x4ecac6`).
There is no `canfly` test and no building test, so flyers and keeps both cast.

`ShadowScale` (a 0..3 setting at settings+0x19) maps to 1/2/4 at `0x4ecb83` and
becomes the divisor `0x4ec250` applies to the projected coordinates -- a shadow
resolution knob. `DrawShadows` is the bool at settings+0xf, reaching the draw as
`gameState+0x19c70`.

### The sprite shadows are the SOFTWARE renderer's answer

`0x4c12b0` parses `shadowgaf` and `shadowart` and sets the type's shadow
sequence at `+0x288` only when BOTH keys are present. There is no fallback: the
4th argument of the key reader `0x5432c0` is a default string, but the default
for `shadowart` is empty AND that path returns 0 (`0x543319`), so a missing key
skips the store either way. `shadows.gaf` holds 17 sequences of 6-12 frames --
pre-rendered directional blobs, one frame per facing.

That art is blitted by `0x4ee310`, which is reached only from `0x511d00` and
never from the model draw. **In Glide it is dead for units.** 104 of 203 unit
FBIs declare a `shadowart` and none of them use it; `araking` declares only
`shadowgaf = shadows` and no `shadowart`, which is exactly why the sprite path
could never explain the Monarch.

This also overturns an earlier, well-evidenced but software-only conclusion: we
excluded `canfly` from casting because `0x4ee340` installs shadow art only when
noshadow and canfly are both clear. True of the sprite; false of the renderer we
target. The 24 flying types carrying a `shadowgaf` are not dead data.

Two wrong answers on the way, both worth naming:

  * A fixed 14x5 blob, our own invention, the same size for every unit and at
    alpha 70 effectively invisible.
  * The model's GROUND PLATE -- the flat untextured unit-sized quad every root
    carries (`AraGP`, +-17.6 for a Monarch, +-14.4 for a swordsman). It looks
    like the answer, and it is not: a rectangle cannot be a silhouette, and
    `arasword` carries the same plate while using a sprite. It is a footprint
    marker.

### It is a flat multiply, not per-triangle alpha

Retail's shadow is one uniform tone with no internal structure -- no darkening
where a wing crosses a body. Sampling a retail screenshot gives shadowed/unshadowed
ratios of 124/225, 106/192, 92/170: a flat multiply to **0.55**, equal on all
three channels, so it multiplies the ground rather than blending toward a grey.

That is what the rasteriser's `alloca` at `0x4eda83` is for -- 0x5f08 bytes of
span/coverage buffer. The whole silhouette is rasterised into it and blended
ONCE. Drawing the triangles individually with alpha instead stacks them wherever
the silhouette folds over itself, which reads as obviously wrong next to retail.

We reproduce it with a coverage mask: clear the batch's bounding box to white,
draw every shadow triangle opaque at 0.55 grey with blending off, then composite
that rect once with `SDL_BLENDMODE_MOD`. Bounding box, not full screen -- a
7680x2160 clear and blit twice a frame is most of the cost and none of the gain.

### Probe the shear sign on a GROUND unit, never on a flyer

Retail's shadow is `(+y/4, +y/4)` from the body in screen space: down AND right.
In our renderer that is `rx + kShadowLX*w[1]` and `rz + kShadowLZ*w[1]` -- plus
on both.

The trap, which cost a wrong commit: a flyer's shadow carries TWO x terms, the
per-vertex shear and the altitude offset applied at the anchor, and the altitude
term can outweigh the vertex one. Probing the sign on flyers (set `kShadowLX` to
2.0 and see which way the silhouettes smear) therefore reports the sign of the
wrong term -- it said "negate x", and negating x sent every GROUND unit's shadow
leaning left, which is what the player then saw.

A ground unit has `alt = 0`, so it isolates the vertex expression exactly. Probe
there. The exaggerated constant is still the right technique: with `+` a ground
unit smears hard right, with `-` hard left, unambiguous in one screenshot.

Shadow triangles skip the backface cull -- a silhouette is the union of both
faces, and culling half of it punches holes. Anchor the shadow on the BODY
anchor plus retail's delta, never on a re-derived ground point: flyers are
exempt from our terrain lift, so a ground-derived anchor picks up a lift the
body never had and the horizontal lean cancels to nothing.

Cost: every casting unit walks its model a second time.

## The projection is a SHEAR, not a tilt (2026-09-12)

`0x421dad` spells retail's world-to-screen transform out in seven instructions:

    movswl 0x2(%esi),%ecx     ; x
    sub    cameraX,%ecx       ; screenX = x - cameraX
    movswl 0xa(%esi),%eax     ; z
    movswl 0x6(%esi),%edx     ; y
    sar    $1,%edx            ; y / 2
    sub    %edx,%eax          ; z - y/2
    sub    cameraY,%eax       ; screenY = z - y/2 - cameraY

So screen Y takes ALL of z and HALF of y, and screen X takes no y at all.

The important part is what that is NOT: 1.0 and 0.5 are not the cosine and sine
of any angle, so the transform cannot be written as a tilt. We had modelled it
as one for a long time -- `cos(gTilt)=0.75`, `sin(gTilt)=0.66` -- which is wrong
in both terms at once, and wrong in a way no amount of tuning the angle could
fix. `kProjY = 0.5` and `kProjZ = 1.0` replace it.

This is the same halving as the terrain lift, which is the point: one projection
governs models, the terrain relief, flyer altitude, the selection ring's mid
height, and the hit box (derived from the same `collect()` walk, so it follows
for free and stays consistent with what is drawn).

`gTilt` is gone entirely, along with the `--tilt` flag that tuned it: there is
no camera angle here to tune. What remains is a triangle depth-sort key inside a
model (`kSortZ`/`kSortY`, the old cos/sin values kept as plain weights), needed
only because we draw without a depth buffer. What retail's software renderer
sorts 3DO primitives by has NOT been established -- that is the open question
here, and the constants say so.

## The terrain-height lift is height/2 (2026-09-12)

Units are drawn lifted up-screen to sit on relief that is painted into flat
tiles. We had been lifting `1.1` px per height unit, picked by eye. Retail's
figure is **0.5**, and the arithmetic is explicit:

  * `0x511140(point)` reads the point's cell through `0x50e600` and returns byte
    `+4` of the 14-byte cell record -- the terrain height.
  * Its caller `0x426820` forms
        `screenY = (z << 4) - cameraY - (height >> 1)`
        `screenX = (x << 4) - cameraX`
    so the lift is height/2 in Y, and there is NO height term in X.

`tools/re/emulift.py` drives both with a stubbed cell lookup to confirm which
byte comes back, since that was the load-bearing assumption. At height 120 that
is 60px of lift against the 132px we were applying -- a unit on raised ground
sat most of four tiles too far up-screen, which is why it looked like it was
standing on terrain beside it rather than on its own.

Our `kHeightScaleX_` was already 0, which the same instruction sequence
confirms is right.

Two other details fall out of the same neighbourhood. `0x511170` is the
BILINEAR sampler: it takes a sub-cell position (`sar $4` for the cell, `and
$0xf` for the fraction), reads four neighbouring cells and interpolates -- so
retail smooths the lift across a slope exactly as our `heightAbove` does. And
the cell record really is 14 bytes with the height at `+4`, which the same code
shows twice over (`+0x4` and `+0x12` are the same field one cell apart).

## Retail never lets a unit stand behind terrain (2026-09-12)

A tall cell's painted face leans up-screen over the lower ground to its north,
so a unit stopping there is drawn inside the rock. Retail does not fix that at
draw time by painting the face back over the unit -- it keeps units OUT of those
cells. The occlusion block is a movement rule, not a rendering one.

This engine had that pass, and I deleted it earlier the same day for blocking
12% of Inner Circle and stranding an army (2 of 24 arriving). It was right all
along; the PROJECTION CONSTANT inside it was wrong. It leaned the face north by
1.1 px per height unit, where retail's figure is 0.5 -- proven separately off
`0x511140` and `0x426820`. At more than twice the real lean it condemned more
than twice the ground:

    Inner Circle     at 1.1: 4554 cells (12.4%)   at 0.5: 2294 (6.2%)
    Two Castles      at 1.1: 7238 ( 4.9%)         at 0.5: 4325 (2.9%)
    Angvir's Maze    at 1.1: 4458 ( 4.4%)         at 0.5: 2047 (2.0%)

Restored at 0.5 and writing into the SHARED overlay -- the per-class grids are
what `navFor()` actually hands a unit, and the legacy grids are not -- the army
test passes 24 of 24, and routing is unchanged at 97/78/31% for 40/80/160-cell
goals.

Two lessons in one bug. A constant that is merely "a bit off" can look like a
design being wrong: I concluded the whole pass was misconceived when it was one
number. And a rendering constant turned out to belong to the movement rules,
which is why it mattered twice over.

## Dynamic analysis: emulating icd routines (2026-09-12)

Static reading gets a routine's shape; it does not tell you whether YOUR port
behaves like it. Emulating the original function and diffing the two does.

The harness (kept outside the repo, in the session scratch dir) is about 90
lines of Python on Unicorn: map the PE's `.text`, `.rdata` and `.data` at their
virtual addresses, give it a stack and a heap, then call a function with the
stdcall / `__thiscall` conventions the binary uses. Sub-calls that need the live
game -- the cell query, the allocator -- are hooked and answered from a
synthetic grid, so a routine can be driven over test data the real game could
never produce on demand.

It observes; it copies nothing. CLAUDE.md is updated accordingly: the binary is
for reverse-engineering, static and dynamic, and never for lifting code or data
into the engine.

First result, checking `pathDirFromDelta` against `0x415040` over 81 deltas:
80 agree, and the single disagreement is the degenerate zero delta, where the
icd answers 5 and we answered 0. The search should never ask for a direction to
where it already stands, so it changes no behaviour -- but it took one run to
find something no amount of re-reading the disassembly had.

The harness lives in `tools/re/` (`emu.py` loads the image and calls into it;
`emusearch.py` drives the path search over a synthetic grid).

### What it found in the search, immediately

Driving `0x4146e0` over a 24x12 grid with a wall across the path and one gap,
hooking the cell query to answer from that grid, and logging every probe:

    c(2,6)...c(7,6)   cardinal march east, blocked by the wall at x=8
    A(7,5) B(8,7)     the cursors split; B steps diagonally into the gap row
    A(7,4) B(9,6)     B is through and back on the goal line
    c(10,6)...c(20,6) march resumes to the goal

27 cell visits for the whole thing. The probe log then gave the exact rule:

    A: probes 6 -> 7 -> 0  ACCEPT      (starts at the BLOCKED dir, rotates up)
    B: probes 6 -> 5      ACCEPT       (starts at the BLOCKED dir, rotates down)
    A next, dir=0: probes 6 -> 7 -> 0  (starts at dir-2, rotates up)
    B next, dir=5: probes 7            (starts at dir+2, rotates down)

So each cursor's FIRST probe begins on the direction that was just refused, and
only afterwards does the +-2 offset apply. Our port seeded both cursors with the
blocked direction, so their first probes opened at blocked+-2 and skipped
straight past it -- turning a two-step detour into a long wander. Seeding
`dirA = blocked-2` and `dirB = blocked+2` makes the offsets land on the blocked
direction and fixes it.

Measured before and after on Inner Circle, unit travelling toward a goal N cells
away, as a percentage of the distance closed:

    goal      +20    +40    +80   +160
    before    70%    97%    19%     9%
    after     95%    97%    78%    31%

and searches that used to abandon at the visit limit (20, 60, 80, 120, 160 cells)
now all arrive. Hours of re-reading the disassembly had not found this; the
emulator found it in one run, because it could show what the original DOES
rather than what it looks like it should do.

### The cost of it working, and the budget

At retail's own budget of 12000 work units a tick, the 8-AI benchmark costs
42.2ms/tick at ~2040 units against a ~19.5ms baseline -- over the 33ms a 30Hz
tick has.

I first put that down to units which used to wedge now marching, and reasoned
that budget-capped searches could not account for 20ms. That was wrong, and one
run settled it: at a budget of 1500 the same benchmark costs 23.5ms at ~2010
units. The searches ARE the cost. A work unit is not a cell query -- a trace
step charges 9 and can probe up to seven directions -- so query count runs well
ahead of the budget.

**Routing does not suffer for it.** Measured at 12000, 3000 and 1500 on Inner
Circle, the percentage of the distance closed is identical for 40-, 80- and
160-cell goals. The budget decides how FAST a search finishes, not whether it
can: the unit walks its straight segment meanwhile and the route lands a few
ticks later either way.

So `setupMatch` runs at 1500. That is 12.5% of retail's default and sits well
inside the 5%..1000% band retail's own quality setting covers (`0x4252e0`), so
it is a supported configuration rather than a deviation.

Two micro-fixes are also in, neither of which moved the number much: the
per-cell scratch is generation-stamped instead of cleared (it was zeroing ~110KB
per request), and the retry sweep does its cheap tests before the O(orders)
`currentLeg` scan.

## Headless in-game screenshots (dev harness)

`--shot` alone captures the LOBBY and exits: it forces the dummy video driver and
the menu path shoots before the game starts. To capture a real in-game frame use
the auto-lobby driver plus a delayed capture, all with the DEBUG binary:

```sh
TAK_MPAUTO=4 TAK_SHOT_MS=35000 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  ./build-dbg/takclient game "Inner Circle" --data <install> --shot out.png
```

- `TAK_MPAUTO=4` drives the lobby the way `--mpai` does, but through the
  INTERACTIVE (rendering) loop instead of the headless one. Without it the client
  sits at NOT READY forever and the shot is a lobby frame.
- `TAK_SHOT_MS=<ms>` delays the capture by wall-clock ms (default is 3 frames).
- Add `TAK_STRESS=1` to spawn each AI at ~95% of the unit cap for instant mass
  combat -- the quickest way to see projectile art, beams and storms on screen.
- No real window is needed; the dummy driver renders correctly.
