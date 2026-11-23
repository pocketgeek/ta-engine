#!/usr/bin/env python3
"""Matched renderer benchmark matrix for the rendering optimization program.

Runs the debug render study (takclient --testbuild with TAK_RENDER_STUDY) over a
fixed scene matrix and compares one or two optimized-Debug clients:

  A/A (variability):  --a CLIENT [--repeats N]          -> A A A A ...
  A/B (matched):      --a BASE --b CANDIDATE [--repeats N] -> A B B A per group

Each run uses a fresh scratch profile (never the user's settings), a fixed 1/60 s
frame clock, frame-indexed camera paths and a fixed-step local simulation, so the
same scene produces the same world hash on every run. The tool refuses a run
whose backend or output size differs from the request, or whose world hash
differs from the other runs of that scene (a rendering change must not move the
simulation).

Timings: FRAME/DRAWPHASE lines (present in every client since 0.7.23) and the
RENDERSTATS line (phase-0 telemetry, extended by workstream A; optional so older
baselines still parse). In RENDERSTATS, t_* are render-thread wall times (some
nest inside others -- never add them up), w_* are CPU times summed over worker
threads, the rest are per-frame counts or byte gauges.
GPU time is the GL_TIME_ELAPSED draw TIMELINE, which includes idle gaps waiting
for CPU submission; it is not device busy time (nothing here measures busy
time). Workstream-A clients read it from a query ring without waiting and print
GPUTIMELINE lines, possibly frames later; older clients block on the result each
frame (which makes their "pipelined" frames partly forced). TAK_PROF_FINISH adds
a glFinish before present so the frame includes completed GPU work
(forced-completion diagnostic timing); pass --pipelined to measure normal
pipelined frames instead. The two modes are never aggregated or compared
together.

Captures (--capture) are separate runs: readback perturbs timing. Their PNGs are
compared run-to-run over the world view and over the whole frame.

Uses only the Python standard library. Linux-specific: /proc metrics, taskset.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess
import sys
import time
import zlib

DEFAULT_MAP = "Ulasem Arena"

# name -> dict(map, count, env, zoom, notes, sizes overrides, segments)
# Per-size overrides: "env@WxH" entries are merged for that output size.
SCENES = {
    "small-battle": dict(count=200, type="araarch", zoom=1.0,
                         env={"TAK_RENDER_BATTLE": "1"},
                         notes="ordinary small battle: 200 Archers, two sides"),
    "stationary-mixed": dict(count=2000, zoom=0.5,
                             notes="dense stationary mixed army (6-type roster)"),
    "moving": dict(count=1200, type="araarch", zoom=0.5, env={"TAK_RENDER_MOTION": "1"},
                   notes="dense continuously moving army (orders reissued every 240 frames)"),
    "total10k-subset": dict(count=10000, zoom=1.0,
                            env={"TAK_RENDER_CENTER": "7600,7600", "TAK_RENDER_OFFSET": "4200,5400"},
                            size_env={"7680x2160": {"TAK_RENDER_OFFSET": "0,3960"}},
                            notes="10k units, camera on a small corner of the army"),
    "total10k-visible": dict(count=10000, zoom=0.25,
                             notes="10k units, (nearly) all on screen"),
    "forest": dict(map="Sea Dragon Spine", count=300, zoom=1.0, env={"TAK_RENDER_FOREST": "1"},
                   notes="densest scenery region of Sea Dragon Spine + 300 mixed units"),
    "pan": dict(count=2000, zoom=0.75, env={"TAK_RENDER_CAMERA": "1"},
                notes="sinusoidal pan (continuous motion with reversals)"),
    "zoom": dict(count=2000, zoom=1.0, env={"TAK_RENDER_CAMERA": "1", "TAK_RENDER_ZOOM": "1"},
                 notes="pan + continuous zoom 0.25..1.0"),
    "jump": dict(count=2000, zoom=0.75, env={"TAK_RENDER_JUMP": "60"},
                 notes="camera jumps between four distant views every 60 frames"),
    "effects": dict(count=600, zoom=0.75, env={"TAK_RENDER_BATTLE": "1", "TAK_RENDER_EXTRAS": "1"},
                    notes="battle + construction sites, ghosts, flyers, corpses, projectiles"),
    "naval": dict(map="Athri Cay", count=1, zoom=0.75, env={"TAK_RENDER_NAVAL": "150"},
                  notes="150 floating hulls on the water nearest the centre of Athri Cay"),
    "churn": dict(count=2000, zoom=0.75,
                  env={"TAK_RENDER_MOTION": "1", "TAK_RENDER_CAMERA": "1",
                       "TAK_RENDER_CHURN": "60", "TAK_RENDER_RESET": "300"},
                  notes="moving army + panning camera; 5% of the army dies/respawns every "
                        "60 frames; simulated render-target reset every 300 frames"),
    "colour-reset": dict(count=200, type="araarch", zoom=1.0,
                         env={"TAK_RENDER_BATTLE": "1", "TAK_RENDER_RESET": "120"},
                         notes="small battle (two player colours) with a simulated render-target "
                               "reset every 120 frames: both colour atlases are recreated"),
    "battle-sparse": dict(count=2000, zoom=0.5,
                          env={"TAK_RENDER_BATTLE": "1", "TAK_RENDER_SPARSE_AFTER": "450",
                               "TAK_RENDER_SPARSE_OFFSET": "8800,200"},
                          segments={"battle": (300, 450), "sparse": (450, None)},
                          notes="large battle, then from frame 450 a sustained empty view"),
    "constrained": dict(count=2000, zoom=1.0, env={"TAK_RENDER_CAMERA": "1", "TAK_RENDER_ZOOM": "1",
                                                   "TAK_GPU_CAP_MIB": "128"},
                        settings={"gpuMemoryLimit": "512"},
                        notes="the zoom scene under a 128 MiB tracked-GPU cap (debug override below "
                              "the 512 MiB Options minimum, which no matrix scene reaches)"),
    # Workstream E: reuse that needs a still or whole-pixel camera.
    "pan-snap": dict(count=2000, zoom=1.0, env={"TAK_RENDER_CAMERA_SNAP": "1"},
                     notes="sinusoidal pan rounded to whole world units at zoom 1: every camera "
                           "step is a whole-pixel translation (exact shadow-tile translation reuse)"),
    "veteran": dict(count=2000, zoom=0.5, env={"TAK_RENDER_VETERAN": "30"},
                    notes="dense stationary mixed army; every 30 frames every fifth unit changes "
                          "veteran level (material-only body change, shadow unchanged)"),
    # Workstream G: distant images under a moving camera (zoom <= 0.6, 1280x960; the
    # 7680x2160 view of Ulasem Arena clamps to zoom 0.73, above the image limit).
    "pan-distant": dict(count=2000, zoom=0.5, env={"TAK_RENDER_CAMERA": "1"},
                        notes="the stationary mixed army at zoom 0.5 under the sinusoidal pan: "
                              "held distant images re-placed every frame, re-baked every 67 ms"),
    "distant-sparse": dict(count=6000, zoom=0.25,
                           env={"TAK_RENDER_SPARSE_AFTER": "450", "TAK_RENDER_SPARSE_OFFSET": "200,200"},
                           segments={"army": (300, 450), "corner": (450, None)},
                           notes="6000 units zoomed out (several distant-image pages), then from frame "
                                 "450 a corner of the army: images expire, pages are released"),
    # Workstream M: every structure type (palette-lit models on all nine palette
    # pages), live geometry (zoom 0.75 is above the distant-image limit). The roster
    # is spelled out so builds without the harness's "structures" keyword run it too.
    "structures": dict(count=800, zoom=0.75,
                       env={"TAK_RENDER_SPACING": "96", "TAK_RENDER_ROSTER":
                            "araat,aracastl,arakeep,aralode,aramana,arangate,aranull,arassh,aratre,arawall,"
                            "creacad,crebomb,cregate,cregatl,crelode,cremana,crenavy,crepris,cresmit,crewall,"
                            "npcflag,npctemp,npctemp2,npcthesh,tarcage,tarcastl,tardung,tarhell,tarlode,tarmana,"
                            "tarngate,tarsh,tarwall,verasy,verat,vercastl,verfltwr,verkeep,verlight,verlode,"
                            "vermana,vermort,verngate,verpill,vertower,verwall,zonfire,zonglyph,zonlode,zonmana,"
                            "zonspide"},
                       notes="800 structures of all 51 types (all nine palette pages), zoom 0.75: "
                             "palette-lit bodies drawn live"),
    # Workstream J: a map whose decoded sections (238 MiB) exceed the 128 MiB CPU
    # budget, under the pan + continuous zoom path: sections are evicted/re-decoded
    # and the mip cache fills (pre-J builds keep every section decoded).
    "bigmap-zoom": dict(map="Middle Earth V1", count=300, zoom=1.0,
                        env={"TAK_RENDER_CAMERA": "1", "TAK_RENDER_ZOOM": "1"},
                        notes="pan + continuous zoom 0.25..1.0 on Middle Earth V1 (255 sections, "
                              "238 MiB decoded: above the CPU section budget)"),
    # Workstream K: terrain work under camera motion on the largest shipped map
    # (Middle Earth V1, 1008x1008 blocks = 32x32 chunks; min zoom 0.24 at 7680x2160).
    "terrain-sweep": dict(map="Middle Earth V1", count=300, zoom=1.0,
                          env={"TAK_RENDER_CENTER": "16128,16128",
                               "TAK_RENDER_SWEEP": "16128,16128,9000,6000,480"},
                          notes="fast elliptical sweep (9000x6000 px, 480 frames: up to 118 px/frame) "
                                "with reversals in both axes over Middle Earth V1"),
    "terrain-zoom-osc": dict(map="Middle Earth V1", count=300, zoom=0.5,
                             env={"TAK_RENDER_CENTER": "16128,16128", "TAK_RENDER_ZOOM_OSC": "0.42,0.6,90"},
                             notes="zoom oscillating 0.42..0.6 every 90 frames: the chunk level flips "
                                   "between 0 and 1 (at 7680x2160 both levels exceed the chunk budget)"),
    "terrain-jumps": dict(map="Middle Earth V1", count=300, zoom=0.75,
                          env={"TAK_RENDER_CENTER": "16128,16128", "TAK_RENDER_JUMP": "20",
                               "TAK_RENDER_JUMP_AT": "4000,4000;28000,4000;28000,28000;4000,28000;16000,16000"},
                          notes="camera jumps between five far-apart views every 20 frames (faster "
                                "than a view's terrain finishes composing)"),
}
QUICK = ["small-battle", "stationary-mixed", "moving", "pan"]
SIZES = {"normal": (1280, 960), "uhd": (7680, 2160)}
# The matrix as it stood before the interactive cases: "--scenes all" keeps meaning
# exactly these, so historical "all" runs and baselines stay comparable.
HISTORICAL = list(SCENES)

# Build ghosts (perf follow-up E): kept out of the historical matrix, like the
# interactive cases below ("--scenes ghost-queues ..." names them explicitly).
GHOST_SCENES = {
    # 16 monarchs x 32 queued builds of 4 repeated structure types on a 4096x3072 px
    # grid around the camera centre.
    "ghost-queues": dict(count=1, zoom=0.5, env={"TAK_RENDER_GHOST_QUEUES": "16,32,4",
                                                 "TAK_RENDER_CAMERA": "1"},
                         notes="512 queued-build ghosts (4 types), zoom 0.5 under the sinusoidal pan: "
                               "part of the grid on screen, the rest off it"),
    "ghost-visible": dict(count=1, zoom=0.25, env={"TAK_RENDER_GHOST_QUEUES": "16,32,4"},
                          notes="the same 512 ghosts, zoom 0.25, still camera: (nearly) all on screen"),
    "ghost-offscreen": dict(count=1, zoom=1.0, env={"TAK_RENDER_GHOST_QUEUES": "16,32,4,0,4000"},
                            notes="the same 512 ghosts 4000 px south of the view: every queued "
                                  "ghost off screen"),
    "ghost-few": dict(count=200, zoom=0.75, env={"TAK_RENDER_GHOST_QUEUES": "4,12,4,0,700",
                                                 "TAK_RENDER_CAMERA": "1"},
                      notes="ordinary case: 200 mixed units, 4 monarchs x 12 queued builds south of "
                            "them, panning camera"),
}
SCENES.update(GHOST_SCENES)

# Silhouette atlas capacity (perf follow-up K), kept out of the historical matrix.
# Named shadow-pages-* because measurement B's interactive cases already have
# shadow-cap-below/-above (zoom 1, a different army) for the same question.
SHADOW_PAGE_SCENES = {
    # At 7680x2160 ("uhd"; the armies are sized for that view and stay far below the
    # limit at 1280x960). Ulasem Arena's
    # zoom-out floor there is 0.73, where TACTICAL DOTS (default on) replace the models,
    # so these views sit at 0.82 -- one wheel notch in -- where a settled mixed-roster
    # tile takes ~2700 px^2 and four 2048^2 pages hold ~6150 of them (the first frames
    # after the spawn draw larger silhouettes, ~1.3x the area). Before K the atlas
    # stopped at four pages: a view needing more drew every shadow directly, for the
    # whole frame, and a view filling them repacked every tile each frame a tile did
    # not fit. TAK_RENDER_OFFSET centres the army in the uhd view.
    "shadow-pages-below": dict(count=5800, zoom=0.82, env={"TAK_RENDER_OFFSET": "442,3727"},
                               notes="stationary mixed army whose settled shadow tiles fill ~95% of "
                                     "four silhouette pages at 7680x2160 (zoom 0.82)"),
    "shadow-pages-moving": dict(count=5400, zoom=0.82,
                                env={"TAK_RENDER_MOTION": "1", "TAK_RENDER_OFFSET": "442,3727"},
                                notes="moving mixed army around four silhouette pages at 7680x2160 "
                                      "(zoom 0.82, orders reissued every 240 frames)"),
    "shadow-pages-above": dict(count=6600, zoom=0.82, env={"TAK_RENDER_OFFSET": "442,3727"},
                               notes="stationary mixed army needing ~108% of four silhouette pages "
                                     "at 7680x2160 (zoom 0.82)"),
    "shadow-pages-dense": dict(count=10000, zoom=0.82, env={"TAK_RENDER_OFFSET": "442,3727"},
                               notes="10k mixed army, ~9800 in view at 7680x2160 (zoom 0.82): tiles "
                                     "of ~6.4 pages"),
    "shadow-pages-pan": dict(count=10000, zoom=0.82, env={"TAK_RENDER_CAMERA": "1"},
                             notes="10k mixed army (~7900 in view) under the sinusoidal pan at 7680x2160: "
                                   "every shadow tile is redrawn every frame (sub-pixel camera steps)"),
    "shadow-pages-osc": dict(count=5000, zoom=0.82,
                             env={"TAK_RENDER_OFFSET": "442,3727", "TAK_RENDER_ZOOM_OSC": "0.8,1.0,600"},
                             notes="5000-unit army, zoom oscillating 0.8..1.0 every 600 frames at "
                                   "7680x2160: its tiles cross four pages twice per period"),
    # 448 MiB, not the 384 MiB floor: under 384 MiB this view crashes 461e9c4b in the body
    # arena's native draw (GeometrySubmit::passDrawBuffer, a pre-existing defect).
    "shadow-pages-constrained": dict(count=10000, zoom=0.82,
                                     env={"TAK_RENDER_OFFSET": "442,3727", "TAK_GPU_CAP_MIB": "448"},
                                     notes="shadow-pages-dense under a 448 MiB tracked-GPU cap: pages past the "
                                           "first four fit only while the terrain loads, then give way"),
}
SCENES.update(SHADOW_PAGE_SCENES)

# Interactive cases (perf follow-up measurement B). The matrix above never moves the
# pointer, so the in-game cursor is never classified (the pointer is unset), nothing is
# selected, no build queue is long and nothing is behind a building. These scenes add
# that work explicitly, without changing any scene above. They need a client with the
# measurement-B harness knobs (TAK_RENDER_MOUSE / _SELECT / _ARM / _GHOSTS / _COVER /
# _FADE_CLOCK); "expect" lists what every run must confirm (RENDER_INTERACTIVE and
# "render interactive:" lines, per-frame counters), so a client without them is refused
# instead of silently measuring the matrix scene. Run with --scenes interactive (or by
# name). Mouse positions are fractions of the world view (TAK_RENDER_MOUSE).

# The silhouette atlas the shadow scenes are sized against: four pages of 2048x2048
# (gameview_render.cpp kMaxPages and the default page limit at 61f294ac).
SHADOW_DESIGN_CAPACITY_PX = 4 * 2048 * 2048
SUBSET_10K = {"TAK_RENDER_CENTER": "7600,7600", "TAK_RENDER_OFFSET": "4200,5400"}
SUBSET_10K_UHD = {"TAK_RENDER_OFFSET": "0,3960"}
# At 7680x2160 the total10k-subset view holds only the army's top edge (a strip a few
# px deep): the pointer scenes look further down, at a corner of about 1600 units.
HOVER_10K_UHD = {"TAK_RENDER_OFFSET": "0,5000", "TAK_RENDER_MOUSE": "sweep:0.62,0.42,1.0,1.0"}
INTERACTIVE = {
    # C: cursor classification. 10000 own units, a small corner of them in view.
    "hover-ground-10k": dict(count=10000, zoom=1.0,
                             env=dict(SUBSET_10K, TAK_RENDER_MOUSE="0.2,0.2"),
                             size_env={"7680x2160": SUBSET_10K_UHD},
                             expect={"mouse": "fixed", "cursor": True},
                             notes="10k own units, a corner in view; pointer held on empty ground, nothing "
                                   "selected: every frame tests every own unit and none is under it"),
    "hover-units-10k": dict(count=10000, type="araarch", zoom=1.0,
                            env=dict(SUBSET_10K, TAK_RENDER_MOUSE="sweep:0.70,0.62,1.0,1.0",
                                     TAK_RENDER_SELECT="1"),
                            size_env={"7680x2160": HOVER_10K_UHD},
                            expect={"mouse": "sweep", "cursor": True, "selected": 1, "pick_hits": True},
                            notes="10k own Archers, a corner in view; one selected (a non-builder: no build "
                                  "strip over the map), pointer sweeping over the visible corner of the army "
                                  "and the ground beside it"),
    "hover-armed-10k": dict(count=10000, type="araarch", zoom=1.0,
                            env=dict(SUBSET_10K, TAK_RENDER_MOUSE="sweep:0.70,0.62,1.0,1.0",
                                     TAK_RENDER_SELECT="200", TAK_RENDER_ARM="g"),
                            size_env={"7680x2160": HOVER_10K_UHD},
                            expect={"mouse": "sweep", "cursor": True, "selected": 200, "armed": "g",
                                    "pick_hits": True},
                            notes="as hover-units-10k with 200 units selected and Guard armed (the armed "
                                  "target scan: every unit under the pointer against the selection)"),
    "select-large": dict(count=2000, zoom=0.5,
                         env={"TAK_RENDER_SELECT": "all", "TAK_RENDER_MOUSE": "sweep:0.05,0.05,0.95,0.95"},
                         expect={"mouse": "sweep", "cursor": True, "selected": 1500, "pick_hits": True},
                         notes="the stationary mixed army (2000) all selected, pointer sweeping the view "
                               "(7680x2160: zoom clamps to 0.73, fully out, so units are Tactical Dots)"),
    # E: queued-build ghosts (16 builders x 24 orders over three repeated types, half of
    # every queue at the far map corner).
    "ghosts-queue": dict(count=1, type="araarch", zoom=0.75, env={"TAK_RENDER_GHOSTS": "16,24,50"},
                         expect={"ghost_orders": 300, "ghost_offscreen": 120},
                         notes="16 builders with 24 queued builds each (3 repeated types), half of each "
                               "queue off screen at the far map corner"),
    "ghosts-queue-pan": dict(count=1, type="araarch", zoom=0.75,
                             env={"TAK_RENDER_GHOSTS": "16,24,50", "TAK_RENDER_CAMERA": "1"},
                             expect={"ghost_orders": 300, "ghost_offscreen": 120},
                             notes="ghosts-queue under the sinusoidal pan"),
    # F: HIDDEN UNITS (Fade unless noted) with crowds under repeated trees and buildings;
    # the fade clock follows the study frame so fades match run to run.
    # (Ulasem Arena, not the forest scene's Sea Dragon Spine: at 7680x2160 that small map
    # clamps the zoom so far out that units are Tactical Dots and nothing can fade.)
    "fade-forest": dict(count=1500, type="araarch", zoom=1.0,
                        env={"TAK_RENDER_FOREST": "1", "TAK_RENDER_SPACING": "28",
                             "TAK_RENDER_FADE_CLOCK": "1"},
                        settings={"hiddenUnits": "fade"},
                        expect={"fade_clock": True, "hidden_faded": True},
                        notes="1500 Archers (28 px apart) in the densest scenery of Ulasem Arena; Fade"),
    "fade-buildings": dict(count=2000, type="araarch", zoom=1.0,
                           env={"TAK_RENDER_COVER": "vertower,4", "TAK_RENDER_SPACING": "32",
                                "TAK_RENDER_FADE_CLOCK": "1"},
                           settings={"hiddenUnits": "fade"},
                           expect={"fade_clock": True, "covers": 50, "hidden_faded": True},
                           notes="2000 Archers (32 px) around a grid of towers (every 4th row/column); Fade"),
    "dots-buildings": dict(count=2000, type="araarch", zoom=1.0,
                           env={"TAK_RENDER_COVER": "vertower,4", "TAK_RENDER_SPACING": "32",
                                "TAK_RENDER_FADE_CLOCK": "1"},
                           settings={"hiddenUnits": "dots"},
                           expect={"fade_clock": True, "covers": 50},
                           notes="fade-buildings with HIDDEN UNITS Dots (the other display path)"),
    # K: unit shadows near the silhouette atlas's capacity (four 2048x2048 pages). The
    # 6-type roster at zoom 1, 40 px apart; at 1280x960 the view cannot hold that much
    # shadow (demand / view area stays far below 16x), so these run at 7680x2160 only.
    # shadow_demand_px is the frame's tile area as asked; "demand" bounds its mean.
    "shadow-cap-below": dict(count=4500, zoom=1.0, env={"TAK_RENDER_SPACING": "40"}, sizes=("uhd",),
                             expect={"demand": (0.85, 0.97)},
                             notes="stationary mixed army whose shadow tiles take about 0.91 of the atlas"),
    "shadow-cap-above": dict(count=5500, zoom=1.0, env={"TAK_RENDER_SPACING": "40"}, sizes=("uhd",),
                             expect={"demand": (1.02, 1.12)},
                             notes="stationary mixed army whose shadow tiles take about 1.06 of the atlas "
                                   "(more than four pages hold)"),
    "shadow-cap-cross": dict(count=5000, zoom=1.0,
                             env={"TAK_RENDER_SPACING": "40", "TAK_RENDER_ZOOM_OSC": "0.92,1.08,240"},
                             sizes=("uhd",), expect={"demand": (0.9, 1.1)},
                             notes="zoom 0.92..1.08 every 240 frames: the demand crosses the atlas "
                                   "capacity (about 0.85..1.14) twice a period"),
    "shadow-cap-below-pan": dict(count=4500, zoom=1.0, env={"TAK_RENDER_SPACING": "40", "TAK_RENDER_CAMERA": "1"},
                                 sizes=("uhd",), expect={"demand": (0.85, 0.97)},
                                 notes="shadow-cap-below under the sinusoidal pan"),
    "shadow-cap-above-pan": dict(count=5500, zoom=1.0, env={"TAK_RENDER_SPACING": "40", "TAK_RENDER_CAMERA": "1"},
                                 sizes=("uhd",), expect={"demand": (1.02, 1.12)},
                                 notes="shadow-cap-above under the sinusoidal pan"),
}
SCENES.update(INTERACTIVE)
INTERACTIVE_RE = re.compile(r"^RENDER_INTERACTIVE mouse=(\w+) fade_clock=(\d)", re.M)
INTERACTIVE_SETUP_RE = re.compile(
    r"render interactive: selected=(\d+) armed=(\S) covers=(\d+) ghost_builders=(\d+) "
    r"ghost_orders=(\d+) ghost_offscreen=(\d+)")


def check_interactive(name, scene, text, measured):
    """Confirms an interactive scene ran as designed; returns what the client reported.
    Raises when the client lacks the measurement-B knobs or the scene missed its design."""
    expect = SCENES[scene].get("expect")
    if not expect:
        return None
    mouse = INTERACTIVE_RE.search(text)
    setup = INTERACTIVE_SETUP_RE.search(text)
    needs_setup = any(k in expect for k in ("selected", "armed", "covers", "ghost_orders", "ghost_offscreen"))
    if ((("mouse" in expect or "fade_clock" in expect) and not mouse) or (needs_setup and not setup)):
        raise RuntimeError(f"{name}: client lacks the interactive render cases (measurement B harness knobs)")
    out = {}
    if mouse:
        out.update(mouse=mouse[1], fade_clock=int(mouse[2]))
    if setup:
        out.update(zip(("selected", "armed", "covers", "ghost_builders", "ghost_orders", "ghost_offscreen"),
                       (int(setup[1]), setup[2], int(setup[3]), int(setup[4]), int(setup[5]), int(setup[6]))))
    stats = measured.get("stats") or {}
    mean = lambda k: (stats.get(k) or {}).get("mean")
    for k in ("cursor_evals", "cursor_pick_tests", "cursor_pick_hits", "ghosts_queued", "ghost_preps",
              "hidden_cover_tests", "hidden_faded", "shadow_demand_px", "shadow_fallback"):
        if mean(k) is not None:
            out[k] = mean(k)
    problems = []
    if "mouse" in expect and out.get("mouse") != expect["mouse"]:
        problems.append(f"pointer {out.get('mouse')} (want {expect['mouse']})")
    if expect.get("fade_clock") and out.get("fade_clock") != 1:
        problems.append("fade clock off")
    if expect.get("cursor") and out.get("cursor_evals") != 1:
        problems.append(f"cursor classified {out.get('cursor_evals')} times a frame (want 1: cursor art missing?)")
    if expect.get("pick_hits") and not out.get("cursor_pick_hits"):
        problems.append("the pointer never was over a unit")
    if "armed" in expect and out.get("armed") != expect["armed"]:
        problems.append(f"armed {out.get('armed')} (want {expect['armed']})")
    for k in ("selected", "covers", "ghost_orders", "ghost_offscreen"):
        if k in expect and out.get(k, 0) < expect[k]:
            problems.append(f"{k} {out.get(k, 0)} (want >= {expect[k]})")
    if expect.get("hidden_faded") and not out.get("hidden_faded"):
        problems.append("no cover faded")
    if "demand" in expect:
        lo, hi = expect["demand"]
        ratio = (out.get("shadow_demand_px") or 0) / SHADOW_DESIGN_CAPACITY_PX
        out["shadow_demand_ratio"] = ratio
        if not lo <= ratio <= hi:
            problems.append(f"shadow demand {ratio:.3f} of the 4x2048^2 atlas (want {lo}..{hi})")
    if problems:
        raise RuntimeError(f"{name}: interactive scene missed its design: " + "; ".join(problems))
    return out

GPU_RE = re.compile(r"^GPUTIMELINE frame=(\d+) ms=(-?[\d.]+)", re.M)
FRAME_RE = re.compile(
    r"^FRAME ms=([\d.]+) update=([\d.]+) anim=([\d.]+) draw=([\d.]+) present=([\d.]+)"
    r".*tick=(\d+) live=(\d+)", re.M)
KV_RE = re.compile(r"(\w+)=(-?[\d.]+)")
ENV_RE = re.compile(r'(\w+)=("[^"]*"|\S+)')


def distribution(values):
    if not values:
        return None
    ordered = sorted(values)
    pick = lambda p: ordered[min(len(ordered) - 1, math.floor((len(ordered) - 1) * p / 100))]
    return {"mean": statistics.fmean(values), "p50": pick(50), "p95": pick(95), "p99": pick(99),
            "max": ordered[-1], "n": len(values)}


def parse_kv(line):
    return {k: float(v) for k, v in KV_RE.findall(line)}


def parse_env(line):
    return {k: v.strip('"') for k, v in ENV_RE.findall(line[len("RENDER_ENV "):])}


def merge_gpu(rows, key, timeline):
    """Replace a row's in-line GPU value with the deferred GPUTIMELINE result for
    its frame; drop negative (not yet available / unsupported) values."""
    for r in rows:
        frame = int(r.get("frame", -1))
        if frame in timeline:
            r[key] = timeline[frame]
        elif key in r and r[key] < 0:
            del r[key]


def parse_log(text, warmup, frames):
    """Parse one run's stdout+stderr. Returns per-frame rows (after warmup)."""
    env_lines = [l for l in text.splitlines() if l.startswith("RENDER_ENV ")]
    done = re.search(r"RENDER_STUDY frames=(\d+) hash=([a-f0-9]+)", text)
    frame_rows = [[float(v) for v in m] for m in FRAME_RE.findall(text)]
    phase_rows, stat_rows, timing = [], [], None
    for line in text.splitlines():
        if line.startswith("DRAWPHASE "):
            phase_rows.append(parse_kv(line))
        elif line.startswith("RENDERSTATS "):
            stat_rows.append(parse_kv(line))
        elif line.startswith("RENDER_TIMING "):
            timing = parse_env("RENDER_ENV " + line[len("RENDER_TIMING "):])
    timeline = {int(f): float(ms) for f, ms in GPU_RE.findall(text) if float(ms) >= 0}
    merge_gpu(phase_rows, "gpu", timeline)
    merge_gpu(stat_rows, "gpu_timeline_ms", timeline)
    scene = re.search(r"render scene: (.*)", text)
    spawn = re.search(r"shadow benchmark: requested=(\d+) spawned=(\d+)", text)
    return {
        "env": parse_env(env_lines[0]) if env_lines else None,
        "frames_done": int(done[1]) if done else None,
        "world_hash": done[2] if done else None,
        "frame": frame_rows, "drawphase": phase_rows, "stats": stat_rows, "timing": timing,
        "gpu_timeline": timeline,
        "scene_line": scene[1] if scene else None,
        "spawned": int(spawn[2]) if spawn else None,
        "warmup": warmup, "frames": frames,
    }


