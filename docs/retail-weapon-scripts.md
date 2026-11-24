# TotalA.exe weapon scripts: names, timing, and what the engine must call

Scope: Total Annihilation 3.1c (`TotalA.exe`, GOG Commander Pack, base 0x400000). All
addresses are **TotalA.exe**. Engine names (`World::tickScriptWeapon`, `RetailAimState`,
`cob::RetailVm`) are the Kingdoms-derived code in `src/sim/sim.cpp`, `src/sim/retailweapon.h`
and `src/cob/retailvm.h` that this document replaces for TA units.

Evidence tags: **[E]** the routine was run under unicorn (`tools/re/weapon_emu.py`, 0
mismatches), **[S]** read from the disassembly, **[U]** not resolved or inferred.

## 0. Data layout used below

**Unit weapon slot record** `S = unit + 4 + 0x1c*slot`, slot 0..2 (init 0x49e070) [S]:

| Offset | Meaning |
|---|---|
| S+0x00 w, S+0x02 w | target: unit index + `0x8000`, or ground point `x>>16`, `z>>16` (a z of 0x8000 is stored as 0x8001) (0x48a060, 0x48a0a0) |
| S+0x04 | aim-return callback object, vtable 0x4fd6f0 (set at 0x485ec6/0x486069/0x48628d) |
| S+0x08 | **aim ready**: set to 1 by the callback when AimX returns nonzero (0x481490) |
| S+0x0c | weapon type pointer |
| S+0x14 w | reload countdown, ticks |
| S+0x16 w, S+0x18 w | heading and pitch passed to the last AimX (relative to the body) |
| S+0x1a b | stockpile count |
| S+0x1b b | flags: bit0 **aim requested**, bit1 slot has a weapon, bits2-3 slot index, bit4 **auto-target enabled** |

**Weapon TDF flags** at weapon+0x111 (parser 0x42e5d1..0x42eb60) [S]: lineofsight bit0,
ballistic bit1, shellweapon bit2, beamweapon bit3, vlaunch bit4, dropped bit8,
soundtrigger bit11, guidance bit12, tracks bit13, unitsonly bit14, groundbounce bit15,
waterweapon bit16, toairweapon bit17, smoketrail bit18, **turret bit19**, selfprop bit20,
propeller bit21, noexplode bit22, burnblow bit23, twophase bit24, cruise bit25,
commandfire bit26, noautorange bit27, stockpile bit28, targetable bit29, interceptor bit30.
Fields: weaponvelocity +0x68, range +0xdc, reloadtime +0xe4 (word, ×30 → ticks),
energypershot +0xc0, metalpershot +0xc4 (floats), minbarrelangle +0xc8 (float,
degrees × π/180), burst +0xea, burstrate +0xec (×30 → ticks), sprayangle +0xee,
accuracy +0x104, tolerance +0x106, pitchtolerance +0x108.

