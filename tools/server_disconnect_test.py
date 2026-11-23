#!/usr/bin/env python3
"""The disconnect wait (protocol 289) on a real server, at protocol level.

Two seated players on a generated map; the server runs with a short stall threshold and
wait (TAK_STALL_MS / TAK_DROP_WAIT_MS) so nothing here waits the real 8 s and 30 s. The
budget counts from a player's last contact, so the silence before a wait is spent too:

  takeover   Bob goes silent: Alice is told the room is WAITING for him with a countdown of
             the budget less the stall threshold already spent, no tick closes during it,
             and when it runs out (the whole budget after his last contact) a bundle carries
             Event::AiTakeover for his seat, the game goes on and his seat issues
             commands (the server's Normal AI). His connection is closed, and a late
             reconnect is told an AI has the seat.
  resume     Bob goes silent past the stall threshold and talks again inside the wait:
             the wait ends, nobody is taken over, and both receive the same bundles.
  reconnect  Bob's connection closes without a Leave: the room waits at once; a new
             connection's Rejoin (token + the first tick it lacks) is Reseated and
             streamed the bundles from exactly that tick, identical to Alice's; the old
             token is spent.
  leave      an explicit LeaveGame is a Leave event at once, with no wait.
  budget     the wait is a budget per seat per game, charged in whole seconds for the
             whole silence: Bob back 0.5 s into his first wait, 3.5 s after he went quiet,
             has spent 4 s of the 8, so his second wait counts down from the 1 s his
             silence has not used (his own client is told the 4 s) and he is taken over
             when it runs out, not 8 s later.
  spent      Bob back in the last second of his wait has spent it all: his next silence
             is taken over as soon as it holds the game past the 1 s grace, without the
             room waiting for him.
  hold       silences shorter than the stall threshold never show a wait, but each holds
             the game (the room runs its lead past Bob's last acknowledged tick, then
             stops) for longer than the 1 s grace, and that is charged: repeated, they
             spend the budget and an AI takes the seat over.
  jitter     silences of a second hold the game for less than the grace: however many,
             they cost nothing, and a stall after them still has the whole budget.
"""
import argparse
import os
from pathlib import Path
import socket
import struct
import tempfile
import time
import traceback

import server_net as auth
from server_net import lobby
from server_public_test import login, options, server

STALL_MS, WAIT_MS = 3000, 8000
HOLD_S, JITTER_S = 2.4, 1.0        # Bob's silences under the threshold (the hold is ~0.7 s less)
AI_TAKEOVER, LEAVE = 4, 2          # net::Event::Kind
WIDE = (23, 26)                    # ReclaimArea, BuildManaArea carry x2/z2 (43 bytes)


def recipe():
    return '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) + bytes([2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()


