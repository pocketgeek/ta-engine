#!/usr/bin/env python3
"""The game room end to end, driven through its own buttons by TAK_ROOM_SCRIPT.

Each case runs the real debug client (a single-player one starts its own takserver, which
logs to the same stream) on a scratch profile with its own settings file, gives it a
TAK_ROOM_SCRIPT -- room actions, each through the member its button calls, each ended by its
own test within its limit -- and reads the log: the client prints "room script: <step>:
done" per step and "room script: finished", and the server its room lines. A script that
cannot finish prints why and ends the client. TAK_QUIT_TICK ends a game that started.

  1  single player (CLI game room): an AI seated, CHANGE MAP to Inner Circle, START; the
     game plays to the quit tick.
  2  single player WATCH: the host leaves its seat, AIs fill the room, START; the host plays
     the game as a spectator.
  3  CHANGE MAP to a 6-player random map, four AIs seated, then a 2-player one: the server
     removes the three AIs past its seats, and the game on it starts.
  4  multiplayer unready: a guest READY, the host changes FOG OF WAR, the server clears the
     guest's READY (the guest is told so), the guest readies again and the host STARTs.
  5  PLAY THE MACHINE (the menu's door): the room opens at once from the single-player
     settings; an AI, CHANGE MAP and CRUSADES are remembered in spRoom.* (mpRoom.* untouched),
     and the next launch's room opens on that map with those rules.
  6  --play-map: the room opens on the Test Map copy and keeps it (CHANGE MAP is refused
     here), starts with one AI and no second human, and writes no remembered map.
  7  a remembered map that is not installed: the room opens on the launch's map (athri cay)
     instead, and says why.
  8  LOCAL SERVER host: the room opens at once as "<name>'s Game" with the CONNECT password,
     which its line shows on SHOW; LEAVE closes the game and goes back to CONNECT TO SERVER.
  9  online CREATE: the window's default makes a room "<account>'s Game" from the multiplayer
     settings; a rule change is remembered in mpRoom.* (spRoom.* untouched).
  10 single player MAP SCENARIO confirm: on a map whose scenario seats more players than it
     has starts, turning the scenario off would remove AIs, so the first click only arms the
     cell (the note says what the next does); the second removes them, and the room says so.
  11 the CREATE window and the password window: CREATE with a name and a password opens that
     room, locked; a second player's game list has it locked (its row asks the password), a
     wrong password is refused in the window, which stays open (its SHOW is pressed there),
     and the right one, sent from the same window, gets in; the room's password starts
     hidden for the host and for the guest, SHOW shows it, and it is hidden again in the
     room entered next (the guest leaves and joins again). No log has either password: the
     clients say only which way SHOW / HIDE went, and the server nothing.
  12 create, SHOW, LEAVE in quick succession, many times: the tables the server sent before
     it took each LEAVE arrive after it, and the client never ends in a room again (it used
     to reopen the room the server had just dissolved). Each round ends with the game list
     held for a moment (browser:<ms>, which fails in a room).
  13 CREATE on a server at its game limit (--max-games 1, one game open): the CREATE window
     says CANNOT OPEN A GAME: server game capacity reached and stays open, and the
     connection is kept: the same client then joins the game that is there.
  14 START on a server at its running-game limit (--max-running-games 1, another player's
     game running; the room's guest is a scripted peer): the room says CANNOT START: server
     running-game capacity reached; retry later, stays open and still takes its host's
     changes.
  15 presses aimed at the screen before (press: steps, which go the pointer's way). The
     CREATE window's CREATE lies over the room's LEAVE: a press that lands there as the room
     opens, and the second press of a double click on CREATE that comes later, are dropped
     (the client says so) and the player is still in the room. A click of its own on that
     spot is LEAVE; a press as the game list appears is dropped again, and a click on the
     list once it has stood joins the game in its first row.
  16 a password the room no longer has: the host here is a scripted peer that still sends
     SetRoomInfo (this client does not). A guest who typed the password has it behind SHOW;
     when the room's table shows the lock gone the client forgets it (and says so, never
     the password), so a lock set again later is not shown as the one typed.

Debug builds only (TAK_ROOM_SCRIPT, TAK_QUIT_TICK and the CLI game are debug hooks).

usage: room_flow_test.py --client <takclient> --server <takserver> --data <retail-install>
                         --hpitool <hpitool> [--case N ...]
"""
import argparse
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

QUIT_TICK = 60   # a started game plays this far, then the client quits


