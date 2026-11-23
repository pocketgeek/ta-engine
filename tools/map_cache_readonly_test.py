#!/usr/bin/env python3
"""The game data folder is only read: map downloads, Test Map copies and saved maps
go to the per-user folder (hpi::userMapRoot), never the --data folder.

1. Network: a map present on the host only is sent to a guest through a takserver
   whose --data, like both players' data folders, has no write permission. The
   server runs without --map-cache-dir, so its default per-user folder is used.
2. --play-map: a debug takclient opens a one-map snapshot on a read-only data folder
   in its private lobby; the verified copy lands beside settings.ini, and the local
   server (--map-cache-dir = that folder) verifies the room's map from it.
Each data folder's tree (names, sizes, write times) must be unchanged afterwards.
"""
import argparse
import os
from pathlib import Path
import socket
import stat
import subprocess
import tempfile
import time

SKIP = ('maps', 'mods', 'overrides', 'mapcache', 'modcache', 'overridecache')


def install(root, data, name):
    """A data folder that links the install's root files, without its maps or caches."""
    target = root / name
    target.mkdir()
    for f in data.iterdir():
        if f.name.lower() in SKIP:
            continue
        (target / f.name).symlink_to(f, target_is_directory=f.is_dir())
    return target


def tree(root):
    out = {}
    for dirpath, dirs, files in os.walk(root):   # symlinks are not followed
        for n in dirs + files:
            p = Path(dirpath) / n
            st = p.lstat()
            out[str(p)] = (st.st_size, st.st_mtime_ns)
    return out


def read_only(path):
    path.chmod(path.stat().st_mode & ~(stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH))


def writable(root):
    for dirpath, dirs, _ in os.walk(root):
        for p in [Path(dirpath)] + [Path(dirpath) / d for d in dirs]:
            if not p.is_symlink():
                p.chmod(p.stat().st_mode | stat.S_IWUSR)


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def scratch_env(home, **extra):
    env = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(home / 'xdg'), XDG_CONFIG_HOME=str(home / 'config'))
    env.pop('TAK_SETTINGS_FILE', None)
    env.update(extra)
    return env


def network(a, root):
    host, peer, referee = (install(root, a.data, n) for n in ('host', 'peer', 'server'))
    read_only(referee)
    server_tree = tree(referee)
    home = root / 'server-home'
    home.mkdir()
    port = free_port()
    log_path = root / 'server.log'
    with log_path.open('w') as log:
        server = subprocess.Popen([str(a.server), '--local', '--no-auth', '--seed', '1', '--port', str(port),
                                   '--data', str(referee)], stdout=log, stderr=log, env=scratch_env(home))
    try:
        deadline = time.monotonic() + 60
        while 'listening on' not in log_path.read_text():
            assert server.poll() is None and time.monotonic() < deadline, log_path.read_text()
            time.sleep(.05)
        result = subprocess.run([str(a.map_tool), '--network', str(port), str(host), str(peer)],
                                capture_output=True, text=True, timeout=240,
                                env=scratch_env(root / 'client-home', TAK_MAP_TEST_READONLY='1'))
        print(result.stdout, end='')
        assert result.returncode == 0, result.stderr + '\n' + log_path.read_text()
        assert 'read-only data folders: unchanged' in result.stdout, result.stdout
        assert server.poll() is None, log_path.read_text()
        # (This harness's own world setup is not the referee's, so the server log
        # reports REFEREE SUSPECT here before and after this change; the players'
        # hashes are checked against each other by the harness.)
        assert 'map verified' in log_path.read_text(), log_path.read_text()
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
    cache = home / 'xdg' / 'TAKengine' / 'takserver' / 'MapCache'
    assert list(cache.glob('*.takmap')), 'server default map cache not used: %s' % cache
    assert tree(referee) == server_tree, 'the server wrote into its --data folder'
    print('network: host-only map sent through a read-only server, cached in its per-user folder')


def play_map(a, root):
    data = install(root, a.data, 'player')
    snapshot = root / 'snapshot' / 'Readonly Test.kmp'
    subprocess.run([str(a.map_tool), '--export-kmp', str(a.data), str(snapshot), 'Ulasem Arena'], check=True)
    read_only(data)
    before = tree(data)
    home = root / 'player-home'
    profile = home / 'profile'
    profile.mkdir(parents=True)
    env = scratch_env(home, TAK_SETTINGS_FILE=str(profile / 'settings.ini'), TAK_HEADLESS='1',
                      SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    # --play-map stops in the private lobby, where the local server has verified the
    # copy it read from the shared per-user folder; SIGTERM ends both processes.
    log_path = root / 'player.log'
    with log_path.open('w') as log:
        client = subprocess.Popen([str(a.client), '--play-map', str(snapshot), '--data', str(data)],
                                  stdout=log, stderr=subprocess.STDOUT, env=env)
    try:
        deadline = time.monotonic() + 180
        while 'map verified' not in log_path.read_text():
            assert client.poll() is None and time.monotonic() < deadline, log_path.read_text()
            time.sleep(.1)
    finally:
        client.terminate()
        try:
            client.wait(timeout=20)
        except subprocess.TimeoutExpired:
            client.kill()
            client.wait()
    out = log_path.read_text()
    assert 'Cannot test map' not in out and 'SAVE FAILED' not in out, out
    # A debug client keeps its profile beside TAK_SETTINGS_FILE; a release client reads
    # no TAK_* variable, so its profile is the per-user folder under XDG_DATA_HOME.
    caches = [profile / 'MapCache', home / 'xdg' / 'TAKengine' / 'TAKingdoms' / 'MapCache']
    found = [c for c in caches if list(c.glob('*.kmp')) and list(c.glob('*.takmap'))]
    assert found, 'no Test Map copy in the player\'s profile (%s): %s' % (', '.join(map(str, caches)), out)
    assert tree(data) == before, 'takclient --play-map wrote into its --data folder'
    assert not (home / 'xdg' / 'TAKengine' / 'takserver').exists(), 'the private server did not share the per-user folder'
    print('play-map: snapshot opened and verified on a read-only data folder, copy in the per-user folder')


def main():
    p = argparse.ArgumentParser()
    for name in ('--server', '--client', '--map-tool', '--data'):
        p.add_argument(name, type=Path, required=True)
    # A Release takclient on Windows or macOS reads only the player's real profile
    # (util/prefpath.h), so its --play-map half cannot be isolated: CMake skips it there.
    p.add_argument('--no-play-map', action='store_true')
    a = p.parse_args()
    a.server, a.client, a.map_tool, a.data = (x.resolve() for x in (a.server, a.client, a.map_tool, a.data))
    with tempfile.TemporaryDirectory(prefix='tak-map-ro-') as tmp:
        root = Path(tmp)
        try:
            network(a, root)
            if a.no_play_map:
                print('play-map: skipped (a Release client here would use the real profile)')
            else:
                play_map(a, root)
        finally:
            writable(root)


if __name__ == '__main__':
    main()
