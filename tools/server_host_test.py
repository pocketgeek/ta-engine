#!/usr/bin/env python3
"""Who holds a room's host powers, and what a reconnect is told, on a real server.

  lobby      A host that created its room to watch it (a spectator host) and leaves it,
             or whose connection goes, takes the room with it when nobody else is in it:
             creating and leaving over and over never uses up --max-games. With a player
             seated, the host's powers pass to that player (told, and usable). An ordinary
             spectator leaving an all-AI game changes nothing: the host keeps the room.
  authority  The host's powers belong to its seat. A host that pauses, loses its link and
             reconnects inside the wait is the host again under its new connection (the
             room is told) and can resume; so can one that reconnects mid-play. A guest that
             reconnects gets no host powers. A host that leaves, paused or not, passes them to
             the other player, and its pause ends with it; a host taken over by an AI after
             its wait passes them too, and coming back late gets nothing. With nobody left to
             take them (an AI and a spectator), nobody has them and the game goes on; with the
             only other player away, its seat takes them when it reconnects.
  speed      A guest back from a dropped link is told the game's speed as it is now (0.1x
             -> 2x and 2x -> 0.1x while it was away), and the host's pause, before the
             bundles it missed, which match the host's.
  lan        A LOCAL SERVER's game is its host's: the host leaving the lobby, as a seated
             player or as a spectator host, or leaving the running game closes it for
             everyone, with no successor.
"""
import argparse
import contextlib
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time
import traceback

import server_net as auth
from server_net import OPTIONS_BYTES, field, lobby
from server_disconnect_test import Player, events_of, pump_all, recipe, same_bundles, waiting_for
from server_public_test import server

STALL_MS, WAIT_MS = 1500, 5000
MAX_GAMES = 2
AI_TAKEOVER = 4      # net::Event::Kind
SPEED = 3            # GameOptions byte: speed in tenths


def options(speed=10):
    # crusades, forfeitSelfDestruct, gameMods, speed (tenths), speedUnlock (on), unitCap,
    # monarchExpendable, stressTest, fogExplored, benchmark, randomStarts, doubleSight, pathMode,
    # allowSpectators (on), mapScenario, powerCodes
    return bytes([0, 1, 0, speed, 1]) + struct.pack('<I', 2000) + bytes([0, 0, 0, 0, 0, 0, 0, 1, 0, 0])


def room_state(body):
    """A LobbyState's room id, options and host."""
    r = auth.Reader(body)
    room = r.num('<I')
    r.field(), r.field(), r.field()
    opts = r.data[r.pos:r.pos + OPTIONS_BYTES]
    r.pos += OPTIONS_BYTES
    return room, opts, r.num('<I')


class Seat(Player):
    """A Player that follows what the server tells it of the room: the host and speed
    (LobbyState, SpeedUpdate) and the host's pause, and the speed it knew at each bundle."""

    def __init__(self, port, h, name):
        peer = auth.BufferedPeer(port)
        peer.send('Hello', struct.pack('<I', auth.VERSION) + field('server-host-test') +
                  struct.pack('<Q', h) + field(name))
        self.id = peer.receive('Welcome').num('<I')   # --no-auth / --lan: no login
        super().__init__(peer)
        self.host = self.speed = self.paused = None
        self.speed_at = {}

    def handle(self, kind, body):
        if kind == auth.MSG['LobbyState']:
            _room, opts, self.host = room_state(body)
            self.speed = opts[SPEED]
        elif kind == auth.MSG['SpeedUpdate']:
            self.speed = body[0]
        elif kind == auth.MSG['Pause']:
            self.paused = True
        elif kind == auth.MSG['Resume']:
            self.paused = False
        elif kind == auth.MSG['TickBundle']:
            self.speed_at[struct.unpack_from('<I', body, 0)[0]] = self.speed
        super().handle(kind, body)

    def answer(self, seconds=10):
        """The answer to a create or join: (ok, slot, why), or (False, None, reason) on a Reject."""
        kinds = (auth.MSG['JoinResult'], auth.MSG['Reject'])
        assert self.pump(seconds, lambda p: any(p.other.get(k) for k in kinds)), 'no answer'
        if self.other.get(auth.MSG['Reject']):
            return False, None, auth.Reader(self.other[auth.MSG['Reject']].pop(0)).field()
        r = auth.Reader(self.other[auth.MSG['JoinResult']].pop(0))
        return bool(r.num('<B')), r.num('<B'), r.field()

    def games(self):
        self.send('ListGames')
        r = self.take('GameList')
        out = {}
        for _ in range(r.num('<I')):
            gid, _name, _map = r.num('<I'), r.field(), r.field()
            r.num('<B'), r.num('<B')
            out[gid] = r.num('<B')   # running
            r.num('<B'), r.num('<I'), r.num('<B')
        return out


