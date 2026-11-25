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
| 3. Kingdoms gameplay stripped | done, audited 2026-11-24: every `Kingdoms` / `KINGDOMS.icd` / monarch / mana / Crusade hit in src/sim and src/ai checked (TA-correct, a kept engine feature, or dead code listed under Open (d)); the Kingdoms rules still running in TA games replaced from TotalA.exe -- unit slots (first free record 0x485fea, freed at the death 0x486dc7; was 5121d8's CRT-ranked slot), SUPPRESS (0x4038a0; was 4040b0's melee / feature / minimum-range ring), the area clear order (no RECLAIMAREA row in 0x4fc6e8 / 0x4fca18: removed), death severity (0x4864b0 for every kind; was 512610's), the air pad list's rebuild after the units (0x495490 -> 0x464f80 -> 0x40b2c0); gods, auras, sacred sites, mana and monarchs went earlier. The ballistic shell, the last Kingdoms rule, is TotalA.exe's since the second pass (2026-11-24): AimX's pitch solve 0x49a890 (equal to the binary on 63 emulated cases, tools/re/ballistic_emu.py), the range gate 0x49aa80 in the fire path, the launch 0x49cde0 on the slot's AimX angles plus the spread with the barrel word (0x49e11b, 0x49ce6a), and the motion 0x49bce3 (move, the wind vector G+0x37ecc = 0x490cf7's, then gravity; once a tick); burst clones copy the emitter (0x49b83b). Kingdoms code left unreachable: the Classic brain and the engine's old Controller levels are deleted; the third pass (2026-11-24) removed the rest that TA data cannot reach: the stone / freeze / mind-control statuses (Unit freeze and stone timers and statue corpses, the charm and its capture gates, cantbestoned / cantbefrozen / cantbecaptured, the client's statue meshes and PETRIFIED / FROZEN labels), the Remote Effect and Wandering weapon kinds and their fields, cananimate / animatetype (Resurrect raises only the body's own unit), weaponswitching (World::setWeapon, the AI weapon switch, switchreloadtime, the HUD weapon buttons and Cycle Weapon / Select Melee / Select Magic keys; Cmd::SetWeapon is refused, its number kept) and melee weapons (footprint contact, Attack_Melee's approach and retarget, the cursor's melee range), the lodestones (onMana, the in-place upgrade, the sacred-site yards and income, mogriumbounty, the lodestone box and the order trail's deposit sites; the ore-patch list is World::depositSpots), the group records' pacing (51b890 / 4d95f0), Kingdoms' experience routines (experiencepoints, getLevel / setLevel, noveteran: TA's veterancy is the kill count) and the Kingdoms side filters in Cartographer. The authored veteran rank became TA's kill count (2026-11-24, owner decision): a placement, a type's scenario stats, Cartographer's unit field (now VETERAN KILLS) and a mission's engine-only `v F` set Unit::kills, rank N = N x 5 kills (.crt 2.0 holds kills; a 1.0 file's rank converts on load and writes back unchanged; `v F` names rank round((F-1) x 10)), so TA's own rules read it -- damage taken 0x489bf3, the lead past 5 kills 0x48a324, the reload 0x49e468; vetMul / vetOverride / the rank levels, the veteran shot and unit models, the HUD rank mark and the script-less reload's veteran divisor are gone (stateHash folds a constant where they went). A mission's [units] `Kills` key is no longer read (TotalA.exe's parse 0x436c30 has none; 1100-odd shipped records say 1..66). Removed fields that sat in the Unit layout are pads; stateHash folds a constant where a folded field went. What is left is justified under Open (d). Evidence: suppress / legion_suppress, retail_trace (slot pools), pathblock, transport*, click_rules, revive_order, server_commands, ta_ballistic, ta_weapon_script; legion baselines retaken for the slot order and the shell motion (tools/scenarios/baseline.json); authored_kills (rank 2 = 10 kills, 8% less damage, the lead; .crt 1.0 conversion; Cartographer round trip) |
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
- Area reclaim (owner's choice, kept 2026-11-24): a right-drag box over the map,
  or the armed CLEAR box, gives each selected reclaimer one area order that
  reclaims (or raises) every wreck and feature in the box in turn
  (World::reclaimArea / tickReclaimArea, Cmd::ReclaimArea; reclaimarea_test,
  click_rules, the client's TA_AREA_DRAG_TEST). Retail TA has no RECLAIMAREA:
  neither mission table (0x4fc6e8 / 0x4fca18) has the row. The stages follow
  Kingdoms' RECLAIMAREA (KINGDOMS.icd 406670 and its cell walk 509cc0); each
  child is TA's own feature reclaim. Not dead code; Cmd::BuildManaArea stays
  refused.

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

## Ported details (formerly listed as Open)

What these entries describe is ported; what is left of each is under "Open".

- Nanoframe (client/nanoraster.h): drawn as retail's height-buffered object
  bitmap, one texel per game pixel -- whole-pixel vertices, retail's polygon
  walk (0x4c0820 / 0x4c8760: left chain backward, rows [ymin, ymax), the walk
  is the cull), byte heights compared `buffer <= own` and written (0x4c0acf,
  0x4c7b53), so they wrap past 205; bias 50, 125 for an FBI `digger` (UnitDef
  +0x241 bit 30, 0x42c885, 0x459a2f); outlines are each row's two end pixels
  of every primitive, depth-tested and writing (0x4c0a90).
- Animated textures: only dont-cache pieces play (cached ones are drawn with
  frame 0 into the cached layer, 0x4586a0(obj, 0, 1) -> 0x459830 arg 1 ->
  0x459b67); a `zbuffer=1` type (UnitDef +0x241 bit 7, 0x42c5bb -> unit +0x114
  bit 0, 0x485ad1) redraws them every frame with the playing frame (0x4586d6 ->
  0x459641 arg 0); others only while active / moving (0x458845, 0x458635).
  Under construction a mobile unit plays all, a structure none (0x45960d).
- Repair pads (client): a rider's heading is the pad's plus its pad piece's own
  y turn (0x43d78e -> 0x43e180); the pad sprays from its QueryNanoPiece to the
  rider's box on each tick SelfRepair mends it (0x402518 -> 0x40252e ->
  0x4025ae, the builder repair's emitter).
- Death accounting (countTaDeath, retailveterancy.h, retaildeath.h): non-weapon
  damage goes through taDamageRecord (0x489bb0 -> 0x489ce0: the damagemodifier
  under 30000, kill scaling, +0xf5 / +0xf0 / +0xf4); the commander rule (only
  the commander's own death, 0x48667e) sweeps with commanderSweep (0x486f10: a
  30000 kind-3 record from itself to each unit not already dying, 0x486f6f); a
  dying carrier gives each passenger a 30000 kind-6 (3 when it died of 3)
  record from its killer and lets it go (0x4867da..0x48681d); a change of owner
  resets +0xf4 / +0xf5 to 10 / 0 (0x485c83); a reclaim (kind 5) pays the
  reclaimer at the death (0x486c9e). The death severity is TotalA.exe's death
  sender's for every kind (2026-11-24 audit; 0x4864b0, retailTaDeathSeverity:
  0 for a change of owner, a reclaim, a feature conversion and a decay,
  0x48653d..0x48654d; else the overkill plus the sample of a second before,
  halved, 1..100, 0x486552..0x4865a4; Killed only when it is not 0, 0x4865c3)
  -- Kingdoms' 512610 severity is no longer called. Unit::deathType keeps the
  engine's cause numbers, mapped onto TA's kinds by retailTaDeathKind.
- Hit response: TotalA.exe 0x406f80..0x4071aa (taHitResponse) for weapon hits
  and every taDamageRecord but water: event 0x10; a cancapture type of a
  computer player rolls its AI's threat deadline (now + 30 + rand(300)) and
  drops its missions (0x439eb0(0)); retaliation only from no mission or a
  Standby (descriptor flag 0x20000; a patrol does not answer), then the auto
  slots. The under-attack request rides World::recordHurt for records
  (HitFx::hurtUnits for weapon hits).
- Air missions (air table 0x4fca18): VTOL_Move 0x40fa20 (take-off over the spot
  to cruisealt/2 when landed, a radius-0 controller at the footprint-snapped
  goal, done), VTOL_Patrol 0x410e70 (320 px past the leg, radius 0x150, sleep
  30), VTOL_RepairPatrol 0x4152f0 (legs at cruisealt, radius 0, sleep 45; job
  search tickFlyingPatrolLook), VTOL_Standby 0x40f7d0 / VTOL_LandIfCan
  0x40f2a0, VTOL_Follow 0x40fbe0, VTOL_SeekGuard 0x410850, AirToAir 0x412d40
  (with VTOL_Evade 0x413bc0), VTOL_SeekAttack 0x4103e0, VTOL_RepairUnit
  0x414e70 (radius 0 over the target, no range test), VTOL_Reclaim 0x414770,
  AirToGround 0x412710 (stage 0's take-off climb, half-way approach, a
  controller on the target of radius slot 0's range, the run 3 x range past
  it, the 90-degree turn, back to stage 2) and AirToGroundHover 0x413470 (the
  hold two thirds of the range out on the near side, 45 degrees either side of
  the bearing in turn), both on slot 0's range (retailairground.h,
  World::tickTaAirAttack). No air-traffic counter (TA has no step-out). The
  flyers' cell grid (word +2) is TotalA.exe's (below, "Flyer cell grid");
  TA's landing test 0x47e2d0 and the projectiles read it.
