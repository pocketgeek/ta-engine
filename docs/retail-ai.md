# Retail Total Annihilation computer-player AI (TotalA.exe 3.1c)

This document records how the shipped AI behaves. The findings come from static analysis
of `TotalA.exe` (GOG Commander Pack, base 0x400000) and from running single routines
under unicorn. No binary code or data was copied. All addresses are virtual addresses.
Tags:

- **[C]**: confirmed. Either an emulation matched it, or the code is short and has
  no branches that could hide another reading.
- **[S]**: read statically and believed correct, but not run.
- **[U]**: unconfirmed or inferred. The note says what is unknown.

Scripts, all under `tools/re/`:

- `ai_emu.py`: harness.
- `ai_emu_tables.py`: tests the type tables built by 0x409730.
- `ai_emu_score.py`: tests the build score at 0x40bb00 against a reference formula
  (404 random cases, 0 mismatches).
- `ai_emu_rng.py`: tests the RNG at 0x4b6c30 (3000 cases, 0 mismatches).
- `ai_emu_trig.py`: tests the trig helpers.
- `ai_profile_ref.py`: reference profile evaluator that produces golden weights and limits.
- `ai_dis.py`, `ai_dump.py`, `ai_calls.py`, `ai_keys2.py`: disassembly helpers.

The retail AI is much simpler than the TA:Kingdoms AI. It has no threat map, no attack
waves planned against a timeline, and no base-layout planner. It consists of:

- a per-type build score from three economy needs, multiplied by profile weights;
- a reservoir-weighted random choice among buildable types;
- random lattice placement around a weighted "base centre";
- nine fixed squads, each run by a small timed task.

## 0. Conventions and structures

**Time.**
- The game tick is `G+0x38a47` (G = `[0x511de8]`). There are 30 ticks per second.
- The clock formatter at 0x40c250 divides by 108000 per hour. [C]

**RNG.** `Random(n)` at 0x4b6c30 works on the global seed `[0x51fc88]`. [C]
- Step: `seed = 16807*seed mod (2^31-1)`. If the result is 0, it becomes 0x7fffffff.
- It returns `seed % n`.
- If `n < 2` it returns 0 and does not advance the seed.
- The seed is set by 0x4b6ca0: `(x ^ 0x66e29572) | 1`, already in `sim/retailrng.h`.
- The AI shares this stream with the rest of the simulation. Retail TA is not lockstep.
  Each machine owns its own units, and AI players run on the hosting machine (string at
  0x507338).

**Players.**
- Player record: `G+0x1b63 + i*0x14b`, for i in 0..9.
  - `+0x73` type: 1 = HUMAN, 2 = AI, 3 = other or remote. Debug dump at 0x40c331. [C]
  - `+0x74` controller pointer. `+0x78` squads array.
  - `+0x146` player index. `+0x108[j]` alliance bytes.
  - `+0x144` (word) number of units owned. `+0x67`/`+0x6b` first/last unit (unit stride 0x118).
- Economy fields, set in 0x401360 and 0x401994–0x401a3d: [C]
  - `+0x8c` energy stored, `+0x90` energy produced, `+0x94` energy consumed.
  - `+0x98` metal stored, `+0x9c` metal produced, `+0xa0` metal consumed.
  - `+0xa4` energy capacity (sum of `energystorage`), `+0xa8` metal capacity.

**Unit (0x118 bytes).** [S]
- `+0x6a/+0x6e/+0x72`: x/y/z in 16.16 px. The words at `+0x6c` and `+0x74` are the integer x and z.
- `+0x92` unit type. `+0x96` owner. `+0xa6` type id. `+0xac` squad (−1 none, 0 new).
- `+0x104` build fraction remaining (0 = complete).
- `+0x5c` current order (`+0x42` = order flags). `+0x10e` bit0 = activated.
- `+0x110` flags (from 0x485a40):
  - 0x10000000 exists.
  - 0x20000000 static (bmcode == 0).
  - 0x80000000 armed: the unit type has a weapon (type bit 0x10000, set at 0x42cf0c).
  - 0x40000/0x80000 move-state field (bits 18-19); 0x100000/0x200000 fire-state field
    (bits 20-21; retail-weapon-scripts.md §8, InitialMission `o` 0x487c9d) (copied from
    standingfire/moveorder at 0x485c90). [S]

