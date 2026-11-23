#!/usr/bin/env python3
"""Inventory literal EMIT_SFX arguments in archived scripts (all versions).

Reports direct PUSH_CONST/EMIT_SFX pairs only. Computed values and runtime
reachability are deliberately not inferred from this static inventory.
"""
import collections
import pathlib
import re
import struct
import subprocess
import sys
root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else 'assets/game')
tool = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else 'build/hpitool').resolve()
metadata = pathlib.Path("src/cob/cob.cpp").read_text()
ops = {int(op, 16): int(argc) for op, argc in re.findall(
    r'\{(0x[0-9A-Fa-f]+),\s*"[^"]+",\s*(\d+)\}', metadata)}
counts = collections.Counter()
legacy = []
scripts = sites = direct = 0
for archive in sorted(root.iterdir()):
    if archive.suffix.lower() not in ('.hpi', '.ufo'):
        continue
    listing = subprocess.check_output([tool, 'list', archive], text=True)
    for line in listing.splitlines():
        fields = line.strip().split(maxsplit=1)
        if len(fields) != 2 or not fields[1].lower().endswith('.cob'):
            continue
        name = fields[1]
        data = subprocess.check_output([tool, 'cat', archive, name])
        words, offset = struct.unpack_from('<I', data, 12)[0], struct.unpack_from('<I', data, 36)[0]
        code = struct.unpack_from(f'<{words}I', data, offset)
        scripts += 1
        # Decode instruction boundaries using the project's opcode metadata.
        previous = None
        pc = 0
        while pc < len(code):
            op = code[pc]
            if op not in ops:
                raise RuntimeError(f'{archive}:{name}: unknown opcode {op:x} at {pc}')
            if op == 0x1000f000:
                sites += 1
                if previous is not None and code[previous] == 0x10021001:
                    value = code[previous + 1]
                    counts[value] += 1
                    direct += 1
                    if value < 256:
                        legacy.append(f'{archive.name}:{name}:{pc} code={value}')
            previous, pc = pc, pc + 1 + ops[op]
print(f'{scripts} archived scripts; {sites} EMIT_SFX sites; {direct} direct literal sites')
print('literal counts:', dict(sorted(counts.items())))
print('\n'.join(legacy))