def create(p, name, watch=False):
    """Create a room (a spectator host's with `watch`); its id."""
    seen = len(p.other.get(auth.MSG['LobbyState'], []))
    p.send('CreateGame', field(name) + field('') + field(recipe()) + field('') + options() + bytes([2, int(watch), 0]))
    ok, slot, why = p.answer()
    assert ok and slot == (0xFF if watch else 0), ('the create was refused', why)
    assert p.pump(5, lambda q: len(q.other.get(auth.MSG['LobbyState'], [])) > seen), 'no LobbyState'
    return room_state(p.other[auth.MSG['LobbyState']][-1])[0]


def join(p, room, slot):
    p.send('JoinGame', struct.pack('<I', room) + field(''))
    ok, got, why = p.answer()
    assert ok and got == slot, (ok, got, why)


def ready_and_start(host, players, ai=()):
    """Seat AIs, ready every seated player (each its own slot), and start. The players
    ready on the room with the AIs seated: a host's change unreadies the guests, and a
    READY sent on an older revision does not count (protocol 323)."""
    for slot in ai:
        auth.slot_update(host, slot, 2, slot % 5, slot, slot, 1, 2)
    if ai:
        def seated(q):
            return any(all(lobby(auth.Reader(b))[3][s][0][0] == 2 for s in ai)
                       for b in q.other.get(auth.MSG['LobbyState'], []))
        assert host.pump(10, seated), 'the AIs were never seated'
        for p in players:
            assert p.pump(10, lambda q: q.rev >= host.rev), 'a player never saw the AIs seated'
    for p in players:
        auth.slot_update(p, p.slot, 1, p.slot % 5, p.slot, p.slot, 1, 2)
    want = len(players) + len(ai)

    def all_ready(q):
        for b in q.other.get(auth.MSG['LobbyState'], []):
            slots = lobby(auth.Reader(b))[3]
            if sum(1 for (values, _n) in slots if values[0] == 2 or (values[0] == 1 and values[4])) >= want:
                return True
        return False
    assert host.pump(10, all_ready), 'the room never got ready'
    host.send('StartGame')


def started(p, h, loaded=True):
    r = p.take('GameStarting')
    lobby(r)
    p.slot, _seed, p.token = r.num('<B'), r.num('<I'), r.num('<Q')
    if loaded:
        p.send('Loaded', struct.pack('<Q', h))


def start(port, h):
    """Alice (host, slot 0) and Bob (slot 1) playing a small generated map at 1x."""
    alice, bob = Seat(port, h, 'Alice'), Seat(port, h, 'Bob')
    room = create(alice, 'host')
    alice.slot = 0
    join(bob, room, 1)
    bob.slot = 1
    ready_and_start(alice, (alice, bob))
    for p in (alice, bob):
        started(p, h)
    assert pump_all((alice, bob), 20, lambda: alice.last_tick() >= 30 and bob.last_tick() >= 30), 'the game never ticked'
    return room, alice, bob


