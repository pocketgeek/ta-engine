# TA Engine port plan

Total Annihilation engine built from the TAK Engine code base (imported without
history). Every retail behaviour is checked against `TotalA.exe` (GOG Commander
Pack 3.1c), statically and by emulating routines with unicorn (`tools/re/`,
venv `~/venvs/ta-re`). The retail specs live in `docs/retail-*.md`.

## Done

- Repo, build, rename (`ta::`, `taclient`, `taserver`, `TA_*`).
- HPI v1 archives (key, LZ77, zlib), TA mount precedence: loose > rev31.gp3 >
  *.ccx > *.ufo > *.hpi, first mount wins (0x41d4c0, 0x4bb320).
- TNT 0x2000 maps (0x483610) load/save, all 275 shipped maps round-trip;
  terrain drawn from the map's own tiles through palette.pal.
- GAF without version check (0x429700); COB v4 and 3DO parse for all shipped files.
- Sides from `gamedata/sidedata.tdf` (ARM, CORE) replace the five Kingdoms factions.
- Map features placed from TNT feature names (`features/<world>/*.tdf`).
- In-game music by battle status (`docs/retail-music.md`, `client/battlemusic.h`).
- Headless server+client game runs in lockstep with ARMCOM vs CORCOM.

## Port steps (all seven landed)

