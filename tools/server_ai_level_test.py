#!/usr/bin/env python3
"""AI levels through a real server's lobby.

  classic     the host seats an AI at level 6 (RETAIL, internally Classic): the room's table shows 6
  every level each of 0..7 round-trips through SlotUpdate -> LobbyState
  unknown     level 8 or more is stored as Normal (2)
"""
import argparse
import contextlib
from pathlib import Path
import struct
import tempfile

import server_net as auth
from server_net import lobby
from server_public_test import login, options, server

LEVELS = 8      # ai::kAiLevels
CLASSIC = 6


def recipe():
    return '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) + bytes([2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()


def level(host, value):
    """Seat slot 2 as an AI (Verdana, colour 1, team 1) at `value`; the level the room shows."""
    auth.slot_update(host, 1, 2, 2, 1, 1, 1, value)
    slots = lobby(host.receive('LobbyState'))[3]
    assert slots[1][0][0] == 2, ('slot 2 is not an AI', slots[1])
    return slots[1][0][5]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-ai-level-') as tmp:
        with server(a.server.resolve(), a.data.resolve(), Path(tmp)) as (port, h, process), \
             contextlib.closing(login(port, h, 'Alice')) as host:
            host.send('CreateGame', auth.field('levels') + auth.field('') + auth.field(recipe()) +
                      auth.field('') + options() + bytes([4, 0, 0]))
            r = host.receive('JoinResult')
            assert r.num('<B') == 1, 'CreateGame refused'
            lobby(host.receive('LobbyState'))
            assert level(host, CLASSIC) == CLASSIC, 'the room did not keep level 6 (RETAIL)'
            for value in range(LEVELS):
                assert level(host, value) == value, ('level did not round-trip', value)
            for value in (LEVELS, 200, 255):
                assert level(host, value) == 2, ('an unknown level was not stored as Normal', value)
            assert level(host, CLASSIC) == CLASSIC
            assert process.poll() is None, 'server exited'
    print('PASS: AI levels -- RETAIL (6, Classic) and every level round-trip the lobby; 8 and up store Normal')


if __name__ == '__main__':
    main()
