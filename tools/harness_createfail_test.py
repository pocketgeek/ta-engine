#!/usr/bin/env python3
"""A headless --mphost run whose map cannot be built fails fast instead of spinning.

createGame() checks the map locally before asking the server for a room; a map that does not
build used to leave the harness calling it every frame until its outer timeout. Now the run
ends with a clear message and a nonzero exit. Debug builds only (the harness flags).
"""
import argparse
import os
import re
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

LIMIT = 60   # seconds; a healthy failure takes a few (server mount + connect)


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def environment(root, name):
    env = {k: v for k, v in os.environ.items() if not k.startswith('TAK_')}
    for sub in ('data', 'config', 'home'):
        (root / name / sub).mkdir(parents=True, exist_ok=True)
    env.update(XDG_DATA_HOME=str(root / name / 'data'), XDG_CONFIG_HOME=str(root / name / 'config'),
               HOME=str(root / name / 'home'), TAK_HEADLESS='1', SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    return env


def hpi_read(hpitool, data, path):
    where = subprocess.run([hpitool, 'where', str(data), path], capture_output=True, text=True, check=True).stdout
    archive, entry = re.search(r'-> (.+?)!(.+)$', where.strip()).groups()
    return subprocess.run([hpitool, 'cat', str(data / archive), entry], capture_output=True, check=True).stdout


def broken_install(root, data, hpitool):
    """The retail install plus one extra map package whose terrain is garbage: it is found by
    name, then fails to build."""
    install = root / 'install'
    install.mkdir()
    for entry in Path(data).iterdir():
        if entry.name.lower() != 'maps':
            os.symlink(entry.resolve(), install / entry.name)
    maps = install / 'Maps'
    maps.mkdir()
    for entry in (Path(data) / 'Maps').iterdir():
        os.symlink(entry.resolve(), maps / entry.name)
    src = root / 'broken-src'
    (src / 'kmap').mkdir(parents=True)
    # Inner Circle with its first terrain tile pointed at art that does not exist: the map parses,
    # so it is found by name, but the package cannot be built ("missing art is an error").
    tnt = bytearray(hpi_read(hpitool, Path(data), 'maps/Inner Circle.tnt'))
    keys = int.from_bytes(tnt[32:36], 'little')   # the tile-key table's offset
    tnt[keys:keys + 4] = b'\xef\xbe\xad\xde'
    (src / 'kmap' / 'Broken Map.tnt').write_bytes(bytes(tnt))
    (src / 'kmap' / 'Broken Map.ota').write_bytes(hpi_read(hpitool, Path(data), 'maps/Inner Circle.ota'))
    subprocess.run([hpitool, 'pack', str(src), str(maps / 'Broken Map.kmp')], check=True)
    return str(install)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', required=True)
    ap.add_argument('--client', required=True)
    ap.add_argument('--data', required=True)
    ap.add_argument('--hpitool', required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        data = broken_install(root, args.data, args.hpitool)
        port = free_port()
        slog = open(root / 'server.log', 'w')
        server = subprocess.Popen([args.server, '--port', str(port), '--data', data, '--local', '--seed', '1', '--no-auth'],
                                  stdout=slog, stderr=subprocess.STDOUT, env=environment(root, 'server'))
        try:
            deadline = time.monotonic() + 90
            while 'listening' not in (root / 'server.log').read_text(errors='replace'):
                if time.monotonic() > deadline or server.poll() is not None:
                    raise AssertionError('server never listened:\n' + (root / 'server.log').read_text(errors='replace')[-3000:])
                time.sleep(0.1)
            t0 = time.monotonic()
            # --time 600: a spinning harness would run (or sit in the lobby) far past LIMIT.
            run = subprocess.run([args.client, 'game', 'Broken Map', '--data', data, '--server', '127.0.0.1',
                                  '--serverport', str(port), '--mphost', '--time', '600'],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                                 env=environment(root, 'client'), timeout=LIMIT)
            took = time.monotonic() - t0
            assert run.returncode != 0, 'a failed createGame exited 0:\n' + run.stdout[-3000:]
            assert 'create game failed' in run.stdout, 'no clear message:\n' + run.stdout[-3000:]
            print('PASS: failed createGame ended the harness in %.1f s (exit %d)' % (took, run.returncode))
        except subprocess.TimeoutExpired as e:
            out = e.stdout.decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
            print(out[-3000:])
            print('FAIL: the harness is still running %d s after a createGame that cannot succeed' % LIMIT)
            return 1
        except AssertionError as e:
            print('FAIL:', e)
            return 1
        finally:
            server.kill()
            server.wait()
            slog.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