def rejoin(port, h, room, old, name):
    """A new connection for `old`'s seat, from the first tick it has no bundle for."""
    back = Seat(port, h, name)
    frm = old.next_tick()
    back.bundles = dict(old.bundles)
    back.send('Rejoin', struct.pack('<IQI', room, old.token, frm))
    r = back.take('Reseated')
    back.slot, back.token, first, end = r.num('<B'), r.num('<Q'), r.num('<I'), r.num('<I')
    assert back.slot == old.slot and first == frm and back.token and back.token != old.token
    return back, frm, end


def goes_on(players, watcher, ticks=30, seconds=10):
    before = watcher.last_tick()
    return pump_all(players, seconds, lambda: watcher.last_tick() > before + ticks)


def stays_frozen(players, watcher, seconds=1.0):
    before = watcher.last_tick()
    pump_all(players, seconds)
    return watcher.last_tick() <= before + 1


def close_all(*players):
    for p in players:
        if p is not None:
            with contextlib.suppress(OSError):
                p.close()


# ---- lobby: rooms a spectator host leaves ---------------------------------------------------

def scenario_spectator_host_leaves(port, h):
    alice = Seat(port, h, 'Alice')
    try:
        for i in range(3 * MAX_GAMES):
            room = create(alice, 'watch%d' % i, watch=True)
            alice.send('LeaveGame')
            alice.send('Ping')
            alice.pump(0.3)
            assert room not in alice.games(), 'game %d stayed listed after its spectator host left' % i
        assert alice.games() == {}
    finally:
        close_all(alice)


def scenario_spectator_host_disconnects(port, h):
    watcher = Seat(port, h, 'Watcher')
    try:
        for i in range(3 * MAX_GAMES):
            alice = Seat(port, h, 'Alice%d' % i)
            room = create(alice, 'watch%d' % i, watch=True)
            alice.close()
            deadline = time.monotonic() + 5
            while room in watcher.games():
                assert time.monotonic() < deadline, 'game %d outlived its spectator host\'s connection' % i
                watcher.pump(0.1)
        create(watcher, 'after')   # capacity is still there
        watcher.send('LeaveGame')
    finally:
        close_all(watcher)


def scenario_spectator_host_hands_over(port, h):
    alice, bob = Seat(port, h, 'Alice'), Seat(port, h, 'Bob')
    try:
        room = create(alice, 'handover', watch=True)
        join(bob, room, 0)
        assert bob.pump(5, lambda p: p.host == alice.id)
        alice.send('LeaveGame')
        assert bob.pump(5, lambda p: p.host == bob.id), ('Bob was never made the host', bob.host, bob.id)
        bob.send('SetGameOptions', options(speed=15))
        assert bob.pump(5, lambda p: p.speed == 15), 'the new host could not change the options'
        assert room in alice.games(), 'the room went with a host that had a successor'
        bob.send('LeaveGame')
    finally:
        close_all(alice, bob)


def scenario_spectator_host_leaves_running(port, h):
    """A spectator host leaving a RUNNING game with a seated human: its powers pass on and its
    pause ends, as a seated host's do."""
    alice, bob = Seat(port, h, 'Alice'), Seat(port, h, 'Bob')
    try:
        room = create(alice, 'watched', watch=True)
        join(bob, room, 0)
        bob.slot = 0
        ready_and_start(alice, (bob,), ai=(1,))
        started(alice, h, loaded=False)   # a spectator host never reports Loaded
        started(bob, h)
        assert pump_all((alice, bob), 20, lambda: bob.last_tick() >= 30), 'the game never ticked'
        alice.send('SetPause', b'\x01')
        assert pump_all((alice, bob), 5, lambda: bob.paused), 'the spectator host could not pause'
        alice.send('LeaveGame')
        assert bob.pump(5, lambda p: p.host == bob.id), ('the host\'s powers did not pass on', bob.host, bob.id)
        assert bob.pump(5, lambda p: p.paused is False), 'the pause outlived the host that made it'
        assert goes_on((bob,), bob), 'the game did not go on without its host'
        bob.send('SetGameOptions', options(speed=20))
        assert bob.pump(5, lambda p: p.speed == 20), 'the new host could not change the speed'
        assert alice.games().get(room) == 1, 'the running game is not listed'
    finally:
        close_all(alice, bob)