| Step | State |
| --- | --- |
| 1. Units and weapons (`retail-data.md`) | done: 278 units, weapons by section, build menus from guis/download |
| 2. Metal / energy economy (`retail-economy.md`) | done: 1 s settle, debt stalls, extractors on ore, wind/tidal, reclaim (`reclaim_test`) |
| 3. Kingdoms gameplay stripped | done (gods, auras, sacred sites, mana build areas, monarchs); see "Known gaps" |
| 4. GUI runtime, front end, sidebar (`retail-gui.md`) | done: MAINMENU, SINGLE, SKIRMISH, SELMAP, SELPROV/TCP, NEWGAME, MSNBRIEF, ENDMSN, in-game sidebar and top bar |
| 5. AI (`retail-ai.md`) | done: every level runs the retail brain (RETAIL EASY/HARD plus the engine's levels) |
| 6. Pathfinding (`retail-pathfinding.md`) | Retail A* verified 613/613 against TotalA.exe; goals per 0x44cf60/0x44d3b0/0x44dc60; Legion kept |
| 7. Rendering, campaigns, Cartographer, random maps, MP, packaging | done: 3DO textures; TA campaigns (`retail-campaign.md`); Cartographer on TA planets: stamps v2/v3 .sct, pastes, undoes and symmetrizes (pixel-mirrored) the map's own tiles, 252x252 minimaps, cell-granular resize, saves missions losslessly; random maps from .sct sections; MP failures return to SELPROV; CPack TGZ builds |

## Pathfinding status (docs/retail-pathfinding.md)

Retail mode follows TotalA.exe. The whole change list is applied except item 17
(the map-load plane's last row/column band, negligible):

- A*: fixed costs, constant weight, no octant march / visit or node limits, no
  retries (an empty heap is an empty route); 613/613 routes match retail.
- Scheduler: 1333 work per tick into persistent per-player credits; 1 per slot
  visit, 100 per init with the tracer run inside it (1 per step), 1 per pop in
  100-pop slices; weight 9.0 / 4.5 / 1.5 per player from 150-tick slot sweeps.
- Grades: unexplored clear (6), outside the 32px plane blocked, cached grade only;
  one 30-tick body threshold with full re-rates.
- Navigator: 20 points, 60-tick request gate (a new goal reopens it after 10
  ticks), re-path only when the route is used up or the mover is blocked; an
  empty route ends a plain move; partial routes are walked.
- Goals: circle (r/16)^2, ring per 0x44d3b0, rect target the far side's middle.
- Motion: no ground scan, no road/water multipliers; the speed cap halves below
  sea level for non-hover, non-floater units; a refused cell entry clamps to half
  a cell and maxvel/2.

crowdbench (80 movers, 2 players), units reaching the goal before -> after:
doors 54 -> 79, bridges 43 -> 78, maze 35 -> 44, opposingdoors 0 -> 12, with
about half the search requests. sharedgoal still lands all 80 in the goal area,
but 12 keep re-asking for the crowded goal through the 60-tick gate (TA's
partial-route handling, [U] in the spec). Legion is unchanged (identical hashes).

## Weapon scripts (docs/retail-weapon-scripts.md)

TA units fire through TotalA.exe's call-ins (AimPrimary.. with the return-value
handshake, FirePrimary.., QueryPrimary.., AimFromPrimary.., RockUnit, TargetCleared,
SetMaxReloadTime), with retail's reload formula, accuracy spread, tolerance defaults and
burst emitters; the Kingdoms AimWeapon/FireWeapon/SET 21-23 handshake is gone. Units on a
plain Move fire on the move. Differences: section 7.8 of the spec.

## Known deviations from retail (deliberate)

- All *.hpi are mounted (retail stops after 10, hiding totala3/4 and worlds.hpi on the GOG layout).
- Battle-music monitor resets each game (retail carries it across games); default
  track table used on GOG installs, where retail plays only building music.

## Out of scope (by decision)

- Save / load games (in-game and ENDMSN's between-missions save): skipped at the
  user's request; ENDMSN's Load/Save buttons are greyed.

## Closed in the final pass

- Campaign: briefing narration with SHUTUP, glamour victory pictures and sounds,
  the side's ending movies (Arm 3.zrb / Core 4.zrb, then 5.zrb; endgame.cpp
  0x41fc85), the 1.zrb intro; the static FFmpeg now has the Smacker decoders.
- Kingdoms leftovers removed: per-unit mana and wandering storms. Cloaking is
  paid from stored energy at the settle (it could not work on TA data before).
- Pathfinding change list item 17 (map-load plane edge band).
- Crusades (Kingdoms' alternate balance) and the Kingdoms random-map generator
  removed; random maps are TA-only (worlds Green, Mars, Archipelago, Metal, Moon,
  Lava) and save a 252x252 minimap.
- FBI keys: canreclamate, mobilestandorders / firestandorders; yardmap codes per
  retail-data.md 2.4 (walls block); TA mission condition units (pixels).
- Weapon scripts: AimX / FireX / QueryX / AimFromX per docs/retail-weapon-scripts.md;
  scripted TA units fire, on the move too.
- Tests: TMPDIR for every ctest test and a /tmp leftover guard (pref_guard_check).
- Front end and client (2026-11-23/24): mouse events mapped to renderer pixels
  (clicks on scaled displays); TA's cursors.gaf drawn with palettes/palette.pal;
  GUI labels centred on their glyph rows; the menus loop allsound [BGM] (drone2,
  0x426453) instead of music. Team-colour textures are logos.gaf 10-frame entries
  only (0x42a182..0x42a298); other multi-frame textures animate per tick
  (0x4b8b90) and play only on an activated structure or a moving mobile
  (0x458845). Movement call-ins StartMoving/StopMoving/MoveRate1..3 (0x43da70);
  ground StartBuilding passes the heading (0x4385a7). Nano spray (0x4720d0,
  0x473d50) and the nanoframe (0x458dd0) ported; root pieces draw (0x459958).
- Sim (same pass): ground/naval chase closes to the order's weapon slot
  (0x4034a0/0x4897e0); Suppress/AirToGround have no group check or step-out
  (0x4038a0, 0x412710, 0x413470); flying builders never step out of a stack
  (0x413d80/0x414380/0x414e70); factory door clearing fits TA's centre pads.
  Release suite 716/716.

## Resolved retail questions (docs/retail-weapon-scripts.md section 8)

- Idle Maneuver: the Standby scan (0x43b700) takes only enemies in sight AND in
  slot 0's range; Maneuver chases them at most maneuverleashlength from its post
  (unit-to-post), then walks back. Retail does not advance on an enemy beyond
  weapon range -- the engine already matched; the scan radius is now sight only
  and the picker has no line-of-fire test.
- D-gun: a plain Attack never fires the Commander's slot-2 commandfire weapon; the
  new D-gun order (Cmd::DGun, the BLAST button; AttackSpecial 0x403190) fires it
  once and ends. A bomber's slot-0 commandfire bombs still fire on Attack.
- armwar / armcom sharing one aim signal: confirmed by emulating TotalA.exe's COB
  VM (tools/re/cob_signal_emu.py) -- the later AimX kills the earlier one, which
  never reports, so retail itself often fires only one of the two. Kept.
- Attack group checks: TotalA.exe has no formation or step-out missions (no
  Move_Ground_Formation, VTOL_Move_Formation or VTOL_StepOut row or name; its
  ground table runs Standby .. RepairUnitNoMove). Suppress (0x4038a0: the 0x800
  event exit, then stages 0-2), AirToGround (0x412710) and AirToGroundHover
  (0x413470) therefore never call an outlier back to its group, leave the order
  for it, or step out of an air stack; World skips Kingdoms' check and step-out
  for them (AirStrike already had neither).
- Groups and air traffic: TotalA.exe's mission tables (ground 0x4fc6e8, air
  0x4fca18; 0x19-byte rows: description, handler, ..., flags dword +0x11, name
  +0x15) hold no group bits, no formation and no step-out mission. So in TA no
  flyer mission steps out of a stack (VTOL_Move 0x40fa20, VTOL_Patrol 0x410e70,
  VTOL_Standby 0x40f7d0, VTOL_LandIfCan 0x40f2a0, VTOL_Follow 0x40fbe0,
  AirToAir 0x412d40, VTOL_SeekAttack 0x4103e0, VTOL_SeekGuard 0x410850,
  VTOL_Pickup 0x4111b0, VTOL_Unload 0x411560), and there are no group records:
  World::retailGroupOf is always null, so Kingdoms' group checks, formations,
  standby re-forms and pacing never run (Legion keeps its own formations).
  A multi-unit order spreads its goals once, at issue (the selection order
  0x48cf30): every selected unit but the target counts in the centre; a unit
  whose mission row has flag 0x2 (Move_Ground, VTOL_Move 0x402; Patrol,
  VTOL_Patrol, the repair patrols 0x412) takes the click plus its offset from
  the centre when within sqrt(3000 n) px, else the click -- ground units too.
  Retail mode now does that for every unit on a move or patrol (client and
  sim); Legion keeps its own ground spread. The AI's group orders (0x480460)
  give every member the same point.
- Flying builders' hover: VTOL_MobileBuild (0x413d80) and VTOL_HelpBuild
  (0x414380) approach the site with radius builddistance (0x413f60, 0x4144ed),
  then on every 150th game tick re-pick a point builddistance from the site at
  the builder's bearing + 0xdb6e, facing it (0x414149..0x4141f9,
  0x414569..0x41461a; retailTaBuildHoverGoal); until the first such tick they
  hold where they arrived. VTOL_RepairUnit (0x414e70) hovers over the target's
  position from its stage 1 (0x415048) and repairs under it (0x415098).

