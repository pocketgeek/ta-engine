#!/usr/bin/env python3
"""legion_scale -- the Legion 10,000 scale suite (phase 0).

  legion_scale.py run    --bin BUILD/legion_scenario --out DIR [--files GLOB]... [--modes legion,retail]
                         [--retail-files GLOB]... [--cores 4-23] [--per-job 4] [--workers 3] [--data DIR]
                         [--timeout SECONDS] [--no-phase]
  legion_scale.py report --results DIR/results.jsonl [--markdown]
  legion_scale.py check  --results DIR/results.jsonl --targets tools/scenarios/scale/targets.json
                         [--base OLD/results.jsonl]

run: every tools/scenarios/scale/*.scn (or --files) in each mode, ONE run each at offset 0, workers exec
(the sim's worker pool at --workers threads, TAK_SIM_WORKERS, as a 24-thread machine runs a real game),
each job pinned to its own --per-job cores. Retail runs only the --retail-files (default: the 100 and
1000 files plus one-10000 and many-10000). A job writes DIR/runs/NAME.MODE.{jsonl,err}; results.jsonl
gets one line per job: the scenario's JSON keys, its SCALE_WALL wall line (tick mean/p50/p99/max,
ticks over the 33.3 ms budget, peak RSS, malloc heap at the end) and, unless --no-phase, the per-phase
wall time from TAK_PHASE's SIMPHASE lines (sum and p99 per phase over the run). The machine (CPU model,
compiler, build type) goes in DIR/machine.json. Wall time is never a gate: it is reported for A/B runs
on the same machine (CLAUDE.md: timing on cores 0-3 via the user's timing tool when it matters).

report: one table row per job (the acceptance keys).
check: the frozen targets (targets.json: per scenario/mode/key a min and/or max; key "hash" with "eq" for an
exact state hash; "wall.FIELD" or "report_only" targets print but never fail) against a run; with --base,
every deterministic key that moved
against an older run (among the keys both carry) is listed too. Exit 0 every target met, 1 a target missed or its scenario/mode not in
the run, 2 tooling.
"""
import argparse
import fnmatch
import glob
import json
import os
import platform
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SCALE = os.path.join(HERE, "scenarios", "scale")
DEFAULT_RETAIL = ["one-100", "one-1000", "many-100", "many-1000", "swap-1000", "bridges-1000", "maze-1000",
                  "mixed-1000", "attack-1000", "churn-1000", "one-10000", "many-10000"]
PHASES = ["tick", "combat", "sep", "explore", "vis", "burn", "grid", "scripts", "movement", "nav", "other", "prologue",
          "compact"]


def cores(spec):
    out = []
    for part in spec.split(","):
        a, _, b = part.partition("-")
        out += list(range(int(a), int(b or a) + 1))
    return out


