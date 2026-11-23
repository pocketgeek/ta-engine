# Retail economy (TotalA.exe v3.1c, GOG Commander Pack)

Clean-room notes: what the binary does, with the address that shows it. Nothing
here is copied code. Tools: `tools/re/economy_*.py` (`economy_emu.py` runs the
routines under unicorn on made-up structs and prints the results; every
"emulated" tag below comes from that script).

**Confidence tags:** **[S]** = read from the code, **[E]** = also checked by
emulation, **[?]** = inferred or not confirmed.

## 0. Structures and field names

Unit-type fields filled by the FBI parser at `0x42c0f2..0x42c548`. `4c46c0`
reads an int and `4c4760` reads a float. The store lands one call later, after
the `fstp`.

| FBI key | type off | stored as |
|---|---|---|
| BuildCostEnergy / BuildCostMetal | 0x186 / 0x18a | float, from an **int** parse (`0x42c0f8`, `0x42c115`) |
| EnergyMake / EnergyUse / MetalMake | 0x1c2 / 0x1c6 / 0x1ca | float |
| ExtractsMetal | 0x1ce | float |
| MakesMetal | 0x22d | **u8 from an int parse** (`0x42c2d3`), so fractions truncate |
| WindGenerator / TidalGenerator | 0x1d2 / 0x1d6 | float |
| cloakcost / cloakcostmoving | 0x1da / 0x1de | float from an int; `moving` defaults to `cloakcost` (`0x42c51c`) |
| EnergyStorage / MetalStorage | 0x1e2 / 0x1e6 | float |
| BuildTime | 0x1ea | int |
| WorkerTime | 0x1fe | u16 |
| MaxDamage | 0x1fa | int |
| BMCode | 0x22f | u8; at spawn, unit flag `0x20000000` = (BMCode==0), i.e. "structure" (`0x485a81..0x485a9e`) |
| weapon energypershot / metalpershot | wtype 0xc0 / 0xc4 | float (`0x42e55b`) |
| feature metal / energy | ftype 0xf0 / 0xec | float, from an int & 0xffff (`0x422a3b`) |
| feature flags word 0xfe | bit7 reclaimable, bit8 autoreclaimable (default 1), bit9 indestructible | `0x422a92..0x422c62` |