- Repair pads (retailairpad.h): VTOL_Landing 0x4118e0 and SelfRepair 0x402430
  run, with the under-3/4-health detours of VTOL_Patrol (0x410f3e),
  VTOL_RepairPatrol (0x415397), VTOL_SeekAttack (0x410518), VTOL_SeekGuard
  (0x410972), AirStrike (0x4125c2), AirToGround (0x412b3f) and
  AirToGroundHover (0x413a8a). Landing by order: the default order on an own
  or allied air base (0x43f0e0 action 1, 0x43faf5..0x43fb26) is VTOL_LANDING
  -- Cmd::Land (protocol 340, World::landOnAirBase). VTOL_GetRepaired 0x415250
  is dead in retail (no code refers to its row 0x4fcc0c or handler, no
  "VTOL_GETREPAIRED" string for 0x438760, no index path through 0x438b90; the
  shipped FBIs name only Standby, VTOL_Standby and GUARD_NOMOVE) and is not
  ported. The pad pick (0x40b530) walks the AIPlayer record's pad list (+0x25),
  which 0x40aa40 rebuilds every 30 ticks per player (0x40b2c0, +0xed from 0) in
  slot order (World::refreshAirPadCaches), now in the player loop after the
  units (0x495490: units 0x48ad30 at 0x4954ed, then 0x464f80 at 0x49555f ->
  0x40b2c0 at 0x465037), from the unit slots (below).
