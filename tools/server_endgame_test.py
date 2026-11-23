#!/usr/bin/env python3
"""The end of a game nobody is seated in, on a real server, at protocol level.

A match that has its result (one team left, or nobody) is ended by the server at that
tick when no human is seated: it closes no more ticks, tells whoever is watching
(Msg::GameOver: the game's length in ticks and the winning team) and writes its replay,
which ends on that tick. A room with a human still seated plays on exactly as before.
Each scenario runs on its own and every failure is reported:

  seated     Alice plays one AI and destroys her own Monarch, so the AI has won; she
             stays seated. The room goes on ticking and nobody is told the game is over
             (a seated human's room is never ended by the server).
  leave      ... then she leaves. No human is seated now, so the game ends at once:
             the watcher is sent GameOver with the AI's team and has every bundle up to
             that length and none after, the room is gone from the server, and the
             server's own replay holds exactly that many ticks.
  takeover   Alice and Bob, no AI. Alice loses and leaves; Bob (the winner) goes
             silent. While the room waits for him it is still his seat, and nothing
             ends; when the wait runs out an AI takes the seat, so no human is seated,
             and the game ends there with Bob's team the winner.
  abandoned  Alice and Bob both leave an undecided game with someone watching: the
             watcher is told the game is over, with no winner.
  walkover   Alice loses to Bob and leaves; when Bob leaves too the watcher is told
             the game is over and that Bob's team won.
  unwatched  an all-AI game whose only watcher (its host) disconnects is torn down at
             once: a game with nobody connected never runs on.
  catchup    (needs --map-tool) a second watcher has not taken the room's map yet
             when the game ends, so it has none of the bundles: it is told the game is
             over, the room stays (refusing new watchers) until it has been sent the
             whole log, and only then goes. Waiting, it is no running game any more:
             it is not in the game list, and it does not count against the server's
             limit on running games (--max-running-games 1 here).
  stuck      (needs --map-tool) a watcher that never takes the map does not hold the
             ended game's room: it goes once that watcher has taken none of the log for
             the drain time (TA_OVER_DRAIN_MS, shortened here).
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import traceback

import server_net as auth
from server_net import lobby
from server_disconnect_test import Player, events_of, pump_all, recipe, waiting_for
from server_public_test import login, server

STALL_MS, WAIT_MS = 1000, 3000     # the disconnect wait, shortened (takeover)
DRAIN_MS = 8000                    # an ended room's wait for a watcher's catch-up, shortened (stuck)
DESTROY = 12                       # net::Cmd::Destroy
LEAVE = 2                          # net::Event::Kind::Leave
NOBODY = 0xFF
# Msg::GameOver. A server built before it existed never sends it; the number past the
# last kind it knows keeps this test runnable against one, to show what it lacks.
GAME_OVER = auth.MSG.get('GameOver', max(auth.MSG.values()) + 1)
auth.MSG.setdefault('GameOver', GAME_OVER)


def options(speed=20):
    # GameOptions wire bytes (server_public_test.options), at 2x so a scenario is short.
    return bytes([0, 1, 0, speed, 0]) + struct.pack('<I', 2000) + bytes([0, 0, 0, 0, 0, 0, 0, 1, 0, 0])


def command(kind, unit):
    return struct.pack('<BBiiffB16s', kind, 7, unit, 0, 0, 0, 0, b'')


def log_says(root, text, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if text in (root / 'log').read_text(errors='replace'):
            return True
        time.sleep(.05)
    return False


def upload(p, room, map_id, package):
    """Offer the host's map package; send it if the server has no copy; verify it."""
    digest = hashlib.sha256(package).hexdigest()
    p.send('MapOffer', struct.pack('<I', room) + auth.field(map_id) + auth.field(digest) +
           struct.pack('<I', len(package)))
    kinds = [auth.MSG[k] for k in ('MapRequest', 'MapOffer', 'MapError')]
    assert p.pump(30, lambda q: any(q.other.get(k) for k in kinds)), 'the server did not answer the map offer'
    assert not p.other.get(auth.MSG['MapError']), 'the server refused the map'
    if p.other.get(auth.MSG['MapRequest']):
        r = p.take('MapRequest')
        assert r.num('<I') == room and r.field() == digest.encode()
        xfer = r.num('<I')
        p.socket.settimeout(30)   # pump() leaves a read timeout far too short for megabytes of chunks
        for offset in range(0, len(package), 65536):
            p.send('MapChunk', struct.pack('<III', room, xfer, offset) + package[offset:offset + 65536])
    r = p.take('MapOffer', 30)
    assert r.num('<I') == room
    r.field()
    assert r.field() == digest.encode(), 'the server verified a different package'
    p.send('MapReady', struct.pack('<I', room) + auth.field(digest))
    return digest


