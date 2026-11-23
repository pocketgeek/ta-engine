#!/usr/bin/env python3
"""Bounded hardware-breakpoint capture of retail's RNG calls under GDB.

Reads arguments, caller and seeds, never patches game code or data. Attaching
and each breakpoint pause the game. All threads resume on detach. Output is
decoded JSONL in an exclusive new file; no executable bytes are exported.

python3 tools/re/trace_rng.py PID --output assets/tmp/retail-traces/rng.jsonl
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def inside_gdb():
    import gdb
    import struct

    limit = int(os.environ['TAK_RNG_LIMIT'])
    records = []
    inferior = gdb.selected_inferior()
    pid = inferior.pid
    crt = os.environ.get('TAK_RNG_CRT') == '1'
    caller_filter = int(os.environ.get('TAK_RNG_CALLER', '0'))

    def u32(address):
        return struct.unpack('<I', inferior.read_memory(address, 4))[0]

    class Observe(gdb.Breakpoint):
        def __init__(self):
            super().__init__('*0x5d4444' if crt else '*0x535cc0', type=gdb.BP_HARDWARE_BREAKPOINT, internal=True)

        def stop(self):
            try:
                sp = int(gdb.parse_and_eval('$esp')) & 0xffffffff
                caller = u32(sp)
                if caller_filter and caller != caller_filter:
                    return False
                bound = None if crt else u32(sp + 4)
                if bound is not None and bound >= 0x80000000:
                    bound -= 0x100000000
                game = u32(0x62d55c)
                record = {'kind': 'crt' if crt else 'rng', 'index': len(records),
                                'thread': gdb.selected_thread().ptid[1],
                                'tick': u32(game + 0x19f44) if game else None,
                                'return_address': caller, 'bound': bound,
                                'seed_before': None if crt else u32(0x64186c)}
                if crt:
                    record['frames'] = []
                    bp = int(gdb.parse_and_eval('$ebp')) & 0xffffffff
                    for _ in range(10):
                        if not bp: break
                        try:
                            parent, ret = u32(bp), u32(bp + 4)
                            record['frames'].append({'frame': bp, 'return_address': ret,
                                                     'args': [u32(bp + 8 + i*4) for i in range(5)]})
                        except gdb.MemoryError:
                            break
                        if parent <= bp or parent - bp > 0x100000: break
                        bp = parent
                records.append(record)
            except Exception as error:
                records.append({'kind': 'error', 'message': str(error)})
                return True
            return len(records) >= limit

    breakpoint = None
    reason = 'limit'
    try:
        breakpoint = Observe()
        print('ARMED: observing CRT calls' if crt else 'ARMED: observing gameplay RNG calls', flush=True)
        gdb.execute('continue')
        if len(records) < limit:
            reason = 'interrupted'
    except (gdb.error, KeyboardInterrupt) as error:
        reason = str(error)
    finally:
        if breakpoint is not None and breakpoint.is_valid():
            breakpoint.delete()
        # Detach before file I/O so the game resumes even if writing fails.
        try:
            gdb.execute('detach')
        except gdb.error as error:
            reason = f'{reason}; detach: {error}'
        with open(os.environ['TAK_RNG_OUTPUT'], 'x') as output:
            for record in [{'kind': 'metadata', 'schema': 1, 'pid': pid,
                            'capture': 'hardware breakpoints; timing perturbed'}, *records,
                           {'kind': 'end', 'reason': reason, 'records': len(records)}]:
                output.write(json.dumps(record) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('--output', required=True)
    parser.add_argument('--limit', type=int, default=512)
    parser.add_argument('--crt', action='store_true', help='observe CRT callers and bounded frame chains; seed is not inferred')
    parser.add_argument('--caller', type=lambda s: int(s, 0), default=0, help='optional return-address filter')
    parser.add_argument('--seconds', type=int, default=20, help='bounded observation window, 1..180 seconds')
    args = parser.parse_args()
    if args.pid < 1 or not 1 <= args.limit <= 4096:
        parser.error('PID must be positive; limit must be 1..4096')
    if not 1 <= args.seconds <= 180 or not 0 <= args.caller <= 0xffffffff:
        parser.error('seconds must be 1..180; caller must be uint32')
    destination = Path(args.output).resolve()
    if destination.exists() or not destination.parent.is_dir():
        parser.error('output must be a new file in an existing directory')
    # Verify supported code/layout while the game continues normally, before
    # asking the debugger to attach. No fallback to software breakpoints.
    from livesample import Mem, verify_code
    mem = Mem(args.pid)
    try:
        verify_code(mem)
    finally:
        mem.close()
    env = dict(os.environ, TAK_RNG_OUTPUT=str(destination), TAK_RNG_LIMIT=str(args.limit),
               TAK_RNG_CRT=str(int(args.crt)), TAK_RNG_CALLER=str(args.caller))
    script = str(Path(__file__).resolve())
    commands = ['gdb', '-nx', '-q', '-batch',
                '-ex', 'set debuginfod enabled off', '-ex', 'set pagination off',
                '-ex', 'set print thread-events off',
                # Wine uses SIGUSR1 for thread coordination; deliver it normally.
                '-ex', 'handle SIGUSR1 nostop noprint pass',
                '-ex', f'attach {args.pid}',
                '-ex', f'python exec(compile(open({script!r}).read(), {script!r}, "exec"))']
    # SIGINT interrupts GDB's continue, allowing the finally block to detach.
    # A hard timeout is a last resort if GDB itself is unresponsive.
    result = subprocess.run(['timeout', '--signal=INT', '--kill-after=5s', f'{args.seconds}s', *commands], env=env)
    if not destination.exists():
        raise RuntimeError(f'GDB produced no trace (status {result.returncode})')
    print(f'Trace saved to {destination}')


if 'gdb' in sys.modules:
    inside_gdb()
elif __name__ == '__main__':
    main()
