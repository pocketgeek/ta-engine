#!/usr/bin/env python3
"""Offline checks for tools/render_benchmark.py: log parsing, windows, the PNG
comparer and A/B aggregation. Needs no client, data or GPU."""

import struct
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import render_benchmark as rb  # noqa: E402


def log(frames, hash_="00ff00ff00ff00ff", stats=True, deferred_gpu=False):
    lines = ['RENDER_ENV video=offscreen renderer=opengl accelerated=1 vsync=0 target=1 '
             'output=1280x960 window=1280x960 world=1075x888 scale=1.000x1.000 max_texture=32768x32768 '
             'gl_vendor="NVIDIA Corporation" gl_renderer="Some GPU/PCIe" gl_version="4.6.0" timer_query=1',
             "render scene: sites=1 ghosts=0 flyers=0 corpses=0 boats=0 scenery=0 center=5000,5000 offset=1,2",
             "shadow benchmark: requested=10 spawned=9 columns=4 zoom=1.000 type=mixed"]
    if deferred_gpu:
        lines.append("RENDER_TIMING frame_mode=pipelined gpu_timeline=gl_time_elapsed_deferred "
                     "gpu_busy=unmeasured telemetry=on")
    for f in range(frames):
        if deferred_gpu and f >= 2:   # results arrive two frames late
            lines.append(f"GPUTIMELINE frame={f - 2} ms={f - 2}.25")
        gpu = -1 if deferred_gpu else f"{f}.5"
        lines.append(f"DRAWPHASE frame={f} proj=0.5 submit=1.0 shadow=0.25 atlas=0.0 body=0.5 gpu={gpu} "
                     "vram=100 body_vertices=10 body_calls=2 images=0 image_bakes=0 bake_vertices=0 "
                     "bake_calls=0 cache_switches=0 image_bytes=0")
        if stats:
            lines.append(f"RENDERSTATS frame={f} gpu_timeline_ms={-1 if deferred_gpu else 0.1} t_geometry=0.2 "
                         f"w_pool_busy=1.5 visible_units={f} geometry_hits=3 miss_cold=1 geom_used_bytes={f << 20}")
        lines.append(f"FRAME ms={f + 1}.0 update=0.1 anim=0.0 draw=0.5 present=0.25 tick={f} live=9 wall_ms=1")
    if deferred_gpu:   # drained at the end of the run
        lines += [f"GPUTIMELINE frame={f} ms={f}.25" for f in range(max(0, frames - 2), frames)]
    lines.append(f"RENDER_STUDY frames={frames} hash={hash_} gpu_textures=1.00MiB")
    return "\n".join(lines) + "\n"


