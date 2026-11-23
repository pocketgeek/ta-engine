#!/usr/bin/env python3
"""Matched AA-off renderer measurements with an optimized Debug takclient.

The reference switch restores tick-based animated-texture invalidation in the
same executable; it does not change simulation or the existing distant cache.
Run captures separately from timings: readback changes the measured workload.
Uses only Python's standard library. Linux /proc supplies optional CPU/RSS data.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import time


SCENES = {
    "sparse": (24, "araat", 1.0, {}),
    "towers": (500, "araat", 1.0, {}),
    "keeps": (1500, "arakeep", 0.5, {}),
    "spaced-keeps": (200, "arakeep", 0.5, {"TAK_RENDER_SPACING": "160"}),
    "distant": (2000, None, 0.25, {}),
    "animated": (1200, "araarch", 0.5, {"TAK_RENDER_MOTION": "1"}),
    "battle": (1200, "araarch", 0.5, {"TAK_RENDER_BATTLE": "1"}),
    "pan": (500, "araat", 0.75, {"TAK_RENDER_CAMERA": "1"}),
    "zoom": (500, "araat", 1.0, {"TAK_RENDER_CAMERA": "1", "TAK_RENDER_ZOOM": "1"}),
}


def distribution(values):
    if not values:
        return None
    ordered = sorted(values)
    return {"mean": statistics.mean(values), **{
        f"p{p}": ordered[min(len(ordered) - 1, math.floor((len(ordered) - 1) * p / 100))]
        for p in (50, 95, 99)
    }}


def process_sample(pid):
    try:
        status = Path(f"/proc/{pid}/status").read_text()
        rss = int(re.search(r"VmRSS:\s+(\d+)", status)[1]) * 1024
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return time.monotonic(), rss, int(fields[11]) + int(fields[12])
    except (OSError, TypeError, IndexError):
        return None


def run(args, scene, leg, mode):
    count, kind, zoom, extra = SCENES[scene]
    name = f"{scene}-{leg}-{mode}"
    profile = args.output / name / "profile"
    pref = profile / "TAKengine" / "TAKingdoms"
    pref.mkdir(parents=True, exist_ok=True)
    # SDL_GetPrefPath uses XDG_DATA_HOME on this backend. Set both directories
    # so existing user preferences never affect the comparison.
    (pref / "settings.ini").write_text(
        "fullscreen = 0\nvsync = 0\nmaxFps = 60\nuiScale = 1\n"
        "scorecardScale = 1\ntreeSway = 1\nunitShadows = 1\n"
        "healthBars = 1\nstatsPanel = 1\nhardwareCursor = 1\n"
        "smoothMotion = 1\nedgeScroll = 0\nmasterVol = 0\n"
    )
    env = {key: value for key, value in os.environ.items() if not key.startswith("TAK_")}
    env.update(
        SDL_VIDEODRIVER=args.video_driver, SDL_RENDER_DRIVER=args.render_driver,
        SDL_AUDIODRIVER="dummy", XDG_CONFIG_HOME=str(profile), XDG_DATA_HOME=str(profile),
        TAK_SHADOW_BENCH=str(count), TAK_PROFILE_ZOOM=str(zoom), TAK_PROF="1",
        TAK_PROF_FRAMES="1", TAK_DISTANT_STATS="1", TAK_PROF_FINISH="1",
        TAK_RENDER_STUDY="1", TAK_RENDER_FRAMES=str(args.frames),
        TAK_TEXTURE_TICK_REFERENCE="1" if mode == "reference" else "0",
    )
    if kind:
        env["TAK_SHADOW_BENCH_TYPE"] = kind
    env.update(extra)
    if args.capture:
        captures = args.output / name / "captures"
        captures.mkdir(parents=True, exist_ok=True)
        env["TAK_RENDER_CAPTURE_DIR"] = str(captures)
    if args.verify and mode == "retained":
        env["TAK_GEOMETRY_VERIFY"] = "1"
        if kind == "arakeep":
            env["TAK_REQUIRE_GEOMETRY_REUSE"] = "1"
    command = [str(args.client), "game", "Ulasem Arena", "--data", str(args.data),
               "--testbuild", "--winsize", "1280", "960", "--novsync", "--maxfps", "0"]
    log = args.output / f"{name}.log"
    samples = []
    start = time.monotonic()
    diagnostics = args.output / f"{name}.stderr.log"
    with log.open("w") as output, diagnostics.open("w") as errors:
        # Separate buffered stdout from stderr so diagnostics cannot split a
        # measurement line. Neither stream needs per-frame flushing.
        process = subprocess.Popen(command, env=env, stdout=output, stderr=errors)
        try:
            while process.poll() is None:
                sample = process_sample(process.pid)
                if sample:
                    samples.append(sample)
                if time.monotonic() - start > args.timeout:
                    raise TimeoutError(f"{name}: exceeded {args.timeout}s")
                time.sleep(0.1)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    if process.returncode:
        raise RuntimeError(f"{name}: exit {process.returncode}; see {log}")
    text = log.read_text() + "\n" + diagnostics.read_text()
    completion = re.search(r"RENDER_STUDY frames=(\d+) hash=([a-f0-9]+)", text)
    if not completion or int(completion[1]) != args.frames:
        raise RuntimeError(f"{name}: requires a Debug client with render-study support")
    # The AA options were removed after 0.7.24 and current clients print no AA
    # lines at all (AA is always off). A pre-removal client must confirm 0x.
    aa = re.findall(r"AA (terrain|models): requested (\d+)x, effective (\d+)x", text)
    if aa and ({kind for kind, _, _ in aa} != {"terrain", "models"} or any(
            request != "0" or effective != "0" for _, request, effective in aa)):
        raise RuntimeError(f"{name}: AA-off baseline was not confirmed")
    frames = re.findall(
        r"^FRAME ms=([\d.]+) update=([\d.]+) anim=([\d.]+) draw=([\d.]+) "
        r"present=([\d.]+).*tick=(\d+) live=(\d+)", text, re.M)
    frames = [[float(value) for value in row] for row in frames][args.warmup:]
    phases = []
    for line in text.splitlines():
        if line.startswith("DRAWPHASE "):
            row = {key: float(value) for key, value in re.findall(r"(\w+)=(-?[\d.]+)", line)}
            if row["frame"] >= args.warmup:
                phases.append(row)
    if len(frames) != args.frames - args.warmup or len(phases) != len(frames):
        raise RuntimeError(f"{name}: incomplete measurement records")
    result = {
        "scene": scene, "mode": mode, "leg": leg, "frames": len(frames),
        "world_hash": completion[2], "wall_seconds": time.monotonic() - start,
        "peak_rss_bytes": max((sample[1] for sample in samples), default=None),
        "captures_enabled": args.capture, "geometry_verifier_enabled": args.verify,
        "settings": {"count": count, "type": kind, "zoom": zoom, **extra},
        "frame_ms": {key: distribution([row[i] for row in frames]) for i, key in
                     enumerate(("total", "update", "animation", "draw", "present"))},
        "render_work": {key: distribution([row[key] for row in phases])
                        for key in phases[0] if key != "frame"},
    }
    if len(samples) > 1:
        ticks = samples[-1][2] - samples[0][2]
        result["client_cpu_percent_all_cores"] = ticks / os.sysconf("SC_CLK_TCK") / (
            samples[-1][0] - samples[0][0]) * 100 / (os.cpu_count() or 1)
    (args.output / f"{name}.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"{name}: {result['frame_ms']['total']}; hash {completion[2]}", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, required=True, help="optimized Debug takclient")
    parser.add_argument("--data", type=Path, required=True, help="legally owned retail data")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scenes", nargs="+", choices=SCENES, default=list(SCENES))
    parser.add_argument("--frames", type=int, default=900)
    parser.add_argument("--warmup", type=int, default=300)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--video-driver", default="offscreen")
    parser.add_argument("--render-driver", default="opengl")
    parser.add_argument("--capture", action="store_true", help="separate visual run; timings include readback")
    parser.add_argument("--verify", action="store_true", help="compare retained geometry to fresh geometry")
    parser.add_argument("--pair", action="store_true", help="one reference/retained pair instead of ABBA")
    args = parser.parse_args()
    if not 0 <= args.warmup < args.frames <= 10000:
        parser.error("require 0 <= warmup < frames <= 10000")
    for field in ("client", "data", "output"):
        setattr(args, field, getattr(args, field).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    metadata = {
        "client_sha256": hashlib.sha256(args.client.read_bytes()).hexdigest(),
        "command_options": vars(args), "logical_cpus": os.cpu_count(),
        "gpu_measurement": "GL_TIME_ELAPSED draw timeline; may include CPU submission gaps",
        "work_measurement": "body SDL submissions/cache bakes; not whole-frame hardware draw calls",
    }
    (args.output / "metadata.json").write_text(json.dumps(metadata, default=str, indent=2) + "\n")
    results = []
    for scene in args.scenes:
        modes = ("reference", "retained") if args.pair else ("reference", "retained", "retained", "reference")
        legs = [run(args, scene, leg, mode) for leg, mode in enumerate(modes)]
        if len({result["world_hash"] for result in legs}) != 1:
            raise RuntimeError(f"{scene}: simulation hashes differ between runs")
        results.extend(legs)
        (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
