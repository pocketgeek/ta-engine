#!/usr/bin/env python3
"""ALLOW SPECTATORS on a real server: the host's room rule, enforced by the server.

Each scenario runs on its own and every failure is reported, so a server without
the Spectate check shows exactly which admission paths it leaves open:

  allowed       ON (the default): the game list says so and a WATCH gets the stream
  refused       OFF: a WATCH is refused with the reason, and the in-game options
                message cannot turn it back on (fixed once the game starts)
  reconnect     OFF: a dropped player's new connection cannot come back as a
                watcher, while its seated reconnect (Rejoin) still works
  switch        OFF: a seated player who leaves the running game cannot watch it
  lobby toggle  the host flips it in the lobby; the last value is what the game
                starts with and what the list reports
  host only     a seated non-host cannot change it; when the host leaves the lobby
                the rule stays with the room and the new host may change it
  host watcher  OFF with the host watching its all-AI room: the host keeps its
                seat (the rule governs who ELSE may watch), outsiders are refused
"""
import argparse
import contextlib
from pathlib import Path
import struct
import tempfile
import traceback

import server_net as auth
from server_net import OPTIONS_BYTES, lobby
from server_public_test import login, options, server

REASON = b'spectators are not allowed in this game'
ALLOW = OPTIONS_BYTES - 3   # allowSpectators, before mapScenario and powerCodes
SCENARIO = OPTIONS_BYTES - 2   # mapScenario, before powerCodes (the last GameOptions byte)


def recipe():
    return '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) + bytes([2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()


def create(peer, name, spectators, watch=False):
    peer.send('CreateGame', auth.field(name) + auth.field('') + auth.field(recipe()) +
              auth.field('') + options(spectators=spectators) + bytes([2, int(watch), 0]))
    result = peer.receive('JoinResult')
    assert result.num('<B') == 1
    assert result.num('<B') == (0xFF if watch else 0)
    return lobby(peer.receive('LobbyState'))


def set_spectators(peer, value):
    raw = bytearray(options(spectators=value))
    peer.send('SetGameOptions', bytes(raw))


def games(peer):
    peer.send('ListGames')
    r = peer.receive('GameList')
    out = {}
    for _ in range(r.num('<I')):
        gid, name, _map = r.num('<I'), r.field(), r.field()
        _players, _cap, running, _locked = r.num('<B'), r.num('<B'), r.num('<B'), r.num('<B')
        r.num('<I')
        out[gid] = (name, running, r.num('<B'))
    assert r.pos == len(r.data), 'trailing GameList bytes'
    return out


def starting(peer):
    r = peer.receive('GameStarting')
    state = lobby(r)
    slot, _seed, token = r.num('<B'), r.num('<I'), r.num('<Q')
    return state, slot, token


def watch(peer, room):
    """Spectate; ('refused', reason) or ('watching', options). Reads whichever answer
    comes first, so an unenforcing server fails the assertion instead of hanging."""
    peer.send('Spectate', struct.pack('<I', room) + auth.field(''))
    want = {auth.MSG['JoinResult'], auth.MSG['GameStarting']}
    while True:
        size = struct.unpack('<I', peer.exact(4))[0]
        message = peer.exact(size)
        if message[0] == auth.MSG['Ping']:
            peer.send('Pong')
            continue
        if message[0] not in want:
            peer.pending[message[0]].append(message[1:])
            continue
        r = auth.Reader(message[1:])
        if message[0] == auth.MSG['GameStarting']:
            return 'watching', lobby(r)[2]
        ok, _slot, why = r.num('<B'), r.num('<B'), r.field()
        assert not ok, 'Spectate answered with a successful JoinResult'
        return 'refused', why


def expect_refused(peer, room, what):
    outcome, detail = watch(peer, room)
    assert outcome == 'refused', what + ': the server let a spectator in'
    assert detail == REASON, (what, detail)
    # The connection stays usable in the lobby (the browser shows the reason).
    peer.send('Ping'); peer.receive('Pong')