- Unit slots (2026-11-24 audit): a new unit takes the first free record of its
  owner's block (0x485fea..0x486051, a record whose type word +0xa6 is 0), the
  death frees it (0x486dc7); the path scheduler walks the block, holes
  included, at 1 a visit and skips free records (0x40eb70: 0x40ed0c ..
  0x40ed33). World::allocateEntitySlot does exactly that, with no random draw
  (Kingdoms' 5121d8 CRT-ranked free slot, freed at retirement, is gone; the
  air pad list reads the same table). The restored Kingdoms replay fixtures
  (tools/retail_replay_probe) keep their own fixed pools.
- SUPPRESS (2026-11-24 audit; World::suppressStage): TotalA.exe's ground row 4
  (0x4038a0). The head ends it on a commandfire shot (0x800); stage 0 refuses
  a flyer (8) and takes slot +0x36's range as the reach; stage 1 makes slots
  0 and 1 manual and points both at the ground (slot 2 alone when +0x36 is 2,
  0x40396b..0x4039c2), wait 0x1c00 (TA's shot 0x400, commandfire shot 0x800,
  no aim 0x1000 -- 0x49e4f2..0x49e50b, 0x49d664; World's 0x10000 / 0x40000 /
  0x100000); stage 2 makes every slot automatic, goes to the back of the list
  after a shot (6), else ends with no mover or no reach left (9), else walks
  a circle of radius reach on the point (0x438930) and takes rand(range / 3)
  off the reach (0x40392e..0x403957). The resolver (0x43f17e..0x43f1a9) reads
  slot 0's weapon only: toairweapon gives no order, a flyer the air branch.
  Kingdoms' SUPPRESS (melee walk to a feature, the feature target, the
  minimum-range ring, the weapon-switch event) is gone with its click rule.
