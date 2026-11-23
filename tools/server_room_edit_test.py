#!/usr/bin/env python3
"""Room editing on a real server (protocol 323): the host changes the room's rules, seats,
map, name and password in the lobby, and every change but the name, the password, the
speed and the host's own READY unreadies the ready guests.

Each case runs on its own and every failure is reported:

  options     SetGameOptions bumps the room's revision (changeBits options) and unreadies
              the guest, the host's READY kept; ALLOW SPECTATORS bumps, speed alone does
              not; unit limit, pathfinder and POWER CODES are adopted in the lobby, MAP
              SCENARIO is not; a mission room adopts only speed and ALLOW SPECTATORS
  stale       a guest's READY on an older revision is stored as not ready, on the
              current one as ready; the host's own is never revision-checked
  join rev    a joiner readies with its JoinResult's revision, before any LobbyState
  slots       the host seating an AI or changing its own team bumps (changeBits players);
              the host never readies a guest; a guest's own edit bumps nothing; a KICK bumps
  join ready  a seat a READY guest left is not ready for the next player
  info        SetRoomInfo renames and locks the room without a bump; GameList follows; the
              password is enforced; a guest's request is ignored; a bad name is refused
              (RoomError); a successor's rename keeps the password it never knew
  create      an empty name is "<name>'s Game" (at most 32 bytes); a name past 32 bytes
              is refused with JoinResult(0) and the connection stays; the log names the
              map and the rules
  shrink      a smaller map moves a human down (JoinResult), removes the AI past it,
              closes the seats past it; GameList shows the capacity; the log line
  grow        a larger map opens the seats up to its capacity
  scenario    MAP SCENARIO off restores the host's start mode in the same change
  refusals    more humans than starts and an unknown recipe are refused (RoomError, then
              a LobbyState with the serial); a guest's request, an old serial and a
              Benchmark room are ignored; an unchanged map is answered to the host alone
  watch       the host leaves its seat to watch (JoinResult 0xFF) and sits again; a
              guest's watch and one in a mission room are ignored; a room started with
              the host watching gives it 0xFF; a running room ignores SetRoomMap
  mods+map    a game-mod offer, a map change and the new map's offer back to back: the
              room still reaches maps ready (the mod validation survives the map change)
  budget      lobby work is charged by kind: 100 ListGames back to back keep the connection
              (cost 1 each); 40 SetRoomMaps (cost 4 each) pass the 128 a second and drop it
  clients     map_transfer_test --room-edit: map changes between real MpClients (downloads,
              flips, changes during an upload, START races); no upload deadline in the log
"""
import argparse
import collections
import contextlib
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import traceback

import server_net as auth
from server_net import MAX_SLOTS, OPTIONS_BYTES, BufferedPeer as Peer
from server_public_test import login as sign_in, server
from map_cache_readonly_test import install

CRUSADES, MODS, SPEED, CAP, STARTS, FOG, PATH, SPECTATORS, SCENARIO, CODES = 0, 2, 3, 5, 13, 11, 15, 16, 17, 18
CHANGE_MAP, CHANGE_SCENARIO, CHANGE_OPTIONS, CHANGE_PLAYERS, CHANGE_MODS, CHANGE_CAPACITY = 1, 2, 4, 8, 16, 32
REQUEST_MAP, REQUEST_INFO = 1, 2
WATCH = 0xFF
UNKNOWN_RECIPE = '~gen1~09000100000000000000038001800108ffffffffff0004'   # a version-9 id
KOTH = 'King of the Hill'


def recipe(players):
    return '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) + bytes([players, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()


def opts(crusades=0, mods=0, speed=10, unit_cap=2000, fog=1, starts=0, path=0, spectators=1, scenario=0, codes=0,
         stress=0, bench=0):
    """GameOptions on the wire (net::put)."""
    return (bytes([crusades, 0, mods, speed, 0]) + struct.pack('<I', unit_cap) +
            bytes([0, stress, fog, bench, starts, 0, path, spectators, scenario, codes]))