def seat_and_start(host, state, extra=None):
    """Ready the host (slot 0), seat an AI in slot 1 unless a player holds it, start."""
    values = state[3][0][0]
    auth.slot_update(host, 0, 1, values[1], values[2], values[3], 1, values[5])
    if extra is None:
        auth.slot_update(host, 1, 2, 1, 1, 1, 1, 0)
    else:
        auth.slot_update(extra, 1, 1, 1, 1, 1, 1, 2)
        # That ready travels on the other player's connection, and the server ignores
        # a start while a human is unready (tryStart returns with no reply): wait until
        # the host sees slot 1 seated and ready, or StartGame can arrive first.
        while True:
            values = lobby(host.receive('LobbyState'))[3][1][0]
            if values[0] == 1 and values[4] == 1:
                break
    host.send('StartGame')


def scenario_allowed(port, h):
    with contextlib.closing(login(port, h, 'Alice')) as host, contextlib.closing(login(port, h, 'Alice')) as viewer:
        state = create(host, 'open', 1)
        assert state[2][ALLOW] == 1
        seat_and_start(host, state)
        started, _, _ = starting(host)
        assert started[2][ALLOW] == 1
        assert games(viewer)[state[0]] == (b'open', 1, 1)
        outcome, opts = watch(viewer, state[0])
        assert outcome == 'watching', ('spectating an open game failed', opts)
        assert opts[ALLOW] == 1


def scenario_refused(port, h):
    with contextlib.closing(login(port, h, 'Alice')) as host, contextlib.closing(login(port, h, 'Alice')) as viewer:
        state = create(host, 'closed', 0)
        assert state[2][ALLOW] == 0, 'room did not adopt ALLOW SPECTATORS off'
        assert games(viewer)[state[0]] == (b'closed', 0, 0), 'list does not show spectators off'
        seat_and_start(host, state)
        started, _, _ = starting(host)
        assert started[2][ALLOW] == 0, 'the game did not start with spectators off'
        assert games(viewer)[state[0]] == (b'closed', 1, 0)
        expect_refused(viewer, state[0], 'browser WATCH')
        # Fixed once running: the in-game options message changes only speed.
        set_spectators(host, 1)
        host.send('Ping'); host.receive('Pong')
        assert games(viewer)[state[0]][2] == 0, 'a running room changed its spectator rule'
        expect_refused(viewer, state[0], 'WATCH after an in-game options change')


def scenario_reconnect(port, h):
    with contextlib.ExitStack() as stack:
        host = login(port, h, 'Alice')
        player = stack.enter_context(contextlib.closing(login(port, h, 'Alice')))
        state = create(host, 'reconnect', 0)
        player.send('JoinGame', struct.pack('<I', state[0]) + auth.field(''))
        joined = player.receive('JoinResult')
        assert joined.num('<B') == 1 and joined.num('<B') == 1, 'a player could not join a no-spectator room'
        lobby(host.receive('LobbyState'))
        seat_and_start(host, state, extra=player)
        _, slot, token = starting(host)
        assert slot == 0 and token
        starting(player)
        host.close()   # a dropped seated player: the slot is held for a rejoin
        back = stack.enter_context(contextlib.closing(login(port, h, 'Alice')))
        expect_refused(back, state[0], 'reconnecting as a spectator')
        # The seat itself is not a spectator path: the resume token still works (a
        # reconnect inside the disconnect wait, from the first tick it lacks).
        back.send('Rejoin', struct.pack('<IQI', state[0], token, 0))
        reseated = back.receive('Reseated')
        slot, rotated = reseated.num('<B'), reseated.num('<Q')
        assert slot == 0 and rotated and rotated != token, 'seated reconnect broke with spectators off'


def scenario_switch(port, h):
    with contextlib.closing(login(port, h, 'Alice')) as host, contextlib.closing(login(port, h, 'Alice')) as player:
        state = create(host, 'switch', 0)
        player.send('JoinGame', struct.pack('<I', state[0]) + auth.field(''))
        assert player.receive('JoinResult').num('<B') == 1
        seat_and_start(host, state, extra=player)
        starting(host); starting(player)
        player.send('LeaveGame')   # forfeits the seat; back in the lobby
        expect_refused(player, state[0], 'a seated player switching to spectator')


