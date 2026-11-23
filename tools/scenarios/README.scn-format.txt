.scn scenario format (version 1)
================================

Parser, world builder and order feed: tools/legion_scn.h. Orders go through
tools/legion_issue_selection.h, which mirrors the client's HUD commands and its
uplink: in Legion each click is one explicit selection order (protocol 267),
landing whole in its tick; in Retail one command per unit, 64 a tick. Checked by
the issue_selection_test ctest.

One directive per line. '#' starts a comment (not inside an ascii map block).
Coordinates are map cells (16 px), x then z; points may be fractional. A file
never names a pathfinding mode: the runner builds it once per mode.

  scn 1                         required first line
  name NAME
  ticks N                       run length (default 3000)
  seed N                        game seed (default 7)
  players N                     1..8 (default 2); player p starts on team p
  team P T
  weapons on|off                default for groups (default off)
  explored all|none             pre-explore the nav map (default all)
  explored rle COUNT:HEX ...    the recording's per-cell owner masks, run-length coded over the whole
                                map (situations; the count must cover the map)
  wanderers on|off              fbi types keep (default) or lose Standby_wander,
                                the home-pull that re-orders idle wanderers every
                                240 ticks (legion_scenario --wanderers overrides)
  crusades on|off               load the Crusades balance overlay for fbi types
                                (default off)
  uplink R                      command window: at most 512 commands
                                outstanding, round trip R ticks (default: none,
                                only the server's 64-per-tick drain); explicit
                                orders (Legion clicks) have their own window of 8
                                orders and do not wait for it
  navbytes N                    Legion only, tests: the navigator's field byte allowance
                                (LegionNavigator::setFieldBytesForTest; never below two
                                whole-map fields; default NavBudget::kFieldBytes): memory
                                pressure on a small map (legion_scenario --field-bytes
                                overrides)
  gateoffsets O,O,...           the start offsets legion_scenario --check / --baseline
                                run by default when the gate's eleven (0,+-1..+-5)
                                put a spawn off the map; at least nine, the core
                                five 0,+-1,+-2 among them (tools/legion_check.py)
  probe approach                the runner also reports the approach.* keys: per
                                unit, the first tick Legion holds it at its
                                approach point (an unreachable goal's nearest
                                reachable spot) and the tick its orders empty.
                                Meant for scenarios whose goal is unreachable.
  probe aware                   Legion only: the runner reports the awareness / parting / give-way counters of
                                LegionNavigator::Stats as aware.* keys (parts, encounters, replans, builds,
                                latency_max / _sum / _n, work, work_max, giveway_starts / _ticks / _timeouts;
                                W7 step 0) and the route-behind metric of every `behind` shape.
  probe lanes                   Legion only: the runner reports the lane counters of LegionNavigator::Stats as
                                lanes.* keys (pivot_calls, pivot_work, pivot_sweeps: the pinwheel's steering
                                work, the base the S1 corner lanes are gated against; lane_work, lane_clipped:
                                0 until W8 step 3 builds the lanes; W8 step 0).
  probe w9                      Legion only: the runner reports the W9 instruments (W9 step 0): w9.* (the twelve
                                never-hashed mechanism counters of LegionNavigator::Stats: seal_settles,
                                creep_presses, deep_rechoices, anchor_advances, gate_lane_moves, gate_holds,
                                fold_binds, turn_guard_rejects, clear_aims, meet_latches,
                                order_inversions_avoided / _taken; 0 until their step builds them) and, in a
                                debug build (NDEBUG undefined), census.* (how often each mechanism's trigger
                                WOULD fire, definitions in legion.cpp's struct Census). `legion_scenario --w9`
                                turns it on for any file; `--trace ID[,ID...]` prints one stderr line per
                                decision of those members (debug build).
  probe scale                   the runner also reports the scale.* keys (tools/legion_scale.h; the
                                Legion 10,000 scale suite in tools/scenarios/scale/): per body, from its
                                last click, command delivery, first movement, physical arrival in the
                                click's packed disc vs order completion (t50/t90/t99/last), path length,
                                stuck/livelocked bodies at the end, and (Legion) the longest wait for a
                                field and the navigator's live containers. `legion_scenario --scale` turns
                                it on for any file; the wall line SCALE_WALL (stderr) adds tick mean/p50/
                                p99/max, ticks over 33.3 ms, peak RSS and malloc heap.
  probe claims                  Legion only: every 10 ticks the navigator audits
                                its arrival-slot claims (the claims invariant);
                                keys claims.bad_max (overlapping + lost +
                                dangling claims, 0 where it holds) and the
                                report-only claims.*_max / orphans_end.

Map (exactly one):
  map ascii                     rows follow, closed by a line 'end':
                                  .  ground (height 100; sea level is 64)
                                  #  wall (1x1 blocking map feature)
                                  ~  deep water (20)   ,  shallow water (56)
                                  ^  high ground (200)
  map flat W H                  open ground
  map gen1 RECIPE               tak::mapgen::generate("~gen1~...")
  map snapshot FILE             a .tnt or .kmp, relative to the .scn file
  wall X Z W H                  ascii/flat only: a block of wall cells
  height X Z W H V              ascii/flat only: set terrain height V

Types:
  type NAME mover FOOT TURN ACCEL SPEED [opts]   ACCEL px/tick^2, SPEED px/tick
                                (SPEED 0: the Keep, canmove with no velocity)
  type NAME flyer FOOT TURN ACCEL SPEED [cruise=80] [opts]   (VTOL standby)
  type NAME boat  FOOT TURN ACCEL SPEED [depth=13] [opts]
  type NAME hover FOOT TURN ACCEL SPEED [opts]
  type NAME structure FOOTX FOOTZ [opts]
  type NAME fbi UNITNAME                         the install's unit (data-gated)
  type NAME roster [opts]       the retail roster's quartiles, cycled by member
                                index: sight 256/320/400, turn 1000/2500,
                                footprint 2/3, speed 1.6..2.6; gun 160,25,1
  opts: sight=N hp=N footz=N turninplace=N gun=RANGE,DAMAGE,RELOAD
  An armed synthetic type without gun= carries the fixtures' gun (120,100,0.1).
  An unarmed fbi type is a copy outside the registry (legacy nav grid).

Groups (spawned in file order, members in row-major order):
  group NAME OWNER TYPE COUNT rect X0 Z0 X1 Z1 [pitch=N] [opts]
                                fills [X0,X1) x [Z0,Z1) every pitch cells
                                (default: largest footprint + 1)
  group NAME OWNER TYPE COUNT cells X,Z X,Z ... [opts]
  group NAME OWNER TYPE COUNT spots X,Z,HEADING,HP[,SPEED] ... [opts]
                                harvested bodies, exact: raw 16.16 px position, raw BAM heading
                                0..65535, raw 16.16 hit points (0 = full), raw 16.16 individual
                                speed (0 = the spawn roll)
  opts: squad=fN|gN             Alt+N / Ctrl+N pressed at tick 0 (one press
                                per number, over every group naming it)
        weapons=on|off

Churn (static-map edits; the world changes mid-run, unlike every other directive):
  churn X Z W H every=N [walk=DX,DZ,ROW] [toggle] [from=T] [until=T]
                                a blocking feature of W x H cells placed every N
                                ticks from tick T (default 0) to U (default the
                                end), before that tick's orders. Event k lands on
                                cell (X + DX*(k % ROW), Z + DZ*(k / ROW)); with
                                'toggle' even events place and odd events lift
                                it again, and the cell advances every two events.

Orders (stable-sorted by tick; issued before World::tick at that tick):
  at TICK move|fight|patrol SEL X Z [queue]
  at TICK move|fight|patrol SEL @GROUP [queue]   the group's centroid then
  at TICK attack|guard SEL @GROUP [queue]        its first live member
  at TICK stop SEL
  at TICK squad SEL fN|gN|none [append]
  SEL is a comma list of groups (selection order = list order, then spawn
  order) or 'all'; one order's selection belongs to one player. A token
  %N is body N (0-based, across the groups in file order): a harvested click
  keeps the recording's selection order, which a group list cannot say.
  move = right-click on ground (Legion: surface movers share the point; flyers,
  bodies over 8 and every unit in other modes keep their offset from the
  selection centroid, clamped to +-60 px per axis). fight/patrol/guard = the
  armed F/P/G order, attack = right-click on an enemy, stop = the Stop key.

Situations (tools/scenarios/situation-*.scn, cut by TAK_SITUATION; src/client/situation.h):
  clock TICK RNG                start the world on the recording's tick counter and game RNG
                                (World::resumeClocks, after every spawn)
  truth TICK X,Z ...            where the recording had every body (raw 16.16 px, group then
                                member order; INT32_MIN,INT32_MIN = dead) TICK ticks in; several
                                lines, ascending. The runner reports truth.tTICK.n,
                                .within2_permille and .moved_* (tools/scn_truth.cpp reports the
                                same on an old build).

Shapes (for the runner's metrics; never affect the world):
  gate NAME X0 Z0 X1 Z1         a segment
  line NAME X0 Z0 X1 Z1         a segment
  region NAME X0 Z0 X1 Z1       a rectangle
  gate options (a gate with unequal sides is a rectangle of centre cells; a segment
  is a strip 8 cells wide):
        lateral=x|z band=N mincount=N edge=N pairwindow=N flip=N
        the lateral axis (default: the longer side), the file width in cells (2),
        members inside for a files sample (3), the end-window depth (2), the
        ticks between a crossing pair's entries (600) and its flip distance
        (the body width)
  lane NAME BX0 BZ0 BX1 BZ1 AX0 AZ0 AX1 AZ1 [bsign=-1|1] [asign=-1|1] [window=N]
       [mincells=N] [across=all]
        the observer's lane-order probe: the first entry of each member into a
        line before a vertex and a line after it (each horizontal or vertical,
        cells floor(lo)..ceil(hi)-1; the lateral cell is x on a horizontal line, z
        on a vertical one, times its sign). Two members entering the first line
        within `window` ticks (300) swap when their order flips by at least
        `mincells` (2) at both lines. Pairs of one group, or of all groups with
        across=all. Keys lane.NAME.pairs / .swaps / .swaps_permille.
  hug NAME BX0 BZ0 BX1 BZ1 AX0 AZ0 AX1 AZ1 tip1=N tip2=N [cells=N]
        the observer's hug probe (the MV-13 S-bend, W8 step 0): the two lines are the lane probe's, one per
        vertex (each horizontal or vertical); tip1 / tip2 are the lateral cell of each vertex's tip (x on a
        horizontal line, z on a vertical one). A member hugs a vertex when its lateral cell on first
        entering the vertex's line is within `cells` (2) of the tip; it carries over when it hugs both.
        Keys hug.NAME.first / .second / .carry.
  reach NAME ATTACKERS TARGETS [range=PX] [marks=T,T]
        the AR-06 probe (comma lists of groups, a trailing * is a prefix): how
        many attackers ever get a shot at the targets, when, the damage dealt
        and what the rest do. In reach = within tickCombat's reach (weapon
        range, + 8*footprint + 24 px for a structure target); range=PX replaces
        it (a Guard's "within 6 cells"); marks add ever_tM / damage_tM at those
        ticks. Keys reach.NAME.first / t50 / ever_end / ever_tM / now_end /
        out600_end / out600_peak / farthest_held / hold_out_max / hold0_max /
        damage_end / damage_tM / targets_dead_end (tools/legion_scenario.cpp).
  creep NAME GROUPS
        the goal-area creep counter (MV-07): member-ticks the groups' ordered
        members spend inside their click's arrival disc stepping at <= cap/8.
        Keys creep.NAME.ticks / .inside_ticks.
  pair NAME LISTA LISTB [cells=N]
        proximity of live ordered members of LISTA x LISTB (comma lists of
        groups, a trailing * is a name prefix) sampled every 10 ticks: pairs
        whose centre cells are within `cells` (2) on both axes. Keys
        pair.NAME.pairs / .contacts / .permille_x100.

  behind NAME LATER EARLIER [cells=N]
        the route-behind metric (W7, user decision 5: the later of two crossing groups routes behind the
        other's stream and never waits at its edge). LATER and EARLIER are comma lists of groups (a trailing * is
        a prefix). Per LATER member: the path walked while ordered against the straight line from its first
        ordered position to its click. A WAIT: a LATER member with orders stood (a step of at most an eighth of
        its speed) with a moving EARLIER member within `cells` (8) cells; a group wait is a tick on which at
        least 2 and a quarter of the LATER members with orders do so; a wait is a run of 30 such ticks. Keys
        behind.NAME.members / .path_cells / .straight_cells / .extra_cells / .detour_permille /
        .wait_member_ticks / .wait_ticks / .wait_run_max / .waits (tools/legion_observe.h BehindProbe).

Start offset: the builder shifts every spawn by 0, +1 or -1 cells on both
axes (BuildOptions::offset); orders, walls and shapes stay put.

Data gating: fbi types, gen1 and snapshot maps need an install (--data, or a
mounted view). Without one, build() returns Built::skipped =
"needs --data: <reason>" and builds nothing.