def room_of(r):
    """A LobbyState (or GameStarting header) in full."""
    out = {'id': r.num('<I'), 'name': r.field().decode(), 'map': r.field().decode(), 'mission': r.field().decode()}
    out['opts'] = r.data[r.pos:r.pos + OPTIONS_BYTES]
    r.pos += OPTIONS_BYTES
    out['host'], out['maps_ready'] = r.num('<I'), r.num('<B')
    out['cap'], out['passworded'], out['rev'] = r.num('<B'), r.num('<B'), r.num('<I')
    out['bits'], out['serial'] = r.num('<B'), r.num('<I')
    out['slots'] = [(tuple(r.num('<B') for _ in range(6)), r.field()) for _ in range(MAX_SLOTS)]
    return out


# Every sign-in on the public server is an account write, and the server takes 16 a second
# (a registration only while fewer than 8 were taken) before it drops one for "login write
# budget". The cases sign in 27 times, 16 of them within one second on a quiet machine.
SIGN_INS_PER_SECOND = 6
_signed_in = collections.deque(maxlen=SIGN_INS_PER_SECOND)


def login(port, h, name):
    """A signed-in connection to the public server, paced: a sign-in starts at least 1.1 s
    after the one SIGN_INS_PER_SECOND before it ended, so no second of the server's holds
    more than SIGN_INS_PER_SECOND, however fast the cases run."""
    if len(_signed_in) == _signed_in.maxlen:
        wait = _signed_in[0] + 1.1 - time.monotonic()
        if wait > 0:
            time.sleep(wait)
    try:
        return sign_in(port, h, name)
    finally:
        _signed_in.append(time.monotonic())


def local_peer(port, h, name):
    """A connection to a --no-auth server (missions, Benchmark rooms, any name)."""
    p = Peer(port)
    p.send('Hello', struct.pack('<I', auth.VERSION) + auth.field('room-edit-test') + struct.pack('<Q', h) + auth.field(name))
    p.receive('Welcome')
    return p


def create(peer, map_id, cap, name='edit', password='', options=None, watch=False, mission=''):
    """CreateGame: (ok, slot, why, rev) and, when it was accepted, the room."""
    peer.send('CreateGame', auth.field(name) + auth.field(password) + auth.field(map_id) + auth.field(mission) +
              (options or opts()) + bytes([cap, int(watch), 0]))
    answer = joined(peer)
    return answer, (state(peer) if answer[0] else None)


def joined(peer):
    r = peer.receive('JoinResult')
    ok, slot, why = r.num('<B'), r.num('<B'), r.field()
    rev = r.num('<I') if r.pos + 4 <= len(r.data) else None
    return ok, slot, why, rev


def join(peer, room, password=''):
    peer.send('JoinGame', struct.pack('<I', room) + auth.field(password))
    return joined(peer)


def state(peer):
    return room_of(peer.receive('LobbyState'))


def latest(peer, required=True):
    """Everything the server sent before a Pong is in; the last LobbyState of it (None,
    when not `required`, if there was none)."""
    peer.send('Ping')
    peer.receive('Pong')
    queued = peer.pending[auth.MSG['LobbyState']]
    assert queued or not required, 'no LobbyState'
    room = None
    while queued:
        room = room_of(peer.track(auth.MSG['LobbyState'], queued.popleft()))
    return room


def sync(*peers):
    """Settle and drop what is queued, so a later check sees only what follows."""
    for p in peers:
        p.send('Ping')
        p.receive('Pong')
        for kind in ('LobbyState', 'JoinResult', 'RoomError', 'GameList'):
            queue = p.pending[auth.MSG[kind]]
            while queue:
                p.track(auth.MSG[kind], queue.popleft())


QUIET = ('JoinResult', 'LobbyState', 'RoomError')


def quiet(peer, what):
    """Ping and read to the Pong: a request the server ignored was answered with nothing."""
    before = {k: len(peer.pending[auth.MSG[k]]) for k in QUIET}
    peer.send('Ping')
    peer.receive('Pong')
    for k in QUIET:
        assert len(peer.pending[auth.MSG[k]]) == before[k], '%s: the server answered with %s' % (what, k)


def ready(peer, slot, faction=None, team=None, rev=None):
    s = slot if faction is None else faction
    auth.slot_update(peer, slot, 1, s % 5, slot, slot if team is None else team, 1, 2, rev=rev)


