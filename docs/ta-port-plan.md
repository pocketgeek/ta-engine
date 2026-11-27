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
| 3. Kingdoms gameplay stripped | done, audited 2026-11-24: every `Kingdoms` / `KINGDOMS.icd` / monarch / mana / Crusade hit in src/sim and src/ai checked (TA-correct, or a kept engine feature); the Kingdoms rules still running in TA games replaced from TotalA.exe -- unit slots (first free record 0x485fea, freed at the death 0x486dc7; was 5121d8's CRT-ranked slot), SUPPRESS (0x4038a0; was 4040b0's melee / feature / minimum-range ring), the area clear order (no RECLAIMAREA row in 0x4fc6e8 / 0x4fca18: removed), death severity (0x4864b0 for every kind; was 512610's), the air pad list's rebuild after the units (0x495490 -> 0x464f80 -> 0x40b2c0); gods, auras, sacred sites, mana and monarchs went earlier. The ballistic shell, the last Kingdoms rule, is TotalA.exe's since the second pass (2026-11-24): AimX's pitch solve 0x49a890 (equal to the binary on 63 emulated cases, tools/re/ballistic_emu.py), the range gate 0x49aa80 in the fire path, the launch 0x49cde0 on the slot's AimX angles plus the spread with the barrel word (0x49e11b, 0x49ce6a), and the motion 0x49bce3 (move, the wind vector G+0x37ecc = 0x490cf7's, then gravity; once a tick); burst clones copy the emitter (0x49b83b). Kingdoms code left unreachable: the Classic brain and the engine's old Controller levels are deleted; the third pass (2026-11-24) removed the rest that TA data cannot reach: the stone / freeze / mind-control statuses (Unit freeze and stone timers and statue corpses, the charm and its capture gates, cantbestoned / cantbefrozen / cantbecaptured, the client's statue meshes and PETRIFIED / FROZEN labels), the Remote Effect and Wandering weapon kinds and their fields, cananimate / animatetype (Resurrect raises only the body's own unit), weaponswitching (World::setWeapon, the AI weapon switch, switchreloadtime, the HUD weapon buttons and Cycle Weapon / Select Melee / Select Magic keys; Cmd::SetWeapon is refused, its number kept) and melee weapons (footprint contact, Attack_Melee's approach and retarget, the cursor's melee range), the lodestones (onMana, the in-place upgrade, the sacred-site yards and income, mogriumbounty, the lodestone box and the order trail's deposit sites; the ore-patch list is World::depositSpots), the group records' pacing (51b890 / 4d95f0), Kingdoms' experience routines (experiencepoints, getLevel / setLevel, noveteran: TA's veterancy is the kill count) and the Kingdoms side filters in Cartographer. The authored veteran rank became TA's kill count (2026-11-24, owner decision): a placement, a type's scenario stats, Cartographer's unit field (now VETERAN KILLS) and a mission's engine-only `v F` set Unit::kills, rank N = N x 5 kills (.crt 2.0 holds kills; a 1.0 file's rank converts on load and writes back unchanged; `v F` names rank round((F-1) x 10)), so TA's own rules read it -- damage taken 0x489bf3, the lead past 5 kills 0x48a324, the reload 0x49e468; vetMul / vetOverride / the rank levels, the veteran shot and unit models, the HUD rank mark and the script-less reload's veteran divisor are gone (stateHash folds a constant where they went). A mission's [units] `Kills` key is no longer read (TotalA.exe's parse 0x436c30 has none; 1100-odd shipped records say 1..66). Removed fields that sat in the Unit layout are pads; stateHash folds a constant where a folded field went. The single-resource pool and the restored Kingdoms construction / Classic AI World hooks went in the 2026-11-25 pass (see Resolved), with site decay ported to TotalA.exe's GetBuilt (0x402da0 / 0x41bcd0). Evidence: suppress / legion_suppress, retail_trace (slot pools), pathblock, transport*, click_rules, revive_order, server_commands, ta_ballistic, ta_weapon_script; legion baselines retaken for the slot order and the shell motion (tools/scenarios/baseline.json); authored_kills (rank 2 = 10 kills, 8% less damage, the lead; .crt 1.0 conversion; Cartographer round trip) |
| 4. GUI runtime, front end, sidebar (`retail-gui.md`) | done: MAINMENU, SINGLE, SKIRMISH, SELMAP, SELPROV/TCP, NEWGAME, MSNBRIEF, ENDMSN, in-game frame laid out as retail (side panel left, minimap top-left, top and bottom bars with sidedata readouts, game view (128,32)-(W-1,H-33)); factory build pages queue production (0x41aa00) |
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

Animation call-ins audited for ground and naval units (spec section 9, 2026-11-26):
Create / SetMaxReloadTime, StartMoving / StopMoving / MoveRate1..3 (0x43da70), Aim / Fire /
Query / AimFrom / SweetSpot / RockUnit / TargetCleared, Killed, SetSpeed / SetDirection.
Fixed: HitByWeapon gets TotalA.exe's two arguments (400 cos h, 400 sin h) and only for a
surviving, non-paralyzer hit (0x489efa..0x489f43; was Kingdoms' damage type / cos / sin /
damage, which tipped hit vehicles sideways); the display VM's AimX / RockUnit get whole
angle words (0x49e2da, 0x456200; was the high byte); a ship in an attack no longer pivots
to face its target (Attack_Chase 0x4035b7 posts no heading; turrets aim). Evidence:
ta_weapon_script sections 7 and 8, naval_combat. Still open: COB pass before the weapon
update (one tick late), aim only in range, and the engine's line-of-fire tests
(slotClearShot, blocked-shot write-off) that TotalA.exe does not have (0x49aa80).

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
  child is TA's own feature reclaim. Not dead code; Cmd::RetiredAreaBuild stays
  refused.
- Per-unit destinations (owner's requirement, kept 2026-11-25, both pathfinding
  modes): a player's group move, fight-move or patrol gives every unit its OWN
  spot, and the client outlines each unit's footprint there in player colour
  (the "blue box" markers, GameView::drawUnitSpots / UnitR::hasSpot), flashed
  after the order and held under Shift. sim/groupspots.h: a unit within TA's
  reach (the selection order 0x48cf30: the click plus its offset from the
  centre when within sqrt(3000 n) px) keeps TA's goal when that footprint is
  free and on the map; every other unit -- where retail sends it to the click
  itself, so several share the point and no marker shows -- takes the free
  footprint spot nearest the click (16 px grid, nearest units first). Ground
  units and flyers alike, and fight-move ground units too (retail: the click).
  Legion keeps its own slots for the surface movers it plans
  (legionSharedClick: the click, slotted on the shared point; the marker is
  the slot); its flyers and oversize bodies take groupSpots, keeping out of a
  room round the click held for the planned ground (was: TA's spread for the
  flyers, +-60 px per axis for the oversize). Network-safe: in a Retail net
  game the client's per-unit commands carry the spots; an explicit order
  (OrderShape::Offsets / FlyerOffsets) is re-spread by the sim
  (applySelectionOrder) from the members' positions at activation, integer
  16.16 only. The AI's group orders (0x480460) still give every member the
  same point. Evidence: group_spots_retail / group_spots_legion (every unit of
  a move, fight-move and patrol has its own destination; groupSpots' own
  rules), unit_spot_markers / unit_spot_markers_retail (debug harness: 22
  units incl. far stragglers and flyers, 22 distinct spots, 22 outlined).