- No area orders (2026-11-24 audit): neither mission table (0x4fc6e8,
  0x4fca18) has a RECLAIMAREA (or any area build), so Kingdoms' RECLAIMAREA
  (406670: World::reclaimArea / tickReclaimArea, the right-drag box's step C
  and the armed CLEAR box) is removed; Cmd::ReclaimArea and Cmd::BuildManaArea
  keep their numbers but the server refuses them and the sim no longer applies
  them.
- Transports read TA's keys (0x489a90: passenger footprint x <= transportsize,
  count < transportcapacity, asked when LOAD is given). LOAD is the carrier's
  order (0x43f701 -> 0x43fb61): GROUND_PICKUP / VTOL_PICKUP for the carrier and
  no mission for the passenger (TA has no Move_Seek_Pickup; World::loadInto).
  GROUND_PICKUP (0x406780; retailGroundPickup): size test, BUSY wait
  (0x438730), a counted TransportPickup call + sleep 15, BUSY wait, aboard ->
  5, third failed call -> 9, else a radius-0 circle on the passenger (wait
  0xe8), controller off and round again. VTOL_PICKUP (0x4111b0;
  retailVtolPickup), VTOL_UNLOAD (0x411560; retailVtolUnload), GROUND_UNLOAD
  (0x406900; retailTransportUnloadApproach / retailSurfaceUnloadTransfer). The
  attach (attachCargo, 0x48ab70 + 0x4384a0) puts the passenger at the head of
  the carrier's list, clears its orders and makes its slots automatic
  (BeCarried 0x402fc0); the release (releaseCargo, DROP_UNIT 0x4813b0) orders
  nothing. The carriers' own scripts run: ATTACH_UNIT / DROP_UNIT (0x4b1b92 /
  0x4b1b7d -> 0x481340 / 0x4813b0) and TA's GET 6..16 (0x480770); SET 6 writes
  BUSY and every SET posts event 4 (0x480b20); GET answers ids 1..20 only
  (0x480781 -> 0x480abb).
- RECLAIMUNIT (World::reclaimUnit / tickReclaimUnit, sim/retailreclaimunit.h,
  Cmd::ReclaimUnit protocol 340): ReclaimUnit 0x404730 (ground row 20) and
  VTOL_ReclaimUnit 0x414a80 (air row 16), from the resolver 0x43f0e0 (action
  12, 0x43f6aa; action 1, 0x43fa13); stage 0 refuses an airborne flyer or a
  cancapture type (0x489960); RepairUnit's rectangle (0x438ad0); a kind-5
  record of 0x438650's rate every 16 ticks within builddistance + the
  target's +0x184; the death pays (1 - remaining) x buildcostmetal
  (0x486c9e..0x486d1e).
- Mission dispatcher (0x43b7c0, retailDispatchMissions): code 3 sleeps
  30 + rand(15) (0x43b890) and code 9 sets mission flag 0x800000 and, alone,
  sleeps 30 + rand(30) (0x43b8e5..0x43b973). Its event 0x10000 test (0x43b840:
  TargetCleared on slots 0..2) is dead in TotalA.exe: the unit event word
  +0xba is 16 bits, the navigator posts only 0x20..0x200 into a mission's
  +0x4e (0x44ced0, nine callers), the dispatcher sets only bit 0 (0x43b7f9),
  and every other write masks it (0x438915, 0x4389b1, 0x438a8a, 0x438b4c,
  0x4110b1) or copies a mission record (0x43a64d). World posts none either.
- Resurrection follows TotalA.exe RESURRECT (0x404db0): any reclaimable feature
  (0x440043, flag 0x80) whose name up to its first '_' names a unit type
  (0x404f36 -> 0x488b10); timer ftol(buildtime x 0.3 / (workertime/30)); the
  unit comes back finished at 1 HP (0x405215) and the caster's REPAIRUNIT on it
  goes to the head of its queue (0x405243..0x40529f, 0x43acb0); a full unit
  cap retries in 300 ticks (0x405119).
- Projectiles: line-of-sight, selfprop/vlaunch and dropped shots follow
  TotalA.exe (spawners 0x49c9c0 / 0x49cc20 / 0x49dd60, motion 0x49b9ae;
  `sim/retailtashot.h`, `World::tickTaSelfProp`). The bomber's AIRSTRIKE run
  follows 0x411f50 (retailAirStrike).
