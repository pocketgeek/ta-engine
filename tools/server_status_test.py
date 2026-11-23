#!/usr/bin/env python3
"""Exercise the one-shot CLI and local UDP status endpoint on a real server."""
import argparse
import contextlib
import json
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time

import server_net as auth
from server_net import BufferedPeer as Peer, lobby
from server_public_test import login, options, server


REQUEST = b'takserver-status-v1\n'
KEYS = ('running_games', 'lobby_games', 'connected_clients')


def invoke(binary, root, *arguments):
    return subprocess.run([str(binary), '--status', *map(str, arguments)],
                          cwd=root, capture_output=True, timeout=8)


def query(binary, root, port, expected):
    result = invoke(binary, root, '--port', port)
    assert result.returncode == 0, result.stderr
    assert len(result.stdout) <= 256, result.stdout
    value = json.loads(result.stdout)
    assert set(value) == set(KEYS), value
    assert all(type(value[key]) is int and value[key] >= 0 for key in KEYS), value
    assert value == dict(zip(KEYS, expected)), value
    return value


def fake_reply(binary, root, reply, expected=None, default_port=False):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as endpoint:
        try:
            endpoint.bind(('127.0.0.1', 7677 if default_port else 0))
        except OSError:
            if default_port:
                return  # A developer may already have their own server on 7677.
            raise
        endpoint.settimeout(5)
        received = []

        def respond():
            try:
                packet, address = endpoint.recvfrom(65535)
                received.append(packet)
                endpoint.sendto(reply, address)
            except OSError as error:
                received.append(error)

        worker = threading.Thread(target=respond)
        worker.start()
        try:
            result = invoke(binary, root, *([] if default_port else
                            ['--port', endpoint.getsockname()[1]]))
        finally:
            worker.join(timeout=6)
        assert not worker.is_alive(), 'status responder did not finish'
        assert received == [REQUEST], received
        if expected is None:
            assert result.returncode != 0 and not result.stdout and result.stderr, result
        else:
            assert result.returncode == 0, result.stderr
            assert json.loads(result.stdout) == dict(zip(KEYS, expected)), result.stdout


def cli_checks(binary, root):
    expected = (2, 1, 7)
    reply = REQUEST + struct.pack('<III', *expected)
    fake_reply(binary, root, reply, expected)
    fake_reply(binary, root, reply, expected, default_port=True)
    for malformed in (reply[:-1], reply + b'extra', reply + bytes(4096), b'wrong-prefix' + bytes(12)):
        fake_reply(binary, root, malformed)
    # A bound but silent endpoint exercises the deadline, even when the OS
    # would immediately reject an unbound UDP port with an ICMP error.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as endpoint:
        endpoint.bind(('127.0.0.1', 0))
        start = time.monotonic()
        result = invoke(binary, root, '--port', endpoint.getsockname()[1])
        assert result.returncode != 0 and not result.stdout and result.stderr, result
        assert time.monotonic() - start < 6, 'status deadline was not bounded'
    for arguments in [('--port', value) for value in ('0', '-1', '65536', 'no-port')] + [('--port',)]:
        result = invoke(binary, root, *arguments)
        assert result.returncode != 0 and not result.stdout and result.stderr, result
    assert list(root.iterdir()) == [], 'status mode created files without --data'


def create_room(peer, name, private=False):
    recipe = '~gen1~' + (struct.pack('<HQBHH', 10, 1234, 0, 128, 128) +
                        bytes([2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0])).hex()
    peer.send('CreateGame', auth.field(name) + auth.field('') + auth.field(recipe) +
              auth.field('') + options() + bytes([2, 0, int(private)]))
    result = peer.receive('JoinResult')
    assert result.num('<B') == 1
    result.num('<B'); result.field()
    return lobby(peer.receive('LobbyState'))


def start_room(peer, baseline):
    values = baseline[3][0][0]
    auth.slot_update(peer, 0, 1, values[1], values[2], values[3], 1, values[5])
    auth.slot_update(peer, 1, 2, 1, 1, 1, 1, 0)
    peer.send('StartGame')
    peer.receive('GameStarting')
    # Deliberately omit Loaded: the room has started, but its simulation waits
    # for this player while status queries exercise a stable game population.


def ping(peer):
    timeout = peer.socket.gettimeout()
    peer.socket.settimeout(2)
    try:
        peer.send('Ping')
        peer.receive('Pong')
    finally:
        peer.socket.settimeout(timeout)