def start(port, h, humans, ais=0, watching_host=None, file_map=None):
    """A started game: `humans` seated in slots 0.., then `ais` AI seats, each seat its
    own team (team = slot). `watching_host`: that account creates the room as a watcher
    instead (an all-AI game). Returns (room, [seated players], host)."""
    players = [Player(login(port, h, name)) for name in humans]
    host = Player(login(port, h, watching_host)) if watching_host else players[0]
    map_id = file_map[0] if file_map else recipe()
    host.send('CreateGame', auth.field('endgame') + auth.field('') + auth.field(map_id) + auth.field('') +
              options() + bytes([2, 1 if watching_host else 0, 0]))
    joined = host.take('JoinResult')
    assert joined.num('<B') == 1, 'CreateGame refused'
    state = lobby(host.take('LobbyState'))
    room = state[0]
    if file_map:
        upload(host, room, *file_map)
    for i, p in enumerate(players):
        if p is not host:
            p.send('JoinGame', struct.pack('<I', room) + auth.field(''))
            joined = p.take('JoinResult')
            assert joined.num('<B') == 1 and joined.num('<B') == i, 'a player could not join'
        auth.slot_update(p, i, 1, i % 5, i, i, 1, 2)
    for slot in range(len(players), len(players) + ais):
        auth.slot_update(host, slot, 2, slot % 5, slot, slot, 1, 0)

    def seated(p):
        table = [lobby(auth.Reader(b)) for b in p.other.get(auth.MSG['LobbyState'], [])]
        if not table:
            return False
        slots = table[-1][3]
        return (all(slots[i][0][0] == 1 and slots[i][0][4] == 1 for i in range(len(players))) and
                all(slots[i][0][0] == 2 for i in range(len(players), len(players) + ais)) and table[-1][4] == 1)
    assert host.pump(15, seated), 'the room never showed every seat taken, ready and the map verified'
    host.send('StartGame')
    for p in players + ([host] if watching_host else []):
        r = p.take('GameStarting', 20)
        lobby(r)
        p.slot, _seed, p.token = r.num('<B'), r.num('<I'), r.num('<Q')
        if p.slot != 0xFF:
            p.send('Loaded', struct.pack('<Q', h))
    everyone = players + ([host] if watching_host else [])
    assert pump_all(everyone, 20, lambda: all(p.last_tick() >= 30 for p in everyone)), 'the game never ticked'
    return room, players, host


def spectate(port, h, name, room, take_map=True):
    """A watcher. In a file-map room the server holds the bundle log back until the
    watcher has verified the map (MapReady); `take_map` False leaves it waiting."""
    w = Player(login(port, h, name))
    w.send('Spectate', struct.pack('<I', room) + auth.field(''))
    r = w.take('GameStarting', 20)
    lobby(r)
    assert r.num('<B') == 0xFF, 'a spectator was given a seat'
    if take_map:
        ready_map(w, room)
    return w


def ready_map(w, room):
    offers = w.other.get(auth.MSG['MapOffer'], [])
    if not offers:
        return
    r = auth.Reader(offers.pop(0))
    assert r.num('<I') == room
    r.field()
    w.send('MapReady', struct.pack('<I', room) + auth.field(r.field()))


def refused_watch(port, h, name, room):
    with_peer = Player(login(port, h, name))
    try:
        with_peer.send('Spectate', struct.pack('<I', room) + auth.field(''))
        answered = with_peer.pump(10, lambda p: p.other.get(auth.MSG['JoinResult']) or p.other.get(auth.MSG['GameStarting']))
        assert answered, 'Spectate was not answered'
        assert not with_peer.other.get(auth.MSG['GameStarting']), 'a game that is over took a new watcher'
        r = with_peer.take('JoinResult')
        assert r.num('<B') == 0
        r.num('<B')
        return r.field()
    finally:
        with_peer.close()


def start_refusals(port, h, name):
    """A lone host creates a room and presses START at once: what the server refuses it
    with. The limit on running games is checked first; under it the START only fails on
    the room itself (its map not verified yet, one seat taken) or is ignored."""
    host = login(port, h, name)
    try:
        host.send('CreateGame', auth.field('another') + auth.field('') + auth.field(recipe()) + auth.field('') +
                  options() + bytes([2, 0, 0]))
        assert host.receive('JoinResult').num('<B') == 1, 'CreateGame refused'
        host.send('StartGame')
        host.send('Ping')
        host.receive('Pong')   # answered once the START has been dealt with
        reasons = []
        for body in host.pending[auth.MSG['MapError']]:
            r = auth.Reader(body)
            r.num('<I')
            reasons.append(r.field())
        return reasons
    finally:
        host.close()


