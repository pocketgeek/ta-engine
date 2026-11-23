#!/usr/bin/env python3
"""Compare mobile placement with retail using initial captured map/entity data.

This verifies the placement core's inputs on a real map, not World map loading
or simulation integration. No later captured frame is injected.
"""
import argparse
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile

from emureload import CapturedProcess


def compare(capture, runner, cases, world_fixture=None, retail_root=None):
    p = CapturedProcess(capture)
    width, height = p.frame['map_cells']
    read = lambda address, size: bytes(p.uc.mem_read(address, size))
    word = lambda address: struct.unpack('<H', read(address, 2))[0]
    byte = lambda address: read(address, 1)[0]
    cell_base = p.u32(p.game + 0x19f04)
    feature_count = p.u32(p.game + 0x19ec0)
    feature_base = p.u32(p.game + 0x19edc)
    flags = [p.u32(feature_base + i*320 + 0x13c) for i in range(feature_count)]
    entity_base, entity_end = (p.u32(p.game + offset) for offset in (0x14e84, 0x14e88))
    if entity_end < entity_base or (entity_end-entity_base) % 312:
        raise ValueError('invalid captured entity table')
    entities = []
    for address in range(entity_base, entity_end, 312):
        entities.extend((1, int(bool(p.u32(address + 0x130) & 0x1000000)),
                         int(bool(p.u32(address + 8))), word(address + 2)))
    mobile = [u for u in p.frame['units'] if byte(u['type_address'] + 0x24a) == 1]
    if not mobile:
        raise ValueError('capture has no mobile placement types')
    sea = byte(p.game + 0x19ef8)
    plane = read(cell_base, width*height*14)
    occupied = [i for i in range(width*height)
                if struct.unpack_from('<H', plane, i*14)[0]]
    featured = [i for i in range(width*height)
                if struct.unpack_from('<H', plane, i*14+8)[0] != 0xffff]
    rng = random.Random(0x507d10)
    expected, fixtures, queries = [], [], []
    p.icd.freeze_hooks()
    for case in range(cases):
        unit = mobile[case % len(mobile)]
        kind, identity = unit['type_address'], unit['id']
        fx, fz = struct.unpack('<hh', read(kind + 0x126, 4))
        max_depth, min_depth = struct.unpack('<hh', read(kind + 0x192, 4))
        slope, water_slope = read(kind + 0x23c, 2)
        candidates = occupied if case % 4 == 0 else featured if case % 4 == 1 else []
        if candidates:
            index = rng.choice(candidates)
            x, z = index % width, index // width
        else:
            x, z = rng.randint(-1, width), rng.randint(-1, height)
        moving = rng.randrange(2)
        packed = ((z & 0xffff) << 16) | (x & 0xffff)
        packed = struct.unpack('<i', struct.pack('<I', packed))[0]
        result, error = p.icd.call(0x507d10, (kind, identity, packed, 1, moving))
        if error or p.missing:
            raise AssertionError((case, error, p.missing))
        cells = {}

        def add_cell(cx, cz):
            if not (0 <= cx < width and 0 <= cz < height):
                raise ValueError('captured feature anchor outside map')
            raw = plane[(cz*width+cx)*14:(cz*width+cx+1)*14]
            cell = (struct.unpack_from('<H', raw)[0], struct.unpack_from('<H', raw, 8)[0],
                    raw[5], raw[6], raw[11], raw[10])
            cells[cx, cz] = cell
            return cell

        if x >= 0 and z >= 0 and x+fx < width and z+fz < height:
            for cz in range(z, z+fz):
                for cx in range(x, x+fx):
                    cell = add_cell(cx, cz)
                    if cell[1] == 0xfffe:
                        add_cell(cx-cell[4], cz-cell[5])
        row = [x,z,fx,fz,width,height,sea,max_depth,min_depth,slope,water_slope,
               identity,moving,feature_count,len(entities)//4,*flags,*entities,len(cells)]
        for (cx, cz), cell in sorted(cells.items()):
            row.extend((cx, cz, *cell))
        fixtures.append(' '.join(map(str, row)))
        expected.append(result)
        queries.append((identity, x, z, moving))
    if world_fixture is None:
        output = subprocess.run([str(runner), '--mobile-placement-sparse'],
                                input='\n'.join(fixtures)+'\n', text=True,
                                capture_output=True, check=True)
        actual = list(map(int, output.stdout.split()))
    else:
        with tempfile.TemporaryDirectory(prefix='tak-placement-world-') as directory:
            root = Path(directory)
            lines = world_fixture.read_text().splitlines()
            header = lines[0].split()
            if int(header[2]) != p.frame['tick']:
                raise ValueError('World fixture starts at another tick')
            header[5] = '0'
            lines[0] = ' '.join(header)
            (root/'input.txt').write_text('\n'.join(lines)+'\n')
            (root/'queries.txt').write_text('\n'.join(' '.join(map(str, q)) for q in queries)+'\n')
            subprocess.run([str(runner), str(retail_root), str(root/'input.txt'), str(root/'output.jsonl'),
                            '--placement-queries', str(root/'queries.txt')], check=True)
            events = [json.loads(line) for line in (root/'output.jsonl').read_text().splitlines()]
            actual = [e['result'] for e in events if e['kind'] == 'placement']
    if len(actual) != len(expected):
        raise AssertionError(('result count', len(actual), len(expected)))
    for index, (a, b) in enumerate(zip(actual, expected)):
        if a != b:
            raise AssertionError((index, queries[index], 'port', a, 'retail', b))
    print(f'PASS: {cases} {"World" if world_fixture else "captured"} mobile placements ({sum(expected)} accepted), '
          f'{len(mobile)} mobile units, initial tick {p.frame["tick"]}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--runner', type=Path, default=Path('build-dbg/retail_ai_test'))
    parser.add_argument('--cases', type=int, default=1000)
    parser.add_argument('--world-fixture', type=Path)
    parser.add_argument('--retail-root', type=Path)
    args = parser.parse_args()
    if not 1 <= args.cases <= 10000:
        parser.error('cases must be 1..10000')
    if args.world_fixture and not args.retail_root:
        parser.error('--world-fixture requires --retail-root and a retail_replay_probe runner')
    compare(json.loads(args.capture.read_text()), args.runner, args.cases,
            args.world_fixture, args.retail_root)


if __name__ == '__main__':
    main()