def window(rows, begin, end, key="frame"):
    """Rows whose frame index lies in [begin, end). FRAME rows are positional."""
    if key is None:
        return rows[begin:end]
    return [r for r in rows if r.get(key, -1) >= begin and (end is None or r[key] < end)]


def columns(rows):
    """Per-key distributions over rows that carry the key (a deferred GPU value
    can be missing from some rows)."""
    keys = []
    for r in rows:
        keys.extend(k for k in r if k != "frame" and k not in keys)
    return {k: distribution([r[k] for r in rows if k in r]) for k in keys}


def summarize(parsed, begin, end):
    frames = parsed["frame"][begin:end]
    out = {"frame_ms": {k: distribution([r[i] for r in frames]) for i, k in
                        enumerate(("total", "update", "anim", "draw", "present"))}}
    phases = window(parsed["drawphase"], begin, end)
    if phases:
        out["drawphase"] = columns(phases)
    stats = window(parsed["stats"], begin, end)
    if stats:
        out["stats"] = columns(stats)
        last = stats[-1]
        out["stats_last"] = {k: v for k, v in last.items() if k != "frame"}
        # Workstream K: share of frames whose visible terrain was incomplete (no chunk
        # image: section fallback; no section either: the blurry underlay).
        if any("terrain_chunk_holes" in r for r in stats):
            out["terrain"] = {name: sum(1 for r in stats if r.get(key, 0) > 0) / len(stats)
                              for name, key in (("hole_frames", "terrain_chunk_holes"),
                                                ("underlay_frames", "terrain_underlay_tiles"),
                                                ("standin_frames", "terrain_chunk_standin"))}
    gpu = [ms for f, ms in parsed.get("gpu_timeline", {}).items() if begin <= f < (end if end is not None else 1 << 30)]
    if not gpu:
        gpu = [r["gpu"] for r in phases if "gpu" in r]
    if gpu:
        out["gpu"] = {"timeline": distribution(gpu)}
    return out


