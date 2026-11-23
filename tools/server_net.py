#!/usr/bin/env python3
"""Shared helpers for the server network tests: the wire message table parsed from
src/net/protocol.h, a strict Peer (Hello + SCRAM login), a BufferedPeer, a server
launcher, the LobbyState decoder and slot_update (SlotUpdate with the room revision
the peer last saw, protocol 323)."""
from collections import defaultdict, deque
import contextlib
import hashlib
import hmac
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
header = (ROOT / 'src/net/protocol.h').read_text()
VERSION = int(re.search(r'kNetVersion = (\d+)', header)[1])
enum = re.search(r'enum class Msg[^\{]*\{(.*?)\};', header, re.S)[1]
enum = re.sub(r'//[^\n]*', '', enum)
MSG, value = {}, 0
for entry in enum.split(','):
    entry = entry.strip()
    if not entry:
        continue
    parts = entry.split('=')
    value = int(parts[1].strip()) if len(parts) == 2 else value + 1
    MSG[parts[0].strip()] = value


def field(value):
    value = value.encode() if isinstance(value, str) else value
    return struct.pack('<H', len(value)) + value


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def num(self, fmt):
        size = struct.calcsize(fmt)
        value = struct.unpack_from(fmt, self.data, self.pos)[0]
        self.pos += size
        return value

    def field(self):
        size = self.num('<H')
        value = self.data[self.pos:self.pos + size]
        self.pos += size
        assert len(value) == size
        return value


class Peer:
    rev = 1   # the room revision this peer last saw (JoinResult, LobbyState); a room starts at 1

    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=10)

    def track(self, kind, body):
        """A Reader over one message for the test, noting the room revision it carries."""
        rev = room_rev(kind, body)
        if rev is not None:
            self.rev = rev
        r = Reader(body)
        r.peer = self
        return r

    def close(self):
        self.socket.close()

    def send(self, kind, payload=b''):
        self.socket.sendall(struct.pack('<IB', len(payload) + 1, MSG[kind]) + payload)

    def exact(self, size):
        data = b''
        while len(data) < size:
            chunk = self.socket.recv(size - len(data))
            if not chunk:
                raise RuntimeError('server closed connection')
            data += chunk
        return data

    def receive(self, kind):
        while True:
            length = struct.unpack('<I', self.exact(4))[0]
            assert 1 <= length <= 16 * 1024 * 1024
            body = self.exact(length)
            if body[0] == MSG['Ping']:
                self.send('Pong')
                continue
            assert body[0] == MSG[kind], (kind, body[0])
            return self.track(body[0], body[1:])

    def hello(self, data_hash):
        self.send('Hello', struct.pack('<I', VERSION) + field('server-net-test') +
                  struct.pack('<Q', data_hash) + field('SomeoneElse'))
        self.receive('AuthRequired')

    def login(self, user, secrets, wrong=False):
        nonce = os.urandom(32)
        self.send('AuthBegin', field(user) + field(nonce))
        challenge = self.receive('AuthChallenge')
        new = challenge.num('<B')
        salt, iterations = challenge.field(), challenge.num('<I')
        server_nonce = challenge.field()
        password = b'local synthetic server test password'
        salted = hashlib.pbkdf2_hmac('sha256', password, salt, iterations)
        client_key = hmac.digest(salted, b'Client Key', 'sha256')
        stored_key = hashlib.sha256(client_key).digest()
        server_key = hmac.digest(salted, b'Server Key', 'sha256')
        pieces = [b'TAK-SCRAM-SHA-256-v1', user.encode(), nonce, server_nonce, salt]
        transcript = b''.join(struct.pack('>I', len(x)) + x for x in pieces) + struct.pack('>I', iterations)
        secrets.extend([password, salt, stored_key, server_key])
        if new:
            assert not wrong
            self.send('AuthRegister', field(stored_key) + field(server_key))
        else:
            signature = hmac.digest(stored_key, transcript, 'sha256')
            proof = bytes(a ^ b for a, b in zip(client_key, signature))
            self.send('AuthProof', field(bytes(32) if wrong else proof))
        reply = self.receive('AuthResult')
        status, signature = reply.num('<B'), reply.field()
        reply.field()
        self.token = reply.field() if reply.pos < len(reply.data) else None   # encrypted sign-in only
        if wrong:
            assert status == 2
            return
        assert status == (1 if new else 0)
        assert hmac.compare_digest(signature, hmac.digest(server_key, transcript, 'sha256'))
        welcome = self.receive('Welcome')
        welcome.num('<I')
        assert welcome.field().lower() == user.lower().encode()