def write_png(path, w, h, pixels):
    raw = b"".join(b"\x00" + bytes(pixels[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    chunk = lambda kind, body: struct.pack(">I", len(body)) + kind + body + struct.pack(
        ">I", zlib.crc32(kind + body) & 0xffffffff)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def check(cond, what):
    if not cond:
        raise SystemExit(f"FAIL: {what}")


def main():
    parsed = rb.parse_log(log(10), 4, 10)
    check(parsed["env"]["renderer"] == "opengl" and parsed["env"]["output"] == "1280x960", "RENDER_ENV fields")
    check(parsed["env"]["gl_renderer"] == "Some GPU/PCIe", "quoted RENDER_ENV value")
    check(parsed["world_hash"] == "00ff00ff00ff00ff" and parsed["frames_done"] == 10, "completion line")
    check(parsed["spawned"] == 9 and parsed["scene_line"].startswith("sites=1"), "scene lines")
    check(len(parsed["frame"]) == 10 and len(parsed["drawphase"]) == 10 and len(parsed["stats"]) == 10, "rows")
    s = rb.summarize(parsed, 4, 10)
    check(s["frame_ms"]["total"]["n"] == 6 and s["frame_ms"]["total"]["mean"] == 7.5, "warmup window positional")
    check(s["drawphase"]["gpu"]["mean"] == 7.0, "drawphase window by frame index")
    check(s["gpu"]["timeline"]["mean"] == 7.0, "in-line GPU timeline")
    check(s["stats"]["w_pool_busy"]["mean"] == 1.5 and s["stats_last"]["geom_used_bytes"] == 9 << 20,
          "worker times and byte gauges")
    deferred = rb.parse_log(log(10, deferred_gpu=True), 4, 10)
    check(deferred["timing"]["frame_mode"] == "pipelined", "RENDER_TIMING parsed")
    check(sorted(deferred["gpu_timeline"]) == list(range(10)), "every deferred GPUTIMELINE frame collected")
    ds = rb.summarize(deferred, 4, 10)
    check(ds["gpu"]["timeline"]["mean"] == 6.75 and ds["gpu"]["timeline"]["n"] == 6,
          "deferred GPU results attach to the frame they measured")
    check(ds["stats"]["gpu_timeline_ms"]["mean"] == 6.75 and ds["drawphase"]["gpu"]["mean"] == 6.75,
          "in-line -1 placeholders replaced, never averaged")
    check(s["stats"]["visible_units"]["p50"] == 6 and s["stats_last"]["visible_units"] == 9, "stats window")
    seg = rb.summarize(parsed, 2, 5)
    check(seg["frame_ms"]["total"]["n"] == 3 and seg["stats"]["visible_units"]["mean"] == 3, "segment window")
    old = rb.parse_log(log(10, stats=False), 4, 10)
    check("stats" not in rb.summarize(old, 4, 10), "pre-telemetry client parses without RENDERSTATS")
    check(rb.world_rect(parsed["env"], 1280, 960) == (0, 0, 1075, 888), "world rect from RENDER_ENV")
    check(rb.world_rect(None, 1280, 960) == (0, 0, 1280, 960), "world rect fallback")
    check(rb.parse_cpus("16-18,20") == {16, 17, 18, 20}, "cpu list")

    with tempfile.TemporaryDirectory() as tmp:
        w, h = 7, 5
        a = [((x * 37 + y * 11) % 256) for y in range(h) for x in range(w * 4)]
        b = list(a)
        b[(3 * w + 6) * 4 + 1] ^= 0x40          # one pixel inside the world rect...
        b[(0 * w + 2) * 4 + 3] ^= 0x01          # ...and an alpha-only change outside it
        write_png(f"{tmp}/a.png", w, h, a)
        write_png(f"{tmp}/b.png", w, h, b)
        check(rb.read_png(f"{tmp}/a.png")[3] == bytes(a), "PNG round trip")
        full = rb.compare_png(f"{tmp}/a.png", f"{tmp}/b.png")
        check(full["differing"] == 2 and full["max_channel_error"] == 0x40, "full-frame compare")
        part = rb.compare_png(f"{tmp}/a.png", f"{tmp}/b.png", (0, 1, w, h))
        check(part["differing"] == 1 and part["pixels"] == w * (h - 1), "rect compare")
        same = rb.compare_png(f"{tmp}/a.png", f"{tmp}/a.png")
        check(same["differing"] == 0, "identical compare")

    def result(label, total, mode="forced"):
        return {"capture": False, "scene": "s", "size": "normal", "label": label, "frame_mode": mode,
                "load": {"loadavg_before": 1.0, "legion_scenario_before": 0},
                "measured": {"frame_ms": {"total": {"mean": total, "p95": total, "p99": total},
                                          "draw": {"mean": total}, "present": {"mean": 0.1}},
                             "drawphase": {}, "stats": {"visible_units": {"mean": 5},
                                                        "t_geometry": {"mean": total / 10}},
                             "stats_last": {"geom_capacity_bytes": 3 << 20}}}
    report = rb.aggregate([result("A", 10.0), result("B", 8.0), result("B", 8.2), result("A", 10.4)])
    d = report[0]["delta"]["frame_ms.total.mean"]
    check(abs(d["relative"] - (8.1 - 10.2) / 10.2) < 1e-9 and d["significant"], "A/B delta")
    noisy = rb.aggregate([result("A", 10.0), result("B", 9.9), result("B", 9.95), result("A", 10.3)])
    check(not noisy[0]["delta"]["frame_ms.total.mean"]["significant"], "delta inside A/A noise")
    md = rb.markdown(report)
    check("| s | normal | forced | A | 2 (0) | 5 |" in md, "markdown row")
    check(report[0]["labels"]["A"]["telemetry"]["t_geometry"]["mean"] == 1.02, "telemetry mean of run means")
    check("telemetry.t_geometry" in report[0]["delta"], "telemetry phase delta")
    check("geom_capacity_bytes" in md and "| 3.000 |" in md, "memory table in MiB")
    mixed = rb.aggregate([result("A", 10.0), result("B", 8.0, "pipelined"), result("B", 8.2),
                          result("A", 10.4, "pipelined")])
    check(len(mixed) == 2 and {r["frame_mode"] for r in mixed} == {"forced", "pipelined"},
          "pipelined and forced runs never share a group")
    busy = result("A", 50.0); busy["load"]["pinned_cpu_busy_fraction"] = 0.9
    check(rb.aggregate([result("A", 10.0), busy], exclude_contended=True)[0]["labels"]["A"]["runs"] == 1, "contended run excluded")
    for name, scene in rb.SCENES.items():
        env = rb.scene_env(name, "normal")
        check(env["TAK_SHADOW_BENCH"] == str(scene["count"]) and scene.get("notes"), f"scene {name}")
    check(rb.scene_env("total10k-subset", "uhd")["TAK_RENDER_OFFSET"] == "0,3960", "per-size scene override")
    interactive_checks()
    print("PASS: render benchmark parser, windows, PNG compare and aggregation")


def interactive_checks():
    """Measurement B: the interactive scenes stay out of the historical matrix and a
    run is refused unless the client confirms the scene as designed."""
    check(not set(rb.INTERACTIVE) & set(rb.HISTORICAL), "interactive scenes are not in the historical 'all'")
    check(len(rb.HISTORICAL) == 24 and set(rb.QUICK) <= set(rb.HISTORICAL) and rb.HISTORICAL[-1] == "terrain-jumps",
          "historical matrix unchanged")
    check(rb.scene_env("hover-units-10k", "uhd")["TAK_RENDER_OFFSET"] == "0,5000" and
          rb.scene_env("hover-units-10k", "uhd")["TAK_RENDER_CENTER"] == "7600,7600", "interactive size override")
    stats = "cursor_evals=1 cursor_pick_tests=200 cursor_pick_hits=3 hidden_faded=0"
    good = (log(10).replace("RENDERSTATS frame=", "RENDERSTATS " + stats + " frame=")
            + "RENDER_INTERACTIVE mouse=sweep fade_clock=0\n"
            + "render interactive: selected=200 armed=g covers=0 ghost_builders=0 ghost_orders=0 ghost_offscreen=0\n")
    parsed = rb.parse_log(good, 4, 10)
    out = rb.check_interactive("x", "hover-armed-10k", good, rb.summarize(parsed, 4, 10))
    check(out["selected"] == 200 and out["armed"] == "g" and out["cursor_evals"] == 1 and
          out["cursor_pick_hits"] == 3, "interactive confirmation parsed")
    check(rb.check_interactive("x", "small-battle", log(10), rb.summarize(parsed, 4, 10)) is None,
          "matrix scenes need no confirmation")

    def refused(scene, text, why):
        p = rb.parse_log(text, 4, 10)
        try:
            rb.check_interactive("x", scene, text, rb.summarize(p, 4, 10))
        except RuntimeError as e:
            check(why in str(e), f"refusal names the cause ({why}): {e}")
            return
        raise SystemExit(f"FAIL: {scene} accepted ({why})")
    refused("hover-armed-10k", log(10), "lacks the interactive render cases")
    refused("hover-armed-10k", good.replace("cursor_evals=1", "cursor_evals=0"), "cursor classified")
    refused("hover-armed-10k", good.replace("cursor_pick_hits=3", "cursor_pick_hits=0"), "never was over a unit")
    refused("hover-armed-10k", good.replace("armed=g", "armed=-"), "armed")
    refused("hover-armed-10k", good.replace("selected=200", "selected=12"), "selected 12")
    refused("fade-buildings", good, "fade clock off")
    ghosts = good.replace("ghost_orders=0 ghost_offscreen=0", "ghost_orders=384 ghost_offscreen=0")
    refused("ghosts-queue", ghosts, "ghost_offscreen 0")


if __name__ == "__main__":
    main()
