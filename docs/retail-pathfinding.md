# TotalA.exe ground pathfinding, and how the engine's Retail mode differs

Scope: Total Annihilation 3.1c (`TotalA.exe`, GOG Commander Pack, base 0x400000).
All addresses below are **TotalA.exe**. Addresses quoted from engine comments
(`icd 0x41….`, `0x4e….`, `0x508…`) are **KINGDOMS.icd** and are only used to name the
engine code being compared. Behaviour was observed only; nothing from the binary is
copied into engine source.

Evidence tags: **[E]** confirmed by running the retail routine under unicorn on synthetic
input, **[S]** read from the disassembly, **[U]** unconfirmed or inferred.

Tools (all observe-only):

| Script | What it does |
|---|---|
| `tools/re/path_probe.py` | disassembly helpers (`dis`, `range`, `sweep`, `calls`, `xref`, `str`) |
| `tools/re/path_model.py` | independent Python reading of the routines below |
| `tools/re/path_emu.py` | runs the retail routines under unicorn and checks them against `path_model.py` |
| `tools/re/path_kingdoms_cmp.cpp` | small driver that runs the engine's Retail kernel (`src/sim/retailtrace.h`, `retailcost.h`) on the same grade planes |
| `tools/re/path_compare.py` | puts TotalA.exe routes and engine routes side by side |

Results (seeded, reproducible):

```
path_emu.py grade 200      grade (0x47de60): 3200 cells, mismatches 0
path_emu.py plane 60       plane (0x440500): 60 maps, mismatches 0
path_emu.py search 400 3   search: 400 cases {reached 124, unreachable 219, partial 41, at-goal 16}, mismatches 0
path_emu.py search 1500 7  search: 1500 cases {reached 455, unreachable 829, partial 153, at-goal 63}, mismatches 0
path_emu.py rect 60        local re-rate (0x47e1f0): mismatches 0 (163 cells differ from the full build, all in the last row/column band)
```

A "search" case runs the real init (0x40e630, which includes the tracer 0x40e160), the
real pop/expand body (0x40df00 → 0x40da70), and the real reconstruction (0x40e050), on
random maps. The maps include blocking features, parked, moving and structure bodies,
unexplored 32px cells, footprints 1–3, three heuristic weights and random headings. In
every case the delivered waypoint list, the notification bits and the goal radius
matched the model exactly.

---

## 1. Data the search runs on

### 1.1 Map cells
- The cell array is 13 bytes per 16-px cell, at `G+0x14287` (G = `[0x511de8]`), with the size at `G+0x14233/0x14237` [S].
  - `+0` u16: index of the unit occupying the cell (0 = none).
  - `+4` u8: height.
  - `+5` u8: max height. `+6` u8: min height. Both are taken over the cell's own sample and its +x, +z and +x+z neighbours, clipped at the map edge (0x483297..0x48332c) [S].
  - `+8` u16: feature index. 0xffff = none. 0xfffe = a continuation cell of a multi-cell feature; bytes `+0xa/+0xb` point back to the feature's origin cell. 0xfffb..0xfffd = blocked [S, E].
- The sea level is the u8 at `G+0x1427f`. Feature definitions are 0x100 bytes each at `G+0x1426f`; flags byte `+0xfe` bit 6 = `blocking` [S, E].

### 1.2 Movement classes (moveinfo.tdf)
- There are 32 class records of 0x20 bytes at 0x512358, parsed by 0x440340 [S]:
  - `+4/+6` footprintx/z (i16), `+8` maxwaterdepth, `+0xa` minwaterdepth (i16).
  - `+0xc` maxslope, `+0xd` badslope, `+0xe` maxwaterslope, `+0xf` badwaterslope (u8).
  - Defaults: depth ±10000, slopes 255, bad = max/2. Clamps: maxslope ≤ maxwaterslope, badslope ≤ maxslope, badwaterslope ≤ maxwaterslope (0x4403fa..0x440415).
- **Each class carries its own cached grade plane** [S]:
  - `+0x10/+0x14` = plane width/height in cells.
  - `+0x18` = 2 bits per cell. Word index `(z>>4)*w + x`, bit offset `(z&15)*2` (setter 0x4404c0, reader 0x40d7b0).
  - `+0x1c` = the class's "parked" timestamp.