**Unit type (0x249 bytes).** FBI key to offset, from the loader at 0x42bf40: [C]

| Offset | Field |
|---|---|
| `+0xa0` | side |
| `+0xbe` | ai_weight (string) |
| `+0xfe` | ai_limit (string) |
| `+0x14a/+0x14c` | footprint x/z |
| `+0x152/+0x156` | canbuild count / list |
| `+0x186` | buildcostenergy |
| `+0x18a` | buildcostmetal |
| `+0x1be` | maxwaterdepth |
| `+0x1c0` | minwaterdepth (from movementclass, default −10000) |
| `+0x1c2` | energymake |
| `+0x1c6` | energyuse |
| `+0x1ca` | metalmake |
| `+0x1ce` | extractsmetal |
| `+0x1d2` | windgenerator |
| `+0x1d6` | tidalgenerator |
| `+0x1e2/+0x1e6` | energy/metal storage |
| `+0x202` | sight |
| `+0x204` | radardistance |
| `+0x206` | sonardistance |
| `+0x22d` | makesmetal (int) |
| `+0x22f` | bmcode |
| `+0x1ee/+0x1f2/+0x1f6` | weapon pointers (an empty slot points at weapon record 0) |
| `+0x231..+0x23d` | bad-target / noChase category masks |

`+0x241` bits:

| Bit | Meaning |
|---|---|
| 0x20 | downloadable |
| 0x40 | builder |
| 0x200 | isairbase |
| 0x400 | istargetingupgrade |
| 0x800 | canfly |
| 0x8000 | shootme |
| 0x10000 | armed |
| 0x1000000 | isfeature |

`+0x245` bits:

| Bit | Meaning |
|---|---|
| 0x10 | canattack |
| 0x100 | canload |
| 0x1000 | cancapture (only commanders set it; the AI uses it as its commander test) |

**Weapon record.** 256 records of 0x115 bytes at `G+0x2cf3`. [C]
- `+0x10a` table id. An empty slot has id 0 (0x42e322).
- `+0xd4` `DAMAGE.default`. `+0xdc` range. `+0x111` flags (bit26 commandfire).

**Map.**
- `G+0x14223/0x14227`: map width/height in px. `G+0x14233/0x14237`: map size in 16-px cells.
- `G+0x1422b` = W−32, `G+0x1422f` = H−128 (0x4833b0).
- `G+0x1425b/0x1425f`: min/max wind. `G+0x37ec8` = 5000 (wind normaliser).
- `G+0x37ee6` / `G+0x1434f`: maxunits.
- `G+0x37eee`: difficulty (0 Easy, 1 Medium, 2 Hard). This is a single global, the
  registry value "Difficulty" (0x4311d1).
- Map-info object `[G+0x391e9]`:
  - `+0` game mode: 1 = campaign, 2 = skirmish (lobby table at `G+0x29a0`), 3 = network.
  - `+0x804` AI profile path.
  - `+0xd30` SurfaceMetal.
  - `+0xd5c+4i` / `+0xd84+4i`: start metal/energy per player in campaign
    (HumanMetal/Energy is i = 0, ComputerMetal/Energy is i = 1).

## 1. Profile selection, grammar and storage

### Selection [C]

- The OTA loader (0x436860) picks one `[GlobalHeader]\[Schema N]` block:
  - Campaign (mode 1): by `type=` in the order Easy,Medium,Hard (difficulty 0),
    Medium,Easy,Hard (1) or Hard,Medium,Easy (2). Jump table at 0x436c1c, cases 0x4368e3–0x436934.
  - Mode 2/3: the `Network N` schema whose `StartPos` special count best matches the
    player count.
- That schema's `aiprofile=` value becomes `ai\<value>.txt`, stored at map-info `+0x804`
  (0x4366d5 → 0x435430(7,…)).