## Open
- Kingdoms behaviour still in place: the flyer missions' stages beyond the
  step-out and group checks are still Kingdoms' (VTOL_Move 0x40fa20 is three
  stages -- take-off, a point controller at the footprint-snapped goal, done --
  with no Maneuver scan or coarse-then-precise arrival; VTOL_RepairUnit's
  approach is radius 0 over the target and its work has no range test, World
  keeps a 2 x builddistance ring); the air-traffic counter and occupancy lists
  (Kingdoms 51dc4b) still update, unread; the under-attack alert's trigger and 15 s cooldown are unchecked; the
  result plate's produced columns stay empty (no lifetime totals); TA wreck
  resurrection; nanoframe outlines are front-facing only and use painter's order
  (retail depth-tests per pixel); room_flow's menu-launched cases hang.
- A PARK round a *mobile* producer (a test fixture; no TA unit builds mobile
  units in the field) can still circle in the ring's acceptance gap -- the
  octile distance (0x44d840) is 0 a cell inside the squared inner test
  (0x44d800) -- when parked outputs wall the band; client watch/spectator
  harness checks (gameview_devharness.cpp) still spawn Kingdoms units.
- Transports read TA's keys (TotalA.exe 0x489a90: passenger footprint x <=
  transportsize, count < transportcapacity; an air carrier holds one, 0x41121a).
  TA's transfer reach is scripted (TransportPickup/TransportDrop, BoomCalc), read
  from the COB for surface carriers (cob::transportBoomReach); the air reach is an
  engine stand-in (TA transfers at the exact point). The unload/pickup missions are
  still Kingdoms-shaped. TA's GROUND_UNLOAD (0x406900; dispatcher codes 0x43b7c0):
  stage 0 takes the FIRST passenger only (0x489690; none: 5) and calls TransportDrop
  (0x4069bf), sleep 15; stage 1 waits while the script holds BUSY (0x438730); stage
  2 ends the order once that passenger has left (0x4069f0..0x406a09: one passenger
  per order), else after 3 calls returns 9 (with a next order: dropped; without:
  stage 0 again in 30+rand(30) ticks, 0x43b8e5..0x43b973), else installs the
  drop-point circle once (0x438930; radius 1.5 x footprintz x 16 px for canhover,
  0x406a23..0x406a5e, else 0) and waits on 0xe8 with no timer. The placement test is
  DROP_UNIT's (0x4813b0 -> 0x47db70 mode 1 on the link's cell: any other body,
  the carrier's own included, refuses). Ported so far: the approach is no longer
  re-anchored at the point every 15 ticks while its route is live (the Kingdoms
  poll made a hover brake onto its own drop point; transport_post_unload armthovr
  x5 far). Still open: one passenger per order, the hover's 1.5 x footprintz
  circle, the hull-frame release point (the boom moves with a coasting hull).
- Projectiles: line-of-sight, selfprop/vlaunch and dropped shots now follow
  TotalA.exe (spawners 0x49c9c0 / 0x49cc20 / 0x49dd60, motion 0x49b9ae;
  `sim/retailtashot.h`, `World::tickTaSelfProp`). The bomber's AIRSTRIKE run
  follows TotalA.exe's 0x411f50 (retailAirStrike): its slot holds the target only
  from the release stage to the turn after it, and the release ring leads the
  bomb's carry (sqrt(2 cruisealt / OTA gravity) x 30 x speed + attackrunlength + 1);
  no group check or air-traffic step-out; the stage-6 repair-pad detour is not
  ported, nor the stage-0 wait for take-off altitude. Ballistic shells still use the release solve
  and Kingdoms' gravity-before-move order (TotalA.exe 0x49cde0 launches on the
  slot's AimX angles + spread and moves before gravity, 0x49bce3); beams
  (lineofsight + beamweapon) still hit instantly; burst clones still re-aim at
  the target; the cruise branch of 0x49b3e0 is ported untested.
- corbats at 1110 px: not the shared-aim signal (its aims use signals 2 and 4).
  Its cannon (cor_bats, 350 px/s) has no arc past ~1094 px on level ground at
  gravity 112, so the range gate 0x49aa80 (ballistic -> 0x49a890 = 0x8000)
  refuses it: retail behaviour.