class Player:
    """A seated peer that keeps up its side of the game: answers pings, acknowledges
    ticks (Progress) and records every bundle, wait notice and other message."""

    def __init__(self, peer):
        self.socket = peer.socket
        self.buf = b''
        self.bundles = {}      # tick -> (raw payload, [(kind, player)], [(kind, player)])
        self.waits = []        # (monotonic time, [(slot, ms left)])
        self.other = {}        # message kind -> [payload]
        self.closed = False
        self.bye = None
        self.token = 0
        self.slot = -1
        self.pinged = 0.0
        self.rev = 1           # the room revision last seen (auth.slot_update sends it)

    def send(self, kind, payload=b''):
        try:
            self.socket.sendall(struct.pack('<IB', len(payload) + 1, auth.MSG[kind]) + payload)
        except OSError:
            self.closed = True   # the server hung up; what it sent is still read

    def close(self):
        self.socket.close()
        self.closed = True

    def next_tick(self):
        t = 0
        while t in self.bundles:
            t += 1
        return t

    def last_tick(self):
        return max(self.bundles) if self.bundles else -1

    def handle(self, kind, body):
        if kind == auth.MSG['Ping']:
            self.send('Pong')
        elif kind == auth.MSG['Pong']:
            pass
        elif kind == auth.MSG['TickBundle']:
            tick, n = struct.unpack_from('<II', body, 0)
            pos, cmds = 8, []
            for _ in range(n):
                k, player = body[pos], body[pos + 1]
                assert k != 27, 'an explicit order in a test bundle'
                cmds.append((k, player))
                pos += 43 if k in WIDE else 35
            ne = struct.unpack_from('<I', body, pos)[0]
            events = [(body[pos + 4 + 2 * i], body[pos + 5 + 2 * i]) for i in range(ne)]
            self.bundles[tick] = (body, cmds, events)
            if tick % 4 == 0:
                self.send('Progress', struct.pack('<III', tick, 10, tick))
        elif kind == auth.MSG['Waiting']:
            n = body[0]
            self.waits.append((time.monotonic(), [struct.unpack_from('<BI', body, 1 + 5 * i) for i in range(n)]))
            self.own = struct.unpack_from('<BI', body, 1 + 5 * n)   # (running, ms): our seat's budget
        elif kind == auth.MSG['Bye']:
            self.bye = body[2:2 + struct.unpack_from('<H', body, 0)[0]]
        else:
            rev = auth.room_rev(kind, body)
            if rev is not None:
                self.rev = rev
            self.other.setdefault(kind, []).append(body)

    def pump(self, seconds, until=None):
        """Read for up to `seconds`; stop early once until(self) holds. Returns it."""
        deadline = time.monotonic() + seconds
        while True:
            if until and until(self):
                return True
            left = deadline - time.monotonic()
            if left <= 0:
                break
            # A live client is never quiet for long, even with no ticks to acknowledge
            # (the game client's round-trip probe pings every second).
            if not self.closed and time.monotonic() - self.pinged > 0.5:
                self.pinged = time.monotonic()
                self.send('Ping')
            while len(self.buf) >= 4 and len(self.buf) >= 4 + struct.unpack_from('<I', self.buf)[0]:
                size = struct.unpack_from('<I', self.buf)[0]
                body, self.buf = self.buf[4:4 + size], self.buf[4 + size:]
                self.handle(body[0], body[1:])
                if until and until(self):
                    return True
            self.socket.settimeout(min(left, 0.05))
            try:
                chunk = self.socket.recv(1 << 16)
            except socket.timeout:
                continue
            except OSError:
                chunk = b''
            if not chunk:
                self.closed = True
                if len(self.buf) < 4:
                    break
                continue   # drain the frames that came before the close
            self.buf += chunk
        return bool(until and until(self))

    def take(self, kind, seconds=10):
        assert self.pump(seconds, lambda p: p.other.get(auth.MSG[kind])), 'no ' + kind
        return auth.Reader(self.other[auth.MSG[kind]].pop(0))


def pump_all(players, seconds, until=None):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        for p in players:
            p.pump(0.02)
        if until and until():
            return True
    return bool(until and until())


def waiting_for(slot):
    return lambda p: bool(p.waits) and any(s == slot for s, _ in p.waits[-1][1])


def not_waiting(p):
    return bool(p.waits) and not p.waits[-1][1]


def start(port, h, alice_name, bob_name):
    alice, bob = Player(login(port, h, alice_name)), Player(login(port, h, bob_name))
    alice.send('CreateGame', auth.field('wait') + auth.field('') + auth.field(recipe()) + auth.field('') +
               options() + bytes([2, 0, 0]))
    assert alice.take('JoinResult').num('<B') == 1
    state = lobby(alice.take('LobbyState'))
    room = state[0]
    bob.send('JoinGame', struct.pack('<I', room) + auth.field(''))
    joined = bob.take('JoinResult')
    assert joined.num('<B') == 1 and joined.num('<B') == 1
    values = state[3][0][0]
    auth.slot_update(alice, 0, 1, values[1], values[2], values[3], 1, values[5])
    auth.slot_update(bob, 1, 1, 1, 1, 1, 1, 2)
    assert alice.pump(10, lambda p: any(lobby(auth.Reader(b))[3][1][0][4] for b in p.other.get(auth.MSG['LobbyState'], [])))
    alice.send('StartGame')
    for p in (alice, bob):
        r = p.take('GameStarting')
        lobby(r)
        p.slot, _seed, p.token = r.num('<B'), r.num('<I'), r.num('<Q')
        assert p.token
        p.send('Loaded', struct.pack('<Q', h))
    assert pump_all((alice, bob), 20, lambda: alice.last_tick() >= 45 and bob.last_tick() >= 45), 'the game never ticked'
    return room, alice, bob


