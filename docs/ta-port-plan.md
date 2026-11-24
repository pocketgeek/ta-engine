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
- Hold Position lift (owner's choice, kept 2026-11-24): a landed flyer on Hold
  Position lifts off to fight a target in range (tickHoldLift, hold_flyer).
  Retail stays down: VTOL_Standby 0x40f9d3 / LandIfCan 0x40f6a3 make the slots
  manual and 0x43b1f0 refuses an attack on Hold (0x43b211), so idle flyers keep
  automatic slots here.

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
- Campaign picker (2026-11-24): the title screen's CAMPAIGN opens the engine's
  campaign picker (CampaignScreen) inside TA's front end. On TA data it is TA's
  any-mission screen: NEWGAME.GUI on playanygame4 (retail-gui.md "New campaign"),
  Arm / Core as the side tabs, Choose Campaign lists camps/*.tdf for the side or
  ALL (0x476a60), Choose Game the missions with DONE / PLAY and the next-up mission
  highlighted; Set Difficulty, Previous Menu, Start. A pick goes through MSNBRIEF to
  the mission (retail-campaign.md 1.3, any mission may start), ENDMSN's Main Menu
  returns to the picker, and a win is recorded in Settings::campaignCompleted (not
  retail, 1.4). The TaCampaign in play now outlives the session, so ENDMSN's Start
  launches the mission picked there.
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
- Nanoframe outlines are front-facing only and use painter's order (retail
  depth-tests per pixel).
- Death accounting (countTaDeath, retailveterancy.h): World's death pipeline
  still keys most deaths on Kingdoms death kinds (Unit::deathType: severity,
  Killed/Dying, the death weapon), mapped onto TA's for counting. TA-shaped
  since 2026-11-24: non-weapon damage goes through taDamageRecord (TotalA.exe
  0x489bb0 -> 0x489ce0: the damagemodifier under 30000, kill scaling, +0xf5 /
  +0xf0 / +0xf4); the commander rule (only the commander's own death,
  0x48667e) sweeps with commanderSweep (0x486f10: a 30000 kind-3 record from
  itself to each unit not already dying, 0x486f6f), so each dies at its own
  update, runs Killed, leaves its corpse and SELFDESTRUCTAS; a dying carrier
  gives each passenger a 30000 kind-6 (3 when it died of 3) record from its
  killer and lets it go (0x4867da..0x48681d), the passenger dying at its own
  update through Killed; a self-destruct runs Killed too (severity from the
  death sender 0x4864b0, retailTaDeathSeverity, for kinds 3 / 5 / 6); a change
  of owner resets +0xf4 / +0xf5 to 10 / 0 (0x485c83). Still open: ReclaimUnit
  (0x404730, kind 5) is not ported -- World has no unit-reclaim order (the
  death side is ready: Unit::kDeathReclaim, severity 0, retailTaDeathCountsAsLoss);
  the remote-owner branch of 0x486f10 (0x49b000 + 0x4864b0(3) for +0x73 3)
  is folded into the record; the kind-3 loss's statistics guard (local
  player +0x129[owner], 0x457540) is network-only and not modelled; the
  other Kingdoms kinds (weapon, water, mission Kill, feature) keep 512610's
  severity and Dying. The Kingdoms experience routines (xp, vetMul) are still
  wired but TA credits no experience, so they stay at level 0 unless a
  placement sets one (the Legion fixture fight-retarget's killers no longer
  reach level 1: six legion keys are accepted regressions in
  tools/scenarios/baseline.json, and 15 combat fixtures' hashes were retaken
  for Unit::lastHitKind).
- Hit response: TotalA.exe 0x406f80..0x4071aa (taHitResponse) replaces
  Kingdoms' 409a50 for weapon hits and every taDamageRecord but water (the
  self-destruct and commander records, passengers' records): event 0x10; a
  cancapture type of a computer player rolls its AI's threat deadline
  (now + 30 + rand(300)) and drops its missions (0x439eb0(0)); retaliation
  only from no mission or a Standby (descriptor flag 0x20000; a patrol no
  longer answers), then the auto slots. Approximations: the retaliation's
  category / reach tests are World's retailAnswersAttacker / slotHitResponse
  (0x49abb0's underwater tests and the +0x23d / +0x231 category sets are not
  traced one by one); the old object of a change of owner (kind 4,
  0x4886a4) runs no response; the weapon path still lowers the HP before the
  response (0x489ce0 responds first). The under-attack request rides
  World::recordHurt for records (HitFx::hurtUnits for weapon hits). The
  speech queue's global spacing (+0x9d/+0xa1, 0x47f6bb) is not modelled: one
  request per frame, text at once.
- Air missions (ported 2026-11-24 from TotalA.exe, air table 0x4fca18):
  VTOL_Move 0x40fa20 (take-off over the spot to cruisealt/2 when landed, a
  radius-0 controller at the footprint-snapped goal, done), VTOL_Patrol
  0x410e70 (320 px past the leg, radius 0x150, sleep 30), VTOL_RepairPatrol
  0x4152f0 (legs at cruisealt, radius 0, sleep 45; job search
  tickFlyingPatrolLook), VTOL_Standby 0x40f7d0 / VTOL_LandIfCan 0x40f2a0,
  VTOL_Follow 0x40fbe0, VTOL_SeekGuard 0x410850, AirToAir 0x412d40 (with
  VTOL_Evade 0x413bc0), VTOL_SeekAttack 0x4103e0, VTOL_RepairUnit 0x414e70
  (radius 0 over the target, no range test), VTOL_Reclaim 0x414770. The
  air-traffic counter is gone (TA has no step-out); the occupancy grid and its
  overlap lists stay (TA's landing test 0x47e2d0 reads cell word +2; projectiles
  read the grid; how TA maintains word +2 is untraced). Still open:
  - No repair pads: VTOL_Landing 0x4118e0 (QueryLandingPad, SELFREPAIR) and
    VTOL_GetRepaired 0x415250 are not ported, so the under-3/4-health pad
    detours of VTOL_Patrol (0x410f3e), VTOL_RepairPatrol (0x415397),
    VTOL_SeekAttack (0x410518), VTOL_SeekGuard (0x410972) and AirStrike are
    skipped.
  - Every TA flyer mission's stage 0 makes the slots manual (0x4898b0(3));
    World keeps an idle flyer's (VTOL_Standby 0x40f9d3, LandIfCan 0x40f6a3)
    automatic because the Hold Position exception (tickHoldLift, hold_flyer)
    relies on it; VTOL_Move clears and restores them at once.
  - The take-off climb of VTOL_RepairUnit / VTOL_Reclaim / VTOL_RepairPatrol's
    jobs (0x414fe1, 0x41485e) and AirToAir / SeekAttack (0x4133fa, 0x4107c6)
    is not modelled; LandIfCan's off-map return (0x40f2e3) is not ported.
  - VTOL_RepairPatrol's job search runs on a 45-tick timer from
    tickPatrolRepair, not from its stage-1 wake. The ground RepairPatrol
    (0x405980, tickGroundPatrolLook) looks on a 60-tick timer the same way
    (its sleep 60), keeping World's patrol navigation (its radius-16 circle
    on the waypoint is not modelled). A fight-move leg (ours; TA has none) keeps
    a scan on VTOL_Move.
- Nanoframe outlines are front-facing only and use painter's order (retail
  depth-tests per pixel).
- Result plate: Kills / Losses still count by the Kingdoms death kinds
  (retailveterancy.h); TA's rule is the death routine's switch at 0x486892 (table
  0x486e64: kinds 1 and 6 always a loss, 5 only when another player hit it last, 3
  by alliance; the kill when finished and the hitter is another player), with the
  TA damage kinds of 0x489bb0's callers (weapon 1/2, self-destruct 3, ...) not yet
  mapped onto the engine's. The bar and well colours (game +0xdcf / +0xdd3) and
  the PlayerColor gadget's art are stand-ins.
- Under-attack alert: the speech line "<unit>:
  Under Attack" (0x47ff69 -> 0x463ca0) and the speech/text level options (game
  +0x37f17 / +0x37f18) are not ported.
- A PARK round a *mobile* producer (a test fixture; no TA unit builds mobile
  units in the field) can still circle in the ring's acceptance gap -- the
  octile distance (0x44d840) is 0 a cell inside the squared inner test
  (0x44d800) -- when parked outputs wall the band; client watch/spectator
  harness checks (gameview_devharness.cpp) still spawn Kingdoms units.
- Transports read TA's keys (TotalA.exe 0x489a90: passenger footprint x <=
  transportsize, count < transportcapacity, asked when LOAD is given). LOAD is
  the carrier's order (0x43f701 -> 0x43fb61): GROUND_PICKUP / VTOL_PICKUP for
  the carrier and no mission for the passenger (TA has no Move_Seek_Pickup;
  World::loadInto, the carrier's list takes Shift / Ctrl / toggle, matchsetup;
  the client no longer boards a unit clicked onto a transport). GROUND_PICKUP
  (0x406780; retailGroundPickup): size test, BUSY wait (0x438730), a counted
  TransportPickup call + sleep 15, BUSY wait, aboard -> 5, third failed call
  -> 9, else a radius-0 circle on the passenger (wait 0xe8), controller off and
  round again. VTOL_PICKUP (0x4111b0; retailVtolPickup): fails while the
  carrier holds anyone (0x41121a) or the passenger is under the sea; take off
  (half cruisealt), follow the passenger at cruisealt (radius 0x30),
  QueryTransport, BeginTransport(model height) and down until the link piece
  meets it, ATTACH_UNIT there, climb back. VTOL_UNLOAD (0x411560;
  retailVtolUnload): to the point at cruisealt (radius 0x140), DROP_UNIT's
  placement test (9 when refused), down to the passenger's height, test
  again, EndTransport and drop where the link holds it, climb back. The attach
  (attachCargo, 0x48ab70 + 0x4384a0) puts the passenger at the head of the
  carrier's list (an unload takes the last one aboard), clears its orders and
  makes its slots automatic (BeCarried 0x402fc0); the release (releaseCargo,
  DROP_UNIT 0x4813b0) orders nothing -- Kingdoms' dispersal PARK (409154) is
  gone. GROUND_UNLOAD (0x406900) as before (retailTransportUnloadApproach /
  retailSurfaceUnloadTransfer). The surface carriers' own scripts
  run (2026-11-24): the sim VM has ATTACH_UNIT / DROP_UNIT (0x4b1b92 / 0x4b1b7d
  -> 0x481340 / 0x4813b0) and TA's GET 6..16 (0x480770: BUSY, PIECE_XZ,
  PIECE_Y, UNIT_XZ, UNIT_Y, UNIT_HEIGHT, XZ_ATAN, XZ_HYPOT, ATAN, HYPOT,
  GROUND_HEIGHT; deterministic: retailDirection, isqrt64), SET 6 writes BUSY
  and every SET posts event 4 (0x480b20). GROUND_PICKUP's call runs
  TransportPickup(passenger) and waits BUSY; GROUND_UNLOAD's runs
  TransportDrop(passenger, packed point), then sleeps 15, waits BUSY and
  tests the passenger aboard (retailScriptedUnloadTransfer). The passenger
  rides the ATTACH_UNIT piece (Unit::attachPiece, +0xf9; cargoAttachPoint,
  4dd250) and DROP_UNIT puts it down at the link after 0x47db70 on its
  footprint cell. A cancelled order no longer stops the animation. A
  script-less (test) carrier keeps the old stand-in (fifteen ticks of BUSY,
  the static BoomCalc reach cob::transportBoomReach, still the engine's
  pre-call brake test). An unload given while TransportPickup still swings
  the boom home races it for the same pieces, as in retail. A hover pinned
  by a cliff with its crane astern can stand out of reach (the roster fixture
  moved hovers to open land). GET ids above 20 (Kingdoms' 27..46: heading,
  terrain, animation speeds) still answer Kingdoms values. GROUND_PICKUP's
  circle and GROUND_UNLOAD's wait also wake 60 ticks after the hull stops
  (the engine's navigator never fails an unreachable point,
  retail-pathfinding.md D16). Air cargo rides the carrier's own point, not
  the link piece's (VTOL_PICKUP's engine attach passes piece -1); the
  BeCarried 10-tick poll after the drop (the default mission comes at once
  here), the flag-4 missions BeCarried keeps, the air base exemption (flagsA
  bit 9), VTOL_PICKUP's event 2 / 0x10000 bits and the unit-follow
  controllers' exact altitude rule (0x44e190 / 0x44e250 are taken as the
  passenger's y plus the altitude) are untraced or unmodelled.
- Mission dispatcher (0x43b7c0, retailDispatchMissions): code 3 sleeps
  30 + rand(15) (0x43b890) and code 9 sets mission flag 0x800000 and, alone,
  sleeps 30 + rand(30) (0x43b8e5..0x43b973); the rest matched already. Not
  modelled: an event 0x10000 drops slots 0..2's targets with TargetCleared
  (0x43b840 -> 0x48a0f0) -- TA's unit event word is 16 bits, so only a
  mission's own pending word carries it, while the engine's 0x10000 is
  Kingdoms' slot-fired event; the second mission list (+0x60, flag 0x40000);
  the 0x800000 flag's reader (the navigator, 0x44f3e9). TA's loop has no call
  cap (0x43b99f); the engine keeps 100 as a guard.
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