**COB VM** (per unit object at unit+0x9a) [S]: **8 threads** of 0xa4 bytes at +0x1c
(0x4b08c0 scans 8; the engine's `RetailVm` has 16). Thread words: 0 status, 1 pc,
2 stack top, 3 sleep counter, 4/5 wait piece/axis, 6 child, **7 signal mask (new engine
threads start with 1, 0x4b091a; a START/CALL child inherits the parent's, 0x4b1958)**,
**8 return callback** (0x4b0b37), 9.. stack. Same layout as `RetailVm::Thread`.

## 1. Entry names TotalA.exe looks up

| Purpose | Slot 0 / 1 / 2 | Table / site | How called |
|---|---|---|---|
| Aim | `AimPrimary` / `AimSecondary` / `AimTertiary` | `[0x509688+4*slot]`, used at 0x49e306, 0x49e370 | new thread, 2 args, return callback, not immediate (0x4b0a70) |
| Fire | `FirePrimary` / `FireSecondary` / `FireTertiary` | `[0x509678+4*slot]`, used at 0x49cb86, 0x49cd44, 0x49cf68 | new thread, 0 args, no callback, not immediate (0x4b0940) |
| Fire point | `QueryPrimary` / `QuerySecondary` / `QueryTertiary` | inline tables at 0x43e1fc, 0x43e25d, 0x43e342 | synchronous, 1 in/out arg, initial 0 (0x4b0bc0) |
| Aim origin | `AimFromPrimary` / `AimFromSecondary` / `AimFromTertiary` | inline table at 0x43e2f8 | synchronous, initial **-1**; still -1 → QueryX instead |
| Target point | `SweetSpot` (on the **target**) | 0x43e3d7 | synchronous, initial 0 |
| Lost target | `TargetCleared(slot)` | 0x489893, 0x489943, 0x48a144, 0x48a2db | new thread, 1 arg = slot 0..2, not immediate |
| Recoil | `RockUnit(a, b)` | 0x499c57, 0x49cbe6, 0x49cda1, 0x49cfc5 | new thread, 2 args, not immediate |
| Reload info | `SetMaxReloadTime(ms)` | 0x49e181 | new thread at unit creation, 1 arg |
| (other) | `QueryNanoPiece`, `HitByWeapon`, `StartBuilding`... | | not weapon-slot related |

All [S]. TotalA.exe 3.1c contains **no** `AimWeapon`, `AimWeapon1..3`, `QueryWeapon`,
`FireWeapon`, `EndBurst`, `Shot` or `RestoreAfterDelay` strings (searched the whole
image) [S]. `RestoreAfterDelay` is a script-to-script convention; the engine never
calls it. `AimWeaponN`/`QueryWeaponN` are Spring conventions and are not used by
retail.

**Name lookup** is an exact, case-sensitive byte compare over the COB name table
(0x4b0bde, 0x4b0a8e, 0x4b0960) [S].

**Missing function / no free thread** [S]:
- Synchronous call (0x4b0c40): lookup or thread allocation fails → returns 0 and leaves
  the caller's in/out variable untouched. So a missing QueryX gives **piece 0** (the
  base piece), a missing AimFromX keeps -1 → falls back to QueryX, a missing SweetSpot
  gives piece 0.
- Asynchronous start with a callback (0x4b0b00): on failure calls `callback(0)`
  immediately. For AimX this means "not ready": the slot stays "aim requested" and
  **never fires** at that target (see §2.4).
- Asynchronous start without callback (Fire, RockUnit, TargetCleared): failure is silent.
- A piece number out of range (<0 or ≥ piece count) gives a zero offset, i.e. the unit
  origin (0x43df16 → 0x43e01d).

## 2. Aim

### 2.1 When it is called

The weapon update **0x49e1a0** runs every tick for each locally simulated unit
(0x48adda, players of type 1/2), immediately **before** that unit's COB VM tick
`0x4b0d60(1)` (0x48adeb). For each slot with a weapon (flags bit1) [S]:

1. `if (reload) --reload` (0x49e1c8).
2. Target point `T` = 0x48a1e0 (§3.3). No target → clear **aim requested** (bit0) and
   skip the slot (0x49e1ea).
3. Aim start, only if **aim requested is clear** (0x49e213, 0x49e357) [S]:
   - **turret=1** (bit19): compute `heading, pitch` (§2.2). No solution → no aim this
     tick. Else store them in S+0x16/0x18, set **ready=0** (S+8), start
     `AimX(heading, pitch)`, set **aim requested** (0x49e2da..0x49e3ab).
   - **turret=0, vlaunch=1** (bit4), and for stockpile weapons only with a stock > 0:
     set ready=0, start `AimX(0, 0)`, set aim requested (0x49e33d..0x49e3ab).
   - any other weapon (not turret, not vlaunch): **no Aim script is ever called**.
4. Fire check (§4).

So AimX is started once per "aim cycle", not every tick, and **independently of
reload**: it starts as soon as the slot has a target and no aim is outstanding.

### 2.2 Arguments and units [S, helpers E]

`P` = aim origin = world position of the AimFromX piece (0x43e2e0: AimFromX(-1) →
if -1, QueryX(0) → piece position + unit position). `T` = target point. `d = P - T`
(0x49e570 computes A - B) [E].

- **heading** = `atan2(d.x, d.z)` in BAM (0x4b715a = atan2(a,b)·32768/π) [E] **minus the
  unit body heading** (word unit+0x66), wrapped to 16 bits (0x49e264..0x49e270). So it is
  **relative to the unit body**, 65536 = 360°. Convention: an object with heading h moves
  along `(-sin h, -cos h)` (projectile velocity 0x49cb06/0x49cb12; 0x4b70ef = r·sin,
  0x4b7123 = r·cos [E]); heading 0 points to -z.
- **pitch**:
  - ballistic=1: 0x49a890(d.x, d.y, d.z, weaponvelocity, minbarrelangle) returns the
    ballistic elevation in BAM, or 0x8000 = no solution (then no aim) (0x49e28e).
  - else lineofsight=1: `atan2(-(d.y>>16), |d.xz|>>16)` = elevation of T seen from P,
    positive = target above (0x49d910, used at 0x49e2c5). World pitch, not relative to the
    body pitch.
  - neither flag: no aim (0x49e2cc).
- AimX gets **two** arguments (nargs = 2): `AimX(heading, pitch)`. No slot argument
  (Kingdoms' AimWeapon has three).
- vlaunch: `AimX(0, 0)`.

Multiplayer side note: the same start is broadcast as packet 0x10 (0x456200) so remote
copies run the animation; irrelevant for lockstep [S].

### 2.3 Thread and "ready" detection [S]

- Started with 0x4b0a70(name, callback = &S+4, immediate = 0, nargs = 2, h, p, 0, 0): a new
  thread with `stack top = nargs-1`, callback stored in thread word 8, not run now. It
  runs in the same tick's VM pass (0x48adeb), right after the weapon update.
- **Ready = return value.** RETURN (0x10065000, 0x4b19d0): if the thread has a callback,
  pop the top of stack and call `callback(value)`. The weapon callback 0x481490 sets
  `S+8 = 1` only if the value is nonzero. A script that ends with `return(0)` leaves the
  weapon unready.
- A thread killed by SIGNAL (0x10067000, 0x4b1a75) is removed **without** calling its
  callback. A blocked thread (wait-for-turn, sleep) simply has not returned yet.

### 2.4 Waiting and re-aim cadence [S]

There is no timeout and no re-call while the aim is outstanding. "Aim requested" (bit0)
is cleared, and AimX started again on the next update, only when:
- the slot has no target / the target died (0x49e1ea, via 0x48a1e0);
- the fire check finds no solution (ballistic 0x8000) (0x49d65b);
- the fire check finds the stored angles out of tolerance (0x49d68a, §4.1);
- a shot was fired (0x49d78b: S+8 = 0, bit0 cleared). **AimX is therefore called again
  before every shot.**

Consequence (retail quirk): if the aim thread returns 0, is killed by a signal, or
cannot start (no AimX, or all 8 threads busy), the slot waits until its target is
lost or cleared. Changing to another live target (0x48a060) does not clear bit0.

## 3. Query / AimFrom / SweetSpot

### 3.1 QueryX — fire point [S]

`0x43e240(unit, out, slot, piece)`: if `piece < 0` call QueryX(&piece=0) synchronously,
then `out = unit position + piece offset` (0x43def0). Used:
- at the shot: projectile start position (0x49d6af turret, 0x49d9e0 non-turret LOS,
  0x49db9b vlaunch, 0x49dd80 dropped);
- at projectile creation, the piece number is queried again and stored in the
  projectile (+0x62, 0x49c82e → 0x43e1e0) for the burst emitter (§5);
- as the fallback for a missing/negative AimFromX;
- for non-turret LOS weapons, also the origin of the direction checked against the body.

The query runs only its own thread with zero elapsed time and reads the 4 argument words
back (0x4b0c40) — the same model as `RetailState::query`.

### 3.2 AimFromX — aim origin [S]

`0x43e2e0`: AimFromX(&piece = -1); if still -1, QueryX(&piece = 0). Used only for the
angles: aim start (0x49e237) and the tolerance re-check at fire time (0x49d5c5,
0x49d924). It is **not** a line-of-fire origin.

### 3.3 Target point and range [S]

`0x48a1e0(unit, out, slot)`:
- ground target: `(x<<16, max(ground height, sea level), z<<16)`;
- unit target: if the target is dead (unit+0xa6 == 0) → clear the slot's target and start
  `TargetCleared(slot)`, return 0. Else `out` = the target's **SweetSpot** piece point
  (0x43e3c0 calls SweetSpot synchronously, then 0x43e0b0); if the shooter has more than 5
  kills (unit+0xb8) and the weapon is not cruise, the point is led by the target's
  motion (0x48a324..).

Range gate `0x49aa80(unit, unit position, T, slot)` (0x49e3c8): horizontal distance
**from the unit origin** ≤ range; not submerged unless waterweapon; ballistic weapons
need a ballistic solution from the unit origin. **No line-of-fire ray test** exists in
the fire path.

## 4. Fire

### 4.1 Sequence (turret weapons, fire routine 0x49d580) [S]

Per slot, after the aim step (0x49e3ae..): `reload == 0` → range gate 0x49aa80 →
player stores ≥ energypershot and metalpershot (or stockpile count > 0) (0x49e3d5..) →
weapon fire routine `[weapon+0x60]` (chosen at 0x49e010: turret → 0x49d580, vlaunch →
0x49db70, lineofsight|selfprop → 0x49d9c0, dropped → 0x49dd60). 0x49d580:

1. Requires **aim requested** and **ready (S+8 ≠ 0)**, else returns without changes.
2. Recompute heading/pitch from the AimFrom piece as in §2.2. No solution → clear bit0.
3. Tolerance (0x49d880): `|h_now - S+0x16| ≤ tolH` and `|p_now - S+0x18| ≤ tolP`
   (signed 16-bit differences). `tolH = tolerance`, `tolP = pitchtolerance` or, if 0,
   `tolerance`; if tolerance is 0, both are **150** (standing) or **2000** (moving: unit
   move-rate bits unit+0x110 & 0xc nonzero). Outside → clear bit0 (re-aim next tick).
   Note: full pitch tolerance, not tolerance/2 as in `RetailAimState::ready`.
4. Fire position = QueryX piece (0x49d6af). Heading made absolute (S+0x16 += unit
   heading), then accuracy spread: `spread = accuracy + 2048 − health·2048/maxhealth`,
   divided by `kills/6` when that is > 1; heading and pitch each get
   `rand(spread) − spread/2` (0x49d6bc..0x49d73e).
5. Spawn the projectile (0x49c9c0 LOS/selfprop, 0x49cde0 ballistic, 0x49cc20 vlaunch).
   **Inside the spawner, after the projectile exists**: start `FireX()` (0 args, no
   callback, not immediate) and `RockUnit(-800·cos h, -800·sin h)` with h = shot heading
   relative to the body (0x49cb86..0x49cbeb).
6. `S+8 = 0`, clear bit0 (0x49d78e).
7. Back in 0x49e1a0: new reload (§5), "fired" bits in unit+0xba, energy/metal charged
   unless stockpile (0x49e4f2..0x49e533).

FireX therefore runs **after** the projectile was created, in the same tick's VM pass;
the engine never waits for it and nothing it does (no SET opcode) releases the shot.
Barrel alternation works because QueryX is evaluated before FireX toggles its static.

### 4.2 Other weapon classes [S]

- **Non-turret LOS/selfprop** (0x49d9c0): no Aim script. World heading/pitch from the
  QueryX point to T is compared (same 0x49d880 tolerance) with the **body** heading
  (unit+0x66) and pitch (unit+0x68); the body must face the target. Spawn as above, so
  FireX and RockUnit are called.
- **vlaunch** (0x49db70): needs ready (S+8) from `AimX(0,0)`; spawns via 0x49cc20
  (FireX, RockUnit).
- **dropped** (0x49dd60): no Aim, **no FireX, no RockUnit**; only QueryX for the drop
  position.

## 5. Reload and burst [S]

**Reload** (0x49e468..0x49e4ee), set only after a successful shot:
`k = min(kills/5, 5)`, `h = health·20 / maxhealth` (unit+0x108 / type+0x1fa),
`reload = (reloadticks·(100−6k)/100)·(120−h)/100` (integer divisions). No random term
(the engine's ±20% `retailWeaponReload` is Kingdoms). It counts down every tick in 0x49e1a0.

**SetMaxReloadTime** (0x49e14b..0x49e186, at unit creation from 0x485f04/0x4860ac/
0x4862c4): arg = `max(reloadticks over the three slots)·1000/30` ms.

**Burst**: the spawned projectile carries `burst` (+0x60, 0x49cb79) and becomes an
**emitter** (projectile tick 0x49b75e..0x49b9a9): every `burstrate` ticks, starting
`burstrate` after the shot, it decrements the count and clones itself into a flying
projectile; it re-reads the stored QueryX piece's position first when
`burstrate ≥ 5` or the count is odd (0x43e240 with the stored piece: **no new QueryX
call**); sprayangle randomises the emitter's heading; at count 0 the emitter dies. So
`burst = N` gives N shots at t+r, t+2r, … t+N·r, and **FireX, AimX, QueryX and RockUnit
are called once per burst**. Reload starts at the first (script) shot. `burst = 0`:
the spawned projectile itself flies.

**TargetCleared(slot)** (slot 0..2) is engine-driven [S]: target unit dead (0x48a2db),
auto-target finds nothing for a slot that had a target (0x408b8a → 0x48a0f0), Stop
(0x401c33..0x401c43, all slots), Paralyze (0x402d58), fire order set to hold/return fire
(0x403146, auto slots), switching a slot's auto-target bit on or off while it holds a
target (0x489800, 0x4898b0), and an order with mission flag 0x10000 (0x43b856, all
slots). Only sent when the slot actually had a target.

## 6. Fight while moving (question 6)

TotalA.exe **fires while moving; a Move order never stops for an enemy** [S]:

- Mission table (0x19-byte records from 0x4fc48c and 0x4fca14): `Move_Ground` handler
  0x4031d0 only sets up the path goal (0x438930, waits on events 0xe0). It never inserts
  an attack mission and never touches the weapons (no call to 0x4898b0, 0x489800,
  0x48a060 or 0x48a0f0).
- Weapon targeting is a separate per-slot sweep, **0x4089a0** (run for every player from
  the controller tick; each call visits about 1/30 of the unit array, so a unit is
  revisited roughly every 30 calls [U for the exact cadence]): if the unit's fire order is
  *fire at will* (`(unit+0x110 & 0x300000) == 0x200000`, set by Standing_FireOrder
  0x403100) and the slot's **auto-target bit4** is set, it keeps a valid unit target
  (no range test) or picks one with 0x40b7b0 (0x408b76) and stores it with 0x48a060.
- Bit4 is set for all slots at unit creation (0x485c58), by Standby (0x40605e), on
  order completion (0x43a2b9) and by Patrol/Follow/attack missions; it is cleared only by
  missions that drive the weapons themselves (Attack_*, Guard_NoMove, build/repair/
  reclaim/capture, Suppress, the VTOL missions …; full list: callers of 0x4898b0). Move
  is not among them.
- So a moving unit keeps its auto targets; 0x49e1a0 aims and fires every tick as in §2–4.
  Turret weapons fire freely (moving units get the wide 2000 default tolerance when the
  TDF gives none); non-turret weapons fire only when the body happens to face the
  target. Hold fire / return fire stop the sweep.
- [U] Return-fire acquisition and the standing move order's effect on idle units were
  not traced.

## 7. Engine implementation spec

What `World::tickScriptWeapon` (sim.cpp ~17670) and its helpers must do for a TA unit.
The Kingdoms handshake (`AimWeapon(h,p,slot)`, SET 21/22/23, `RetailAimState` 0xe0
countdown, `FireWeapon(slot)` releasing the shot via SET 23) does not exist in TA and
must not be used for TA scripts; that handshake is why no TA unit fires today (no
AimWeapon/FireWeapon index, and no TA script ever executes SET 22/23).

### 7.1 Names per slot

```
aim[3]    = {"AimPrimary","AimSecondary","AimTertiary"}
fire[3]   = {"FirePrimary","FireSecondary","FireTertiary"}
query[3]  = {"QueryPrimary","QuerySecondary","QueryTertiary"}
aimfrom[3]= {"AimFromPrimary","AimFromSecondary","AimFromTertiary"}
```
plus `SweetSpot` (target unit), `TargetCleared`, `RockUnit`, `SetMaxReloadTime`.
Exact-case lookup. Slots beyond 3 do not exist.

### 7.2 Per-slot state (mirror S)

`aimRequested` (bit0), `aimReady` (S+8), `aimHeading`, `aimPitch` (relative, as passed),
`reload` ticks, `autoTarget` (bit4), target (unit id or ground x/z). All of it is sim
state for `stateHash()`.

### 7.3 VM changes (`cob::RetailVm`, `RetailState`)

1. Thread word 8 = return callback. `startArguments` gets an optional callback tag
   (e.g. `0x100 | slot`); 0 = none.
2. RETURN (0x10065000): before `finish(slot)`, if `words[8] != 0`, pop the return value
   and hand `(tag, value)` to the host; the host sets `aimReady[slot] = true` iff
   `value != 0`. SIGNAL kills (0x10067000) and `clearThreads` must **not** call it.
3. If the start fails (no such script, no free thread) and a callback was requested,
   report value 0 at once (no state change for the aim callback).
4. Retail has **8** threads per unit (0x4b08c0); 16 lets scripts start threads retail
   would refuse. Use 8 for TA.
5. Default signal mask of an engine-started thread is 1 (already `words[7]=1`).

### 7.4 The tick (replaces the AimWeapon/FireWeapon body)

Call order per unit per tick: weapons for slots 0,1,2, then the unit's VM tick with
elapsed 1 (so a just-started AimX runs this tick).

For each slot with a weapon:
1. `if (reload) --reload`.
2. Resolve `T` (§3.3). None / dead target → (dead: clear target, start
   `TargetCleared(slot)`, not immediate) `aimRequested = false`; next slot.
3. If `!aimRequested`:
   - turret: `P = aimFromPoint(slot)` (AimFromX(-1) → if -1 QueryX(0) → piece point,
     invalid piece → unit origin). `d = P − T`. `heading = uint16(atan2BAM(d.x, d.z) −
     retailHeading(u))`. `pitch = ballistic ? ballisticPitch(d, weaponvelocity,
     minbarrelangle) : (lineofsight ? atan2BAM(−(d.y>>16), |d.xz|>>16) : none)`.
     No solution (0x8000 or none) → skip. Else store angles, `aimReady = false`,
     `startArguments(aim[slot], {heading, pitch, 0, 0}, 2, callback=slot)`,
     `aimRequested = true`.
   - vlaunch (and stock > 0 if stockpile): same with `{0,0}`.
   - otherwise nothing.
   **Sign check:** `retailDirectAim` is fed `target − source` in `queryWeaponAim`; TA
   uses `source − target` (§2.2 [E]). With the same heading convention
   (`retailGroundStep` moves along (−sin, −cos), like TA) that gives a heading off by
   0x8000 and a negated pitch. Feed `P − T` for TA, or confirm with a turret test.
4. If `reload == 0` and in range from the **unit origin** (§3.3) and resources/stock OK:
   - turret: need `aimRequested && aimReady`. Recompute angles from the AimFrom piece;
     no solution → `aimRequested = false`. Outside tolerance (§4.1 step 3, full pitch
     tolerance, 150/2000 defaults) → `aimRequested = false`. Else fire.
   - non-turret LOS: compare world angles from the QueryX point with the body
     heading/pitch, same tolerance. vlaunch: need `aimReady`. dropped: no aim test.
   - **Fire** = muzzle = `queryPoint(slot)` (QueryX now, before FireX runs) → spread →
     create the projectile **now** (`fire(u, t, slot, …)`), then
     `startArguments(fire[slot], {}, 0)` and `RockUnit(−800·cos h, −800·sin h)` (not
     for dropped), `aimReady = false`, `aimRequested = false`,
     `reload = (reloadticks·(100−6k)/100)·(120−h)/100`, charge energy/metal.
5. Burst: hand `burst/burstrate` and the QueryX piece number to the projectile; it emits
   the N shots itself (§5). No further script calls.

### 7.5 Query helpers

- `queryUnitScriptPoint(…, sweetSpot=false, slot)` must call `query[slot]` with args
  `{0,0,0,0}` (TA has no slot argument; Kingdoms' QueryWeapon gets `{0, slot}`), and a
  missing script means piece 0, not the unit origin.
- Add `aimFromPoint(slot)`: `aimfrom[slot]` with `{-1,0,0,0}`, fallback to the query.
- `SweetSpot` with `{0,0,0,0}`; missing → piece 0.

### 7.6 Targeting and TargetCleared

- Auto targets (the 0x4089a0 sweep) only with fire-at-will and `autoTarget`; Move keeps
  `autoTarget`, so units fight while moving (§6).
- `TargetCleared(slot)` with one argument (slot 0..2), not immediate, only when a slot
  that held a target loses it (§5 list).
- `SetMaxReloadTime(max reloadticks · 1000 / 30)` at creation (compare
  `maxWeaponReloadMs`, which must be built from the integer tick values).

### 7.7 Kingdoms features to bypass for TA

`RetailAimState` (0xe0 countdown, SET 22/23), `retailWeaponReload` (±20% random),
`retailWeaponFireEvent`, `legionAimStall`, the `aim.flags&16` release path, the
facing-request gate (`slotNeedsFacing`), `AimWeapon`'s third argument, and the
Kingdoms tolerance/2 pitch test.
