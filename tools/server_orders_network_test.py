#!/usr/bin/env python3
"""Selection-order transactions (protocol 267) through a real local server.

A whole transaction (Begin, members in two chunks, Commit) activates as ONE Cmd::Order
record in one tick bundle with its exact members; an ordinary command sent right behind
it lands in the same tick after it (the order takes no command credit, and nothing
overtakes it); an aborted transaction never reaches a bundle; a burst of orders past the
per-client window is cut (logged) and the rest activate in serial order, and the server names
every cut order to its sender (Msg::InputDropped -- an order that never comes back in a bundle
must still leave the client's send window, review R1), as it counts every dropped ordinary
command; a malformed
transaction (a duplicate member) fails that connection only, and the server keeps
serving the other account.
"""
import argparse
import contextlib
from pathlib import Path
import struct
import tempfile
import server_net as auth
from server_net import lobby
from server_public_test import server, login


def options(path=4):
    # GameOptions wire bytes (server_public_test.options), pathfinding byte `path`: 4 = Legion, 0 = Retail.
    return bytes([0, 1, 0, 10, 0]) + struct.pack('<I', 2000) + bytes([0, 0, 0, 0, 0, 0, path, 1, 0, 0])

CMD = dict(Move=0, Stop=4, ReclaimArea=23, BuildManaArea=26, Order=27)


def varint(v):
    out = b''
    while v >= 0x80:
        out += bytes([(v & 0x7f) | 0x80]); v >>= 7
    return out + bytes([v])


def command(kind, unit=0, x=0.0, z=0.0, target=0, queue=0, player=0):
    return struct.pack('<BBiiffB16s', kind, player, unit, target, x, z, queue, b'')


def members(ids, prev=-1):
    out = struct.pack('<I', len(ids))
    for i in ids:
        out += varint(i - prev); prev = i
    return out, prev


def digest(ids):
    h = 1469598103934665603
    for i in ids:
        for k in range(4):
            h ^= (i >> (8 * k)) & 0xff
            h = (h * 1099511628211) & 0xffffffffffffffff
    return h


def frame(kind, payload):
    return struct.pack('<IB', len(payload) + 1, auth.MSG[kind]) + payload


def transaction(serial, ids, tmpl, shape=0, chunk=2048, abort_after=None):
    out = frame('OrderBegin', struct.pack('<I', serial) + tmpl + bytes([shape]) + struct.pack('<I', len(ids)))
    prev = -1
    for at in range(0, len(ids), chunk):
        if abort_after is not None and at >= abort_after:
            return out + frame('OrderAbort', struct.pack('<I', serial))
        body, prev = members(ids[at:at + chunk], prev)
        out += frame('OrderMembers', struct.pack('<I', serial) + body)
    return out + frame('OrderCommit', struct.pack('<IIQ', serial, len(ids), digest(ids)))


def read_varint(data, pos):
    v, shift = 0, 0
    while True:
        b = data[pos]; pos += 1
        v |= (b & 0x7f) << shift; shift += 7
        if not b & 0x80:
            return v, pos


def bundle(r):
    """One TickBundle: (tick, [('order', player, serial, verb, ids) | ('cmd', player, kind, unit)])."""
    data, pos = r.data, r.pos
    tick, count = struct.unpack_from('<II', data, pos); pos += 8
    out = []
    for _ in range(count):
        kind, player = data[pos], data[pos + 1]
        if kind == CMD['Order']:
            serial, = struct.unpack_from('<I', data, pos + 2); pos += 6
            verb = data[pos]; pos += 35
            pos += 1
            n, = struct.unpack_from('<I', data, pos); pos += 4
            ids, prev = [], -1
            for _ in range(n):
                d, pos = read_varint(data, pos); prev += d; ids.append(prev)
            out.append(('order', player, serial, verb, ids))
        else:
            unit, = struct.unpack_from('<i', data, pos + 2)
            pos += 43 if kind in (CMD['ReclaimArea'], CMD['BuildManaArea']) else 35
            out.append(('cmd', player, kind, unit))
    return tick, out


DROPS = []   # (commands, serial) of every InputDropped seen, in order


def next_bundle(p):
    """The next TickBundle, skipping every other message (statuses, pings; InputDropped is kept in DROPS)."""
    while True:
        length = struct.unpack('<I', p.exact(4))[0]
        body = p.exact(length)
        if body[0] == auth.MSG['Ping']:
            p.send('Pong'); continue
        if body[0] == auth.MSG['InputDropped']:
            DROPS.append(struct.unpack_from('<II', body, 1)); continue
        if body[0] == auth.MSG['TickBundle']:
            return bundle(auth.Reader(body[1:]))


def until(p, want, ticks=300):
    """Bundles until want(items_so_far) holds; returns [(tick, items)]."""
    seen = []
    for _ in range(ticks):
        seen.append(next_bundle(p))
        if want(seen):
            return seen
    raise AssertionError('not seen within %d ticks: %s' % (ticks, [s for s in seen if s[1]][:5]))