- If the value is empty, `ai\default.txt` is used (0x43671a).
- If the file fails to load (for example `aiprofile=AIR`), 0x4648e0 falls back to
  `ai\default.txt` (0x464911).
- The same schema supplies ComputerMetal/Energy start resources (0x436677, used at
  0x465f22) and SurfaceMetal.
- There is one profile for all AI players. It is parsed once per game by 0x4648e0, called
  from 0x464990 after every player's AI objects exist. The console command
  `ReloadAIProfiles` (0x40a100) resets the tables and parses again.
- `aiprofile` values found in the 275 OTAs: DEFAULT 333, MISSIONS 121, SeaBattle 43,
  Hover 38, AirBattle 31, Waterwrld 24, Acid 10, empty 27, Metal 3, Urban 3, Krogoth 1,
  AIR 1 (no such file, so it falls back).

### Grammar

The file goes through the generic console/script executor 0x4b7a30. [C]
- Lines are split on LF and tokenised on whitespace, up to 20 tokens of 126 chars (0x4b7440).
- `#` starts a comment.
- The command is matched case-insensitively against the global command table.
- `plan`, `weight` and `limit` are registered at 0x406f00. Any other first token,
  including `//`, goes to the table's fallback handler and has no effect on the AI.

Commands:

- `plan <w1> [w2…]` (0x406c90): sets the global flag `[0x501774]` to 1 if any argument
  case-insensitively equals `any` or the current difficulty word, otherwise 0.
  `weight` and `limit` do nothing while the flag is 0. The flag's initial value is 1.
- `weight <name> <float>` (0x406db0 → 0x409dc0): for every player slot with a
  controller, and every type in the set:
  `if !wlock[t]: w[t] = clamp(trunc(w[t]*f), 0, 100); if name is a unit: wlock[t]=1`.
  The weight is a byte and can never go above 100. Factors greater than 1 only undo
  earlier reductions.
- `limit <name> <int>` (0x406e40 → 0x409e90): for AI players only (type 2):
  `if !llock[t]: lim[t]=v; if unit: llock[t]=1`.
- `<name>` resolution (0x488d30):
  - First a binary search for a unit name (case-insensitive). On a hit the set is that
    one type and it locks.
  - Otherwise it is a FBI `Category=` token (0x488c50, case-insensitive). The set is the
    category's types and it does not lock. An unknown category is an empty set.
- Locks therefore mean: the first unit-specific line wins over everything after it.
  Category lines compound multiplicatively (weight) or overwrite (limit) on unlocked types.

### Storage

AIPlayer record, `[0x5119c0 + 4*p]`, 0x10d bytes, constructor 0x409160, reset 0x409470.
Per type id t, for 1 ≤ t < `G+0x1438f`:

| Field | Meaning | Initial value |
|---|---|---|
| `+0xb1` byte | weight | 100 |
| `+0xc1` int | weight lock | 0 |
| `+0xd1` int | limit (−1 = none) | −1 |
| `+0xe1` int | limit lock | 0 |
| `+0x81` word | completed count | — |
| `+0x91` sbyte | "value" | — |
| `+0x69` 3×sbyte | (base, baseML, baseEL) | — |
| `+0xa1` sbyte | base-centre weight: 40 if bmcode==0, +20 if it has a canbuild list | — |

After the profile, for each AI player:
- 0x409f80 runs each unit's FBI `ai_weight=` string as a script line, with the plan
  flag forced on, if that type is not weight-locked. Seven FBIs have one, for example
  `weight ARMMAV 0.5`.
- 0x40a040 was meant to do the same with `ai_limit`. It parses `+0xbe` again (the
  `ai_weight` string) and checks the limit lock instead. **FBI `ai_limit=` is never
  applied (retail bug).** [C]

The debug dump at 0x40c250 prints `limit - base:baseML:baseEL : weight - name` for each type.

## 2. Difficulty, skirmish and campaign

All difficulty effects found:

1. **Which `plan` section of the profile applies** (above). [C] Shipped profiles change
   weights and limits per section. Examples:
   - default.txt: PLANT 2/3/4 and CONSTR 3/4/4 across Easy/Medium/Hard.
   - missions.txt Easy: `limit constr 1`, `weight level2 0.25`, and big guns (corint,
     armbrtha, …) disabled.
