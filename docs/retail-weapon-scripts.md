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

### 7.8 State in the engine

Implemented (`World::tickScriptWeapon`, `taAimAngles`, `aimFromPoint`, `taWeaponScript`,
`tickWeaponBursts`; `cob::RetailVm` word 8; test `tools/ta_weapon_script_test.cpp`):
7.1-7.7 as written, with these differences:

- The engine runs every unit's COB pass before the weapon update, so an AimX or FireX
  started in tick N runs in the pass of tick N+1 (retail: the same tick). Order within a
  unit is unchanged; readiness and the shot come one tick later.
- The fire gate is reached only from the engine's callers, which test the slot's range
  first; an aim therefore starts once the target is in range, not as soon as it is held.
- A ballistic shell launches as 0x49cde0 does: on AimX's stored angles plus the spread,
  from the QueryX point, with vy less gravity x (barrel word S+0x10 / velocity), the barrel
  word being 1.25 x (QueryX.z - AimFromX.z) in the creation pose (0x49e0ea..0x49e11b); it
  moves, drifts with the wind vector and falls once a tick (0x49bce3; `taBallisticStep`).
  The pitch is 0x49a890 with minbarrelangle (`ta::taBallisticPitch`, checked by
  tools/re/ballistic_emu.py); the range gate 0x49aa80's surfaced and arc tests
  run in the fire path (`World::taRangeGate`); its horizontal range stays the callers' test,
  to the target's origin rather than the target point. Line-of-sight and selfprop shots match retail: 0x49c9c0 aims from
  the QueryX point straight at the target point and ignores the spread.
- Burst clones copy the emitter (0x49b83b): its position (re-read from the stored QueryX
  piece when burstrate >= 5 or the count is odd), velocity and angles; sprayangle turns the
  emitter's horizontal velocity for the next clone (0x49b8f1); randomdecay jitters a clone's
  expiry. The emitter is the spawner's own record.
- Non-turret LOS weapons compare only the heading with the body (no body-pitch test); a
  stockpile weapon's stock is not checked for vlaunch.
- Sign check (`ta_weapon_script_test`): with d = P - T the AimPrimary heading plus the body
  heading points from the turret at targets ahead, behind and to both sides, in the
  mover's own convention (a body with heading h steps along (-sin h, -cos h)).
  `queryWeaponAim`'s `retailDirectAim(target - source)` is the launch convention
  (projectile velocity (+sin, +cos)); it is not used for AimX.

Observed consequence of 2.4: scripts that give two weapons the same aim signal (armwar's
AimPrimary and AimSecondary both send and mask signal 4; armcom's primary and tertiary
share 2) lose the lower slot's aim thread when both aims start in one tick; that slot
then waits until its target is lost (armwar's Light Cannon never fires at a target the
EMG also aims at). This follows the spec; it is not verified against a running TotalA.exe.

## 8. Maneuver chase, D-gun and shared aim signals

Answers §6's open [U] (idle units and the standing move order) and two slot questions.
Unit `+0x110`: **move state = bits 18-19** (`(f >> 18) & 3`: 0 Hold Position, 1 Maneuver,
2 Roam), **fire state = bits 20-21** (0 Hold Fire, 1 Return Fire, 2 Fire At Will), from
FBI flagsA bits 0-1 / 2-3 at creation (0x485c8a..0x485cb6) and from the
`Standing_MoveOrder` 0x4030d0 / `Standing_FireOrder` 0x403100 missions [S].
(`retail-ai.md` §"Unit" had the two fields swapped; fixed.)

Mission records (25 bytes, the copy at `[0x512344]`): +0 status text, +4 handler, +0x11
dword = mission flags, +0x15 name. Handlers used below: Standby 0x405fe0,
Attack_NoMove 0x402160, Attack_Chase 0x4034a0, AttackSpecial 0x403190, Suppress
0x4038a0, Move_Ground 0x4031d0 [S]. Dispatcher 0x43b7c0: handler result 1 = next state,
2/4 = stay, 5/8 = order finished (unlinked, the loop runs the new head at once), 7 =
clear the queue; when the queue is empty a type-1/2 owner's unit gets a fresh order of
its `defaultmissiontype` (0x43b9ad..0x43ba30) [S].

### 8.1 Idle Maneuver chase [S]

**Standby** (0x405fe0), a ground unit's default mission:
- stage 0: all slots automatic (0x489800(3)), wake next tick;
- stage 1: scan 0x43b700; a hit goes to 0x43b1f0(unit, hit, 0) and, if that inserted
  orders, Standby returns 5 (retires). Otherwise it sleeps **30 + rand(30)** ticks
  (0x4b6c30, 0x439e80; it also wakes on order event 0x10000, source [U]).

