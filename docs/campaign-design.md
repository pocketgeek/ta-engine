# Campaign system — design & RE notes

How the retail Book of Darien (base) and Iron Plague campaigns work, and the plan
to rebuild them. Reverse-engineered by static analysis of `KINGDOMS.icd` + the
shipped mission data (`missions.hpi`, `IPMissions.hpi`, `camps/*.tdf`); see the
per-area findings below. Nothing here is copied from retail code/data.

## 0. `KINGDOMS.icd` vs `ironplague.icd` — settled

They are **the same engine at two patch levels**, not two games. `ironplague.icd`
is an orphaned older build (PE link Dec 1999, FileVersion **2.0.0.1**,
`...\Final_Release\Kingdoms.pdb`); `KINGDOMS.icd` is the shipped final patch (Feb
2000, **3.0.0.1**, `...\V3Final_Release\`) and a strict superset. The install's
`Kingdoms.exe` stub composes `Kingdoms.ICD` — nothing ever loads `ironplague.icd`.
v3.0 adds exactly two things over 2.0, **both already handled by us**: the
campaign-picker front-end (`SelectPlayerAndCampaign` / `PlayerCampaignDialogue`)
and the Crusades unit-balance overlay (our `--crusades`). **One engine covers
both campaigns; there is no Iron-Plague-only code path.** Iron Plague is content
(the `takx*` maps + new unit FBIs), not a separate executable. Keep RE-ing against
`KINGDOMS.icd` only.

## 1. Data model

A **campaign** is a flat, ordered mission list — pure data, no code:

```
camps/<name>.tdf
  [HEADER]   { campaignside=Core; }          # campaignside is vestigial (TA Arm/Core), ignore
  [MISSION0] { missionfile=<stem>.ota; missionname=<stem>; }
  [MISSION1] { ... }                          # play/unlock order = the section index
```

| Campaign file (archive) | Missions | Stems |
|---|---|---|
| `camps/book of darien.tdf` (`data.hpi`) | **48** | `takmission01_mt` … `takmission47_*` |
| `camps/the iron plague.tdf` (`IPData.hpi`) | **25** | `takx01_dh` … `takx25_dh` |
| `camps/ipalt.tdf` (`IPData.hpi`) | **25** | shares takx01–24, **alt finale `takx26`** instead of `takx25` |

`ipalt` is an *alternate ending* of Iron Plague (the two IP tdfs are byte-identical
through MISSION23, diverging only at the finale). The base list has a comparable
one-off (`takmission39a_mt` sequenced before `takmission39_mt`). The `_mt/_ph/_dh`
suffix is a **narrator/voice-track tag** (also names the movie + briefing wav); it
does **not** track `kingdom`.

A **mission** is a bundle keyed by the stem:

| File (in `missions.hpi` / `IPMissions.hpi`) | Role |
|---|---|
| `<stem>.ota`  | `[GlobalHeader]` (mission config + win/lose rules) + `[Map Data][units]` (placements) |
| `<stem>.tnt`  | terrain |
| `<stem>.cob`  | mission "god" script (COB VM) |
| `<stem>.tdf`  | **allowed unit-type set** for the mission (`[ARAARCH]{} [NPCEMEN]{} …`) — build restriction |
| `<stem>.txt`  | **objectives** text (bulleted lines) |
| `Movies/<stem>.bik` | fullscreen **intro movie** (voice+portraits baked in) |

### `.ota` `[GlobalHeader]` keys that matter

- `kingdom=` — the **terrain tileset / world** (aramon/veruna/taros/zhon/creon/caves/
  volcano) *and* which victory screen shows. **Not** the player's faction.
- `ismission=` (0/1) — a scripting-mode flag (most campaign missions are `0`), not
  the campaign marker.
- `lineofsight=` (fog on/off), `mapping=` (minimap pre-revealed), `maxunits=`.
- `Player<N>=[control] [role] logo <C> [kingdom] [massattack <T>]` — slot defs:
  - `control` — `strategic` (full AI economy+attack) | `passive` (inert NPC/wildlife) | *absent* = human/interactive. Slot 1 is normally the human.
  - `role` — `opponent` | `ally` | `neutral` (diplomacy toward the human).
  - `logo <C>` — colour/team index; the trailing kingdom word sets the faction (omitted for neutral owners). Units in `[Map Data]` reference the slot via their `Player=` field.
- **Header-embedded victory/defeat conditions** — see §3.

### Text / audio wiring

- Objectives: the `.txt` (already shown as a 30 s corner briefing).
- Localized mission names/briefings: `translate/missions.tdf`, `unitmissions.tdf` (`english.hpi`).
- Briefing VO: loose `Sounds/<stem>.wav` (only a few shipped).
- Per-mission AI tuning: `ai/mission<NN>.txt`.

## 2. Mission scripting (COB)

**Correction the RE forced:** a mission `.cob` is an ordinary COB v6 module whose
`MAP_COMMAND` / `PLAY_SOUND` opcodes take a **per-cob string-table index** as their
first operand — the name-table entry is a *text command string* (`"Create ZONTER"`,
`"SetMission m 130 74"`, `"<stem>.wav"`). Today `src/cob/cob.cpp` never parses that
table (stops at `nPieces=0`) and `main.cpp:mapCommand()` treats the operand as a
fixed integer subcommand — which only works for `takmission01_mt.cob`'s indices and
is wrong for every other mission. **Fix: parse the COB name table; dispatch
`MAP_COMMAND` on the verb string; resolve `PLAY_SOUND` to that name's `.wav`.**

### Engine-invoked entry points
`Start` (once at load) · `TriggerHit(regionId, unitId, ownerByte)` (unit enters an
armed region; `ownerByte==0` = the human) · `UnitCreated(unitId, 0)` (build finish)
· `UnitDestroyed(unitId)` · `WindChange` (periodic).

### `MAP_COMMAND` verbs (retail dispatcher `0x4d35c0`)
`Create <TYPE>` (spawn; **returns unit id**; `(x,y)` or `(player,x,y)`) ·
`SetMission <unitId>` + order-queue string (see below) · `SetTrigger <id> …`
(arm region: `(id,x,y,r)` circle / `(id,x1,y1,x2,y2)` rect) · `RemoveTrigger <id>`
· `GetUtype <TYPE>` (**returns** type id) · `WriteValue <name>` / `ReadValue <name>`
(persistent mission variables) · `SetAttribute <Health|Mana|Attack|Armor>Percentage
<unitId> <pct>` · `Capture <unitId>[,player]` · `ScreenShake`. (`Kill`/`Attack`
exist but shipped missions don't use them.) There is **no** message/movie map-command.

### `SetMission` order mini-language (parser `0x513fb0`)
Comma-separated queue; dispatch on first letter:
`m X Y` move · `ma X Y` move-attack · `a <TYPE>` attack-all-of-type · `a X Y`
attack ground · `p X Y[ Z]` patrol · `w N[ M]` wait N s · `wa` wait-for-attack ·
`s` **make selectable** (hand the unit to the player) · `d` self-destruct ·
`b <TYPE> H X Y` timed reinforcement drop · `o A[ B]` stance · `v F` scalar · `c` flag.
Example (`takmission01`): `SetMission s, w 4, m 133 68, m 129 68, w 3, m 133 68, a TARZOM`.

### GET / SET
`GET_UNIT_VALUE id=7` → a unit's type id (compare against `GetUtype`). `GET valId=30`
→ roster/build-list index (already implemented). `SET_UNIT_VALUE(id=2, value=1|0)` →
**force victory / force defeat** (scripted override of the data-driven rules).

### PLAY_SOUND
`PLAY_SOUND <nameIdx>` plays `nameTable[nameIdx]` as a `.wav` (character VO / the
`<stem>.wav` briefing). Currently stubbed → mission audio is silent.

## 3. Win / lose — **data-driven**

Win/lose is primarily evaluated by the engine from `.ota [GlobalHeader]` condition
keys (each is a `VictoryCondition_*` / `DefeatCondition_*` object checked every tick):

- **Victory:** `DestroyAllUnits`, `KillAllMobileUnits`, `KillAllOfType=<T>`,
  `KillUnitType=<T>,<n>`, `KillEnemyCommander`, `MoveUnitToRadius=<T>,<X>,<Z>,<r>`
  (escort), `UnitTypePassesX/Z=<T>,<v>`, `VictoryTimerRunsOut=<s>`, `CaptureUnitType`,
  `BuildUnitType`, `AllUnitsKilledOfType`.
- **Defeat:** `CommanderKilled`, `AllUnitsKilled`, `AllUnitsKilledOfType`,
  `UnitTypeKilled=<T>,<n>`, `DeathTimerRunsOut=<s>`, `AnyUnitPassesX/Z`.
- **Scripted override:** the COB can force the result with `SET_UNIT_VALUE(2, 1|0)`.

Today the engine only honours the scripted `value==1` (victory) and last-team-standing;
`value==0` (defeat) and *all* the data-driven conditions are unimplemented.

## 4. UX flow (retail)

```
MainMenu (PlayStory "girl" door → Choice::Campaign)
  → Select profile + campaign        [PlayerCampaignDialogue.gui]  (resume at saved index)
  → per mission i:
       intro movie   Movies/<stem>.bik           (fullscreen, skippable — BEFORE briefing)
       briefing      [Briefing.gui] + .txt lines (+ optional <stem>.wav VO) → Proceed
       load          [loadscreen.gui]
       play          <stem>.tnt/.ota/.cob
         WIN  → singleplayerwin.gaf banner → Victory.bik → victory<kingdom>.gui
                → Proceed: persist index, next mission (posttakmission24 after M24;
                  PostTakCredits after the final) ; or Main Menu
         LOSE → singleplayerlose.gaf → Defeat.gui → Restart (reload) / Main Menu
```
In-mission: `F2MenuSinglePlayer.gui` (pause) → `GameInfoBriefing.gui` (re-read
objectives); `SinglePlayerExitMenu.gui` **Restart** = retry, **Exit Battle** = menu.

Dialogue/narration is **movie/data-driven**, not COB-driven: the intro `.bik` carries
the voiced story; on-map lines come from `.ota` message triggers; Iron Plague adds an
in-engine portrait widget (`playercampaigndialogue.gaf`). **Progress** lives in the
Windows registry/ini in retail (`FavoriteCampaign`, `InitialMission`, per profile);
our portable replacement: a small per-profile/per-campaign record `{campaignFile,
highestUnlocked, lastPlayed}` via the existing `ta::Settings` persistence.

## 5. Current engine — reuse vs. build

**Reusable substrate (works today):** COB mission VM + event wiring
(`main.cpp:1428-1458`), region arming + `TriggerHit` sweep (`3091-3112`), `.txt`
briefing panel, the `.crt` skirmish-scenario win/spawn system (`1491-1614`),
outcome→banner→menu, and the Bink player (`MainMenu::playIntro`).

**Missing / wrong (the spine):**
| Need | State |
|---|---|
| `camps/*.tdf` loader + campaign state machine + progress persistence | MISSING |
| Release-valid mission launch (missions are `#ifndef NDEBUG` only, `9749-9754`) | MISSING |
| Mission players/factions from `PlayerN` defs; **AI for mission enemies** (AI is server-only; missions run local free-run with none) | MISSING |
| COB name-table parse + verb-string `MAP_COMMAND` dispatch + `PLAY_SOUND` | WRONG/PARTIAL |
| Data-driven `VictoryCondition_*` / `DefeatCondition_*` evaluator (+ scripted `SET_UNIT_VALUE(2,0)` defeat) | MISSING |
| Per-mission buildable-set filter from the `.tdf` | MISSING |
| Per-mission intro movie; win/lose → next/retry; victory/defeat screens | MISSING |

## 6. Key architecture decision — where a mission runs

Skirmish today = client + an auto-launched local `taserver` (server = authoritative
sim + AI + determinism). The old mission path = client-only local free-run sim with
the COB script in the viewer and **no AI**. A campaign needs the COB script to drive
the authoritative sim (spawns/orders/triggers/win-lose) *and* AI for `strategic`
opponents. Two ways:

- **A. Through the server** (consistent with the project's server-authoritative
  design): the server loads the mission (units, `PlayerN` slots, `.tdf`, the COB
  script, victory rules), runs script + AI, relays to the client (which owns the
  campaign UI: movies, briefing, objectives, progression). Reuses server AI + one
  sim path. Cost: move the mission COB VM + rule evaluation to the server.
- **B. Local client sim** (extend the existing debug path to Release): client runs
  sim + COB script + a **new local mission-AI** for strategic opponents. Faster to a
  playable mission; cost: a second sim path + a local AI, diverging from SP-skirmish.

Recommendation: **A** — it matches "AI is server-side only / SP auto-launches a local
server", keeps a single sim path, and gets AI for free; the COB VM move is the main lift.

## 7. Phased plan

Status (2026-09-07): phases 1–7 are **done and verified** (bar one data-absent
item). A campaign mission runs over the real taserver/taclient in lockstep
(`err=none`, reproducible hash); the full front-end loop — pick → intro movie →
briefing (+ VO) → play (objectives panel) → post-mission cutscene → victory/defeat
→ next/retry, with end-of-campaign credits — works from the main menu (or `taclient
game --campaign <stem>`); the conjure menu is restricted per mission; and the
mission-runner sim has condition guards, the full `SetMission` verb set, and proper
compacted-slot diplomacy. The Iron Plague dialogue widget is the sole open item and
is **not implementable against this install** (see phase 7 below).

1. **Scripting core (no campaign yet).** ✅ COB name table (`src/cob`);
   verb-string `MAP_COMMAND` dispatch (Create/SetMission/SetTrigger/GetUtype/
   WriteValue-ReadValue/SetAttribute/Capture/ScreenShake); `SetMission` order
   mini-language — m/ma/a/p(patrol-loop)/s/d/**w(timed wait)**/o(stance)/v done;
   **wa (wait-for-attack) and b (timed reinforcement) still TODO**. `MissionScript`
   in `src/sim/mission.{h,cpp}`; `Order.wait` + `World::orderWait`/`patrolTo` back the
   pacing. Exercised by `tools/missiontool` (WIN/LOSE/TRIGGER-march all pass).
2. **Data-driven win/lose.** ✅ `.ota` `VictoryCondition_*`/`DefeatCondition_*` →
   evaluator ticked in the sim; scripted `SET_UNIT_VALUE(2,0/1)` honoured. Win and
   lose paths pass in `missiontool`. (Condition *guards* — don't win before the
   target existed — still TODO.)
3. **Mission players + AI (the §6 decision).** ✅ `PlayerN` slots parsed into
   `setupMission` (`src/sim/matchsetup.cpp`); enemies are script-driven (no skirmish
   AI). Server hosts a mission referee (`server.cpp`), client builds the same
   deterministic world (`startMpGame`), and both run the in-sim god script in
   lockstep — the mission is authoritative on the server, which broadcasts
   `MissionOutcome` (kNetVersion 17). Headless driver: `taclient … --mpmission <stem>`.
   First-pass diplomacy (opponents team 1, everyone else allied); proper
   neutral/ally roles and non-zero human slots are TODO.
4. **Per-mission unit restriction.** ✅ `missions/<stem>.tdf` (a list of allowed unit
   ids) is loaded into `missionAllowed_` on launch; `GameView::conjureMenu` intersects
   `registry_.buildable(builder)` with it, so a builder only offers permitted units
   (the click handler reads the same filtered `iconRects_`). UI-only; empty/absent =
   unrestricted. Verified: mission 1 = 10 allowed types; a real conjuror's 9-unit menu
   drops to 8 where a unit isn't permitted (takmission43_ph/47_ph).
5. **Campaign spine.** ✅ `camps/*.tdf` loader (`src/campaign/campaign.{h,cpp}`,
   `loadCampaigns` — Book of Darien 48, The Iron Plague 25, ipalt 25); progress
   persisted in `ta::Settings` (`campaignDone`, `campaign.<id>=n`); win→advance
   handled in `main()`.
6. **Front-end flow.** ✅ `Choice::Campaign` → `CampaignScreen`
   (`src/client/campaignscreen.{h,cpp}`): campaign tabs + completed/current(PLAY)/
   LOCKED rows. A pick runs the full sequence: intro movie (`MainMenu::playIntro` on
   `Movies/<stem>.bik`) → `BriefingScreen` (objectives from `missions/<stem>.txt`) →
   the autoMode-8 lockstep mission → `ResultScreen` (VICTORY/DEFEAT → next/retry/menu,
   chaining via `pendingCampaign` without bouncing through the menu). On victory
   `world_.missionOutcome()` drives the banner and bumps persisted progress. Our own
   block-font screens rather than the retail `Briefing`/`victory<kingdom>` `.gui`
   (deferred to polish). Modals auto-proceed under the dummy video driver (headless).
7. **Polish.** ✅ mostly done:
   - `SetMission` `wa` (ambush hold, `Order.waitAttack`) + `b` (timed reinforcement,
     `MissionScript::pendingSpawns_`) verbs.
   - Condition guards (`Cond::armed`) so a mission can't resolve before its target
     exists; compacted-slot diplomacy (opponents team 1, human/allies/neutrals team 0),
     shared by placements and the script's Create refs.
   - In-mission objectives panel (`GameView::drawObjectivesPanel`, O to toggle);
     briefing VO (`Sounds/<stem>.wav`); `posttakmission24`-style post-mission cutscenes
     (`post<stem>.bik`) + end-of-campaign credits (`PostTakCredits.bik`/`CREDITS.BIK`);
     `ipalt` folded into Iron Plague as an alt-ending branch row.
   - **Iron Plague dialogue widget — NOT implementable against this install.** Mission
     cobs contain no `PLAY_SOUND`, there are no `Sounds/takx*.wav`, the `.ota` has no
     message/dialogue fields, and there is no message MAP_COMMAND (§2). In-mission
     character dialogue has no script/data hook here; mission narrative is carried by
     the briefing VO where a `Sounds/<stem>.wav` ships. Left for a future asset set.
   - Still deferred: retail `.gui` briefing/victory art (our block-font screens stand
     in); commander-specific CommanderKilled; server-side enforcement of the unit
     restriction (today UI-only); non-zero human room slot.

Each phase is independently testable and lands behind the existing `--mission` /
`--mpmission` / menu paths before the front-end goes live.