2. **The campaign schema** (Easy/Medium/Hard OTA blocks with their own aiprofile,
   start resources and units). [C]
3. **AI income multiplier.** Every resource credit to a unit owned by a type-2 player is
   scaled: **Easy ×0.5, Medium ×0.7, Hard ×1.0**. [C, constants 0x4fc480/488 and copies]
   - Production loop 0x401360 (energy generation 0x401441; extract/maker branches
     0x4014c2–0x40175e).
   - Reclaim/refund 0x4026ab, 0x41bbbd, 0x4238c9, 0x486ce6.
   - 0x465443/0x4654af (unit-spawn path; the exact quantity is not identified [U]).
   - Resource transfer 0x464c0c (direction not verified [U]).
4. No difficulty-dependent timer, unit cap, aggression or cheat vision was found. The
   AI attack code does cheat by using nearest-enemy without any visibility test (§3.5).

**Campaign versus skirmish.**
- Campaign (mode 1): the AI never builds `downloadable=1` units (0x40bb7b). Start
  resources come from the schema.
- Skirmish/MP: resources come from the lobby table.
- Otherwise the code is identical.
- `missions.txt` is an ordinary profile (§4).

## 3. Decision loop

### 3.1 Objects and cadence

Per-tick player loop 0x464f80. For p = 0..9, if active, of type 1/2/3, and `+0x146 != 10`:

**a) Controller tick (0x408c40)**, only if type == 2. The controller object is 0x3d
bytes, built by 0x408cb0.
- Countdown `+5` (starts at 30). When it reaches 0 it is reset to 30 and
  **squad assignment** 0x408830 runs.
- Then task slots k = 1..9 (`+0x11+4k`) run in index order when `now >= task.next`.
  `task.next` starts at 0, so every task runs on the first tick.
- Then 0x4089a0(1), weapon auto-target upkeep. Other player types call 0x4089a0(0).

**b) Shared cache tick 0x40b2c0(p)**, for every player that has an AIPlayer record,
humans included. If `now >= last+30`:
- 0x40aa40 refreshes the cache.
- `last = now`.
- `if Random(30)==0` → 0x409730 rebuilds the type tables. The tables are also built
  once when the record is created (0x40b347).

**Squads.** Each player has 10 squads (`player+0x78`, 0x20 bytes each: vector of unit
pointers at +0x14/+0x18; 0x480190). The unit field `+0xac` holds the squad, and moves
go through 0x480250. New units get squad 0 (0x485d29). Dead units get −1.

Tasks (the constructor parameters are at 0x408cb0):

| Squad | Contents (0x408830) | Task | Period (ticks) |
|---|---|---|---|
| 1 | static unarmed (economy, factories) | A 0x4086d0 economy/factories | 30 |
| 2 | land strike group | B 0x4077e0 (min 3, launch 6, R=20000, source 3) | 300 |
| 3 | mobile armed (land) | C 0x4079f0 follow squad 2 | 150 |
| 4 | mobile builders (incl. commander) | D 0x408100 builders | 90 |
| 5 | static armed (defences) | E 0x407380 (no-op) | — |
| 6 | naval strike group | F 0x4077e0 (3, 6, 50000, source 7) | 300 |
| 7 | naval (minwaterdepth > 0) | G 0x4079f0 follow squad 6 | 150 |
| 8 | aircraft (canfly) | H 0x407ae0 air patrol/raid | 30+Random(900) |
| 9 | (nothing ever assigned) | I 0x407e90 hunter | 30+Random(150) |

**Squad assignment** (0x408830). For each own unit with flag 0x20:
- Set the fire state: commander (cancapture) 0x40000, otherwise 0x80000.
- Set the move state: 0x200000.
- Then, only if the squad is 0:
  - static: armed → 5, else 1;
  - else builder → 4;
  - else canfly → 8;
  - else minwaterdepth > 0 → 7;
  - else armed → 3;
  - else it stays in squad 0 (for example unarmed scouts and transports).
