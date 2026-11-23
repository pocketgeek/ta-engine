#!/usr/bin/env python3
"""The lobby's map previews never outlive the data set they read.

A generated map's preview is rolled on a worker, and the full-window map view decodes
terrain on its own workers; all of them read the client's data set (GameView::vfs_).
startMpGame remounts that data set for the room's mods, and GameView's destructor destroys
it. The debug probe TAK_PREVIEW_LIFETIME_TEST puts a read gate on the data set, holds the
preview readers at a read, lets them go one at a time from two seconds on (the last held
first, so waiting for anything less than all of them leaves one held), and ends the run with
FAIL if the data set goes while one is still in it (it would read freed memory):

  gen       three rolls (two of them superseded) held as the game starts
  big       the full-window view of a generated map held as the game starts (a ready guest
            can have it open when the host starts)
  teardown  both held as the view is destroyed at the end of the game

Each game must then play normally. One server, the three clients side by side. Debug builds
only (the harness flags and the probe).
"""
import argparse
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

MAP = 'Inner Circle'
MODES = ('gen', 'big', 'teardown')


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


def check(mode, code, log):
    """The probe's verdict, then a normal game."""
    if 'FAIL: preview lifetime' in log:
        raise AssertionError(mode + ': ' + re.search(r'FAIL: preview lifetime.*', log).group(0))
    held = re.search(r'preview lifetime \(%s\): (\d+) preview reads held' % mode, log)
    if not held:
        raise AssertionError(mode + ': the probe never held its readers:\n' + log[-3000:])
    passed = re.search(r'PASS: preview lifetime \(%s\).*' % mode, log)
    if not passed:
        raise AssertionError(mode + ': the data set never went (no verdict):\n' + log[-3000:])
    done = re.search(r'mp-headless done: tick=(\d+) hash=\w+ units=\d+ err=(.*?) end=', log)
    if not done or int(done.group(1)) <= 0 or done.group(2) != 'none':
        raise AssertionError(mode + ': the game did not play normally:\n' + log[-3000:])
    if mode != 'teardown' and passed.start() > done.start():
        raise AssertionError(mode + ': the data set went after the game, not at its start')
    if code != 0:
        raise AssertionError('%s: exit %d:\n%s' % (mode, code, log[-3000:]))
    print('%s: %s held; %s; game to tick %s' % (mode, held.group(1), passed.group(0), done.group(1)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', required=True)
    ap.add_argument('--client', required=True)
    ap.add_argument('--data', required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        port = free_port()
        slog = open(root / 'server.log', 'w')
        server = subprocess.Popen([args.server, '--port', str(port), '--data', args.data, '--local', '--seed', '1',
                                   '--no-auth'], stdout=slog, stderr=subprocess.STDOUT, env=environment(root, 'server'))
        clients = {}
        try:
            deadline = time.monotonic() + 90
            while 'listening' not in (root / 'server.log').read_text(errors='replace'):
                if time.monotonic() > deadline or server.poll() is not None:
                    raise AssertionError('server never listened:\n' + (root / 'server.log').read_text(errors='replace')[-3000:])
                time.sleep(0.1)
            for mode in MODES:
                env = environment(root, mode)
                env.update(TAK_PREVIEW_LIFETIME_TEST=mode, TAK_SPEED='80')
                log = open(root / (mode + '.log'), 'w')
                clients[mode] = (subprocess.Popen(
                    [args.client, 'game', MAP, '--data', args.data, '--server', '127.0.0.1', '--serverport', str(port),
                     '--mpai', '--time', '3'], stdout=log, stderr=subprocess.STDOUT, env=env), log)
            failed = []
            for mode, (client, log) in clients.items():
                try:
                    try:
                        code = client.wait(150)
                    except subprocess.TimeoutExpired:
                        client.kill()
                        client.wait()
                        raise AssertionError(mode + ': the client is still running after 150 s:\n' +
                                             (root / (mode + '.log')).read_text(errors='replace')[-3000:])
                    log.close()
                    check(mode, code, (root / (mode + '.log')).read_text(errors='replace'))
                except AssertionError as e:
                    print('FAIL:', e)
                    failed.append(mode)
            if failed:
                print('server log:\n' + (root / 'server.log').read_text(errors='replace')[-3000:])
                return 1
            print('PASS: no preview reader outlived the data set it read; every game then played')
        except AssertionError as e:
            print('FAIL:', e)
            return 1
        finally:
            for client, log in clients.values():
                if client.poll() is None:
                    client.kill()
                    client.wait()
                log.close()
            server.kill()
            server.wait()
            slog.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