**Scan 0x43b700**: only with fire state 2; then the picker 0x40b7b0(unit, slot 0,
mode 0). Mode 0 gathers the owner's known-enemy list (0x40ad80, list player+5, fallback
+0x19) within the unit's own **sightdistance** (type+0x202) of its **current position**,
then up to 50 random draws keeping min `rand(d²)` (non-bad-category first, §6). Every
candidate must pass **0x49abb0(unit, cand, 0) = slot 0's range gate** (horizontal
centre distance² ≤ range², plus the waterweapon/submerged/toairweapon/ballistic gates;
skipped only for kamikaze types, flagsA bit28) and must not be in `noChaseCategory`
(+0x23d, mode 0 only). **So Standby only ever takes an enemy already inside the scanning
weapon's range.** There is no leash test in the scan.

**0x43b1f0(unit, target, force)** (also Patrol 0x4033eb, the hit response 0x4070dd,
guard 0x4063f5):
- target == unit → 0. Hold Position and `!force` → 0. Hold Fire and `!force` → 0.
- mission = 0x43f0e0(action 3, unit, target): mobile (unit+0 ≠ 0) → ATTACK_CHASE,
  armed static → ATTACK_NOMOVE, flyers → AIRSTRIKE/AIRTOAIR/... ; none → 0.
- **Maneuver and !force**: insert at the head `Move_Ground(unit's 16.16 position now)`,
  then ahead of it `Attack_Chase(target, leash = maneuverleashlength (type+0x214),
  post +0x2e/+0x30 = the unit's integer x/z now)` (order ctor 0x43a0c0, leash at order
  +0x3e). Both inherit the head order's 0x4000 flag.
- **Roam, or force**: insert `Attack(target)` with leash 0; no return move.
- returns 1.