def lose(loser, others):
    """`loser` destroys its own Monarch (the default rules: that is defeat). Returns the
    tick the order ran in; the self-destruct lands at most 75 ticks later."""
    loser.send('PlayerCommands', struct.pack('<I', 48) + b''.join(command(DESTROY, unit) for unit in range(1, 49)))

    def ordered(p):
        return [t for t, (_, cmds, _) in p.bundles.items() if (DESTROY, loser.slot) in cmds]
    assert pump_all([loser] + others, 10, lambda: ordered(loser)), 'the self-destruct order never reached a tick'
    tick = min(ordered(loser))
    assert pump_all([loser] + others, 20, lambda: loser.last_tick() >= tick + 120), 'the game stopped before the result'
    return tick


def game_over(p):
    return p.other.get(GAME_OVER)


def told_over(watcher, others, seconds, what):
    """The watcher's GameOver: (ticks, team). Everyone keeps pumping meanwhile."""
    assert pump_all([watcher] + others, seconds, lambda: game_over(watcher)), what
    r = auth.Reader(watcher.other[GAME_OVER][0])
    ticks, team = r.num('<I'), r.num('<B')
    assert r.pos == len(r.data), 'trailing GameOver bytes'
    return ticks, team


def whole_game(watcher, ticks):
    """The watcher has exactly the game: every bundle below `ticks`, and none arrives after."""
    assert watcher.pump(10, lambda p: len(p.bundles) >= ticks), ('the watcher is missing bundles', len(watcher.bundles), ticks)
    watcher.pump(1.2)
    assert sorted(watcher.bundles) == list(range(ticks)), ('the watcher does not hold ticks 0..%d exactly' % (ticks - 1),
                                                           len(watcher.bundles), watcher.last_tick())
    assert len(watcher.other[GAME_OVER]) == 1, 'the game was declared over more than once'


def scenario_seated_then_leave(port, h, root, _packages):
    room, (alice,), _ = start(port, h, ['Alice'], ais=1)
    wendy = spectate(port, h, 'Wendy', room)
    try:
        lose(alice, [wendy])
        # seated: the AI has won, and the room plays on with Alice in her seat.
        before = alice.last_tick()
        assert pump_all([alice, wendy], 10, lambda: alice.last_tick() >= before + 90), 'a decided game with a human seated stopped'
        assert not game_over(alice) and not game_over(wendy), 'a room with a human seated was declared over'
        assert room in listed(port, h), 'a decided game with a human seated left the list'
        print('ok   seated')
        # leave: now nobody is seated.
        left_at = alice.last_tick()
        alice.send('LeaveGame')
        ticks, team = told_over(wendy, [alice], 5, 'the watcher was never told the game is over: the server plays on')
        assert team == 1, ('the winner is not the AI\'s team', team)
        assert left_at < ticks <= left_at + 60, ('the game did not end when the last human left', left_at, ticks)
        whole_game(wendy, ticks)
        assert log_says(root, 'game %d over after %d ticks (match over): team 2 wins' % (room, ticks)), 'the end was not logged'
        assert log_says(root, 'game %d ended (match over)' % room), 'the room was not torn down'
        assert log_says(root, '(%d ticks, ' % ticks), 'the server\'s replay does not end on the last tick'
        assert list((root / 'replays').glob('game-%d-*.tarep' % room)), 'the server wrote no replay'
        assert room not in listed(port, h), 'the ended game is still listed'
        assert refused_watch(port, h, 'Wendy', room) == b'game not found or ended'
    finally:
        alice.close(); wendy.close()


def listed(port, h):
    peer = Player(login(port, h, 'Wendy'))
    try:
        peer.send('ListGames')
        r = peer.take('GameList')
        rooms = []
        for _ in range(r.num('<I')):
            rooms.append(r.num('<I'))
            r.field(); r.field()
            r.pos += 4 + 4 + 1
        return rooms
    finally:
        peer.close()


