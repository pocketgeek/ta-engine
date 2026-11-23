#!/usr/bin/env python3
"""Reject mislabeled Linux packages before upload (ELF64, little-endian, machine)."""
import pathlib
import struct
import sys

expected = {"x64": 62, "arm64": 183}[sys.argv[1]]
for filename in sys.argv[2:]:
    with pathlib.Path(filename).open("rb") as binary:
        header = binary.read(20)
    if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01":
        sys.exit(f"{filename}: expected little-endian ELF64")
    machine = struct.unpack_from("<H", header, 18)[0]
    if machine != expected:
        sys.exit(f"{filename}: ELF machine {machine}, expected {expected}")
    print(f"{filename}: verified {sys.argv[1]}")
