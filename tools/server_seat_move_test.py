#!/usr/bin/env python3
"""Msg::MoveSeat on a real server: a seated player takes another OPEN seat.

Under START LOCATIONS: BY SLOT the seat is the start position (and under FIXED the
seat order decides it), so this is how a human picks where they open. Checked:

  host move     the host leaves slot 1 for slot 3: JoinResult names the new seat, the
                table shows it there with its side and colour, and slot 1 is open
  joiner        a second client joins into the opened seat and sees the host's move
  refused       an occupied, AI, closed, past-capacity, out-of-range or own seat is
                ignored -- no JoinResult, no table, for either client
  player move   a non-host moves; the host's table shows the old seat open
  stale edits   a host edit sent before a move landed -- READY on the seat it left,
                or a type click on a seat a player just took -- is dropped
  running       once the game runs a move is ignored
"""
import argparse
import contextlib
from pathlib import Path
import struct
import tempfile

import server_net as auth
from server_net import lobby
from server_public_test import login, options, server

CAP = 4


def recipe():
    # CAP players: the server opens a random map's seats from its player count.
    return '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) + bytes([CAP, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()


def joined(peer):
    r = peer.receive('JoinResult')
    assert r.num('<B') == 1, 'JoinResult refused'
    return r.num('<B')


def table(peer):
    return lobby(peer.receive('LobbyState'))[3]


def quiet(peer, what):
    """Ping and read to the Pong: a move that was ignored sent nothing before it."""
    peer.send('Ping')
    while True:
        size = struct.unpack('<I', peer.exact(4))[0]
        body = peer.exact(size)
        if body[0] == auth.MSG['Ping']:
            peer.send('Pong')
            continue
        if body[0] == auth.MSG['Pong']:
            return
        assert body[0] not in (auth.MSG['JoinResult'], auth.MSG['LobbyState']), what + ': the server took the move'


def move(peer, slot):
    peer.send('MoveSeat', bytes([slot]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-seat-move-') as tmp:
        with server(a.server.resolve(), a.data.resolve(), Path(tmp)) as (port, h, process), \
             contextlib.closing(login(port, h, 'Alice')) as host, \
             contextlib.closing(login(port, h, 'Bobby')) as player:
            host.send('CreateGame', auth.field('seats') + auth.field('') + auth.field(recipe()) +
                      auth.field('') + options(random_starts=2) + bytes([CAP, 0, 0]))
            assert joined(host) == 0
            room = lobby(host.receive('LobbyState'))[0]

            # host move: take slot 3 with a chosen side (Zhon) and colour 0.
            auth.slot_update(host, 0, 1, 3, 0, 0, 0, 2)
            table(host)
            move(host, 2)
            assert joined(host) == 2, 'the host was not told its new seat'
            slots = table(host)
            assert slots[2][0][:3] == (1, 3, 0) and slots[2][1] == b'Alice', ('the host is not in slot 3', slots[2])
            assert slots[0][0][0] == 0 and slots[0][1] == b'', ('slot 1 did not open', slots[0])
            assert slots[0][0][2] == 2 and slots[0][0][3] == 2, 'the opened seat did not take the old colour/team'

            # stale host edit: READY on the row it just left, sent before the move
            # reached it, must not make that open seat a clientless HUMAN.
            auth.slot_update(host, 0, 1, 3, 0, 0, 1, 2)
            quiet(host, 'a stale edit of the left seat')

            # joiner: the first open seat is the one the host left.
            player.send('JoinGame', struct.pack('<I', room) + auth.field(''))
            assert joined(player) == 0
            seen = table(player)
            assert seen[2][1] == b'Alice' and seen[0][1] == b'Bobby', 'the joiner does not see the move'
            table(host)

            # refused: occupied, AI, closed, past capacity, out of range, own seat.
            auth.slot_update(host, 3, 2, 1, 3, 3, 1, 2)   # slot 4: AI
            auth.slot_update(host, 1, 3, 0, 1, 1, 0, 2)   # slot 2: closed
            for _ in range(2):
                table(host); table(player)
            for slot, what in ((2, 'occupied'), (3, 'AI'), (1, 'closed'), (CAP, 'past capacity'),
                               (200, 'out of range'), (0, 'own seat')):
                move(player, slot)
                quiet(player, what)
            quiet(host, 'refused moves')
            player.send('MoveSeat', b'')        # malformed payloads too
            player.send('MoveSeat', bytes([1, 1]))
            quiet(player, 'malformed')

            # player move: reopen slot 2 and take it; the host sees slot 1 open.
            auth.slot_update(host, 1, 0, 0, 1, 1, 0, 2)
            table(host); table(player)
            move(player, 1)
            assert joined(player) == 1
            mine = table(player)
            assert mine[1][1] == b'Bobby' and mine[0][0][0] == 0
            slots = table(host)
            assert slots[1][0][0] == 1 and slots[1][1] == b'Bobby', ('the host does not see the move', slots[1])
            assert slots[0][0][0] == 0 and slots[0][1] == b'', 'the player\'s old seat did not open'

            # stale host type click: the host's table still showed slot 2 open; AI,
            # CLOSED or OPEN on it must not replace the player who just took it.
            for t in (2, 3, 0):
                auth.slot_update(host, 1, t, 0, 1, 1, 0, 2)
            quiet(host, 'a type change of a seated slot')
            quiet(player, 'a type change of a seated slot')

            # running: no moves once the game starts.
            v = slots[2][0]
            auth.slot_update(host, 2, 1, v[1], v[2], v[3], 1, v[5])
            v = slots[1][0]
            auth.slot_update(player, 1, 1, v[1], v[2], v[3], 1, v[5])
            for _ in range(2):
                table(host); table(player)
            host.send('StartGame')
            r = player.receive('GameStarting')
            lobby(r)
            assert r.num('<B') == 1, 'the player did not start in its moved seat'
            r = host.receive('GameStarting')
            lobby(r)
            assert r.num('<B') == 2, 'the host did not start in its moved seat'
            move(player, 0)
            quiet(player, 'a running game')
            assert process.poll() is None, 'server exited'
    print('PASS: seat moves -- host and player moves, joiner sees them, refused seats, '
          'malformed payloads, stale host edits dropped, none once running')


if __name__ == '__main__':
    main()
