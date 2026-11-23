#!/usr/bin/env python3
"""Compare engine Bink colors against the user's retail DLL under Proton.

Requires zig, numpy and Pillow. Assets and decoded reference frames stay outside
this repository. A fresh, temporary Proton prefix isolates the reference process.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from PIL import Image

REFERENCE = r'''
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
typedef unsigned (__stdcall *Step)(void*);
int main(int argc, char** argv) {
    if (argc != 5) return 1;
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) return 2;
    void* (__stdcall *open)(const char*, unsigned) =
        (void*)GetProcAddress(dll, "_BinkOpen@8");
    Step frame = (Step)GetProcAddress(dll, "_BinkDoFrame@4");
    Step next = (Step)GetProcAddress(dll, "_BinkNextFrame@4");
    Step close = (Step)GetProcAddress(dll, "_BinkClose@4");
    unsigned (__stdcall *copy)(void*, void*, int, unsigned, unsigned, unsigned, unsigned) =
        (void*)GetProcAddress(dll, "_BinkCopyToBuffer@28");
    if (!open || !frame || !next || !close || !copy) return 3;
    void* b = open(argv[2], 0);
    if (!b) return 4;
    unsigned* info = b;
    int wanted = atoi(argv[4]);
    if (wanted < 0 || wanted >= info[2]) return 5;
    unsigned bytes = info[0] * info[1] * 4;
    void* pixels = calloc(bytes, 1);
    if (!pixels) return 6;
    // Copy every decoded frame: the DLL normally updates dirty regions only.
    // Surface 1 is BGRX, pitch in bytes; no scaling or vertical inversion.
    for (int i = 0; i <= wanted; ++i) {
        frame(b);
        copy(b, pixels, info[0] * 4, info[1], 0, 0, 1);
        if (i < wanted) next(b);
    }
    FILE* f = fopen(argv[3], "wb");
    if (!f) return 7;
    int ok = fwrite(pixels, 1, bytes, f) == bytes;
    fclose(f); free(pixels); close(b);
    return ok ? 0 : 8;
}
'''


def winpath(path):
    return 'Z:' + str(Path(path).resolve()).replace('/', '\\')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--proton', type=Path, required=True)
    parser.add_argument('--steam', type=Path, required=True)
    parser.add_argument('--biktool', type=Path, default=Path('build/biktool'))
    args = parser.parse_args()
    clips = {p.name.lower(): p for p in (args.data / 'Movies/Gui').iterdir()}
    with tempfile.TemporaryDirectory(prefix='tak-bink-colors-') as temp:
        work = Path(temp)
        source, exe = work / 'reference.c', work / 'reference.exe'
        source.write_text(REFERENCE)
        subprocess.run(['zig', 'cc', '-target', 'x86-windows-gnu', str(source),
                        '-o', str(exe)], check=True, timeout=120)
        prefix = work / 'proton'
        prefix.mkdir()
        env = dict(os.environ, STEAM_COMPAT_DATA_PATH=str(prefix),
                   STEAM_COMPAT_CLIENT_INSTALL_PATH=str(args.steam.resolve()),
                   WINEDEBUG='-all', PROTON_USE_XALIA='0')
        try:
            for door in ('snort', 'girl', 'knight', 'machine'):
                for clip, frame in ((4, 0), (5, 0), (5, 10), (7, 0)):
                    movie = clips[f'{door}{clip}.bik']
                    raw, png = work / 'reference.raw', work / 'engine.png'
                    subprocess.run([str(args.proton.resolve()), 'run', str(exe),
                                    winpath(args.data / 'binkw32.dll'), winpath(movie),
                                    winpath(raw), str(frame)],
                                   env=env, check=True, timeout=60,
                                   stdout=subprocess.DEVNULL)
                    subprocess.run([str(args.biktool.resolve()), str(movie), str(frame),
                                    str(png)], check=True, timeout=60,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    engine = np.array(Image.open(png))[:, :, :3].astype(float)
                    height, width = engine.shape[:2]
                    reference = np.fromfile(raw, np.uint8).reshape(height, width, 4)[:, :, [2, 1, 0]]
                    error = np.abs(engine - reference).mean()
                    print(f'{movie.name} frame {frame}: mean RGB error {error:.3f}', flush=True)
                    # Integer conversion/chroma rounding varies between the decoders.
                    # Standard BT.601 fails this bound (4--7 on the idle door frames).
                    if error >= 3:
                        raise AssertionError(f'{movie.name}: color conversion differs from retail')
        finally:
            server = args.proton.resolve().parent / 'files/bin/wineserver'
            if server.exists():
                cleanup_env = dict(env, WINEPREFIX=str(prefix / 'pfx'))
                subprocess.run([str(server), '-k'], env=cleanup_env, timeout=10)
                subprocess.run([str(server), '-w'], env=cleanup_env, timeout=10)



if __name__ == '__main__':
    main()
