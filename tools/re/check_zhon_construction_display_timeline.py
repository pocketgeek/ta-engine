#!/usr/bin/env python3
"""Compare Zonhunt's display COB with retail on native construction callbacks.

The callback schedule comes from the bounded placed-site 41ef00/4dc800 trace;
the full native and World COB piece/thread state is then compared each tick.
This is headless and does not compare rendered pixels or a live camera.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_COB = ROOT / "assets/extracted/all/scripts/zonhunt.cob"
CHECKER = Path(__file__).with_name("check_script_state.py")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cob", type=Path, default=DEFAULT_COB)
    parser.add_argument("--binary", default="build-o2/retail_script_test")
    parser.add_argument("--ticks", type=int, default=103)
    args = parser.parse_args()
    if args.cob.stem.lower() != "zonhunt":
        parser.error("--cob must select zonhunt.cob")
    if args.ticks < 1 or args.ticks > 10000:
        parser.error("--ticks must be in 1..10000")

    header = struct.unpack_from("<10I", args.cob.read_bytes())
    _, _, piece_count, _, static_count, *_ = header
    state_size = 0xA48 + static_count * 4 + piece_count * 0x6C
    with tempfile.TemporaryDirectory(prefix="tak-zhon-display-") as temporary:
        state = Path(temporary) / "zonhunt-zero.state"
        state.write_bytes(bytes(state_size))
        command = [sys.executable, str(CHECKER), str(args.cob), str(state),
                   "--binary", args.binary, "--ticks", str(args.ticks),
                   "--profile", "0", "--zhon-flight-timeline"]
        subprocess.run(command, cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