- At map load 0x440940 sizes and builds every plane (0x440470 then 0x440500).
- A unit type points at its class through UnitType+0x1b6. The mover copies that pointer to mover+4 (0x43dc3f). **A unit type with no movementclass has a null class and is never scheduled for a search** (0x40ed4a). Such a unit keeps the provisional straight segment from §4.1 [S].

### 1.3 Per-cell terrain/body grade — 0x47de60(class, cell) [E]
The result is 0 = blocked, 1 = passable at a penalty, 3 = clear. It never returns 2.

```
feature blocks (def flag bit 6, index >= feature count, or 0xfffb..0xfffd)  -> 0
unit index != 0:
    unit record has no mover (structures)                                  -> 0
    mover+0x26 (tick of last cell change) < class+0x1c                     -> 0   ("parked")
    otherwise the body is transparent: fall through to the terrain test
lo = cell+6, hi = cell+5, slope = hi - lo (u8)
lo < sea - maxwaterdepth  or  hi > sea - minwaterdepth                     -> 0
if lo < sea:  slope <= badwaterslope -> 3; slope <= maxwaterslope -> 1; else 0
else:         slope <= badslope      -> 3; slope <= maxslope      -> 1; else 0
```

There is no "bad depth" band and no road grade.

### 1.4 Footprint plane — full build 0x440500 [E]
This runs as two separable passes, first along x over raw grades and then along z over
the x results. Each pass takes the minimum over the window `[i, i+foot)`; positions
outside the map count as 0. If that minimum is 3 but the neighbour just before the window
(i−1) or just after it (i+foot) is below 3, the result is written as **1**. The combined
result is:
- the minimum over the footprint (top-left anchored at the cell);
- demoted from 3 to 1 if any cell of the one-cell ring, corners included, is not 3.

### 1.5 Footprint plane — local re-rate 0x440830 → 0x47e1f0 [E]
This runs when a body appears, parks or leaves.
- The rectangle `[x−fx, x+w+1) × [z−fz, z+h+1)` is re-rated cell by cell. Each cell takes the footprint minimum from 0x47dfc0, then the four ring strips are tested in this order:
  1. `(x−1, z−1, fx+1, 1)`
  2. `(x+fx, z−1, 1, fz+1)`
  3. `(x, z+fz, fx+1, 1)`
  4. `(x−1, z, 1, fz+1)`

  If any strip is not 3, the cell becomes 1.
- 0x47dfc0 returns 0 for any rectangle with `x<0`, `z<0`, `x+w ≥ W` or `z+h ≥ H`. **The last row and column are excluded.**
- The full build does not exclude them. TA therefore rates the last row/column band (cells with x+fx ≥ W−1 or z+fz ≥ H−1) differently at map load than after a local refresh. All 724 differences in a 300-map sweep fall in that band.

### 1.6 Body timestamps (who blocks the search) [S, E]
- 0x47cc30 runs whenever a unit's occupied cells change and stamps `mover+0x26 = tick`.
- At every search init, 0x440af0 runs for the requester's class:
  - It sets `class+0x1c = max(tick,30) − 30`.
  - It temporarily stamps the requester with `tick`, so the requester does not block itself.
  - It re-rates the rectangles of bodies whose stamp crossed the new threshold, i.e. bodies with unit flag 0x10000000 and stamp in `[old, new)`.
  - It restores the requester's stamp.
- 0x440be0 runs when the search finishes and re-blocks the requester if needed.
- Net rule: **a unit that changed cells within the last 30 ticks is invisible to the search. Older bodies and all structures are walls.** There is no intermediate grade.

---

## 2. The search