def loadavg():
    try:
        return float(Path("/proc/loadavg").read_text().split()[0])
    except OSError:
        return None


def count_processes(name):
    n = 0
    for p in Path("/proc").iterdir():
        if p.name.isdigit():
            try:
                if (p / "comm").read_text().strip() == name[:15]:
                    n += 1
            except OSError:
                pass
    return n


def cpu_busy(cpus):
    """Cumulative (busy, total) jiffies for the given CPU ids from /proc/stat."""
    busy = total = 0
    try:
        for line in Path("/proc/stat").read_text().splitlines():
            m = re.match(r"cpu(\d+) (.*)", line)
            if m and int(m[1]) in cpus:
                v = [int(x) for x in m[2].split()]
                idle = v[3] + (v[4] if len(v) > 4 else 0)
                busy += sum(v) - idle
                total += sum(v)
    except OSError:
        return None
    return busy, total


def parse_cpus(spec):
    out = set()
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out.update(range(int(a), int(b) + 1))
        elif part:
            out.add(int(part))
    return out


def process_sample(pid):
    try:
        status = Path(f"/proc/{pid}/status").read_text()
        rss = int(re.search(r"VmRSS:\s+(\d+)", status)[1]) * 1024
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return time.monotonic(), rss, int(fields[11]) + int(fields[12])
    except (OSError, TypeError, IndexError):
        return None


