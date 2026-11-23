#!/usr/bin/env python3
"""Package the existing procedural application artwork for Windows and macOS.

Run after building makeicons:
  python3 tools/make-platform-icons.py build/makeicons
Only Python's standard library is needed; generated icons are checked in so
normal builds need neither this script nor an image conversion dependency.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('renderer', type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[1] / 'res/icons')
    parser.add_argument('--app', choices=('takclient', 'cartographer'), default='takclient')
    args = parser.parse_args()
    images = {}
    with tempfile.TemporaryDirectory(prefix='tak-icons-') as temp:
        for size in (16, 24, 32, 48, 64, 128, 256, 512, 1024):
            folder = Path(temp) / str(size)
            subprocess.run([str(args.renderer.resolve()), str(folder), str(size)], check=True,
                           stdout=subprocess.DEVNULL)
            images[size] = (folder / (args.app + '.png')).read_bytes()
    args.output.mkdir(parents=True, exist_ok=True)
    sizes = (16, 24, 32, 48, 64, 128, 256)
    ico = bytearray(struct.pack('<HHH', 0, 1, len(sizes)))
    offset = 6 + 16 * len(sizes)
    for size in sizes:
        data = images[size]
        ico += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for size in sizes:
        ico += images[size]
    (args.output / (args.app + '.ico')).write_bytes(ico)
    chunks = bytearray()
    for tag, size in ((b'icp4',16), (b'icp5',32), (b'icp6',64), (b'ic07',128),
                      (b'ic08',256), (b'ic09',512), (b'ic10',1024),
                      (b'ic11',32), (b'ic12',64), (b'ic13',256), (b'ic14',512)):
        data = images[size]
        chunks += tag + struct.pack('>I', len(data)+8) + data
    (args.output / (args.app + '.icns')).write_bytes(b'icns' + struct.pack('>I', len(chunks)+8) + chunks)
    print(f'Wrote {args.app}.ico and {args.app}.icns from the existing application artwork.')


if __name__ == '__main__':
    main()