### 2.1 Scheduler — 0x40eb70, called once per sim tick from 0x464f80 [S]
- One pathfinder object exists, at `G+0x14207` (0xc9 bytes, ctor 0x40e9e0), so **one search is active at a time**.
- **Budget.** `+0x48` = **1333**. Each tick, every active player adds `1333 / u16 [G+0x2a3c]` to a persistent credit (`+0xa1+4p`). An active player is one with a player record, type 1/2/3, and index ≠ 10. [U]: `G+0x2a3c` is taken to be the active-player count. Credit is added whether or not the player has requests. The tick's `total` is the sum of the credits, and the loop runs while `total > 0`.
- **Picking work.** If no search is active, the player cursor `+0x78` advances until it reaches a player with credit > 0; the current player keeps its turn while its credit stays positive. That player's unit cursor (`+0x79+4p`) then advances one record, wrapping at the player's last unit (0x40ed0c). The visit costs **1**.
- **Starting a search.** If the unit has a mover with a class, and `nav->wantsPath()` (vtable+0x18 = 0x44f260) returns the navigator, the search is initialized. Initialization costs **+100**, plus **1 per tracer step** because the tracer runs to completion inside init.
- **Running a search.** A running search pops nodes at **1** each, in slices of 100 pops (0x40eeaf). Each slice is charged to the current player and to `total`. In practice about 1333 work units go to path finding per tick.
- **Adaptive heuristic weight** `+0x54` = 1.5 (0x18000) [S]:
  - Every 150 ticks (counter `[0x511a38]`), each player's slot-visit count `[0x511a10+4p]` is divided by maxunits (`G+0x1434f`).
  - Weight `[0x5119e8+4p]`: fewer than 1 full sweep → **9.0**; fewer than 2 → **4.5**; otherwise **1.5**. The branch for fewer than 3 also stores 1.5.
  - The weight is copied into the search at init (`+0x50`) and **stays fixed** for the whole search.
- There is no priority class and no node limit.

### 2.2 Init — 0x40e630 [E]
1. Refresh the class's bodies (0x440af0, §1.6). Load the costs (§2.4). Clear the touched cells (0x40d900).
2. The goal controller (`+0x60`) enumerates its goal cells (vtable+0x18). Each in-map goal cell gets flag 4, and the goal cell nearest the start by squared distance is kept as the tracer's target (`+0x38/+0x3c`).
3. If the controller already accepts the start (vtable+0x14), notify **0x100**, deliver an empty route, and stop.
4. `h0 = (distance(start) × weight) >> 16`. If the start is outside the map, notify 0x200, deliver an empty route, and stop.
5. Run the tracer (§2.3). It returns `r`:
   - `r == 0`: the goal is reachable. Notify **0x100** and search with goal radius 0.
   - `0 < r < h0`: notify **0x200** and run a **partial** search with goal radius `r`, so it stops at the first cell whose weighted distance is ≤ r.
   - `r ≥ h0`: notify **0x200** and deliver an **empty route** (unreachable).
6. Seed A*:
   - The start cell gets flag 1, and its direction = `((heading + 0x1000) >> 13) & 7`.
   - The start node has g = 0, f = h0, run = 100.
   - The fan spread is 4.

### 2.3 Tracer — 0x40e160 [E]
This is a reachability probe that decides whether A* runs. It is not budget-sliced and has
no visit limit; it ends only on success or on a closed loop.
- `best = h0`. If the start grade is below 1, return `best`.
- There is no direct route: a start at distance 0 that the controller does not accept
  (the octile distance undershoots the squared accept test by about a cell) returns 0,
  and A* runs with goal radius 0 to the first distance-0 neighbour. The engine's
  Kingdoms `[start, start]` route here left parked units one cell short for good.
- **Cardinal march** toward the target cell: dx < 0 → dir 2, dx > 0 → dir 6, otherwise z ≤ 0 → dir 0, else dir 4. That is, it closes x first, then z.
  - Each passable cell gets flag 8 and a direction.
  - Entering a goal cell → return 0.
  - `best = min(best, weighted distance)`. If `best == 0` at the top of the loop, return 0.
- **On a wall, twin trace.** Both cursors start at the blocked point, `dA = dB = dir+2`.
  - **A step:** search directions from `dA−2`, rotating +1, until a passable one is found. If all are blocked, return `best`. A closes the loop when, after A has moved at least once, A is back on B's cell with `dA == dB`; return `best`.
  - **B step:** search from `dB+2`, rotating −1. B moves by **minus** the direction vector. If B was on A's cell with `dA == dB`, return `best`.
  - Either cursor leaves the wall when it lands on the L-shaped line from the hit point to the target (x leg first, then z). Marching resumes from there.
  - Every traced cell gets flag 8 and its direction. A goal cell returns 0, and `best` is updated after each step.

### 2.4 A* — init + 0x40ed90..0x40eee5 (same body as 0x40df00) + 0x40da70 [E]
- **Grid.** One node per 16-px cell, at the footprint origin (top-left). There are 8 neighbours. The direction tables are dx = {0,−1,−1,−1,0,1,1,1}, dz = {−1,−1,0,1,1,1,0,−1}.
- **Open set.** A binary min-heap on f, with the same pending-root trick as Kingdoms:
  - The popped root is not removed until the first child opens. That child reuses the root slot and sifts down.
  - A decrease-key that displaces the pending root removes it.
  - Ties do not move: sift-up requires `<`; sift-down picks the right child only when it is strictly smaller.