def events_of(p, kind):
    return [(t, e) for t, (_, _, ev) in sorted(p.bundles.items()) for e in ev if e[0] == kind]


def same_bundles(a, b, first):
    common = [t for t in a.bundles if t >= first and t in b.bundles]
    assert len(common) > 30, ('too few common ticks', len(common))
    for t in common:
        assert a.bundles[t][0] == b.bundles[t][0], 'the peers were sent different bundles for tick %d' % t


def scenario_takeover(port, h):
    room, alice, bob = start(port, h, 'Alice', 'Bob')
    try:
        # Bob stops reading and sending. The room stops for him with a countdown, short by
        # the silence the stall threshold has already spent.
        silent = time.monotonic()
        assert alice.pump(10, waiting_for(1)), 'Alice was never told the room waits for Bob'
        began, waits = alice.waits[-1]
        ms = dict(waits)[1]
        assert WAIT_MS - STALL_MS - 700 <= ms <= WAIT_MS - STALL_MS, ('the countdown was not the budget less the silence', ms)
        frozen = alice.last_tick()
        alice.pump(1.0)
        assert alice.last_tick() <= frozen + 1, ('ticks closed during the wait', frozen, alice.last_tick())
        # The wait runs out: the countdown clears and the seat is taken over in a bundle.
        assert alice.pump(WAIT_MS / 1000 + 8, lambda p: events_of(p, AI_TAKEOVER)), 'no AI takeover event'
        took = time.monotonic() - silent
        assert WAIT_MS / 1000 - 0.7 <= took <= WAIT_MS / 1000 + 1.0, ('the takeover did not come a budget after the last contact', took)
        assert any(not w for _, w in alice.waits), 'the countdown never cleared'
        tick, event = events_of(alice, AI_TAKEOVER)[0]
        assert event == (AI_TAKEOVER, 1)
        assert not events_of(alice, LEAVE) and not events_of(alice, 1), 'the seat was forfeited, not taken over'
        # The game goes on, and Bob's seat now gives orders: the server AI (Normal).
        def ai_ordered(p):
            return any(player == 1 for t, (_, cmds, _) in p.bundles.items() if t >= tick for _, player in cmds)
        assert alice.pump(30, ai_ordered), 'the AI never issued a command for the taken-over seat'
        assert alice.pump(10, lambda p: p.last_tick() > tick + 30), 'the game stopped after the takeover'
        # Bob's old connection is closed with the reason; a late reconnect is told why.
        bob.pump(5, lambda p: p.closed)
        assert bob.closed and (bob.bye is None or b'AI' in bob.bye), bob.bye
        late = Player(login(port, h, 'Bob'))
        late.send('Rejoin', struct.pack('<IQI', room, bob.token, bob.next_tick()))
        refused = late.take('JoinResult')
        assert refused.num('<B') == 0
        refused.num('<B')
        assert b'AI took over' in refused.field()
        late.close()
    finally:
        alice.close(); bob.close()


def scenario_resume(port, h):
    _room, alice, bob = start(port, h, 'Alice', 'Bob')
    try:
        assert alice.pump(10, waiting_for(1)), 'the room never waited for the silent player'
        # Bob talks again inside the wait: the wait ends and the game goes on.
        bob.send('Ping')
        assert pump_all((alice, bob), 5, lambda: not_waiting(alice)), 'the wait did not end when Bob came back'
        before = alice.last_tick()
        pump_all((alice, bob), WAIT_MS / 1000 + 2)
        assert alice.last_tick() > before + 30, 'the game did not go on'
        assert not events_of(alice, AI_TAKEOVER) and not events_of(bob, AI_TAKEOVER), 'a returning player was taken over'
        same_bundles(alice, bob, 0)
    finally:
        alice.close(); bob.close()