def scenario_spectator_leaves(port, h):
    alice, carol = Seat(port, h, 'Alice'), Seat(port, h, 'Carol')
    try:
        room = create(alice, 'allai', watch=True)
        ready_and_start(alice, (), ai=(0, 1))
        started(alice, h, loaded=False)   # a spectator host never reports Loaded
        carol.send('Spectate', struct.pack('<I', room) + field(''))
        started(carol, h, loaded=False)
        assert pump_all((alice, carol), 15, lambda: carol.last_tick() >= 30), 'the spectator saw no game'
        carol.send('LeaveGame')
        assert goes_on((alice,), alice), 'the game stopped when a spectator left'
        assert alice.host == alice.id, 'a spectator leaving moved the host'
        alice.send('SetPause', b'\x01')
        assert alice.pump(5, lambda p: p.paused), 'the host lost its powers when a spectator left'
        assert carol.games().get(room) == 1, 'the room is not listed as running'
    finally:
        close_all(alice, carol)


# ---- authority: the host's seat across reconnects and departures ----------------------------

def scenario_host_pause_reconnect(port, h):
    room, alice, bob = start(port, h)
    back = None
    try:
        alice.send('SetPause', b'\x01')
        assert pump_all((alice, bob), 5, lambda: bob.paused), 'the pause never reached the room'
        alice.close()
        assert bob.pump(5, waiting_for(0)), 'the room never waited for the host'
        back, frm, _end = rejoin(port, h, room, alice, 'Alice')
        assert back.id != alice.id
        assert stays_frozen((back, bob), bob), 'the pause ended without its host'
        told = back.paused
        back.send('SetPause', b'\x00')
        assert pump_all((back, bob), 5, lambda: bob.paused is False), 'the reconnected host could not resume'
        assert goes_on((back, bob), bob), 'the game did not go on'
        assert back.host == back.id and bob.host == back.id, \
            ('the reconnected host was not made the host again', back.id, back.host, bob.host)
        assert told, 'the reconnected host was not told the game is paused'
        same_bundles(bob, back, frm)
    finally:
        close_all(alice, bob, back)


def scenario_host_reconnect_running(port, h):
    room, alice, bob = start(port, h)
    back = None
    try:
        alice.close()
        assert bob.pump(5, waiting_for(0)), 'the room never waited for the host'
        back, frm, _end = rejoin(port, h, room, alice, 'Alice')
        assert pump_all((back, bob), 5, lambda: back.host == back.id and bob.host == back.id), \
            ('the reconnected host was not made the host again', back.id, back.host, bob.host)
        back.send('SetGameOptions', options(speed=20))
        assert pump_all((back, bob), 5, lambda: bob.speed == 20 and back.speed == 20), \
            'the reconnected host could not change the speed'
        assert goes_on((back, bob), bob)
        same_bundles(bob, back, frm)
    finally:
        close_all(alice, bob, back)


def scenario_guest_reconnect(port, h):
    room, alice, bob = start(port, h)
    back = None
    try:
        bob.close()
        assert alice.pump(5, waiting_for(1)), 'the room never waited for the guest'
        back, frm, _end = rejoin(port, h, room, bob, 'Bob')
        assert pump_all((alice, back), 5, lambda: back.host == alice.id), \
            ('the reconnected guest was not told who the host is', back.host, alice.id)
        back.send('SetPause', b'\x01')
        back.send('SetGameOptions', options(speed=20))
        pump_all((alice, back), 1.5)
        assert alice.paused is None and alice.speed == 10 and back.speed == 10, \
            'a reconnected guest used the host\'s powers'
        assert alice.host == alice.id
        alice.send('SetPause', b'\x01')
        assert pump_all((alice, back), 5, lambda: back.paused), 'the host lost its powers to a guest\'s reconnect'
        alice.send('SetPause', b'\x00')
        assert goes_on((alice, back), back)
        same_bundles(alice, back, frm)
    finally:
        close_all(alice, bob, back)