def recipe(players):
    """A small generated map for `players` (the server opens its seats from that count)."""
    return '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) +
                       bytes([players, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()


def profile(root, name, script, extra=None):
    """A scratch profile (data, config, settings file) and the client's environment."""
    home = Path(root) / name
    for sub in ('data', 'config', 'home'):
        (home / sub).mkdir(parents=True, exist_ok=True)
    settings = home / 'settings.ini'
    settings.write_text('fullscreen = 0\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith('TAK_')}
    env.update(XDG_DATA_HOME=str(home / 'data'), XDG_CONFIG_HOME=str(home / 'config'), HOME=str(home / 'home'),
               TAK_SETTINGS_FILE=str(settings), SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
               TAK_ROOM_SCRIPT=script, TAK_QUIT_TICK=str(QUIT_TICK), TAK_SPEED='40')
    env.update(extra or {})
    return home, env


def run_client(client, argv, env, home, timeout=180):
    """One client run to its end; its whole log (stdout and stderr, its server's lines too)."""
    try:
        done = subprocess.run([client] + argv + ['--winsize', '960', '540'], cwd=home, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                              timeout=timeout)
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
        raise AssertionError('the client was still running after %d s:\n%s' % (timeout, out[-4000:]))
    return done.returncode, done.stdout


def script_ran(log, steps):
    """Every step finished, in order, and nothing failed."""
    for line in log.splitlines():
        if line.startswith('room script: ') and not re.search(
                r': done$|^room script: (finished|wrote |map!:.*: refused here: |(join|create|start)!.*: refused: )', line):
            raise AssertionError('the room script failed: %s' % line)
    at = 0
    for step in steps:
        found = log.find('room script: %s: done' % step, at)
        assert found >= 0, 'step %r never finished' % step
        at = found + 1   # a step given twice finished twice
    assert 'room script: finished' in log, 'the script did not finish'


def played_to_quit(log, code):
    """The started game played to the quit tick and the client ended cleanly."""
    assert code == 0, 'the client exited %d' % code
    played = re.search(r'replay: wrote .*\((\d+) ticks\)', log)
    assert played and int(played[1]) >= QUIT_TICK, 'the game did not play to the quit tick'


def case_single_player(args, root):
    """CLI game room: seat an AI, CHANGE MAP, START, play to the quit tick."""
    home, env = profile(root, 'case1', 'ai:1;map:Inner Circle;start')
    code, log = run_client(args.client, ['game', 'Athri Cay', '--data', args.data], env, home)
    try:
        assert re.search(r"client \d+ created game \d+ ", log), 'no room was created'
        script_ran(log, ['ai:1', 'map:Inner Circle', 'start'])
        # The panel's PICK: the server changed the map (an AI seated on a 4-start map stays).
        assert re.search(r"game \d+: host changed the map to 'inner circle' \(capacity \d, scenario 0, 0 AI removed",
                         log, re.I), 'the server did not change the map'
        assert re.search(r'game \d+ starting with 2 players', log), 'the game did not start with the AI'
        played_to_quit(log, code)
    except AssertionError as e:
        raise AssertionError('%s\n%s' % (e, log[-4000:]))


def case_capacity(args, root):
    """CHANGE MAP to fewer seats: the AIs past them are removed."""
    six, two = recipe(6), recipe(2)
    steps = ['map:' + six, 'ai:1', 'ai:2', 'ai:3', 'ai:4', 'map:' + two, 'start']
    home, env = profile(root, 'case3', ';'.join(steps))
    code, log = run_client(args.client, ['game', 'Athri Cay', '--data', args.data], env, home)
    try:
        script_ran(log, steps)
        assert re.search(r"game \d+: host changed the map to '%s' \(capacity 6, scenario 0, 0 AI removed" % six, log), \
            'the 6-player map was not taken'
        # Capacity 2: the host keeps seat 1 and the AI in seat 2; the AIs in seats 3-5 go.
        assert re.search(r"game \d+: host changed the map to '%s' \(capacity 2, scenario 0, 3 AI removed, 0 moved" % two,
                         log), 'the server did not remove the three AIs past the 2-player map'
        assert re.search(r'game \d+ starting with 2 players', log), 'the game did not start with the AI that stayed'
        played_to_quit(log, code)
    except AssertionError as e:
        raise AssertionError('%s\n%s' % (e, log[-4000:]))


def case_watch(args, root):
    """Single player WATCH: the host watches, AIs fill the room, START as a spectator."""
    home, env = profile(root, 'case2', 'watch;start')
    code, log = run_client(args.client, ['game', 'Athri Cay', '--data', args.data], env, home)
    try:
        script_ran(log, ['watch', 'start'])
        assert re.search(r'client \d+ now watches game \d+', log), 'the server never had the host watching'
        started = re.search(r'game \d+ starting with (\d+) players', log)
        assert started and int(started[1]) >= 2, 'the AIs did not fill the room'
        assert code == 0, 'the client exited %d' % code
        played = re.search(r'replay: wrote .*\((\d+) ticks\)', log)
        assert played and int(played[1]) >= QUIT_TICK, 'the watched game did not play to the quit tick'
    except AssertionError as e:
        raise AssertionError('%s\n%s' % (e, log[-4000:]))


def wait_for(path, marker, proc, timeout=180):
    """`marker` in the log at `path` of a running client."""
    deadline = time.monotonic() + timeout
    while marker not in path.read_text(errors='replace'):
        if proc.poll() is not None or time.monotonic() > deadline:
            raise AssertionError('%r never appeared (client %s):\n%s' % (
                marker, 'exited %d' % proc.returncode if proc.poll() is not None else 'still running',
                path.read_text(errors='replace')[-4000:]))
        time.sleep(0.1)


def run_until(client, argv, env, home, marker, timeout=180):
    """A client that does not end by itself (a room it cannot start, the menu it returns to):
    run it until `marker` is in its log, then end it (SIGTERM, as a window close); its log."""
    path = home / 'client.log'
    with open(path, 'w') as out:
        proc = subprocess.Popen([client] + argv + ['--winsize', '960', '540'], cwd=home, env=env,
                                stdout=out, stderr=subprocess.STDOUT)
    try:
        wait_for(path, marker, proc, timeout)
        time.sleep(0.5)   # the lines that follow it (a settings write) land
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(20)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
    return path.read_text(errors='replace')


def settings_of(home):
    """The profile's settings file as key -> value (a repeated key keeps its last value)."""
    out = {}
    for line in (home / 'settings.ini').read_text(errors='replace').splitlines():
        key, eq, value = line.partition(' = ')
        if eq:
            out[key.strip()] = value.strip()
    return out


def case_door(args, root):
    """PLAY THE MACHINE opens its room at once; the room's changes go to spRoom.*."""
    steps = ['ai:1', 'map:Inner Circle', 'opt:crusades=1', 'start']
    home, env = profile(root, 'case5', ';'.join(steps), {'TAK_MENU_PICK': 'singleplayer'})
    code, log = run_client(args.client, ['menu', '--data', args.data], env, home)
    try:
        assert re.search(r"client \d+ created game \d+ 'Single Player' on 'athri cay'", log, re.I), \
            'the door did not open its room on the launch map'
        script_ran(log, steps)
        played_to_quit(log, code)
        saved = settings_of(home)
        assert saved.get('spRoom.map', '').lower() == 'inner circle', 'the picked map was not remembered: %r' % saved
        assert saved.get('spRoom.crusades') == '1' and saved.get('spRoom.generated') == '0', \
            'CRUSADES was not remembered: %r' % saved
        assert saved.get('mpRoom.map') == '' and saved.get('mpRoom.crusades') == '0', \
            'the multiplayer set changed: %r' % saved
    except AssertionError as e:
        raise AssertionError('%s\n%s' % (e, log[-4000:]))
    # The next launch's room: that map, those rules.
    env['TAK_ROOM_SCRIPT'] = 'ai:1;start'
    code, log = run_client(args.client, ['menu', '--data', args.data], env, home)
    try:
        assert re.search(r"client \d+ created game \d+ 'Single Player' on 'inner circle' \(crusades=1 ", log, re.I), \
            'the next room did not open on the remembered map and rules'
        script_ran(log, ['ai:1', 'start'])
        played_to_quit(log, code)
    except AssertionError as e:
        raise AssertionError('%s\n%s' % (e, log[-4000:]))


def playtest_snapshot(root, data, hpitool):
    """A Cartographer snapshot: one map (Inner Circle's terrain and header, renamed) in a .kmp."""
    src = Path(root) / 'playtest-src'
    (src / 'kmap').mkdir(parents=True)
    for ext in ('tnt', 'ota'):
        (src / 'kmap' / ('Playtest Map.' + ext)).write_bytes(hpi_read(hpitool, data, 'maps/Inner Circle.' + ext))
    kmp = Path(root) / 'Playtest Map.kmp'
    subprocess.run([hpitool, 'pack', str(src), str(kmp)], check=True, stdout=subprocess.DEVNULL)
    return str(kmp)


def case_play_map(args, root):
    """--play-map: its room plays the Test Map copy only, starts solo with an AI, remembers nothing."""
    kmp = playtest_snapshot(root, args.data, args.hpitool)
    steps = ['map!:Inner Circle', 'ai:1', 'start']
    home, env = profile(root, 'case6', ';'.join(steps))
    code, log = run_client(args.client, ['--play-map', kmp, '--data', args.data], env, home)
    try:
        assert re.search(r"client \d+ created game \d+ 'Single Player' on 'playtest map \[[0-9a-f]{16}\]'", log, re.I), \
            'the room did not open on the Test Map copy'
        assert "room script: map!:Inner Circle: refused here: a test map's room keeps its map" in log, \
            'CHANGE MAP was not refused in the room'
        script_ran(log, steps)
        assert re.search(r'game \d+ starting with 2 players', log), 'the game did not start with the AI'
        played_to_quit(log, code)
        saved = settings_of(home) if (home / 'settings.ini').exists() else {}
        assert not saved.get('spRoom.map') and saved.get('spRoom.generated', '0') == '0', \
            'the Test Map run remembered a map: %r' % saved
    except AssertionError as e:
        raise AssertionError('%s\n%s' % (e, log[-4000:]))


def case_fallback(args, root):
    """A remembered map that is not installed: the room opens on the launch's map, and says why."""
    home, env = profile(root, 'case7', 'ai:1;start', {'TAK_MENU_PICK': 'singleplayer'})
    (home / 'settings.ini').write_text('fullscreen = 0\nspRoom.map = No Such Map\n')
    code, log = run_client(args.client, ['menu', '--data', args.data], env, home)
    try:
        opened = re.search(r"client \d+ created game \d+ 'Single Player' on '([^']*)'", log)
        assert opened and (opened[1].lower() == 'athri cay' or opened[1].startswith('~gen1~')), \
            'the room did not open on the fallback map'
        assert re.search(r'room note: THE REMEMBERED MAP COULD NOT BE USED \(IT IS NOT INSTALLED\): .+ WAS CHOSEN', log), \
            'the room did not say why'
        script_ran(log, ['ai:1', 'start'])
        played_to_quit(log, code)
        assert settings_of(home).get('spRoom.map') == 'No Such Map', 'the fallback was remembered'
    except AssertionError as e:
        raise AssertionError('%s\n%s' % (e, log[-4000:]))


def case_lan_host(args, root):
    """LOCAL SERVER host: "<name>'s Game" with its password at once; SHOW; LEAVE to CONNECT."""
    home, env = profile(root, 'case8', 'showpass:1;leave', {'TAK_MENU_PICK': 'lanhost:alice:pw'})
    log = run_until(args.client, ['menu', '--data', args.data], env, home, 'menu: back to CONNECT TO SERVER')
    try:
        assert re.search(r"client \d+ created game \d+ 'alice's Game' on 'athri cay'", log, re.I), \
            'the LOCAL SERVER room was not opened at once'
        assert re.search(r"room: game \d+ 'alice's Game' \(password set\)", log), 'the room has no password'
        script_ran(log, ['showpass:1', 'leave'])
        # The password typed on CONNECT is the room's to show: hidden until SHOW.
        assert 'room: password shown' in log, "the room's line did not show the CONNECT password"
        assert log.find('menu: back to CONNECT TO SERVER') > log.find('room script: leave: done'), \
            'LEAVE did not go back to CONNECT TO SERVER'
    except AssertionError as e:
        raise AssertionError('%s\n%s' % (e, log[-4000:]))


def account_server(args, root, name, options=()):
    """A loopback server that signs players in (each client has an account): its process,
    port and log path."""
    port = free_port()
    shome, senv = profile(root, name, '')
    slog_path = shome / 'server.log'
    with open(slog_path, 'w') as slog:
        server = subprocess.Popen([args.server, '--port', str(port), '--data', args.data, '--local', '--seed', '1',
                                   *options], stdout=slog, stderr=subprocess.STDOUT, env=senv, cwd=shome)
    try:
        deadline = time.monotonic() + 90
        while 'listening' not in slog_path.read_text(errors='replace'):
            assert time.monotonic() < deadline and server.poll() is None, \
                'the server never listened:\n' + slog_path.read_text(errors='replace')[-3000:]
            time.sleep(0.1)
    except BaseException:
        server.kill(); server.wait()
        raise
    return server, port, slog_path


def case_online_create(args, root):
    """Online CREATE: the window's default "<account>'s Game", from the multiplayer settings, remembered there."""
    server, port, slog_path = account_server(args, root, 'case9-server')
    try:
        steps = ['create', 'opt:crusades=1']
        home, env = profile(root, 'case9', ';'.join(steps),
                            {'TAK_MENU_PICK': 'connect:alice:alicepass1@tcp://127.0.0.1:%d' % port})
        log = run_until(args.client, ['menu', '--data', args.data], env, home, 'room script: finished')
        slog_text = slog_path.read_text(errors='replace')
        try:
            script_ran(log, steps)
            assert re.search(r"client \d+ created game \d+ 'alice's Game' on '[^']+' \(crusades=0 ", slog_text), \
                'CREATE did not open "alice\'s Game" from the multiplayer defaults'
            assert re.search(r'game \d+: host changed the rules \(crusades', slog_text), 'the rule did not reach the server'
            saved = settings_of(home)
            assert saved.get('mpRoom.crusades') == '1', 'CRUSADES was not remembered: %r' % saved
            assert saved.get('spRoom.crusades') == '0' and saved.get('spRoom.map') == '', \
                'the single-player set changed: %r' % saved
        except AssertionError as e:
            raise AssertionError('%s\nclient:\n%s\nserver:\n%s' % (e, log[-3000:], slog_text[-3000:]))
    finally:
        if server.poll() is None:
            server.kill(); server.wait()


def case_windows(args, root):
    """The CREATE window and the password window: a named, locked game; a wrong password, then the right one."""
    server, port, slog_path = account_server(args, root, 'case11-server')
    host = None
    right, wrong = 'oak-Xq7.leaf', 'acorn-Zk4.cup'   # found in a log only if a log has them
    try:
        # The host: CREATE's window with a name and a password, then SHOW and HIDE in the room.
        hsteps = ['create:Friday Night:' + right, 'showpass:1', 'showpass:0', 'wait:humans=2']
        hhome, henv = profile(root, 'case11-host', ';'.join(hsteps),
                              {'TAK_MENU_PICK': 'connect:alice:alicepass1@tcp://127.0.0.1:%d' % port})
        hlog_path = hhome / 'client.log'
        with open(hlog_path, 'w') as out:
            host = subprocess.Popen([args.client, 'menu', '--data', args.data, '--winsize', '960', '540'], cwd=hhome,
                                    env=henv, stdout=out, stderr=subprocess.STDOUT)
        wait_for(hlog_path, 'room script: showpass:0: done', host)
        # The guest: the game's row with no password, then a wrong one; SHOW in the window the
        # refusal left open (no window: the step fails); the right one from it; SHOW in the
        # room. Then out (after READY's round trip, so no table of the room is still on its
        # way), in again, and SHOW again: it prints only if that room opened hidden.
        gsteps = ['join!:Friday Night', 'join!:Friday Night:' + wrong, 'showpass:1', 'join:Friday Night:' + right,
                  'showpass:1', 'ready', 'leave', 'join:Friday Night:' + right, 'showpass:1']
        ghome, genv = profile(root, 'case11-guest', ';'.join(gsteps),
                              {'TAK_MENU_PICK': 'connect:bob:bobpass12@tcp://127.0.0.1:%d' % port})
        glog = run_until(args.client, ['menu', '--data', args.data], genv, ghome, 'room script: finished')
        wait_for(hlog_path, 'room script: finished', host, 30)
        hlog, slog_text = hlog_path.read_text(errors='replace'), slog_path.read_text(errors='replace')
        try:
            script_ran(hlog, hsteps)
            script_ran(glog, gsteps)
            assert re.search(r"client \d+ created game \d+ 'Friday Night' on ", slog_text), \
                'CREATE did not open a game under the name typed'
            for who, log, rooms in (('host', hlog, 1), ('guest', glog, 2)):
                assert len(re.findall(r"room: game \d+ 'Friday Night' \(password set\)", log)) == rooms, \
                    "the %s's room is not Friday Night, locked" % who
                # Hidden whenever a room opens: each SHOW changed the line (it tells only a change).
                assert log.count('room: password shown') == rooms, \
                    "the %s's password did not start hidden in each room, then show" % who
                # Only the script's own lines name its steps; nothing else has a password.
                told = [line for line in log.splitlines()
                        if not line.startswith('room script: ') and (right in line or wrong in line)]
                assert not told, "the %s's client logged a password: %r" % (who, told)
            assert right not in slog_text and wrong not in slog_text, 'the server logged a password'
            assert hlog.find('room: password hidden') > hlog.find('room: password shown'), 'HIDE did not hide it again'
            # The second player's list has the game locked: its row asks the password before anything is sent.
            assert 'room script: join!:Friday Night: refused: a locked game needs its password' in glog, \
                "the locked game's row did not open the password window"
            assert 'room script: join!:Friday Night:%s: refused: CANNOT JOIN: wrong password' % wrong in glog, \
                'the wrong password was not refused in the window'
            assert glog.find("room: game") > glog.find('refused: CANNOT JOIN: wrong password'), \
                'the guest was in the room before the right password'
            assert len(re.findall(r'client \d+ joined game \d+ at slot 1', slog_text)) == 2, \
                'the server did not seat the guest once for each join'
        except AssertionError as e:
            raise AssertionError('%s\nhost:\n%s\nguest:\n%s\nserver:\n%s' % (e, hlog[-2500:], glog[-2500:], slog_text[-2500:]))
    finally:
        if host is not None and host.poll() is None:
            host.terminate()
            try:
                host.wait(20)
            except subprocess.TimeoutExpired:
                host.kill(); host.wait()
        if server.poll() is None:
            server.kill(); server.wait()


RACE_ROUNDS = 20


def case_leave_race(args, root):
    """create, SHOW, LEAVE in quick succession, many times: never in a room after LEAVE."""
    server, port, slog_path = account_server(args, root, 'case12-server')
    try:
        # Each round leaves two frames after its room's first table, while the server is
        # still verifying the room's packages (a table follows each step of that); then the
        # game list must hold. (The pause also keeps the rounds inside the server's request
        # budget: a CREATE and its two offers are 48 of the 128 a second.)
        steps = []
        for n in range(RACE_ROUNDS):
            steps += ['create:Race %d:pw' % n, 'showpass:1', 'leave', 'browser:700']
        home, env = profile(root, 'case12', ';'.join(steps),
                            {'TAK_MENU_PICK': 'connect:alice:alicepass1@tcp://127.0.0.1:%d' % port})
        log = ''
        try:
            log = run_until(args.client, ['menu', '--data', args.data], env, home, 'room script: finished', timeout=120)
            slog_text = slog_path.read_text(errors='replace')
            script_ran(log, steps)
            # Every room was seen once: one that opens again after its LEAVE is told again.
            rooms = re.findall(r"room: game (\d+) 'Race \d+' \(password set\)", log)
            assert len(rooms) == RACE_ROUNDS and len(set(rooms)) == RACE_ROUNDS, \
                'the rooms the client was in: %r' % rooms
            assert len(re.findall(r'game \d+ dissolved', slog_text)) == RACE_ROUNDS, 'the server did not dissolve every room'
            assert 'reject client' not in slog_text, 'the server ended the connection'
        except AssertionError as e:
            raise AssertionError('%s\nclient:\n%s\nserver:\n%s' % (
                e, log[-3000:], slog_path.read_text(errors='replace')[-2000:]))
    finally:
        if server.poll() is None:
            server.kill(); server.wait()


def case_game_limit(args, root):
    """CREATE at the server's game limit: refused in the CREATE window, the connection kept."""
    server, port, slog_path = account_server(args, root, 'case13-server', ['--max-games', '1'])
    host = None
    try:
        # The host's game is the one the server has room for.
        hsteps = ['create:First', 'wait:humans=2']
        hhome, henv = profile(root, 'case13-host', ';'.join(hsteps),
                              {'TAK_MENU_PICK': 'connect:alice:alicepass1@tcp://127.0.0.1:%d' % port})
        hlog_path = hhome / 'client.log'
        with open(hlog_path, 'w') as out:
            host = subprocess.Popen([args.client, 'menu', '--data', args.data, '--winsize', '960', '540'], cwd=hhome,
                                    env=henv, stdout=out, stderr=subprocess.STDOUT)
        wait_for(hlog_path, 'room script: create:First: done', host)
        # The second player's CREATE is refused in its window, which stays open (its SHOW
        # is pressed there: no window, and the step fails); then the same connection joins
        # the game that is there.
        refused = 'create!:Second:pw'
        gsteps = [refused, 'showpass:1', 'join:First']
        ghome, genv = profile(root, 'case13-guest', ';'.join(gsteps),
                              {'TAK_MENU_PICK': 'connect:bob:bobpass12@tcp://127.0.0.1:%d' % port})
        glog = ''
        try:
            glog = run_until(args.client, ['menu', '--data', args.data], genv, ghome, 'room script: finished', timeout=90)
            wait_for(hlog_path, 'room script: finished', host, 30)
            slog_text = slog_path.read_text(errors='replace')
            script_ran(glog, gsteps)
            script_ran(hlog_path.read_text(errors='replace'), hsteps)
            assert 'room script: %s: refused: CANNOT OPEN A GAME: server game capacity reached' % refused in glog, \
                'the CREATE window did not say why'
            turned = re.search(r'client (\d+): create refused -- server game capacity reached', slog_text)
            assert turned, 'the server did not refuse the CREATE at its limit'
            assert re.search(r'client %s joined game \d+ at slot 1' % turned[1], slog_text), \
                'the refused connection did not go on to join'
            assert 'reject client' not in slog_text, 'the server ended a connection'
            assert re.search(r"room: game \d+ 'First'", glog), "the second player is not in the first one's room"
        except AssertionError as e:
            raise AssertionError('%s\nhost:\n%s\nguest:\n%s\nserver:\n%s' % (
                e, hlog_path.read_text(errors='replace')[-2000:], glog[-2500:],
                slog_path.read_text(errors='replace')[-2000:]))
    finally:
        if host is not None and host.poll() is None:
            host.terminate()
            try:
                host.wait(20)
            except subprocess.TimeoutExpired:
                host.kill(); host.wait()
        if server.poll() is None:
            server.kill(); server.wait()


def case_running_limit(args, root):
    """START at the server's running-game limit: refused in the room, which stays open."""
    import server_net as auth
    from server_host_test import Seat, create as seat_create, ready_and_start, started
    port = free_port()
    shome, senv = profile(root, 'case14-server', '')
    slog_path = shome / 'server.log'
    with open(slog_path, 'w') as slog:
        server = subprocess.Popen([args.server, '--port', str(port), '--data', args.data, '--local', '--seed', '1',
                                   '--no-auth', '--max-running-games', '1'],
                                  stdout=slog, stderr=subprocess.STDOUT, env=senv, cwd=shome)
    other = guest = proc = None
    try:
        deadline = time.monotonic() + 90
        while 'listening' not in slog_path.read_text(errors='replace'):
            assert time.monotonic() < deadline and server.poll() is None, \
                'the server never listened:\n' + slog_path.read_text(errors='replace')[-3000:]
            time.sleep(0.1)
        h = int(re.search(r'retail gameplay hash ([0-9a-f]+)', slog_path.read_text(errors='replace'))[1], 16)
        # Another player's game (a scripted peer and an AI) runs: the server is at its limit.
        other = Seat(port, h, 'Other')
        seat_create(other, 'running')
        other.slot = 0
        ready_and_start(other, (other,), ai=(1,))
        started(other, h)
        assert other.pump(20, lambda p: p.last_tick() >= 15), "the other player's game never ticked"
        # The host (harness mode 1, which never starts by itself here) presses START once
        # its guest is ready; then changes a rule in the room the refusal left open.
        steps = ['wait:ready=2', 'start!', 'opt:crusades=1']
        home, env = profile(root, 'case14', ';'.join(steps), {'TAK_MPAUTO': '1', 'TAK_MP_WAIT': '9'})
        path = home / 'client.log'
        with open(path, 'w') as out:
            proc = subprocess.Popen([args.client, 'game', 'Athri Cay', '--data', args.data, '--server', '127.0.0.1',
                                     '--serverport', str(port), '--winsize', '960', '540'], cwd=home, env=env,
                                    stdout=out, stderr=subprocess.STDOUT)
        try:
            # The guest: a scripted peer that joins the host's room, says the room's
            # packages are verified (it holds none: only the server's START needs to hear
            # it) and readies on the room as it then is.
            guest = Seat(port, h, 'Guest')
            deadline = time.monotonic() + 90
            room = verified = readied = None
            while 'room script: finished' not in path.read_text(errors='replace'):
                assert proc.poll() is None, 'the client ended (exit %s)' % proc.returncode
                assert time.monotonic() < deadline, 'the script did not finish'
                other.pump(0.05)   # the other game keeps running: its player keeps up
                if room is None:
                    waiting = [gid for gid, running in guest.games().items() if not running]
                    if waiting:
                        room = waiting[0]
                        guest.send('JoinGame', struct.pack('<I', room) + auth.field(''))
                        ok, guest.slot, why = guest.answer()
                        assert ok, 'the guest was not let in: %r' % why
                    continue
                guest.pump(0.05)
                for offer, ready in (('MapOffer', 'MapReady'), ('ModOffer', 'ModReady')):
                    for body in guest.other.pop(auth.MSG[offer], []):
                        r = auth.Reader(body)
                        r.num('<I'), r.field()
                        guest.send(ready, struct.pack('<I', room) + auth.field(r.field()))
                        verified = time.monotonic()
                # READY once the room has been quiet for a moment (both packages offered), and
                # again whenever a later table shows the seat not ready.
                tables = guest.other.get(auth.MSG['LobbyState'], [])
                seat = auth.lobby(auth.Reader(tables[-1]))[3][guest.slot][0] if tables else None
                if verified and time.monotonic() - verified > 0.5 and seat and not seat[4] and readied != guest.rev:
                    readied = guest.rev
                    auth.slot_update(guest, guest.slot, 1, seat[1], seat[2], seat[3], 1, 2)
            other.pump(0.5)
            log, slog_text = path.read_text(errors='replace'), slog_path.read_text(errors='replace')
            script_ran(log, steps)
            assert 'room script: start!: refused: CANNOT START: server running-game capacity reached; retry later' in log, \
                'the room did not say why START was refused'
            refusal = re.search(r'game \d+: start refused -- server running-game capacity reached; retry later', slog_text)
            assert refusal, 'the server did not refuse the START at its limit'
            assert re.search(r'game \d+: host changed the rules \(crusades', slog_text[refusal.end():]), \
                'the room did not take its host\'s change after the refusal'
            assert 'reject client' not in slog_text, 'the server ended a connection'
            assert proc.poll() is None, 'the client did not stay in its room'
        except AssertionError as e:
            raise AssertionError('%s\nclient:\n%s\nserver:\n%s' % (
                e, path.read_text(errors='replace')[-3000:], slog_path.read_text(errors='replace')[-2000:]))
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(20)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
        for peer in (other, guest):
            if peer is not None:
                peer.close()
        if server.poll() is None:
            server.kill(); server.wait()


def case_stale_press(args, root):
    """Presses aimed at the screen before: a double click on CREATE does not LEAVE the room it opens."""
    server, port, slog_path = account_server(args, root, 'case15-server')
    host = None
    try:
        # Another player's game, for the list's first row.
        hsteps = ['create:First', 'wait:humans=2']
        hhome, henv = profile(root, 'case15-host', ';'.join(hsteps),
                              {'TAK_MENU_PICK': 'connect:alice:alicepass1@tcp://127.0.0.1:%d' % port})
        hlog_path = hhome / 'client.log'
        with open(hlog_path, 'w') as out:
            host = subprocess.Popen([args.client, 'menu', '--data', args.data, '--winsize', '960', '540'], cwd=hhome,
                                    env=henv, stdout=out, stderr=subprocess.STDOUT)
        wait_for(hlog_path, 'room script: create:First: done', host)
        # (400, 359) is on the CREATE window's CREATE and on a multiplayer room's LEAVE,
        # (850, 87) on the game list's CREATE and (300, 149) on its first row. A dropped
        # press changes nothing, so each is followed by a step that fails had it been taken.
        steps = ['window:create:Twice:pw', 'browser:400',
                 'press:400:359',          # the window's CREATE
                 'wait:humans=1',          # the room is open
                 'press:400:359',          # lands as the room opens, over LEAVE: dropped
                 'showpass:1',             # still in the room (in the list this step fails)
                 'press:400:359:2:400',    # the second press of a double click, later: dropped
                 'showpass:0',             # still in the room
                 'press:400:359',          # a click of its own there is LEAVE
                 'press:850:87',           # lands as the list appears, on its CREATE: dropped
                 'browser:400',
                 'press:300:149',          # a click on First's row (under a window it is none)
                 'wait:humans=2']
        # What each press came to, in order.
        presses = ['taken', 'dropped', 'dropped', 'taken', 'dropped', 'taken']
        home, env = profile(root, 'case15', ';'.join(steps),
                            {'TAK_MENU_PICK': 'connect:bob:bobpass12@tcp://127.0.0.1:%d' % port})
        log = ''
        try:
            log = run_until(args.client, ['menu', '--data', args.data], env, home, 'room script: finished', timeout=90)
            wait_for(hlog_path, 'room script: finished', host, 30)
            slog_text = slog_path.read_text(errors='replace')
            script_ran(log, steps)
            # (The client says a press was dropped before the step that made it ends.)
            came, dropped = [], False
            for line in log.splitlines():
                if line.startswith('lobby: press dropped'):
                    dropped = True
                elif re.match(r'room script: press:.*: done$', line):
                    came.append('dropped' if dropped else 'taken')
                    dropped = False
            assert came == presses, 'what the presses came to: %r, not %r' % (came, presses)
            made = re.search(r"client (\d+) created game (\d+) 'Twice'", slog_text)
            assert made, 'the press on CREATE did not create the game'
            # One room 'Twice', entered once and left once (the steps ran in order: after SHOW,
            # by the click that was a click on LEAVE).
            assert len(re.findall(r"room: game \d+ 'Twice'", log)) == 1, 'the room was entered more than once'
            assert len(re.findall(r'game %s dissolved' % made[2], slog_text)) == 1, 'the room was not left once'
            assert len(re.findall(r'client %s joined game \d+ at slot 1' % made[1], slog_text)) == 1, \
                'the player did not join First once, by the click on the list that had stood'
            assert re.search(r"room: game \d+ 'First'", log), "the player is not in First's room"
        except AssertionError as e:
            raise AssertionError('%s\nclient:\n%s\nhost:\n%s\nserver:\n%s' % (
                e, log[-3000:], hlog_path.read_text(errors='replace')[-1500:],
                slog_path.read_text(errors='replace')[-2000:]))
    finally:
        if host is not None and host.poll() is None:
            host.terminate()
            try:
                host.wait(20)
            except subprocess.TimeoutExpired:
                host.kill(); host.wait()
        if server.poll() is None:
            server.kill(); server.wait()


def case_password_dropped(args, root):
    """A password the room's table says is gone is forgotten, not shown behind SHOW."""
    import server_net as auth
    from server_disconnect_test import Player
    from server_host_test import options
    from server_public_test import login
    server, port, slog_path = account_server(args, root, 'case16-server')
    host = proc = None
    typed = 'birch-Qz1'
    try:
        h = int(re.search(r'retail gameplay hash ([0-9a-f]+)', slog_path.read_text(errors='replace'))[1], 16)
        # The host: a scripted peer with an account, in a locked room on a generated map.
        host = Player(login(port, h, 'carol'))
        host.send('CreateGame', auth.field('Locked') + auth.field(typed) + auth.field(recipe(2)) + auth.field('') +
                  options() + bytes([2, 0, 0]))
        assert host.take('JoinResult').num('<B') == 1, 'the scripted host could not create its room'
        table = lambda: auth.lobby(auth.Reader(host.other[auth.MSG['LobbyState']][-1]))
        assert host.pump(5, lambda p: p.other.get(auth.MSG['LobbyState'])) and table()[6] == 1, 'the room is not locked'
        steps = ['join:Locked:' + typed, 'showpass:1', 'wait:ready=1']
        home, env = profile(root, 'case16', ';'.join(steps),
                            {'TAK_MENU_PICK': 'connect:bob:bobpass12@tcp://127.0.0.1:%d' % port})
        path = home / 'client.log'
        with open(path, 'w') as out:
            proc = subprocess.Popen([args.client, 'menu', '--data', args.data, '--winsize', '960', '540'], cwd=home,
                                    env=env, stdout=out, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 90
            dropped = False
            while 'room script: finished' not in path.read_text(errors='replace'):
                assert proc.poll() is None, 'the client ended (exit %s)' % proc.returncode
                assert time.monotonic() < deadline, 'the script did not finish'
                host.pump(0.05)
                if not dropped and 'room script: showpass:1: done' in path.read_text(errors='replace'):
                    # The guest shows the password it typed. The host drops the lock, then
                    # readies: the guest's script goes on once that table is there.
                    dropped = True
                    host.send('SetRoomInfo', bytes([2]) + auth.field('') + auth.field(''))
                    assert host.pump(5, lambda p: table()[6] == 0), 'the server did not drop the lock'
                    seat = table()[3][0][0]
                    auth.slot_update(host, 0, 1, seat[1], seat[2], seat[3], 1, 2)
            time.sleep(0.3)
            log, slog_text = path.read_text(errors='replace'), slog_path.read_text(errors='replace')
            script_ran(log, steps)
            shown, forgot = log.find('room: password shown'), log.find('room: password forgotten')
            assert 0 <= shown < forgot, 'the client still holds a password the room no longer has'
            assert re.search(r'game \d+: password cleared', slog_text), 'the server did not take the SetRoomInfo'
            told = [line for line in (log + slog_text).splitlines() if typed in line and not line.startswith('room script: ')]
            assert not told, 'a log has the password: %r' % told
        except AssertionError as e:
            raise AssertionError('%s\nclient:\n%s\nserver:\n%s' % (
                e, path.read_text(errors='replace')[-3000:], slog_path.read_text(errors='replace')[-2000:]))
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(20)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
        if host is not None:
            host.close()
        if server.poll() is None:
            server.kill(); server.wait()


def hpi_read(hpitool, data, path):
    """One file of the install, from the archive that wins it."""
    where = subprocess.run([hpitool, 'where', str(data), path], capture_output=True, text=True, check=True).stdout
    archive, entry = re.search(r'-> (.+?)!(.+)$', where.strip()).groups()
    return subprocess.run([hpitool, 'cat', str(Path(data) / archive), entry], capture_output=True, check=True).stdout


def seats_install(root, data, hpitool):
    """The retail install plus "Scenario Seats": King of the Hill with only its first two
    start positions, so its scenario (placements for players 1-4) needs four seats and the
    map alone gives two."""
    install = Path(root) / 'seats-install'
    install.mkdir()
    for entry in Path(data).iterdir():
        if entry.name.lower() != 'maps':
            os.symlink(entry.resolve(), install / entry.name)
    maps = install / 'Maps'
    maps.mkdir()
    for entry in (Path(data) / 'Maps').iterdir():
        os.symlink(entry.resolve(), maps / entry.name)
    src = Path(root) / 'seats-src'
    (src / 'kmap').mkdir(parents=True)
    for ext in ('tnt', 'crt', 'tdf'):
        (src / 'kmap' / ('Scenario Seats.' + ext)).write_bytes(hpi_read(hpitool, data, 'maps/King of the Hill.' + ext))
    ota = hpi_read(hpitool, data, 'maps/King of the Hill.ota').decode('latin-1')
    ota, dropped = re.subn(r'\[special[23]\]\s*\{[^{}]*\}\s*', '', ota)
    assert dropped == 2, 'King of the Hill no longer has the start positions this case drops'
    ota = ota.replace('King of the Hill', 'Scenario Seats').replace('numplayers=4', 'numplayers=2')
    (src / 'kmap' / 'Scenario Seats.ota').write_bytes(ota.encode('latin-1'))
    subprocess.run([hpitool, 'pack', str(src), str(maps / 'Scenario Seats.kmp')], check=True, stdout=subprocess.DEVNULL)
    return str(install)


def case_scenario_confirm(args, root):
    """Single player MAP SCENARIO off with AIs in the scenario's extra seats: arm, confirm."""
    data = seats_install(root, args.data, args.hpitool)
    # The room opens with the scenario on (four seats); three AIs fill them.
    home, env = profile(root, 'case10', 'ai:1;ai:2;ai:3;scenario:0;start', {'TAK_MAP_SCENARIO': '1'})
    code, log = run_client(args.client, ['game', 'Scenario Seats', '--data', data], env, home)
    try:
        script_ran(log, ['ai:1', 'ai:2', 'ai:3', 'scenario:0', 'start'])
        assert re.search(r"client \d+ created game \d+ 'Single Player' on 'Scenario Seats' .*scenario=1", log), \
            'the room did not open on Scenario Seats with its scenario on'
        armed = log.find('room note: CLICK AGAIN: TURNING MAP SCENARIO OFF REMOVES 2 AI PLAYERS')
        assert armed >= 0, 'the first click did not arm the cell'
        turned = [m.start() for m in re.finditer(r'host turned MAP SCENARIO off \(capacity 2, scenario 0, 2 AI removed', log)]
        assert len(turned) == 1 and turned[0] > armed, 'the change was not sent once, after the confirm'
        # (The server sends the new room before it logs the change: the note may come first.)
        assert 'room note: 2 AI PLAYERS LEFT: THE MAP HAS 2 STARTS' in log[armed:], 'the room did not tell the removal'
        assert re.search(r'game \d+ starting with 2 players', log), 'the game did not start with the one AI left'
        assert code == 0, 'the client exited %d' % code
        played = re.search(r'replay: wrote .*\((\d+) ticks\)', log)
        assert played and int(played[1]) >= QUIT_TICK, 'the game did not play to the quit tick'
    except AssertionError as e:
        raise AssertionError('%s\n%s' % (e, log[-4000:]))


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def case_unready(args, root):
    """Multiplayer: the host's rule change clears a READY guest; it readies again; START."""
    port = free_port()
    shome, senv = profile(root, 'case4-server', '')
    slog_path = shome / 'server.log'
    slog = open(slog_path, 'w')
    server = subprocess.Popen([args.server, '--port', str(port), '--data', args.data, '--local', '--seed', '1', '--no-auth'],
                              stdout=slog, stderr=subprocess.STDOUT, env=senv, cwd=shome)
    clients = []
    try:
        deadline = time.monotonic() + 90
        while 'listening' not in slog_path.read_text(errors='replace'):
            assert time.monotonic() < deadline and server.poll() is None, \
                'the server never listened:\n' + slog_path.read_text(errors='replace')[-3000:]
            time.sleep(0.1)
        net = ['game', 'Athri Cay', '--data', args.data, '--server', '127.0.0.1', '--serverport', str(port)]
        # Windowed clients (the room draws, and tells), driven by the harness modes: the host
        # (1) waits for nine READY (never) instead of starting itself, so its script STARTs;
        # the guest (2) joins, readies, and readies again when its READY is cleared.
        hhome, henv = profile(root, 'case4-host', 'wait:ready=2;opt:fog=2;wait:ready=2;start',
                              {'TAK_MPAUTO': '1', 'TAK_MP_WAIT': '9'})
        ghome, genv = profile(root, 'case4-guest', '', {'TAK_MPAUTO': '2'})
        for name, home, env in (('host', hhome, henv), ('guest', ghome, genv)):
            log = open(home / 'client.log', 'w')
            clients.append((name, home, subprocess.Popen([args.client] + net + ['--winsize', '960', '540'], cwd=home,
                                                         env=env, stdout=log, stderr=subprocess.STDOUT), log))
        logs = {}
        for name, home, proc, log in clients:
            try:
                code = proc.wait(180)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
                code = 'still running after 180 s'
            log.close()
            logs[name] = (code, (home / 'client.log').read_text(errors='replace'))
        server.kill(); server.wait(); slog.close()
        slog_text = slog_path.read_text(errors='replace')
        try:
            script_ran(logs['host'][1], ['wait:ready=2', 'opt:fog=2', 'wait:ready=2', 'start'])
            assert re.search(r'game \d+: host changed the rules \(fog[^;]*; rev \d+, 1 unreadied\)', slog_text), \
                'the fog change did not unready the guest'
            assert 'room note: HOST CHANGED FOG OF WAR -- READY AGAIN' in logs['guest'][1], 'the guest was not told'
            assert 'room note: PLAYERS MUST READY AGAIN' in logs['host'][1], 'the host was not told'
            assert re.search(r'game \d+ starting with 2 players', slog_text), 'the game did not start'
            for name, (code, log) in logs.items():
                assert code == 0, 'the %s exited %s' % (name, code)
                played = re.search(r'replay: wrote .*\((\d+) ticks\)', log)
                assert played and int(played[1]) >= QUIT_TICK, 'the %s did not play to the quit tick' % name
        except AssertionError as e:
            raise AssertionError('%s\nhost:\n%s\nguest:\n%s\nserver:\n%s' % (
                e, logs['host'][1][-2500:], logs['guest'][1][-2500:], slog_text[-2500:]))
    finally:
        for _, _, proc, log in clients:
            if proc.poll() is None:
                proc.kill(); proc.wait()
            log.close()
        if server.poll() is None:
            server.kill(); server.wait()
        slog.close()


CASES = {1: case_single_player, 2: case_watch, 3: case_capacity, 4: case_unready, 5: case_door, 6: case_play_map,
         7: case_fallback, 8: case_lan_host, 9: case_online_create, 10: case_scenario_confirm, 11: case_windows,
         12: case_leave_race, 13: case_game_limit, 14: case_running_limit, 15: case_stale_press,
         16: case_password_dropped}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--client', required=True)
    ap.add_argument('--server', required=True)    # the multiplayer cases' own server
    ap.add_argument('--data', required=True)
    ap.add_argument('--hpitool', required=True)   # packs the --play-map case's snapshot
    ap.add_argument('--case', type=int, action='append', choices=sorted(CASES))
    args = ap.parse_args()
    for name in ('client', 'server', 'data', 'hpitool'):
        setattr(args, name, os.path.abspath(getattr(args, name)))
    failed = 0
    with tempfile.TemporaryDirectory(prefix='tak-room-flow-') as root:
        for n in args.case or sorted(CASES):
            try:
                CASES[n](args, root)
                print('PASS: case %d (%s)' % (n, CASES[n].__doc__.split('\n')[0]))
            except AssertionError as e:
                print('FAIL: case %d: %s' % (n, e))
                failed += 1
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
