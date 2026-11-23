#!/usr/bin/env python3
"""A map's AI profiles, made into a .kmp by the Cartographer, play in a real game.

Headless Cartographer --bundle writes two maps from Inner Circle's terrain, each with
the DEFAULT AI profile named by its .ota (aiprofile=e2e, so ai/e2e.txt) and the file
all our levels read, ai/takengine.txt, both in the editor's canonical form and kept at
the .kmp root. The
profiles put a limit on every side's factories (category FACTORY): 0 in the first map,
2 in the second. Both maps sit in one scratch Maps/ folder, so their ai/e2e.txt and
ai/takengine.txt collide. Headless --mpai games (seed 1, the AI seat on DEFAULT, 60 s):

  stock    Inner Circle, with the two maps installed and an unticked game pack: the
           same state hash as in a plain install, and no factory limit in the AI's
           weight dump (TAK_CLASSIC_WEIGHTS)
  alpha    the first map: the server log names ai/e2e.txt and ai/takengine.txt from the map
           package, and the dump shows every factory at limit 0
  beta     the second map: its own files, not the first map's (limit 2)
  mod      the first map with a ticked game pack carrying ai/e2e.txt (limit 1) and
           ai/takengine.txt: the log names both from the game mods, and the dump shows 1
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from server_public_test import server

SECONDS = '60'
PROFILE = 'e2e'
PACK = 'e2e_ai'
MAGIC = '// TAK Cartographer AI profile 1\n'


def hpi_read(hpitool, data, path):
    where = subprocess.run([str(hpitool), 'where', str(data), path], capture_output=True, text=True, check=True).stdout
    archive, entry = re.search(r'-> (.+?)!(.+)$', where.strip()).groups()
    return subprocess.run([str(hpitool), 'cat', str(data / archive), entry], capture_output=True, check=True).stdout


def factories(hpitool, data):
    """FBI unit names whose category has FACTORY, from the install's root archives."""
    names = set()
    for archive in sorted(data.iterdir()):
        if archive.suffix.lower() not in ('.hpi', '.ufo') or not archive.is_file():
            continue
        listing = subprocess.run([str(hpitool), 'list', str(archive)], capture_output=True, text=True).stdout
        for entry in re.findall(r'^\s*\d+\s+(units/[^/\s]+\.fbi)\s*$', listing, re.I | re.M):
            fbi = subprocess.run([str(hpitool), 'cat', str(archive), entry], capture_output=True).stdout
            fbi = fbi.decode('latin-1')
            category = re.search(r'^\s*category\s*=\s*([^;]*);', fbi, re.I | re.M)
            unit = re.search(r'^\s*unitname\s*=\s*([^;\s]*)\s*;', fbi, re.I | re.M)
            if category and unit and 'FACTORY' in category[1].upper().split():
                names.add(unit[1].lower())
    return sorted(names)


def install(root, data, name):
    """A scratch data root linking the install's archives, with its own Maps/ and mods/."""
    target = root / name
    target.mkdir()
    for f in data.iterdir():
        if f.name.lower() in ('overrides', 'mods', 'modcache', 'overridecache', 'mapcache', 'maps'):
            continue
        (target / f.name).symlink_to(f, target_is_directory=f.is_dir())
    maps = target / 'Maps'
    maps.mkdir()
    for f in data.iterdir():
        if f.name.lower() == 'maps' and f.is_dir():
            for m in f.iterdir():
                (maps / m.name).symlink_to(m)
    return target


def profile(kind, units, limit):
    note = ('// Units not listed are built at full chance with no limit.\n' if kind == 'default'
            else '// Units not listed keep the built-in values.\n')
    return (MAGIC + note + ''.join('limit %s %d\n' % (u, limit) for u in units)).encode()


