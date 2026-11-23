#!/usr/bin/env python3
"""Compare C++ continuation with retail execution from a captured search.

This is a frozen-world kernel check, not a simulation replay: missions can
cancel the search before it runs. Grades are computed by the retail executable
from the partial captured world; C++ must ask for exactly the same coordinates.
No future-frame state or routes are injected. Heap values match after every
pop, and all cell flags/directions match at the end.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from check_cost_search import digest
from emureload import CapturedProcess
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX


def compare(capture, runner, limit):
    p = CapturedProcess(capture)
    get = lambda offset: p.u32(p.obj + offset)
    assert get(0x5c) == 2, 'requires a suspended phase-2 search'
    width, height = p.frame['map_cells']
    cell_address = get(0x1c)
    cells = bytes(p.uc.mem_read(cell_address, width * height * 4))
    controller = get(0x68)
    assert p.u32(controller) == 0x5f28d8, 'requires the cell-goal controller'
    gx, gz, tolerance = struct.unpack('<hhi', p.uc.mem_read(controller + 8, 8))

    def heap_words():
        pointers = struct.unpack('<' + 'I' * get(0x14), p.uc.mem_read(get(4), get(0x14) * 4))
        result = []
        for pointer in pointers:
            _, x, z, cost, priority, entry, run = struct.unpack('<Ihhiihh', p.uc.mem_read(pointer, 20))
            result.extend((z * width + x, cost, priority, entry, run))
        return result

    heap = heap_words()
    spread = 2 + int(get(0x1ad) > 0)
    lines = [' '.join(map(str, (width, height, heap[0], 0, gx, gz,
                               get(0x54), get(0x40), limit, get(0x44)))),
             f'{tolerance} {get(0x191)} {get(0x18)} {spread} {get(0x14)}',
             ' '.join(str(get(0x90 + i * 4)) for i in range(8)),
             ' '.join(str(get(0x70 + i * 4)) for i in range(8)),
             ' '.join(str(get(i)) for i in (0xc0, 0xc4, 0xbc, 0xc8, 0xb4, 0xb8))]
    lines.extend(f'0 {cells[i]} {cells[i+1]}' for i in range(0, len(cells), 4))
    lines.extend(' '.join(map(str, heap[i:i+5])) for i in range(0, len(heap), 5))
    queries = []
    current = []

    def enter(uc, address, size, data):
        assert not current, 'nested grade query'
        stack = uc.reg_read(UC_X86_REG_ESP)
        ret, x, z = struct.unpack('<Iii', uc.mem_read(stack, 12))
        assert ret == 0x4140d9, 'unexpected cost-search query caller'
        current.extend((x, z))

    def leave(uc, address, size, data):
        assert len(current) == 2
        queries.append((*current, uc.reg_read(UC_X86_REG_EAX)))
        current.clear()

    p.uc.hook_add(UC_HOOK_CODE, enter, begin=0x4139d0, end=0x4139d0)
    p.uc.hook_add(UC_HOOK_CODE, leave, begin=0x4140d9, end=0x4140d9)
    expected = []
    for index in range(limit):
        value, error = p.icd.call(0x4142c0, ecx=p.obj)
        assert error is None, (index + 1, error, p.missing)
        expected.append(f'{get(0x191)} {int(bool(value))} {get(0x54)} {get(0x14)} {digest(heap_words())}')
        if value or get(0x14) == get(0x18):
            break
        p.put(p.obj + 0x44, struct.pack('<I', spread))
    assert not current
    final_cells = bytes(p.uc.mem_read(cell_address, width * height * 4))
    expected.extend((f'CELLS {digest(final_cells[i] | final_cells[i+1] << 8 for i in range(0, len(final_cells), 4))}', 'END'))
    lines.extend(' '.join(map(str, query)) for query in queries)
    result = subprocess.run([str(runner), '--resume'], input='\n'.join(lines) + '\n',
                            text=True, capture_output=True, check=True)
    actual = result.stdout.splitlines()
    assert len(actual) == len(expected), (len(actual), len(expected))
    for index, (a, b) in enumerate(zip(actual, expected)):
        assert a == b, (index + 1, 'port', a, 'retail', b)
    return {'complete_state_match': False, 'pops': len(expected) - 2,
            'grade_queries': len(queries), 'cells': width * height,
            'arrived': bool(value), 'heap_and_final_cells_match': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('runner', type=Path)
    parser.add_argument('--pops', type=int, default=1200)
    args = parser.parse_args()
    if not 1 <= args.pops <= 10000: parser.error('pops must be 1..10000')
    print(json.dumps(compare(json.loads(args.capture.read_text()), args.runner.resolve(), args.pops)))


if __name__ == '__main__':
    main()
