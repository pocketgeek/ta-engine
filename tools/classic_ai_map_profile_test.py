#!/usr/bin/env python3
"""The Classic AI reads its profile from the room's own data: game mod > map > base.

Retail's file choice (ai/<aiprofile>.txt, else ai/default.txt) is looked up in the
room's layers, for ai/ files: a game mod's file wins over the map package's, which
wins over the base install's. Headless --mpai games on Inner Circle, seed 1, the AI
seat on Classic (shown to players as RETAIL):

  control   the base install's ai/default.txt; the AI builds the watched units
  game mod  a game pack carrying ai/default.txt that starts `limit <unit> 0` for them
            (the profile's first writer wins, so the lines go first): never built
  map       the same file inside a single-map archive under Maps/ (Inner Circle's
            map under another name): it travels in the room's map package, and the
            watched units are never built
  both      that map archive, and a game pack whose ai/default.txt is the base
            install's: the game mod's file is used, and the watched units are built

The server log names the file and the layer it came from in each game. Last, a
campaign mission hosted with no settings plays its opponents on Classic over the
mission's own aiprofile file.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from server_public_test import server

SECONDS = '120'
MAP = 'AI Circle'


def hpi_read(hpitool, data, path):
    where = subprocess.run([str(hpitool), 'where', str(data), path], capture_output=True, text=True, check=True).stdout
    archive, entry = re.search(r'-> (.+?)!(.+)$', where.strip()).groups()
    return subprocess.run([str(hpitool), 'cat', str(data / archive), entry], capture_output=True, check=True).stdout


def install(root, data, name):
    target = root / name
    target.mkdir()
    for f in data.resolve().iterdir():
        if f.name.lower() in ('overrides', 'mods', 'modcache', 'overridecache', 'mapcache', 'maps'):
            continue
        (target / f.name).symlink_to(f, target_is_directory=f.is_dir())
    maps = target / 'Maps'
    maps.mkdir()
    for f in data.resolve().iterdir():
        if f.name.lower() == 'maps' and f.is_dir():
            for m in f.iterdir():
                (maps / m.name).symlink_to(m)
    return target


def play(a, data, work, mission=None, settings=''):
    work.mkdir()
    roster = work / 'roster.jsonl'
    os.environ['TAK_ROSTER_DUMP'] = str(roster)   # the server reads it at its first sample
    os.environ['TAK_ROSTER_EVERY'] = '150'
    try:
        with server(a.server.resolve(), data, work, '--no-auth', '--seed', '1') as (port, _, process):
            settings_file = work / 'settings.ini'
            settings_file.write_text(settings)
            env = dict(os.environ, HOME=str(work), XDG_DATA_HOME=str(work), XDG_CONFIG_HOME=str(work),
                       TAK_SETTINGS_FILE=str(settings_file), TAK_HEADLESS='1', TAK_SPEED='80',
                       SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
            env.pop('TAK_ROSTER_DUMP')
            env.pop('TAK_AI_LEVEL', None)
            if mission:
                mode = [mission, '--mpmission', mission, '--time', '20']
            else:
                env['TAK_AI_LEVEL'] = '6'
                mode = [a.map, '--mpai', '--time', SECONDS]
            result = subprocess.run([str(a.client.resolve()), 'game', mode[0], '--data', str(data),
                                     '--server', '127.0.0.1', '--serverport', str(port), *mode[1:]],
                                    env=env, capture_output=True, text=True, timeout=600)
            log = (work / 'log').read_text()
            assert result.returncode == 0 and 'hash=' in result.stdout + result.stderr, \
                result.stdout + result.stderr + '\n' + log
            assert process.poll() is None, log
    finally:
        os.environ.pop('TAK_ROSTER_DUMP', None)
        os.environ.pop('TAK_ROSTER_EVERY', None)
    built, initial, levels = set(), None, {}
    if roster.exists():
        for line in roster.read_text().splitlines():
            row = json.loads(line)
            if row['kind'] == 'meta':
                levels = {p['p']: p['level'] for p in row['players']}
                continue
            types = {u[2] for u in row['units'] if levels.get(u[1]) == 6}
            if initial is None:
                initial = types
            built |= types
    return built - (initial or set()), levels, log


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--server', type=Path, required=True)
    p.add_argument('--client', type=Path, required=True)
    p.add_argument('--hpitool', type=Path, required=True)
    p.add_argument('--data', type=Path, required=True)
    a = p.parse_args()
    a.data = a.data.resolve()
    with tempfile.TemporaryDirectory(prefix='tak-classic-map-') as tmp:
        root = Path(tmp)
        base_profile = hpi_read(a.hpitool, a.data, 'ai/default.txt')

        a.map = 'Inner Circle'
        control, levels, log = play(a, install(root, a.data, 'control-data'), root / 'control')
        ai = [v for v in levels.values() if v >= 0]
        assert ai and all(v == 6 for v in ai), levels
        assert re.search(r'DEFAULT AI profile ai/default\.txt from the base data', log, re.I), log
        watched = sorted(control)[:2]
        assert len(watched) == 2, 'the control Classic AI built too little: %s' % sorted(control)
        profile = ''.join('limit %s 0\n' % u for u in watched).encode() + base_profile

        data = install(root, a.data, 'mod-data')
        pack = data / 'mods' / 'game' / 'my_ai' / 'ai'
        pack.mkdir(parents=True)
        (pack / 'default.txt').write_bytes(profile)
        modded, _, log = play(a, data, root / 'mod', settings='mpRoom.gamePack = ' + 'my_ai'.encode().hex() + '\n')
        assert re.search(r'DEFAULT AI profile ai/default\.txt from the game mods', log, re.I), log
        banned = [u for u in watched if u in modded]
        assert not banned, 'the Classic AI built %s despite the game mod\'s ai/default.txt' % banned

        data = install(root, a.data, 'map-data')
        kmp = root / 'kmp'
        (kmp / 'kmap').mkdir(parents=True)
        (kmp / 'ai').mkdir()
        (kmp / 'ai' / 'default.txt').write_bytes(profile)
        for ext in ('.tnt', '.ota'):
            (kmp / 'kmap' / (MAP + ext)).write_bytes(hpi_read(a.hpitool, a.data, 'maps/Inner Circle' + ext))
        subprocess.run([str(a.hpitool), 'pack', str(kmp), str(data / 'Maps' / (MAP + '.kmp'))],
                       check=True, capture_output=True)
        a.map = MAP
        mapped, _, log = play(a, data, root / 'map')
        assert re.search(r'DEFAULT AI profile ai/default\.txt from the map package', log, re.I), log
        banned = [u for u in watched if u in mapped]
        assert not banned, 'the Classic AI built %s despite the map\'s ai/default.txt' % banned

        both_pack = data / 'mods' / 'game' / 'my_ai' / 'ai'
        both_pack.mkdir(parents=True)
        (both_pack / 'default.txt').write_bytes(base_profile)
        both, _, log = play(a, data, root / 'both', settings='mpRoom.gamePack = ' + 'my_ai'.encode().hex() + '\n')
        assert re.search(r'DEFAULT AI profile ai/default\.txt from the game mods', log, re.I), log
        assert any(u in both for u in watched), \
            'the Classic AI built none of %s: the map\'s ai/default.txt won over the game mod\'s (built: %s)' % (
                watched, sorted(both))

        _, _, log = play(a, install(root, a.data, 'mission-data'), root / 'mission', mission='takmission08_dh')
        assert re.search(r"mission 'takmission08_dh' -- \d+ strategic AI player\(s\) profile=\S+ DEFAULT AI \(ai/\S+\.txt\)",
                         log, re.I), log
        print('PASS: Classic reads the room\'s ai/default.txt (base, game mod, map package; %s never built '
              'from the mod or map file; a game mod\'s file wins over the map\'s); a campaign mission plays Classic '
              'with no settings' % ', '.join(watched))


if __name__ == '__main__':
    main()