def bundle(a, data, root, name, units, limit):
    """Cartographer opens a source map by name (its own archive's ai/ files) and --bundles it."""
    author = install(root, data, 'author-' + name.replace(' ', '-'))
    kit = root / ('kit-' + name.replace(' ', '-'))
    (kit / 'kmap').mkdir(parents=True)
    (kit / 'ai').mkdir()
    ota = hpi_read(a.hpitool, data, 'maps/Inner Circle.ota').decode('latin-1')
    ota, n = re.subn(r'aiprofile\s*=\s*[^;]*;', 'aiprofile=%s;' % PROFILE, ota, flags=re.I)
    assert n == 1, ota
    (kit / 'kmap' / 'E2E Source.ota').write_bytes(ota.encode('latin-1'))
    (kit / 'kmap' / 'E2E Source.tnt').write_bytes(hpi_read(a.hpitool, data, 'maps/Inner Circle.tnt'))
    (kit / 'ai' / (PROFILE + '.txt')).write_bytes(profile('default', units, limit))
    (kit / 'ai' / 'takengine.txt').write_bytes(profile('level', units, limit))
    subprocess.run([str(a.hpitool), 'pack', str(kit), str(author / 'Maps' / 'E2E Source.kmp')],
                   check=True, capture_output=True)
    home = root / ('home-' + name.replace(' ', '-'))
    home.mkdir()
    out = root / (name + '.kmp')
    env = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(home), XDG_CONFIG_HOME=str(home),
               TAK_CARTOGRAPHER_PROFILE=str(home / 'cartographer'), TAK_SETTINGS_FILE=str(home / 'settings.ini'),
               SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    result = subprocess.run([str(a.cartographer), 'E2E Source', '--data', str(author), '--bundle', str(out)],
                            env=env, capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, result.stdout + result.stderr
    listing = subprocess.run([str(a.hpitool), 'list', str(out)], capture_output=True, text=True, check=True).stdout
    entries = {e.lower() for e in re.findall(r'^\s*\d+\s+(\S.*?)\s*$', listing, re.M)}
    for want in ('ai/%s.txt' % PROFILE, 'ai/takengine.txt', 'kmap/%s/%s.tnt' % (name.lower(), name.lower()),
                 'kmap/%s/%s.ota' % (name.lower(), name.lower())):
        assert want in entries, '%s not in %s: %s' % (want, out, sorted(entries))
    assert not any(e.startswith('kmap/') and '/ai/' in e for e in entries), sorted(entries)
    for path, want in (('ai/%s.txt' % PROFILE, profile('default', units, limit)), ('ai/takengine.txt', profile('level', units, limit))):
        got = subprocess.run([str(a.hpitool), 'cat', str(out), path], capture_output=True, check=True).stdout
        assert got == want, '%s in %s changed: %r' % (path, out, got)
    ota = subprocess.run([str(a.hpitool), 'cat', str(out), 'kmap/%s/%s.ota' % (name, name)],
                         capture_output=True, check=True).stdout.decode('latin-1')
    assert re.search(r'aiprofile\s*=\s*%s\s*;' % PROFILE, ota, re.I), ota
    return out


def play(a, data, work, map_name, settings=''):
    work.mkdir()
    dump = work / 'weights'
    os.environ['TAK_CLASSIC_WEIGHTS'] = str(dump)   # the server's DEFAULT brains write it at tick 600
    try:
        with server(a.server.resolve(), data, work, '--no-auth', '--seed', '1') as (port, _, process):
            settings_file = work / 'settings.ini'
            settings_file.write_text(settings)
            env = dict(os.environ, HOME=str(work), XDG_DATA_HOME=str(work), XDG_CONFIG_HOME=str(work),
                       TAK_SETTINGS_FILE=str(settings_file), TAK_HEADLESS='1', TAK_SPEED='80',
                       SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', TAK_AI_LEVEL='6')
            env.pop('TAK_CLASSIC_WEIGHTS')
            result = subprocess.run([str(a.client), 'game', map_name, '--data', str(data), '--server', '127.0.0.1',
                                     '--serverport', str(port), '--mpai', '--time', SECONDS],
                                    env=env, capture_output=True, text=True, timeout=600)
            log = (work / 'log').read_text()
            out = result.stdout + result.stderr
            assert result.returncode == 0 and 'hash=' in out, out + '\n' + log
            assert process.poll() is None, log
    finally:
        os.environ.pop('TAK_CLASSIC_WEIGHTS', None)
    limits = {}
    dumps = sorted(work.glob('weights.p*'))
    assert dumps, 'no DEFAULT AI weight dump in %s:\n%s' % (work, log)
    for path in dumps:
        for limit, unit in re.findall(r"^\s*(n/a|-?\d+)\s+-\s+-?\d+\s+=\s+-?\d+\s+-\s+'(\S+)", path.read_text(), re.M):
            limits.setdefault(unit.lower(), set()).add(limit)
    return re.findall(r'hash=([0-9a-f]+)', out)[-1], limits, log


def expect_limits(limits, units, want, what):
    for u in units:
        assert limits.get(u) == {want}, '%s: %s has limit %s in the DEFAULT AI dump, not %s' % (
            what, u, sorted(limits.get(u, {'(missing)'})), want)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--server', type=Path, required=True)
    p.add_argument('--client', type=Path, required=True)
    p.add_argument('--cartographer', type=Path, required=True)
    p.add_argument('--hpitool', type=Path, required=True)
    p.add_argument('--data', type=Path, required=True)
    a = p.parse_args()
    a.data = a.data.resolve()
    for tool in ('server', 'client', 'cartographer', 'hpitool'):
        setattr(a, tool, getattr(a, tool).resolve())
    with tempfile.TemporaryDirectory(prefix='tak-carto-ai-e2e-') as tmp:
        root = Path(tmp)
        units = factories(a.hpitool, a.data)
        assert len(units) >= 4, 'too few factories found: %s' % units
        alpha = bundle(a, a.data, root, 'E2E Alpha', units, 0)
        beta = bundle(a, a.data, root, 'E2E Beta', units, 2)

        plain_hash, _, _ = play(a, install(root, a.data, 'plain-data'), root / 'plain', 'Inner Circle')

        data = install(root, a.data, 'play-data')
        shutil.copy(alpha, data / 'Maps' / alpha.name)
        shutil.copy(beta, data / 'Maps' / beta.name)
        pack = data / 'mods' / 'game' / PACK / 'ai'
        pack.mkdir(parents=True)
        (pack / (PROFILE + '.txt')).write_bytes(profile('default', units, 1))
        (pack / 'takengine.txt').write_bytes(profile('level', units, 1))

        stock_hash, limits, log = play(a, data, root / 'stock', 'Inner Circle')
        assert stock_hash == plain_hash, 'Inner Circle hash %s with the AI maps installed, %s without' % (
            stock_hash, plain_hash)
        assert re.search(r'DEFAULT AI profile ai/default\.txt from the base data', log, re.I), log
        assert not re.search(r'our AI levels play their built-in profiles adjusted', log), log
        assert all(limits.get(u) != {'0'} for u in units), 'stock map: a factory is limited to 0'

        for name, limit in (('E2E Alpha', 0), ('E2E Beta', 2)):
            _, limits, log = play(a, data, root / name.replace(' ', '-'), name)
            assert re.search(r'DEFAULT AI profile ai/%s\.txt from the map package' % PROFILE, log, re.I), log
            assert re.search(r"our AI levels play their built-in profiles adjusted by the map package's ai/takengine\.txt",
                             log, re.I), log
            expect_limits(limits, units, str(limit), name)

        _, limits, log = play(a, data, root / 'mod', 'E2E Alpha',
                              settings='mpRoom.gamePack = ' + PACK.encode().hex() + '\n')
        assert re.search(r'DEFAULT AI profile ai/%s\.txt from the game mods' % PROFILE, log, re.I), log
        assert re.search(r"our AI levels play their built-in profiles adjusted by the game mods' ai/takengine\.txt",
                         log, re.I), log
        expect_limits(limits, units, '1', 'E2E Alpha with the ticked pack')

        print('PASS: Cartographer --bundle carries ai/%s.txt and ai/takengine.txt at the .kmp root; each of two '
              'colliding maps plays its own files (limit 0 / 2 on %d factories), a ticked game pack\'s files '
              'win (limit 1), and the stock map hash %s is unchanged' % (PROFILE, len(units), stock_hash))


if __name__ == '__main__':
    main()
