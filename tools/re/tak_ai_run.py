#!/usr/bin/env python3
"""Run one headless tak --mpai game with the debug roster dump (AI comparison, our side).

  python3 tools/re/tak_ai_run.py --out ~/tak-tmp/retail-ai/tak/run1.jsonl \
      [--build build-o2] [--data assets/game] [--map "Abnar's Terrace"] \
      [--side aramon] [--enemy taros] [--level 2] [--seconds 600] [--seed 1]

A fresh DEBUG takserver (--no-auth --local --seed S) runs with TAK_ROSTER_DUMP
pointing at --out, and a headless DEBUG takclient joins it with --mpai: the
client's seat stays idle and the server seats one AI (TAK_AI_LEVEL picks its
level). The JSONL has the same schema as tools/re/retail_ai_poll.py's, so
tools/re/compare_ai.py reads both. Prints the client's final line (it carries
the state hash=, which the dump must not change). Scratch XDG_DATA_HOME/HOME
directories keep the run away from the real profile; both processes run in
their own sessions and only those are signalled.
"""
import argparse
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parents[2]
SIDES = {'aramon': 'ara', 'taros': 'tar', 'veruna': 'ver', 'zhon': 'zon', 'creon': 'cre'}


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def stop(process):
    if process and process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(10)
        except (OSError, subprocess.TimeoutExpired):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except OSError:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, help='roster JSONL to write (omit for a dump-off run)')
    parser.add_argument('--build', type=Path, default=REPO / 'build-o2', help='a DEBUG build dir')
    parser.add_argument('--data', type=Path, default=REPO / 'assets/game')
    parser.add_argument('--map', default='Inner Circle')
    parser.add_argument('--side', choices=SIDES, default='aramon', help='the idle client seat')
    parser.add_argument('--enemy', choices=SIDES, default='taros', help='the AI seat')
    parser.add_argument('--level', type=int, help='TAK_AI_LEVEL (default: the client\'s default, Normal)')
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--every', type=int, default=30, help='TAK_ROSTER_EVERY ticks')
    parser.add_argument('--cores', default='4-23')
    parser.add_argument('--legion', action='store_true', help='TAK_LEGION=1 (Legion pathfinding)')
    args = parser.parse_args()
    scratch = Path(tempfile.mkdtemp(prefix='tak-ai-run-', dir=os.environ.get('TMPDIR')))
    env = dict(os.environ, XDG_DATA_HOME=str(scratch / 'xdg'), HOME=str(scratch / 'home'))
    (scratch / 'home').mkdir()
    if args.legion:
        env['TAK_LEGION'] = '1'
    server_env = dict(env)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.unlink(missing_ok=True)
        server_env.update(TAK_ROSTER_DUMP=str(args.out.resolve()), TAK_ROSTER_EVERY=str(args.every))
    pin = ['taskset', '-c', args.cores] if args.cores else []
    port = free_port()
    server_log = scratch / 'server.log'
    server = client = None
    try:
        with open(server_log, 'w') as out:
            server = subprocess.Popen([*pin, str(args.build / 'takserver'), '--port', str(port),
                                       '--data', str(args.data), '--no-auth', '--local',
                                       '--seed', str(args.seed)], env=server_env, stdout=out,
                                      stderr=subprocess.STDOUT, start_new_session=True)
        for _ in range(240):
            if 'listening' in server_log.read_text(errors='ignore') or server.poll() is not None:
                break
            time.sleep(0.5)
        if 'listening' not in server_log.read_text(errors='ignore'):
            sys.exit(f'takserver did not start; see {server_log}')
        client_env = dict(env, TAK_HEADLESS='1', SDL_VIDEODRIVER='dummy')
        if args.level is not None:
            client_env['TAK_AI_LEVEL'] = str(args.level)
        client = subprocess.Popen([*pin, str(args.build / 'takclient'), 'game', args.map, '--data',
                                   str(args.data), '--server', '127.0.0.1', '--serverport', str(port),
                                   '--side', SIDES[args.side], '--aiside', SIDES[args.enemy],
                                   '--mpai', '--time', str(args.seconds)], env=client_env,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                  start_new_session=True)
        output, _ = client.communicate(timeout=args.seconds * 6 + 300)
        done = [line for line in output.splitlines() if 'mp-headless done' in line]
        print(done[-1] if done else output[-2000:])
        return 0 if done and 'err=none' in done[-1] else 1
    finally:
        stop(client)
        stop(server)


if __name__ == '__main__':
    raise SystemExit(main())
