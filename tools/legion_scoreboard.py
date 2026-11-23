#!/usr/bin/env python3
"""Legion vs Retail scoreboard from crowdbench screen rows (tools/crowdbench_screen.py).

  legion_scoreboard.py SCREEN.jsonl [MORE.jsonl ...] [--md OUT.md] [--tolerance 0.02]

Both modes come from the same rows, i.e. the same binary: Retail is re-run on
the build under test, never read from a frozen file (the removed-mode baseline
the scratch scoreboard used is gone). A case is (scenario, units, players,
moving percent, ticks, turn rate); values are means over the seeds present.

Judged metrics (Legion counts as worse only beyond the tolerance):
  higher is better  crossed_middle, arrived_settled
  lower is better   spin_unit_ticks, class_terrain_stuck_unit_ticks, final_terrain_stuck,
                    final_open_idle, trapped_units_moving_after_grace,
                    path_optimality_ratio_mean, stalled_no_progress (Retail age
                    classes waiting + parked, no_progress part), arrival_tick_p50,
                    arrival_tick_p95 (-1 = never reached: worse than any tick)
Reported, not judged:
  stalled_held_by_design (the deliberate holds the old stalled_unit_ticks
  wrongly counted as losses), cap8_moving_samples, complete_outside_radius,
  cross_gap_max.
"""
import argparse
import collections
import json
import math
import statistics
import sys

HIGHER = ("crossed_middle", "arrived_settled")
LOWER = ("spin_unit_ticks", "class_terrain_stuck_unit_ticks", "final_terrain_stuck", "final_open_idle",
         "trapped_units_moving_after_grace", "path_optimality_ratio_mean", "stalled_no_progress",
         "arrival_tick_p50", "arrival_tick_p95")
CENSORED = ("arrival_tick_p50", "arrival_tick_p95")   # -1 means the milestone was never reached
REPORTED = ("stalled_held_by_design", "cap8_moving_samples", "complete_outside_radius", "cross_gap_max")
# Acceptance rows (report only, no bound): the five acceptance scenarios and
# opposingcolumns, whose spin at 200x1 is the row T8 added (177 at b8a4110).
ACCEPTANCE_SCENARIOS = ("jagged", "trapped", "crowdtrap", "singleunit", "groupdetour", "opposingcolumns")
ACCEPTANCE_METRICS = ("spin_unit_ticks", "spin_while_crowd_held_unit_ticks", "units_ever_terrain_stuck",
                      "trapped_units_moving_after_grace", "arrived_settled")


def derived(row):
    """Row values by metric name, with the stalled split folded in."""
    values = dict(row)
    parts = ("age_waiting_{}_unit_ticks", "age_parked_{}_unit_ticks")
    for name, kind in (("stalled_no_progress", "no_progress"), ("stalled_held_by_design", "held_by_design")):
        keys = [part.format(kind) for part in parts]
        if all(isinstance(row.get(key), (int, float)) for key in keys):
            values[name] = sum(row[key] for key in keys)
    return values


def value_of(values, metric):
    value = values.get(metric)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if metric in CENSORED and value < 0:
        return math.inf
    if value < 0:
        return None   # -1 = not measured (e.g. no optimal path computed)
    return value


def load(paths):
    cases = collections.defaultdict(lambda: collections.defaultdict(lambda: collections.defaultdict(list)))
    for path in paths:
        with open(path) as handle:
            for line in handle:
                if not line.strip():
                    continue
                row = json.loads(line)
                if "error" in row or "mode" not in row:
                    continue
                key = (row["scenario"], row["units_per_player"], row["players"], row.get("moving_percent", 100),
                       row["ticks"], row.get("turn_rate"))
                values = derived(row)
                for metric in set(HIGHER + LOWER + REPORTED + ACCEPTANCE_METRICS):
                    value = value_of(values, metric)
                    if value is not None:
                        cases[key][row["mode"]][metric].append(value)
    return cases


def mean(values):
    return math.inf if any(v == math.inf for v in values) else statistics.mean(values)


def judge(legion, retail, metric, tolerance):
    """True when Legion is worse than Retail beyond the tolerance."""
    if legion == retail:
        return False
    if math.inf in (legion, retail):
        return legion == math.inf   # never reached vs reached
    slack = max(1e-9, abs(retail) * tolerance)
    return legion < retail - slack if metric in HIGHER else legion > retail + slack


def fmt(value):
    if value == math.inf:
        return "never"
    return f"{value:,.0f}" if float(value).is_integer() else f"{value:,.3f}"


def scoreboard(cases, tolerance):
    wins, losses, lines, reported = collections.Counter(), collections.Counter(), [], []
    for key in sorted(cases, key=lambda k: tuple(str(part) for part in k)):
        legion, retail = cases[key].get("legion"), cases[key].get("retail")
        if not legion or not retail:
            continue
        name = f"| {key[0]} | {key[1]}x{key[2]}@{key[3]} | {key[4]} |"
        for metric in HIGHER + LOWER:
            if not legion.get(metric) or not retail.get(metric):
                continue
            lv, rv = mean(legion[metric]), mean(retail[metric])
            if judge(lv, rv, metric, tolerance):
                losses[metric] += 1
                lines.append(f"{name} {metric} | {fmt(lv)} | {fmt(rv)} |")
            else:
                wins[metric] += 1
        cells = []
        for metric in REPORTED:
            lv = fmt(mean(legion[metric])) if legion.get(metric) else "-"
            rv = fmt(mean(retail[metric])) if retail.get(metric) else "-"
            cells.append(f"{lv} / {rv}")
        reported.append(f"{name} " + " | ".join(cells) + " |")
    out = [f"# Legion scoreboard (Legion vs Retail on the same binary; mean over seeds; "
           f"{tolerance * 100:g}% tolerance)", "",
           "| metric | legion >= retail | legion worse |", "|---|---|---|"]
    out += [f"| {metric} | {wins[metric]} | {losses[metric]} |" for metric in HIGHER + LOWER]
    out += ["", "## Cases where Legion is worse than Retail", "",
            "| scenario | population | ticks | metric | legion | retail |", "|---|---|---|---|---|---|"] + lines
    out += ["", "## Reported (legion / retail), not judged", "",
            "| scenario | population | ticks | " + " | ".join(REPORTED) + " |",
            "|---|---|---|" + "---|" * len(REPORTED)] + reported
    out += ["", "## Acceptance rows (legion / retail), report only", "",
            "| scenario | population | ticks | " + " | ".join(ACCEPTANCE_METRICS) + " |",
            "|---|---|---|" + "---|" * len(ACCEPTANCE_METRICS)]
    for key in sorted(cases, key=lambda k: tuple(str(part) for part in k)):
        legion, retail = cases[key].get("legion", {}), cases[key].get("retail", {})
        if key[0] not in ACCEPTANCE_SCENARIOS or not legion:
            continue
        cells = [f"{fmt(mean(legion[m])) if legion.get(m) else '-'} / {fmt(mean(retail[m])) if retail.get(m) else '-'}"
                 for m in ACCEPTANCE_METRICS]
        out.append(f"| {key[0]} | {key[1]}x{key[2]}@{key[3]} | {key[4]} | " + " | ".join(cells) + " |")
    return "\n".join(out) + "\n", sum(losses.values())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("rows", nargs="+", help="crowdbench screen JSONL files")
    parser.add_argument("--md", help="also write the markdown here")
    parser.add_argument("--tolerance", type=float, default=0.02)
    args = parser.parse_args(argv)
    text, _ = scoreboard(load(args.rows), args.tolerance)
    if args.md:
        with open(args.md, "w") as handle:
            handle.write(text)
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