- [S] The flag meanings come from 0x485c90.

**Squad 9.** Task I is never fed by AI code, so it is inert. The only writers of squad 9
are human group keys (0x48d920) and savegame load. [S]

### 3.2 Shared cache (0x40aa40, once per 30 ticks)

The record clears its lists:
- `+5`: known enemies.
- `+0x15`: enemies with unit flag 0x100.
- `+0x25`: own active air pads.

It also clears the counts, `+0x75` (number of own completed units with a canbuild list)
and `+0x79` (has an active targeting facility). Then it scans every unit:
- **Enemy**, meaning the alliance byte is 0. Pushed to `+5` if visible to p (0x465ac0)
  and `!(flags&0x8000)`. Pushed to `+0x15` if flags&0x100.
- **Own**, complete (`+0x104==0`):
  - `count[t]++`.
  - `+0x75++` if it has a canbuild list.
  - Air pad (builder, isairbase, active) → `+0x25`.
  - Active `istargetingupgrade` → `+0x79=1`.
  - Weighted sums: `w=+0xa1[t]` (40/60/20/0), `Σw`, `Σw·x`, `Σw·z`.
- **Base centre** `+0x35` = `Σw·pos/Σw` in 16.16 px, or (0,0,0) if Σw == 0.
  The constants are 1/65536 and 65536.0. [C]
- Allied non-own units are ignored.

### 3.3 Type tables (0x409730) [C by `ai_emu_tables.py`]

For each type t, with c = count[t] (completed):

```
eNet(t) = energyuse≠0 ? energyuse : wind>0 ? -wind*G[0x37ede] : tidal>0 ? -tidal*G[0x14267] : 0   (0x488f30)
value[t] (sbyte):
  s = (extractsmetal≠0 ? 11 : 1) + (makesmetal ? 10 : 0) + (eNet<0 ? 10 : 0)
  s = trunc(s + 0.01*buildcostmetal); s = trunc(s + 0.002*buildcostenergy)
  w = (canattack ? 11 : 1) + Σ_{weapon slots with id≠0} (range/100 + damage/40 + 5)   (int division)
  value = clamp(s + clamp(w,-100,100), -100, 100)
base (t[0]):
  g = (canattack?21:1) + (builder && c<3 ? 30:0) + (eNet<0?50:0) + (extractsmetal≠0?50:0)
      + (makesmetal?25:0) + (canfly?40:0) + (sonardistance≠0?15:0) + (radardistance≠0?5:0)
  v = trunc(g + clamp(energymake,0,30))
  if c==0 v*=4; if c==1 v*=2; if minwaterdepth>=0 v*=3
  if player.unitCount > maxunits/2: v += value[t]/2          (C division, toward 0)
  if canload (transport) or isfeature: v = 0
  if windgenerator≠0 and G[0x1425f] (max wind) < G[0x37ec8]/2 (=2500): v = 0
  base = min(v, 100)
baseML (t[1]) = trunc(clamp((extractsmetal≠0?100:0) + (makesmetal?25:0) - 0.02*bcMetal, 0, 100))
baseEL (t[2]) = trunc(clamp(-0.0025*bcEnergy - 5*eNet, 0, 100))
```

### 3.4 Economy

**Build score** `Score(p,t)` (0x40bb00). [C, 404 random cases emulated]

```
if eStored<50 or mStored<25: return 0
if mode==campaign and t.downloadable: return 0
E = trunc(max(0,(min(eCap,1000)-eStored)*0.125)); M = trunc(max(0,(min(mCap,500)-mStored)*0.25))
if eProd-eUse < 1: E += 20          if mProd-mUse < 1: M += 20
if eProd<50: E += 100 elif eProd<200: E += 10
if mProd<3:  M += 100 elif mProd<5:  M += 20
M = clamp(M,0,100); E = clamp(E-M,0,100); G = max(0,100-M-E)
if !(limit[t]==-1 or count[t]<limit[t]): return 0
return trunc((G*base + M*baseML + E*baseEL) * weight[t] / 10000)
```

