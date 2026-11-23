#!/usr/bin/env python3
"""The silence timeouts around game start, on a real server, at protocol level.

A seated client that has been sent GameStarting and has not answered Loaded is loading
on its main thread and does not read its socket, so the server gives it 30 s of silence
instead of the 15 s of play and the lobby. The server runs with both scaled down by 5
(TAK_TIMEOUT_MS=3000, TAK_LOAD_TIMEOUT_MS=6000) so nothing here waits the real times:

  kept       Bob goes silent right after GameStarting for 4.5 s (the "20 s" case: past the
             play timeout, inside the loading one) and is still connected; he then reports
             Loaded and the game ticks for both.
  dropped    Bob is silent for 7.5 s (the "35 s" case): dropped, and the server log says
             'timed out while loading (6 s)'.
  midgame    Bob has loaded and then goes silent for 4.5 s: dropped at the play timeout,
             the log says 'silent past the player timeout'.
  lobby      Bob has joined a room that has not started (seated, never loaded) and goes
             silent for 4.5 s: dropped at the play timeout, not the loading one.
  refbuild   The server's own referee build takes 7.5 s (TAK_TEST_REFEREE_DELAY_MS, debug
             builds), longer than even the loading timeout, while it reads no socket. Both
             seats are still connected afterwards and the game ticks: the build is not
             client silence. A client that then goes quiet is still dropped.
"""
import argparse
import os
from pathlib import Path
import struct
import tempfile
import time
import traceback

import server_net as auth
from server_net import lobby
from server_public_test import login, options, server
from server_disconnect_test import Player, pump_all, recipe

PLAY_MS, LOAD_MS = 3000, 6000
SILENT_KEPT, SILENT_DROPPED = 4.5, 7.5


def begin(port, h, alice_name, bob_name, bob_loads):
    """Two seated players through GameStarting. Alice reports Loaded; Bob only if bob_loads."""
    alice, bob = Player(login(port, h, alice_name)), Player(login(port, h, bob_name))
    alice.send('CreateGame', auth.field('wait') + auth.field('') + auth.field(recipe()) + auth.field('') +
               options() + bytes([2, 0, 0]))
    assert alice.take('JoinResult').num('<B') == 1
    state = lobby(alice.take('LobbyState'))
    bob.send('JoinGame', struct.pack('<I', state[0]) + auth.field(''))
    joined = bob.take('JoinResult')
    assert joined.num('<B') == 1 and joined.num('<B') == 1
    values = state[3][0][0]
    auth.slot_update(alice, 0, 1, values[1], values[2], values[3], 1, values[5])
    auth.slot_update(bob, 1, 1, 1, 1, 1, 1, 2)
    assert alice.pump(10, lambda p: any(lobby(auth.Reader(b))[3][1][0][4] for b in p.other.get(auth.MSG['LobbyState'], [])))
    alice.send('StartGame')
    for p in (alice, bob):
        r = p.take('GameStarting')
        lobby(r)
        p.slot, _seed, p.token = r.num('<B'), r.num('<I'), r.num('<Q')
    alice.send('Loaded', struct.pack('<Q', h))
    if bob_loads:
        bob.send('Loaded', struct.pack('<Q', h))
        assert pump_all((alice, bob), 20, lambda: alice.last_tick() >= 10 and bob.last_tick() >= 10), 'no ticks'
    return alice, bob


def silent(alice, seconds):
    """Bob says nothing (and reads nothing); Alice stays a live client."""
    alice.pump(seconds)


def scenario_kept(port, h, log):
    alice, bob = begin(port, h, 'Alice1', 'Bob1', False)
    try:
        silent(alice, SILENT_KEPT)
        assert not alice.closed
        bob.send('Loaded', struct.pack('<Q', h))
        assert pump_all((alice, bob), 20, lambda: alice.last_tick() >= 10 and bob.last_tick() >= 10), \
            'a loading client that was silent inside the loading timeout was dropped'
        assert not bob.closed and bob.bye is None
    finally:
        alice.close(); bob.close()