def scene_env(scene, size_key):
    s = SCENES[scene]
    env = {"TAK_SHADOW_BENCH": str(s["count"]), "TAK_PROFILE_ZOOM": str(s["zoom"])}
    if s.get("type"):
        env["TAK_SHADOW_BENCH_TYPE"] = s["type"]
    env.update(s.get("env", {}))
    w, h = SIZES[size_key]
    env.update(s.get("size_env", {}).get(f"{w}x{h}", {}))
    return env


SETTINGS = {
    "fullscreen": "0", "vsync": "0", "maxFps": "0", "uiScale": "1", "scorecardScale": "1",
    "treeSway": "1", "unitShadows": "1", "healthBars": "1", "statsPanel": "0",
    "hardwareCursor": "1", "smoothMotion": "1", "edgeScroll": "0", "masterVol": "0",
}


def wait_quiet(args, cpus):
    """Wait (up to --wait-quiet seconds) until the pinned CPUs are mostly idle,
    sampling 3 s at a time. Returns (seconds waited, last busy fraction)."""
    if not args.wait_quiet or not cpus:
        return 0.0, None
    start, busy = time.monotonic(), None
    while True:
        a = cpu_busy(cpus)
        time.sleep(3)
        b = cpu_busy(cpus)
        busy = (b[0] - a[0]) / max(1, b[1] - a[1]) if a and b else 0.0
        if busy <= args.quiet_busy or time.monotonic() - start >= args.wait_quiet:
            return time.monotonic() - start, busy


