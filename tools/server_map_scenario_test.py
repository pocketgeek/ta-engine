#!/usr/bin/env python3
"""MAP SCENARIO on a real server: the room option, enforced against the verified map.

Each scenario runs on its own and every failure is reported, so a server without one
of the checks shows exactly which it lacks:

  refused     ON for a map with no scenario (Inner Circle): START is refused with the
              reason, and the server log says so
  seats       ON for King of the Hill: the room forces fixed starts; START is refused
              until every seat the scenario's placements and rules belong to is played
              (setupMatch's own message), then the game starts with the option on
  incomplete  ON for King of the Hill with a package that leaves out its Use Only .tdf:
              START is refused, since setupMatch would read each machine's own copy,
              which the package digest does not cover
  locked      the option changes only with the map (Msg::SetRoomMap, protocol 323):
              SetGameOptions cannot turn it on or off
  sanitised   the server clears it where a map scenario cannot exist: a generated map,
              a stress test, a benchmark and a campaign mission, including a room the
              host switches to a stress test after creating it
  off         OFF on King of the Hill is an ordinary skirmish: two players to start

The rooms run on an authenticated server, i.e. a public one, so the option is proved
allowed there; the stress/benchmark/mission rooms need a local test server.
"""
import argparse
import contextlib
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import traceback

import server_net as auth
from server_net import OPTIONS_BYTES, BufferedPeer as Peer, lobby
from server_public_test import login, options, server

SCENARIO = OPTIONS_BYTES - 2   # mapScenario, before powerCodes
RANDOM = 13                    # randomStarts
SEAT_TWO = b'Seat player 2 in the lobby to play this scenario'


def recipe():
    return '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) + bytes([2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()


def create(peer, map_id, scenario, capacity=4, random_starts=0, stress=0, benchmark=0, mission=''):
    peer.send('CreateGame', auth.field('scenario') + auth.field('') + auth.field(map_id) + auth.field(mission) +
              options(stress=stress, benchmark=benchmark, scenario=scenario, random_starts=random_starts) +
              bytes([capacity, 0, 0]))
    result = peer.receive('JoinResult')
    assert result.num('<B') == 1, 'CreateGame refused'
    result.num('<B'); result.field()
    return lobby(peer.receive('LobbyState'))


def settle(peer):
    """A wire-order barrier: everything the server sent before the Pong is now queued."""
    peer.send('Ping')
    peer.receive('Pong')


def latest(peer):
    settle(peer)
    queued = peer.pending[auth.MSG['LobbyState']]
    assert queued, 'no LobbyState'
    state = None
    while queued:
        state = lobby(auth.Reader(queued.popleft()))
    return state


def upload(peer, room, map_id, package):
    """Offer the host's package; send it if the server has no copy, then verify it."""
    digest = hashlib.sha256(package).hexdigest()
    peer.send('MapOffer', struct.pack('<I', room) + auth.field(map_id) + auth.field(digest) +
              struct.pack('<I', len(package)))
    want = {auth.MSG['MapRequest'], auth.MSG['MapOffer'], auth.MSG['MapError']}
    while True:
        size = struct.unpack('<I', peer.exact(4))[0]
        message = peer.exact(size)
        if message[0] == auth.MSG['Ping']:
            peer.send('Pong'); continue
        if message[0] not in want:
            peer.pending[message[0]].append(message[1:]); continue
        break
    r = auth.Reader(message[1:])
    assert message[0] != auth.MSG['MapError'], ('map refused', r.num('<I'), r.field())
    if message[0] == auth.MSG['MapRequest']:
        assert r.num('<I') == room
        assert r.field() == digest.encode(), 'the server asked for another package'
        xfer = r.num('<I')   # the server's transfer id: every chunk carries it
        for offset in range(0, len(package), 65536):
            peer.send('MapChunk', struct.pack('<III', room, xfer, offset) + package[offset:offset + 65536])
        r = peer.receive('MapOffer')
    assert r.num('<I') == room
    r.field()
    assert r.field() == digest.encode(), 'server verified a different package'
    peer.send('MapReady', struct.pack('<I', room) + auth.field(digest))


def ready_host(peer, state):
    values = state[3][0][0]
    auth.slot_update(peer, 0, 1, values[1], values[2], values[3], 1, values[5])


def seat_ai(peer, slot):
    auth.slot_update(peer, slot, 2, slot % 5, slot, slot, 1, 2)