def scenario_dropped(port, h, log):
    alice, bob = begin(port, h, 'Alice2', 'Bob2', False)
    try:
        silent(alice, SILENT_DROPPED)
        bob.pump(2)
        assert bob.closed, 'a client silent past the loading timeout was kept'
        text = log().replace('\n', ' ')
        assert 'timed out while loading (6 s)' in text, text[-1500:]
    finally:
        alice.close(); bob.close()


def scenario_midgame(port, h, log):
    alice, bob = begin(port, h, 'Alice3', 'Bob3', True)
    try:
        silent(alice, SILENT_KEPT)
        bob.pump(2)
        assert bob.closed, 'a loaded client silent past the play timeout was kept'
        assert 'silent past the player timeout' in log()
    finally:
        alice.close(); bob.close()


def scenario_lobby(port, h, log):
    """A seat in a room that has not started is not loading: the play timeout applies."""
    alice, bob = Player(login(port, h, 'Alice4')), Player(login(port, h, 'Bob4'))
    try:
        alice.send('CreateGame', auth.field('wait') + auth.field('') + auth.field(recipe()) + auth.field('') +
                   options() + bytes([2, 0, 0]))
        assert alice.take('JoinResult').num('<B') == 1
        state = lobby(alice.take('LobbyState'))
        bob.send('JoinGame', struct.pack('<I', state[0]) + auth.field(''))
        assert bob.take('JoinResult').num('<B') == 1
        before = log().count('silent past the player timeout')
        silent(alice, SILENT_KEPT)
        bob.pump(2)
        assert bob.closed, 'a silent seat in an unstarted room got the loading timeout'
        assert log().count('silent past the player timeout') > before, log()[-1500:]
    finally:
        alice.close(); bob.close()


class Skipped(Exception):
    pass


def scenario_refbuild(port, h, log):
    """Time the server spends building the referee world is not the clients' silence."""
    started = time.monotonic()
    alice, bob = begin(port, h, 'Alice5', 'Bob5', True)   # begin() asserts ticks reach both
    try:
        # The hook is debug-only: a release takserver starts at once, which proves nothing.
        if time.monotonic() - started < SILENT_DROPPED:
            raise Skipped('TAK_TEST_REFEREE_DELAY_MS not honoured (a release takserver)')
        assert not alice.closed and not bob.closed, 'a slow referee build dropped the room'
        silent(alice, SILENT_KEPT)
        bob.pump(2)
        assert bob.closed, 'the refreshed clock masked a genuinely silent client'
    finally:
        alice.close(); bob.close()


SCENARIOS = [scenario_kept, scenario_dropped, scenario_midgame, scenario_lobby, scenario_refbuild]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    ap.add_argument('--load-ms', type=int, default=LOAD_MS)   # the "before" check sets it to PLAY_MS
    a = ap.parse_args()
    os.environ['TAK_TIMEOUT_MS'], os.environ['TAK_LOAD_TIMEOUT_MS'] = str(PLAY_MS), str(a.load_ms)
    # No disconnect wait: a silent seat must meet the plain silence timeout.
    os.environ['TAK_STALL_MS'] = '60000'
    failed = []
    # A fresh server per scenario: one server refuses new accounts from one address
    # after a few registrations, and every scenario registers two.
    for scenario in SCENARIOS:
        name = scenario.__name__[len('scenario_'):]
        if name == 'refbuild':
            os.environ['TAK_TEST_REFEREE_DELAY_MS'] = str(int(SILENT_DROPPED * 1000))
        else:
            os.environ.pop('TAK_TEST_REFEREE_DELAY_MS', None)
        with tempfile.TemporaryDirectory(prefix='tak-loadto-') as tmp:
            root = Path(tmp)
            with server(a.server.resolve(), a.data.resolve(), root) as (port, h, process):
                try:
                    scenario(port, h, lambda: (root / 'log').read_text(errors='replace'))
                    print('ok   ' + name)
                except Skipped as why:
                    print('skip ' + name + ': ' + str(why))
                except Exception:
                    failed.append(name)
                    print('FAIL ' + name)
                    traceback.print_exc()
                assert process.poll() is None, 'server exited'
    if failed:
        raise SystemExit('FAIL: ' + ', '.join(failed))
    print('PASS: loading seats get the longer silence timeout; play and the room lobby keep the short one')


if __name__ == '__main__':
    main()
