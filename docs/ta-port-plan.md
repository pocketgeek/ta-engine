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
- Result plate and alerts (2026-11-24): the sim keeps TA's lifetime totals
  (produced / wasted energy and metal as doubles, 0x4019ce..0x401b27, hashed) and
  the map's killmul / timemul; the outcome0.pcx plate fills Name, Kills, Losses,
  Energy / Metal Produced, Excess Energy / Metal and Score as 0x41e420 lays them out
  (count-up bars, docs/retail-campaign.md 4.3). The under-attack alert is TA's
  (client/monarchalert.h): any own unit hit while off screen (0x4071d8 ->
  0x47f850), its own sound class's underattack slot, once per 600 game ticks
  from the last one heard (event table 0x5086dc: 20 s, 0x47feee). room_flow
  passes all 16 cases: a TA_MENU_PICK (debug) and a LAN game's return go to the
  engine menu, not TA's front end; the fallback map is Great Divide; the Test Map
  fixture drops Ashap Plateau's useonlyunits.
- Death accounting, alert details, plate art (2026-11-24): Kills / Losses and the
  unit kill count follow TotalA.exe's death packet switch (0x486892, table
  0x486e64) at the death (0x4866d0), not at retirement: kinds 1 and 6 a loss, 5
  only when another player hit it last, 3 a loss never a kill; the kill when the
  victim is finished and +0xf4 names another player (owner test, no alliance);
  the killer unit's +0xb8 under the same test (0x4869ca). Engine causes mapped
  to TA kinds (retailTaDeathKind): weapon 1, self-destruct 3, change of owner 4,
  passenger 6 (3 when the carrier died of 3, 0x4867da), wall-to-feature 7,
  decay/abort 9, deadly water 11 (no loss now); the commander rule counts 3.
  TA has no experience/score/bounty per kill. TA's veterancy is the kill count:
  0x489bf3 scales every record by the victim's kills (4% per 5, max 20%;
  retailTaVeteranDamage), on hits, self-destruct and water. Under-attack: skipped
  while the current mission has flag 0x80 (Attack_NoMove / Attack_Chase /
  AttackSpecial, table 0x4fc490), and only when the previous record came from
  another player or was a weapon hit (Unit::lastHitKind = +0xf5, hashed); the
  cooldown starts only when the speech plays (0x47feee); "<unit>: Under Attack"
  goes to the message area (0x47ff69 -> 0x463ca0); Options UNIT SPEECH / UNIT
  CHAT TEXT are TA's SPEECH (sounds.gui) and UNITCHAT (speeds.gui), Off|Medium|
  Full = 0|5|10, defaults 10 / 5 (0x430537..0x430596). Plate: guipal.pal's EGA
  colours matched into palette.pal (0x4ac7d0): cell 84, bevel 0 / 82, well 90
  (+0xdd3, guipal 8), bar 213 (+0xdcf, guipal 4); PlayerColor is textures\
  logos.gaf "32xlogos" frame = colour, mapped from (1,1)-(31,31) onto 91x21
  (0x41e5b1, 0x4a4a1a).

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
  (Kingdoms 51dc4b) still update, unread; nanoframe outlines are front-facing
  only and use painter's order (retail depth-tests per pixel).
- Death accounting (countTaDeath, retailveterancy.h): World's death pipeline
  still runs on Kingdoms death kinds (Unit::deathType: severity, Killed/Dying, the
  death weapon), mapped onto TA's for counting only. Not yet TA-shaped: the
  commander rule's sweep (0x486f10) is still Kingdoms' immediate removal (TA
  sends each unit a kind-3 record, so each runs Killed and leaves its corpse /
  explosion); a passenger still dies without Killed (TA: a 30000 kind-6/3
  record, 0x48680b); no engine cause yet for kind 5 (ReclaimUnit 0x404730 is not
  ported); a unit already at 0 HP but not yet swept when its commander dies
  counts nothing (TA: its own death packet later). The kind-3 loss's statistics
  guard (local player +0x129[owner], 0x457540) is network-only and not modelled.
  A change of owner keeps the unit's +0xf4/+0xf5 (TA's new object starts at
  10 / 0). The Kingdoms experience routines (xp, vetMul) are still wired but TA
  credits no experience, so they stay at level 0 unless a placement sets one.