def endpoint_checks(port, peer):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as endpoint:
        endpoint.settimeout(.15)
        endpoint.connect(('127.0.0.1', port))
        for malformed in (b'', b'?', REQUEST[:-1], REQUEST + b'x', REQUEST + bytes(4096)):
            endpoint.send(malformed)
            try:
                unexpected = endpoint.recv(65535)
            except (socket.timeout, ConnectionRefusedError):
                continue
            raise AssertionError(('malformed status request received a response', unexpected))
        # A bounded malformed burst must not delay TCP service. After the
        # server drains its rate-limited backlog, ordinary status works again.
        for _ in range(64):
            endpoint.send(b'malformed')
        ping(peer)
    time.sleep(2.1)

    # A wildcard UDP bind would answer a request sent to 127.0.0.2 too.
    # Also check a non-loopback interface when one is available locally.
    addresses = {'127.0.0.2'}
    try:
        for item in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            address = item[4][0]
            if not address.startswith('127.') and address != '0.0.0.0':
                addresses.add(address)
    except OSError:
        pass
    for address in addresses:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as endpoint:
            endpoint.settimeout(.2)
            try:
                endpoint.sendto(REQUEST, (address, port))
                packet, _ = endpoint.recvfrom(65535)
            except OSError:
                continue
            raise AssertionError(('status endpoint answered outside 127.0.0.1', address, packet))


def real_server_checks(binary, data, root, query_root, tls_tool):
    with server(binary, data, root) as (port, fingerprint, process), contextlib.ExitStack() as stack:
        query(binary, query_root, port, (0, 0, 0))
        # Open TCP and authentication-in-progress connections are not players.
        pending = stack.enter_context(contextlib.closing(Peer(port)))
        authenticating = stack.enter_context(contextlib.closing(Peer(port)))
        authenticating.hello(fingerprint)
        query(binary, query_root, port, (0, 0, 0))
        # Register once, then authenticate seven independent sessions. This
        # respects the per-address registration throttle and checks that the
        # metric counts connections, including those sharing an account.
        peers = [stack.enter_context(contextlib.closing(login(port, fingerprint, 'Alice')))
                 for _ in range(7)]
        query(binary, query_root, port, (0, 0, 7))
        first = create_room(peers[0], 'first')
        query(binary, query_root, port, (0, 1, 7))
        start_room(peers[0], first)
        query(binary, query_root, port, (1, 0, 7))
        second = create_room(peers[1], 'second')
        start_room(peers[1], second)
        create_room(peers[2], 'private lobby', private=True)
        query(binary, query_root, port, (2, 1, 7))
        # A browser becomes a spectator without changing the client total.
        peers[3].send('Spectate', struct.pack('<I', first[0]) + auth.field(''))
        peers[3].receive('GameStarting')
        before = (root / 'accounts').read_bytes()
        query(binary, query_root, port, (2, 1, 7))
        query(binary, query_root, port, (2, 1, 7))
        assert (root / 'accounts').read_bytes() == before
        endpoint_checks(port, peers[4])
        query(binary, query_root, port, (2, 1, 7))
        peers[2].send('LeaveGame'); ping(peers[2])
        query(binary, query_root, port, (2, 0, 7))
        peers[6].close()
        deadline = time.monotonic() + 3
        while True:
            result = invoke(binary, query_root, '--port', port)
            assert result.returncode == 0, result.stderr
            if json.loads(result.stdout)['connected_clients'] == 6:
                break
            assert time.monotonic() < deadline, result.stdout
            time.sleep(.05)
        assert process.poll() is None
        pending.close(); authenticating.close()

    if tls_tool:
        subprocess.run([str(tls_tool), '--fixtures', str(root)], check=True, timeout=10)
        context = ssl.create_default_context(cafile=str(root / 'cert.pem'))
        with server(binary, data, root, '--tls-cert', str(root / 'cert.pem'),
                    '--tls-key', str(root / 'key.pem')) as (port, fingerprint, process):
            query(binary, query_root, port, (0, 0, 0))
            with contextlib.closing(login(port, fingerprint, 'Alice', context)):
                query(binary, query_root, port, (0, 0, 1))
                assert process.poll() is None

    # TCP and UDP ports are independent: reserve only the UDP port and verify
    # startup does not leave a game listener running without status support.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as occupied:
        occupied.bind(('127.0.0.1', 0))
        port = occupied.getsockname()[1]
        with socket.socket() as tcp:
            tcp.bind(('127.0.0.1', port))
        result = subprocess.run([str(binary), '--local', '--data', str(data), '--port', str(port),
                                 '--accounts', str(root / 'accounts'), '--map-cache-dir', str(root / 'maps')],
                                capture_output=True, timeout=40)
        assert result.returncode != 0, result
        assert b'status' in result.stderr.lower(), result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--data', type=Path)
    parser.add_argument('--tls-tool', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-status-') as temporary:
        root = Path(temporary)
        query_root = root / 'query'
        query_root.mkdir()
        binary = args.server.resolve()
        cli_checks(binary, query_root)
        if args.data:
            real_server_checks(binary, args.data.resolve(), root, query_root,
                               args.tls_tool.resolve() if args.tls_tool else None)
        assert list(query_root.iterdir()) == [], 'status mode wrote files'
    checks = 'status CLI JSON, bounded replies/deadline and no-data mode'
    if args.data:
        checks += ', local endpoint, room/client counts and UDP collision'
        if args.tls_tool:
            checks += ', TLS independence'
    print('PASS: ' + checks)


if __name__ == '__main__':
    main()