- **Pop.** If the cell has flag 4, it is the goal: reconstruct the route. Otherwise set the flags byte to 2 (closed), then expand the relative directions `−spread..+spread` around the cell's direction byte. Spread is **4 for the first pop**, so all 8 directions are tried and 180° is evaluated twice; it is **2 after that** (forward, ±45°, ±90°) and never widens.
- **Grade query 0x40d7b0:**
  - Outside the class plane → 0.
  - Then the 32-px explored bitmap `G+0x14273` is sampled at `(x/2 + fx/4, z/2 + fz/4)`. Outside that plane → **0**. Not explored by the searching player (`+0x78`) → **2** (passable, no penalty).
  - Otherwise the cached 2-bit grade.
- **New cell:**
  - Grade < 1 without trace flag 8 → closed (flags |= 3).
  - Otherwise `h = (distance × weight) >> 16`. Flags |= 5 if `h ≤ radius`, else 1.
  - `entry = grade > 1 ? 0 : 30`.
- **Step cost:**

  ```
  g = parent.g + step[dir] + turn[rel] + entry + (rel != 0 && parent.run < 5 ? 75 : 0)
  step  = 16 cardinal, 22 diagonal                          (0x40e66b)
  turn  = {0, 40, 60, 80, 100, 80, 60, 40}  indexed by rel&7 (0x4fca10)
  run   = rel ? 1 : parent.run + 1                           (start node run = 100)
  f     = g + h
  ```

  Costs do not depend on the unit; there is no turn-rate, footprint or road scaling.
- **Open cell:** recompute g with the cell's stored entry. If it improves, update dir, g, f (by delta) and run, then sift up.
- **Termination.** There is **no node limit**. Popping a goal-flagged cell ends the search. An empty heap delivers an empty route (0x40eec9).
- **Heuristic.** The controller distance (§2.6) times the player weight, which is ≥ 1.5. With 16/22 step costs this is an inadmissible, greedy-leaning weighted A*.

### 2.5 Reconstruction — 0x40e050 [E]
Walk back from the end cell along the direction bytes. Each cell where the direction
changes goes into a **64-entry ring**, followed by the start. Up to 64 points are emitted
start-first, so a longer route loses its goal end. Each cell becomes world pixels:
`((x·2 + fx)·8, (z·2 + fz)·8)`, the footprint centre. The points go to the navigator
through 0x44f080.

### 2.6 Goal controllers (vtables at 0x4fd328 / 0x4fd358 / 0x4fd388) [S]
Cells are footprint origins, `round(world/16 − foot/2)` (0x44cf60).

| Type | Accepts (vt+0x14) | Distance (vt+0x1c) | Target point (vt+0x20) |
|---|---|---|---|
| circle (4), 0x44cf60 | `dx²+dz² ≤ (r/16)²`, r in px, truncating division | `max(0, 18·max + 7·min − r)` (0x44d350) | goal cell centre |
| ring (5), 0x44d3b0 | `(in/16)² ≤ d² ≤ (out/16)²` | outside: `oct − out`; inside: `in − oct`; oct = 18·max + 7·min (0x44d840) | ring centre + (in+out)/2 toward the unit (0x44d720) |
| rect (6), 0x44d8a0 | on the perimeter of `[x0−fx, x0+w] × [z0−fz, z0+h]` | outside: 16·max + 6·min to the box; inside: 16·(nearest side) | `((minX+maxX)/2, maxZ)` (0x44dc60) |

A move order uses a circle whose radius = the order radius + 4 (0x403238).

---

## 3. Following the path

### 3.1 Ground navigator (vtable 0x4fd458, 0x65 bytes, ctor 0x44f010) [S]
- **Layout:** `+4` goal controller, `+8` unit, `+0xc` up to **20** waypoints (i16 x,z in px), `+0x5c` count, `+0x60` last request tick, `+0x64` flags (bit 0 route valid, bit 1 wants a path, bit 3 dirty).
- **deliver 0x44f080:**
  - 0 points: if the goal is not satisfied, notify **0x40** (failed) to the order; clear bits 0 and 1.
  - Otherwise keep `min(n, 20)` points. **A 21+ point route is cut to its first 20.**
