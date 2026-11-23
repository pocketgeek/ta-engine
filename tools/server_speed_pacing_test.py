#!/usr/bin/env python3
"""In-game speed changes on a real server: the tick rate follows the speed both ways.

An all-AI game watched by its host (the spectator acknowledges ticks, as the client does)
is run at 1x, raised to 2x (retail's top speed, +10) and dropped back to 1x, timing the TickBundles as they arrive.
Two regressions are covered. A Windows server slept a whole timer quantum (~15.6 ms) in
poll() for every tick it had in flight, so it never got far past 70 ticks/s: raising the
speed sped up the clients' animation but not the game. And the deficit that server ran up
at 8x was then paid back at 1x, as periodic "catching up" bursts.
"""
import argparse
import contextlib
from pathlib import Path
import socket
import struct
import tempfile
import time
import server_net as auth
from server_public_test import server, login


def options(speed):
    # crusades, forfeitSelfDestruct, gameMods, speed (tenths), speedUnlock, unitCap,
    # monarchExpendable, stressTest, fogExplored, benchmark, randomStarts, doubleSight, pathMode,
    # allowSpectators, mapScenario, powerCodes
    return bytes([0, 1, 0, speed, 1]) + struct.pack('<I', 2000) + bytes([0, 0, 0, 0, 0, 0, 0, 1, 0, 0])


class Watcher:
    """Reads frames as they arrive, timestamps TickBundles and acknowledges every 30th tick."""

    def __init__(self, peer):
        self.peer, self.ticks, self.buffer = peer, [], b''
        peer.socket.setblocking(False)

    def pump(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                chunk = self.peer.socket.recv(1 << 16)
                if not chunk:
                    raise RuntimeError('server closed the connection')
                self.buffer += chunk
            except (BlockingIOError, socket.timeout):
                time.sleep(0.0005)
                continue
            now = time.monotonic()
            while len(self.buffer) >= 4:
                size = struct.unpack_from('<I', self.buffer)[0]
                if len(self.buffer) < 4 + size:
                    break
                body, self.buffer = self.buffer[4:4 + size], self.buffer[4 + size:]
                if body[0] == auth.MSG['Ping']:
                    self.send('Pong')
                elif body[0] == auth.MSG['TickBundle']:
                    tick = struct.unpack_from('<I', body, 1)[0]
                    self.ticks.append((now, tick))
                    if tick % 30 == 0:
                        self.send('StateHash', struct.pack('<IQ', tick, 0))

    def send(self, kind, payload=b''):
        frame = struct.pack('<IB', len(payload) + 1, auth.MSG[kind]) + payload
        self.peer.socket.setblocking(True)
        self.peer.socket.sendall(frame)
        self.peer.socket.setblocking(False)

    def rate(self, start, end):
        n = sum(1 for t, _ in self.ticks if start <= t < end)
        return n / (end - start)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-speed-pacing-') as tmp:
        root = Path(tmp)
        with server(a.server.resolve(), a.data.resolve(), root) as (port, h, process):
            with contextlib.closing(login(port, h, 'Host')) as p:
                recipe = '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) + bytes([2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()
                # max players 8, host spectates (takes no slot), private
                p.send('CreateGame', auth.field('pacing') + auth.field('') + auth.field(recipe) + auth.field('') +
                       options(10) + bytes([8, 1, 1]))
                r = p.receive('JoinResult')
                assert r.num('<B') == 1, 'create failed'
                p.receive('LobbyState')
                for slot in (0, 1):
                    auth.slot_update(p, slot, 2, slot, slot, slot, 1, 1)
                p.send('StartGame')
                p.receive('GameStarting')
                p.send('Loaded')
                w = Watcher(p)
                w.pump(3.0)
                assert w.ticks, 'the game never ticked'
                t0 = time.monotonic()
                w.send('SetGameOptions', options(20))   # +10 = 2x, retail's top speed
                w.pump(3.0)
                t1 = time.monotonic()
                w.send('SetGameOptions', options(10))
                w.pump(4.0)
                t2 = time.monotonic()
                assert process.poll() is None, 'server exited'
                slow = w.rate(t0 - 2.0, t0)
                fast = w.rate(t0 + 0.5, t1)
                back = w.rate(t1 + 0.5, t2)
                windows = [w.rate(s, s + 1.0) for s in (t1 + 0.5, t1 + 1.5, t1 + 2.5)]
                print(f'1x {slow:.1f}/s, 2x {fast:.1f}/s, back to 1x {back:.1f}/s '
                      f'(per second: {", ".join(f"{x:.0f}" for x in windows)})')
                failures = []
                if not 25 <= slow <= 35:
                    failures.append(f'1x ran {slow:.1f} ticks/s, want 30')
                if fast < 50:
                    failures.append(f'2x ran {fast:.1f} ticks/s, want 60 (the speed change did not speed the game up)')
                if not 25 <= back <= 35 or max(windows) > 40:
                    failures.append(f'1x after 2x ran {back:.1f} ticks/s, peak {max(windows):.0f} in one second, '
                                    'want 30 with no catch-up burst')
                assert not failures, '; '.join(failures)
    print('PASS in-game speed changes re-pace the server: 1x -> 2x -> 1x')


if __name__ == '__main__':
    main()