def seat_ai(host, slot, team=None):
    auth.slot_update(host, slot, 2, slot % 5, slot, slot if team is None else team, 1, 2)


def set_room_map(peer, serial, map_id, cap, scenario=0, starts=0):
    peer.send('SetRoomMap', struct.pack('<I', serial) + auth.field(map_id) + bytes([cap, scenario, starts]))


def set_room_info(peer, flags, name='', password=''):
    peer.send('SetRoomInfo', bytes([flags]) + auth.field(name) + auth.field(password))


def room_error(peer):
    r = peer.receive('RoomError')
    return r.num('<B'), r.num('<I'), r.field()


def games(peer):
    peer.send('ListGames')
    r = peer.receive('GameList')
    out = {}
    for _ in range(r.num('<I')):
        gid, name, map_id = r.num('<I'), r.field(), r.field()
        players, cap, running, locked = r.num('<B'), r.num('<B'), r.num('<B'), r.num('<B')
        r.num('<I'), r.num('<B')
        out[gid] = {'name': name.decode(), 'map': map_id.decode(), 'cap': cap, 'passworded': locked}
    return out


def log_says(root, text, seconds=3):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if text in (root / 'log').read_text():
            return True
        time.sleep(.05)
    return False


def readied(host, slot):
    """Wait until the host's table shows `slot` ready (the READY travels on another
    connection); the room as it then is."""
    for _ in range(50):
        room = latest(host, required=False)
        if room and room['slots'][slot][0][4]:
            return room
        time.sleep(.02)
    raise AssertionError('slot %d never showed READY' % slot)


# ---- cases ------------------------------------------------------------------------------

