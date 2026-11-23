#!/usr/bin/env python3
"""Headless integration check for weapon land/water impact-effect routing.

Runs the rebuilt client against a shipped map and its authored FBI/explosion
assets. The isolated GameView fixture calls the same production hit-effect
helper used by live HitFlash events, then checks class selection, authored
variant loading/timing/expiry, water-class fallback and missing-art particles.
It does not synthesize a projectile hit or establish native retail hit parity.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build-o2/takclient"))
    parser.add_argument("--data", type=Path, default=Path("/home/pocket_geek/TAK/assets/game"))
    parser.add_argument("--map", default="Lake Lokken",
                        help="shipped map containing both land and open water")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    binary = args.binary if args.binary.is_absolute() else root / args.binary
    data = args.data if args.data.is_absolute() else root / args.data
    with tempfile.TemporaryDirectory(prefix="tak-impact-effect-") as tmp:
        shot = Path(tmp) / "headless.png"
        env = os.environ.copy()
        env.update({
            "SDL_VIDEODRIVER": "dummy",
            "SDL_AUDIODRIVER": "dummy",
            "TAK_WEAPON_IMPACT_EFFECT_TEST": "1",
            # A scratch profile: a --data launch saves that folder as the player's
            # data folder setting (src/util/prefpath.h).
            "XDG_DATA_HOME": tmp,
            "XDG_CONFIG_HOME": tmp,
        })
        result = subprocess.run(
            [str(binary), "game", args.map, "--data", str(data), "--firetest",
             "--nofog", "--shot", str(shot)],
            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=120,
        )
    if result.returncode != 0 or "PASS: Arapult land/water authored impact variants" not in result.stdout:
        raise SystemExit(result.stdout or f"client exited {result.returncode} without probe output")
    firing = result.stdout.split("BASILISK_AUDIO_BEGIN\n", 1)[1].split("BASILISK_AUDIO_IMPACT\n", 1)[0]
    impact = result.stdout.split("BASILISK_AUDIO_IMPACT\n", 1)[1].split("BASILISK_AUDIO_END\n", 1)[0]
    if "SND " in firing or "SND arrow08" not in impact:
        raise SystemExit("Basilisk must cast silently and retain its ARROW08 impact:\n" + result.stdout)
    print("PASS: Basilisk silent cast and authored impact sound")
    print(next(line for line in result.stdout.splitlines()
               if line.startswith("PASS: Arapult land/water authored impact variants")))


if __name__ == "__main__":
    main()