def host_leaves(port, h, paused):
    _room, alice, bob = start(port, h)
    try:
        if paused:
            alice.send('SetPause', b'\x01')
            assert pump_all((alice, bob), 5, lambda: bob.paused), 'the pause never reached the room'
        alice.send('LeaveGame')
        assert bob.pump(5, lambda p: p.host == bob.id), ('the host\'s powers did not pass on', bob.host, bob.id)
        if paused:
            assert bob.pump(5, lambda p: p.paused is False), 'the pause outlived the host that made it'
        assert goes_on((bob,), bob), 'the game did not go on without its host'
        bob.send('SetGameOptions', options(speed=20))
        assert bob.pump(5, lambda p: p.speed == 20), 'the new host could not change the speed'
        bob.send('SetPause', b'\x01')
        assert bob.pump(5, lambda p: p.paused), 'the new host could not pause'
        bob.send('SetPause', b'\x00')
        assert bob.pump(5, lambda p: p.paused is False), 'the new host could not resume'
    finally:
        close_all(alice, bob)


def scenario_host_leaves_paused(port, h):
    host_leaves(port, h, True)


def scenario_host_leaves_running(port, h):
    host_leaves(port, h, False)


def scenario_host_timeout(port, h):
    room, alice, bob = start(port, h)
    try:
        # Alice goes silent past the stall threshold and her whole wait: an AI takes her seat.
        assert bob.pump(WAIT_MS / 1000 + 8, lambda p: events_of(p, AI_TAKEOVER)), 'no AI takeover'
        assert events_of(bob, AI_TAKEOVER)[0][1] == (AI_TAKEOVER, 0)
        assert bob.pump(5, lambda p: p.host == bob.id), ('the successor was never told', bob.host, bob.id)
        bob.send('SetPause', b'\x01')
        assert bob.pump(5, lambda p: p.paused), 'the successor could not pause'
        bob.send('SetPause', b'\x00')
        assert bob.pump(5, lambda p: p.paused is False), 'the successor could not resume'
        # The former host, back too late: refused, and the host stays with Bob.
        late = Seat(port, h, 'Alice')
        late.send('Rejoin', struct.pack('<IQI', room, alice.token, alice.next_tick()))
        ok, _slot, why = late.answer()
        assert not ok and b'AI took over' in why, why
        late.close()
        bob.send('SetGameOptions', options(speed=20))
        assert bob.pump(5, lambda p: p.speed == 20 and p.host == bob.id), 'the host moved back to the former host'
    finally:
        close_all(alice, bob)


def scenario_no_successor(port, h):
    alice, carol = Seat(port, h, 'Alice'), Seat(port, h, 'Carol')
    try:
        room = create(alice, 'alone')
        alice.slot = 0
        ready_and_start(alice, (alice,), ai=(1,))
        started(alice, h)
        assert alice.pump(15, lambda p: p.last_tick() >= 30), 'the game never ticked'
        carol.send('Spectate', struct.pack('<I', room) + field(''))
        started(carol, h, loaded=False)
        assert pump_all((alice, carol), 10, lambda: carol.last_tick() >= 30), 'the spectator saw no game'
        alice.send('SetPause', b'\x01')
        assert pump_all((alice, carol), 5, lambda: carol.paused), 'the pause never reached the spectator'
        alice.send('LeaveGame')
        # Nobody may take the host's powers (an AI and a spectator): nobody has them, and
        # the pause they held is over.
        assert carol.pump(5, lambda p: p.paused is False and p.host == 0), \
            ('the game was left paused with nobody to resume it', carol.paused, carol.host)
        assert goes_on((carol,), carol), 'the game did not go on'
        carol.send('SetPause', b'\x01')
        assert stays_frozen((carol,), carol, 1.0) is False and carol.paused is False, 'a spectator took the host\'s powers'
    finally:
        close_all(alice, carol)