- Shot timing and body test (2026-11-24). World's tick runs the projectile
  pass after the units, as TotalA.exe's 0x495490 does (0x4954ed units, whose
  0x48adda -> 0x49e1a0 fires; then 0x495513 -> 0x49b720 projectiles): every
  shot moves and is tested in its fire tick, against the bodies where the
  units' pass left them. The spawners set +0x42 to the fire tick and +0x46
  to now + lifetime (LOS / selfprop 0x49cb5f..0x49cb65, vlaunch
  0x49ccf6..0x49cd17, ballistic and dropped from the fire tick); burst clones
  join after the pass's count (0x49b720 reads it once) and move from the next
  tick. The ground bodies and the flyers (word +2, "Flyer cell grid"
  below) are met where the units' pass left them.
  Every shot kind tests a point with 0x49b090 (World::projectileCollision):
  off the map gone (0x49b0ac), the cell's body (word +0) when not the shot's
  owner's and the point is below its y + model height (0x49b1c7..0x49b211,
  no outline: the Kingdoms selection-quad test, projectilePointInUnit, is
  deleted), the cell's flyer (word +2) from its y + type +0x162 (0) to its
  y + model height inclusive (0x49b222..0x49b275), then features, water and
  ground. Evidence: ta_shot (a shell, a rocket and a bomb have moved one step
  when their fire tick ends; a tall 2x2 tower and a low 5x5 solar struck at
  their corner cells under their tops; an LLT laser and a rocket at a
  walking corak burst in it where it stands that tick), ta_beam,
  ta_ballistic, retailgap_weapons, scenery_damage; legion baselines retaken
  for air-stack-duel, air-stack-barracks and air-stack-barracks-5 (their
  fights land a tick sooner).
- Beams and the other line-of-sight shots (2026-11-24) fly as TotalA.exe's
  (rule 2 of 0x49b9ae; `World::tickTaLineOfSight`, `taLineOfSightStep`): the
  head moves one velocity step a tick until the expiry (range << 16) /
  weaponvelocity (0x49cb17), then the shot is gone without a burst
  (0x49bc8b); a beamweapon's tail starts at the muzzle (0x49c761) and moves
  from the second tick after the counter passes launch + `duration`
  (0x49bbe1..0x49bc07; most lasers' duration truncates to 0, so they fly as a
  two-step streak; the Zeus's 10 s keeps its bolt joined to the muzzle). Only
  the head is tested, once a tick (0x49bd86 -> 0x49b090): the enemy body
  owning its cell, when the point is below that body's y + model height
  (0x49b1c7..0x49b211, no outline test), a flyer there, or the ground bursts
  it (damage on contact, 0x499eb0); own bodies and anything below it pass. noexplode (the D-gun)
  deals its damage every tick it meets something and flies on (0x499ede).
  The client draws
  rendertype 0 as 0x49bf3a..0x49c097 does (color, and color2 a pixel off the
  major axis, through the guipal -> palette.pal map 0x4ac7d0) and rendertype
  7 as 0x49c48c's two jittered strands of 5 px steps (`client/tabeamdraw.h`,
  `GameView::drawTaLineShot`). Evidence: ta_beam, beam_fx (debug).
- Flyer cell grid (2026-11-24; World::stampAirOccupant,
  RetailAirOccupancy::taStamp / taUnstamp). TotalA.exe writes cell word +2
  from the position setter 0x48a9f0, which the mover's commit calls inside
  the unit's own update (0x43d6d0 -> 0x43d775, and 0x43da0e): when the
  footprint origin ((x - footx << 19) + 0x80000) >> 20 or the layer
  (+0x110 & 3, 2 = airborne) changes (0x48aa35..0x48aa56), 0x47d0e0 clears
  the recorded cells still holding the unit's index (0x47d23b..0x47d284;
  skipped after an off-map stamp, +0x82 = G+0x142b7) and 0x47cc30 stamps the
  new ones (0x47cf98..0x47d042): off the map nothing, an empty cell takes the
  unit, an occupied one keeps its flyer unless that flyer's owner is remote
  (player +0x73 == 3, 0x47cfda..0x47cfe9; every owner is local to the
  lockstep simulation, so the first flyer keeps it). Creation stamps
  (0x48610f, 0x48630c) and death unstamps (0x48682d -> 0x47cbd0). World
  stamps each flyer as its update in the units' pass ends, at its spawn and
  at its death: the units after it, the landing test and the projectile pass
  meet it where it now is. No overlap lists and no random owner (Kingdoms'
  506c40 / 507050 rebuild and hand-over, with their gameRand draws, no longer
  run in a TA game). retail_motion's overhead-flyer case spawns the flyer
  overhead first, as 0x47cc30 lets the first stamp keep the shared cells.
  Legion baselines retaken for the air-stack fixtures and air-guard-lost.