def run_one(args, client, label, scene, size_key, leg, capture=False):
    w, h = SIZES[size_key]
    name = f"{scene}-{size_key}-{leg:02d}-{label}" + ("-capture" if capture else "")
    root = args.output / name
    if root.exists():
        shutil.rmtree(root)
    profile = root / "profile"
    pref = profile / "TAKengine" / "TAKingdoms"
    pref.mkdir(parents=True, exist_ok=True)
    settings = dict(SETTINGS, **SCENES[scene].get("settings", {}))
    (pref / "settings.ini").write_text("".join(f"{k} = {v}\n" for k, v in settings.items()))
    env = {k: v for k, v in os.environ.items() if not k.startswith("TAK_") and k != "DISPLAY"}
    env.update(SDL_VIDEODRIVER=args.video_driver, SDL_RENDER_DRIVER=args.render_driver,
               SDL_AUDIODRIVER="dummy", XDG_CONFIG_HOME=str(profile), XDG_DATA_HOME=str(profile),
               HOME=str(profile), TAK_PROF="1", TAK_PROF_FRAMES="1", TAK_RENDER_STUDY="1",
               TAK_RENDER_FRAMES=str(args.frames))
    if not args.pipelined:
        env["TAK_PROF_FINISH"] = "1"
    env.update(scene_env(scene, size_key))
    for extra in args.env + (args.a_env if label == "A" else args.b_env):
        k, _, v = extra.partition("=")
        env[k] = v
    if capture:
        (root / "captures").mkdir(parents=True, exist_ok=True)
        env["TAK_RENDER_CAPTURE_DIR"] = str(root / "captures")
    command = [str(client), "game", SCENES[scene].get("map", DEFAULT_MAP), "--data", str(args.data),
               "--testbuild", "--winsize", str(w), str(h), "--novsync", "--maxfps", "0"]
    if args.cpus:
        command = ["taskset", "-c", args.cpus] + command
    cpus = parse_cpus(args.cpus) if args.cpus else set()
    waited, idle_busy = wait_quiet(args, cpus) if not capture else (0.0, None)
    load = {"waited_quiet_s": waited, "busy_before": idle_busy, "loadavg_before": loadavg(), "legion_scenario_before": count_processes("legion_scenario"),
            "pinned_cpus": args.cpus}
    busy0 = cpu_busy(cpus) if cpus else None
    log, err = root / "stdout.log", root / "stderr.log"
    samples, start = [], time.monotonic()
    with log.open("w") as out, err.open("w") as errors:
        process = subprocess.Popen(command, env=env, stdout=out, stderr=errors)
        try:
            while process.poll() is None:
                s = process_sample(process.pid)
                if s:
                    samples.append(s)
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
    busy1 = cpu_busy(cpus) if cpus else None
    load.update(loadavg_after=loadavg(), legion_scenario_after=count_processes("legion_scenario"))
    if busy0 and busy1 and busy1[1] > busy0[1]:
        load["pinned_cpu_busy_fraction"] = (busy1[0] - busy0[0]) / (busy1[1] - busy0[1])
    if process.returncode:
        raise RuntimeError(f"{name}: exit {process.returncode}; see {log}")
    parsed = parse_log(log.read_text() + "\n" + err.read_text(), args.warmup, args.frames)
    if parsed["frames_done"] != args.frames:
        raise RuntimeError(f"{name}: requires a Debug client with render-study support")
    renv = parsed["env"]
    if renv is None:
        if not args.allow_unverified_env:
            raise RuntimeError(f"{name}: client prints no RENDER_ENV (pass --allow-unverified-env "
                               "for a pre-phase-0 client)")
    else:
        if renv.get("renderer") != args.render_driver:
            raise RuntimeError(f"{name}: asked for renderer {args.render_driver}, got {renv.get('renderer')}")
        if renv.get("output") != f"{w}x{h}":
            raise RuntimeError(f"{name}: asked for output {w}x{h}, got {renv.get('output')}")
    if len(parsed["frame"]) != args.frames or len(parsed["drawphase"]) != args.frames:
        raise RuntimeError(f"{name}: incomplete measurement records")
    frame_mode = "pipelined" if args.pipelined else "forced"
    timing = parsed["timing"]
    if timing and timing.get("frame_mode") != ("pipelined" if args.pipelined else "forced_completion"):
        raise RuntimeError(f"{name}: asked for {frame_mode} frames, client reports {timing.get('frame_mode')}")
    stats_last = result_zoom = None
    if parsed["stats"]:
        stats_last = parsed["stats"][-1]
        result_zoom = stats_last.get("zoom")
    interactive = check_interactive(name, scene, log.read_text() + "\n" + err.read_text(),
                                    summarize(parsed, args.warmup, args.frames))
    if result_zoom is not None and abs(result_zoom - SCENES[scene]["zoom"]) > 1e-3 and not (
            SCENES[scene].get("env", {}).get("TAK_RENDER_ZOOM") or
            SCENES[scene].get("env", {}).get("TAK_RENDER_ZOOM_OSC")):
        print(f"{name}: note: requested zoom {SCENES[scene]['zoom']} ran at {result_zoom:.4f} "
              "(map-fills-window clamp)", flush=True)
    result = {
        "name": name, "scene": scene, "actual_zoom_last": result_zoom, "size": size_key, "label": label, "leg": leg,
        "frame_mode": frame_mode, "render_timing": timing,
        "client": str(client), "capture": capture, "world_hash": parsed["world_hash"],
        "render_env": renv, "spawned": parsed["spawned"], "scene_line": parsed["scene_line"],
        "settings": settings, "scene_env": scene_env(scene, size_key),
        "wall_seconds": time.monotonic() - start, "load": load,
        "peak_rss_bytes": max((s[1] for s in samples), default=None),
        "final_rss_bytes": samples[-1][1] if samples else None,
        "measured": summarize(parsed, args.warmup, args.frames),
    }
    if interactive is not None:
        result["interactive"] = interactive
    segments = SCENES[scene].get("segments")
    if segments:
        result["segments"] = {k: summarize(parsed, b, e if e is not None else args.frames)
                              for k, (b, e) in segments.items()}
    if len(samples) > 1:
        ticks = samples[-1][2] - samples[0][2]
        result["client_cpu_percent_all_cores"] = ticks / os.sysconf("SC_CLK_TCK") / (
            samples[-1][0] - samples[0][0]) * 100 / (os.cpu_count() or 1)
    if capture:
        result["captures"] = sorted(p.name for p in (root / "captures").glob("*.png"))
    else:
        shutil.rmtree(profile, ignore_errors=True)
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    m = result["measured"]
    print(f"{name}: total mean {m['frame_ms']['total']['mean']:.3f} p95 {m['frame_ms']['total']['p95']:.3f}"
          f" draw {m['frame_ms']['draw']['mean']:.3f} hash {parsed['world_hash']}"
          f" load {load['loadavg_before']}", flush=True)
    return result