- Under-attack: the gate is decided on weapon hits only (World::applyHit); TA
  runs 0x406f80 for every damage record but repair and water (self-destruct,
  change of owner, passengers too), and World's hit response before it is still
  Kingdoms' 409a50 (TA's own response, 0x406f80..0x4071aa, is not ported). The
  speech queue's global spacing (+0x9d/+0xa1, 0x47f6bb) is not modelled: one
  request per frame, text at once.
- A PARK round a *mobile* producer (a test fixture; no TA unit builds mobile
  units in the field) can still circle in the ring's acceptance gap -- the
  octile distance (0x44d840) is 0 a cell inside the squared inner test
  (0x44d800) -- when parked outputs wall the band; client watch/spectator
  harness checks (gameview_devharness.cpp) still spawn Kingdoms units.
- Transports read TA's keys (TotalA.exe 0x489a90: passenger footprint x <=
  transportsize, count < transportcapacity; an air carrier holds one, 0x41121a).
  TA's transfer reach is scripted (TransportPickup/TransportDrop, BoomCalc), read
  from the COB for surface carriers (cob::transportBoomReach); the air reach is an
  engine stand-in (TA transfers at the exact point). GROUND_UNLOAD (0x406900) is
  ported (retailTransportUnloadApproach / retailSurfaceUnloadTransfer): every
  surface unload starts with its TransportDrop call (counted, mission +0x36); out
  of reach it sleeps 15 and installs the drop-point circle (0x438930: 1.5 x
  footprintz x 16 px for a hover, else 0, 0x406a23..0x406a5e), waking on the
  navigator only; the third failed call returns 9 (next order: dropped; none: a
  call every 30+rand(30), 0x43b8e5..0x43b973). A refused DROP_UNIT (mode 1 at
  release) is a failed call, not an abort. One passenger per order (the FIRST,
  0x489690; 0x4069f0..0x406a09). The release point rides the hull: the call's
  offset turned by the heading change since (retailHullPoint, Order::
  transportHeading). Engine stand-ins: the boom animation is the 15-tick beam;
  the wait also wakes after 60 ticks once the hull has stopped (the engine's
  navigator never posts retail's 0x40 for a goal it cannot reach, see
  retail-pathfinding.md D16); the released passenger keeps Kingdoms' dispersal
  PARK (409154; DROP_UNIT past placement is untraced). Still Kingdoms-shaped:
  GROUND_PICKUP (0x406780: carrier-only -- the passenger gets no mission; stage
  0 the size test, then TransportPickup + BUSY, 3 calls, the circle on the
  passenger's position with radius 0, wait 0xe8; ours keeps the reciprocal
  passenger Load order and the engine beam), VTOL_PICKUP / VTOL_UNLOAD
  (0x4111b0 / 0x411560, not re-traced). The dispatcher's code 3 sleeps
  30+rand(15) and code 9 30+rand(30) with mission flag 0x800000 in TotalA.exe
  (0x43b890, 0x43b94f); retailDispatchMissions still has Kingdoms' rand(30)+15
  and 0x400000 (GROUND_UNLOAD handles its own 9).
- Resurrection follows TotalA.exe RESURRECT (0x404db0): any reclaimable feature
  (0x440043, flag 0x80) whose name up to its first '_' names a unit type
  (0x404f36 -> 0x488b10; FeatType::raises: cornecro_dead and cornecro_heap both
  raise cornecro) -- a body, a map wreck or a heap (Cmd::Resurrect's target
  -featureId); timer ftol(buildtime x 0.3 / (workertime/30)) counted down one a
  tick, nothing paid; the unit comes back finished at 1 HP (0x405215) and the
  caster's REPAIRUNIT on it goes to the head of its queue (0x405243..0x40529f,
  0x43acb0); a full unit cap retries in 300 ticks (0x405119). The animate
  channel (cananimate) is still Kingdoms'. The client's right-click raises a
  feature too (GameView::reviveKindOnFeature); the cursor over a raisable
  feature still shows reclaim.
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