def machine(binary):
    info = {"platform": platform.platform(), "python": platform.python_version()}
    try:
        for ln in open("/proc/cpuinfo"):
            if ln.startswith("model name"):
                info["cpu"] = ln.split(":", 1)[1].strip()
                break
        info["cpus"] = os.cpu_count()
    except OSError:
        pass
    try:
        info["compiler"] = subprocess.run(["c++", "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except (OSError, IndexError):
        pass
    cache = os.path.join(os.path.dirname(os.path.abspath(binary)), "CMakeCache.txt")
    if os.path.exists(cache):
        for ln in open(cache):
            for key in ("CMAKE_BUILD_TYPE", "CMAKE_CXX_FLAGS", "CMAKE_CXX_COMPILER"):
                if ln.startswith(key + ":"):
                    info[key] = ln.split("=", 1)[1].strip()
    try:
        info["governor"] = open("/sys/devices/system/cpu/cpu4/cpufreq/scaling_governor").read().strip()
    except OSError:
        pass
    return info


def parse_err(text):
    wall, phases = {}, {}
    m = re.search(r"^SCALE_WALL (\S+) (\S+) offset=(-?\d+) (\S+) (.*)$", text, re.M)
    if m:
        for kv in m.group(5).split():
            k, _, v = kv.partition("=")
            v = v.split("@")[0]
            try:
                wall[k] = float(v) if "." in v else int(v)
            except ValueError:
                wall[k] = v
    vals = {p: [] for p in PHASES}
    for ln in text.splitlines():
        if not ln.startswith("SIMPHASE "):
            continue
        for kv in ln.split()[1:]:
            k, _, v = kv.partition("=")
            if k in vals:
                try:
                    vals[k].append(float(v.rstrip("ms")))
                except ValueError:
                    pass
    for p, v in vals.items():
        if v:
            s = sorted(v)
            phases[p] = {"sum_ms": round(sum(v), 1), "mean_ms": round(sum(v) / len(v), 3),
                         "p99_ms": round(s[(len(s) * 99 + 99) // 100 - 1], 3), "max_ms": round(s[-1], 3)}
    return wall, phases


def cmd_run(a):
    files = []
    for g in a.files or [os.path.join(SCALE, "*.scn")]:
        files += sorted(glob.glob(g))
    names = [os.path.basename(f)[:-4] for f in files]
    retail = a.retail_files or DEFAULT_RETAIL
    modes = a.modes.split(",")
    jobs = []
    for f, n in zip(files, names):
        for m in modes:
            if m == "retail" and not any(fnmatch.fnmatch(n, r) for r in retail):
                continue
            jobs.append((f, n, m))
    # Longest first: ticks x bodies, from the file.
    def weight(j):
        txt = open(j[0]).read()
        t = int(re.search(r"^ticks (\d+)", txt, re.M).group(1))
        n = sum(int(x) for x in re.findall(r"^group \S+ \d+ \S+ (\d+)", txt, re.M))
        return -(t * n * (3 if j[2] == "retail" else 1))
    jobs.sort(key=weight)
    os.makedirs(os.path.join(a.out, "runs"), exist_ok=True)
    json.dump(machine(a.bin), open(os.path.join(a.out, "machine.json"), "w"), indent=1)
    cl = cores(a.cores)
    slots = [cl[i:i + a.per_job] for i in range(0, len(cl) - a.per_job + 1, a.per_job)]
    if not slots:
        raise ValueError(f"--cores {a.cores} has fewer than --per-job {a.per_job} cores: no job slot")
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("TAK_") and k not in ("TAK_SETTINGS_FILE",):
            env.pop(k)
    env["TAK_SIM_WORKERS"] = str(a.workers)
    if not a.no_phase:
        env.update(TAK_PHASE="1", TAK_PHASE_MS="0")
    free, procs, t0 = list(range(len(slots))), {}, time.time()
    while jobs or procs:
        while jobs and free:
            f, n, m = jobs.pop(0)
            s = free.pop(0)
            base = os.path.join(a.out, "runs", f"{n}.{m}")
            for stale in (".timeout", ".rc"):
                if os.path.exists(base + stale):
                    os.remove(base + stale)
            argv = ["taskset", "-c", ",".join(map(str, slots[s])), a.bin, f, "--mode", m, "--workers", "--offsets", "0",
                    "--json"]
            if a.data:
                argv += ["--data", a.data]
            if a.no_vertex_probe:
                argv += ["--no-vertex-probe"]   # as the game (stage 2): the Stats-only vertex probe off
            p = subprocess.Popen(argv, stdout=open(base + ".jsonl", "w"), stderr=open(base + ".err", "w"), env=env)
            procs[p] = (n, m, s, time.time())
            print(f"{int(time.time() - t0):6d}s start {n} {m} cores {slots[s][0]}-{slots[s][-1]}", flush=True)
        for p in list(procs):
            n, m, s, st = procs[p]
            if p.poll() is None and time.time() - st > a.timeout:
                p.kill()
                p.wait()
                open(os.path.join(a.out, "runs", f"{n}.{m}.timeout"), "w").write(str(a.timeout))
            if p.poll() is not None:
                procs.pop(p)
                free.append(s)
                open(os.path.join(a.out, "runs", f"{n}.{m}.rc"), "w").write(str(p.returncode))
                print(f"{int(time.time() - t0):6d}s done {n} {m} rc={p.returncode} {int(time.time() - st)}s", flush=True)
        time.sleep(0.5)
    with open(os.path.join(a.out, "results.jsonl"), "w") as o:
        for n in sorted(set(names)):
            for m in modes:
                base = os.path.join(a.out, "runs", f"{n}.{m}")
                if not os.path.exists(base + ".rc"):
                    continue
                rc = int(open(base + ".rc").read())
                line = open(base + ".jsonl").readline()
                rec = {"scenario": n, "mode": m, "rc": rc, "timeout": os.path.exists(base + ".timeout")}
                if line.strip():
                    d = json.loads(line)
                    rec.update(units=d.get("units"), ticks=d.get("ticks"), hash=d.get("hash"), keys=d.get("keys"))
                rec["wall"], rec["phases"] = parse_err(open(base + ".err").read())
                o.write(json.dumps(rec, sort_keys=True) + "\n")
    print("elapsed", int(time.time() - t0), "s", flush=True)
    return 0


def load(path):
    return [json.loads(ln) for ln in open(path) if ln.strip()]


COLS = [("units", None), ("deliver.max", "scale.deliver.max"), ("start.last", "scale.start.last"),
        ("wait.max", "scale.wait.max"), ("arrive.n", "scale.arrive.n"), ("arr.t50", "scale.arrive.t50"),
        ("arr.t90", "scale.arrive.t90"), ("arr.t99", "scale.arrive.t99"), ("arr.last", "scale.arrive.last"),
        ("compl.n", "scale.complete.n"), ("compl.t99", "scale.complete.t99"), ("stuck", "scale.stuck_end"),
        ("livelock", "scale.livelock_end"), ("path.p90", "scale.path_x1000.p90"),
        ("groups", "scale.nav.groups_peak"), ("navMB", "scale.nav.bytes_peak")]


def cmd_report(a):
    rows = load(a.results)
    head = ["scenario", "mode"] + [c for c, _ in COLS] + ["tick.mean", "tick.p99", "tick.max", "over33", "rssMB",
                                                         "nav.p99", "move.p99", "apply.max"]
    out = []
    for r in rows:
        k = r.get("keys") or {}
        cells = [r["scenario"], r["mode"] + ("(T/O)" if r.get("timeout") else "")]
        for c, key in COLS:
            if key is None:
                cells.append(str(r.get("units", "")))
            elif key == "scale.nav.bytes_peak":
                cells.append(f"{k[key] / 1048576:.0f}" if key in k else "")
            else:
                cells.append(str(k.get(key, "")))
        w, ph = r.get("wall", {}), r.get("phases", {})
        cells += [f"{w.get('tick_mean_ms', '')}", f"{w.get('tick_p99_ms', '')}", f"{w.get('tick_max_ms', '')}",
                  f"{w.get('over_budget', '')}",
                  f"{w['rss_peak_kb'] / 1024:.0f}" if isinstance(w.get("rss_peak_kb"), int) else "",
                  f"{ph.get('nav', {}).get('p99_ms', '')}", f"{ph.get('movement', {}).get('p99_ms', '')}",
                  f"{w.get('apply_max_ms', '')}"]   # the longest tick of applyCommand (stage 3's membership application)
        out.append(cells)
    if a.markdown:
        print("| " + " | ".join(head) + " |")
        print("|" + "---|" * len(head))
        for c in out:
            print("| " + " | ".join(c) + " |")
    else:
        widths = [max(len(h), *(len(c[i]) for c in out)) for i, h in enumerate(head)]
        print("  ".join(h.rjust(wd) for h, wd in zip(head, widths)))
        for c in out:
            print("  ".join(x.rjust(wd) for x, wd in zip(c, widths)))
    return 0


def cmd_check(a):
    rows = {(r["scenario"], r["mode"]): r for r in load(a.results)}
    targets = json.load(open(a.targets))
    fails, met, missing, reported = [], 0, [], []
    for t in targets["targets"]:
        r = rows.get((t["scenario"], t["mode"]))
        if r is None:
            missing.append(f'{t["scenario"]} {t["mode"]}')
            continue
        k = r.get("keys") or {}
        if t["key"] == "hash":   # exact: the state hash at offset 0 (Retail must not move)
            v = (r.get("hash") or {}).get("0")
            ok = v == t["eq"]
        elif t["key"].startswith("wall."):   # wall time: SCALE_WALL fields, always report-only
            v = (r.get("wall") or {}).get(t["key"][5:])
            ok = v is not None and ("max" not in t or v <= t["max"]) and ("min" not in t or v >= t["min"])
        else:
            v = k.get(t["key"])
            ok = v is not None
            if ok and "max" in t:
                ok &= v != -1 and v <= t["max"]
            if ok and "min" in t:
                ok &= v >= t["min"]
        bound = " ".join(f"{b}={t[b]}" for b in ("min", "max", "eq") if b in t)
        line = f'{t["scenario"]} {t["mode"]} {t["key"]} = {v} (target {bound}; {t.get("why", "")})'
        if ok:
            met += 1
        elif t.get("report_only") or t["key"].startswith("wall."):
            reported.append(line)
        else:
            fails.append(line)
    print(f"targets met {met}, missed {len(fails)}, report-only missed {len(reported)}, not run {len(missing)}")
    for f in fails:
        print("  MISS", f)
    for f in reported:
        print("  MISS (report only)", f)
    for m in missing:
        print("  NOT RUN", m)
    if a.base:
        base = {(r["scenario"], r["mode"]): r for r in load(a.base)}
        for key, r in sorted(rows.items()):
            b = base.get(key)
            if not b:
                continue
            # Only keys both runs carry: a trimmed base (baseline-*.jsonl) keeps a subset of the keys.
            moved = [(k, b["keys"][k], v) for k, v in (r.get("keys") or {}).items() if k in b["keys"] and b["keys"][k] != v]
            if moved or r.get("hash") != b.get("hash"):
                print(f"  MOVED {key[0]} {key[1]}: hash {b.get('hash')} -> {r.get('hash')}; {len(moved)} keys")
    return 1 if fails or missing else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--bin", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--files", action="append")
    r.add_argument("--retail-files", action="append")
    r.add_argument("--modes", default="legion,retail")
    r.add_argument("--cores", default="4-23")
    r.add_argument("--per-job", type=int, default=4)
    r.add_argument("--workers", type=int, default=3)
    r.add_argument("--data")
    r.add_argument("--timeout", type=int, default=5400)
    r.add_argument("--no-phase", action="store_true")
    r.add_argument("--no-vertex-probe", action="store_true",
                   help="pass --no-vertex-probe (a binary from stage 2 on): the W8 vertex probe off, as in the game")
    p = sub.add_parser("report")
    p.add_argument("--results", required=True)
    p.add_argument("--markdown", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("--results", required=True)
    c.add_argument("--targets", required=True)
    c.add_argument("--base")
    a = ap.parse_args()
    try:
        return {"run": cmd_run, "report": cmd_report, "check": cmd_check}[a.cmd](a)
    except (OSError, ValueError, KeyError) as e:
        print("legion_scale:", e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