**Player** (array at `game+0x1b63`, stride 0x14b; `[p+0x27]` points to the lobby info record):
- 0x8c = stored E, 0x98 = stored M
- 0x90 / 0x94 = E income / E requested, 0x9c / 0xa0 = M income / M requested (the last settle's sums)
- 0xa4 / 0xa8 = E / M storage cap, rebuilt at every settle
- 0xdc / 0xe0 = base E / M storage, used only when flag `p+0x149` bit0 is set
- 0xe4 / 0xe8 = M / E share thresholds
- qword running totals: 0xac E produced, 0xb4 M produced, 0xbc E consumed, 0xc4 M consumed, 0xcc E wasted, 0xd4 M wasted
- 0xec = pointer to a player-level accumulator (sharing goes through it)
- 0xf0 = tick of the next settle
- 0x73 = player kind: 1 local human, 2 AI, 3 remote **[?]** (from how it is used)

**Resource accumulator** (0x34 bytes; constructor `0x401070`; one per unit at
`unit+0xbc`, plus the player-level one). Each field is a float:
- +0 E produced, +4 E requested (display), +8 E new requests (pooled), +0xc **E debt**
- +0x10 / +0x14 last E produced / requested
- +0x18..+0x2c the same six fields for metal
- +0x30 = player pointer

Accumulator methods:
- `0x401180(acc, e)`: E requested += e. If E debt <= 0, E new += e and return 1 (go ahead); otherwise return 0 (stalled).
- `0x4011c0(e, m)` (thiscall): adds both to requested. Only if **both** debts <= 0 does it add both to the new requests and return 1.
- `0x401220(e)` / `0x401260(m)` / `0x4012a0(e, m)`: **pay now from storage**. If stored >= cost, take it from storage, add it to requested, and return 1. `0x4012a0` needs both resources (E is taken before M is checked).

## 1. Tick rate and settle cadence

- The sim clock is `GetTickCount*30/1000`: `0x4b6340` uses the multiplier `0x1e` set at `0x49e98f`. Ticks to run each frame = elapsed × gamespeed × 0.1 + carried fraction, capped at 5 (`0x495370..0x4953fe`). **gamespeed 10 = 30 ticks/s.** The registry default is unconfirmed **[?]**.
- Sim tick `0x495490`: `tick++` (`0x4954c0`) → `0x48ad30` (units/orders) → … → `0x464f80` (player loop) → … → `0x490c40` (wind, `0x49558f`) → … → `0x457d30` (local auto-share, `0x49563a`).
- **Economy settle `0x401360(player)`** runs from `0x46555a` inside `0x464f80`, and only when `p.nextSettle(0xf0) <= tick`; then `nextSettle += 30` (`0x465077..0x465092`). `nextSettle` starts at the tick the player is set up (`0x464715`). So settles happen **once every 30 ticks (1 s) per player**, and **FBI rates are per second**. Only player kinds 1 and 2 are settled (`0x465539`); remote players are settled on their own machines.
- Builders, repairers and the like run every tick: their order handlers sleep one tick (`0x439e80(1)`, wake check `0x43b7e8`). Their requests pile up in the accumulator until the next settle **[S]**.

## 2. Settle algorithm (`0x401360`) [E]

1. Set the caps to 0 (`0x40137d`).
2. Loop over the player's unit slots in order (`[p+0x67]..[p+0x6b]`, stride 0x118). Skip a slot unless its flags include `0x10000000`.
   - **Structure** (flag `0x20000000`) **and activated** (`unit+0x10e` bit0, the Activate/Deactivate state, `0x48b090`):
     - `eu = EnergyUse`.
     - If `eu >= 0`: E requested += eu. If E debt > 0, `ok = 0`; otherwise E new += eu and `ok = 1`.
     - If `eu < 0`: E produced += mult·(−eu) and `ok = 0`. This is how the solar collector works (`EnergyUse=-20`).
     - Then **only the first matching branch runs**:
       1. If ExtractsMetal > 0: when `ok`, M produced += mult·`unit+0x58`.
       2. Else if MakesMetal ≠ 0: when `ok`, M produced += mult·MakesMetal.
       3. Else if WindGenerator > 0: E produced += mult·(windF·WindGenerator) (`0x40156f`).
       4. Else if TidalGenerator > 0: E produced += mult·(tidal·TidalGenerator) (`0x4015df`).
   - **Mobile unit, activated or moving** (flags & 0xC **[?]**): the same `eu` handling, with no extractor, maker, wind or tidal branch (`0x401617`).
   - **Fully built** (`unit+0x104` build-remaining == 0.0): E produced += mult·EnergyMake and M produced += mult·MetalMake. Both apply **whether or not the unit is activated**. The cap gains EnergyStorage / MetalStorage (`0x4016c5..0x4017c5`).
   - **Cloak** (skipped for player kind 3). Requirements:
     - flag 0x800 (cloak wanted) is set and flag 0x1000 is clear;
     - `unit+0xb0 <= tick`. Building, repairing, reclaiming and resurrecting set `unit+0xb0` to tick+150, tick+300 or tick+900.

     Cost = `trunc(moving ? cloakcostmoving : cloakcost)`. If cost <= stored E, take it from storage **at once**, add it to E requested, and set the cloak on. Otherwise set the cloak off (`0x48b090(4, on)`, `0x4017d9..0x401872`).
   - Add the unit's accumulator fields into the per-player sums (float32 adds).
3. Add in the player-level accumulator (`0x40190e`). If `p+0x149` bit0 is set: capE += baseE and capM += baseM (`0x401986`).
4. Publish for the HUD: income = ΣProduced and requested = ΣRequested. The qword stats gain produced and requested; requested is counted as "consumed" (`0x4019b0..0x401a49`).
5. For E, then M (`0x401a4d` loop):
   - `avail = stored + ΣProduced`.
   - `fDebt = (ΣDebt <= avail) ? 1 : avail/ΣDebt`, then `avail -= min(ΣDebt, avail)`.
   - `fNew = (ΣNew <= avail) ? 1 : avail/ΣNew`, then `avail -= min(ΣNew, avail)`.
6. `stored = avail`. If avail > cap, then stored = cap and wasted += avail − cap (`0x401ab3..0x401b27`).
7. For every live unit and for the player-level accumulator (`0x401b37`, `0x401bae`):
   - last produced = produced and last requested = requested;
   - produced = requested = new = 0;
   - **debt = new·(1−fNew) + debt·(1−fDebt)**, for each resource.

`mult` (the AI handicap) is applied wherever production is credited:
- player kind 2, difficulty `game+0x37eee` 0 (EASY): × 0.5;
- difficulty 1 (MEDIUM): × 0.7;
- otherwise × 1.0.

Names come from `0x40c3cf`; constants are at `0x4fc480` / `0x4fc488`. Emulated: 100 E/s becomes 50 / 70 / 100. It is **not** applied to consumption.

**What a shortage does [E].** There are no priorities by unit type. All of last second's new requests get the same fraction `fNew`, and the unpaid part becomes per-unit **debt**. Old debt is paid before new requests. A unit whose debt (E, or E or M for builders) is above 0 cannot act. Its request is still counted in "requested" for the HUD, but it is not pooled and its work does not happen. Units act optimistically: a request accepted during the second runs at once and is reconciled at the settle.

Emulated example: stored 0, income 54 E/s, a mex (3 E/s) and a maker (60 E/s), plus an older 5 E debt. The old debt is cleared first, then `fNew = 49/63`. The maker carries 13.33 E of debt; the mex still made its metal that second. A unit that starts the second in debt makes no metal that second.

## 3. Construction (`0x41ba60(builder, target, amt)`) [E]

The order handlers call this every tick with `amt = (float)(WorkerTime/30)`, where the division is a **signed int** divide (magic `0x88888889`, e.g. `0x4029db`, `0x403e12`, `0x404118`).

- If remaining == 0, return.
- `new = rem − amt/BuildTime` and `c = clamp(new, 0, 1)` (`0x41bac1..0x41bb1e`).
- `d = rem − c`, `E = BuildCostEnergy·d`, `M = BuildCostMetal·d`.
- `hp += trunc(rem·MaxDamage) − trunc(c·MaxDamage)`, capped at MaxDamage (`0x41bc69..0x41bc89`).
- The progress is applied only if `0x4011c0(E, M)` on the **builder's** accumulator returns 1, i.e. both debts are <= 0.
- When remaining reaches 0, `0x41b8d0` completes the unit (sets Activate if the type has ActivateWhenBuilt).

Emulated: WorkerTime 300 (amt 10), BuildTime 1000, cost 1000 E / 100 M, HP 500 → each tick takes rem −0.01, +5 HP, 10 E, 1 M; a builder in debt makes no progress.

So the drain is **cost × WorkerTime / BuildTime / 30 per tick, i.e. cost × WorkerTime / BuildTime per second**. Build time in seconds is BuildTime / floor(WorkerTime/30) / 30.

A negative `amt` (deconstruct) runs the frame backwards and credits `BuildCostMetal·|d|` to the **target's** M produced. It costs nothing, and a frame taken back to full is killed with 30000 damage (`0x41bbfa..0x41bc49`). The only caller computing a negative amount (`0x41bcd0`) has no callers found **[?]**.

**Repair** (`0x41bd10`, every tick):
- `e = trunc((BCE·amt − 1)/BuildTime + 1)` and `h = trunc((MaxDamage·amt − 1)/BuildTime + 1)`; each is then capped at 1 (`0x41bd87..0x41bda4`).
- If `0x401180(e)` succeeds, heal `h` (`0x489bb0` type 10, capped at MaxDamage, `0x489d59`).
- In practice that is **1 E and 1 HP per tick whatever the WorkerTime** (emulated with three very different units). The repairer can't cloak for 150 ticks.

**Stockpile weapons** pay `energypershot` / `metalpershot` gradually through `0x4011c0` (`0x402bf3..0x402c6b`) **[S, not detailed]**.

## 4. Weapons, sharing, HUD

- **Weapons** (`0x49e3ed`): a non-stockpile weapon fires only if stored E >= energypershot **and** stored M >= metalpershot. After it fires, `0x4012a0` takes both from storage at once (`0x49e533`).
- **Manual give** (`0x464b30` E, `0x464c60` M; Share dialog `0x49362c` / `0x49365e`, console `0x416c8d` / `0x416ce3`):
  - the amount is capped at the giver's stored amount;
  - it is taken from storage at once (shown under requested);
  - it is credited to the receiver's player-level accumulator as produced, so it lands at **their** next settle, limited by their cap; the excess is wasted. The receiver's AI mult applies.
- **Auto-share** (`0x457d30`, local player only, multiplayer flag `game+0x2a44`&1, every tick where `tick % 60 == 0`):
  - **Metal**, if info+0x97 bit1 is set and stored M > threshold (`p+0xe4`):
    - Candidates are players with kind 3, `info+0x94 == 1`, allied (`p+0x108[i]`), and stored M lower than ours.
    - The **last** candidate by index gets `min((storedM − thr)/3, theirCapM − theirStoredM)`.
  - **Energy**: the same with info+0x97 bit2, threshold `p+0xe8`, and factor **1/2** (`0x4fd4bc`).
  - Thresholds start at 0 (`0x4647b3`). The console sets them to `min(N, cap)` ("Will share metal if above %d", `0x419340` / `0x419400`).
- **HUD** (`0x468e80..0x468fbf`):
  - The stored figures shown move toward the real ones by `(target − shown)/8` per frame (at least ±1) and are capped at the cap.
  - Income and expense come from `p+0x90/0x94/0x9c/0xa0` (`0x464ab0..0x464b10`) and are refreshed every 30 ticks (`p+0xf8`).
  - **Expense = everything requested last second**: stalled requests, cloak, shots and gifts included. **Income** = everything produced after the AI mult. There is no smoothing beyond the 1 s window.

## 5. Metal map and extraction

- Cell grid: 13-byte cells at `game+0x14287`, width/height at `0x14233` / `0x14237`; byte +7 = metal (`0x481550`). The 4-byte TNT attribute record holds no metal.
- On load (`0x4839b3..0x4839ed`), **every cell gets `SurfaceMetal`**. That is the OTA schema int (`0x4366be`), used if it is >= 0 and the TNT version is >= 0x2000; otherwise 0.
- After features are placed (`0x422040`, called from `0x483d7d`), every feature with metal ≠ 0 and **indestructible** set writes `cell = (u8)trunc(metal)` over its footprint, overwriting what was there (`0x4220ab..0x422104`). The metal-patch features (e.g. `features/archi/metal.tdf` ArchMetal1 metal=187, 3×3) supply the per-cell metal.
- `MohoMetal` is never read; no such string exists.
- **Extractor yield** (`0x437840`, run once at unit creation from `0x485f0a`/`0x4860b2`/`0x4862ca`): `unit+0x58 = ExtractsMetal × Σ_{footprint cells}(metal + 1)`.
  - The sum is a u16 placed as 16.16 and multiplied by 2^-16, so it is exact.
  - **+1 per cell**: a 3×3 mex on bare ground (SurfaceMetal 0) still gives 9 × 0.001 = 0.009 M/s.
  - The area is the extractor's own footprint (x, y at `unit+0x76/0x78`, w, h at `unit+0x7e/0x80`); there is no radius.
  - Cells are never used up. Two extractors can only share cells if their footprints overlap, which placement normally prevents **[?]**.
  - The COB `SetSpeed` gets Σ(metal+1) (`0x4378f0`).
  - Emulated: a 2×2 on 10/20/30/40 gives 104 × 0.001.
- The value is paid out once per second, and only while the unit is activated and not in E debt (§2).

## 6. Wind and tide

- Min/max wind (`game+0x1425b/0x1425f`, `0x483796..0x4837db`): taken from the OTA `minwindspeed`/`maxwindspeed` (parser default 0) when the field is >= 0 (it starts at −1, `0x435e0f`) and the TNT version is >= 0x2000. Otherwise **100 / 2000** (`0x483693`).
- Tidal (`game+0x14267`, `0x48384e`) = OTA `tidalstrength` (float) if >= 0, else 0.5. It is constant for the game.
- Update `0x490c40`, checked every tick, initialised at load with divisor 5000 (`0x4918ed`):
  - If `nextChange < tick` (strict):
    - `nextChange += (5 + (crt_rand()·10)/32768)·30`, i.e. **5–14 s**. Note `crt_rand` = the MSVC `rand` LCG `0x4e4870`, which is **not** the synced RNG.
    - `speed = min + R(max − min)`, using the synced Park-Miller RNG `0x4b6c30`: `seed = seed·16807 mod (2^31−1)`, return `seed % n`. For `n < 2` it returns 0 without advancing the seed.
    - If speed ≠ 0: `dir = R(65536)`.
    - `windF = speed / 5000` (int/int on the x87), and set to 1.0 if it is > 1.0.
    - Set flag `0x37ee2` so windmills get `SetDirection`/`SetSpeed` script calls (`0x437910`).
  - Emulated: rand 0 / 16383 / 32767 adds 150 / 270 / 420 ticks.
- Wind generator output = `windF × WindGenerator` E/s. ARMWIN (30) gives 30·speed/5000, i.e. at most 12 with max 2000. Tidal output = `tidal × TidalGenerator`.

## 7. Reclaim, resurrect, capture, death

Order table at `0x4fc840` (record = status, handler, …, name).

- **Feature reclaim**:
  - Ground handler `0x404ad0`: timer = `trunc(15 + 0.5·(M+E))` ticks (`0x404bba`). VTOL handler `0x414770`: `trunc(30 + 0.5·(M+E))` (`0x414919`).
  - The timer counts down 2 every 2 ticks (`0x404c73..0x404c7f`) and **does not depend on WorkerTime**.
  - On completion, `0x4237d0` credits the **whole** feature energy and metal to the reclaimer's produced (AI mult applies, `0x4238b3..0x42395b`) and removes the feature.
  - Wrecks are ordinary features; their metal comes from the corpse TDF (e.g. armlab_dead metal=564), not from the unit.
- **Unit reclaim** (`0x404730`):
  - Every 15 ticks, damage of type 5 = `max(1, trunc(WT·((kills+5)/5)·MaxHP·15 / (max(BCM,10)·300)))` (`0x438650`, `0x404981`).
  - When the target dies from type-5 damage, the killer's M produced += `(1 − remaining)·BuildCostMetal` (`0x486c9e..0x486d1e`). There is no energy refund.
  - So a full reclaim takes about `BCM·10/WT` seconds.
- **Resurrect** (`0x404db0`): timer = `trunc(BuildTime·0.3 / (WT/30))` (`0x404fb0`). No resource cost was found in the handler **[?]**.
- **Capture** (`0x404270`): timer = `min(trunc(15·BCE/1000 + 30·BCM/140 + 150), 1800)` ticks (`0x404359..0x40439b`). No resource transfer.
- **Death or self-destruct**: no refund. The only payout on death is the type-5 reclaim case **[S]**.

## 8. Starting resources (`0x465e30`, commander spawn `0x4653e2`)

- **Mode 3** (`0x435100()==3`, multiplayer/skirmish **[?]**):
  - All players take the host's lobby values: `info+0xa1` = energy/100 and `info+0xa3` = metal/100 (the lobby slider stores value/100, `0x445dfe` / `0x445d0a`).
  - stored E = a1·100 and stored M = a3·100 (`0x465eb2..0x465eed`).
  - Commander spawn sets base storage = **max(start, 200)** for each resource and sets flag 0x149 (`0x496e90`). It also adds the start amounts to the commander's produced, so the first settle overfills the cap and logs waste **[S]**.
- **Mode 1** (campaign): the OTA `HumanMetal/HumanEnergy` (player 0) and `ComputerMetal/ComputerEnergy` (player 1) become both the stored amount and, via `0x496e90`, the base storage.
- **Mode 2**: a per-player table at `game+0x29a0` (+0xc metal, +0x10 energy) **[?]**.
- The commander adds EnergyMake 25 / MetalMake 1 per second (FBI). It has no storage of its own.

## Engine implementation spec

Everything is per player, in sim order. The engine uses float32 IEEE with no contraction (`detmath-scope.md`). Do each operation in float32 in the order below. Retail evaluated some chains in 80-bit x87, so results can differ from retail by about 1 ulp, but they are identical across peers.

Constants:

```
TICKS_PER_SEC = 30
SETTLE_PERIOD = 30 ticks              (nextSettle = tick at player init; settle when nextSettle <= tick, then += 30)
AI_MULT = {EASY 0.5f, MEDIUM 0.7f, HARD 1.0f}   applied only to production credits of AI players
WIND_DIV = 5000; WIND_DEFAULT_MIN = 100, WIND_DEFAULT_MAX = 2000; TIDAL_DEFAULT = 0.5f
BASE_STORAGE_MIN = 200
SHARE_PERIOD = 60 ticks; SHARE_FRAC_M = 1/3, SHARE_FRAC_E = 1/2
```

Accumulator per unit, plus one per player. For each resource r: `prod, req, newReq, debt, lastProd, lastReq` (float).

- `request(r, x)` (per tick, for builders etc.): `req += x`; `if debt <= 0 { newReq += x; return true }`, else return false.
- `requestBoth(e, m)`: add both to `req`; `if debtE <= 0 && debtM <= 0 { newReqE += e; newReqM += m; return true }`, else return false.
- `payNow(r, x)` (cloak, shots, gifts): `if stored >= x { stored -= x; req += x; return true }`, else return false.

Per tick, in this order: units and orders (build, repair, reclaim requests), then the player settle (when due), then wind, then auto-share (every 60 ticks).

Settle:
1. `capE = capM = 0`.
2. For each unit slot in index order:
   - **Structure** (BMCode 0) and activated:
     - `eu >= 0`: `req += eu`, then `ok = (debt <= 0)`, and if ok `newReq += eu`.
     - Else: `prodE += mult·(−eu)` and `ok = false`.
     - Then the first true branch of:
       - `ExtractsMetal > 0 → if ok prodM += mult·extractRate`
       - `MakesMetal → if ok prodM += mult·(float)MakesMetal`
       - `Wind > 0 → prodE += mult·(windF·Wind)`
       - `Tidal > 0 → prodE += mult·(tidal·Tidal)`
   - **Mobile**, activated or moving: the `eu` handling only.
   - Fully built: `prodE += mult·EnergyMake`, `prodM += mult·MetalMake`, `capE += EStorage`, `capM += MStorage`.
   - Cloak (if wanted, allowed, and `noCloakUntil <= tick`): `cost = (float)(int)(moving ? ccm : cc)`; `payNow(E, cost)` decides cloak on or off.
   - Add all eight fields to the sums.
3. Add the player accumulator. If base storage is set: `capE += baseE`, `capM += baseM`.
4. HUD/stats: income = Σprod, expense = Σreq.
5. For r in (E, M):
   ```
   avail = stored + Σprod
   fD = ΣDebt <= avail ? 1 : avail/ΣDebt;  avail -= min(ΣDebt, avail)
   fN = ΣNew  <= avail ? 1 : avail/ΣNew;   avail -= min(ΣNew, avail)
   if avail > cap { wasted += avail − cap; avail = cap }
   stored = avail
   ```
6. For every live unit and the player accumulator: `lastProd = prod; lastReq = req; debt = newReq·(1−fN) + debt·(1−fD); prod = req = newReq = 0`.

Other rules:
- **Build step** (per tick): `amt = floor(WT/30)` (int, as float). Skip if `rem == 0`.
  ```
  c = clamp(rem − amt/BT, 0, 1);  d = rem − c
  if requestBoth(BCE·d, BCM·d) {
      hp = min(hp + (int)(rem·HP) − (int)(c·HP), HP)
      rem = c
      if rem == 0 complete()
  }
  ```
  Set `noCloakUntil = tick + 150` while building or repairing.
- **Repair** (per tick): `if request(E, 1) { hp = min(hp + 1, HP) }`.
- **Weapons**: fire only if `storedE >= eps && storedM >= mps`; on fire, `payNow` both.
- **Extractor**: at creation, `extractRate = ExtractsMetal · Σ_{footprint}(cellMetal + 1)`.
  - `cellMetal` = SurfaceMetal, then for each indestructible feature with metal ≠ 0, `(u8)(int)metal` over its footprint, applied in cell order (later features overwrite).
- **Wind** (every tick):
  - `if nextChange < tick`: `nextChange += (5 + lcg()·10/32768)·30`, where `lcg` is the MSVC rand LCG (`s = s·214013 + 2531011; (s>>16)&0x7fff`).
    - Retail's LCG is unsynced. For lockstep, use a **separate seeded instance** (e.g. seeded from the game seed) so the synced RNG stream matches retail.
  - `speed = min + rng(max − min)` (retail RNG; no draw if the range is < 2).
  - `if speed ≠ 0: dir = rng(65536)`.
  - `windF = min(speed/5000f, 1)`.
  - Initialise at map load with `nextChange = 0` and one update.
- **Reclaim**:
  - Feature: timer `(int)(15 + 0.5·(M+E))` ticks (VTOL 30+). At the end, `prodE += mult·E` and `prodM += mult·M` on the reclaimer.
  - Unit: type-5 damage every 15 ticks as in §7. On the kill, `prodM += mult·(1 − rem)·BCM` to the killer.
- **Start**: `stored = startAmount`, `base = max(startAmount, 200)`. Optionally emulate the first-settle double credit, which only adds waste stats.
- **Share**:
  - Manual: `payNow(r, min(x, stored))` on the giver; the receiver's player accumulator `prod += mult·x`.
  - Auto, every 60 ticks: give to the last eligible ally with less stored, `min((stored − thr)·k, theirCap − theirStored)` with k = 1/3 for metal and 1/2 for energy. In lockstep, run it for every player, not only the local one.

Unconfirmed or open: the registry default for gamespeed; the exact moving-flag bits (0x110 & 0xC); mode 2 start resources; whether resurrect costs anything; how the per-cell metal is shown under the cursor (not traced); `info+0x94` / `info+0x97` bit meanings for auto-share.