def case_options(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host, \
         contextlib.closing(login(env.port, env.h, 'Bobby')) as guest:
        _, room = create(host, recipe(2), 2)
        assert room['rev'] == 1, 'a room starts at revision 1'
        assert join(guest, room['id'])[0]
        ready(host, 0)
        ready(guest, 1)
        room = readied(host, 1)
        rev = room['rev']
        assert room['slots'][0][0][4] == 1
        host.send('SetGameOptions', opts(crusades=1))
        room = latest(host)
        assert room['opts'][CRUSADES] == 1 and room['rev'] == rev + 1 and room['bits'] == CHANGE_OPTIONS, room
        assert room['slots'][1][0][4] == 0, 'the guest stayed ready over a rules change'
        assert room['slots'][0][0][4] == 1, "the host's READY changed with its own change"
        assert log_says(env.root, 'host changed the rules (crusades; rev %d, 1 unreadied)' % (rev + 1)), 'the rules line'
        sync(guest)
        ready(guest, 1)
        rev = readied(host, 1)['rev']
        host.send('SetGameOptions', opts(crusades=1, spectators=0))
        room = latest(host)
        assert room['rev'] == rev + 1 and room['opts'][SPECTATORS] == 0 and room['slots'][1][0][4] == 0, \
            'ALLOW SPECTATORS is a change too'
        sync(guest)
        ready(guest, 1)
        rev = readied(host, 1)['rev']
        host.send('SetGameOptions', opts(crusades=1, spectators=0, speed=14))
        room = latest(host)
        assert room['opts'][SPEED] == 14 and room['rev'] == rev and room['slots'][1][0][4] == 1, \
            'a speed change unreadied the guest'
        host.send('SetGameOptions', opts(crusades=1, spectators=0, speed=14, unit_cap=500, path=4, codes=1, scenario=1))
        room = latest(host)
        assert struct.unpack_from('<I', room['opts'], CAP)[0] == 500 and room['opts'][PATH] == 4 and \
            room['opts'][CODES] == 1, ('unit limit, pathfinder and POWER CODES are adopted in the lobby', room['opts'])
        assert room['opts'][SCENARIO] == 0, 'MAP SCENARIO changed through SetGameOptions'
    # A campaign mission's rules are its own: only the speed and ALLOW SPECTATORS change.
    with contextlib.closing(local_peer(env.local_port, env.local_h, 'Mission')) as host:
        _, room = create(host, KOTH, 8, mission='camp01')
        host.send('SetGameOptions', opts(crusades=1, speed=14, spectators=0, codes=1, unit_cap=500, fog=2))
        room = latest(host)
        o = room['opts']
        assert o[SPEED] == 14 and o[SPECTATORS] == 0, 'a mission room did not adopt the speed and ALLOW SPECTATORS'
        assert o[CRUSADES] == 0 and o[CODES] == 0 and o[FOG] == 1 and struct.unpack_from('<I', o, CAP)[0] == 2000, \
            ('a mission room adopted its rules', o)


def case_stale(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host, \
         contextlib.closing(login(env.port, env.h, 'Bobby')) as guest:
        _, room = create(host, recipe(2), 2)
        assert join(guest, room['id'])[0]
        host.send('SetGameOptions', opts(crusades=1))
        rev = latest(host)['rev']
        assert rev == 2
        sync(guest)
        ready(guest, 1, rev=rev - 1)
        room = latest(host)
        assert room['slots'][1][0][4] == 0, 'a READY sent on an older revision counted'
        assert room['slots'][1][0][1] == 1, "the stale edit's other fields still apply"
        ready(guest, 1, rev=rev)
        assert latest(host)['slots'][1][0][4] == 1, 'a READY on the current revision did not count'
        ready(host, 0, rev=0)
        assert latest(host)['slots'][0][0][4] == 1, "the host's READY was revision-checked"


def case_join_rev(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host, \
         contextlib.closing(login(env.port, env.h, 'Carol')) as late:
        _, room = create(host, recipe(4), 4)
        for flag in (1, 0, 1):
            host.send('SetGameOptions', opts(crusades=flag))
        rev = latest(host)['rev']
        assert rev == 4
        late.send('JoinGame', struct.pack('<I', room['id']) + auth.field(''))
        ok, slot, _, joined_rev = joined(late)   # the LobbyState is not read before READY
        assert ok and joined_rev == rev, ('the JoinResult carries the revision', joined_rev, rev)
        ready(late, slot)
        assert readied(host, slot)['rev'] == rev


def case_slots(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host, \
         contextlib.closing(login(env.port, env.h, 'Bobby')) as guest, \
         contextlib.closing(login(env.port, env.h, 'Carol')) as third:
        _, room = create(host, recipe(4), 4)
        assert join(guest, room['id'])[0] and join(third, room['id'])[0]
        sync(guest, third)
        ready(guest, 1)
        rev = readied(host, 1)['rev']
        seat_ai(host, 3)
        room = latest(host)
        assert room['rev'] == rev + 1 and room['bits'] == CHANGE_PLAYERS and room['slots'][1][0][4] == 0, \
            'seating an AI did not unready the guest'
        assert log_says(env.root, 'host changed the players (rev %d, 1 unreadied)' % (rev + 1)), 'the players line'
        sync(guest)
        ready(guest, 1)
        rev = readied(host, 1)['rev']
        auth.slot_update(host, 0, 1, 0, 0, 5, 1)   # the host's own team
        room = latest(host)
        assert room['rev'] == rev + 1 and room['slots'][1][0][4] == 0, "the host's own team change did not unready"
        # The host never readies someone else.
        auth.slot_update(host, 1, 1, 1, 1, 1, 1)
        assert latest(host)['slots'][1][0][4] == 0, "the host readied the guest's seat"
        sync(guest, host)
        rev = host.rev
        auth.slot_update(guest, 1, 1, 3, 1, 1, 0)   # the guest's own side
        for _ in range(50):   # the guest's edit travels on its own connection
            room = latest(host, required=False)
            if room:
                break
            time.sleep(.02)
        assert room and room['rev'] == rev and room['slots'][1][0][1] == 3, "a guest's own change bumped the room"
        host.send('Kick', bytes([2]))
        room = latest(host)
        assert room['rev'] == rev + 1 and room['bits'] == CHANGE_PLAYERS and room['slots'][2][0][0] == 0, \
            'a KICK did not bump the room'


def case_join_ready(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host, \
         contextlib.closing(login(env.port, env.h, 'Bobby')) as guest, \
         contextlib.closing(login(env.port, env.h, 'Carol')) as third:
        _, room = create(host, recipe(2), 2)
        assert join(guest, room['id'])[0]
        ready(guest, 1)
        readied(host, 1)
        guest.send('LeaveGame')
        assert latest(host)['slots'][1][0][4] == 0, 'the left seat kept its READY'
        ok, slot, _, _ = join(third, room['id'])
        assert ok and slot == 1
        assert latest(host)['slots'][1][0][4] == 0, 'a newcomer took the seat READY'


def case_info(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host, \
         contextlib.closing(login(env.port, env.h, 'Bobby')) as guest, \
         contextlib.closing(login(env.port, env.h, 'Carol')) as outsider:
        _, room = create(host, recipe(4), 4)
        assert join(guest, room['id'])[0]
        sync(guest)
        ready(guest, 1)
        rev = readied(host, 1)['rev']
        set_room_info(host, 3, 'Renamed', 'pw')
        room = latest(host)
        assert room['name'] == 'Renamed' and room['passworded'] == 1, room
        assert room['rev'] == rev and room['slots'][1][0][4] == 1, 'a rename unreadied the guest'
        listed = games(outsider)[room['id']]
        assert listed['name'] == 'Renamed' and listed['passworded'] == 1, listed
        assert join(outsider, room['id'])[2] == b'wrong password'
        ok, slot, _, _ = join(outsider, room['id'], 'pw')
        assert ok, 'the new password was refused'
        assert log_says(env.root, "game %d: renamed to 'Renamed'" % room['id']) and \
            log_says(env.root, 'game %d: password set' % room['id']), 'the info lines'
        sync(host, guest, outsider)
        set_room_info(guest, 1, 'Mine')
        quiet(guest, "a guest's SetRoomInfo")
        quiet(host, "a guest's SetRoomInfo")
        for bad in ('', ' ' * 3, 'x' * 33, 'tab\there'):
            set_room_info(host, 1, bad)
            request, serial, _ = room_error(host)
            assert request == REQUEST_INFO and serial == 0, (bad, request, serial)
            assert state(host)['name'] == 'Renamed', 'a refused name changed the room'
        set_room_info(host, 1, '  Spaced  ')
        assert latest(host)['name'] == 'Spaced', 'a name is trimmed'
        # A successor host never knew the password: its rename keeps it.
        host.send('LeaveGame')
        room = latest(guest)
        sync(outsider)
        set_room_info(guest, 1, 'Mine')
        room = latest(guest)
        assert room['name'] == 'Mine' and room['passworded'] == 1, ("a successor's rename cleared the password", room)


def case_create(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host:
        (ok, slot, _, rev), room = create(host, recipe(2), 2, name='')
        assert ok and rev == 1 and room['name'] == "Alice's Game", room['name']
        assert log_says(env.root, "created game %d 'Alice's Game' on '%s' (crusades=0 fog=1 starts=0 cap=2000 path=0 "
                        "codes=0 scenario=0 spectators=1 mods=0)" % (room['id'], recipe(2))), 'the created-game line'
        host.send('LeaveGame')
        (ok, _, why, _), _ = create(host, recipe(2), 2, name='n' * 33)
        assert not ok and b'32' in why, ('a 33-byte name was not refused', why)
        (ok, _, why, _), _ = create(host, recipe(2), 2, password='p' * 129)
        assert not ok and b'128' in why, ('a 129-byte password was not refused', why)
        (ok, _, _, _), room = create(host, recipe(2), 2, name='n' * 32)
        assert ok and room['name'] == 'n' * 32, 'the connection did not stay usable'
    with contextlib.closing(local_peer(env.local_port, env.local_h, 'L' * 40)) as host:
        (ok, _, _, _), room = create(host, recipe(2), 2, name='')
        assert ok and room['name'] == 'L' * 32, ('the default name is cut to 32 bytes', room['name'])


def case_shrink(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host, \
         contextlib.closing(login(env.port, env.h, 'Bobby')) as guest, \
         contextlib.closing(login(env.port, env.h, 'Carol')) as outsider:
        _, room = create(host, recipe(8), 8)
        assert room['cap'] == 8
        assert join(guest, room['id'])[0]
        sync(guest)
        guest.send('MoveSeat', bytes([5]))
        assert joined(guest)[1] == 5
        for slot in (2, 3, 6):
            seat_ai(host, slot)
        sync(host, guest)   # the host's edits first, so the guest readies on the room they made
        ready(guest, 5)
        room = readied(host, 5)
        rev = room['rev']
        sync(guest)
        set_room_map(host, 7, recipe(4), 8)
        ok, slot, _, moved_rev = joined(guest)
        assert ok and slot == 1 and moved_rev == rev + 1, ('the guest was not moved to the lowest open seat', slot, moved_rev)
        room = latest(host)
        types = [s[0][0] for s in room['slots']]
        assert types == [1, 1, 2, 2, 3, 3, 3, 3], ('the seats after the change', types)
        assert room['slots'][1][1] == b'Bobby' and room['slots'][1][0][4] == 0, 'the moved guest is unready'
        assert room['cap'] == 4 and room['rev'] == rev + 1 and room['serial'] == 7, room
        assert room['bits'] == CHANGE_MAP | CHANGE_CAPACITY and room['map'] == recipe(4), room['bits']
        assert games(outsider)[room['id']]['cap'] == 4, 'GameList shows the old capacity'
        assert log_says(env.root, "host changed the map to '%s' (capacity 4, scenario 0, 1 AI removed, 1 moved, 1 unreadied)"
                        % recipe(4)), 'the map line'


def case_grow(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host:
        _, room = create(host, recipe(2), 2)
        set_room_map(host, 1, recipe(6), 2)
        room = latest(host)
        types = [s[0][0] for s in room['slots']]
        assert room['cap'] == 6 and types == [1, 0, 0, 0, 0, 0, 3, 3], ('the seats after growing', types)
        assert room['bits'] == CHANGE_MAP | CHANGE_CAPACITY and room['serial'] == 1


def case_scenario(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host:
        _, room = create(host, KOTH, 4, options=opts(scenario=1, starts=1))
        assert room['opts'][SCENARIO] == 1 and room['opts'][STARTS] == 0
        rev = room['rev']
        set_room_map(host, 1, KOTH, 4, scenario=0, starts=2)
        room = latest(host)
        assert room['opts'][SCENARIO] == 0 and room['opts'][STARTS] == 2, ('the start mode came back', room['opts'])
        assert room['rev'] == rev + 1 and room['bits'] == CHANGE_SCENARIO and room['map'] == KOTH, room
        assert log_says(env.root, 'host turned MAP SCENARIO off'), 'the scenario line'
        set_room_map(host, 2, KOTH, 4, scenario=0, starts=1)
        room = latest(host)
        assert room['opts'][STARTS] == 1 and room['bits'] == CHANGE_OPTIONS, 'a start mode alone is a rules change'


def case_refusals(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host, \
         contextlib.closing(login(env.port, env.h, 'Bobby')) as guest, \
         contextlib.closing(login(env.port, env.h, 'Carol')) as third:
        _, room = create(host, recipe(4), 4)
        assert join(guest, room['id'])[0] and join(third, room['id'])[0]
        sync(host, guest, third)
        rev = host.rev
        set_room_map(host, 3, recipe(2), 4)
        request, serial, why = room_error(host)
        assert request == REQUEST_MAP and serial == 3 and b'2 starts and 3 players are seated' in why, (request, serial, why)
        room = state(host)
        assert room['rev'] == rev and room['serial'] == 3 and room['map'] == recipe(4), 'a refusal changed the room'
        set_room_map(host, 4, UNKNOWN_RECIPE, 4)
        request, serial, why = room_error(host)
        assert request == REQUEST_MAP and serial == 4 and b'generate a new map' in why, why
        assert state(host)['serial'] == 4
        sync(host, guest, third)
        set_room_map(guest, 9, recipe(8), 4)
        quiet(guest, "a guest's SetRoomMap")
        set_room_map(host, 4, recipe(8), 4)
        quiet(host, 'an old serial')
        set_room_map(host, 5, recipe(4), 4)   # the room's map as it is
        room = state(host)
        assert room['serial'] == 5 and room['rev'] == rev, 'an unchanged map bumped the room'
        quiet(guest, 'an unchanged map')
    with contextlib.closing(local_peer(env.local_port, env.local_h, 'Bench')) as host:
        _, room = create(host, 'Ulasem Arena', 8, options=opts(bench=1), watch=True)
        sync(host)
        set_room_map(host, 1, recipe(2), 2)
        quiet(host, 'a Benchmark room')


def case_watch(env):
    with contextlib.closing(login(env.port, env.h, 'Alice')) as host, \
         contextlib.closing(login(env.port, env.h, 'Bobby')) as guest:
        _, room = create(host, recipe(4), 4)
        assert join(guest, room['id'])[0]
        sync(host, guest)
        host.send('MoveSeat', bytes([WATCH]))
        ok, slot, _, _ = joined(host)
        assert ok and slot == WATCH, 'the host was not told it watches'
        room = state(host)
        assert room['slots'][0][0][0] == 0 and room['slots'][0][1] == b'', 'the host seat did not open'
        assert room['bits'] == CHANGE_PLAYERS
        assert log_says(env.root, 'now watches game %d' % room['id']), 'the watch line'
        sync(guest)
        guest.send('MoveSeat', bytes([WATCH]))
        quiet(guest, "a guest's watch")
        host.send('MoveSeat', bytes([0]))
        ok, slot, _, _ = joined(host)
        assert ok and slot == 0, 'the watching host could not sit again'
        room = state(host)
        assert room['slots'][0][0][0] == 1 and room['slots'][0][1] == b'Alice'
        # A room started with the host watching: GameStarting gives it 0xFF.
        host.send('MoveSeat', bytes([WATCH]))
        assert joined(host)[1] == WATCH
        guest.send('LeaveGame')
        for slot in (0, 1):
            seat_ai(host, slot)
        latest(host)
        host.send('StartGame')
        r = host.receive('GameStarting')
        room_of(r)
        assert r.num('<B') == WATCH, 'the watching host started in a seat'
        sync(host)
        set_room_map(host, 99, recipe(2), 2)
        quiet(host, 'a running room')
    with contextlib.closing(local_peer(env.local_port, env.local_h, 'Mission')) as host:
        create(host, KOTH, 8, mission='camp01')
        sync(host)
        host.send('MoveSeat', bytes([WATCH]))
        quiet(host, 'a watch in a mission room')


def case_mods_map(env):
    """critique A1: a map change while the host's game-mod package validates."""
    package = env.packages[KOTH]
    digest = hashlib.sha256(package).hexdigest()
    mods = struct.pack('<III', 0x32564f54, 0, 0)   # an empty game-mod package (no packs)
    mods_digest = hashlib.sha256(mods).hexdigest()
    with contextlib.closing(login(env.port, env.h, 'Modder')) as host:
        _, room = create(host, 'Inner Circle', 4, options=opts(mods=1))
        rid = room['id']
        host.send('ModOffer', struct.pack('<I', rid) + auth.field('mods') + auth.field(mods_digest) + struct.pack('<I', len(mods)))
        set_room_map(host, 1, KOTH, 4)
        host.send('MapOffer', struct.pack('<I', rid) + auth.field(KOTH) + auth.field(digest) + struct.pack('<I', len(package)))
        r = host.receive('ModRequest')
        assert r.num('<I') == rid and r.field() == mods_digest.encode()
        host.send('ModChunk', struct.pack('<III', rid, r.num('<I'), 0) + mods)
        r = host.receive('MapRequest')
        assert r.num('<I') == rid and r.field() == digest.encode(), 'the deferred map offer was lost'
        xfer = r.num('<I')
        host.send('MapChunk', struct.pack('<III', rid, xfer - 1, 0) + package[:65536])   # an older transfer's chunk
        for offset in range(0, len(package), 65536):
            host.send('MapChunk', struct.pack('<III', rid, xfer, offset) + package[offset:offset + 65536])
        offer = host.receive('ModOffer')
        offer.num('<I'), offer.field()
        host.send('ModReady', struct.pack('<I', rid) + auth.field(offer.field()))
        offer = host.receive('MapOffer')
        offer.num('<I'), offer.field()
        assert offer.field() == digest.encode()
        host.send('MapReady', struct.pack('<I', rid) + auth.field(digest))
        for _ in range(20):
            room = latest(host)
            if room['maps_ready']:
                break
        assert room['maps_ready'] and room['map'] == KOTH, 'the room never reached maps ready'
        assert not host.pending[auth.MSG['MapError']] and not host.pending[auth.MSG['ModError']], 'a transfer error'


def case_budget(env):
    time.sleep(1.1)   # a fresh second of the address's budget, whatever the cases before used
    with contextlib.closing(local_peer(env.local_port, env.local_h, 'Cheap')) as p:
        for _ in range(100):
            p.send('ListGames')
        p.send('Ping')
        p.receive('Pong')
        assert len(p.pending[auth.MSG['GameList']]) == 100, 'a ListGames went unanswered'
    with contextlib.closing(local_peer(env.local_port, env.local_h, 'Costly')) as p:
        for serial in range(1, 41):
            set_room_map(p, serial, recipe(2), 2)
        try:
            p.send('Ping')
            p.receive('Pong')
            dropped = False
        except (RuntimeError, OSError):
            dropped = True
        assert dropped, '40 SetRoomMaps in a second did not pass the per-connection budget'
        assert log_says(env.local_root, 'request rate exceeded'), 'the drop line'


def case_clients(env):
    root = env.root.parent / 'clients'
    root.mkdir()
    roots = [install(root, env.data, n) for n in ('host', 'peer', 'third', 'server')]
    home = root / 'server-home'
    home.mkdir()
    with server(env.binary, roots[3], home, '--no-auth', '--seed', '1') as (port, _h, process):
        result = subprocess.run([str(env.map_tool), '--room-edit', str(port), *map(str, roots[:3])],
                                capture_output=True, text=True, timeout=150)
        print(result.stdout, end='')
        log = (home / 'log').read_text()
        assert result.returncode == 0, result.stderr + '\n' + log[-4000:]
        assert 'upload deadline' not in log, 'an upload deadline fired'
        assert process.poll() is None, 'the server exited'


CASES = [case_options, case_stale, case_join_rev, case_slots, case_join_ready, case_info, case_create, case_shrink,
         case_grow, case_scenario, case_refusals, case_watch, case_mods_map, case_budget, case_clients]


class Env:
    pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    ap.add_argument('--map-tool', type=Path, required=True)
    a = ap.parse_args()
    failed = []
    with tempfile.TemporaryDirectory(prefix='tak-room-edit-') as tmp:
        tmp = Path(tmp)
        env = Env()
        env.binary, env.data, env.map_tool = a.server.resolve(), a.data.resolve(), a.map_tool.resolve()
        out = tmp / 'koth.takmap'
        subprocess.run([str(env.map_tool), '--export-package', str(env.data), str(out), KOTH], check=True)
        env.packages = {KOTH: out.read_bytes()}
        (tmp / 'public').mkdir()
        (tmp / 'local').mkdir()
        with server(env.binary, env.data, tmp / 'public') as (port, h, process), \
             server(env.binary, env.data, tmp / 'local', '--no-auth') as (local_port, local_h, local_process):
            env.port, env.h, env.local_port, env.local_h = port, h, local_port, local_h
            env.root, env.local_root = tmp / 'public', tmp / 'local'
            for case in CASES:
                name = case.__name__[len('case_'):]
                try:
                    case(env)
                    print('ok   ' + name)
                except Exception:
                    failed.append(name)
                    print('FAIL ' + name)
                    traceback.print_exc()
            assert process.poll() is None and local_process.poll() is None, 'a server exited'
        if failed:
            print((tmp / 'public' / 'log').read_text()[-4000:])
    if failed:
        raise SystemExit('FAIL: ' + ', '.join(failed))
    print('PASS: room editing -- rules, stale READY, join revision, seat edits, join READY, name and password, '
          'create names, capacity down and up, scenario start mode, refusals, watching, mods with a map change, '
          'lobby budget, real clients')


if __name__ == '__main__':
    main()