- **setGoal 0x44f2a0** (order side 0x438930/0x438a00/0x438ad0):
  - Cancel any active search for this navigator (0x40e9c0). Notify the old goal 0x80. Set bit 1.
  - **Keep the existing route** if it has ≥ 3 points and either (a) the new goal accepts its last waypoint, or (b) `2·|last − target| < |unit − target|`.
  - Otherwise, if `(unit+0x5c)+0x42 & 0x800000` is clear [U: what that bit means], install a **provisional 2-point route** `[unit position, target point]`. The unit starts straight at the target while the search is queued.
  - If the last request was ≥ 10 ticks ago, clear `+0x60` so the request gate opens at once.
- **wantsPath 0x44f260:** bit 1 set and `tick ≥ +0x60 + 60`. On success it stamps `+0x60 = tick`. **A re-path can happen at most once every 60 ticks.**
- **Per-tick update 0x44f1a0:**
  - If the goal accepts the unit's cell: notify **0x20** (arrived) and release.
  - If `|unit − wp[1]|² ≤ 25` (5 px, integer pixels), drop `wp[0]`.
  - Set bit 1 (re-path) when fewer than 2 points remain, or always while mover+0x2e bit 2 (blocked) is set.
- **Order events** live in order+0x4e: 0x20 arrived, 0x40 failed, 0x80 replaced, 0x100 path found, 0x200 unreachable/partial. A plain move waits on mask **0xe0** (0x403244).
- **What follows from this [U]:**
  - An unreachable goal ends the move order through 0x40.
  - A partial route walks to the closest point, exhausts, asks again after the 60-tick gate, and usually comes back empty, which also ends in 0x40.
- **MobileBuild (0x403a20) [S].** Stage 0 installs the rect goal round the site (0x438ad0)
  and waits on 0xe0. On 0x40 (failed) stage 1 measures unit to site minus both footprint
  half-diagonals against builddistance (UnitType+0x212); beyond it the unit says "I can't
  reach the construction site" (0x501580, via 0x47f780) and the mission ends (returns 8).
  Within reach it places the site as on arrival. The engine's `prepareBuildApproach` does
  this on its 0x200 (`retailBuildWithinReach`), without the chatter. Note that the search
  reads unexplored cells as clear (§2.4), so on an unexplored map the builder first
  scouts toward the site and fails only once the way is known.

### 3.2 Steering — mover update 0x43dd20 → ground 0x43cd20 [S]
- **Aim point.** Read `wp0, wp1, wp2` (indices clamp to the last point). Let d = |wp1 − unit|.
  - If d > 80 px, aim at `wp1 − unit_dir(wp0→wp1) · min(d − 80, |wp1 − wp0|)`: an 80-px carrot pulled back along the current segment.
  - Otherwise aim at wp1.
- **Turning.** The turn is clamped to ±UnitType+0x1ba (turnrate) per tick.
- **Speed.** Accelerate (UnitType+0x19e) when both
  `|aim|² > 4·(|Δheading|·v/turnrate)²` and `|wp2 − unit|² > (v²/(2·brakerate))²`
  hold. Otherwise brake (UnitType+0x19a). Without a route, the unit brakes.
- **Speed cap 0x43cc20:**
  - `maxvelocity × pct / 100`, where `pct` is selected by `clamp(pitch>>11, −5, 5)` from {25, 55, 70, 85, 100, 100, 75, 50, 25, 20, 15} (0x505200..0x50520a) [S]. [U]: which sign of pitch is uphill.
  - **Halved** while the unit's height is below sea level, unless the unit is canhover or floater (UnitType+0x241 & 0x81000).

### 3.3 Cell entry — 0x43d7ff [S]
- When the new position maps to a new footprint cell, local players (type 1/2) test it with 0x47db70. The test fails on:
  - a footprint that leaves the map;
  - a blocking feature;
  - **any other unit in a cell (moving or not)**;
  - a depth limit;
  - a slope over maxslope (or over maxwaterslope under water).

  The bad-slope values are not used here.
- **On refusal:** the position is clamped to within half a cell of the current cell centre, speed is capped at maxvelocity/2, and mover+0x2e bit 2 is set. That makes the navigator ask for a new path, subject to the 60-tick gate.
- **Otherwise:** the unit leaves its old cells (0x47d0e0), takes the new ones, and is stamped (0x47cc30).
- [U]: units with UnitType+0x22f == 0 use 0x47d2e0 instead.

