#!/usr/bin/env python3
"""Voice chat through a real local server (protocol 276; spectator rules 316).

Three players and a spectator in one room. Voice frames are opaque to the server, so the
"stream" is synthetic packets; tools/voice_test.cpp covers the codec and the client.

  listen      a connection that never sent VoiceListen 1 receives no voice
  lobby       before the game runs a frame reaches the whole room, whatever the mask
  mask        in a running game the talker's VOICE mask picks the seated recipients,
              exactly as chat's does: one player, everyone (0xff), nobody (0); the
              spectators only with the SPECTATORS flag (316), and a spectator reaches no
              player at all; nobody hears themselves
  names       the talker's name rides on START frames (and every 25th), its slot always
  budget      a burst past the per-second budget is cut and logged, the END marker still
              arrives, and the game's tick bundles keep coming throughout
  end         END markers skip the budgets but only one is relayed per transmission that
              relayed audio: a stream of bare ENDs reaches nobody
  abuse       a flood far past it, a malformed frame (wrong length, audio with END,
              START with END), or a client posing as the server (VoiceDown) costs that
              connection only; the game goes on for the others
"""
import argparse
import collections
import contextlib
from pathlib import Path
import struct
import tempfile
import threading
import time

import server_net as auth
from server_net import lobby
from server_public_test import server, login, options

START, END, HAS_NAME, SPECTATORS = 1, 2, 4, 8


class Listener:
    """Reads one connection on a thread: answers pings, keeps every other frame."""

    def __init__(self, peer, name):
        self.peer, self.name = peer, name
        self.frames = collections.deque()
        self.lock = threading.Lock()
        self.closed = False
        self.peer.socket.settimeout(None)
        # This thread answers pings and acknowledges ticks while the test sends too: one
        # writer at a time, or two frames could interleave on the socket.
        self.write = threading.Lock()
        send = peer.send

        def locked(kind, payload=b''):
            with self.write:
                send(kind, payload)
        peer.send = locked
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            while True:
                size = struct.unpack('<I', self.peer.exact(4))[0]
                body = self.peer.exact(size)
                if body[0] == auth.MSG['Ping']:
                    self.peer.send('Pong')
                    continue
                if body[0] == auth.MSG['TickBundle']:
                    # Acknowledge it as a client does, or the server's flow control holds
                    # the room a few ticks ahead of us.
                    tick = struct.unpack_from('<I', body, 1)[0]
                    if tick % 4 == 0:
                        self.peer.send('Progress', struct.pack('<III', tick, 20, tick))
                with self.lock:
                    self.frames.append((body[0], body[1:]))
        except (RuntimeError, OSError):
            self.closed = True

    def raw(self, data):
        with self.write:
            self.peer.socket.sendall(data)

    def take(self, kind):
        with self.lock:
            keep, out = collections.deque(), []
            for k, body in self.frames:
                if k == auth.MSG[kind]:
                    out.append(body)
                else:
                    keep.append((k, body))
            self.frames = keep
        return out

    def wait(self, kind, count=1, timeout=5.0):
        got, deadline = [], time.monotonic() + timeout
        while time.monotonic() < deadline:
            got += self.take(kind)
            if len(got) >= count:
                return got
            time.sleep(0.01)
        return got

    def voice(self, settle=0.4):
        time.sleep(settle)
        out = []
        for body in self.take('VoiceDown'):
            talker, slot, seq, flags, size = struct.unpack_from('<IBHBH', body, 0)
            opus = body[10:10 + size]
            name = None
            if flags & HAS_NAME:
                n = struct.unpack_from('<H', body, 10 + size)[0]
                name = body[12 + size:12 + size + n].decode()
                assert 12 + size + n == len(body), 'trailing VoiceDown bytes'
            else:
                assert 10 + size == len(body), 'trailing VoiceDown bytes'
            out.append(dict(talker=talker, slot=slot, seq=seq, flags=flags, opus=opus, name=name))
        return out


def up(seq, recipients, flags=0, payload=None):
    payload = bytes([seq & 0xff]) * 60 if payload is None else payload
    return struct.pack('<HBBH', seq, recipients, flags, len(payload)) + payload


