Harvested situations -- NOT FAITHFUL ENOUGH TO GATE ON (PLAN 3.8 step 9, A11 unmet)
===================================================================================

Six moments cut out of the user's recordings by TAK_SITUATION (src/client/situation.h),
on the builds that recorded them (legion-r9 on 02aa55a, legion-r10 on 7aae704; both
replay exactly there). Each file is the world at the snapshot tick (every live body with
its exact position, heading, hit points, individual speed, recorded id; the exploration
masks; the game clock and RNG; the orders in flight re-issued at tick 0), the next 600
ticks of the human commands re-clustered into clicks, and the recording's own positions
100 and 300 ticks in (`truth`). Data-gated (fbi types and a gen1 map): they need --data.

The plan's rule is that a situation is committed to the gates only if (1) at +300 ticks at
least 90% of its bodies are within 2 cells of the recording's, measured on the recording
build (tools/scn_truth.cpp, built in that tree), and (2) its symptom reproduces within 30%.
No moment clears (1) at +300, so these files are kept here, outside tools/scenarios/*.scn
(the baseline, the gates and the nightly never look in this directory), as a record and as
a starting point. Nothing gates on them. Delete the directory if the decision is to rely on
the hand-built twins (corner, split, battle-assault) instead.

Fidelity, per mille of bodies within 2 cells of the recording (all bodies / bodies the
recording moved more than 2 cells), scn_truth on the recording build, start offset 0:

  moment        snapshot  bodies   +100 ticks       +300 ticks
  r9  36060     36056       356    960 / 929        853 / 711
  r9  65850     65844       709    881 / 816        703 / 600
  r9  61930     61922       667    838 / 823        623 / 538
  r10 46292     46291       425    842 / 772        520 / 407
  r10 87332     87331      1033    842 / 746        621 / 475
  r10 104540    104540     1279    942 / 881        747 / 698

At +100 two moments clear 90% (r9 36060, r10 104540); at +300 none does (best 853). The
same scn on the head gives the same numbers for r9 36060 (960 / 853), so the loss is in the
rebuild, not in what Legion has changed since.

What was tried, each with no effect on the +300 number of r9 36060 (853 before and after,
or +/- 3): the recorded exploration masks, the game clock and RNG state (and perturbing the
RNG by 1, 2, 3), ids that are the recorded ids, stances (no body had one), a velocity for
the bodies already moving, a 1/65536 px nudge of every body. A per-tick comparison on the
recording build shows the first deviation at tick +1: 8 of the 356 bodies were already in
flight at the snapshot (a wander step, an arrival), their Legion group and mission state is
not in the file, and the difference grows through contacts (23 bodies off by more than
0.01 cell at +5, 36 at +8, 132 at +300). A faithful file would need that hidden state
(Legion groups and member states, mission state machines, script threads).

Symptom on the head (legion_scenario --mode legion, 3000 ticks, offset 0), observer
`stopped_permille` against the audit's held% (speed-0 samples of the ordered members; the
two definitions differ, see docs/legion-scenario-keys.md):

  moment        head    audit held%   within 30%
  r9  36060     268     284           yes
  r9  65850     237     367           no (-35%)
  r9  61930     275     273           yes
  r10 46292     328     294           yes
  r10 87332     377     401           yes
  r10 104540    242     (counter bar: fieldWork per window, no reference measured; head
                       work.field_work.total 51.4M over 3000 ticks, p99 132k per tick)

To re-cut a moment: build the recording's tree, copy src/client/situation.h and
tools/{legion_scn.h,legion_issue_selection.h,scn_truth.cpp} into it, add the three World
accessors of this commit (gameRngState, resumeClocks, setNextUnitId) and the arming block of
src/client/main.cpp, then
  TAK_REPLAY_VERIFY=1 TAK_SITUATION=<tick>:<out.scn> takclient replay <replay.takrep> --data <install>
and check the result with that tree's scn_truth.