---

## 4. Comparison with the engine's Retail mode

`path_compare.py` runs the engine kernel on the TotalA.exe grade planes, with tolerance 4,
weight 1.5 and even map sizes:

| Engine configuration | Cases | Same outcome | Identical routes |
|---|---|---|---|
| as shipped, default costs (turnrate ≤ 200) | 311 | 311 | 202 |
| as shipped, turnrate 500 | 311 | 311 | 152 |
| TotalA.exe cost constants (items 1–2 below) | 311 | 311 | 292 |
| + no weight ramp, no octant march, no node/visit limit (items 3–5; scratch copy of the headers only) | 311 + 302 | all | **613 / 613** |

In other words, the engine kernel **is** TotalA.exe's algorithm plus Kingdoms additions.
Removing those additions and swapping the constants makes its routes identical to
TotalA.exe on every test case.

### 4.1 Identical (no change needed)
- **Terrain grade** (`retailTerrainGrade`, `retailgrade.h`): same min/max 2×2 sampling (NavGrid ctor, `sim.cpp` ~1490), same depth tests, same bad/hard slope split by `lo < sea`. TA's grade 1 corresponds to the engine's 4 and TA's 3 to the engine's 6. TA has no bad-depth band or road grade, but the engine's sentinels default them to the hard limits.
- **Footprint/ring rule** (`retailCachedFootprintGrade`, `boxMinFootprintGrade`): equals TA's local re-rate 0x47e1f0, including the last row/column exclusion. TA's map-load build differs only in that band (§1.5).
- **Visibility sampling** `(x/2 + fx/4, z/2 + fz/4)`.
- **Tracer:** cardinal march, twin trace, L-line exit, loop tests, flag 8 letting A* through a traced cell.
- **A\* core:** heap and pending-root behaviour, tie rules, start node (run 100, heading rounding), fan 4 then 2 (retry 0), `remaining ≤ radius` goal flagging, partial search with the tracer's radius, empty route when `r ≥ h0`.
- **Reconstruction:** 64-entry ring, start included, goal end dropped (`retailReconstructRoute`).
- **Controllers:** circle metric `18·max + 7·min − tolerance` (`retailGoalDistance`); the rect accepts/metric rules; move-order tolerance radius+4.
- **Waypoint conversion** `cell·16 + foot·8`.
- **Navigator and steering:** steering point with the 80-px pull-back (`retailSteeringPoint` mode 0); 5-px waypoint advance; accelerate/brake test; pitch speed table (`retailGroundPitchCap` produces exactly 25/55/70/85/100/100/75/50/25/20/15); provisional direct segment while a search is pending.

### 4.2 Differences (engine = Kingdoms, TA = TotalA.exe)