- Combat sight test (2026-11-24; World::combatLineOfSight,
  NavGrid::obstacleLosBetween). The engine's 2D line test for line-of-fire
  weapons (an engine deviation: TotalA.exe admits a shot on range alone)
  takes only the obstacle overlay (buildings, blocking features, wrecks) as
  a wall, no longer the sight grid's terrain: a slope too steep to climb,
  a cliff face or deep water is a movement limit, and terrain that does stop
  a shot is found by its impacts (noteShotImpact / repositionForShot, which
  test the 3D chord). TotalA.exe's Attack_Chase (0x4034a0) holds and fires
  once 0x49abb0 says the target is in range (stage 1, 0x4035d0..0x40360b);
  World's sight test had kept a ground unit with a laser in range of a
  target on a plateau walking at the foot of the slope, never firing. That
  was watch_end's all-AI stall: after the shot-pass change its stress game
  ended with ~73 Core units below Arm's last units on the Ashap plateau,
  299 of 338 in-range samples of the line-of-fire ones refused for no line
  of sight; it is decided now (tick 49687). Evidence: blocked_shot
  (plateau: a Can stops at its range below a 50-high step and hits the
  Peewee above it within 15 s), naval_combat (a ship behind raised land puts
  three shots on it, which write the target off), watch_end; legion
  baselines retaken for attackring-river-64 (the bank strip fires across the
  river: damage was 0; fewer ever reach the ring, licensed).

## Open

Classes: (a) Kingdoms gameplay still running in a TA game; (b) a retail detail
approximated (marked "cosmetic" when it changes no simulation state); (c) a
test or fixture note; (d) Kingdoms code still compiled but unreachable from TA
data (cleanup, no gameplay).

- (a) None left in the shot paths. The fire gate's out-of-range bit (unit+0xbb
  0x10, set by 0x49aa80's refusal at 0x49e53a and by 0x49d664) is not kept.
- (b) GROUND_PICKUP's circle and GROUND_UNLOAD's wait also wake 60 ticks after
  the hull stops (flag 0x80000000, retailtransport.h). Retail's wait has no
  timer: the navigator's failure (0x40) after its empty 60-tick re-ask wakes
  it. Tried removing the stand-in (2026-11-24): the engine's navigator then
  never fails a radius-0 circle on a standing passenger, and transport,
  transport_roster, transport_post_unload, transport_map_roundtrip and
  legion_transport stop boarding. The difference is in that re-ask's result,
  not in the missions.
- (b) Ground RepairPatrol (0x405980): stage 1 re-installs a radius-16 circle
  on the waypoint at every 60-tick wake (0x4059c5..0x4059e1) and moves on at
  any navigator event (0xe0 -> 6); World keeps its patrol navigation and runs
  the look (tickGroundPatrolLook) on a 60-tick timer. VTOL_RepairPatrol's job
  search runs on a 45-tick timer from tickPatrolRepair, not its stage-1 wake.
- (b) Flyer take-off climbs not modelled: VTOL_RepairUnit / VTOL_Reclaim /
  VTOL_RepairPatrol's jobs (0x414fe1, 0x41485e), AirToAir / SeekAttack
  (0x4133fa, 0x4107c6), AIRSTRIKE's stage-0 wait for take-off altitude; nor
  LandIfCan's off-map return (0x40f2e3) or AIRSTRIKE's stage-6 pad detour.
- (b) AirToGroundHover at a point is ours (TotalA.exe's stages read the target,
  +0x16; World runs them on the point); its wait mask's event 0x1000
  (0x413996) is not posted. Two misses of 0x49abb0 build a controller that is
  never installed (0x41391e), so only its rand(0x10000) remains.
- (b) Hit response approximations: the retaliation's category / reach tests are
  World's retailAnswersAttacker / slotHitResponse (0x49abb0's underwater tests
  and the +0x23d / +0x231 category sets are not traced one by one); the old
  object of a change of owner (kind 4, 0x4886a4) runs no response; the weapon
  path lowers the HP before the response (0x489ce0 responds first).
