#!/usr/bin/env python3
"""Capture the fullscreen Proton game and normalize its centered low-res surface."""

import argparse
import subprocess
import tempfile
from pathlib import Path

from PIL import Image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="normalized client screenshot path")
    parser.add_argument(
        "--source", type=Path,
        help="use an existing full-desktop capture instead of invoking Spectacle",
    )
    parser.add_argument("--client-width", type=int, default=640)
    parser.add_argument("--client-height", type=int, default=480)
    args = parser.parse_args()

    temp_path = None
    source = args.source
    if source is None:
        with tempfile.NamedTemporaryFile(prefix="retail-full-", suffix=".png", delete=False) as f:
            temp_path = Path(f.name)
        subprocess.run(["spectacle", "-b", "-n", "-o", str(temp_path)], check=True)
        source = temp_path

    try:
        with Image.open(source) as desktop:
            width, height = desktop.size
            scale = min(width / args.client_width, height / args.client_height)
            surface_width = round(args.client_width * scale)
            surface_height = round(args.client_height * scale)
            left = (width - surface_width) // 2
            top = (height - surface_height) // 2
            client = desktop.crop((left, top, left + surface_width, top + surface_height))
            if client.size != (args.client_width, args.client_height):
                client = client.resize((args.client_width, args.client_height), Image.Resampling.NEAREST)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            client.save(args.output)
            print(
                f"captured {width}x{height}; surface {left},{top} "
                f"{surface_width}x{surface_height}; client {args.client_width}x{args.client_height} "
                f"-> {args.output}"
            )
    finally:
        if temp_path is not None:
            temp_path.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