| # | Engine place | Engine (Kingdoms) | TotalA.exe |
|---|---|---|---|
| D1 | `retailPathCosts`, `RetailCostSearch::Costs` (`retailcost.h`) | step 16/23; turn {0,80,120,160,200,…} scaled by turn rate; entry ground 24 / road 8 / slope 48 (320 heavy) / traffic 80; short turn 136 if run < footprint+3, scaled | step **16/22**; turn **{0,40,60,80,100,80,60,40}** fixed; entry **0** (grades 2, 3) / **30** (grade 1); short turn **75** if run < **5**; no per-unit scaling |
| D2 | `RetailCostSearch::pop` | after 100 pops, weight += 655 per pop (cap 50) | weight fixed for the whole search |
| D3 | `RetailReachability::step` phase 1 | octant march, 7×7 blocked check, slope/ground/road counters, direct 2-point route (−1), traffic bookkeeping | none: the cardinal march starts at the start cell |
| D4 | `RetailReachability::step`, `RetailSearchAttempt::step` | visit limit (w+h)·20; budget-sliced tracer; cost-search node limit cells/20, retries up to 3 (spread 3, limits ×2…), 81-cell probe on final failure | tracer runs to completion inside init (1 work per step); A* unlimited; empty heap → empty route; no retries |
| D5 | `retailInitialPathWeight` (`retailtrace.h`) | load/age formula from 1.5 (3.0 heavy) up to 20 | per player, recomputed every 150 ticks from slot sweeps: < 1 sweep → 9.0, < 2 → 4.5, else 1.5 |
| D6 | `RetailSearchScheduler::tick`, `kPathBudgetDefault`, `kWork*` | 12000 budget; shares by pending players; priority 5×; 7 per slot; 500 init; 10 per pop + 50 on arrival | 1333 per tick into persistent per-player credits for **all** active players; 1 per slot; 100 per init (+1 per tracer step); 1 per pop in 100-pop slices; current player keeps its turn while its credit is > 0; no priority |
| D7 | `retailSearchGrade`, `retailSearchVisible` | unexplored → 5 (traffic cost, 0x40 mark) except within 1 cell of the start; outside the 32-px plane → "not visible" (5); grade 2 → live query; grade 3 → gate probe | unexplored → **2** (clear, entry 0), no start exemption; outside the 32-px plane → **0**; cached grade only |
| D8 | `retailCachedBodyGrade`, `RetailGradePreparation`, `retailAgeGradeRect` | recent 10 / stale 150; moving body → 2 in the plane (search does a live query), stale → 0; age = cap the border at 4 and the interior at 2/0 | one threshold: stamp ≥ tick−30 → transparent, older → 0; structures 0; the age step is a **full re-rate** of `[x−fx, x+w+1)×[z−fz, z+h+1)` (0x440830) |
| D9 | navigator cap (`sim.cpp` ~18749, "clipped = route.size() > 64") | installs 64 points | installs **20** (0x44f0bc) |
| D10 | `retailAdmitNavigator` (`retailscheduler.h`), re-ask dice (`sim.cpp` ~18990: routeCrowded/routeTraffic, rand(8)+rand(8)+…) | 15-tick gate; random re-ask cadence; crowd/traffic flags | **60-tick** gate (0x44f260); a new goal clears it if the last request is ≥ 10 ticks old; re-path when the route has < 2 points or while blocked; no dice, no crowd flags |
| D11 | `retailCircleRadiusSquared` (`retailgoal.h`) | r ≤ 30000: (r² + 128)/256 | always `(r/16)²`, truncating (0x44cf60) |
| D12 | `RetailRingGoal` | inside distance uses Euclid `isqrt`; inner accept `s·256 ≥ in²` | inside distance `in − (18·max + 7·min)`; inner accept `(in/16)² ≤ d²` |
| D13 | `RetailRectGoal::navigationCell` | perimeter clamp of the unit's cell | target `((minX+maxX)/2, maxZ)` (only affects the provisional segment) |
| D14 | `retailGroundScan` / `groundSpeedMode` / `groundMovementMode` (`retailmotion.h`, `sim.cpp` ~19036) | speed ×0.667/×0.333 and a 16-px steering threshold near grade < 4 cells | no such scan: the threshold is always 80 px, the cap is pitch × water only (0x43cd20, 0x43cc20) |
| D15 | water speed (`groundTerrainMultiplier`; FBI `watermultiplier` default 1) | no slowdown with TA data; the multiplier also scales brake and turn | **speed cap halved** below sea level for non-hover, non-floater units; brake, accel and turn are unscaled |
| D16 | partial/failed route handling (`sim.cpp` ~18790: appended order point, refusal caps, re-ask) | keeps re-asking toward the ordered point | partial route walked; empty result → 0x40 → move order ends (§3.1) [U for the order-side end] |
| D17 | cell-entry refusal (`bodyBlockStreak` etc.) | Kingdoms refusal/streak logic | §3.3: any other unit blocks entry; clamp to the cell, speed ≤ maxvel/2, re-path flag [S, engine side not fully mapped] |
| D18 | plane build at load | Kingdoms bounds everywhere | TA's full build allows the last row/column (§1.5); negligible |

---

## 5. Engine change list

These are the minimal edits that make `PathfindingMode::Retail` follow TotalA.exe. They
are listed in priority order with a confidence level. Items 1–5 were verified together:
applied to scratch copies of the headers, they gave 613/613 identical routes. If Kingdoms
behaviour must stay available, gate each item behind a "TA rules" switch.

1. **`retailPathCosts` / `RetailCostSearch::Costs`** (`retailcost.h`). *High.*
   - Old: Kingdoms tables scaled by turn rate, footprint and road/water multipliers.
   - TA:
     - `step = {16,22,16,22,16,22,16,22}`; `turn = {0,40,60,80,100,80,60,40}`.
     - `ground = road = traffic = 0`; `slope (grade 4) = 30`.
     - `shortTurn = 75`; `minStraight = 5`.
     - Ignore turnRate, footprint, road/water multipliers and heavyFloater.