def scenario_takeover(port, h, root, _packages):
    room, (alice, bob), _ = start(port, h, ['Alice', 'Bob'])
    wendy = spectate(port, h, 'Wendy', room)
    try:
        lose(alice, [bob, wendy])
        alice.send('LeaveGame')
        assert pump_all([alice, bob, wendy], 5, lambda: events_of(wendy, LEAVE)), 'no Leave event'
        before = wendy.last_tick()
        assert pump_all([bob, wendy], 5, lambda: wendy.last_tick() >= before + 45), 'the game stopped with the winner still seated'
        assert not game_over(wendy), 'the game was declared over with a human (the winner) still seated'
        # Bob goes silent: the room waits for him, and a seat being waited for is still his.
        assert wendy.pump(STALL_MS / 1000 + 5, waiting_for(1)), 'the room never waited for the silent winner'
        frozen = wendy.last_tick()
        wendy.pump(0.8)
        assert not game_over(wendy), 'the game was declared over during the wait'
        # The wait runs out: an AI has the seat, no human is seated, the result stands.
        ticks, team = told_over(wendy, [], WAIT_MS / 1000 + 5, 'the game did not end when an AI took the last human seat')
        assert team == 1, ('the winner is not Bob\'s team', team)
        assert ticks == frozen + 1, ('ticks closed after the wait began', frozen, ticks)
        whole_game(wendy, ticks)
        assert log_says(root, 'game %d ended (match over)' % room), 'the room was not torn down'
    finally:
        alice.close(); bob.close(); wendy.close()


def scenario_abandoned(port, h, root, _packages):
    room, (alice, bob), _ = start(port, h, ['Alice', 'Bob'])
    wendy = spectate(port, h, 'Wendy', room)
    try:
        assert pump_all([alice, bob, wendy], 10, lambda: wendy.last_tick() >= 60)
        alice.send('LeaveGame')
        bob.send('LeaveGame')
        ticks, team = told_over(wendy, [alice, bob], 5, 'the watcher of an abandoned game was never told it is over')
        assert team == NOBODY, ('an undecided game was given a winner', team)
        whole_game(wendy, ticks)
        assert log_says(root, 'game %d over after %d ticks (all players gone): no winner' % (room, ticks))
        assert log_says(root, 'game %d ended (all players gone)' % room), 'the room was not torn down'
    finally:
        alice.close(); bob.close(); wendy.close()


def scenario_walkover(port, h, root, _packages):
    room, (alice, bob), _ = start(port, h, ['Alice', 'Bob'])
    wendy = spectate(port, h, 'Wendy', room)
    try:
        lose(alice, [bob, wendy])
        alice.send('LeaveGame')
        assert pump_all([alice, bob, wendy], 5, lambda: events_of(wendy, LEAVE)), 'no Leave event'
        assert not game_over(wendy), 'the game was declared over with the winner still seated'
        bob.send('LeaveGame')
        ticks, team = told_over(wendy, [alice, bob], 5, 'the watcher was never told the game is over')
        assert team == 1, ('the winner is not Bob\'s team', team)
        whole_game(wendy, ticks)
        assert log_says(root, 'game %d ended (all players gone)' % room), 'the room was not torn down'
    finally:
        alice.close(); bob.close(); wendy.close()


def scenario_unwatched(port, h, root, _packages):
    room, _, host = start(port, h, [], ais=2, watching_host='Wendy')
    try:
        assert host.pump(10, lambda p: p.last_tick() >= 60), 'the all-AI game did not run for its watcher'
        assert not game_over(host), 'an undecided all-AI game was declared over'
    finally:
        host.close()
    assert log_says(root, 'game %d ended (last player left)' % room), 'an all-AI game with nobody connected ran on'
    assert room not in listed(port, h)


def scenario_catchup(port, h, root, packages):
    room, (alice,), _ = start(port, h, ['Alice'], ais=1, file_map=('Inner Circle', packages['Inner Circle']))
    wendy = spectate(port, h, 'Wendy', room)
    late = None
    try:
        lose(alice, [wendy])
        # The second watcher arrives now (earlier it would hold the game back: with no
        # undefeated human seated the room paces itself to its slowest watcher).
        late = spectate(port, h, 'Bob', room, take_map=False)
        pump_all([alice, wendy, late], 0.5)
        assert not late.bundles, 'the log was streamed to a watcher that has not verified the map'
        alice.send('LeaveGame')
        ticks, team = told_over(wendy, [alice, late], 5, 'the watcher was never told the game is over')
        assert team == 1
        whole_game(wendy, ticks)
        # The late watcher is told too, and the room waits for it: over, but not gone.
        assert late.pump(5, game_over), 'a watcher still catching up was not told the game is over'
        assert auth.Reader(late.other[GAME_OVER][0]).num('<I') == ticks
        assert not late.bundles
        assert log_says(root, 'game %d over after %d ticks (match over)' % (room, ticks))
        assert not log_says(root, 'game %d ended' % room, seconds=1), 'the room went before its watcher had the log'
        assert refused_watch(port, h, 'Carol', room) == b'game not found or ended', 'a game that is over took a new watcher'
        # ... and no running game: not listed (a Watch on it is refused), and not held
        # against the server's limit of one running game.
        assert room not in listed(port, h), 'a game that is over is still listed while its watcher catches up'
        assert not [why for why in start_refusals(port, h, 'Carol') if b'capacity' in why], \
            'a game that is over still counts against --max-running-games'
        ready_map(late, room)
        whole_game(late, ticks)
        assert all(late.bundles[t][0] == wendy.bundles[t][0] for t in range(ticks)), 'the two watchers were sent different games'
        assert log_says(root, 'game %d ended (match over)' % room), 'the room stayed after its last watcher caught up'
    finally:
        alice.close(); wendy.close()
        if late:
            late.close()


