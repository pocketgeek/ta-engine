#!/usr/bin/env python3
"""The headless harness keeps its seat, and reports the exact end tick, when it simulates slowly.

Real takserver + headless takclient games (Inner Circle, --seed 1) with every client tick
slowed by TAK_SIM_SLOW_US, standing in for a stress game on a loaded machine (the 0.9.4
sweeps measured 200-500 ms per tick):

  seat    300 ms per tick. The client used to service its socket only every 64 ticks it
          simulated inline, and 64 such ticks are about 19 s of silence: the server dropped
          the seat after its 15 s timeout ("peer closed"). The game must now run to its time
          limit with no error.
  result  150 ms per tick with the server far ahead; the player destroys its own Monarch
          (TAK_DEFEAT_TEST=2). The client used to drain every buffered bundle past the
          result, so the reported end tick (and hash) depended on how far behind it was.
          The run must now end at exactly the tick its result landed (tick == result).

Debug builds only (the harness flags and TAK_SIM_SLOW_US).
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

MAP = 'Inner Circle'
DONE = re.compile(r'mp-headless done: tick=(\d+) hash=(\w+) units=\d+ err=(.+?) end=(\w+)(?: result=(\d+))?')


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def environment(root, name):
    env = {k: v for k, v in os.environ.items() if not k.startswith('TAK_')}
    for sub in ('data', 'config', 'home'):
        (root / name / sub).mkdir(parents=True, exist_ok=True)
    env.update(XDG_DATA_HOME=str(root / name / 'data'), XDG_CONFIG_HOME=str(root / name / 'config'),
               TAK_HEADLESS='1', SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    if os.name != 'nt':
        env['HOME'] = str(root / name / 'home')
    return env


def play(args, root, slow_us, seconds, extra):
    """One game; returns (client log, server log)."""
    port = free_port()
    slog_path = root / 'server.log'
    with open(slog_path, 'w') as slog:
        server = subprocess.Popen([args.server, '--port', str(port), '--data', args.data, '--local',
                                   '--seed', '1', '--no-auth'],
                                  stdout=slog, stderr=subprocess.STDOUT, env=environment(root, 'server'))
        try:
            deadline = time.monotonic() + 90
            while 'listening' not in slog_path.read_text(errors='replace'):
                if time.monotonic() > deadline or server.poll() is not None:
                    raise AssertionError('server never listened:\n' + slog_path.read_text(errors='replace')[-3000:])
                time.sleep(0.1)
            env = environment(root, 'client')
            env.update(TAK_SPEED='80', TAK_SIM_SLOW_US=str(slow_us), **extra)
            run = subprocess.run([args.client, 'game', MAP, '--data', args.data, '--server', '127.0.0.1',
                                  '--serverport', str(port), '--mpai', '--time', str(seconds)],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                                 env=env, timeout=240)
        finally:
            server.kill()
            server.wait()
    (root / 'client.log').write_text(run.stdout)
    return run.stdout, slog_path.read_text(errors='replace')


def done(log):
    m = DONE.search(log)
    assert m, 'no completion line:\n' + log[-3000:]
    return int(m.group(1)), m.group(3), m.group(4), int(m.group(5)) if m.group(5) else None


def case_seat(args, root):
    log, slog = play(args, root, 300000, 6, {})
    tick, err, end, _ = done(log)
    assert err == 'none', 'the seat was lost at tick %d (err=%s); server:\n%s' % (tick, err, slog[-2000:])
    assert end == 'timelimit' and tick == 180, 'ended at tick %d (%s), not the 180-tick time limit' % (tick, end)
    return 'kept the seat to tick %d at 300 ms per tick' % tick


def case_result(args, root):
    log, _ = play(args, root, 150000, 20, {'TAK_DEFEAT_TEST': '2'})
    tick, err, end, result = done(log)
    assert err == 'none', 'err=%s at tick %d' % (err, tick)
    assert end == 'concluded', 'the self-destroyed player never lost (ended %s at tick %d)' % (end, tick)
    assert result is not None, 'no result tick reported; ended at tick %d' % tick
    assert tick == result, 'reported end tick %d, but the result landed at tick %d' % (tick, result)
    return 'ended at tick %d, the result tick' % tick


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', required=True)
    ap.add_argument('--client', required=True)
    ap.add_argument('--data', required=True)
    ap.add_argument('--case', action='append')
    args = ap.parse_args()
    cases = {'seat': case_seat, 'result': case_result}
    names = args.case or list(cases)
    base = Path(tempfile.mkdtemp(prefix='tak-harness-seat-'))
    failed = False
    try:
        with concurrent.futures.ThreadPoolExecutor(len(names)) as pool:
            jobs = {}
            for name in names:
                (base / name).mkdir()
                jobs[name] = pool.submit(cases[name], args, base / name)
            for name, job in jobs.items():
                try:
                    print('PASS %s: %s' % (name, job.result()), flush=True)
                except Exception as e:  # report every case, then fail
                    failed = True
                    print('FAIL %s: %s' % (name, e), flush=True)
    finally:
        if not failed:
            shutil.rmtree(base, ignore_errors=True)
        else:
            print('logs kept in', base)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