def frame(kind, payload):
    return struct.pack('<IB', len(payload) + 1, auth.MSG[kind]) + payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-voice-') as tmp:
        root = Path(tmp)
        with server(a.server.resolve(), a.data.resolve(), root) as (port, h, process), contextlib.ExitStack() as stack:
            peers = {n: stack.enter_context(contextlib.closing(login(port, h, n)))
                     for n in ('Alice', 'Bob', 'Carol', 'Dave', 'Erin', 'Frank')}
            alice, bob, carol, dave = (peers[n] for n in ('Alice', 'Bob', 'Carol', 'Dave'))

            # A 4-player recipe: the server opens a random map's seats from its player count.
            recipe = '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) + bytes([4, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()
            alice.send('CreateGame', auth.field('voice') + auth.field('') + auth.field(recipe) + auth.field('') +
                       options() + bytes([4, 0, 0]))
            assert alice.receive('JoinResult').num('<B') == 1
            room = lobby(alice.receive('LobbyState'))
            rid = room[0]
            for p in (bob, carol):
                p.send('JoinGame', struct.pack('<I', rid) + auth.field(''))
                r = p.receive('JoinResult')
                assert r.num('<B') == 1, 'join failed'
            L = {n: Listener(peers[n], n) for n in peers}
            ids = {}

            # listen: Carol has not asked for voice yet.
            for n in ('Alice', 'Bob'):
                peers[n].send('VoiceListen', b'\x01')
            time.sleep(0.2)
            alice.send('VoiceUp', up(1, 0x02, START))
            v = L['Bob'].voice()
            assert len(v) == 1 and v[0]['name'] == 'Alice' and v[0]['slot'] == 0 and v[0]['flags'] & START, v
            ids['Alice'] = v[0]['talker']
            assert L['Carol'].voice(0) == [], 'a connection that never asked for voice received it'
            assert L['Alice'].voice(0) == [], 'the talker heard itself'
            print('listen: only connections that asked for voice receive it; no echo')

            # lobby: a mask of only Bob still reaches the whole room while it is not running.
            carol.send('VoiceListen', b'\x01')
            time.sleep(0.2)
            alice.send('VoiceUp', up(2, 0x02))
            assert len(L['Bob'].voice()) == 1 and len(L['Carol'].voice(0)) == 1, 'lobby voice is for the whole room'
            print('lobby: voice reaches everyone in the room before the game runs')

            # Start: Alice slot 0, Bob 1, Carol 2.
            values = room[3][0][0]
            auth.slot_update(alice, 0, 1, values[1], values[2], values[3], 1, values[5])
            auth.slot_update(bob, 1, 1, 1, 1, 1, 1, 2)
            auth.slot_update(carol, 2, 1, 2, 2, 2, 1, 2)
            time.sleep(0.3)
            alice.send('StartGame')
            for n in ('Alice', 'Bob', 'Carol'):
                assert L[n].wait('GameStarting'), n + ' never got GameStarting'
                peers[n].send('Loaded')
            assert L['Alice'].wait('TickBundle', 5), 'the game is not ticking'
            for n in ('Dave', 'Erin', 'Frank'):   # Erin and Frank never listen; they misbehave below
                peers[n].send('Spectate', struct.pack('<I', rid) + auth.field(''))
                assert L[n].wait('GameStarting'), n + ' was not admitted as a spectator'
            dave.send('VoiceListen', b'\x01')
            time.sleep(0.3)
            for n in L:
                L[n].voice(0)

            def heard(talker, mask, seq, flags=0):
                peers[talker].send('VoiceUp', up(seq, mask, flags))
                time.sleep(0.4)
                return {n: [f for f in L[n].voice(0)] for n in L}

            got = heard('Alice', 0x02, 10)
            assert len(got['Bob']) == 1 and not got['Carol'] and not got['Dave'] and not got['Alice'], \
                ('mask Bob', {k: len(x) for k, x in got.items()})
            got = heard('Alice', 0xff, 11)
            assert len(got['Bob']) == 1 and len(got['Carol']) == 1 and not got['Dave'] and not got['Alice'], \
                'mask 0xff must reach every player, and not the spectator without the SPECTATORS flag'
            got = heard('Alice', 0xff, 111, SPECTATORS)
            assert len(got['Bob']) == 1 and len(got['Carol']) == 1 and len(got['Dave']) == 1, \
                'mask 0xff with the SPECTATORS flag must reach the spectator too'
            assert not (got['Dave'][0]['flags'] & SPECTATORS), 'the SPECTATORS flag must not be relayed'
            got = heard('Alice', 0x00, 112, SPECTATORS)
            assert len(got['Dave']) == 1 and not got['Bob'] and not got['Carol'], 'the SPECTATORS flag alone: spectators only'
            got = heard('Alice', 0x00, 12)
            assert not any(got.values()), 'an empty mask delivered voice'
            got = heard('Bob', 0x03, 13, START)
            assert len(got['Alice']) == 1 and not got['Bob'] and not got['Carol'] and not got['Dave'], \
                'a mask with the talker\'s own bit must not echo, and must not leak'
            assert got['Alice'][0]['slot'] == 1 and got['Alice'][0]['name'] == 'Bob'
            got = heard('Dave', 0xff, 14, START | SPECTATORS)
            assert not any(got[n] for n in ('Alice', 'Bob', 'Carol', 'Dave')), 'a spectator must reach no player in game'
            got = heard('Alice', 0x04, 15)
            assert got['Carol'][0]['opus'] == bytes([15]) * 60 and got['Carol'][0]['seq'] == 15, 'payload intact'
            print('mask: one player, everyone, nobody, no echo, spectators by flag, spectator to no player')

            # names: a plain frame repeats the name every 25 frames.
            for i in range(30):
                alice.send('VoiceUp', up(100 + i, 0x02))
                time.sleep(0.021)
            v = L['Bob'].voice()
            assert len(v) == 30 and all(f['slot'] == 0 for f in v)
            assert 1 <= sum(1 for f in v if f['name'] == 'Alice') <= 2, [f['name'] for f in v]
            print('names: slot on every frame, name on START and every 25th')

            # budget: twice the voice rate (100 frames/s) for a second -- cut to the per-second
            # budget and logged; the game's tick bundles keep coming throughout.
            time.sleep(1.1)   # a fresh budget window
            L['Bob'].take('TickBundle')
            t0 = time.monotonic()
            for i in range(100):
                alice.send('VoiceUp', up(200 + i, 0x02))
                time.sleep(max(0.0, t0 + (i + 1) * 0.01 - time.monotonic()))
            v = L['Bob'].voice(0.5)
            assert 45 <= len(v) <= 80, 'budget let %d of 100 frames through' % len(v)
            assert len(L['Bob'].wait('TickBundle', 20, 3)) >= 20, 'tick bundles stalled under voice traffic'
            deadline = time.monotonic() + 3
            while 'voice over budget' not in (root / 'log').read_text() and time.monotonic() < deadline:
                time.sleep(0.05)
            assert 'voice over budget' in (root / 'log').read_text(), 'the cut was not logged'
            print('budget: 100 frames/s cut to %d, logged, ticks flowing' % len(v))

            # burst: 200 frames in one write (a stalled client catching up) -- the recipient's
            # low-priority queue keeps only the newest, and the END marker still arrives.
            time.sleep(1.1)
            L['Alice'].raw(b''.join(frame('VoiceUp', up(500 + i, 0x02)) for i in range(200)) +
                                 frame('VoiceUp', up(700, 0x02, END, b'')))
            v = L['Bob'].voice(1.0)
            plain = [f for f in v if not f['flags'] & END]
            assert 1 <= len(plain) <= 60 and any(f['flags'] & END for f in v), (len(plain), [f['flags'] for f in v][-3:])
            assert [f['seq'] for f in plain] == sorted(f['seq'] for f in plain), 'voice frames reordered'
            print('burst: %d of 200 delivered, in order, END kept' % len(plain))

            # end: END skips the budgets, but only the END owed to a transmission that relayed
            # audio goes out. 250 bare ENDs in a second (under the disconnect threshold) reach
            # nobody; one audio frame followed by 50 ENDs delivers that frame and one END.
            time.sleep(1.1)
            L['Alice'].raw(b''.join(frame('VoiceUp', up(800 + i, 0xff, END, b'')) for i in range(250)))
            assert L['Bob'].voice(0.8) == [], 'bare END markers were relayed'
            time.sleep(1.1)
            L['Alice'].raw(frame('VoiceUp', up(1100, 0xff, START)) +
                           b''.join(frame('VoiceUp', up(1101 + i, 0xff, END, b'')) for i in range(50)))
            v = L['Bob'].voice(0.8)
            assert [f['flags'] & (START | END) for f in v] == [START, END], [f['flags'] for f in v]
            assert not L['Alice'].closed, 'the talker lost its connection'
            print('end: bare ENDs reach nobody; one END per transmission')

            # Ticks flowed for everyone throughout: the same bundles, in the same order.
            for n in ('Alice', 'Bob', 'Carol'):
                assert not L[n].take('Desynced'), n + ' desynced'

            # abuse: a flood far past the budget, a malformed frame, or a client posing as the
            # server each cost that connection only.
            L['Carol'].raw(b''.join(frame('VoiceUp', up(i, 0xff)) for i in range(400)))
            dave.send('VoiceUp', struct.pack('<HBBH', 1, 0xff, 0, 50) + b'x' * 10)
            peers['Erin'].send('VoiceUp', up(1, 0xff, END, b'x' * 400))
            peers['Frank'].send('VoiceUp', up(1, 0xff, START | END, b''))
            bob.send('VoiceDown', struct.pack('<IBHBH', 1, 0, 0, 0, 0))
            deadline = time.monotonic() + 5
            while not all(L[n].closed for n in ('Carol', 'Dave', 'Erin', 'Frank', 'Bob')) and time.monotonic() < deadline:
                time.sleep(0.05)
            assert L['Carol'].closed, 'a voice flood did not cost the flooder its connection'
            assert L['Dave'].closed, 'a malformed voice frame was accepted'
            assert L['Erin'].closed, 'an END frame carrying audio was accepted'
            assert L['Frank'].closed, 'a START|END frame was accepted'
            assert L['Bob'].closed, 'a client sent VoiceDown and kept its connection'
            L['Alice'].take('Pong')
            alice.send('Ping')
            assert L['Alice'].wait('Pong') and not L['Alice'].closed, 'the innocent player lost its connection'
            print('abuse: flood, malformed frames and a fake VoiceDown each cost only that connection')
            assert process.poll() is None, 'the server died'
    print('server_voice: all passed')


if __name__ == '__main__':
    main()
