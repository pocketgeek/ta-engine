#!/usr/bin/env python3
"""Compare every main-menu door frame with retail Bink and trace BinkWait cadence.

This is a headless decoder/reference probe: it loads the shipped Bink DLL under
Proton, never starts KINGDOMS or a GUI. The DLL helper waits on each native frame
deadline, records its QPC timestamp, and copies the decoded BGRX frame. The engine
side decodes the same frame with biktool and compares RGB values.

Requires zig, numpy, Pillow, Proton, and the retail Movies/Gui clips + binkw32.dll.
"""
import argparse
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

import numpy as np
from PIL import Image


REFERENCE = r'''
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef unsigned (__stdcall *Step)(void*);
typedef unsigned (__stdcall *Wait)(void*);
typedef void (__stdcall *Next)(void*);
#pragma pack(push, 1)
typedef struct Header {
    char magic[8];
    unsigned width, height, frames;
    unsigned long long qpc_frequency;
} Header;
#pragma pack(pop)
int main(int argc, char** argv) {
    if (argc != 5) return 1;
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) return 2;
    void* (__stdcall *open)(const char*, unsigned) =
        (void*)GetProcAddress(dll, "_BinkOpen@8");
    Step frame = (Step)GetProcAddress(dll, "_BinkDoFrame@4");
    Next next = (Next)GetProcAddress(dll, "_BinkNextFrame@4");
    Step close = (Step)GetProcAddress(dll, "_BinkClose@4");
    Wait wait = (Wait)GetProcAddress(dll, "_BinkWait@4");
    unsigned (__stdcall *copy)(void*, void*, int, unsigned, unsigned, unsigned, unsigned) =
        (void*)GetProcAddress(dll, "_BinkCopyToBuffer@28");
    if (!open || !frame || !next || !close || !wait || !copy) return 3;
    void* b = open(argv[2], 0);
    if (!b) return 4;
    unsigned* info = (unsigned*)b;
    unsigned width = info[0], height = info[1];
    unsigned frames = (unsigned)strtoul(argv[4], NULL, 10);
    if (!width || !height || !frames || width > 4096 || height > 4096 || frames > 10000)
        return 5;
    FILE* out = fopen(argv[3], "wb");
    if (!out) return 6;
    unsigned bytes = width * height * 4;
    unsigned char* pixels = (unsigned char*)calloc(bytes, 1);
    if (!pixels) return 7;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency)) return 8;
    Header h = {{'T','A','K','D','O','O','R','1'}, width, height, frames,
                (unsigned long long)frequency.QuadPart};
    if (fwrite(&h, 1, sizeof(h), out) != sizeof(h)) return 9;
    LARGE_INTEGER stamp;
    for (unsigned i = 0; i < frames; ++i) {
        // Retail's render path spins on BinkWait until this exact decoded frame is
        // due, then calls DoFrame, copies the dirty-updated surface, and advances.
        while (wait(b)) { }
        QueryPerformanceCounter(&stamp);
        frame(b);
        copy(b, pixels, (int)(width * 4), height, 0, 0, 1);
        unsigned long long ticks = (unsigned long long)stamp.QuadPart;
        if (fwrite(&ticks, 1, sizeof(ticks), out) != sizeof(ticks)
            || fwrite(pixels, 1, bytes, out) != bytes) return 10;
        next(b);
    }
    free(pixels);
    fclose(out);
    close(b);
    return 0;
}
'''


