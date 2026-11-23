#!/usr/bin/env python3
"""Our levels' ai/takengine.txt: a game mod's (through the real mod transfer) or the
map's (in its .kmp, through the room's map package) reaches the server, and every one
of our levels reads it, each over its own built-in table.

Headless all-AI games on AI Circle (Inner Circle's map in a single-map archive under
Maps/), seed 1, a spectating host: seat 0 Aramon on Easy, seat 1 Taros on Normal.
Every game ticks a game pack, uploaded with the room's game mods and applied by the
server (the only place the AI runs). Misplaced takengine.txt files say `limit <unit> 0`
for both seats' watched units in every game -- at the game pack's root, in a cosmetic
pack's ai/ and in a base install archive's ai/ -- and are never read; nor is an old
per-level ai/n.txt in the game pack's ai/ (or the map's) banning the Normal seat's.

  control  the map's ai/takengine.txt bans both seats' watched units, but the game
           pack's ai/takengine.txt (a comment only) wins over it: both AIs build them
  map      the game pack has no ai/takengine.txt; the map's bans both seats' watched
           units: neither the Easy nor the Normal AI builds them
  modded   the game pack's ai/takengine.txt bans both seats' watched units (the map's
           says nothing): the same
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from server_public_test import server

# Units each AI builds within two minutes in this game (seat -> units). With the
# 5000 mana a game opens with, the Normal seat starts on a Temple.
EASY_SEAT, NORMAL_SEAT = 0, 1
WATCHED = {EASY_SEAT: ('araat', 'aralode'), NORMAL_SEAT: ('tarhell', 'tarlode')}
SECONDS = '120'
MAP = 'AI Circle'
GAME_TIMEOUT = 240   # per game; ctest allows 900 s for all three


def ban(units):
    return ''.join('limit %s 0\n' % u for u in units)


def hpi_read(hpitool, data, path):
    where = subprocess.run([str(hpitool), 'where', str(data), path], capture_output=True, text=True, check=True).stdout
    archive, entry = re.search(r'-> (.+?)!(.+)$', where.strip()).groups()
    return subprocess.run([str(hpitool), 'cat', str(data / archive), entry], capture_output=True, check=True).stdout


def pack(hpitool, folder, out):
    subprocess.run([str(hpitool), 'pack', str(folder), str(out)], check=True, capture_output=True)


def install(a, root, name, mod_n, map_n):
    """A scratch install over the real one, with the game pack my_ai and the decoys.
    mod_n / map_n: the game pack's / the map archive's ai/takengine.txt (None = no such
    file)."""
    target = root / name
    target.mkdir()
    jersey = None
    for f in a.data.iterdir():
        low = f.name.lower()
        if low in ('overrides', 'mods', 'modcache', 'overridecache', 'mapcache', 'maps'):
            continue
        if low == 'jersey.hpi':   # a small sounds-only root archive: repacked below
            jersey = f
            continue
        (target / f.name).symlink_to(f, target_is_directory=f.is_dir())
    maps = target / 'Maps'
    maps.mkdir()
    for f in a.data.iterdir():
        if f.name.lower() == 'maps' and f.is_dir():
            for m in f.iterdir():
                (maps / m.name).symlink_to(m)
    decoy = ban(WATCHED[EASY_SEAT] + WATCHED[NORMAL_SEAT])
    old = ban(WATCHED[NORMAL_SEAT])
    # The base install: a root archive that also carries ai/takengine.txt.
    base = root / (name + '-jersey')
    subprocess.run([str(a.hpitool), 'extract', str(jersey), str(base)], check=True, capture_output=True)
    (base / 'ai').mkdir()
    (base / 'ai' / 'takengine.txt').write_text(decoy)
    pack(a.hpitool, base, target / jersey.name)
    # The map: Inner Circle under another name, its archive maybe carrying
    # ai/takengine.txt, and an old ai/n.txt that no longer travels.
    kmp = root / (name + '-kmp')
    (kmp / 'kmap').mkdir(parents=True)
    (kmp / 'ai').mkdir()
    (kmp / 'ai' / 'n.txt').write_text(old)
    if map_n is not None:
        (kmp / 'ai' / 'takengine.txt').write_text(map_n)
    for ext in ('.tnt', '.ota'):
        (kmp / 'kmap' / (MAP + ext)).write_bytes(hpi_read(a.hpitool, a.data, 'maps/Inner Circle' + ext))
    pack(a.hpitool, kmp, maps / (MAP + '.kmp'))
    # The game pack: only its ai/takengine.txt is read; a root takengine.txt is not,
    # nor the old per-level ai/n.txt.
    game = target / 'mods' / 'game' / 'my_ai'
    (game / 'ai').mkdir(parents=True)
    (game / 'ai' / 'n.txt').write_text(old)
    if mod_n is not None:
        (game / 'ai' / 'takengine.txt').write_text(mod_n)
    (game / 'takengine.txt').write_text(decoy)
    # A cosmetic pack's ai/ never reaches a game.
    skin = target / 'mods' / 'cosmetic' / 'skin' / 'ai'
    skin.mkdir(parents=True)
    (skin / 'takengine.txt').write_text(decoy)
    return target


def play(a, root, name, mod_n, map_n):
    data = install(a, root, name, mod_n, map_n)
    work = root / (name + '-run')
    work.mkdir()
    roster = work / 'roster.jsonl'
    os.environ['TAK_ROSTER_DUMP'] = str(roster)   # the server reads it at its first sample
    os.environ['TAK_ROSTER_EVERY'] = '150'
    try:
        with server(a.server.resolve(), data, work, '--no-auth', '--seed', '1') as (port, _, process):
            settings = work / 'settings.ini'
            settings.write_text('mpRoom.gamePack = ' + 'my_ai'.encode().hex() + '\n' +
                                'mods.cosmeticPack = ' + 'skin'.encode().hex() + '\n')
            env = dict(os.environ, HOME=str(work), XDG_DATA_HOME=str(work), XDG_CONFIG_HOME=str(work),
                       TAK_SETTINGS_FILE=str(settings), TAK_HEADLESS='1', TAK_SPEED='80',
                       TAK_MP_WATCH='1', TAK_MP_AIS='2', TAK_AI_LEVELS='1,2',
                       SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
            env.pop('TAK_ROSTER_DUMP')
            env.pop('TAK_AI_LEVEL', None)
            # One game takes about 20 s (56 s for all three alone, 75 s beside five more
            # copies and a CPU hog). Its own deadline sits well inside ctest's, so a stuck
            # game is reported with the client's output and the server log (0.8.11 and
            # 0.8.13 each hit ctest's 600 s limit, which kills the test silently).
            try:
                result = subprocess.run([str(a.client.resolve()), 'game', MAP, '--data', str(data),
                                         '--server', '127.0.0.1', '--serverport', str(port), '--mphost', '--time', SECONDS],
                                        env=env, capture_output=True, text=True, timeout=GAME_TIMEOUT)
            except subprocess.TimeoutExpired as e:
                text = lambda b: b.decode(errors='replace') if isinstance(b, bytes) else (b or '')
                raise AssertionError('the %s game did not finish in %d s\n--- client stdout\n%s\n--- client stderr\n%s\n'
                                     '--- server log\n%s' % (name, GAME_TIMEOUT, text(e.stdout), text(e.stderr),
                                                              (work / 'log').read_text()))
            log = (work / 'log').read_text()
            assert result.returncode == 0 and 'hash=' in result.stdout + result.stderr, \
                result.stdout + result.stderr + '\n' + log
            assert process.poll() is None, log
    finally:
        os.environ.pop('TAK_ROSTER_DUMP', None)
        os.environ.pop('TAK_ROSTER_EVERY', None)
    built, initial, levels = {}, {}, {}
    for line in roster.read_text().splitlines():
        row = json.loads(line)
        if row['kind'] == 'meta':
            levels = {p['p']: p['level'] for p in row['players']}
            continue
        seen = {}
        for u in row['units']:
            seen.setdefault(u[1], set()).add(u[2])
        for seat, types in seen.items():
            if seat not in initial:
                initial[seat] = types
            built.setdefault(seat, set()).update(types - initial[seat])
    assert levels.get(EASY_SEAT) == 1 and levels.get(NORMAL_SEAT) == 2, levels
    if a.verbose:
        print(name, {seat: sorted(types) for seat, types in built.items()})
    return built, log


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--server', type=Path, required=True)
    p.add_argument('--client', type=Path, required=True)
    p.add_argument('--hpitool', type=Path, required=True)
    p.add_argument('--data', type=Path, required=True)
    p.add_argument('--verbose', action='store_true')
    a = p.parse_args()
    a.data = a.data.resolve()
    from_mod = "our AI levels play their built-in profiles adjusted by the game mods' ai/takengine.txt"
    from_map = "our AI levels play their built-in profiles adjusted by the map package's ai/takengine.txt"
    adjusted = ' play their built-in profiles adjusted'
    nothing = '// a profile for our levels that changes nothing\n'
    both = ban(WATCHED[EASY_SEAT] + WATCHED[NORMAL_SEAT])

    def obeyed(built, name):
        for seat in (EASY_SEAT, NORMAL_SEAT):
            banned = [u for u in WATCHED[NORMAL_SEAT] + WATCHED[EASY_SEAT] if u in built.get(seat, set())]
            assert not banned, 'seat %d built %s despite the %s ai/takengine.txt' % (seat, banned, name)

    with tempfile.TemporaryDirectory(prefix='tak-ai-mod-') as tmp:
        root = Path(tmp)
        control, log = play(a, root, 'control', nothing, both)
        assert from_mod in log, log
        assert log.count(adjusted) == 1, log   # once for the room, not per level
        for seat, units in WATCHED.items():
            missing = [u for u in units if u not in control.get(seat, set())]
            assert not missing, 'the control game\'s seat %d never built %s (built: %s)' % (
                seat, missing, sorted(control.get(seat, set())))
        mapped, log = play(a, root, 'map', None, both)
        assert from_map in log, log
        assert log.count(adjusted) == 1, log
        obeyed(mapped, "map's")
        modded, log = play(a, root, 'modded', both, nothing)
        assert from_mod in log, log
        assert log.count(adjusted) == 1, log
        obeyed(modded, "mod's")
        print('PASS: ai/takengine.txt from the game mods or the map package reaches both the Easy and the Normal '
              'seat (neither built %s); a game mod\'s wins over the map\'s; root, cosmetic and base install '
              'takengine.txt and the old ai/n.txt are not read' % ', '.join(WATCHED[EASY_SEAT] + WATCHED[NORMAL_SEAT]))

if __name__ == '__main__':
    main()