**Attack_Chase** (0x4034a0), on every run (each wake: 30-tick timers in stages 3, path
events 0xe0/0x40e0, fire/target events): first, if `leash ≠ 0` and
`trunc(hypot(unit.x_int − post.x, unit.z_int − post.z)) ≥ leash` (0x4fb440 → _ftol)
→ **5** (finished). It also finishes on event 0x800 (a commandfire shot, §8.2), on
0x10008, or with no target. The leash is measured from the **post to the unit itself**,
not to the target. Stages: 0 setup (slot = order +0x36, else 0x4897e0 = first slot with a
weapon); 1/3 target in range of that slot (0x49abb0) → slots 0 and 2 manual, the slot
gets the target (0x48a060), wait; out of range → all slots automatic again
(0x489800(3)) and stage 2 paths toward the target (0x438930/0x438a00 with radii from the
slot's range 0x49adf0, substates at order +0x3a).

When the chase finishes the Move_Ground walks back to the post; the empty queue then
gets a new Standby at the post.

**Answer.** An idle human unit on Maneuver does **not** advance on a visible enemy
that is inside its leash but outside its weapon range: Standby's scan only takes
targets inside slot 0's range (and inside its sightdistance). The leash bounds only
how far it **follows** a target it already engaged (one that walks out of range): it
chases until it is `maneuverleashlength` px from where it stood, then walks back and
resumes Standby. Same situation:
- **Hold Position (0)**: 0x43b1f0 refuses; no order ever, the unit never moves. Its
  automatic slots still fire at anything in range (sweep 0x4089a0).
- **Roam (2)**: the same in-range-only pick; the chase has no leash and no return
  move, so it follows the target anywhere; Standby restarts wherever the chase ended.
- Being hit (0x406f80, owner type 1/2, armed, finished, head order absent or with mission
  flag 0x20000 such as Standby) also calls 0x43b1f0(unit, attacker, 0) only when the
  attacker is in **slot 0's range** and not in noChase/slot-0 badTarget; otherwise, with
  fire state ≠ 0, each automatic non-commandfire slot whose current target is none, out
  of range or bad takes the attacker if the attacker is in that slot's range
  (0x407102..0x40718d). Return Fire units on Maneuver thus chase (leashed) an in-range
  attacker. A shooter out of range is never answered by movement.
- AI owners: squad assignment 0x408830 sets new units to Fire At Will and to Maneuver
  if the type has flagsB bit12 (cancapture: the commanders), Roam otherwise.

**Engine spec (World).** The ported pieces already match: `retailHostScan` /
`retailSelectTarget(sweep=false)` require `slotInRange` (= 0x49abb0), and
`retailIssueAutoAttack` is 0x43b1f0 (Hold → none, Maneuver → leashed attack + return
Move, Roam → plain). Keep that. Changes for TA:
1. Mission-scan radius = `sightdistance` only (0x40b81f..: type+0x202), not
   `max(sight, range)`; scanning slot = 0x4897e0 (first slot with a weapon).
2. TA has no Standby_Wander: `retailWanderScan`'s "drop a hit at/after the leash from the
   post" is Kingdoms; a TA ground unit always uses Standby. Standby period: 30 + rand(30).
3. Chase end: retire the attack when `trunc(hypot(int dx, int dz)) ≥ leash` between the
   unit's whole-pixel position and the post (already `chaseLeash`), then the return Move.
4. **`tools/retailgap_test.cpp` "a leashed unit advances on an enemy inside its leash"
   expects non-retail behaviour** (its enemy is at `maxRange()+60`, outside armpw's EMG
   range): retail stays put. A retail test: put the enemy in range, then move it away
   (e.g. order it to walk off): the armpw follows, stops following once it is 640 px
   from its start, walks back, and is idle there again; on Hold Position it never moves;
   on Roam it follows without limit and does not return.

### 8.2 D-gun and commandfire [S]

- **Sweep** 0x4089a0(arg): per automatic slot (bit4) with a weapon, not `dropped`;
  **if arg == 0 (any non-AI owner) a commandfire weapon (+0x111 bit26) is skipped**
  (0x408a88..0x408a96). The controller tick passes arg 1 for AI owners (type 2), so their
  commandfire slots are swept like any other. Guard target picks (0x406445, 0x40fdeb) and
  the hit response's return fire (0x407137) also skip commandfire slots for everyone.
- **Plain Attack on a unit** (Attack_Chase): slots **0 and 2** are made manual (0x4898b0:
  bit4 off; a held target is cleared with `TargetCleared(slot)`), the order slot (slot 0
  for armcom) gets the target, **slot 1 stays automatic** (fed by the sweep, so only on
  Fire At Will, with its own pick in its own range). armcom: the laser fires, the D-gun
  (slot 2) has no target and never fires. Static attackers (Attack_NoMove 0x402160):
  slot 0 manual + target, slots 1 and 2 stay automatic.
- **Ground attack** (action 3 without target → SUPPRESS 0x4038a0): slots 0 and 1 manual,
  both get the point (0x48a0a0); slot 2 stays automatic (a human's commandfire slot is
  then idle; an AI's sweeps).
- **D-gun order** = mission **AttackSpecial** (picker action 4, 0x43f7e8, requires
  `candgun`, flagsB bit14). Handler 0x403190 turns the order into the action-3 mission for
  the same target (0x438b90) and sets order **+0x36 = 2**:
  - unit target → Attack_Chase with slot 2: slots 0 and 2 manual, **slot 2** gets the
    target, slot 1 automatic; chase range = slot 2's range (240 for ARM_DISINTEGRATOR).
    A commandfire shot posts fired bit **0x800** (0x49e4f2; others 0x400) and
    Attack_Chase finishes on 0x800 (0x4034af): **one D-gun shot per order**.
  - ground → Suppress with slot 2: **all** slots manual, only slot 2 gets the point
    (0x40396b..0x40398a). Suppress does not finish on 0x800; it walks its counter
    (order +0x3a = range, minus rand(range/3) per shot event, 0x40392e..) [shot count U].
  - the fire itself is the normal weapon update (§2-4): reload, 400 energy per shot.
- **AI owners** never issue AttackSpecial (the AI controller's 0x43f0e0 calls use
  actions 2, 3, 9, 0xe: 0x40804c, 0x408299, 0x408403, 0x408435, 0x408589). Their D-gun
  fires only through the sweep: whenever slot 2 is automatic (Standby, Move, Patrol,
  ground Suppress, Attack_NoMove) the commander auto-targets enemies within 240 and
  fires when stores allow. During an Attack_Chase slot 2 is manual and silent.

**Engine spec.** Order gets a `slot` (+0x36). Attack-on-unit: `setSlotMode` manual for
slots 0 and 2 only (with TargetCleared when one held a target), target into `slot`,
slot 1 untouched; Attack_NoMove: slot 0 only. Ground attack: slots 0 and 1 manual with
the point; D-gun ground: all three manual, point in slot 2. The sweep skips commandfire
slots unless the owner is AI. D-gun command available only with `candgun`; the unit
D-gun order ends after its first commandfire shot (track "this unit fired a commandfire
weapon this tick" as the 0x800 event).

### 8.3 Two aims, one signal [E for the VM, S for the rest]

**armwar.cob** (cobtool disasm): Weapon1 = ARMWAR_LCANNON (slot 0, range 240, reload 2.13 s),
Weapon2 = ARMWAR_EMG (slot 1, range 180, reload 0.4 s). AimPrimary and AimSecondary are
identical except the arm piece:
`signal 4; set-signal-mask 4; static1=1; while(!static2) sleep 100; turn torso y to
heading; turn arm x to -pitch; wait-for-turn torso y; wait-for-turn arm x;
start-script RestoreAfterDelay(); return 1` (RestoreAfterDelay inherits mask 4).

(a) **Start order.** 0x49e1a0 loops slots 0,1,2 in one call per unit per tick and starts
`AimX` (not immediate) for every slot that has a target and no outstanding aim
(0x49e213/0x49e357 → 0x4b0a70). The unit's VM pass 0x4b0d60 runs right after, in the
same tick. So two slots that become targetable in the same tick start their aims in
the same tick; a slot that gets its target later starts later. No one-slot-per-tick
rule and no wait for the other slot.

(b) **VM** (0x4b08c0, 0x4b0d60, 0x4b0da0, SIGNAL 0x4b1a75; emulated with
`tools/re/cob_signal_emu.py`, 0x4b0a70 + 0x4b0da0 under unicorn):
- 8 threads; a new thread takes the **lowest free index**, status 0x1000000, mask 1.
- A pass runs threads **in index order**; each runs until it blocks or ends.
- **WAIT_TURN always yields** (status 0x2100000) and is re-tested at the thread's
  next pass (turn speed of that piece/axis 0 → continue), even if the piece is already
  there [E].
- **SIGNAL s** kills every thread with status ≠ 0 and `mask & s` ≠ 0: running, blocked,
  sleeping, or **created this tick but not yet run** (whose mask is still 1), and itself
  if its own mask matches. No callback. Killing a thread wakes a parent waiting on it
  (status 0x28xxxxx with word6 == index). [E: case 3, SIGNAL 1 killed an unrun thread,
  SIGNAL 4 did not.]
- Same-tick starts [E, case 1]: AimPrimary is allocated first, so it has the lower index,
  runs first, sets mask 4 and blocks at WAIT_TURN; AimSecondary then runs in the same pass
  and its `signal 4` kills it. Result: slot 0 never gets a callback, slot 1 ready.
- Staggered starts [E, case 2]: an aim started while the other slot's aim thread is
  still alive (blocked turning) kills it, whichever slot it is.

(c) **No re-arm.** Aim requested (S+0x1b bit0) is cleared only at 0x49e1ea (the slot has
no target at the weapon update), 0x49d65e/0x49d68d (no solution / out of tolerance, both
only after ready), 0x49d795 (shot) and 0x49dd36 (vlaunch shot). No timeout, nothing on
reload, and retargeting (0x48a060/0x48a0a0) keeps the bit. A killed aim therefore stays
pending until the slot's target becomes "none" at an update: the target dies (0x48a1e0),
Stop, Hold/Return Fire, or a mission toggling the slot's auto bit while it holds a
target (0x489800/0x4898b0 clear it with TargetCleared, e.g. Attack_Chase going out of
range → 0x489800(3), or a new Attack order's 0x4898b0(0)). The sweep keeps a living
enemy target with no range test, so an idle unit's stuck slot stays stuck for the whole
life of that target.

**Conclusion.** In retail armwar does **not** reliably fire both weapons at one target.
If both slots acquire it at the same sweep visit (an enemy that appears inside both
ranges), the Light Cannon's first aim is killed in the first VM pass and the cannon is
silent until that target is gone; only the EMG fires. If the cannon acquires first (an
enemy walking in from 240 px), both fire until one slot starts an aim while the other's
aim thread is still turning; the killed slot is then silent for that target. The same
holds for armcom slots 0 and 2 (signal 2). This is the retail behaviour; it is not a
port artefact. (Not observed in a running TotalA.exe.)

**Engine spec.** To reproduce retail exactly:
1. `cob::RetailVm`: 8 threads, lowest-free allocation, index-order pass, SIGNAL as
   above (any non-free thread, including unrun ones and the caller), no callback on kill,
   WAIT_TURN/WAIT_MOVE always yield one pass.
2. Per unit per tick: weapon update (slots 0,1,2) **then** the unit's VM pass (§7.8
   currently runs the VM first; with that order a same-tick pair still kills slot 0, but
   staggered collisions happen one tick later than retail).
3. Clear `aimRequested` only at the sites in (c). Keep `aimRequested` across
   `setSlotTarget` to a new live target. Any timed re-arm would be an engine
   deviation, not retail.