- Alt+N formations (owner's requirement, kept 2026-11-25, both pathfinding
  modes): Alt+N puts the selection in formation N (Cmd::SetSquad, Unit::squad
  -N); its members walk at the slowest member's speed (a member behind the
  centre catches up at its own), its flyers fly at it too (World::flyerPace_
  in tickFlightBody; in Legion only a formation without ground members, the
  mixed ones keep Legion's flight stations), and an idle straggler of a
  resting formation rejoins the centre. TotalA.exe has no formations (no
  formation mission or group record, see "Groups and air traffic" below);
  Retail mode used to leave Alt+N inert after the Kingdoms group records went.
  Legion additionally keeps its formation slots (legionFormationGroup,
  legionStandbyCheck). Evidence: group_spots_retail / group_spots_legion (six
  walkers at 70 and 35 px/s, or flyers at 240 and 120: loose they arrive
  1047..2164 / 341..687 ticks, as an Alt+1 formation within 1 to 30 ticks of
  each other at the slow units' time; an idle straggler 900 px out walks
  back), movement_orders formation gait.

- Alt+number formations (owner's choice, kept 2026-11-25): Unit::squad < 0
  formations, Cmd::SetSquad, the pacing at the slowest member and the
  stragglers' rejoin, Order::formationLevel / attackFormation and the formation
  legs (retailGroundFormation / retailFlightFormation), Legion's formation
  slots, and the per-unit destination spots and their markers. TotalA.exe has
  no formations; nothing in a retail game builds a formation leg.
- Decay of an unfinished site's last refund: the refund a decay step puts on
  the site's own metal accumulator lands at its owner's next settle; one the
  site still holds when it dies goes with it (taSettle sums live units; how
  retail's settle treats a dying unit's accumulator is not traced).
- Auto-share switches start off: TotalA.exe's thresholds start at 0
  (0x4647b3); the initial value of the player info +0x97 bits the console's
  ShareMetal / ShareEnergy toggle is not traced.

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
- Nano spray, rechecked (client/retailnano.h; tools/re/nano_emu.py emulates
  the spray object; nano_spray test). Each spraying wake makes one object on
  effect list 6 (0x4720d0, reverse 0x472200; at most 401, 0x47219f) that emits
  5 particles at once and 5 more on the next tick (0x471d70(1), 0x472f60) --
  10 a tick from a builder, not 5. A particle runs from the QueryNanoPiece
  point (0x43e400: position + piece origin, true 3D) to a random point of the
  middle 3/11 of the target's box in floor(len/4) ticks and steps a whole tick
  at a time; it is one world pixel, opaque, and its colour steps 0xa1..0xa7
  round every tick (0x4739b0; was a fixed green per particle). Boxes: a unit's
  position +-footprint*8 and 0..model height (no 8 px floor); a feature's
  footprint from its corner cell, from the ground there up by its TDF height
  (0x404ceb..0x404d37; was a 16 px cube). Cadence follows the sim's spraying
  wakes (Unit::constructionEmissions[0]): build/repair each tick (the old hp
  test caught one tick in three), ReclaimUnit one 0x472200 per two-tick wake
  (0x404a4e; was not sprayed), feature reclaim two per wake while the
  countdown is above 15 (0x404cb5..0x404d4c), RESURRECT each channel tick
  (0x40509d; was not sprayed); a factory's own BUILD (0x402abd) and the pad
  still read progress / hp. Each dot is shown by the LOS of its projected cell
  (0x473a7c..0x473b05), and list 6 draws after the whole ground pass
  (0x469b18), before the flyers: dots no longer hide behind units. Capture
  (0x404676, reverse, every 2 ticks) waits for a CAPTURE order. Harness:
  TA_CONJURE_TEST (+ TA_CONJURE_RECLAIM=feature|unit) with --shot.
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
  (client/commanderalert.h): any own unit hit while off screen (0x4071d8 ->
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
  World::retailGroupOf is always null, so Kingdoms' group checks, formation
  legs, standby re-forms and group-record pacing never run. The engine's own
  Alt+N formations (pacing to the slowest member, the rejoin) run in both
  modes: "Known deviations".
  A multi-unit order spreads its goals once, at issue (the selection order
  0x48cf30): every selected unit but the target counts in the centre; a unit
  whose mission row has flag 0x2 (Move_Ground, VTOL_Move 0x402; Patrol,
  VTOL_Patrol, the repair patrols 0x412) takes the click plus its offset from
  the centre when within sqrt(3000 n) px, else the click -- ground units too.
  The engine keeps that offset for the units within reach and gives every
  other unit its own free spot instead of the click (sim/groupspots.h, both
  modes; "Known deviations"). The AI's group orders (0x480460) give every
  member the same point.
- Flying builders' hover: VTOL_MobileBuild (0x413d80) and VTOL_HelpBuild
  (0x414380) approach the site with radius builddistance (0x413f60, 0x4144ed),
  then on every 150th game tick re-pick a point builddistance from the site at
  the builder's bearing + 0xdb6e, facing it (0x414149..0x4141f9,
  0x414569..0x41461a; retailTaBuildHoverGoal); until the first such tick they
  hold where they arrived. VTOL_RepairUnit (0x414e70) hovers over the target's
  position from its stage 1 (0x415048) and repairs under it (0x415098).

- Ground construction (2026-11-26, assist_reach_test): HelpBuild (0x403f70)
  approaches on its ring within builddistance of the site (0x403fd9, 0x44d3b0;
  retailHelpBuildRing, Order::helpRing) instead of Kingdoms' rectangle round
  the footprint, ends on a failed approach ("I can't get there", no reach
  fallback), and calls StartBuilding then waits for INBUILDSTANCE before its
  first work (0x4040d4, 0x4040ee); MobileBuild does the same after placing
  (0x403de1, 0x403df5). Neither turns the body any more. A builder held at
  its job still reports its movement rate (StopMoving: ARMCOM's StartBuilding
  waits for its legs, MotionControl static 2), and a MobileBuild site counts
  as begun only once the stance is up (our site's GetBuilt timer was woken by
  the walk's site-work notes; retail's sleeps 300 ticks from placement). The client's ground
  build stance follows the sim's StartBuilding/StopBuilding
  (Unit::workScriptWorking) and a builder's nano sprays only on its own paid
  steps (constructionEmissions), so a helper walking to a factory's output no
  longer sprays at it from across the map (the output's progress was the
  factory's work). Open: retail's HelpBuild walks on to a site finished
  meanwhile and ends on arrival (ours ends at once); Follow_Ground (0x406300)
  also copies its ward's ReclaimUnit (flag 0x100000 with a target, 0x4065e7)
  and follows on a circle round ward + offset with radius +0x36/2, re-aimed
  every 30 ticks (0x4066ac..0x4066e9), where ours keeps Kingdoms' ring and
  15 + rand(15) look; RepairUnit's ground handler is TotalA.exe 0x405300
  (ours cites Kingdoms' 0x407190: unverified).

## Ported details (formerly listed as Open)

What these entries describe is ported; what is left of each is under "Open".

- Stockpiles and interceptors (stockpile_test). The build menu's MAKENUKE /
  MAKEANTI button (0x419b00: left +1, right -1, Shift 5; Cmd::Train /
  Unqueue with that name, matchsetup stockpileButton) gives BUILDWEAPON slot
  0 through 0x43b0b0 (joins the last BuildWeapon on the slot, else appends;
  a negative count takes back from the last ones). BuildWeapon (0x402b70,
  row 0x4fc571 "Nanolathing", flags 0xc0140, no flag 4: a plain order clears
  it) is ScriptedOrder::Kind::BuildWeapon: stage 0 ends at count 0, sleeps
  300 at 200 stock (0x402cca), else stage 1: five more ticks of reloadtime
  (+0xe4), metal/energy step trunc(next*cost/reload) - trunc(prev*cost/reload)
  requested from the unit's books (0x4011c0; refused: 10-tick sleep, else 5),
  stage 2 stock++ (S+0x1a, Unit::stock), count--, back to stage 0. A stock
  takes (ceil(reload/5)-1)*5 ticks (armsilo: 5395 for 180000 E / 2000 M).
  Fire: a stockpile weapon needs stock (0x49e3e2; vlaunch aim too,
  0x49e33d), spends one (0x49e455), sets no reload and pays nothing
  (0x49e463 -> 0x49e4f2, 0x49e518). The build button shows "<stock> +<queued>"
  (0x4199b0, 0x439d80). Interceptors: the fire sweep (0x408b38..0x408b5c)
  asks 0x49d120 for an interceptor slot -- stock in the slot; the first live
  shot of another player, targetable (bit 29), whose target point (+0x28/+0x30)
  lies in the unsigned coverage square of the unit, chased by no shot -- and
  sets the slot's point to that shot's head (0x48a0a0); an auto interceptor
  slot keeps that point (syncSlotMode). The vlaunch routine scans again
  (0x49dc17..0x49dc36; none: no shot) and the spawner hands the quarry over
  (+0x56, 0x49cd25; Projectile::targetShot by serial). The rocket steers at
  the quarry's head (0x49b47a), bursts within its areaofeffect of it
  (0x49b106..0x49b1a9), and its burst bursts every other live shot within
  that radius (0x49a664..0x49a7ee, World::burstTaShot). Engine deviation: a
  quarry that is gone is let go (retail keeps a stale +0x56). With it the
  shot environment test is retail's (0x49b28a..0x49b3cd): feature and
  ground strike only strictly above the shot, a groundbounce bounce takes
  -(vy sar 2) (it was KINGDOMS.icd 0x52a6f3's <=, which burst a vlaunch
  rocket leaving at ground level).
- Carried units' orientation (sim): 0x43d78e -> 0x43e180 every mover tick
  of an attached unit, pad rider and transport cargo alike: heading = the
  carrier's + the attach piece's y turn, pitch + x turn, roll + z turn
  (World::carriedOrientation; piece -1 takes no turn, retail reads the words
  in front of the piece list). A flyer leaves its pad on the pad's heading.
- Factory output on the build piece (sim): BUILD attaches its new unit to
  the QueryBuildInfo piece (0x402948: 0x48aac0(output, factory, piece, 1);
  piece in unit +0xf9, factory in +0x86) and the mover's attached branch
  (0x43d6d0) puts it on that piece's point every tick (0x43e060; a floater
  held at sea less waterline, 0x43d712..0x43d75a) and turns it with the
  piece (0x43d78e -> 0x43e180). World::followBuildPiece does it for an
  output under construction (Unit::buildPiece, cleared at completion), so
  a Flash on armvp's pad turns with StartBuilding's `spin pad` (182 BAM a
  tick). The output used to sit unturned at its spawn point. Completion
  (BUILD's next stage 0x402b07 -> 0x41b8d0) detaches it (0x41b9a1:
  0x48aac0(output, 0, -1, 1); 0x48ab70 at 0x48acaf..0x48acdc unlinks it,
  clears +0x86 and puts it back on the map, 0x47cb40) and writes no
  heading: it leaves on whatever heading the pad gave it, as here.
  Fallout: a guard's door step aside (engine deviation) that is held where
  it began never counted as done, and the Retail navigator kept leading it
  back into the same gap; once no output leaves and no site is refused the
  door now ends such a held step and the guard resumes (clearYardDoor;
  landdoor_exit Retail armlab Ward).
- Aircraft activation (sim + client): 0x43d210 (the flight mode setter)
  calls 0x48b090(1, 1) on any change to a mode other than landed and
  0x48b090(1, 0) on landing (0x43d225..0x43d275), and VTOL_LandIfCan
  deactivates as it starts down (0x40f4c7); the edge runs Activate /
  Deactivate (0x48b0f7 / 0x48b11c). The landing mission's host now
  switches the script on those edges (World::switchScriptEdge; it only set
  Unit::active, so a flyer stayed activated after its first take-off).
  The display VM mirrored activation only for onoffable / activatewhenbuilt
  types, so no aircraft ever got Activate: TA flyers open their wings there
  (armfig activatescr: wing1/wing2 slide out 6 px and tilt) and fold them in
  Deactivate. A flyer now follows the script's bit (GET ACTIVATION word;
  client/retailactivation.h).
- Wheels: the shipped scripts that turn wheels are armyork, armmlv, cormlv
  (StartMoving: `spin wheelN around x-axis speed <150>`, StopMoving:
  stop-spin) and cormabm (StartMoving / StopMoving start its mobile /
  imobile scripts), all on 0x43da70's movement call-ins, which the display
  VM already got with the right tier (verified, unit_animation). armflash,
  armfav, corfav, armstump and the other TA vehicles have no wheel pieces
  (their wheels and treads are faces of `base`, textures `wheel`,
  `Tredside1/2`) and those textures have one frame each in the shipped GAFs,
  so TotalA.exe draws them still too: nothing to port.
- Animation captures: `TA_ANIM_TEST=factory:<factory>:<unit> | move:<unit> |
  air:<flyer>` with `--testbuild --time N --shot`, the camera on the unit,
  the display VMs run by advance(); TA_PIECELOG takes a comma list of ids
  and logs heading and position (TA_PIECELOG_PERIOD sets the interval).
  Evidence: unit_animation (new).
- Transports: 0x48ab70 calls 0x4384a0 only for a carrier without isairbase
  (0x48ad19, flagsA bit 9); 0x4384a0 keeps the descriptor-flag-4 missions
  (0x4384c3: ground rows BuildingBuild, Paralyze, GetBuilt, BeCarried,
  MakeSelectable, Wait, WaitForAttack, AttackUType, SelfRepair; no VTOL row)
  and puts BeCarried in front. BeCarried (0x402fc0): stage 0 at its first
  dispatch aboard, stage 1 a 10-tick sleep per wake (Unit::beCarriedWake);
  after a drop the unit holds (weapons only) until that wake, then its
  next mission or the default one starts. VTOL_PICKUP: event 0x10000 is the
  passenger cloaking (0x48b16e..0x48b195 through the mission listener's
  0x438870; kTargetCloakedEvent), waited on by stages 1..3 (0x100e8 /
  0x100ea) and failing the pickup (0x10048); event 2 is never posted in
  TotalA.exe (writer survey in retailtransport.h). The unit controllers'
  height (0x44e3c0 + 0x44e6c0): the passenger's own y plus the offset
  (cruisealt for stage 1, the link's negated height for stage 3), capped at
  0x1ff0000 (World::followHeight). transport_roster's retailBeCarried.

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
- Repair pads (client): a rider's orientation is the pad's plus its pad
  piece's own turns (0x43d78e -> 0x43e180: unit +0x64/+0x66/+0x68 = carrier +
  piece). Axes: 0x43def0 composes a piece as (piece +0x14 + unit +0x64, +0x12 +
  +0x66, +0x10 + +0x68) (0x43df67..0x43df9c; records of 0x36 from the list's
  +0x22) and 0x4b6cc0 turns about z, then x, then y, so the piece's x turn goes
  to +0x68 (the body's pitch), z to +0x64 (roll), y to the heading
  (client/padrider.h, pad_rider test: the body built from those words turns a
  point as the piece's script turn does). Drawn: heading, pitch and roll, the
  bank a plane landed with replaced (evidence: armasp before/after, a fighter
  parked banked before, level after). No shipped pad script turns its pad
  pieces (armasp, corasp, armcarry, corcarry), so the pad's own attitude is
  what shows. The pad sprays from its QueryNanoPiece to the rider's box on each
  tick SelfRepair mends it (0x402518 -> 0x40252e -> 0x4025ae, the builder
  repair's emitter).
- Model primitives (client/primitivedraw.h): every objects3d load
  (0x4cb560 -> 0x4cb4c0 -> 0x4cb370, called at 0x4cb552) swaps a piece's
  selection primitive into slot 0 (0x4cb37b..0x4cb3b7) and bubble-sorts
  primitives 1..n-1 by ascending average raw y (strict-greater swaps, int32
  sum / count with idiv; 0x4cb3e8..0x4cb4a5), so the draw loops that start at
  1 when +0xc != -1 (0x459aa5 object bitmap, 0x45908a nanoframe outline,
  0x458568 direct draw) skip exactly the selection primitive. The body and the
  nanoframe walk primitives in that order. Untextured primitives keep the
  file's flag word (0x42a140; bit 0 on all 6066 shipped ones) and are flat
  polygons of any vertex count in their raw palette index, unshaded (0x4c1000
  / 0x4c0310 -> span writer 0x4c06e0); index 0 is opaque black (the object
  bitmap's transparent key is 1: 0x4598ae, read at +8 by 0x458d30). The body
  draw had skipped them (a null atlas op); they now draw through 8x8 palette
  swatches (`#pal<index>`) in the atlas -- armcv's colour-0 face was a hole,
  now black. A texture name found nowhere is colour 0xd1 (0x42a204; none in
  the shipped models). A textured primitive is drawn only as a quad (0x4585e2,
  0x459b1a; none shipped besides selection primitives) and still outlined by
  the nanoframe (0x459080 has no count test). Evidence: primitive_draw test;
  armca / corlab / armcv finished and nanoframe captures (debug harness
  TA_CONJURE_REMAINING places a site at a chosen build fraction).
- Unit speech queue (client/speechqueue.h): the global queue (0x51e68c, made
  at 0x47edea: spacing +0xa1 = 30 ticks, 0x47ee07) takes a request unless the
  event's next-allowed tick is ahead (0x47faf0) or the event is queued
  (0x47fb0a); a full queue (8, 0x47fb1a) plays its last entry text-only
  (0x47fb27); entries go before the first of strictly lower priority. Once a
  frame (0x4999a6 -> 0x47f680) the head plays: its speech only when 30 ticks
  have passed since the last voiced turn (0x47f6bb), which then restarts the
  spacing (0x47f6d7), its text line regardless (0x47f6df). The under-attack
  alert goes through it (speech_queue test).
- Ring goal acceptance (sim/retailgoal.h) is TotalA.exe's: accepts
  `(in/16)^2 <= d^2 <= (out/16)^2` (0x44d800 against the squares 0x44d3b0
  stores at +0x14 / +0x18) and measures 18*max + 7*min against in / out
  (0x44d840). Retail has the same acceptance gap (a cell whose octile distance
  is 0 the squared test refuses); a PARK round a TA factory meets it as retail
  does (navyyard_exit counts such ships). A PARK round a *mobile* producer is
  an engine deviation no TA data reaches (no shipped BMcode=1 unit has a
  mobile unit in its build menu, guis/*.gui and download/*.tdf), kept only for
  the production / movement_orders fixtures, which settle.
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
  and the armed CLEAR box) is removed; Cmd::ReclaimArea and Cmd::RetiredAreaBuild
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
- Flyer cell grid (2026-11-24, revised in the close-out pass;
  World::stampAirOccupant, RetailAirOccupancy::taStamp / taUnstamp /
  restamp). TotalA.exe writes cell word +2 from the position setter
  0x48a9f0, which the mover's commit calls inside the unit's own update
  (0x43d6d0 -> 0x43da0e; the attached branch 0x43d775): when the footprint
  origin ((x - footx << 19) + 0x80000) >> 20 or the layer (+0x110 & 3, 2 =
  airborne; a layer change waits in mover +0x2e for the commit, 0x43d210 /
  0x48ab70) changes (0x48aa35..0x48aa56), 0x47d0e0 clears the recorded cells
  still holding the unit's index (0x47d23b..0x47d284; skipped after an
  off-map stamp, +0x82 = G+0x142b7) and 0x47cc30 stamps the new ones
  (0x47cf98..0x47d042): off the map nothing, an empty cell takes the unit,
  an occupied one keeps its holder, which is flagged overlapped (+0x110
  0x4000000) while the newcomer is flagged waiting (0x8000000); a remote
  holder (player +0x73 == 3, 0x47cfda..0x47cfe9) would give the cell up, but
  every owner is local to the lockstep simulation. The flags are read by the
  grid code itself: 0x47d0e0 drops the unit's waiting flag and, when it was
  overlapped, walks the sector buckets for every unit touching its old
  footprint (0x47e5c0, strict overlap 0x47e674..0x47e697) and restamps each
  (0x47ed30 -> 0x47c790): a waiting one drops its flag and stamps its
  footprint again, taking the cells left -- and, meeting its own cells too,
  flags itself again (0x47ca04: any nonzero cell). World stamps at the
  flight commit (tickFlightBody), a layer change (groundMove), a pad
  rider's and a passenger's attached commit, creation (0x48610f, 0x48630c;
  the grid is sized on first use) and death (0x48682d -> 0x47cbd0); debug
  builds check after every unit update that no other writer left a stamp
  stale (TA_VERIFY_AIR=1 aborts). The touching walk goes by sector column,
  row, then unit id (retail's in-bucket order is the bucket's link order,
  not kept). Kingdoms' overlap lists, insert / clear / remove passes and
  random owner are gone (retail_visual_test's fixtures replaced by TA
  ones). Evidence: retail_motion (overhead flyer), air_pad, air_traffic
  (counts waiting flyers), the debug verifier over the air / transport /
  pad tests. Legion baselines retaken for the air-stack fixtures.
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
- Navigator failure on an unreachable circle (2026-11-24, retailtransport.h,
  World::deliverSearchRoute / tickNavigationMovement): GROUND_PICKUP's
  radius-0 circle on a standing passenger (0x40686f -> 0x438930) and
  GROUND_UNLOAD's drop-point circle (0x406a5e) wait on the navigator only
  (0xe8, no timer); the stand-in 60-tick wake (flag 0x80000000) is gone. The
  route to the nearest cell is walked, the navigator asks again when it has
  fewer than two points (0x44f1a0) through wantsPath's 60-tick gate
  (0x44f260), the tracer finds no closer cell (0x40e630: r >= h0), the
  deliver posts the failure 0x40 for an unsatisfied goal (0x44f080; World's
  0x200) and the mission calls again. What had stopped it: the old arrival
  radius stopped a load leg 16..32 px short with no event (a leg the
  navigator drives now ends only on its controller), the re-ask skipped an
  unload-approach leg, and terrain-only harnesses refused cell entry on a
  non-square footprint's bounding square while the search planned on the
  rectangle (0x47db70 tests the rectangle). Evidence: transport (the circle
  wakes on the failure, no timer), transport_roster (its coast explored:
  unexplored cells read clear, D7), transport_post_unload,
  transport_map_roundtrip, legion_transport.
- The navigator's 0x800000 reader (2026-11-24, retailProvisionalRoute):
  setGoal installs the provisional route [unit, target] only when the head
  mission lacks 0x800000 (0x44f3e9..0x44f3f2), the flag a return 9 leaves
  (0x43b8e5). SUPPRESS's approach reads SUPPRESS's mission (its circle,
  0x40392e), the transport approaches their own. Evidence: suppress (a
  flagged SUPPRESS waits for the search, an unflagged one starts at once),
  retail_mission.
- The second mission list (2026-11-24, retailDispatchSecondList): 0x43bad0,
  after the main dispatcher in the unit update (0x48af98 -> 0x48af9e), the
  timer alone wakes a mission (no events), a sleeping one is passed over,
  the walk restarts from the head after each run, 6/7 end it, the rest
  remove (jump table 0x43bc60). Its rows are the common table's 9 and 10
  (flag 0x40000; 0x43ac60 links by it): World's self-destruct countdown
  (0x402010) runs as that list's mission, after the main dispatch, not
  before the unit's orders. Evidence: retail_mission (the walk), kill_credit,
  building_death.
- RepairPatrol's look (2026-11-24): a canreclamate builder's ground patrol is
  RepairPatrol (0x43f3b9..0x43f41d, retailGroundRepairPatrol): stage 0's
  return waypoint (0x43a020), stage 1 moves on at a navigator event (6,
  0x4059ac), else re-installs a radius-16 circle on the waypoint (0x4059c5),
  sleeps 60 (0x4059cf) and looks for work (groundPatrolLook): 6 for a repair,
  3 for a job ahead, a reclaim clearing the wait word first (0x405c13).
  VTOL_RepairPatrol's look (flyingPatrolLook) runs at its stage-1 wake
  (sleep 45, 0x415386) the same way (0x415564, 0x415603, 0x415790). The
  separate timers are gone. Evidence: reclaim (the look at the start and at
  the next 60-tick wake), flyer_combat.
- Flyer take-offs (audited 2026-11-24): VTOL_RepairUnit 0x414fe1,
  VTOL_Reclaim 0x41485e, AirToAir 0x4133fa, VTOL_SeekAttack 0x4107c6 and
  AirStrike 0x41215c take off through flyerTakeOff (0x43d210(2), the climb
  controller at cruisealt / 2, 0x44e2d0 / 0x44e6c0); AirToAir's one-tick
  sleep (0x413444) lets its stage 1 head off while it climbs. AIRSTRIKE's
  stage-0 wait (0x4121a3) and stage-6 pad detour (0x4125c2..0x4126b4) and
  LandIfCan's off-map return (0x40f2e3) are in retailAirStrike /
  retailflight.h. Evidence: air_pad (each take-off).
- A flyer's attack at a point (2026-11-24): the resolver's point branch
  (0x43f1b7..0x43f302) gives AIRSTRIKE for a dropped slot-0 weapon, else
  AIRTOGROUND, a hover gunship too (hoverattack is read only with a target,
  0x43f2fa); the engine's hover at a point is gone. AirToGroundHover's
  two-miss branch waits on 0x110e8 (0x413996), its 0x1000 the refused aim
  (World's 0x100000). Evidence: suppress, mission_orders, retail_mission.
- The hover gunship's jink (2026-11-26, a "Brawler strafes" report):
  AirToGroundHover's stage 3 wakes only on the target controller's arrival
  (0x20 from the navigator 0x490690 while 0x44e5b0 has the flyer within 16
  px, 0x44e730(0x10); the controller is kept, 0x44e3a0) and puts the next
  hold 45 degrees the other way round the bearing it then has, so the holds
  alternate between two points about 188 px apart for the Brawler and it runs
  back and forth between them, nose on the target (0x44e530), about 76 ticks
  a leg. The whole loop (mission 0x413470, controller 0x44e330 and its
  vtable 0x4fd3b8, the flyer navigator 0x4fd9b0 / 0x4fd9e0 -> 0x490690 /
  0x490650, the VTOL mover 0x43d290, attitude 0x43d0d0) replayed on
  TotalA.exe under unicorn (tools/re/airhover_emu.py) matches World tick for
  tick, bit for bit, over 1299 ticks and 17 holds. Three differences found on
  the way are fixed: the mover's distance and approach factor are 53-bit, not
  floats (0x43d51b..0x43d5bc), its X correction goes through a float as the
  Z one does (0x43d5fe), and the retention is an integer division
  (0x43d326); the attitude's denominator is the map's gravity (game +0x14263,
  0x43d186), not 8155; a flyer's bankscale defaults to 1.0 (0x42c186; Core
  Shadow, Hurricane, the seaplane fighters). Not fixed: World gives every
  unit its own maximum speed (individualSpeed, 0.9..1.1 x maxvelocity, a
  gameRand(201) at birth: Kingdoms' 0x512430); TotalA.exe's movers read the
  type's +0x192 (0x43d301, 0x43cc76) and it has no such draw, so a TA unit's
  speed is its type's. Evidence: flyer_combat ("hover jink", "attitude").
- The hit response's tests (2026-11-24, World::taWeaponReach,
  taHitResponse, slotHitResponse): 0x49abb0 line by line (waterweapon:
  floater / hover against the sea; else the shooter's and target's tops,
  toairweapon on a flying target, a ballistic arc via 0x49a890; then the
  range from the squares' high dwords); part (a) tests nochasecategory
  (+0x23d) and slot 0's badtargetcategory (+0x231) and 0x49abb0 (no leash
  reach); part (b) 0x4070f2..0x4071aa. The weapon path responds before the
  HP drop (0x489da2 before 0x489eb5). Evidence: fire_sweep retaliation
  (range, categories, waterweapon, a lethal hit answered).

## Open

Classes: (a) Kingdoms gameplay still running in a TA game; (b) a retail detail
approximated (marked "cosmetic" when it changes no simulation state); (c) a
test or fixture note; (d) Kingdoms code still compiled but unreachable from TA
data (cleanup, no gameplay).

None left: see "Resolved (close-final pass, 2026-11-25)".

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

## Resolved (close-out pass, 2026-11-24)

- Flyer cell grid: stamped at the mover's commit, with TA's overlap flags
  and hand-over (see Ported, "Flyer cell grid").
- Fire gate's out-of-range bit (unit word +0xba 0x1000, World 0x100000):
  World::noteSlotOutOfRange raises it for a loaded slot whose range gate
  refuses (0x49e3ae, 0x49e3cf -> 0x49e53a) -- the fire path's surfaced / arc
  tests, the callers' horizontal test (tickSlotWeapons,
  tickAttackWalkWeapons, tickCombat for the slots the attack aimed) -- and
  for a turret aim with no solution (0x49d65b..0x49d664); setSlotTarget /
  setSlotPoint clear it (0x48a060 / 0x48a0a0, & 0x83ff). Its readers are
  the mission dispatcher's wait masks (0x43b7fe: unit +0xba | mission
  +0x4e): Suppress (0x1c00) and AirToGroundHover (0x110e8, 0x413996; World's
  hover handler must add 0x100000 to its mask to wake on it).
- 0x49b090's interceptor test: within reach of the shot it chases (+0x56,
  weapon word +0xd6) the shot bursts (0x49b1a9 -> 0x499eb0) and the body
  tests still run (0x49b1ae): ProjectileCollisionResult::intercepted beside
  the body struck (ta_shot_test). Shots are handed targets now (see Ported,
  "Stockpiles and interceptors").
- Kingdoms projectile kinds: no TA weapon reaches them -- every weapon some
  unit fires is claimed by a TA spawner (scan of every weapons/*.tdf in the
  GOG install: lineofsight / selfprop -> 0x49c9c0, turret ballistic ->
  0x49cde0, dropped -> 0x49dd60); the Core Contingency mines' ARMMINE* /
  CORMINE* records (ballistic, no turret, no aim handler: 0x49e010 gives
  them none) are only their units' explodeas / selfdestructas, no unit
  fires them through 0x49e1a0. Removed: flame streams, the straight /
  lightning / status shots and their launch delays (start + 1, nimbustime),
  the non-selfprop guided launch and pass, Weapon::losStatus,
  statusShotRefused, Weapon::nimbus / nimbusTicks, UnitType::hasNimbusArt,
  the hweffect lightning, gaf/nimbus.h and the gameplay hash's nimbus and
  storm-timing parts (mod_packs now uses feature burn timing). A TA weapon
  no spawner takes (a turret neither lineofsight, selfprop nor ballistic,
  0x49d74b..0x49d783) fires and pays nothing (0x49e441); the remaining
  2D / instant paths are the synthetic test armies' (taAim None).
- Transports: VTOL_PICKUP attaches at the QueryTransport piece (mission
  +0x36, 0x4114a0..0x4114a6) and the passenger rides it (attached branch
  0x43d6d0 -> 0x43e060, cargoAttachPoint).
- Death accounting, network only: proved without effect in lockstep. The
  remote-owner branch of 0x486f10 (0x486f7e: owner record +0x73 not 1 / 2 ->
  0x49b000(unit, 1), +0x110 |= 0x4000, 0x4864b0(unit, 3)) is the proxy path
  of retail's per-owner simulation: the owner's machine runs the 30000
  kind-3 record (0x486f94) and its death packets drive the proxies. Every
  lockstep peer runs every unit as its owner does, so the local branch on
  all peers is the deterministic equivalent (commanderSweep). The kind-3
  loss guard (0x4869f5..0x486a31: the local player's +0x1c8c[owner] byte)
  is set only when a remote player's final statistics packet arrives
  (0x457540 / 0x45768a, packet 0x29); a lockstep game exchanges no such
  packet, so the byte stays 0 and every peer counts the loss; it only
  touches the statistics word +0xfe, never a kill or veterancy.
- The pad list's rand(30) (0x40b301 -> 0x4b6c30, the global stream
  [0x51fc88] the sim also draws from): retail draws it on the host that runs
  the AI, interleaved with that host's own simulation. Here the AI runs on
  the server only (server/roomtick.cpp: Controller::tick with a const World,
  its orders sent as commands); a draw from the sim's stream there would
  advance it on the server alone and desync every client. So it stays on
  the AI's own deterministic stream (TaRetailBrain::random,
  docs/retail-ai.md "RNG in lockstep"): the retail interleaving cannot be
  reproduced in lockstep.
- Kingdoms code removed: the line-of-sight status branch and nimbus (above);
  RetailAirOccupancy's Kingdoms passes and Unit::airOccupant's overlap lists
  (retail_visual_test's fixtures replaced by TA ones); Unit::weaponSlot (a
  pad, reads are slot 0); the group records and checks (World::retailGroupOf,
  retailAttackOutOfSlot, the Move_Ground / VTOL_Move / guard / standby group
  checks, AirAttackGroup's formation push, pushGuardFormation,
  flightFormation, retailgroup.h's record helpers -- Legion keeps
  legionFormationGroup, legionStandbyCheck and the formation numbering);
  the client's Kingdoms build-sparkle art names; unbuilt nimbus_test and
  effect_inventory.

## Resolved (mana pool and client pass, 2026-11-25)

- The engine's single-resource pool is gone: Player::mana / income / storage /
  manaCapacity / manaMult / constructionShare / displayResources /
  manaShareMask, UnitType::income / storage / buildCost, the accrual and the
  allied overflow sharing, creditCreationStorage, RetailConstructionResources
  and with it Kingdoms' replayed construction (Unit::retailSite / retailBuild,
  now pads; tickRetailConstruction, tickRetailGetBuilt, initializeRetailSite,
  completeRetailConstruction), the replayed Classic AI's World side
  (Player::retailAi / buildCache, tickRetailAiBase / Strike; the build-cache
  priority refresh; the bookkeeping clock keeps its rand(30) draw), the
  allocation-paced corpse revive (unreachable: reviveKind is 1 or 0), the
  unused workProduction, and the client's mana readouts (the legacy top-left
  bar, the MANA panel, the mana bulb). TA's metal / energy books
  (sim/taeconomy) are the only economy. stateHash folds, where the pool's
  double went, the value it held at a tick's end (a TA player's stored metal,
  which the pool mirrored, else 100), so the hashes of TA games are unchanged.
  MatchSlot::manaMult is incomeMult (PlayerBooks::mult).
- Auto-share (TotalA.exe 0x457d30, after the wind 0x49563a, every 60 ticks;
  World::taShare): a human player with ShareMetal / ShareEnergy on (info +0x97
  bits 2 / 4, toggled at 0x418d04 / 0x418dc4) above its threshold (+0xe4 /
  +0xe8; SetShareMetal / SetShareEnergy store min(N, cap), 0x419340 /
  0x419400) gives the last allied human by index storing less
  min((stored - threshold) x 1/3 (metal, 0x4fd4b8) or 1/2 (energy,
  0x4fd4bc), their room) through the manual give (0x464c60 / 0x464b30): paid
  at once, credited to the receiver's own accumulator. Cmd::ShareMana is
  Cmd::ShareResources (same number: targetId econ::M / E, queue bit 0 on,
  bit 1 also the threshold); the diplomacy menu's SHARE MANA column is your
  row's SHARE M / E switches. Evidence: unit_gift (the switch's wire round
  trip and hash, the last poorer allied human, the room cap, a third / a half
  above the threshold, enemies and computers skipped), map_transfer (server
  stamped, referee and spectator lockstep).
- ATM is TotalA.exe's (0x4170c0): stored energy and metal += 1000 (-(-1000.0f),
  0x4fcc7c), capped at the next settle. MANAME, Kingdoms', is retired
  (PowerCode::RetiredManaMe, never parsed, refused by the server and the sim).
  Evidence: power_codes.
- Map scenario resource conditions 24 / 25 read TA's books: the one scenario
  "resources" figure is metal and energy alike (as actions 16..20 write it), so
  "less than v" holds when either store is under v and "greater than v" when
  both are over it. TA's own campaigns have no resource condition
  (docs/retail-campaign.md). Evidence: scenario_resources.
- Site decay is TotalA.exe's GetBuilt (0x402da0): sleeps 300 / 30, then on a
  timer wake with no work 0x41bcd0(site, 11) and a sleep of 11 (was Kingdoms'
  sleep of 1); the step 0x41ba60 with amount -(buildtime x 11) /
  buildcostenergy takes 11 / buildcostenergy of the build, the HP word the
  matching points, and refunds buildcostmetal x the step as the SITE's metal
  production (x0.5 / x0.7 easy / medium computer, 0x4fcc98 / 0x4fcc90), no
  energy; the site dies (kind 9) once nothing is left, after its last refund.
  Kingdoms' 429d70 (0.5 / buildtime a tick, refund into the pool, none on the
  last step) is gone. Evidence: site_decay (new), queue_modifiers,
  structure_combat_site.
- COB GET is one routine for both hosts (sim/retailscriptget.h, TotalA.exe
  0x480770, table 0x480ac4): the display VM answers from the sim's words in the
  snapshot (World::scriptGetWords: ACTIVATION / INBUILDSTANCE / BUSY / YARD /
  BUGGER_OFF from the script, the standing orders, HEALTH, BUILD_PERCENT_LEFT
  from the remaining work 0x480a44), UNIT_XZ / Y / HEIGHT from the argument's
  unit, PIECE_XZ / Y from its own VM's pieces posed as 0x43e060 does,
  GROUND_HEIGHT from the map. The sim's GET 17 reads TA's remaining work
  (was 1 for any unfinished TA site). Evidence: ta_get_range.
- Unit speech: the sim posts TotalA.exe's 0x47f780 requests
  (World::unitSpeech); the client keeps its local player's, through the speech
  queue and the event table 0x5086dc (client/unitspeech.h: priority, cooldown,
  slot, line). RECLAIMUNIT: "Reclamation failed" / "That unit cannot be
  reclaimed" (event 7, 0x404791..0x4047e6, 0x414d61 / 0x414d89) and the work
  event 11 "working" (0x4048be, 0x414d20). The status line is TotalA.exe's
  mission status text (table 0x4fc490..: "Reclaiming", "Nanolathing", "Under
  construction", "Being transported", "Resurrecting", ...; client/unitstatus.h),
  not Kingdoms' unitmissions.tdf labels. The armed CLEAR cursor over a
  raisable feature is Reclaim, as retail's. Evidence: unit_status,
  click_rules.
- Client Kingdoms names and art: the old HUD's Kingdoms .gui command panel,
  info bar and order column (guis/<side>ingame.gui, anims/<side>ingame.gaf,
  actionbuttons / igcommonbuttons -- none in TA's data), the per-side art
  swap, the generated maps' Kingdoms shoreline waves (TA ships no wave
  feature), the lobby preview's Kingdoms TNT branches and kingdom palettes,
  the "aramon" feature-world defaults; Monarch names are Commander
  (CommanderAlert, SelectCommander, startAtCommander; the saved hotkey keeps
  "select.monarch"), and the user-facing Kingdoms text (data folder help,
  sound help). Retained: KINGDOMS.icd citations in comments and the
  monarchExpendable wire / settings name.
- tdf orderedChildren lists a duplicate section where it stood (it listed
  the first definition's slot with the later one's body).
- Unbuilt tools/ sources: ported and registered -- site_decay (rewritten for
  TA's decay), hp_overflow, scenario_actions, scenario_info,
  scenario_outcome, scenario_selectors, scenario_placement (TA copyright line
  in their FBIs), tdf_limits (TA's first-definition and value rules);
  deleted (Kingdoms fixtures or Kingdoms rules: mana pool, unit mana / xp,
  canbuild/ build trees, buildtime / workertime timing, multiplier damage,
  the lightning quad, the retail replay probe) -- production_test,
  ai_economy_test, owner_change_test, campaign_conditions_test,
  scenario_test, retail_replay_probe, retail_visual_test,
  crusades_damage_test, stuckrepro, cartographer_validation_test,
  duration_test, helpbuild_test, factory_death_test, command_validation_test,
  scenario_restrictions_test, cartographer_aiprofile_test, match_codec_test
  (its pinned bytes predate the current codec); retail_construction_test
  (built; Kingdoms' construction object) deleted with
  RetailConstructionResources; retail_ai_test keeps its oracle modes, its
  World checks went with Player::retailAi. Left unbuilt, as they reference
  nothing removed: acme_test_driver (docs/public-server.md),
  featburn_bench / legion_staticsync_bench (their scripts), footprobe,
  retail_cost_test (an oracle driver), cartographer_document_test,
  cartographer_playtest_test, replay_writer_test, music_decode_test.

## Resolved (close-final pass, 2026-11-25)

- Fire gate to the SweetSpot: the weapon update runs the whole range gate
  0x49aa80 at 0x49e3c8 (World::taRangeGate): the horizontal range from the
  shooter's origin to the slot's target point (a unit's SweetSpot, 0x48a1e0
  -> 0x43e3c0 -> 0x43e0b0's bounds centre, or its led point), the surfaced
  and the arc tests. Why the first attempt left units standing: Attack_Chase
  tests the target's ORIGIN (0x49abb0 at 0x4035d6 / 0x4037d7), but it does
  not stop on that test -- stage 2 paths into the circle of the slot's range
  round the target (0x438930, substate 0) and stage 3 re-tests only every 30
  ticks without stopping the walk (0x4037fd), so retail ends inside the
  range -- whereas World stops the walker the tick the origin test passes,
  where a SweetSpot past the origin keeps the gate shut for good. An attack
  free to move now keeps closing until its target point passes the gate too
  (tickCombat, sweetShort). Evidence: sweetspot_gate (six walker / target
  pairs from out of range: all fire, every shot's target point within
  range); air-stack-barracks-5's hashes retaken (a Drake's run refuses a
  shot whose target point is out of range).
- Non-turret LOS / selfprop weapons (0x49d9c0): the world heading (0x49da11)
  and the line-of-sight pitch (0x49da4d) from the QueryX point are tested by
  0x49d880 against the body's heading +0x66 and pitch +0x68 (World's
  groundPitch); a ballistic or selfprop weapon of that class takes the LOS
  pitch too. (docs/retail-weapon-scripts.md 7.8.)
- The cruise branch of 0x49b3e0 is tested: armsilo's nuclear_missile
  (cruise=1) turns over above the cruise height after its vertical launch,
  glides for its target at 700 px (+0x16 = 0x2bc) while more than 1024 px
  out (never climbing, never below 700), dives inside 1024 px for the ground
  at the target point and bursts within its area of it (stockpile). The port
  needed no change.
- Lightning strands (rendertype 7): TotalA.exe's CRT stream 0x4e4870 is
  seeded with the clock at start (srand(time(0)), 0x49e8b2..0x49e8bb and
  0x4971a5..0x4971ae) and shared by 61 call sites; the client keeps one such
  stream (GameView::crtRand_, seeded from time(nullptr)) advanced by every
  strand draw, frame after frame (was: reseeded per frame from the frame,
  the shooter and the launch). Drawing only, it never reaches the lockstep
  sim; the interleaving with retail's other CRT callers is not observable
  under a clock seed.
- D-gun pose (rendertype 3, 0x49c1f2..0x49c263): the model is drawn by
  0x46bae0(surface, &position, weapon model +0x74, &local 0x38). Traced:
  nothing in 0x49be60 writes locals 0x38..0x3d (rendertype 1 builds its
  pose at local 0x30 from the shot's +0x34 / +0x36 / +0x38 plus 0x8000;
  0x4b8500 and 0x46bae0 clean their own arguments, so the pointer is the
  frame's local), so retail's D-gun pose is whatever the stack held: there
  is no retail value to port. The client keeps drawing the model through
  the generic model path, oriented along its flight.
- The acos quirk (a target below a ballistic weapon at short range is aimed
  with acos's unsigned angle, 0x49a9ce: the shell goes up and long) is
  retail behaviour, ported.
- ARMORED: SET 20 (0x480be3 -> 0x48b090(2, value); no call-in for that bit)
  is stored (UnitScript::armored, hashed while set) and GET 20 answers it
  (0x480aa9; sim host and the display VM through retailscriptget.h). Its
  effect: 0x489bb0 scales a hit or record under 30000 by the type's
  damagemodifier when +0x10e & 2 (0x489bcd..0x489bf1) -- the ARMORED bit, not
  the activation (World applied Kingdoms' 51a16e rule, activated units);
  so the Toaster (cortoast, 0.15) takes 15% while closed. SET 1 ACTIVATION
  (0x480b49 -> 0x48b090(1)) now switches the script and starts Activate /
  Deactivate on a change (armsolar's HitByWeapon closes and reopens it).
  Evidence: armored (new), kill_credit, scenario_actions.
- A passenger re-picked before its BeCarried wake: 0x4384a0 keeps the old
  BeCarried (flag 4) and pushes a new one in front, whose schedule runs from
  its first dispatch aboard (attachCargo resets Unit::beCarriedWake; after
  the next drop the old one retires at once, 0x402fce). Evidence: transport
  roster "re-picked".
- The boom race (an unload given while TransportPickup still swings the
  boom races it for the same pieces) is retail behaviour.
- The watch / spectator harness checks were already on TA units (d1e1577);
  the render study's churn now spawns armpw / corak / armrock / corstorm
  (was Kingdoms' roster), and a stale Kingdoms capacity note went.
- The script-less carrier stand-in is gone: TotalA.exe has no unit without
  a script, so GROUND_PICKUP / GROUND_UNLOAD only call TransportPickup /
  TransportDrop (0x40680f, 0x4069bf) and World now does the same (removed:
  the fifteen-tick BUSY, the BoomCalc reach test in the sim, the hull-point
  drop, retailSurfaceUnloadTransfer / retailHullPoint /
  World::scriptedTransport). The engine-built fixture carriers carry a real
  script (tools/test_carrier_script.h: armtship's BoomCalc reach, BUSY,
  ATTACH_UNIT, the boom turned and run out to the point and DROP_UNIT on a
  link piece). Legion classifies an unload approach that a TransportDrop
  call has folded its unload into as the Unload mission still.
- A hover pinned by a cliff with its crane astern: traced as retail. The
  first TransportDrop is out of reach, so 0x406a23..0x406a5e installs the
  drop-point circle of ftol(1.5 x footprintz x 16) = 96 px (armthovr); the
  hover comes to rest against the cliff inside it (61.6 px from the point),
  the navigator arrives, BoomCalc's 87.5 px boom does not reach; after the
  third call 0x406a13 returns 9 and, with no next order, the dispatcher
  calls again every 30 + rand(30) ticks with the count kept (0x43b8e5..
  0x43b973) without a new circle -- retail stands too. Evidence: transport
  roster "hover unload against a cliff".

## Resolved (ten players, 2026-11-26)

- Player limit 8 -> 10, as retail: TotalA.exe's game struct holds ten 0x14b-byte
  player records at game+0x1b63 (index math 0x4473b1..0x4473c3; the loop at
  0x445799..0x4457ae steps to +0x2851 = 10 x 0x14b past the first); 98 maps of
  the installed data define StartPos1..StartPos10 and textures/logos.gaf has ten
  colour frames. `sim::kMaxPlayers` and `net::kMaxSlots` are 10 (static_assert
  in net/matchconfig.cpp); random maps take 2..10 players (`mapgen::kMaxPlayers`).
- The map scenario's neutral owner is one named id, `sim::kNeutralPlayer` = 10
  (`crt::kNeutralOwner`), no longer the literal 8. It keeps colour slot 9, which
  a tenth player may also wear (retail has no neutral owner in skirmish).
- .crt 3.0: owners 0..9, neutral 10, banks 0 = All Players, 1..10 = players,
  11 = neutral. `crt::parse` renumbers a 1.0 / 2.0 file (owner 8 -> 10, banks
  from 9 on up two), `crt::write` renumbers back for those versions (byte-exact
  round trip) unless the model needs the new numbering; Cartographer saves 3.0.
  Its rule-name sidecar is version 2 (version 1 banks renumber the same way).
- Protocol 343: RoomState and the replay header carry ten slots; Chat and VoiceUp
  recipients are u16 (`net::SlotMask`, `kEverySlot` = 0xffff).
- The retail path scheduler's record table (`RetailSearchScheduler::playerCount`)
  is retail's ten plus one for the neutral owner; that record is empty outside
  map scenarios and an empty record is skipped, so the walk is retail's.
- Client: the battle room's slot rows are 24 px (ten rows above the rules on the
  540-high canvas), the diplomacy panel shrinks to fit eleven rows, the skirmish
  front end's ally groups take their first member's slot as the team (the old
  `10 + group` folded onto other seats' teams under `team % kMaxSlots`).
- Open: retail's load screen plate has seven player rows (guis/loadscreen.gui),
  so a game of eight or more shows the first seven.