def scenario_held_successor(port, h):
    room, alice, bob = start(port, h)
    carol = back = None
    try:
        carol = Seat(port, h, 'Carol')
        carol.send('Spectate', struct.pack('<I', room) + field(''))
        started(carol, h, loaded=False)
        assert pump_all((alice, bob, carol), 10, lambda: carol.last_tick() >= 30), 'the spectator saw no game'
        bob.close()
        assert alice.pump(5, waiting_for(1)), 'the room never waited for the guest'
        alice.send('LeaveGame')
        # The only player left is away: nobody connected holds the host's powers ...
        assert carol.pump(5, lambda p: p.host == 0), ('the host\'s powers went to a connection', carol.host)
        # ... until the seat that holds them is back.
        back, _frm, _end = rejoin(port, h, room, bob, 'Bob')
        assert pump_all((back, carol), 5, lambda: back.host == back.id and carol.host == back.id), \
            ('the held seat did not take the host\'s powers back with it', back.id, back.host, carol.host)
        back.send('SetPause', b'\x01')
        assert pump_all((back, carol), 5, lambda: carol.paused), 'the returning successor could not pause'
        back.send('SetPause', b'\x00')
        assert goes_on((back, carol), carol), 'the game did not go on'
    finally:
        close_all(alice, bob, carol, back)


# ---- speed: what a reconnecting guest is told -----------------------------------------------

def guest_misses(port, h, before, after, pause):
    room, alice, bob = start(port, h)
    back = None
    try:
        alice.send('SetGameOptions', options(speed=before))
        assert pump_all((alice, bob), 5, lambda: alice.speed == before and bob.speed == before)
        assert goes_on((alice, bob), bob, ticks=3)
        # Bob stops reading for a moment (the bundles in his socket are lost with it), then
        # his link goes. While he is away the host changes the speed, and maybe pauses.
        alice.pump(0.7)
        bob.close()
        assert alice.pump(5, waiting_for(1)), 'the room never waited for the guest'
        alice.send('SetGameOptions', options(speed=after))
        if pause:
            alice.send('SetPause', b'\x01')
        assert alice.pump(5, lambda p: p.speed == after and (p.paused or not pause))
        back, frm, end = rejoin(port, h, room, bob, 'Bob')
        assert end > frm, ('nothing to catch up on', frm, end)
        assert pump_all((alice, back), 10, lambda: back.next_tick() >= end), 'the guest was not caught up'
        assert back.speed == after, ('the reconnected guest kept the old speed', back.speed, after)
        late = {t: s for t, s in back.speed_at.items() if t >= frm and s != after}
        assert not late, ('bundles arrived before the reconnected guest knew the speed', sorted(late.items())[:5])
        assert bool(back.paused) == pause, ('the reconnected guest was told the wrong pause', back.paused)
        if pause:
            assert stays_frozen((alice, back), back)
            alice.send('SetPause', b'\x00')
            assert pump_all((alice, back), 5, lambda: back.paused is False)
        assert goes_on((alice, back), back, ticks=3 if after < 10 else 30)
        same_bundles(alice, back, 0)
    finally:
        close_all(alice, bob, back)


def scenario_speed_up_while_away(port, h):
    guest_misses(port, h, 1, 20, False)


def scenario_speed_down_while_away(port, h):
    guest_misses(port, h, 20, 1, False)


def scenario_speed_and_pause_while_away(port, h):
    guest_misses(port, h, 1, 20, True)


# ---- lan: a LOCAL SERVER's game closes with its host ----------------------------------------

def closed_by_host(p):
    assert p.pump(5, lambda q: q.closed and (q.other.get(auth.MSG['Reject']) or q.bye is not None)), \
        'the game did not close for the others when its LOCAL SERVER host left'
    reason = auth.Reader(p.other[auth.MSG['Reject']][0]).field() if p.other.get(auth.MSG['Reject']) else p.bye
    assert reason == b'the host closed the game', reason


def scenario_lan_lobby(port, h):
    alice, bob = Seat(port, h, 'Alice'), Seat(port, h, 'Bob')
    try:
        room = create(alice, 'lan')
        join(bob, room, 1)
        alice.send('LeaveGame')
        closed_by_host(bob)
        assert alice.games() == {}
    finally:
        close_all(alice, bob)