# ---- PNG comparison (8-bit RGB/RGBA, non-interlaced: what tak::png writes) ----

def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, w = 8, [], None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError(f"{path}: unsupported PNG format")
            bpp = 4 if ctype == 6 else 3
        elif kind == b"IDAT":
            idat.append(body)
        pos += 12 + length
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride if f else 0):   # filter 0 (what tak::png writes): bytes as stored
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return w, h, bpp, bytes(out)


def compare_png(a, b, rect=None):
    wa, ha, bpp, pa = read_png(a)
    wb, hb, bppb, pb = read_png(b)
    if (wa, ha, bpp) != (wb, hb, bppb):
        return {"comparable": False}
    x0, y0, x1, y1 = rect if rect else (0, 0, wa, ha)
    differing, max_err = 0, 0
    for y in range(y0, y1):
        row = y * wa * bpp
        sa, sb = pa[row + x0 * bpp:row + x1 * bpp], pb[row + x0 * bpp:row + x1 * bpp]
        if sa == sb:
            continue
        for x in range(0, len(sa), bpp):
            if sa[x:x + bpp] != sb[x:x + bpp]:
                differing += 1
                max_err = max(max_err, max(abs(sa[x + k] - sb[x + k]) for k in range(min(bpp, 3))))
    return {"comparable": True, "pixels": (x1 - x0) * (y1 - y0), "differing": differing,
            "max_channel_error": max_err}


def world_rect(render_env, w, h):
    # The world view the client reports (left of the command panel, above the
    # bottom bar). Excludes the HUD and its dynamic text.
    m = re.match(r"(\d+)x(\d+)$", (render_env or {}).get("world", ""))
    return (0, 0, int(m[1]), int(m[2])) if m else (0, 0, w, h)


# ---- Aggregation ----

KEY_METRICS = [("frame_ms", "total", "mean"), ("frame_ms", "total", "p50"), ("frame_ms", "total", "p95"),
               ("frame_ms", "total", "p99"),
               ("frame_ms", "draw", "mean"), ("frame_ms", "draw", "p50"), ("frame_ms", "draw", "p95"),
               ("frame_ms", "draw", "p99"),
               ("frame_ms", "present", "mean"),
               ("drawphase", "proj", "mean"), ("drawphase", "shadow", "mean"),
               ("drawphase", "body", "mean"), ("gpu", "timeline", "mean")]

# Renderer telemetry carried into the report: mean of each run's per-frame mean
# (gauges ending in _bytes use the run's LAST frame: retained memory at the end).
# Phase/worker times also get an A/B delta with the A/A-noise rule.
TELEMETRY_PHASES = ["t_candidates", "t_painter_sort", "t_atlas", "t_geometry", "t_distant",
                    "t_shadow_pack", "t_shadow_alloc", "t_shadow_prep", "t_shadow_raster",
                    "t_shadow_composite", "t_body_assembly", "t_body_submit", "t_terrain",
                    "t_terrain_upload", "t_effects", "t_fog", "t_pool_dispatch", "t_pool_wait", "t_schedule_plan",
                    "t_hidden_units", "t_cursor", "t_ghosts", "t_build_ghosts",
                    "w_geometry_key", "w_geometry_build", "w_pool_busy", "w_pool_wake",
                    "w_terrain_compose", "w_section_decode", "w_terrain_upload", "w_terrain_wasted",
                    "w_unit_grid_build"]
TELEMETRY_COUNTS = ["visible_units", "geometry_hits", "geometry_misses", "miss_cold", "miss_owner",
                    "miss_resource", "miss_pose", "miss_material", "miss_zoom", "miss_placement",
                    "miss_changed", "inval_pose", "inval_material", "inval_placement", "inval_zoom",
                    "geometry_owner_reassign", "verts_generated", "body_verts_copied",
                    "verts_submitted", "bytes_uploaded", "texture_upload_bytes", "shadow_units",
                    "shadow_tiles_reused", "shadow_tiles_dirty", "shadow_dirty_pixels",
                    "shadow_dirty_new", "shadow_dirty_owner", "shadow_dirty_content",
                    "shadow_dirty_relocated", "shadow_dirty_resized", "shadow_dirty_moved",
                    "shadow_fallback", "atlas_regions", "atlas_pixels", "atlas_flushes",
                    "logical_draws", "native_draws", "sdl_geometry_draws", "retained_draws",
                    "flushes", "target_switches", "distant_images", "distant_hits", "distant_bakes",
                    "terrain_jobs_queued", "terrain_jobs_used", "terrain_jobs_discarded",
                    "terrain_jobs_cancelled", "geom_reallocs", "frame_reallocs", "pool_dispatches",
                    "pool_serial_runs",
                    # workstream C: stable geometry entries
                    "geom_entries", "geom_entry_acquired", "geom_entry_replaced",
                    "geom_entry_released_destroyed", "geom_entry_released_expired",
                    "geom_entry_released_budget", "geom_entry_trimmed", "geom_entry_refused",
                    "geom_entries_retained", "geom_entries_warm_free", "geom_entries_allocated",
                    # workstream E: body/shadow invalidation split
                    "geom_shadow_kept", "geom_shadow_skipped", "shadow_reused_body",
                    "shadow_reused_translated",
                    # workstream F: persistent shadow tile cells
                    "shadow_cell_allocs", "shadow_cell_kept", "shadow_cell_freed",
                    "shadow_cell_expired", "shadow_cell_evicted", "shadow_compactions", "shadow_compactions_free",
                    "shadow_migrated", "shadow_page_releases", "shadow_page_shrinks",
                    "shadow_cells", "shadow_cells_retained", "shadow_pages", "shadow_page_allocs",
                    "shadow_atlas_capacity_px", "shadow_atlas_cell_px", "shadow_atlas_tile_px",
                    "shadow_atlas_shelf_free_px",
                    # workstream I: cost-planned pool passes
                    "pool_posted", "pool_idle_wakes", "pool_chunks", "sched_geometry_threads",
                    "sched_shadow_threads", "sched_copy_threads", "sched_geometry_estimate_us",
                    # workstream J: CPU budgets (per-frame events)
                    "cpu_trims", "cpu_trim_bytes", "terrain_section_decodes", "terrain_section_evictions",
                    "terrain_section_redecodes", "terrain_section_prefetch_skipped", "terrain_mip_evictions",
                    "terrain_mip_regens", "terrain_mip_trims",
                    # workstream K: camera-prioritized terrain
                    "terrain_chunk_visible", "terrain_chunk_ready", "terrain_chunk_standin",
                    "terrain_chunk_holes", "terrain_underlay_tiles", "terrain_chunk_pending",
                    "terrain_jobs_completed", "terrain_section_uploads", "terrain_jobs_obsolete",
                    "terrain_jobs_aborted", "terrain_jobs_nearby", "terrain_jobs_offview",
                    "terrain_upload_deferred", "terrain_gated_frames",
                    # workstream L: unit broad phase
                    "units_live", "units_scanned", "unit_candidates", "unit_index_queries",
                    "unit_index_returned", "unit_index_rows", "unit_index_full_scans",
                    # workstream M: indexed structure palette
                    "palette_indexed_draws", "palette_spans", "palette_span_commands",
                    # workstream N: native passes (primes = SDL state primings, a subset of flushes)
                    "body_submissions", "arena_draws", "native_passes", "native_pass_commands",
                    "native_pass_refused", "native_primes",
                    # workstream H: retained body arena (per frame)
                    "arena_append", "arena_rebuild", "arena_stream", "arena_verts_retained",
                    "arena_verts_uploaded", "arena_verts_twice", "arena_fallbacks",
                    # HIDDEN UNITS option (client/hiddenunits.h)
                    "hidden_eligible", "hidden_candidates", "hidden_marked", "hidden_faded",
                    # measurement B: interactive cases (cursor, ghosts, cover tests, shadow demand)
                    "cursor_evals", "cursor_pick_tests", "cursor_pick_hits", "ghosts_queued", "ghost_preps",
                    "hidden_cover_tests", "shadow_direct_units", "shadow_demand_px",
                    # build ghosts (perf follow-up E)
                    "ghost_draws", "ghost_culled", "ghost_collects", "ghost_cache_hits", "ghost_tris",
                    # perf follow-up F: FADE covers handed out (= the base's exact tests) and exact tests run
                    "hidden_cover_visits", "hidden_cover_reaches",
                    # perf follow-up K: shadow atlas capacity (shadow_demand_px and shadow_direct_units above)
                    "shadow_composite_runs", "shadow_page_limit", "shadow_extended_pages", "shadow_pressure_sheds"]