@contextlib.contextmanager
def server(binary, data, root, extra=()):
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    command = [str(binary), '--local', '--data', str(data), '--port', str(port),
               '--accounts', str(root / 'accounts.conf'), *extra]
    with (root / 'server.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                text = (root / 'server.log').read_text(errors='replace')
                if 'listening on' in text:
                    fingerprint = re.search(r'retail gameplay hash ([0-9a-f]+)', text)
                    yield port, int(fingerprint[1], 16)
                    break
                if process.poll() is not None:
                    raise RuntimeError('server startup failed: ' + text)
                time.sleep(.05)
            else:
                raise RuntimeError('server startup timed out')
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


MAX_SLOTS = int(re.search(r'kMaxSlots = (\d+)', header)[1])


class BufferedPeer(Peer):
    """A Peer that buffers messages of other kinds until they are asked for."""

    def __init__(self, port):
        super().__init__(port)
        self.pending = defaultdict(deque)

    def receive(self, kind):
        wanted = MSG[kind]
        if self.pending[wanted]:
            return self.track(wanted, self.pending[wanted].popleft())
        while True:
            size = struct.unpack('<I', self.exact(4))[0]
            assert 1 <= size <= 16 * 1024 * 1024
            message = self.exact(size)
            if message[0] == MSG['Ping']:
                self.send('Pong')
            elif message[0] == wanted:
                return self.track(wanted, message[1:])
            else:
                self.pending[message[0]].append(message[1:])


OPTIONS_BYTES = 19  # GameOptions on the wire; its last three bytes are allowSpectators, mapScenario, powerCodes


def lobby(r):
    """A LobbyState (or GameStarting header): (room, map_id, options, slots, maps_ready,
    capacity, passworded, rev, change_bits, map_serial). A Reader from a Peer also
    notes the revision on that peer, for slot_update."""
    room = r.num('<I')
    name, map_id, mission = r.field(), r.field(), r.field()
    options = r.data[r.pos:r.pos + OPTIONS_BYTES]
    r.pos += OPTIONS_BYTES
    host, ready = r.num('<I'), r.num('<B')
    capacity, passworded, rev, change_bits, map_serial = r.num('<B'), r.num('<B'), r.num('<I'), r.num('<B'), r.num('<I')
    slots = []
    for _ in range(MAX_SLOTS):
        values = tuple(r.num('<B') for _ in range(6))
        slots.append((values, r.field()))
    if getattr(r, 'peer', None) is not None:
        r.peer.rev = rev
    return room, map_id, options, slots, ready, capacity, passworded, rev, change_bits, map_serial


def room_rev(kind, body):
    """The room revision an accepted JoinResult, a LobbyState or a GameStarting carries
    (None for anything else)."""
    try:
        if kind == MSG['JoinResult']:
            at = 4 + struct.unpack_from('<H', body, 2)[0]
            return struct.unpack_from('<I', body, at)[0] if body[0] and len(body) >= at + 4 else None
        if kind in (MSG['LobbyState'], MSG['GameStarting']):
            return lobby(Reader(body))[7]
    except (struct.error, AssertionError, IndexError):
        pass
    return None


def slot_update(peer, slot, type_, faction, color, team, ready, ai=2, rev=None):
    """Msg::SlotUpdate. `rev`: the room revision the edit was made on; None sends the
    last one the peer saw (a guest's READY on an older one is stored as not ready)."""
    if rev is None:
        rev = getattr(peer, 'rev', 1)
    peer.send('SlotUpdate', bytes([slot, type_, faction, color, team, ready, ai]) + struct.pack('<I', rev))
