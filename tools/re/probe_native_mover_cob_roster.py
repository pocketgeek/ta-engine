#!/usr/bin/env python3
"""Discover and compare native mover→COB transitions for the shipped roster.

This enumerates every FBI unit with canmove enabled whose matching shipped COB
declares at least one native mover callback, then passes that roster to
probe_native_mover_cob_transitions.py. The child probe drives the native mover
and native COB scheduler in headless Unicorn and compares complete COB state
with the World script VM. No game GUI is launched.
"""
import argparse
import subprocess
import sys
from pathlib import Path

from check_movement_callback_order import (
    CALLBACKS,
    ROOT,
    SCRIPT_ROOT,
    UNIT_ROOT,
    cob_methods,
    fbi_info,
)


def discover_units():
    """Return sorted movable FBI/COB pairs with movement callback methods."""
    units = []
    for fbi in sorted(UNIT_ROOT.glob("*.fbi")):
        name = fbi.stem.lower()
        info = fbi_info(name)
        if not bool(float(info.get("canmove", "0"))):
            continue
        cob = SCRIPT_ROOT / f"{name}.cob"
        if not cob.is_file():
            continue
        if cob_methods(name).intersection(CALLBACKS):
            units.append(name)
    return units


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--script-binary",
        default="build-o2/retail_script_test",
        help="World COB VM comparison binary (default: %(default)s)",
    )
    parser.add_argument(
        "--expect-count",
        type=int,
        default=151,
        help="fail if asset discovery finds another count (0 disables; default: %(default)s)",
    )
    args = parser.parse_args()

    units = discover_units()
    if args.expect_count and len(units) != args.expect_count:
        raise SystemExit(
            f"expected {args.expect_count} movable FBI/COB pairs, found {len(units)}"
        )
    print(
        f"Discovered {len(units)} movable shipped FBI/COB pairs declaring "
        f"{', '.join(CALLBACKS)}.",
        flush=True,
    )
    probe = Path(__file__).with_name("probe_native_mover_cob_transitions.py")
    subprocess.run(
        [
            sys.executable,
            str(probe),
            "--script-binary",
            args.script_binary,
            "--units",
            *units,
        ],
        cwd=ROOT,
        check=True,
    )


if __name__ == "__main__":
    main()