- (b) Mission dispatcher: the second mission list (+0x60, flag 0x40000) and
  the 0x800000 flag's reader (the navigator, 0x44f3e9) are not modelled; TA's
  loop has no call cap (0x43b99f), the engine keeps 100 as a guard.
- (b) Flyer cell grid (see Ported): World stamps a flyer as its update
  ends; TotalA.exe stamps it inside the mover's commit (0x43d775), so the
  unit's own later steps in that update (its weapons, 0x48adda) run with
  the stamp already moved. Nothing in a unit's own update reads its own
  stamp. The stamp's overlap flags (+0x110 0x4000000 / 0x8000000,
  0x47cfeb..0x47d02d) are not kept: no TA reader is traced.
- (d) RetailAirOccupancy's Kingdoms passes (insert / clear / remove, the
  overlap lists in Unit::airOccupant's count / links) are left for
  retail_visual_test's Kingdoms fixtures; World no longer runs them.
- (b) Shot pass leftovers: 0x49b090's interceptor test (+0x56 target shot
  within weapon +0xd6, 0x49b106..0x49b1a9) bursts and goes on to the body tests; World's
  optional targetShot returns at once (no TA shot passes one). The Kingdoms
  kinds left in the pass (guided3d without taSelfProp, straight, flames,
  the 2D fallback) keep their own launch delays and are tested by the same
  0x49b090; of TA's weapons only a non-turret ballistic one reaches them
  (the Core Contingency mines' ARMMINE* / CORMINE*, not traced through
  0x49e1a0's dispatch). The lightning
  strands draw with the client's own CRT-rand stream (0x4e4870 is the
  game's shared one; drawing only). The D-gun (rendertype 3) draws its model
  at the head through the generic model path; 0x49c1f2's own pose words
  (+0x38 local) are not traced. The cruise branch of 0x49b3e0 is ported untested; non-turret LOS weapons compare only the
  heading with the body (docs/retail-weapon-scripts.md 7.8). The range gate
  0x49aa80 measures the horizontal range to the slot's target point (the
  SweetSpot, 0x49e3c8); World keeps the callers' test to the target's origin
  for that part and runs the surfaced and arc tests in the fire path.
  A target below a ballistic weapon at short range is aimed with acos's
  unsigned angle (0x49a9ce): the shell goes up and long, as in retail.
- (b) Transports: air cargo rides the carrier's own point, not the link
  piece's (VTOL_PICKUP's engine attach passes piece -1); BeCarried's 10-tick
  poll after the drop (the default mission comes at once here), the flag-4
  missions BeCarried keeps, the air base exemption (flagsA bit 9),
  VTOL_PICKUP's event 2 / 0x10000 bits and the unit-follow controllers' exact
  altitude rule (0x44e190 / 0x44e250) are untraced. GET 20 ARMORED answers 0
  (SET 20, 0x48b090(2), is not stored). An unload given while
  TransportPickup still swings the boom races it for the same pieces, as in
  retail.
- (b) Death accounting, network only: the remote-owner branch of 0x486f10
  (0x49b000 + 0x4864b0(3) for +0x73 3) is folded into the record, and the
  kind-3 loss's statistics guard (player +0x129[owner], 0x457540) is not
  modelled -- a lockstep game has no remote owner.
- (b) The pad list's rand(30) (0x40b301: 0 rebuilds the AI's type tables,
  0x409730) is drawn from the AI's own stream (TaRetailBrain::random), not the
  sim's: retail draws the whole AI on one host, so its interleaving with the
  sim stream cannot be reproduced (docs/retail-ai.md "RNG in lockstep").
- (b, cosmetic) Nanoframe: retail skips primitive 0, not the selection
  primitive, whenever a piece has one (0x459aa5, 0x45908a, 0x458568; pieces are
  not reordered at load, 0x4cb4c0), so 24 unit models (armca, corlab,
  cortitan...) draw their selection quad and drop a textured face in retail;
  the client keeps skipping the selection primitive. Retail draws a textured
  primitive only as a quad (0x459b1a, 0x4585e2); the nanoframe does, the
  finished-unit draw does not.
- (b, cosmetic) Repair pads: the pad piece's x/z turns go to +0x64 / +0x68 but
  which entry word is which axis is not traced, and the sim does not set the
  rider's own heading.