def scenario_lobby_toggle(port, h):
    with contextlib.closing(login(port, h, 'Alice')) as host, contextlib.closing(login(port, h, 'Alice')) as viewer:
        state = create(host, 'toggle', 1)
        for value in (0, 1, 0):
            set_spectators(host, value)
            assert lobby(host.receive('LobbyState'))[2][ALLOW] == value, 'the lobby did not adopt the toggle'
            assert games(viewer)[state[0]][2] == value
        set_spectators(host, 7)   # any non-zero byte reads as ON, stored as 1
        assert lobby(host.receive('LobbyState'))[2][ALLOW] == 1
        set_spectators(host, 0)
        lobby(host.receive('LobbyState'))
        seat_and_start(host, state)
        started, _, _ = starting(host)
        assert started[2][ALLOW] == 0, 'the last lobby value did not survive into the start'
        expect_refused(viewer, state[0], 'WATCH after a lobby toggle to off')


def scenario_host_only(port, h):
    with contextlib.closing(login(port, h, 'Alice')) as host, \
         contextlib.closing(login(port, h, 'Alice')) as player, \
         contextlib.closing(login(port, h, 'Alice')) as viewer:
        state = create(host, 'host only', 0)
        player.send('JoinGame', struct.pack('<I', state[0]) + auth.field(''))
        assert player.receive('JoinResult').num('<B') == 1
        lobby(player.receive('LobbyState')); lobby(host.receive('LobbyState'))
        # A seated non-host's options message is ignored, this rule with the rest.
        set_spectators(player, 1)
        player.send('Ping'); player.receive('Pong')
        assert games(viewer)[state[0]][2] == 0, 'a non-host changed ALLOW SPECTATORS'
        # The host leaves the lobby: the room passes to the player with its rule kept,
        # and only now may that player change it.
        host.send('LeaveGame')
        assert lobby(player.receive('LobbyState'))[2][ALLOW] == 0, 'the rule did not survive the host leaving'
        set_spectators(player, 1)
        assert lobby(player.receive('LobbyState'))[2][ALLOW] == 1, 'the new host could not change the rule'
        assert games(viewer)[state[0]][2] == 1


def scenario_host_watcher(port, h):
    with contextlib.closing(login(port, h, 'Alice')) as host, contextlib.closing(login(port, h, 'Alice')) as viewer:
        state = create(host, 'watched', 1, watch=True)
        for slot in (0, 1):
            auth.slot_update(host, slot, 2, slot, slot, slot, 1, 0)
        # Turning it off with the host watching keeps the host's seat: it is still in
        # the room (it receives the table) and starts the game as its watcher.
        set_spectators(host, 0)
        latest = None
        for _ in range(3):
            latest = lobby(host.receive('LobbyState'))
        assert latest[2][ALLOW] == 0
        host.send('StartGame')
        started, slot, _ = starting(host)
        assert slot == 0xFF, 'the watching host lost its seat'
        assert started[2][ALLOW] == 0
        expect_refused(viewer, state[0], 'outsider WATCH of a host-watched room')


SCENARIOS = [scenario_allowed, scenario_refused, scenario_reconnect, scenario_switch,
             scenario_lobby_toggle, scenario_host_only, scenario_host_watcher]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    a = ap.parse_args()
    failed = []
    with tempfile.TemporaryDirectory(prefix='tak-spectators-') as tmp:
        with server(a.server.resolve(), a.data.resolve(), Path(tmp)) as (port, h, process):
            for scenario in SCENARIOS:
                name = scenario.__name__[len('scenario_'):]
                try:
                    scenario(port, h)
                    print('ok   ' + name)
                except Exception:
                    failed.append(name)
                    print('FAIL ' + name)
                    traceback.print_exc()
            assert process.poll() is None, 'server exited'
    if failed:
        raise SystemExit('FAIL: ' + ', '.join(failed))
    print('PASS: ALLOW SPECTATORS -- open watch, refused watch (locked in game), reconnect, '
          'switch to spectator, lobby toggle into the start, host only (and handed over), host-watched room')


if __name__ == '__main__':
    main()