def start(peer):
    """START: ('starting', options), ('refused', reason), or ('nothing', None) when the
    server ignored it (a Ping/Pong barrier proves nothing else was coming)."""
    peer.send('StartGame')
    peer.send('Ping')
    want = {auth.MSG['GameStarting'], auth.MSG['MapError'], auth.MSG['Pong']}
    outcome = ('nothing', None)
    while True:   # the server answers the START before the Pong; read through to it
        size = struct.unpack('<I', peer.exact(4))[0]
        message = peer.exact(size)
        if message[0] == auth.MSG['Ping']:
            peer.send('Pong'); continue
        if message[0] not in want:
            peer.pending[message[0]].append(message[1:]); continue
        r = auth.Reader(message[1:])
        if message[0] == auth.MSG['Pong']:
            return outcome
        if outcome[0] != 'nothing':
            continue
        if message[0] == auth.MSG['GameStarting']:
            outcome = ('starting', lobby(r)[2])
        else:
            r.num('<I')
            outcome = ('refused', r.field())


def log_says(root, text):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if text in (root / 'log').read_text():
            return True
        time.sleep(.05)
    return False


def scenario_refused(port, h, packages, root):
    with contextlib.closing(login(port, h, 'Alice')) as host:
        state = create(host, 'Inner Circle', 1)
        assert state[2][SCENARIO] == 1, 'the room did not adopt MAP SCENARIO'
        upload(host, state[0], 'Inner Circle', packages['Inner Circle'])
        ready_host(host, state)
        seat_ai(host, 1)
        latest(host)
        outcome, why = start(host)
        assert outcome == 'refused', ('a map without a scenario started with MAP SCENARIO on', outcome)
        assert b'no scenario' in why, why
        assert log_says(root, 'start refused -- this map has no scenario'), 'the refusal was not logged'


def scenario_seats(port, h, packages, root):
    with contextlib.closing(login(port, h, 'Alice')) as host:
        state = create(host, 'King of the Hill', 1, random_starts=1)
        assert state[2][SCENARIO] == 1, 'the room did not adopt MAP SCENARIO'
        assert state[2][RANDOM] == 0, 'MAP SCENARIO did not force fixed start locations'
        assert [state[3][i][0][0] for i in range(8)] == [1, 0, 0, 0, 3, 3, 3, 3]
        upload(host, state[0], 'King of the Hill', packages['King of the Hill'])
        ready_host(host, state)
        latest(host)
        outcome, why = start(host)
        assert outcome == 'refused', ('the scenario started without the seats it needs', outcome)
        assert why == SEAT_TWO, why
        assert log_says(root, 'start refused -- Seat player 2'), 'the seat refusal was not logged'
        for slot in (1, 2):
            seat_ai(host, slot)
        latest(host)
        outcome, why = start(host)
        assert outcome == 'refused' and why == b'Seat player 4 in the lobby to play this scenario', (outcome, why)
        seat_ai(host, 3)
        latest(host)
        outcome, opts = start(host)
        assert outcome == 'starting', ('the full scenario room did not start', outcome, opts)
        assert opts[SCENARIO] == 1 and opts[RANDOM] == 0, 'the game did not start with the scenario rules'


def scenario_incomplete(port, h, packages, root):
    # Its own account: the server's per-account lobby budget (CreateGame, MapOffer and
    # StartGame cost 16 each) would otherwise run dry over the scenarios' rapid STARTs.
    with contextlib.closing(login(port, h, 'Bob')) as host:
        state = create(host, 'King of the Hill', 1)
        upload(host, state[0], 'King of the Hill', packages['King of the Hill without its .tdf'])
        ready_host(host, state)
        for slot in (1, 2, 3):
            seat_ai(host, slot)
        latest(host)
        outcome, why = start(host)
        assert outcome == 'refused', ('a package without its Use Only list started its scenario', outcome)
        assert why == b'the map package leaves out its Use Only list', why
        assert log_says(root, 'start refused -- the map package leaves out'), 'the refusal was not logged'


def scenario_locked(port, h, packages, root):
    for created in (1, 0):
        with contextlib.closing(login(port, h, 'Alice')) as host:
            state = create(host, 'King of the Hill', created)
            assert state[2][SCENARIO] == created
            host.send('SetGameOptions', options(scenario=1 - created, random_starts=1))
            after = latest(host)
            assert after[2][SCENARIO] == created, 'SetGameOptions changed MAP SCENARIO after creation'
            assert after[2][RANDOM] == (0 if created else 1), 'random starts with MAP SCENARIO on'


