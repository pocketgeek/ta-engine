#!/usr/bin/env python3
"""Compare all navigation modes with serial/workers and checked-in checkpoints.

The golden is deliberately external to the binary so compiler/architecture
differences fail CI too. Updating it requires a reviewed simulation change.
"""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("golden", type=Path)
    args = parser.parse_args()
    expected = json.loads(args.golden.read_text())
    for mode in ("retail", "legion"):
        outputs = {}
        for execution in ("serial", "workers"):
            run = subprocess.run([str(args.binary.resolve()), mode, execution],
                                 check=True, text=True, capture_output=True, timeout=180)
            checkpoints = [line for line in run.stdout.splitlines() if line.startswith("mode=")]
            if len(checkpoints) != 24:
                raise RuntimeError(f"{mode}/{execution}: expected 24 checkpoints, got {len(checkpoints)}")
            outputs[execution] = checkpoints
        if outputs["serial"] != outputs["workers"]:
            raise RuntimeError(f"{mode}: serial/worker state differs")
        if outputs["serial"] != expected[mode]:
            differences = [(i, old, new) for i, (old, new) in
                           enumerate(zip(expected[mode], outputs["serial"])) if old != new]
            raise RuntimeError(f"{mode}: cross-build golden mismatch: {differences[:3]}")
        print(f"{mode}: 24 golden checkpoints agree, serial and workers", flush=True)


if __name__ == "__main__":
    main()