TELEMETRY_BYTES = ["geom_used_bytes", "geom_capacity_bytes", "body_used_bytes", "body_capacity_bytes",
                   "shadow_scratch_used_bytes", "shadow_scratch_capacity_bytes",
                   "frame_lists_used_bytes", "frame_lists_capacity_bytes", "terrain_mip_cpu_bytes",
                   "terrain_section_cpu_bytes", "gpu_tracked_bytes", "geom_retained_bytes",
                   "geom_warm_free_bytes", "shadow_atlas_gpu_bytes",
                   # workstream J: CPU budgets (gauges; totals exclude GPU memory and RSS)
                   "geom_pinned_bytes", "worker_scratch_used_bytes", "worker_scratch_capacity_bytes",
                   "terrain_section_pinned_bytes", "terrain_section_young_bytes", "terrain_section_peak_bytes",
                   "terrain_mip_pinned_bytes",
                   "terrain_mip_peak_bytes", "arena_cpu_bytes", "distant_metadata_bytes",
                   "cpu_cache_used_bytes", "cpu_cache_retained_bytes", "cpu_cache_pinned_bytes",
                   "cpu_cache_evictable_bytes", "cpu_cache_peak_bytes",
                   # workstream L: the shown snapshot's unit grid
                   "unit_grid_bytes",
                   # workstream H: arena storage (GPU, counted in gpuvram) and its in-flight estimate
                   "arena_capacity_bytes", "arena_inflight_bytes",
                   # build ghosts (perf follow-up E): retained model-local ghost geometry
                   "ghost_cache_bytes"]


def metric(result, path):
    node = result["measured"]
    for k in path:
        if node is None or k not in node:
            return None
        node = node[k]
    return node


# The pinned CPUs' busy fraction includes the client itself (about 0.2 of
# eight CPUs for these scenes). Above this, another process was competing for
# the pinned cores and the run's timing is not comparable.
CONTENDED_BUSY = 0.5


def contended(result):
    return (result["load"].get("pinned_cpu_busy_fraction") or 0) > CONTENDED_BUSY


def spread_entry(vals):
    mean = statistics.fmean(vals)
    spread = (max(vals) - min(vals)) / mean if mean > 0 and len(vals) > 1 else None
    return {"mean": mean, "min": min(vals), "max": max(vals), "aa_rel_spread": spread}


def telemetry(runs):
    out = {}
    for k in TELEMETRY_PHASES + TELEMETRY_COUNTS:
        vals = [r["measured"]["stats"][k]["mean"] for r in runs
                if r["measured"].get("stats") and k in r["measured"]["stats"]]
        if vals:
            out[k] = spread_entry(vals)
    for k in TELEMETRY_BYTES:
        vals = [r["measured"]["stats_last"][k] for r in runs
                if r["measured"].get("stats_last") and k in r["measured"]["stats_last"]]
        if vals:
            out[k] = spread_entry(vals)
    return out


def delta(a, b):
    if not a or not b or a["mean"] <= 0:
        return None
    noise = max(a["aa_rel_spread"] or 0, b["aa_rel_spread"] or 0)
    change = (b["mean"] - a["mean"]) / a["mean"]
    return {"relative": change, "noise": noise, "significant": abs(change) > max(2 * noise, 0.02)}


def aggregate(results, exclude_contended=False):
    # Pipelined and forced-completion runs measure different things: separate
    # groups, never compared with each other.
    groups = {}
    for r in results:
        if r["capture"] or (exclude_contended and contended(r)):
            continue
        key = (r["scene"], r["size"], r.get("frame_mode", "forced"))
        groups.setdefault(key, {}).setdefault(r["label"], []).append(r)
    report = []
    for (scene, size, mode), by_label in sorted(groups.items()):
        row = {"scene": scene, "size": size, "frame_mode": mode, "labels": {}}
        for label, runs in by_label.items():
            entry = {"runs": len(runs)}
            for path in KEY_METRICS:
                vals = [v for v in (metric(r, path) for r in runs) if v is not None]
                if not vals:
                    continue
                entry[".".join(path)] = spread_entry(vals)
            entry["telemetry"] = telemetry(runs)
            st = [r["measured"].get("stats") for r in runs if r["measured"].get("stats")]
            if st:
                entry["visible_units"] = st[0].get("visible_units", {}).get("mean")
                entry["painter_items"] = st[0].get("painter_items", {}).get("mean")
            entry["loadavg"] = [r["load"]["loadavg_before"] for r in runs]
            entry["legion_scenario"] = [r["load"]["legion_scenario_before"] for r in runs]
            entry["contended_runs"] = sum(contended(r) for r in runs)
            row["labels"][label] = entry
        if len(by_label) == 2 and "A" in by_label and "B" in by_label:
            row["delta"] = {}
            for path in KEY_METRICS:
                k = ".".join(path)
                d = delta(row["labels"]["A"].get(k), row["labels"]["B"].get(k))
                if d:
                    row["delta"][k] = d
            ta, tb = row["labels"]["A"]["telemetry"], row["labels"]["B"]["telemetry"]
            for k in TELEMETRY_PHASES:
                d = delta(ta.get(k), tb.get(k))
                if d:
                    row["delta"]["telemetry." + k] = d
        report.append(row)
    return report


def telemetry_table(report, keys, title, scale=1.0, unit=""):
    present = [k for k in keys if any(k in e.get("telemetry", {}) for row in report for e in row["labels"].values())]
    if not present:
        return []
    lines = ["", f"{title}", "", "| scene | size | mode | label | " + " | ".join(present) + " |",
             "|---|---|---|---|" + "---:|" * len(present)]
    for row in report:
        for label, e in sorted(row["labels"].items()):
            t = e.get("telemetry", {})
            cells = []
            for k in present:
                v = t.get(k, {}).get("mean")
                cells.append("-" if v is None else f"{v / scale:.3f}{unit}" if scale != 1.0 or k.startswith(("t_", "w_"))
                             else f"{v:.0f}" if v >= 100 else f"{v:.2f}")
            lines.append(f"| {row['scene']} | {row['size']} | {row.get('frame_mode', 'forced')} | {label} | "
                         + " | ".join(cells) + " |")
    return lines


