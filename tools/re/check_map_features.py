#!/usr/bin/env python3
"""Compare a map loaded by World with an initial retail feature plane.

No captured cells are fed to the port. With --fixture, restore initial feature
identities through production placement before comparing every cell.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from captured_memory import initial_memory_reader


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--runner', type=Path, default=None)
    ap.add_argument('--retail-root', type=Path, required=True)
    ap.add_argument('--map')
    ap.add_argument('--fixture', type=Path)
    args = ap.parse_args()
    frame = json.loads(args.capture.read_text())['frames'][0]
    read = initial_memory_reader(frame)
    game = next(r['address'] for r in frame['runtime_state']['world_buffers']
                if r['name'] == 'game_fields')
    word = lambda address: struct.unpack('<I', read(address, 4))[0]
    width, height = frame['map_cells']
    cells = read(word(game+0x19f04), width*height*14)
    count, table = word(game+0x19ec0), word(game+0x19edc)
    names = [read(table+i*320, 32).split(b'\0')[0].decode('ascii').lower()
             for i in range(count)]
    if args.fixture:
        runner = args.runner or Path('build-dbg/retail_replay_probe')
        fixture = args.fixture.read_text().splitlines()
        header = fixture[0].split()
        if int(header[1]) < 34 or int(header[2]) != frame['tick']:
            raise ValueError('fixture must have matching initial tick and captured balance selection')
        if int(header[-1]) != read(0x641144, 1)[0]:
            raise ValueError('fixture balance differs from capture')
        header[5] = '0'
        fixture[0] = ' '.join(header)
        with tempfile.TemporaryDirectory(prefix='tak-feature-map-') as directory:
            root = Path(directory)
            (root/'input').write_text('\n'.join(fixture)+'\n')
            subprocess.run([str(runner),str(args.retail_root),str(root/'input'),
                            str(root/'output'),'--feature-map'],check=True)
            lines = (root/'output').read_text().splitlines()
    else:
        if not args.map:
            raise ValueError('--map is required without --fixture')
        runner = args.runner or Path('build-dbg/retail_ai_test')
        result = subprocess.run([str(runner), '--map-features', str(args.retail_root), args.map],
                                capture_output=True, text=True, check=True)
        lines = result.stdout.splitlines()
    if list(map(int, lines[0].split())) != [width, height] or len(lines) != width*height+1:
        raise AssertionError('port map dimensions differ')
    for index, line in enumerate(lines[1:]):
        name, back_x, back_z, sample, low, clearable = line.split()
        native = struct.unpack_from('<H', cells, index*14+8)[0]
        expected = names[native] if native < count else str(native)
        if native < count:
            flags = word(table+native*320+316)
            if bool(int(clearable)) != bool(flags & 0x20000):
                raise AssertionError((index % width,index // width,name,'clearable',clearable,hex(flags)))
        if name != expected:
            raise AssertionError((index % width, index // width, 'feature', name, expected))
        if native == 0xfffe and (int(back_x), int(back_z)) != (cells[index*14+11], cells[index*14+10]):
            raise AssertionError((index % width, index // width, 'footprint anchor'))
        if int(sample) != cells[index*14+4]:
            raise AssertionError((index % width, index // width, 'height'))
        # The last row/column have no full terrain quad and placement rejects
        # them before reading their bounds. Their native padding is not terrain.
        if index % width < width-1 and index // width < height-1 and int(low) != cells[index*14+6]:
            raise AssertionError((index % width, index // width, 'quad low height'))
    print(f'PASS: {width*height} World-loaded feature cells, clearability flags and footprint references; '
          f'heights and {(width-1)*(height-1)} interior quad lows, initial tick {frame["tick"]}')


if __name__ == '__main__':
    main()