- `cap`, `stored`, `prod` and `use` are the player float fields listed in §0.
- `count` counts completed units only, so units still under construction do not count
  against limits.

**What to build** (0x40bdb0, for a given builder or factory u). Weighted reservoir:

```
choice=0; total=0
for t in u.canbuild (list order):
  s=Score(p,t); if s>0: total+=s; if Random(total) < s: choice=t
if choice and strcmp(side(choice), side(u)) != 0: choice = 0   (no cross-side builds)
```

**Factories** (task A, every 30 ticks, squad 1). Each static, existing, non-0x4000 unit:
- **Metal makers** (`makesmetal`):
  - If `mStored*2 < eStored`, and `eProd-eUse > 0`, and `Random(5)!=0` → turn on
    (0x48b090(1,1)).
  - Else if `mStored*2 >= eStored` → turn off (0x48b090(1,0)).
  - Otherwise there is no change.
- **Factories** (canbuild list, no current order `+0x5c==0`): `t=choose(u)`; if
  non-zero, queue one t (0x419b00(name, u, 1)).

**Builders** (task D, every 90 ticks, squad 4). Base = `+0x35`.

Pass 1, for each u with a canbuild list:
- Commander: skip if `+0x75 >= 5` or `now < ctrl+0xd`.
  - `ctrl+0xd` is set to `now+30+Random(300)` whenever the AI commander is hit.
  - The same hit handler also clears its interruptible orders (0x406fc9).
- Skip if the current order has flag 0x08.
- `t = choose(u)`. Then `site = FindSite(p, u.pos, t)`.
- Commander: reject the site if `|base-site| > ((W-32)+(H-128))/3` px.
- Issue action 14 (MOBILEBUILD) of type t at the site, replacing current orders
  (0x43f0e0 / 0x43adc0).