def markdown(report):
    lines = ["| scene | size | mode | label | runs (contended) | visible | total mean | median | p95 | p99 | draw | present | gpu timeline | A/A spread (total mean) | A/A spread (draw mean) |",
             "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    fmt = lambda e, k: f"{e[k]['mean']:.3f}" if k in e else "-"
    for row in report:
        for label, e in sorted(row["labels"].items()):
            spread = e.get("frame_ms.total.mean", {}).get("aa_rel_spread")
            dspread = e.get("frame_ms.draw.mean", {}).get("aa_rel_spread")
            visible = e.get("visible_units")
            lines.append(f"| {row['scene']} | {row['size']} | {row.get('frame_mode', 'forced')} | {label} | "
                         f"{e['runs']} ({e.get('contended_runs', 0)}) | "
                         f"{'-' if visible is None else f'{visible:.0f}'} | {fmt(e, 'frame_ms.total.mean')} | "
                         f"{fmt(e, 'frame_ms.total.p50')} | {fmt(e, 'frame_ms.total.p95')} | {fmt(e, 'frame_ms.total.p99')} | "
                         f"{fmt(e, 'frame_ms.draw.mean')} | {fmt(e, 'frame_ms.present.mean')} | "
                         f"{fmt(e, 'gpu.timeline.mean')} | "
                         f"{'-' if spread is None else f'{spread * 100:.1f}%'} | "
                         f"{'-' if dspread is None else f'{dspread * 100:.1f}%'} |")
    lines += ["", "GPU timeline = GL_TIME_ELAPSED draw timeline (includes CPU-submission gaps; not GPU busy time)."]
    lines += telemetry_table(report, TELEMETRY_PHASES,
                             "Renderer phases, ms per frame (t_ = render-thread wall, nested phases included in "
                             "their parent; w_ = CPU summed over worker threads)")
    lines += telemetry_table(report, TELEMETRY_COUNTS, "Renderer counters, mean per frame")
    lines += telemetry_table(report, TELEMETRY_BYTES, "Retained memory at the last frame, MiB", scale=1 << 20)
    deltas = [row for row in report if row.get("delta")]
    if deltas:
        lines += ["", "| scene | size | mode | metric | B vs A | noise (max A/A spread) | material |",
                  "|---|---|---|---|---:|---:|---|"]
        for row in deltas:
            for k, d in row["delta"].items():
                lines.append(f"| {row['scene']} | {row['size']} | {row.get('frame_mode', 'forced')} | {k} | "
                             f"{d['relative'] * 100:+.1f}% | "
                             f"{d['noise'] * 100:.1f}% | {'yes' if d['significant'] else 'no'} |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--a", type=Path, required=True, help="baseline optimized-Debug takclient")
    p.add_argument("--b", type=Path, help="candidate takclient (omit for A/A)")
    p.add_argument("--data", type=Path, required=True, help="legally owned retail install")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--scenes", nargs="+", default=QUICK,
                   help=f"scene names or groups: 'all' (the matrix before the interactive cases), 'quick', "
                        f"'interactive'; known: {', '.join(SCENES)}")
    p.add_argument("--sizes", nargs="+", default=["normal"], choices=list(SIZES))
    p.add_argument("--repeats", type=int, default=2, help="A/B: ABBA groups; A/A: runs = 2*repeats")
    p.add_argument("--frames", type=int, default=900)
    p.add_argument("--warmup", type=int, default=300)
    p.add_argument("--timeout", type=float, default=600)
    p.add_argument("--cpus", default="16-23", help="taskset CPU list for the client ('' = unpinned)")
    p.add_argument("--wait-quiet", type=float, default=0,
                   help="before each run, wait up to this many seconds for the pinned CPUs to go idle")
    p.add_argument("--quiet-busy", type=float, default=0.15, help="idle threshold for --wait-quiet")
    p.add_argument("--video-driver", default="offscreen")
    p.add_argument("--render-driver", default="opengl")
    p.add_argument("--pipelined", action="store_true", help="no glFinish before present")
    p.add_argument("--capture", action="store_true", help="also one capture run per label per scene and compare")
    p.add_argument("--capture-only", action="store_true")
    p.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for every run")
    p.add_argument("--a-env", action="append", default=[],
                   help="extra KEY=VALUE for label A runs only (same-binary A/B, e.g. TAK_RENDER_STATS=0)")
    p.add_argument("--b-env", action="append", default=[], help="extra KEY=VALUE for label B runs only")
    p.add_argument("--allow-unverified-env", action="store_true")
    p.add_argument("--list", action="store_true", help="print the scene matrix and exit")
    p.add_argument("--reaggregate", action="store_true",
                   help="rebuild report.json/report.md from --output/results.json without running")
    p.add_argument("--exclude-contended", action="store_true",
                   help=f"leave runs whose pinned CPUs were over {int(CONTENDED_BUSY * 100)}%% busy out of the report")
    args = p.parse_args(argv)
    if args.list:
        for name, s in SCENES.items():
            group = " [interactive]" if name in INTERACTIVE else ""
            print(f"{name:18s} {s.get('map', DEFAULT_MAP):18s} {s['notes']}{group}")
        return 0
    groups = {"all": HISTORICAL, "quick": QUICK, "interactive": list(INTERACTIVE)}
    scenes = [s for name in args.scenes for s in groups.get(name, [name])]
    for s in scenes:
        if s not in SCENES:
            p.error(f"unknown scene {s}")
    if not 0 <= args.warmup < args.frames <= 10000:
        p.error("require 0 <= warmup < frames <= 10000")
    args.output = args.output.resolve()
    if args.reaggregate:
        report = aggregate(json.loads((args.output / "results.json").read_text()), args.exclude_contended)
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        (args.output / "report.md").write_text(markdown(report))
        print(markdown(report))
        return 0
    args.output.mkdir(parents=True, exist_ok=True)
    clients = {"A": args.a.resolve()}
    if args.b:
        clients["B"] = args.b.resolve()
    metadata = {
        "clients": {k: {"path": str(v), "sha256": hashlib.sha256(v.read_bytes()).hexdigest()}
                    for k, v in clients.items()},
        "options": {k: str(v) for k, v in vars(args).items()}, "scenes": {s: SCENES[s] for s in scenes},
        "settings": SETTINGS, "logical_cpus": os.cpu_count(), "uname": " ".join(os.uname()),
        "gpu_measurement": "GL_TIME_ELAPSED draw timeline; includes CPU-submission gaps; not busy time "
                           "(GPU busy time is not measured)",
        "frame_measurement": "pipelined" if args.pipelined else "forced completion (glFinish before present)",
    }
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2, default=str) + "\n")
    order = (["A", "B", "B", "A"] if "B" in clients else ["A", "A"]) * args.repeats
    results = []
    resultfile = args.output / "results.json"
    for size in args.sizes:
        for scene in scenes:
            if size not in SCENES[scene].get("sizes", SIZES):
                print(f"{scene}: skipped at {size} (defined for {', '.join(SCENES[scene]['sizes'])} only)", flush=True)
                continue
            hashes = set()
            if not args.capture_only:
                for leg, label in enumerate(order):
                    r = run_one(args, clients[label], label, scene, size, leg)
                    hashes.add(r["world_hash"])
                    results.append(r)
                    resultfile.write_text(json.dumps(results, indent=2) + "\n")
            if args.capture or args.capture_only:
                caps, cap_env = {}, None
                labels = list(clients) + (["A"] if "B" not in clients else [])
                for i, label in enumerate(labels):
                    r = run_one(args, clients[label], label, scene, size, 90 + i, capture=True)
                    hashes.add(r["world_hash"])
                    caps[f"{label}{i}"] = args.output / r["name"] / "captures"
                    cap_env = cap_env or r["render_env"]
                    results.append(r)
                keys = list(caps)
                w, h = SIZES[size]
                comparison = {}
                for png in sorted(p.name for p in caps[keys[0]].glob("*.png")):
                    a, b = caps[keys[0]] / png, caps[keys[1]] / png
                    if b.exists():
                        comparison[png] = {"world": compare_png(a, b, world_rect(cap_env, w, h)),
                                           "full": compare_png(a, b)}
                (args.output / f"captures-{scene}-{size}.json").write_text(
                    json.dumps({"pair": keys, "comparison": comparison}, indent=2) + "\n")
                worst = max((c["world"].get("differing", 0) for c in comparison.values()), default=0)
                print(f"captures {scene}/{size} {keys[0]} vs {keys[1]}: {len(comparison)} frames, "
                      f"worst world-view differing pixels {worst}", flush=True)
            if len(hashes) > 1:
                raise RuntimeError(f"{scene}/{size}: world hashes differ between runs: {hashes}")
            resultfile.write_text(json.dumps(results, indent=2) + "\n")
    report = aggregate(results, args.exclude_contended)
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    (args.output / "report.md").write_text(markdown(report))
    print(markdown(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
