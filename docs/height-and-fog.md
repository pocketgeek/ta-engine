# Height rendering and fog of war

TA:Kingdoms terrain is a **flat tile mosaic** — cliff/slope relief is painted
into the 32px tile art, and the heightmap drives gameplay (nav) plus a **per-unit
screen lift**, *not* geometric terrain displacement. This matches the retail
engine (KINGDOMS.icd: flat-tile software renderer + vertical unit lift, no
terrain skew). Everything below lives in `src/client/main.cpp` unless noted.

## Unit lift

- `heightAbove(wx, wz)` = bilinear heightmap sample minus the map's **modal**
  height (`heightRef_`), clamped ≥ 0. The modal reference means a uniformly-high
  map lifts nothing.
- `terrainLift = heightAbove * kHeightScale_` (screen-Y, toward north) and
  `terrainLiftX = heightAbove * kHeightScaleX_` (screen-X). The 2.5D view is
  tilted diagonally, so height would displace both axes. Current constants:
  `kHeightScale_ = 1.1` (art-calibrated; shared by the seat lift, wall occlusion,
  and picking) and `kHeightScaleX_ = 0` (horizontal **off** — a nonzero X scale
  can't be inverted cleanly for both N-S and E-W walls, so vertical-only is the
  retail-faithful choice; the residual "east offset" on diagonal walls is the
  accepted limit). Live-tunable via `TA_HSCALE` / `TA_HSCALEX`.
- Applied to **every height-anchored draw** of a mobile unit: sprite, shadow,
  HP/production bars, selection brackets, order rings, build ghosts, projectiles,
  particles, effects.
- **Buildings are exempt.** Use `isStructure(type) = type->maxVel <= 0` (the FBI
  `canmove` flag is unreliable — the Keep sets `canmove=1` with no velocity). A
  building sits flat on its footprint; `uLiftX(u)` / `uLiftY(u)` return 0 for
  structures. (Mana-deposit cells carry a large heightmap value while their
  medallion — a feature — draws unlifted, so lifting a lodestone there would float
  it off its base.)

## Wall occlusion

A unit on flat ground *behind* a wall is placed correctly, but the wall's baked
relief art projects up-and-north over that ground, and terrain paints before
units — so the unit would draw *over* the wall. `wallOcclusionY` scans up to
`kOccScan_` (12) cells south (toward the camera) for the tallest blocking wall
and clips the unit's model above that line, redrawing the hidden part as a faint
team-tinted silhouette so the unit is never lost. `World::setTerrain`
(`src/sim/sim.cpp`) also blocks the occluded ground in the nav grids so units
settle in *front* of walls; flat wall tops have no higher cell to their south, so
they stay walkable ramparts.

## Height-aware picking (screen ↔ world)

Because units render lifted, **every** screen↔world interaction must invert or
apply the lift — a flat `offX + sx/zoom` map lands on the wrong cell:

- `pickWorld(sx, sy)` — screen→world. Marches down the lift to return the
  front-most surface whose lifted position is under the cursor. Use for
  move/attack orders, click-select, and **placement/building** (conjuring onto
  elevated ground otherwise drops the unit on the low cell behind the rise).
- `unitScreen(u)` — world→screen of a unit's drawn body, including flyer cruise
  altitude. Use for **marquee/click selection** so a lifted or airborne unit
  (e.g. the flying Monarch) is picked where it is drawn.

## Fog of war

Fog is a **local display** computation, **not** part of the deterministic sim:
`World::updateVisibility` (`src/sim/sim.cpp`) runs at 4 Hz and is skipped for the
headless referee (`visPlayer_ < 0`), and `vis_` is **not** folded into
`stateHash`. So fog and line-of-sight math may use plain floats freely (no
`detmath` requirement).

- **Reveal + line of sight.** Each unit reveals cells within
  `max(sight, radar) / 16`, but a cell is revealed only if `sightClear()` finds no
  intervening terrain rising above the straight eye→target sight line — so walls,
  cliffs, and hills cast fog shadows on their far side, while water (being low)
  never blocks. Eye height = the unit's ground height + `TA_FOG_EYE` (default
  40); an obstacle must clear the sight line by `TA_FOG_MARGIN` (default 16) to
  block. Those defaults mean real walls (height 60–220 on athri cay) shadow but
  small rock clutter (< ~56) does not. Radar range skips the LoS test (radar is
  not line of sight). `World` keeps the raw heightmap (`heights_`) for this.
- **Overlay.** `drawFog` renders the darkening as a per-cell **lifted mesh**
  (`SDL_RenderGeometry`, `terrainLift` / `terrainLiftX` per grid corner, culled to
  the viewport plus a max-lift margin) so the fog sits on the terrain relief and
  follows a unit up a hill, rather than a flat sheet at ground level.

## Tuning knobs (env)

| Var | Effect | Default |
| --- | --- | --- |
| `TA_HSCALE` | unit lift per height unit (screen-Y) | 1.1 |
| `TA_HSCALEX` | unit lift per height unit (screen-X) | 0 (off) |
| `TA_FOG_EYE` | fog eye height above the unit's ground cell | 40 |
| `TA_FOG_MARGIN` | how far terrain must clear the sight line to block | 16 |
| `TA_HDEBUG` | force-enable the height debug overlay (also F7 in game) | off |