Pass 2, for each u whose order is null or has flag 0x4000:
- Commander, only if `+0x75 >= 5`:
  - If `|u-base| > 640px`: P = base + 640px at `Random(65536)`.
  - Else P = 2·base − u (the commander's own position reflected through the base centre).
  - Move (action 2) to P, then patrol (action 9, queued) to base.
- Other builders, with d = |base−u|:
  - If d ≥ 320px: P = base.
  - Else if d < 16px: P = u + 320px at a random angle.
  - Else P = u + 320px along (base−u).
  - Patrol (action 9, replace) to P. In retail, builders on patrol repair, reclaim and assist.

`Random(65536)` angles: `x -= r·sin(a)`, `z -= r·cos(a)` (0x4b70ef/0x4b7123, 65536 = 2π). [C]

**Site finding** (0x40bfe0):
- `R = ai+0x109`. R starts at 0. Each call adds 160 while `R < max(W,H)`, and R never shrinks.
- Centre C = base if `|base−pos| ≤ R`, else the point R px from pos toward the base.
- **Extractor** (`extractsmetal≠0`) and `Random(255) > SurfaceMetal` → **metal search**
  (0x40a260):
  - Candidates come from `ai+0x4d`, built once per game (0x40a7b0) from every map cell
    holding an **indestructible feature with metal > 0** (the decorative ore features).
    Each entry is (cell x, cell z, metal).
  - Keep those with (cell distance)² ≤ (4R)². Sort by distance.
  - Walk them nearest first. Corner = cell − (footprint−3)/2. Stop once d² > d²(first
    buildable candidate) + 160.
  - Take the buildable one (0x47d2e0) with the most map-metal under its footprint
    (`[0x51e688]`, sum of the cells' metal bytes).
  - If there are no candidates, it fails. There is no fallback to generic placement.
- **Otherwise, generic placement** (0x40a5d0), up to 30 tries:
  - r = Random(R) px, a = Random(65536). Point = C − (r·sin a, r·cos a).
  - Corner cell c = (point − footprint·8px + 8px) >> 4 px.
  - Lattice snap: `gx = (cx/sx)*sx + ox + Random(sx − E − fx)`,
    `gz = (cz/sz)*sz + oz + Random(sz − E − fz)`.
  - Accept if placement is OK (0x47db70) and the metal under the footprint is
    `≤ 2·SurfaceMetal·fx·fz` (keeps ore spots free).
  - The lattice (0x40a150, at construction) uses the uninitialised fields E=`+0xf9` (land)
    and E'=`+0x105` (water). [U] Treat E as 0.
    - Land: `sx = Random(10)+E+8`, `sz = Random(3)+E+8`, `ox = Random(sx)−sx/2`, `oz = Random(sz)−sz/2`.
    - Water (minwaterdepth ≥ 0): `sx = Random(20)+E'+8`, `sz = Random(3)+E'+8`, offsets likewise.
- On success: site = `((2g + footprint) << 19)` per axis, the footprint centre in 16.16 px.

### 3.5 Military

**Strike groups** (task B/F, every 300 ticks). Parameters (min=3, launch=6, R, src):

1. **Gather** (0x407560):
   - If the squad is empty, move the first unit of src into it.
   - While size > 1 and the farthest member has d² ≥ R·size from the centroid (x/z integer
     px), send it back to src and recompute.
   - Then every src unit with d² < R·size (size taken before the transfer) joins.
   - For R = 20000: about 141·√n px.
2. If the squad is empty, stop.
3. If `size ≤ 3`, or (`state==0` and `size < 6`): **rally**.
   - Rally point = centroid of squad 5 (defences), else squad 1, else squad 4.
   - If one exists: `state=0`, move (action 2, parameter 160 [U meaning]) the whole squad
     there, stop.
   - If none exists, fall through to attack.
4. **Attack**:
   - `state=1`.
   - Target = nearest enemy unit to the squad centroid (0x4071f0). The scan covers all
     non-allied players of type 1/2/3, units that exist and are not 0x8000, not
     `(flags&3)==2`, not `+0x10e&4`.
   - **No visibility check** and no range limit.
   - Order action 3 (attack) on the target for the whole squad.
   - Attacking continues while size > 3 and retargets every 10 s.

**Reserves** (task C/G, every 150 ticks): if both squads are non-empty, move squad 3
(or 7) to the centroid of squad 2 (or 6).

**Air** (task H):
- If size < 5:
  - If the base centre is (0,0): queued patrol (action 9) to the position of the nearest
    enemy to the squad centroid.
  - Else n = 2+Random(2) waypoints, each base + ((Random(W/8)−W/16), (Random(H/8)−H/16))
    px. The first is a move, the rest are queued patrols.
- If size ≥ 5: patrol (replace) to a random map-edge point:
  - `Random(2)` → x = Random(W), and z = 0 if Random(2) else H−1;
  - otherwise x = 0 if Random(2) else W−1, and z = Random(H).

**Hunter** (task I, inert in practice). It probes 320px steps from the current target
and keeps a visible probe point p when `Random(Σ enemy value within 160px of p) >
Random(best)`. With 1/10 chance per run it re-seeds a random direction. Armed members
then get action 3 toward the target.

**Defence, retreat and targeting**:
- There is no retreat logic apart from the commander's hit hold and its patrol.
- Static defences (squad 5) are only a rally anchor.
- Weapon target selection 0x40b7b0 runs for all players:
  - Up to 50 random draws from the known-enemy list within range.
  - Keeps the minimum `Random(d²)`, preferring non-bad-category targets.
  - AI owners (type 2) may auto-target non-`shootme` units (structures).
  - AI owners auto-fire `commandfire` weapons (the 0x4089a0 arg=1 path skips the
    `+0x111` bit26 filter).

**Categories, TEDClass, threat**:
- Categories are used only for profile weight/limit sets and weapon target masks.
- `TEDClass` and `ThreatLevel` are not read by the AI. [S: no FBI key strings for them]
- "Value" (§3.3) is the only threat-like quantity. It is used for the near-cap base
  bonus and for the hunter.

## 4. Per-map and mission behaviour

- `missions.txt` is a normal profile used by the campaign maps (121 OTAs). Per plan it
  sets:
  - `limit constr` 1/2/8 (Easy/Medium/Hard);
  - `weight level2` 0.25/0.5/2;
  - limit 0 on super-weapons (Easy/Medium: corint, armbrtha, armanni, cordoom, cortron,
    armemp; Hard: cortron, armemp).
  It contains no scripting.
- OTA unit fields `AiIgnore` (bit 0x20) and `AiPriorityTarget` (bit 0x40) of the record
  byte `+0x23` are parsed (0x436f7e/0x436f9c). No reader exists: a linear sweep of
  .text finds only bit 0x80 read at 0x488475. **Dead in 3.1c.** [S]
- The only other per-map influences:
  - SurfaceMetal (the extractor search mode and generic-site metal cap).
  - Wind (windmills disabled when max wind < 2500).
  - The schema's start resources.
  - Map ore features (extractor candidates).

## Engine implementation spec

**Data per AI player p:**
- `weight[t]` u8=100, `wlock[t]`, `limit[t]`=−1, `llock[t]`;
- `count[t]` (completed); `value[t]`, `base[t]`, `ml[t]`, `el[t]` (s8);
- `baseW[t]` = (static?40:0)+(canbuild?20:0);
- `baseCenter` (16.16), `R` = 0, `nBuildCapable`;
- `oreSpots` (once per game);
- lattice `{sx,sz,ox,oz}` for land and for water;
- `cmdHoldUntil` = 0;
- 10 squads;
- tasks[1..9] each with `next` = 0;
- B/F `state` = 0;
- `squadAssignCountdown` = 30;
- `lastCache` = 0.

**Load:**
- Choose the profile (§1). Reset tables. Apply the profile lines with the plan flag
  (initially on).
- Then apply each unit type's FBI `ai_weight` line with the flag on.
- Ignore `ai_limit`, as retail does. Optionally fix it behind a flag.
- Build the lattice in this RNG order: `Random(10)`, `Random(3)`, `Random(sx)`,
  `Random(sz)`, `Random(20)`, `Random(3)`, `Random(sx_w)`, `Random(sz_w)`. Then build
  the type tables, then `oreSpots`.

**Every tick, players in index order:**

1. If AI:
   1. `if --countdown<=0 {countdown=30; AssignSquads}`.
   2. For k = 1..9: `if now>=task[k].next: run task k`. Each task first sets its own
      `next`: A +30, B/F +300, C/G +150, D +90, H +30+Random(900), I +30+Random(150).
2. If `now >= lastCache+30`:
   1. `Cache()`.
   2. `lastCache=now`.
   3. `if Random(30)==0: Tables()`.

**Task bodies:**
- Formulas, constants and RNG call order exactly as in §3.3–3.5.
- Distances: squared integer px for squad gathering. Euclidean 16.16 for
  site/commander/builder radii.
- Thresholds:
  - commander 640px; builder 320px/16px; rally move parameter 160;
  - B: 3/6, R=20000; F: 3/6, R=50000;
  - air waypoints within ±W/16 and ±H/16;
  - metal-search window +160 cells²;
  - 30 placement tries; R step 160;
  - commander site limit ((W−32)+(H−128))/3;
  - near-cap = unitCount > maxunits/2;
  - wind cut-off 2500 (= 5000/2).

**Difficulty:**
- Select the plan section.
- Multiply every resource credit to AI-owned units by 0.5/0.7/1.0.

**RNG in lockstep:**
- Retail draws from the global simulation stream on one host only, so its exact
  interleaving cannot be reproduced and is not needed.
- Give each AI player its own Park–Miller stream: `seed = retailSeed(gameSeed ^ (p+1)·0x9E3779B9)`,
  `Random(n)` = §0 semantics.
- Run all AI code on every peer at fixed tick points in the order above.
- Issue AI orders as ordinary sim commands, so replays and desync checks cover them.

**Deliberate retail quirks to keep (or flag):**
- Weight clamped at 100.
- First unit-specific profile line locks the value.
- Counts exclude units under construction.
- Nearest-enemy targeting ignores line of sight.
- No fallback when the extractor ore search fails.
- Squad 9 unused.
- AI auto-fires commandfire weapons and auto-targets structures.