2. **`RetailCostSearch::pop`**. *High.*
   - Old: `if (processed > 100) heuristicWeight += 655 …`.
   - TA: delete it; the weight is constant for the search.
3. **`RetailReachability::step`**. *High.*
   - Old: phase 1 octant march, 7×7 check, counters, direct route.
   - TA: start in phase 2 with `origin = start`, after the start-grade test and `mark(start, 0x20)`. Remove `visitLimit`.
4. **`RetailSearchAttempt::step` / `RetailSearchWorker::dispatch`**. *High.*
   - Old: node limit cells/20, Retry with retry 1–3, final 81-cell probe.
   - TA: no node limit. Exhaustion (empty heap) completes with an empty route and no retry. `spread` stays 2 after the first pop.
5. **`retailInitialPathWeight`** (`retailtrace.h`) plus the scheduler state. *Medium-high.*
   - Old: Kingdoms load/age formula.
   - TA: per player, start at 98304 (1.5). Every 150 ticks set it from `slotVisits150 / maxunits`: < 1 → 589824, < 2 → 294912, else 98304. Then reset the counters.
6. **`retailSearchGrade` / `World::searchGrade`** (`retailgrade.h`, `sim.cpp` ~10910). *High (emulated).*
   - Unexplored → return a clear grade at no cost: the engine's 6, not 5. Drop the near-start exemption.
   - Out of the 32-px plane → 0.
   - Drop the live query for grade 2 and the special-blocker probe.
7. **Body grades** (`retailCachedBodyGrade`, `RetailGradePreparation::prepare/finish`, `ageSearchBody`). *Medium-high.*
   - One threshold `t30 = max(tick,30) − 30`: stamp ≥ t30 → transparent (7), else 0; structures 0; requester transparent.
   - When a body crosses the threshold, fully re-rate its rectangle (`refreshSearchRect`) instead of `retailAgeGradeRect`.
   - Run this at search init for the requester's class only.
8. **Navigator cap** (`sim.cpp`, "clipped = route.size() > 64"). *High (static).* 64 → **20** points, counting the start point that the navigator holds as wp[0].
9. **Re-path cadence** (`retailAdmitNavigator`, re-ask block in `sim.cpp` ~18980–19010). *Medium.*
   - Gate 15 → **60** ticks. A new goal clears the stamp unless the last request was < 10 ticks ago.
   - Request when the route has < 2 points or the mover is blocked.
   - Remove the random dice, routeCrowded/routeTraffic and the appended-point re-ask loop. An empty result while unsatisfied should fail the move (event 0x40).
10. **Scheduler** (`RetailSearchScheduler::tick`, `kPathBudgetDefault`, `kWork*`). *Medium (static).*
    - Per tick: every active player's persistent credit += 1333 / playerCount; loop while the sum of credits > 0.
    - Costs: 1 per slot visit, 100 per init plus 1 per tracer step, 1 per pop, charged per 100 pops.
    - The current player keeps its turn while its credit is > 0. No 5× priority.
    - This affects only *when* routes arrive, not their shape.
11. **`retailCircleRadiusSquared`** (`retailgoal.h`). *Medium-high.* Always `(r/16)²` with truncating division.
12. **`RetailRingGoal`**. *Medium.*
    - Inside distance: `inner − (18·max + 7·min)`, floored at 0.
    - Accept: `(inner/16)² ≤ d² ≤ (outer/16)²`.
13. **`RetailRectGoal::navigationCell`**. *Medium.* Target = `((minX+maxX)/2, maxZ)`.
14. **Water speed** (`retailGroundSpeedCap` callers). *Medium.*
    - Below sea level and not canhover/floater: halve the speed **cap** only.
    - Do not route this through `waterMult`, which also scales brake and turn.
15. **Ground scan** (`retailGroundScan` → `groundSpeedMode` / `groundMovementMode`). *Medium.* Skip the scan for TA: speed mode 0, steering threshold always 80 px.
16. **Cell-entry refusal**. *Low-medium; needs its own mapping of the engine side.*
    - Refuse any cell holding another unit, regardless of motion.
    - Clamp to ±half a cell around the current cell centre and cap speed at maxvelocity/2.
    - Set the blocked flag so the navigator re-paths through the 60-tick gate.
17. **Map-load plane** (optional, negligible). Allow footprints touching the last row/column until the first local refresh, as 0x440500 does.