def winpath(path: Path) -> str:
    return 'Z:' + str(path.resolve()).replace('/', '\\')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True,
                        help='retail root containing Movies/Gui and binkw32.dll')
    parser.add_argument('--proton', type=Path, required=True)
    parser.add_argument('--steam', type=Path, required=True)
    parser.add_argument('--biktool', type=Path, default=Path('build-o2/biktool'))
    parser.add_argument('--mean-error-limit', type=float, default=3.0,
                        help='per-frame mean absolute RGB error bound (default: 3/255)')
    args = parser.parse_args()

    def ci_child(root: Path, *wanted: str) -> Path:
        for component in wanted:
            lower = component.lower()
            matches = [p for p in root.iterdir() if p.name.lower() == lower]
            if len(matches) != 1:
                raise FileNotFoundError(f'{root}: expected exactly one {component!r}')
            root = matches[0]
        return root

    clips_dir = ci_child(args.data, 'Movies', 'Gui')
    dll = ci_child(args.data, 'binkw32.dll')
    clips = {}
    for door in ('machine', 'girl', 'knight', 'snort'):
        for n in range(4, 8):
            clips[f'{door}{n}'] = ci_child(clips_dir, f'{door}{n}.bik')

    with tempfile.TemporaryDirectory(prefix='tak-bink-all-frames-') as temp_name:
        work = Path(temp_name)
        source, exe = work / 'reference.c', work / 'reference.exe'
        source.write_text(REFERENCE)
        subprocess.run(['zig', 'cc', '-target', 'x86-windows-gnu', str(source),
                        '-o', str(exe)], check=True, timeout=120)
        prefix = work / 'proton-data'
        prefix.mkdir()
        env = dict(os.environ, STEAM_COMPAT_DATA_PATH=str(prefix),
                   STEAM_COMPAT_CLIENT_INSTALL_PATH=str(args.steam.resolve()),
                   WINEDEBUG='-all', PROTON_USE_XALIA='0')
        failures = []
        total_frames = 0
        try:
            for name, movie in clips.items():
                ref_path = work / f'{name}.ref'
                info = subprocess.run([str(args.biktool), str(movie)], check=True,
                                      capture_output=True, text=True, timeout=120)
                metadata = info.stdout.strip()
                match = re.search(r'bik:\s+(\d+)x(\d+)\s+fps=([0-9.]+)\s+frames=(\d+)', metadata)
                if not match:
                    raise RuntimeError(f'{name}: could not parse local Bink metadata: {metadata}')
                local_width, local_height, fps, frame_count = (
                    int(match.group(1)), int(match.group(2)), float(match.group(3)),
                    int(match.group(4)))
                container = movie.read_bytes()[:44]
                if len(container) < 44 or container[:4] != b'BIKf':
                    raise RuntimeError(f'{name}: unsupported or truncated Bink container header')
                container_count = struct.unpack_from('<I', container, 8)[0]
                container_width, container_height = struct.unpack_from('<II', container, 20)
                rate_num, rate_den = struct.unpack_from('<II', container, 28)
                if not rate_num or not rate_den:
                    raise RuntimeError(f'{name}: invalid Bink frame rate {rate_num}/{rate_den}')
                container_fps = rate_num / rate_den
                if ((local_width, local_height, frame_count) !=
                        (container_width, container_height, container_count)
                        or abs(fps - container_fps) > 1e-6):
                    failures.append(f'{name}: biktool/container metadata mismatch: '
                                    f'engine={local_width}x{local_height}/{fps:g}fps/{frame_count}; '
                                    f'container={container_width}x{container_height}/'
                                    f'{container_fps:g}fps/{container_count}')
                    continue
                subprocess.run([str(args.proton.resolve()), 'run', str(exe),
                                winpath(dll), winpath(movie), winpath(ref_path), str(container_count)],
                               env=env, check=True, timeout=180,
                               stdout=subprocess.DEVNULL)
                ref = ref_path.read_bytes()
                if len(ref) < 28:
                    raise RuntimeError(f'{name}: truncated native trace')
                magic, width, height, traced_count, qpc_hz = struct.unpack_from('<8sIIIQ', ref)
                if magic != b'TAKDOOR1' or not qpc_hz:
                    raise RuntimeError(f'{name}: invalid native trace header')
                frame_bytes = width * height * 4
                expected = 28 + traced_count * (8 + frame_bytes)
                if len(ref) != expected:
                    raise RuntimeError(f'{name}: trace length {len(ref)} != {expected}')
                if (width, height, traced_count) != (local_width, local_height, container_count):
                    failures.append(f'{name}: native/local stream metadata mismatch: '
                                    f'native={width}x{height}/{traced_count}, '
                                    f'local={local_width}x{local_height}/{container_count}')
                    continue

                times = []
                errors = []
                record = 28
                for frame_index in range(traced_count):
                    ticks = struct.unpack_from('<Q', ref, record)[0]
                    record += 8
                    native = np.frombuffer(ref, np.uint8, count=frame_bytes,
                                           offset=record).reshape(height, width, 4)
                    record += frame_bytes
                    # biktool writes exactly the engine BinkVideo RGBA conversion used
                    # by the main menu. It accepts one frame number and emits a PNG.
                    png = work / 'engine-frame.png'
                    subprocess.run([str(args.biktool), str(movie), str(frame_index), str(png)],
                                   check=True, capture_output=True, timeout=120)
                    engine = np.asarray(Image.open(png).convert('RGB'), dtype=np.int16)
                    retail_rgb = native[:, :, [2, 1, 0]].astype(np.int16)
                    error = np.abs(engine - retail_rgb).mean()
                    errors.append(float(error))
                    times.append(ticks / qpc_hz)

                intervals_ms = np.diff(np.asarray(times)) * 1000.0
                expected_interval = 1000.0 / container_fps
                median_interval = float(np.median(intervals_ms)) if len(intervals_ms) else 0.0
                max_error = max(errors, default=0.0)
                mean_error = float(np.mean(errors)) if errors else 0.0
                total_frames += traced_count
                interval_text = (f'{median_interval:.2f}ms (header={expected_interval:.2f}ms)'
                                 if len(intervals_ms) else 'n/a (single-frame clip)')
                print(f'{movie.name}: {width}x{height}, {traced_count} frames; '
                      f'BinkWait median={interval_text}; '
                      f'RGB mean={mean_error:.3f}, worst frame={max_error:.3f}', flush=True)
                if max_error >= args.mean_error_limit:
                    bad_frame = int(np.argmax(errors))
                    failures.append(f'{name}: frame {bad_frame} mean RGB error '
                                    f'{max_error:.3f} >= {args.mean_error_limit:.3f}')
                # BinkWait is clocked to the media stream. Allow 12 ms for its
                # scheduler/QPC granularity while rejecting a wrong playback rate.
                if len(intervals_ms) and abs(median_interval - expected_interval) > 12.0:
                    failures.append(f'{name}: median BinkWait interval '
                                    f'{median_interval:.2f}ms differs from '
                                    f'{expected_interval:.2f}ms')
        finally:
            server = args.proton.resolve().parent / 'files/bin/wineserver'
            if server.exists():
                cleanup_env = dict(env, WINEPREFIX=str(prefix / 'pfx'))
                subprocess.run([str(server), '-k'], env=cleanup_env, timeout=10)
                subprocess.run([str(server), '-w'], env=cleanup_env, timeout=10)

        print(f'Compared {total_frames} frames across {len(clips)} door clips.')
        if failures:
            for failure in failures:
                print('FAIL:', failure)
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