def scenario_reconnect(port, h):
    room, alice, bob = start(port, h, 'Alice', 'Bob')
    try:
        bob.close()   # the link goes without a Leave
        assert alice.pump(5, waiting_for(1)), 'a closed connection did not start the wait'
        frm = bob.next_tick()
        back = Player(login(port, h, 'Bob'))
        back.bundles = dict(bob.bundles)
        back.send('Rejoin', struct.pack('<IQI', room, bob.token, frm))
        r = back.take('Reseated')
        slot, token, start_tick, end = r.num('<B'), r.num('<Q'), r.num('<I'), r.num('<I')
        assert slot == 1 and token and token != bob.token, (slot, token)
        assert start_tick == frm and end >= frm, (start_tick, frm, end)
        assert pump_all((alice, back), 5, lambda: not_waiting(alice)), 'the wait did not end on the reconnect'
        before = alice.last_tick()
        pump_all((alice, back), WAIT_MS / 1000 + 2)
        assert alice.last_tick() > before + 30, 'the game did not go on after the reconnect'
        assert back.next_tick() > end, 'the reconnect was not caught up'
        assert not events_of(alice, AI_TAKEOVER), 'a reconnected player was taken over'
        same_bundles(alice, back, frm)
        # The old token is spent.
        other = Player(login(port, h, 'Bob'))
        other.send('Rejoin', struct.pack('<IQI', room, bob.token, 0))
        refused = other.take('JoinResult')
        assert refused.num('<B') == 0
        refused.num('<B')
        assert refused.field() == b'invalid or expired resume token'
        other.close(); back.close()
    finally:
        alice.close()


def scenario_leave(port, h):
    _room, alice, bob = start(port, h, 'Alice', 'Bob')
    try:
        bob.send('LeaveGame')
        assert pump_all((alice, bob), 5, lambda: events_of(alice, LEAVE)), 'no Leave event'
        assert not any(w for _, w in alice.waits), 'an explicit leave made the room wait'
        assert not events_of(alice, AI_TAKEOVER)
    finally:
        alice.close(); bob.close()


def stall_and_return(alice, bob, after):
    """Bob goes silent; `after` seconds into the wait Alice is shown, he talks again.
    Returns once the wait is over and both have caught up a little."""
    assert alice.pump(10, waiting_for(1)), 'the room never waited for the silent player'
    began, waits = alice.waits[-1]
    assert dict(waits)[1] > WAIT_MS - STALL_MS - 700, ('the first wait did not have the whole budget', waits)
    alice.pump(after - (time.monotonic() - began))
    bob.send('Ping')
    assert pump_all((alice, bob), 5, lambda: not_waiting(alice)), 'the wait did not end when Bob came back'
    before = alice.last_tick()
    assert pump_all((alice, bob), 5, lambda: alice.last_tick() > before + 15), 'the game did not go on'
    assert not events_of(alice, AI_TAKEOVER), 'a returning player was taken over'
    return len(alice.waits)


def scenario_budget(port, h):
    _room, alice, bob = start(port, h, 'Alice', 'Bob')
    try:
        stall_and_return(alice, bob, 0.5)   # silent ~3.5 s, charged 4 s: 4 s of the budget left
        # Bob's own client is told what is left, the window it would reconnect within.
        assert getattr(bob, 'own', None) == (0, WAIT_MS - 4000), ('Bob was not told his budget', getattr(bob, 'own', None))
        # Bob goes silent again: the countdown starts from what is left less this silence,
        # and runs out.
        assert alice.pump(10, waiting_for(1)), 'the second stall did not make the room wait'
        began, waits = alice.waits[-1]
        assert 0 < dict(waits)[1] <= WAIT_MS - 4000 - STALL_MS, ('the second wait did not start from the rest of the budget', waits)
        assert alice.pump(WAIT_MS / 1000 + 5, lambda p: events_of(p, AI_TAKEOVER)), 'no AI takeover on the second stall'
        took = time.monotonic() - began
        assert 0.4 <= took < (WAIT_MS - 4000 - STALL_MS) / 1000 + 0.6, ('the takeover did not come when the budget ran out', took)
        assert events_of(alice, AI_TAKEOVER)[0][1] == (AI_TAKEOVER, 1)
        assert not events_of(alice, LEAVE)
    finally:
        alice.close(); bob.close()


