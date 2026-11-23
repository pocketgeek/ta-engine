#!/usr/bin/env python3
"""POWER CODES on a local server: the room option and Cmd::PowerCode.

  room        the option travels in LobbyState, and the host may change it in a skirmish lobby
              (SetGameOptions, protocol 323; a mission room keeps it off: server_room_edit_test)
  off         a power code sent while the room has POWER CODES off never reaches a tick bundle,
              and the connection stays up (the chat line carried it; it is not a fault)
  on          the same command is sequenced, owned by the sender's slot
  forged      a code the client never sends (Radar, a client-side code) or an unknown argument
              fails the connection as invalid player commands
"""
import argparse
from pathlib import Path
import struct
import tempfile
import server_net as auth
from server_net import OPTIONS_BYTES, lobby
from server_public_test import server, login, options

POWER = OPTIONS_BYTES - 1       # powerCodes, the last GameOptions byte
CMD_MOVE, CMD_POWER = 0, 28     # net::Cmd::Move, net::Cmd::PowerCode (after Order = 27)
DOUBLE_SHOT, RADAR = 3, 8       # sim::PowerCode
MARKER = 123456                 # a Move for a unit nobody owns: shows the batch was sequenced


def command(kind, unit=0, target=0, queue=0):
    return struct.pack('<BBiiffB16s', kind, 7, unit, target, 0, 0, queue, b'')


def recipe():
    return '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) + bytes([2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()


def start(p, power):
    p.send('CreateGame', auth.field('codes') + auth.field('') + auth.field(recipe()) + auth.field('') +
           options(power_codes=power) + bytes([2, 0, 0]))
    p.receive('JoinResult')
    state = lobby(p.receive('LobbyState'))
    assert state[2][POWER] == power, ('the room did not keep POWER CODES', power, state[2])
    # The host may change it in a skirmish lobby, and back.
    p.send('SetGameOptions', options(power_codes=1 - power))
    state = lobby(p.receive('LobbyState'))
    assert state[2][POWER] == 1 - power, 'SetGameOptions did not change POWER CODES in the lobby'
    p.send('SetGameOptions', options(power_codes=power))
    state = lobby(p.receive('LobbyState'))
    assert state[2][POWER] == power, 'SetGameOptions did not change POWER CODES back'
    values = state[3][0][0]
    auth.slot_update(p, 0, 1, values[1], values[2], values[3], 1, values[5])
    auth.slot_update(p, 1, 2, 1, 1, 1, 1, 0)
    p.send('StartGame'); p.receive('GameStarting'); p.send('Loaded')


def sequenced(p):
    """Send a power code then the marker; return the power codes the bundles carried."""
    p.send('PlayerCommands', struct.pack('<I', 2) + command(CMD_POWER, target=DOUBLE_SHOT) + command(CMD_MOVE, unit=MARKER))
    seen = []
    for _ in range(300):
        tick = p.receive('TickBundle'); tick.num('<I'); count = tick.num('<I')
        for _ in range(count):
            kind, owner, unit, target = struct.unpack_from('<BBii', tick.data, tick.pos)
            tick.pos += 43 if kind in (23, 26) else 35
            if kind == CMD_POWER:
                seen.append((owner, target))
            if kind == CMD_MOVE and unit == MARKER:
                return seen
    raise AssertionError('the marker command never reached a tick')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True); ap.add_argument('--data', type=Path, required=True)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-power-codes-') as tmp:
        root = Path(tmp)
        try:
            run(a, root)
        except BaseException:
            print((root / 'log').read_text()[-3000:])
            raise
    print('PASS POWER CODES: room option set in the lobby, codes dropped while off, sequenced while on, forgeries refused')


def run(a, root):
    if True:
        with server(a.server.resolve(), a.data.resolve(), root) as (port, h, process):
            # Two accounts, logged in up front: the server paces new registrations per
            # address. Each forgery is a fresh login of 'Open' (an existing account).
            peers = {n: login(port, h, n) for n in ('Closed', 'Open')}
            try:
                p = peers['Closed']
                start(p, 0)
                assert sequenced(p) == [], 'a power code reached the bundle with POWER CODES off'
                p.send('Ping'); p.receive('Pong')   # still connected: refused, not a fault
                p = peers['Open']
                start(p, 1)
                assert sequenced(p) == [(0, DOUBLE_SHOT)], 'the power code was not sequenced for its sender'
                p.send('Ping'); p.receive('Pong')
                forgeries = {'Radar': command(CMD_POWER, target=RADAR), 'Argument': command(CMD_POWER, target=5, queue=9),
                             'Zero': command(CMD_POWER, target=0)}
                for name, forged in forgeries.items():
                    peers['Open'].socket.close()
                    p = peers['Open'] = login(port, h, 'Open')
                    start(p, 1)
                    p.send('PlayerCommands', struct.pack('<I', 1) + forged)
                    p.socket.settimeout(10)
                    closed = False
                    try:
                        p.receive('Pong')   # never sent a Ping: returns only by the connection closing
                    except TimeoutError:
                        pass
                    except (ConnectionError, EOFError, AssertionError, OSError, RuntimeError):
                        closed = True
                    assert closed, 'a forged power code was accepted: ' + name
                # The same room and command path, with a valid code, keeps the connection
                # (checked above): the close is the server refusing the forgery.
            finally:
                for peer in peers.values():
                    peer.socket.close()
            assert process.poll() is None


if __name__ == '__main__':
    main()