def mine(seen):
    return [(t, it) for t, items in seen for it in items if it[1] == 0]


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True); a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-orders-') as tmp:
        root = Path(tmp)
        with server(a.server.resolve(), a.data.resolve(), root) as (port, h, process):
            with contextlib.closing(login(port, h, 'Orders')) as p, contextlib.closing(login(port, h, 'Observer')) as observer:
                recipe = '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) + bytes([2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()
                p.send('CreateGame', auth.field('orders') + auth.field('') + auth.field(recipe) + auth.field('') + options() + bytes([2, 0, 0]))
                p.receive('JoinResult'); values = lobby(p.receive('LobbyState'))[3][0][0]
                auth.slot_update(p, 0, 1, values[1], values[2], values[3], 1, values[5])
                auth.slot_update(p, 1, 2, 1, 1, 1, 1, 0)
                p.send('StartGame'); p.receive('GameStarting'); p.send('Loaded')

                # 1. A whole order and an ordinary command right behind it: one tick, in that order.
                ids = list(range(5, 3005))
                move = command(CMD['Move'], x=1200.0, z=900.0)
                p.socket.sendall(transaction(1, ids, move, shape=1) +
                                 frame('PlayerCommands', struct.pack('<I', 1) + command(CMD['Stop'], unit=777001)))
                seen = until(p, lambda s: any(it[0] == 'cmd' and it[3] == 777001 for _, it in mine(s)))
                got = mine(seen)
                order = [(t, it) for t, it in got if it[0] == 'order' and it[2] == 1]
                cmd = [(t, it) for t, it in got if it[0] == 'cmd' and it[3] == 777001]
                assert len(order) == 1 and order[0][1][4] == ids and order[0][1][3] == CMD['Move'], order[:1]
                assert cmd[0][0] == order[0][0], 'the command behind the order waited %d ticks' % (cmd[0][0] - order[0][0])
                assert got.index(cmd[0]) > got.index(order[0]), 'a command overtook the order sent before it'

                # 2. An aborted transaction never activates; the next serial does.
                p.socket.sendall(transaction(2, list(range(0, 5000)), move, abort_after=2048) +
                                 transaction(3, [11, 12, 13], command(CMD['Stop'])))
                seen = until(p, lambda s: any(it[0] == 'order' and it[2] == 3 for _, it in mine(s)))
                serials = [it[2] for _, it in mine(seen) if it[0] == 'order']
                assert serials == [3], serials

                # 3. A burst past the per-client window (64 orders): some are cut and logged, the rest
                # activate in serial order.
                burst = b''.join(transaction(s, [s], command(CMD['Stop'])) for s in range(10, 210))
                p.socket.sendall(burst)
                seen = until(p, lambda s: len([1 for _, it in mine(s) if it[0] == 'order']) >= 64, ticks=60)
                for _ in range(20):
                    seen.append(next_bundle(p))
                serials = [it[2] for _, it in mine(seen) if it[0] == 'order']
                assert serials == sorted(serials) and 64 <= len(serials) < 200, (len(serials), serials[:5])
                assert serials[:64] == list(range(10, 74)), serials[:70]
                text = (root / 'log').read_text()
                assert 'order budget/queue exceeded' in text, 'the cut was not logged'
                # Every cut order was named to its sender, once, and no activated one was.
                cut = sorted(s for c, s in DROPS if s)
                assert cut == sorted(set(range(10, 210)) - set(serials)), ('drop notices', cut[:5], len(cut), len(serials))
                assert all(c == 0 for c, s in DROPS), DROPS[:5]
                del DROPS[:]

                # 3b. Ordinary commands past the queue (512) are dropped whole, and counted to the sender.
                p.socket.sendall(frame('PlayerCommands', struct.pack('<I', 600) + command(CMD['Stop'], unit=5) * 600))
                for _ in range(30):
                    if DROPS:
                        break
                    next_bundle(p)
                assert DROPS == [(600, 0)], DROPS
                del DROPS[:]

                # 4. The per-second member budget (65,536 a client) delays, never drops: seven orders of 10,000
                # members sent at once all activate, in serial order, the last once the budget's second turns.
                big = list(range(0, 20000, 2))
                p.socket.sendall(b''.join(transaction(s, big, move) for s in range(250, 257)))
                seen = until(p, lambda s: any(it[0] == 'order' and it[2] == 256 for _, it in mine(s)), ticks=400)
                got = [(t, it[2]) for t, it in mine(seen) if it[0] == 'order']
                assert [g for _, g in got] == list(range(250, 257)), got
                # 70,000 members cannot all fit one budget second (65,536): at least the last waits for it to turn.
                assert got[-1][0] > got[0][0], 'the member budget did not delay the last order: %s' % got

                # 5. A malformed transaction (a duplicate member: a zero delta) fails this connection only.
                bad = frame('OrderBegin', struct.pack('<I', 300) + move + bytes([0]) + struct.pack('<I', 2))
                bad += frame('OrderMembers', struct.pack('<II', 300, 2) + varint(6) + varint(0))
                p.socket.sendall(bad)
                closed = False
                try:
                    for _ in range(400):
                        next_bundle(p)
                except (RuntimeError, AssertionError, OSError):
                    closed = True
                assert closed, 'a malformed transaction did not fail its connection'
                observer.send('ListGames'); assert observer.receive('GameList').num('<I') == 1

                # 6. A Retail game refuses orders: its clients keep the per-unit commands.
                observer.send('CreateGame', auth.field('retail') + auth.field('') + auth.field(recipe) + auth.field('') + options(0) + bytes([2, 0, 0]))
                observer.receive('JoinResult'); values = lobby(observer.receive('LobbyState'))[3][0][0]
                auth.slot_update(observer, 0, 1, values[1], values[2], values[3], 1, values[5])
                auth.slot_update(observer, 1, 2, 1, 1, 1, 1, 0)
                observer.send('StartGame'); observer.receive('GameStarting'); observer.send('Loaded')
                observer.socket.sendall(transaction(1, [1, 2, 3], move))
                closed = False
                try:
                    for _ in range(400):
                        next_bundle(observer)
                except (RuntimeError, AssertionError, OSError):
                    closed = True
                assert closed, 'a Retail game accepted an order transaction'
                assert process.poll() is None
    print('PASS selection orders through a local server: whole activation, FIFO without starvation, abort, window, '
          'drop notices, member budget delays, malformed, Retail refuses')


if __name__ == '__main__':
    main()