def scenario_sanitised(port, h, packages, root):
    with contextlib.closing(login(port, h, 'Alice')) as host:
        state = create(host, recipe(), 1, capacity=2)
        assert state[2][SCENARIO] == 0, 'a generated map kept MAP SCENARIO'


def local_sanitised(binary, data, root):
    """Stress tests, benchmarks and missions are refused on public servers; a local test
    server accepts them, and must still clear MAP SCENARIO for each."""
    with server(binary, data, root, '--no-auth') as (port, h, process):
        for kwargs in ({'stress': 2}, {'benchmark': 3}, {'mission': 'camp01'}):
            with contextlib.closing(Peer(port)) as peer:
                peer.send('Hello', struct.pack('<I', auth.VERSION) + auth.field('map-scenario-test') +
                          struct.pack('<Q', h) + auth.field('Local'))
                peer.receive('Welcome')
                state = create(peer, 'King of the Hill', 1, **kwargs)
                assert state[2][SCENARIO] == 0, ('MAP SCENARIO kept for', kwargs)
        with contextlib.closing(Peer(port)) as peer:
            peer.send('Hello', struct.pack('<I', auth.VERSION) + auth.field('map-scenario-test') +
                      struct.pack('<Q', h) + auth.field('Local'))
            peer.receive('Welcome')
            state = create(peer, 'King of the Hill', 1)
            assert state[2][SCENARIO] == 1, 'the local room did not adopt MAP SCENARIO'
            peer.send('SetGameOptions', options(stress=2, scenario=1))
            after = latest(peer)
            assert after[2][SCENARIO] == 0, 'MAP SCENARIO kept after the host switched the room to a stress test'
        assert process.poll() is None, 'local server exited'


def scenario_off(port, h, packages, root):
    with contextlib.closing(login(port, h, 'Alice')) as host:
        state = create(host, 'King of the Hill', 0)
        upload(host, state[0], 'King of the Hill', packages['King of the Hill'])
        ready_host(host, state)
        latest(host)
        outcome, _ = start(host)
        assert outcome == 'nothing', ('a one-player skirmish started', outcome)
        seat_ai(host, 1)
        latest(host)
        outcome, opts = start(host)
        assert outcome == 'starting', ('a two-player skirmish did not start', outcome, opts)
        assert opts[SCENARIO] == 0


SCENARIOS = [scenario_refused, scenario_seats, scenario_incomplete, scenario_locked, scenario_sanitised, scenario_off]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    ap.add_argument('--map-tool', type=Path, required=True)
    a = ap.parse_args()
    failed = []
    with tempfile.TemporaryDirectory(prefix='tak-map-scenario-') as tmp:
        root = Path(tmp)
        packages = {}
        for name in ('Inner Circle', 'King of the Hill'):
            out = root / (name.replace(' ', '-') + '.takmap')
            subprocess.run([str(a.map_tool.resolve()), '--export-package', str(a.data.resolve()), str(out), name],
                           check=True)
            packages[name] = out.read_bytes()
        out = root / 'King-of-the-Hill-no-tdf.takmap'
        subprocess.run([str(a.map_tool.resolve()), '--export-package', str(a.data.resolve()), str(out),
                        'King of the Hill', '.tdf'], check=True)
        packages['King of the Hill without its .tdf'] = out.read_bytes()
        public = root / 'public'
        public.mkdir()
        with server(a.server.resolve(), a.data.resolve(), public) as (port, h, process):
            for scenario in SCENARIOS:
                name = scenario.__name__[len('scenario_'):]
                try:
                    scenario(port, h, packages, public)
                    print('ok   ' + name)
                except Exception:
                    failed.append(name)
                    print('FAIL ' + name)
                    traceback.print_exc()
            assert process.poll() is None, 'server exited'
        local = root / 'local'
        local.mkdir()
        try:
            local_sanitised(a.server.resolve(), a.data.resolve(), local)
            print('ok   sanitised (local: stress, benchmark, mission)')
        except Exception:
            failed.append('sanitised (local)')
            print('FAIL sanitised (local)')
            traceback.print_exc()
    if failed:
        raise SystemExit('FAIL: ' + ', '.join(failed))
    print('PASS: MAP SCENARIO -- refused without a scenario (logged), fixed starts and required seats before '
          'START, refused for a package missing a scenario file, kept by SetGameOptions, cleared for '
          'generated/stress/benchmark/mission rooms, off is a skirmish')


if __name__ == '__main__':
    main()