def scenario_stuck(port, h, root, packages):
    room, (alice,), _ = start(port, h, ['Alice'], ais=1, file_map=('Inner Circle', packages['Inner Circle']))
    late = None
    try:
        lose(alice, [])
        late = spectate(port, h, 'Bob', room, take_map=False)
        pump_all([alice, late], 0.5)
        alice.send('LeaveGame')
        began = time.monotonic()
        ticks, team = told_over(late, [alice], 5, 'the watcher was never told the game is over')
        assert team == 1 and not late.bundles
        # It keeps its connection alive (pump pings) but never takes the map.
        late.pump(DRAIN_MS / 1000 - 2)
        assert not log_says(root, 'game %d ended' % room, seconds=0.2), 'the room did not wait for its watcher at all'
        assert pump_all([late], 6, lambda: 'game %d ended (match over)' % room in (root / 'log').read_text(errors='replace')), \
            'a watcher that never takes the map held the ended game\'s room'
        waited = time.monotonic() - began
        assert DRAIN_MS / 1000 - 0.5 <= waited <= DRAIN_MS / 1000 + 3, ('the room did not go after the drain time', waited)
        assert log_says(root, '(%d ticks, ' % ticks), 'the replay of the ended game was not written'
        assert not late.bundles and not late.closed, 'the watcher was sent the log, or dropped'
    finally:
        alice.close()
        if late:
            late.close()


SCENARIOS = [scenario_seated_then_leave, scenario_takeover, scenario_abandoned, scenario_walkover,
             scenario_unwatched, scenario_catchup, scenario_stuck]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    ap.add_argument('--map-tool', type=Path)
    a = ap.parse_args()
    os.environ['TA_STALL_MS'], os.environ['TA_DROP_WAIT_MS'] = str(STALL_MS), str(WAIT_MS)
    os.environ['TA_OVER_DRAIN_MS'] = str(DRAIN_MS)
    failed = []
    with tempfile.TemporaryDirectory(prefix='ta-endgame-') as tmp:
        root = Path(tmp)
        (root / 'replays').mkdir()
        packages = {}
        if a.map_tool:
            out = root / 'Inner-Circle.tamap'
            subprocess.run([str(a.map_tool.resolve()), '--export-package', str(a.data.resolve()), str(out), 'Inner Circle'],
                           check=True)
            packages['Inner Circle'] = out.read_bytes()
        # One running game at a time: every scenario's game has ended before the next starts.
        with server(a.server.resolve(), a.data.resolve(), root, '--replaydir', str(root / 'replays'),
                    '--max-running-games', '1') as (port, h, process):
            for scenario in SCENARIOS:
                name = scenario.__name__[len('scenario_'):].replace('seated_then_leave', 'leave')
                if scenario in (scenario_catchup, scenario_stuck) and not packages:
                    print('skip ' + name + ' (no --map-tool)')
                    continue
                try:
                    scenario(port, h, root, packages)
                    print('ok   ' + name)
                except Exception:
                    failed.append(name)
                    print('FAIL ' + name)
                    traceback.print_exc()
            assert process.poll() is None, 'server exited'
        if failed:
            print((root / 'log').read_text(errors='replace')[-5000:])
    if failed:
        raise SystemExit('FAIL: ' + ', '.join(failed))
    print('PASS: game end -- a seated human\'s room plays on after the result; with no human seated the server '
          'ends the match on the result\'s tick, tells the watchers the length and the winner, and its replay '
          'ends there; a taken-over seat is not a human\'s; abandoned and walked-over games tell their watcher; '
          'a game nobody is connected to is torn down; a watcher still catching up gets the whole log first, '
          'from a room that is no longer listed or counted as a running game, and one that never takes the map '
          'does not hold the room')


if __name__ == '__main__':
    main()