def scenario_spent(port, h):
    _room, alice, bob = start(port, h, 'Alice', 'Bob')
    try:
        seen = stall_and_return(alice, bob, (WAIT_MS - STALL_MS - 500) / 1000)   # back in the last second
        silent = time.monotonic()
        assert alice.pump(STALL_MS / 1000 + 6, lambda p: events_of(p, AI_TAKEOVER)), 'no AI takeover with the budget spent'
        took = time.monotonic() - silent
        assert took < STALL_MS / 1000 + 1.5, ('the takeover waited', took)
        assert not any(s == 1 for _, w in alice.waits[seen:] for s, _ in w), 'the room waited for a seat with no budget left'
        assert events_of(alice, AI_TAKEOVER)[0][1] == (AI_TAKEOVER, 1)
    finally:
        alice.close(); bob.close()


def short_silences(alice, bob, seconds, times):
    """Bob goes quiet for `seconds` (under the stall threshold), then keeps up for a
    moment, up to `times` over or until his seat is taken over. Returns how many."""
    n = 0
    while n < times and not events_of(alice, AI_TAKEOVER):
        n += 1
        alice.pump(seconds, lambda p: events_of(p, AI_TAKEOVER))
        before = alice.last_tick()
        pump_all((alice, bob), 0.8, lambda: events_of(alice, AI_TAKEOVER) or alice.last_tick() > before + 18)
    return n


def scenario_hold(port, h):
    _room, alice, bob = start(port, h, 'Alice', 'Bob')
    try:
        # Each silence holds the game ~1.7 s, charged 1 s past the grace: eight spend the 8 s
        # budget and the ninth is taken over a second into its hold.
        n = short_silences(alice, bob, HOLD_S, 16)
        assert events_of(alice, AI_TAKEOVER), 'repeated short holds never spent the budget'
        assert events_of(alice, AI_TAKEOVER)[0][1] == (AI_TAKEOVER, 1)
        assert WAIT_MS // 2000 + 1 <= n <= WAIT_MS // 1000 + 2, ('the budget was not spent by the holds beyond the grace', n)
        assert not any(s == 1 for _, w in alice.waits for s, _ in w), 'a silence under the threshold made the room wait'
    finally:
        alice.close(); bob.close()


def scenario_jitter(port, h):
    _room, alice, bob = start(port, h, 'Alice', 'Bob')
    try:
        # Charged a rounded-up second each without the grace, these would spend it all.
        n = short_silences(alice, bob, JITTER_S, WAIT_MS // 1000 + 2)
        assert not events_of(alice, AI_TAKEOVER), ('ordinary jitter was charged', n)
        assert not any(s == 1 for _, w in alice.waits for s, _ in w), 'jitter made the room wait'
        # A real stall afterwards still has the whole budget.
        assert alice.pump(10, waiting_for(1)), 'the stall after the jitter did not make the room wait'
        ms = dict(alice.waits[-1][1])[1]
        assert ms > WAIT_MS - STALL_MS - 700, ('jitter spent the budget', ms)
    finally:
        alice.close(); bob.close()


SCENARIOS = [scenario_takeover, scenario_resume, scenario_reconnect, scenario_leave, scenario_budget, scenario_spent,
             scenario_hold, scenario_jitter]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    a = ap.parse_args()
    os.environ['TAK_STALL_MS'], os.environ['TAK_DROP_WAIT_MS'] = str(STALL_MS), str(WAIT_MS)
    failed = []
    with tempfile.TemporaryDirectory(prefix='tak-disconnect-') as tmp:
        root = Path(tmp)
        with server(a.server.resolve(), a.data.resolve(), root) as (port, h, process):
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
            print((root / 'log').read_text(errors='replace')[-4000:])
    if failed:
        raise SystemExit('FAIL: ' + ', '.join(failed))
    print('PASS: disconnect wait -- silent seat waited for then taken over by a Normal AI, resume inside the '
          'wait, reconnect from the first missing tick, explicit leave without a wait, one wait budget per seat '
          'over all of its stalls, counted from the last contact, short holds past a 1 s grace charged, jitter free')


if __name__ == '__main__':
    main()