def scenario_lan_spectator_host(port, h):
    alice, bob = Seat(port, h, 'Alice'), Seat(port, h, 'Bob')
    try:
        room = create(alice, 'lan', watch=True)
        join(bob, room, 0)
        alice.send('LeaveGame')
        closed_by_host(bob)
        assert alice.games() == {}
    finally:
        close_all(alice, bob)


def scenario_lan_running(port, h):
    _room, alice, bob = start(port, h)
    try:
        alice.send('LeaveGame')
        closed_by_host(bob)
        assert bob.host != bob.id, 'a LOCAL SERVER\'s host powers passed to a guest'
        deadline = time.monotonic() + 5
        while alice.games():
            assert time.monotonic() < deadline, 'the closed game was never torn down'
            alice.pump(0.1)
    finally:
        close_all(alice, bob)


CASES = {
    'lobby': [scenario_spectator_host_leaves, scenario_spectator_host_disconnects,
              scenario_spectator_host_hands_over, scenario_spectator_host_leaves_running,
              scenario_spectator_leaves],
    'authority': [scenario_host_pause_reconnect, scenario_host_reconnect_running, scenario_guest_reconnect,
                  scenario_host_leaves_paused, scenario_host_leaves_running, scenario_host_timeout,
                  scenario_no_successor, scenario_held_successor],
    'speed': [scenario_speed_up_while_away, scenario_speed_down_while_away, scenario_speed_and_pause_while_away],
    'lan': [scenario_lan_lobby, scenario_lan_spectator_host, scenario_lan_running],
}


@contextlib.contextmanager
def lan_server(binary, data, root):
    """takserver --lan (a player's LOCAL SERVER): no accounts, every interface."""
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    with (root / 'log').open('w') as log:
        p = subprocess.Popen([str(binary), '--lan', '--data', str(data), '--port', str(port),
                              '--map-cache-dir', str(root / 'maps')], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                text = (root / 'log').read_text()
                if 'listening on' in text:
                    yield port, int(re.search(r'retail gameplay hash ([0-9a-f]+)', text)[1], 16), p
                    return
                assert p.poll() is None, text
                time.sleep(.05)
            raise AssertionError('server startup timeout')
        finally:
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    ap.add_argument('--case', choices=sorted(CASES), action='append')
    ap.add_argument('--only', action='append', help='run just these scenarios (names as printed)')
    a = ap.parse_args()
    os.environ['TAK_STALL_MS'], os.environ['TAK_DROP_WAIT_MS'] = str(STALL_MS), str(WAIT_MS)
    failed = []
    for case in a.case or sorted(CASES):
        with tempfile.TemporaryDirectory(prefix='tak-host-') as tmp:
            root = Path(tmp)
            launch = (lan_server(a.server.resolve(), a.data.resolve(), root) if case == 'lan' else
                      server(a.server.resolve(), a.data.resolve(), root, '--no-auth', '--max-games', str(MAX_GAMES)))
            with launch as (port, h, process):
                for scenario in CASES[case]:
                    name = scenario.__name__[len('scenario_'):]
                    if a.only and name not in a.only:
                        continue
                    try:
                        scenario(port, h)
                        print('ok   ' + name, flush=True)
                    except Exception:
                        failed.append(name)
                        print('FAIL ' + name, flush=True)
                        traceback.print_exc()
                assert process.poll() is None, 'server exited'
            if failed:
                print((root / 'log').read_text(errors='replace')[-4000:])
    if failed:
        raise SystemExit('FAIL: ' + ', '.join(failed))
    print('PASS: host powers -- spectator hosts leave no orphan rooms, the host\'s seat keeps them across a '
          'reconnect and passes them on when it goes, a reconnect is told the speed, pause and host as they are '
          'now, a LOCAL SERVER\'s game closes with its host')


if __name__ == '__main__':
    main()
