#!/usr/bin/env python3
"""Run retail TA:Kingdoms skirmishes headless and record the AI (comparison harness).

  # one-time: build the template Proton prefix (about a minute)
  python3 tools/re/retail_ai_harness.py setup

  # one game: idle human (seat 0) vs one retail AI, 10 game-minutes
  python3 tools/re/retail_ai_harness.py run --label det1 --side aramon --enemy taros \
      --minutes 10 [--seed 1234] [--aivai] [--map "Alchemist Glen"] [--printweights 600]

Everything at run time lives under --root (default ~/tak-tmp/retail-ai): a
template prefix, and per run a reflink copy of the game, a copy of the prefix,
a private kwin_wayland --virtual display, logs and the JSONL capture. The
user's own install and Faugus prefixes are never touched: the game is copied
from --game (default assets/game, the v4.0BB build every RE address is
verified against). Teardown kills only the PIDs this run started: the
display-holder by the PID it recorded, the kwin it launched, GDB, and every
process whose environment names this run's prefix or its private
XDG_RUNTIME_DIR (kwin, Xwayland, umu, Proton and the game all carry one).

How a run works (details in tools/re/README-retail-ai.md):
 1. kwin_wayland --virtual --xwayland gives an off-screen X display.
 2. umu-run + Proton runs Kingdoms.exe -AutoStop=999999, which stops pacing
    ticks to real time (about 140 ticks/s at speed 0, 280 at +10).
 3. The Win32 input bridge (retail_driver.py) clicks into the skirmish battle
    room, optionally finds --map in the list, and starts. Sides come from the
    registry (Skirmish GameOptions Side/EnemySide), written before launch.
 4. GDB, attached in the battle room, stops the game on the write of tick
    --poke-tick (a hardware watchpoint on the tick counter). It clears
    -AutoStop's cached argument, which otherwise reseeds BOTH RNGs with 1234
    before every tick, writes --seed into the game RNG and, for --aivai, hands
    seat 0 to the AI (the two bytes the debug "+AI" command writes), then
    detaches.
 5. retail_ai_poll.py records a JSONL sample every --every ticks.
 6. --printweights T: at tick >= T, sets the debug-console byte and types
    "+PrintWeights <p> <file>" for every seat; the dumps are copied into the
    run directory (never into the repo).
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import retail_ai_poll  # noqa: E402

DEFAULT_ROOT = Path.home() / 'tak-tmp/retail-ai'
DEFAULT_PROTON = Path.home() / '.local/share/Steam/compatibilitytools.d/Proton-CachyOS Latest'
DEFAULT_UMU = Path.home() / '.local/share/faugus-launcher/umu-run'
# Lobby side order (Skirmish GameOptions Side/EnemySide), checked after start
# against the monarch's type prefix.
SIDES = {'aramon': 0, 'taros': 1, 'veruna': 2, 'zhon': 3, 'creon': 4}
PREFIX = {'aramon': 'ara', 'taros': 'tar', 'veruna': 'ver', 'zhon': 'zon', 'creon': 'cre'}
# Positions on retail's 640x480 client (BattleMenuSingle.gui and the title screen).
CLICK_SKIRMISH = (120, 300)
CLICK_START = (566, 431)
MAP_LIST_ROW0 = (380, 286)        # first visible row of the battle room's map list
MAP_LIST_ROW_H = 22
MAP_LIST_ROWS = 5
MAP_SCROLL_DOWN = (570, 375)      # the list's down arrow
MAP_SCROLL_UP = (570, 286)        # and its up arrow
RNG_SEED = 0x64186c
DEBUG_ROOT = 0x62d558

BASE_REG = r'''Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\Cavedog Entertainment\Kingdoms\RendererOptions]
"RendererPrefs"=dword:0001000e
"RendererType"=dword:00000004
"UseHardwareCursor"=dword:00000000

[HKEY_CURRENT_USER\Software\Cavedog Entertainment\Kingdoms\GameOptions]
"AllowCheating"=dword:00000001

[HKEY_CURRENT_USER\Software\Cavedog Entertainment\Kingdoms\Skirmish GameOptions]
"AllowCheating"=dword:00000001
"RandomStartLocations"=dword:00000000
"Side"=dword:00000000
"EnemySide"=dword:00000001
"FavoriteUnitsPerPlayer"=dword:00000096

[HKEY_CURRENT_USER\Software\Cavedog Entertainment\Kingdoms\InterfaceOptions]
"GameSpeed"=dword:00000014

[HKEY_CURRENT_USER\Software\Cavedog Entertainment\Kingdoms\MusicOptions]
"MusicOn"=dword:00000000

[HKEY_CURRENT_USER\Software\Cavedog Entertainment\Kingdoms\SoundOptions]
"SoundVolume"=dword:00000000
'''


def log(message):
    print(f'[{time.strftime("%H:%M:%S")}] {message}', file=sys.stderr, flush=True)


def taskset(cores):
    return ['taskset', '-c', cores] if cores else []


class Run:
    """One harness run: its directories, environment and the PIDs it owns."""

    def __init__(self, args, directory):
        self.args = args
        self.dir = directory
        self.game = directory / 'game'
        self.prefix = directory / 'pfx'
        self.xdg = directory / 'xdg'
        self.kwin = None
        self.launcher = None
        self.display = None
        self.game_pid = None
        self.holder_pid = None   # the display-holder's `sleep`, recorded at start
        self.helpers = []        # other Popen children to stop at teardown (GDB)

    # ---- environment -------------------------------------------------
    def env(self):
        env = dict(os.environ)
        env.update({'WINEPREFIX': str(self.prefix), 'PROTONPATH': str(self.args.proton),
                    'GAMEID': 'umu-0', 'UMU_RUNTIME_UPDATE': '0', 'PROTON_ENABLE_WAYLAND': '0',
                    'PULSE_SERVER': 'none', 'WINEDEBUG': '-all', 'DISPLAY': self.display,
                    'TMPDIR': str(self.args.tmpdir), 'XDG_RUNTIME_DIR': str(self.xdg)})
        env.pop('WAYLAND_DISPLAY', None)
        return env

    def start_display(self):
        self.xdg.mkdir(mode=0o700, parents=True, exist_ok=True)
        marker = self.dir / 'display.txt'
        marker.unlink(missing_ok=True)
        pid_file = self.dir / 'display-holder.pid'
        pid_file.unlink(missing_ok=True)
        # kwin runs this as its client; `exec` keeps the shell's PID for the
        # sleep, so the PID written first is the one teardown has to stop (kwin
        # does not keep it in its own process group, and it outlives a kwin
        # that has already exited).
        holder = self.dir / 'display-holder.sh'
        holder.write_text('#!/bin/sh\necho "$$" > "$2.tmp" && mv "$2.tmp" "$2"\n'
                          'echo "$DISPLAY" > "$1"\nexec sleep 1000000\n')
        holder.chmod(0o755)
        env = dict(os.environ, XDG_RUNTIME_DIR=str(self.xdg))
        env.pop('WAYLAND_DISPLAY', None)
        env.pop('DISPLAY', None)
        with open(self.dir / 'kwin.log', 'w') as out:
            self.kwin = subprocess.Popen(
                [*taskset(self.args.cores), 'kwin_wayland', '--virtual', '--xwayland',
                 '--socket', f'tak-rai-{os.getpid()}', '--width', '1024', '--height', '768',
                 f'{holder} {marker} {pid_file}'], stdout=out, stderr=subprocess.STDOUT, env=env,
                start_new_session=True)
        for _ in range(100):
            if marker.exists() and marker.read_text().strip():
                self.display = marker.read_text().strip()
                self.holder_pid = int(pid_file.read_text())
                log(f'virtual display {self.display} (kwin pid {self.kwin.pid}, holder pid {self.holder_pid})')
                return
            if self.kwin.poll() is not None:
                break
            time.sleep(0.1)
        raise RuntimeError('kwin_wayland --virtual did not provide an X display; see kwin.log')

    def umu(self, argv, log_name, wait=False):
        command = [*taskset(self.args.cores), sys.executable, str(self.args.umu), *argv]
        out = open(self.dir / log_name, 'w')
        process = subprocess.Popen(command, cwd=self.game if self.game.exists() else self.dir,
                                   env=self.env(), stdout=out, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        if wait:
            process.wait()
            out.close()
        return process

    # ---- process ownership ------------------------------------------
    def owned_pids(self, display=False):
        """PIDs whose environment names this run's prefix (Proton, wine, the game);
        with display=True also those whose XDG_RUNTIME_DIR is this run's private
        one (kwin, Xwayland, the display-holder; the wine side carries it too)."""
        # umu passes WINEPREFIX=<prefix>; inside Proton it becomes <prefix>/pfx/ and
        # STEAM_COMPAT_DATA_PATH=<prefix>.
        prefix = str(self.prefix).encode()
        xdg = str(self.xdg).encode()
        found = []
        for proc in Path('/proc').glob('[0-9]*'):
            if int(proc.name) == os.getpid():
                continue
            try:
                environ = (proc / 'environ').read_bytes()
            except OSError:
                continue
            for entry in environ.split(b'\0'):
                key, _, value = entry.partition(b'=')
                if key in (b'WINEPREFIX', b'STEAM_COMPAT_DATA_PATH') and (
                        value.rstrip(b'/') == prefix or value.startswith(prefix + b'/')):
                    found.append(int(proc.name))
                    break
                if display and key == b'XDG_RUNTIME_DIR' and value.rstrip(b'/') == xdg:
                    found.append(int(proc.name))
                    break
        return found

    def _is_holder(self, pid):
        """The recorded PID is still this run's display-holder sleep (not a reused PID)."""
        try:
            argv = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
        except OSError:
            return False
        return len(argv) >= 2 and os.path.basename(argv[0]) == b'sleep' and argv[1] == b'1000000'

    @staticmethod
    def _gone(pid, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except OSError:
                return True
            # A zombie child of ours counts as gone (nothing left running).
            try:
                if Path(f'/proc/{pid}/stat').read_text().split(') ')[-1].startswith('Z'):
                    return True
            except OSError:
                return True
            time.sleep(0.1)
        return False

    def find_game(self, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for pid in self.owned_pids():
                try:
                    maps = Path(f'/proc/{pid}/maps').read_text(errors='ignore')
                except OSError:
                    continue
                if any(line.startswith('00400000-') and 'KINGDOMS.icd' in line
                       for line in maps.splitlines()):
                    self.game_pid = pid
                    return pid
            time.sleep(0.5)
        raise RuntimeError('retail game process did not appear')

    def teardown(self):
        for attempt in range(3):
            pids = self.owned_pids()
            if not pids:
                break
            for pid in pids:
                try:
                    os.kill(pid, signal.SIGTERM if attempt == 0 else signal.SIGKILL)
                except OSError:
                    pass
            time.sleep(2)
        if self.launcher and self.launcher.poll() is None:
            self.launcher.kill()
        for helper in self.helpers:
            if helper.poll() is None:
                helper.kill()
                helper.wait()
        # The display-holder by the PID it wrote: it is not in kwin's process
        # group, and it is left behind whenever kwin has already gone.
        if self.holder_pid and self._is_holder(self.holder_pid):
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.kill(self.holder_pid, sig)
                except OSError:
                    break
                if self._gone(self.holder_pid, 2):
                    break
        if self.kwin and self.kwin.poll() is None:
            # kwin leads the session it was started in (Xwayland stays in it).
            try:
                os.killpg(self.kwin.pid, signal.SIGTERM)
            except OSError:
                pass
            try:
                self.kwin.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(self.kwin.pid, signal.SIGKILL)
                self.kwin.wait()
        # Anything else that still carries this run's prefix or private
        # XDG_RUNTIME_DIR is this run's by construction: stop it too.
        left = self.owned_pids(display=True)
        if left:
            log(f'warning: processes still alive after teardown, killing: {left}')
            for pid in left:
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
            for pid in left:
                self._gone(pid, 2)

    # ---- driving -----------------------------------------------------
    def windows(self):
        result = subprocess.run(['wmctrl', '-l'], env=dict(os.environ, DISPLAY=self.display),
                                capture_output=True, text=True)
        return [line.split()[0] for line in result.stdout.splitlines() if 'Kingdoms' in line]

    def screenshot(self, name):
        if self.args.no_screenshots:
            return
        ids = self.windows()
        if ids:
            subprocess.run(['timeout', '10', 'import', '-window', ids[0], str(self.dir / name)],
                           env=dict(os.environ, DISPLAY=self.display), capture_output=True)

    def drive(self, *command, stdin=None):
        env = dict(os.environ, DISPLAY=self.display, TMPDIR=str(self.args.tmpdir))
        result = subprocess.run(
            ['timeout', '120', sys.executable, str(HERE / 'retail_driver.py'), '--game-pid',
             str(self.game_pid), *command], env=env, input=stdin, text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(f'input bridge failed ({" ".join(command)}): {result.stderr.strip()[-400:]}')
        return result.stdout

    def click(self, xy, pause=1.0):
        self.drive('click', str(xy[0]), str(xy[1]), 'left')
        time.sleep(pause)

    def chat(self, text, pause_ms=300):
        script = f'key 13\nwait {pause_ms}\ntext {text}\nwait 200\nkey 13\nwait {pause_ms}\n'
        self.drive('sequence', '-', stdin=script)


# ---- prefix and game copy ------------------------------------------------
def copy_tree(source, destination):
    subprocess.run(['cp', '-a', '--reflink=auto', str(source), str(destination)], check=True)


def set_reg_dwords(user_reg, section, values):
    """Set dword values in one HKCU section of a Wine user.reg (text edit, prefix offline)."""
    text = user_reg.read_text(encoding='latin1')
    header = '[' + section.replace('\\', '\\\\') + ']'
    lines = text.split('\n')
    start = next((i for i, line in enumerate(lines) if line.lower().startswith(header.lower())), None)
    if start is None:
        lines += ['', f'{header} 0', *[f'"{k}"=dword:{v:08x}' for k, v in values.items()], '']
    else:
        end = start + 1
        while end < len(lines) and lines[end].strip():
            end += 1
        body = lines[start + 1:end]
        for key, value in values.items():
            entry = f'"{key}"=dword:{value:08x}'
            index = next((i for i, line in enumerate(body) if line.lower().startswith(f'"{key.lower()}"=')), None)
            if index is None:
                body.append(entry)
            else:
                body[index] = entry
        lines[start + 1:end] = body
    user_reg.write_text('\n'.join(lines), encoding='latin1')


def setup(args):
    template = args.root / 'prefix-template'
    if (template / 'user.reg').exists() and not args.force:
        log(f'template prefix already exists: {template}')
        return template
    if template.exists():
        shutil.rmtree(template)
    work = args.root / 'setup'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    run = Run(args, work)
    run.prefix = template
    template.mkdir(parents=True)
    (work / 'base.reg').write_text(BASE_REG.replace('\n', '\r\n'), encoding='latin1')
    try:
        run.start_display()
        log('creating the template Proton prefix (about a minute)')
        process = run.umu(['regedit', '/S', str(work / 'base.reg')], 'setup-umu.log', wait=True)
        if process.returncode:
            raise RuntimeError(f'umu-run regedit failed ({process.returncode}); see {work}/setup-umu.log')
        # Let wineserver flush the registry, then stop everything this setup owns.
        deadline = time.monotonic() + 30
        while run.owned_pids() and time.monotonic() < deadline:
            time.sleep(1)
    finally:
        run.teardown()
    if 'Skirmish GameOptions' not in (template / 'user.reg').read_text(encoding='latin1'):
        raise RuntimeError('registry import did not reach user.reg')
    log(f'template prefix ready: {template}')
    return template


def prepare(args, run):
    template = args.root / 'prefix-template'
    if not (template / 'user.reg').exists():
        raise RuntimeError(f'no template prefix; run "{Path(__file__).name} setup" first')
    source = Path(args.game).resolve()
    if not (source / 'Kingdoms.exe').exists() or not (source / 'KINGDOMS.icd').exists():
        raise RuntimeError(f'{source} is not a Kingdoms install (Kingdoms.exe + KINGDOMS.icd)')
    log(f'copying game ({source}) and prefix into {run.dir}')
    copy_tree(source, run.game)
    copy_tree(template, run.prefix)
    section = r'Software\Cavedog Entertainment\Kingdoms\Skirmish GameOptions'
    set_reg_dwords(run.prefix / 'user.reg', section, {
        'Side': SIDES[args.side], 'EnemySide': SIDES[args.enemy], 'RandomStartLocations': 0,
        'FavoriteUnitsPerPlayer': args.unit_cap, 'AllowCheating': 1})
    set_reg_dwords(run.prefix / 'user.reg', r'Software\Cavedog Entertainment\Kingdoms\InterfaceOptions',
                   {'GameSpeed': args.game_speed + 10})


# ---- GDB tick-boundary pokes ------------------------------------------
GDB_SCRIPT = r'''
import gdb, os, struct
inferior = gdb.selected_inferior()
u32 = lambda a: struct.unpack('<I', inferior.read_memory(a, 4))[0]
target = int(os.environ['TAK_POKE_TICK'])
seed = os.environ['TAK_POKE_SEED']
aivai = os.environ.get('TAK_POKE_AIVAI') == '1'
game = u32(0x62d55c)
tick_address = game + 0x19f44
result = {'game': game}
try:
    current = u32(tick_address)
    if current >= target:
        result['error'] = f'already at tick {current} (> poke tick {target})'
    else:
        gdb.execute(f'watch *(unsigned int*){tick_address:#x}', to_string=True)
        open(os.environ['TAK_POKE_ARMED'], 'w').close()
        while u32(tick_address) < target:
            gdb.execute('continue', to_string=True)
        gdb.execute('delete', to_string=True)
        result['tick'] = u32(tick_address)
        result['rng_before'] = u32(0x64186c)
        # -AutoStop makes the per-tick step (0x526310) reseed the game RNG with 1234
        # before every tick. Clearing the switch's cached N (0x6411cc) stops that
        # (and its wall-clock exit); unpaced ticks stay, being latched in 0x641806
        # at start-up. From here the game runs one continuous RNG stream from the
        # seed, as a normal game does from its start-up seed.
        if os.environ.get('TAK_POKE_KEEP_RESEED') != '1':
            inferior.write_memory(0x6411cc, struct.pack('<I', 0))
        inferior.write_memory(0x64186c, struct.pack('<I', int(seed, 0)))
        if aivai:
            owner = game + 0x2404
            inferior.write_memory(owner + 0xea, b'\x02')
            inferior.write_memory(u32(owner + 0x50) + 0x84, b'\x02')
            result['aivai'] = True
        result['rng_after'] = u32(0x64186c)
except Exception as error:
    result['error'] = str(error)
finally:
    try:
        gdb.execute('delete', to_string=True)
    except gdb.error:
        pass
    gdb.execute('detach', to_string=True)
    with open(os.environ['TAK_POKE_OUT'], 'w') as out:
        out.write(__import__('json').dumps(result))
'''


class TickPoke:
    """GDB, attached in the battle room before Start, stops the game on the write
    of tick `tick` (a hardware watchpoint on the tick counter, so the stop point
    is the same instruction every run), writes the seed and the AI-handover
    bytes, then detaches. The game object [0x62d55c] already exists in the
    battle room and keeps its address when the game starts."""

    def __init__(self, run, tick, seed, aivai, keep_reseed=False):
        self.run = run
        script = run.dir / 'poke_gdb.py'
        script.write_text(GDB_SCRIPT)
        self.out, self.armed = run.dir / 'poke.json', run.dir / 'poke.armed'
        env = dict(os.environ, TAK_POKE_TICK=str(tick), TAK_POKE_OUT=str(self.out),
                   TAK_POKE_ARMED=str(self.armed), TAK_POKE_AIVAI='1' if aivai else '0',
                   TAK_POKE_KEEP_RESEED='1' if keep_reseed else '0')
        env['TAK_POKE_SEED'] = str(seed)
        commands = ['gdb', '-nx', '-q', '-batch', '-ex', 'set debuginfod enabled off',
                    '-ex', 'set pagination off', '-ex', 'set print thread-events off',
                    '-ex', 'handle SIGUSR1 nostop noprint pass', '-ex', f'attach {run.game_pid}',
                    '-ex', f'source {script}']
        self.log = open(run.dir / 'gdb.log', 'w')
        # SIGINT interrupts a pending continue so the script still detaches.
        self.process = subprocess.Popen(['timeout', '--signal=INT', '--kill-after=5s', '600s', *commands],
                                        env=env, stdout=self.log, stderr=subprocess.STDOUT)
        run.helpers.append(self.process)
        deadline = time.monotonic() + 60
        while not self.armed.exists():
            if self.process.poll() is not None or time.monotonic() > deadline:
                self.finish()
                raise RuntimeError('GDB did not arm the tick watchpoint; see gdb.log')
            time.sleep(0.2)

    def finish(self, timeout=300):
        try:
            self.process.wait(timeout)
        except subprocess.TimeoutExpired:
            self.process.send_signal(signal.SIGINT)
            self.process.wait(30)
        self.log.close()
        if not self.out.exists():
            raise RuntimeError('GDB poke produced no result; see gdb.log')
        result = json.loads(self.out.read_text())
        if 'error' in result:
            raise RuntimeError(f'GDB poke failed: {result["error"]}')
        return result


def poke_debug_console(pid, value=1):
    """Unlock retail's debug console tier: one byte, [[0x62d558]+4]+4."""
    fd = os.open(f'/proc/{pid}/mem', os.O_RDWR)
    try:
        u = lambda a: struct.unpack('<I', os.pread(fd, 4, a))[0]
        address = u(u(DEBUG_ROOT) + 4) + 4
        os.pwrite(fd, bytes([value]), address)
        return os.pread(fd, 1, address)[0]
    finally:
        os.close(fd)


def print_weights(run, reader, players):
    """+PrintWeights for every seat, typed in one input sequence; returns the
    dumps copied into the run dir. The game keeps running while it is typed."""
    poke_debug_console(run.game_pid, 1)
    tick = reader.tick()
    script = ''.join(f'key 13\nwait 300\ntext +printweights {p} pw{p}.txt\nwait 200\nkey 13\nwait 300\n'
                     for p in players)
    run.drive('sequence', '-', stdin=script)
    copied = []
    for p in players:
        produced = run.game / f'pw{p}.txt'
        for _ in range(20):
            if produced.exists() and produced.stat().st_size:
                break
            time.sleep(0.25)
        if produced.exists():
            destination = run.dir / f'printweights-p{p}.txt'
            shutil.copyfile(produced, destination)
            copied.append(str(destination))
    poke_debug_console(run.game_pid, 0)
    return {'requested_tick': tick, 'files': copied}


# ---- one game ---------------------------------------------------------
def wait_started(reader, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if reader.game() and reader.tick() > 0:
                return reader.tick()
        except (OSError, RuntimeError):
            pass
        time.sleep(0.01)
    raise RuntimeError('the skirmish did not start (tick stayed 0)')


def selected_map(pid):
    """The battle room's selected map, from the map object at [G+0x175dc] (its
    second path string is the terrain file, e.g. MAPS\\abnar's terrace.TNT)."""
    fd = os.open(f'/proc/{pid}/mem', os.O_RDONLY)
    try:
        u = lambda a: struct.unpack('<I', os.pread(fd, 4, a))[0]
        game = u(retail_ai_poll.GAME_PTR)
        if not game:
            return None
        info = u(game + 0x175dc)
        raw = os.pread(fd, 0x100, info + 0x104 + 0x100).split(b'\0', 1)[0].decode('latin1')
    finally:
        os.close(fd)
    name = re.split(r'[\\/]', raw)[-1]
    return re.sub(r'\.tnt$', '', name, flags=re.I) or None


class MapList:
    """Drives the battle room's alphabetical map list: one down-arrow click scrolls
    one row, and a click on the top visible row selects it."""

    def __init__(self, run):
        self.run, self.top = run, 0

    def probe(self, top):
        steps = top - self.top
        arrow = MAP_SCROLL_DOWN if steps > 0 else MAP_SCROLL_UP
        lines = [f'click {arrow[0]} {arrow[1]} left\nwait 40' for _ in range(abs(steps))]
        lines.append(f'click {MAP_LIST_ROW0[0]} {MAP_LIST_ROW0[1]} left\nwait 300')
        self.run.drive('sequence', '-', stdin='\n'.join(lines) + '\n')
        self.top = top
        return selected_map(self.run.game_pid)

    def select_row(self, row):
        self.run.drive('sequence', '-', stdin=f'click {MAP_LIST_ROW0[0]} '
                       f'{MAP_LIST_ROW0[1] + row * MAP_LIST_ROW_H} left\nwait 300\n')
        return selected_map(self.run.game_pid)


def estimate_map_row(game, target):
    """A first guess at the target's list row: the Maps/*.kmp map packs that sort
    before it (the archives add a few dozen more, so the search corrects it)."""
    names = sorted({p.stem.lower() for p in (Path(game) / 'Maps').glob('*.[kK][mM][pP]')})
    return sum(1 for name in names if name < target.lower())


def choose_map(run, target, guess=0):
    """Select `target` (case-insensitive, as listed) in the battle room. The list
    is sorted case-insensitively, so gallop from the guess, then bisect, over the
    scroll position; past the end the list stops scrolling and the target may sit
    lower in the last window."""
    key = target.lower()
    maps = MapList(run)
    below = lambda top: (maps.probe(top) or '').lower() <= key
    if not below(0):
        raise RuntimeError(f'map {target!r} sorts before the first map {selected_map(run.game_pid)!r}')
    # Invariant: below(lo) and not below(hi); 1024 rows is past any real list.
    lo, hi, step = 0, 1024, 8
    point = min(max(0, guess), hi - 1)
    if point and below(point):
        lo = point
        while lo + step < hi:
            if not below(lo + step):
                hi = lo + step
                break
            lo, step = lo + step, step * 2
    elif point:
        hi = point
        while hi - step > lo:
            if below(hi - step):
                lo = hi - step
                break
            hi, step = hi - step, step * 2
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if below(mid):
            lo = mid
        else:
            hi = mid
    name = maps.probe(lo)
    for row in range(MAP_LIST_ROWS):
        if name and name.lower() == key:
            return name
        if row + 1 < MAP_LIST_ROWS:
            name = maps.select_row(row + 1)
    raise RuntimeError(f'map {target!r} is not in the skirmish list')


def run_game(args):
    label = args.label or time.strftime('run-%Y%m%d-%H%M%S')
    directory = args.root / 'runs' / label
    if directory.exists():
        raise RuntimeError(f'{directory} already exists; choose another --label')
    directory.mkdir(parents=True)
    run = Run(args, directory)
    meta = {'label': label, 'side': args.side, 'enemy': args.enemy, 'aivai': args.aivai,
            'seed': args.seed, 'poke_tick': args.poke_tick, 'reseed_every_tick': args.reseed_every_tick,
            'unit_cap': args.unit_cap, 'game_speed': args.game_speed}
    try:
        prepare(args, run)
        run.start_display()
        run.launcher = run.umu(['Kingdoms.exe', f'-AutoStop={args.autostop}'], 'umu.log')
        run.find_game()
        log(f'game pid {run.game_pid}')
        for _ in range(120):
            if run.windows():
                break
            time.sleep(0.5)
        else:
            raise RuntimeError('no Kingdoms window appeared')
        time.sleep(args.title_wait)
        run.screenshot('title.png')
        reader = retail_ai_poll.RetailReader(run.game_pid)
        run.click(CLICK_SKIRMISH, 4.0)
        if args.map:
            meta['map'] = choose_map(run, args.map, estimate_map_row(run.game, args.map))
        else:
            meta['map'] = selected_map(run.game_pid)
        log(f'map: {meta["map"]}')
        run.screenshot('lobby.png')
        if args.stop_at_lobby:
            release = directory / 'release'
            log(f'stopped at the lobby (DISPLAY={run.display}, game pid {run.game_pid}); '
                f'touch {release} to tear down')
            while not release.exists():
                time.sleep(1)
            return 0
        poke = TickPoke(run, args.poke_tick, args.seed, args.aivai, args.reseed_every_tick)
        run.click(CLICK_START, 0.0)
        poke = poke.finish()
        meta['poke'] = poke
        log(f'poked at tick {poke["tick"]}: rng {poke["rng_before"]:#x} -> {poke["rng_after"]:#x}'
            f'{" + seat 0 to AI" if args.aivai else ""}')
        started = wait_started(reader)
        log(f'game running (tick {started})')
        first = None
        while first is None:
            first = reader.sample()
        sides = {}
        for unit in first['units']:
            sides.setdefault(unit[1], unit[2][:3])
        meta['monarch_prefixes'] = sides
        want = {0: PREFIX[args.side], 1: PREFIX[args.enemy]}
        if any(sides.get(p) != prefix for p, prefix in want.items()):
            log(f'warning: seat factions {sides} differ from the requested {want}')
        meta['players'] = [{'p': p['p'], 'controller': p['controller']} for p in first['players']]
        max_ticks = int(args.minutes * 60 * 30)
        output = directory / 'retail.jsonl'
        printed = None
        if args.printweights is not None and args.printweights < max_ticks:
            # Poll to the PrintWeights tick, dump, then go on in the same file.
            end = retail_ai_poll.poll(run.game_pid, output, args.every, args.printweights,
                                      args.squads_every, meta, log=log, write_end=False)
            if end['reason'] == 'max-ticks':
                printed = print_weights(run, reader, [p['p'] for p in first['players']])
                log(f'PrintWeights: {printed}')
                end = retail_ai_poll.poll(run.game_pid, output, args.every, max_ticks,
                                          args.squads_every, meta, log=log, resume=True)
        else:
            end = retail_ai_poll.poll(run.game_pid, output, args.every, max_ticks,
                                      args.squads_every, meta, log=log)
        run.screenshot('end.png')
        meta.update(end=end, printweights=printed)
        (directory / 'meta.json').write_text(json.dumps(meta, indent=1))
        log(f'done: {end}; capture {output}')
        return 0
    finally:
        run.teardown()
        if not args.keep_files:
            for heavy in (run.game, run.prefix, run.xdg):
                shutil.rmtree(heavy, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=Path(os.environ.get('TAK_RETAIL_AI_ROOT', DEFAULT_ROOT)))
    parser.add_argument('--proton', type=Path, default=DEFAULT_PROTON)
    parser.add_argument('--umu', type=Path, default=DEFAULT_UMU)
    parser.add_argument('--cores', default='4-23', help='taskset core list ("" = no pinning)')
    parser.add_argument('--tmpdir', type=Path, default=Path.home() / 'tak-tmp/tmp')
    parser.add_argument('--no-screenshots', action='store_true')
    sub = parser.add_subparsers(dest='command', required=True)
    s = sub.add_parser('setup', help='create the template prefix')
    s.add_argument('--force', action='store_true')
    r = sub.add_parser('run', help='play one recorded skirmish')
    r.add_argument('--label')
    r.add_argument('--game', default=str(REPO / 'assets/game'))
    r.add_argument('--side', choices=SIDES, default='aramon', help='seat 0 (idle human, or AI with --aivai)')
    r.add_argument('--enemy', choices=SIDES, default='taros', help='seat 1 (retail AI)')
    r.add_argument('--aivai', action='store_true', help='hand seat 0 to the AI at --poke-tick')
    r.add_argument('--seed', type=lambda v: int(v, 0), default=1,
                   help='game-RNG state written at --poke-tick (default 1)')
    r.add_argument('--poke-tick', type=int, default=1, help='tick boundary for the seed/AI pokes')
    r.add_argument('--minutes', type=float, default=10.0, help='game minutes to record')
    r.add_argument('--every', type=int, default=30, help='sample cadence in ticks')
    r.add_argument('--squads-every', type=int, default=150, help='decode squads every N ticks (0 = off)')
    r.add_argument('--map', help='skirmish map by name (default: the lobby default, Abnar\'s Terrace)')
    r.add_argument('--unit-cap', type=int, default=150, help='FavoriteUnitsPerPlayer (v4.0 default 150)')
    r.add_argument('--game-speed', type=int, default=0, choices=range(-10, 11),
                   help='-10..+10 (default 0, normal; the speed changes the retail sim, so keep it fixed)')
    r.add_argument('--autostop', type=int, default=999999)
    r.add_argument('--reseed-every-tick', action='store_true',
                   help='debug: keep -AutoStop\'s reseed of both RNGs before every tick. Runs then repeat '
                        'decision for decision, but every tick starts from the same draws and --seed has '
                        'no lasting effect, so such runs are not samples of retail play')
    r.add_argument('--title-wait', type=float, default=25.0, help='seconds after the window before clicking')
    r.add_argument('--printweights', type=int, metavar='TICK', help='dump +PrintWeights at this tick')
    r.add_argument('--keep-files', action='store_true', help='keep the run\'s game and prefix copies')
    r.add_argument('--stop-at-lobby', action='store_true', help='debug: stop in the battle room')
    args = parser.parse_args()
    if args.command == 'run' and not (1 <= args.seed < 0x7fffffff and args.poke_tick >= 1):
        parser.error('--seed must be 1..2^31-2 (a Park-Miller state) and --poke-tick >= 1')
    args.tmpdir.mkdir(parents=True, exist_ok=True)
    args.root.mkdir(parents=True, exist_ok=True)
    try:
        if args.command == 'setup':
            setup(args)
            return 0
        return run_game(args)
    except RuntimeError as error:
        log(f'error: {error}')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