- (b, cosmetic) Unit speech: the queue's global spacing (+0x9d / +0xa1,
  0x47f6bb) is not modelled (one request per frame, text at once);
  RECLAIMUNIT's "Reclaiming" status line, its speech lines (0x47f780) and work
  sound (0x47f780(unit, 0xb)); the cursor over a raisable feature shows
  reclaim. The display VM's own GET 1..19 read its snapshot (ACTIVATION from
  the build queue, BUSY from motion) rather than the sim's state.
- (c) A PARK round a *mobile* producer (a test fixture; no TA unit builds
  mobile units in the field) can still circle in the ring's acceptance gap --
  the octile distance (0x44d840) is 0 a cell inside the squared inner test
  (0x44d800) -- when parked outputs wall the band. The client watch/spectator
  harness checks (gameview_devharness.cpp) still spawn Kingdoms units. A
  script-less (test) carrier keeps the old transport stand-in (fifteen ticks
  of BUSY, the static BoomCalc reach). A hover pinned by a cliff with its crane
  astern can stand out of reach (the roster fixture moved hovers to open land).
- (d) Kingdoms code compiled but unreachable from TA data: removed 2026-11-24
  (step 3), with these leftovers kept on purpose:
  - The line-of-sight status branch (Weapon::losStatus, never set by
    taweapons.cpp; tickStraightProjectiles' status update, the launch and
    burst-emitter tests on it, World::statusShotRefused now a constant
    `false`): it sits in the line-of-sight shot path another change was
    rewriting at the time; drop it with that path. Likewise the faction
    nimbus (Weapon::nimbus / nimbusTicks, UnitType::hasNimbusArt, the client's
    factionNimbus), which no TA weapon sets.
  - The group checks' structure (World::retailGroupOf, always null; the
    formation-level legs and retailgroup.h's check helpers that Legion's own
    formations and retailMissionFlags still call): only the pacing (51b890 /
    4d95f0) went.
  - Player::mana / income / storage, UnitType::income / storage: the engine's
    single-resource pool, mirrored from TA's stored metal (taeconomy.cpp),
    hashed, read by the scenario resource conditions and the HUD, and the
    economy the synthetic-type tests run on; not Kingdoms gameplay on TA data
    (every TA type's income and storage are 0).
  - Unit::weaponSlot stays (hashed, always 0).
  - Kingdoms names in the client's old HUD (gameview_hud.cpp order column, the
    Kingdoms side conjure/wave art names in gameview_render.cpp) and unbuilt
    tools/ sources not in CMakeLists.txt (scenario_stats_test, owner_change_test
    and others): display-only or dead files, not reached by a TA game.

## Resolved (2026-11-24 audit)

- corbats at 1110 px: not the shared-aim signal (its aims use signals 2 and 4).
  Its cannon (cor_bats, 350 px/s) has no arc past ~1094 px on level ground at
  gravity 112, so the range gate 0x49aa80 (ballistic -> 0x49a890 = 0x8000)
  refuses it: retail behaviour.
- Legion-mode air attacks (air-stack-barracks / -5): waiting_no_progress
  135 / 225 is the landed Drakes' (armfig) take-off climb, not a Legion
  conflict. AirToGround's stage 0 (0x4128b5 -> 0x43d210(2)) climbs over the
  spot to cruisealt / 2 before stage 1 moves off, so each Drake stands at its
  spawn x/z from the order (tick 2) to tick 57-58 (dump: every still run of
  the five is 0..57), i.e. 45 unit-ticks counted as waiting (stillRun 10..150)
  x 3 / x 5. Retail and Legion modes give identical keys and hashes (flyers do
  not use Legion's ground mover or stacking), and VTOL_Move's identical
  take-off already shows in air-stack-move (498). The movement keys
  (sideways, backward, back, reversals) are the run and the 90-degree turn
  passing over the target.
- The AI's group orders (0x480460) give every member the same point: the
  retail brain issues one Move / Patrol / Attack per unit to the group's
  point (ai/taretail.cpp), with no spread shape.
- Stale entries removed: the nanoframe outlines (client/nanoraster.h
  rasterises per pixel), the result plate's Kills / Losses (countTaDeath
  follows 0x486892), the under-attack speech line and options (ported, see
  "Closed in the final pass").
