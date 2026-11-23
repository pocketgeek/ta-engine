#!/usr/bin/env python3
"""Exercise the real server's account login over loopback with SCRAM.

Requires a retail installation; creates only synthetic temporary accounts.
"""
import argparse
import contextlib
import os
from pathlib import Path
import ssl
import subprocess
import tempfile

import server_net as net


def tls_peer(port, fingerprint, context):
    peer = net.Peer(port)
    peer.socket = context.wrap_socket(peer.socket, server_hostname='localhost')
    peer.hello(fingerprint)
    return peer


def spend(port, fingerprint, context, user, token, kind='AuthToken'):
    """Offer a token on a connection of its own: (status, the next token)."""
    with contextlib.closing(tls_peer(port, fingerprint, context)) as peer:
        peer.send(kind, net.field(user) + net.field(token))
        reply = peer.receive('AuthResult')
        status, _, _ = reply.num('<B'), reply.field(), reply.field()
        following = reply.field() if reply.pos < len(reply.data) else None
        if kind == 'AuthToken' and status == 0:
            peer.receive('Welcome')
        return status, following


def token_checks(binary, data, root, client_tool, secrets):
    cert = root / 'cert.pem'
    context = ssl.create_default_context(cafile=str(cert))
    tls = ('--tls-cert', str(cert), '--tls-key', str(root / 'key.pem'),
           '--map-cache-dir', str(root / 'maps'))
    OK, BAD, THROTTLED = 0, 2, 4
    with net.server(binary, data, root, tls) as (port, fingerprint):
        with contextlib.closing(tls_peer(port, fingerprint, context)) as bob:
            bob.login('Bob', secrets)
        with contextlib.closing(tls_peer(port, fingerprint, context)) as alice:
            alice.login('TokenAlice', secrets)
        first = alice.token
        assert first and len(first) == 32, 'a password login over TLS earned no token'
        status, second = spend(port, fingerprint, context, 'TokenAlice', first)
        assert status == OK and second and second != first, 'the token did not sign in and rotate'
        status, third = spend(port, fingerprint, context, 'TokenAlice', first)
        assert status == OK and third and third != second, 'a token whose reply was lost could not be resent'
        assert spend(port, fingerprint, context, 'TokenAlice', second)[0] == BAD, 'a lost reply left its token good'
        assert spend(port, fingerprint, context, 'TokenAlice', third)[0] == BAD, 'a token used twice left the line alive'
    # A fresh server, each: an address only gets a few failed attempts.
    with net.server(binary, data, root, tls) as (port, fingerprint):
        with contextlib.closing(tls_peer(port, fingerprint, context)) as alice:
            alice.login('TokenAlice', secrets)
        fourth = alice.token
        assert spend(port, fingerprint, context, 'Bob', fourth)[0] == BAD, 'a token worked for another account'
        status, fifth = spend(port, fingerprint, context, 'tokenalice', fourth)
        assert status == OK and fifth, 'the token did not survive being refused for another account'
        status, sixth = spend(port, fingerprint, context, 'TokenAlice', fifth)
        assert status == OK and sixth, 'the second rotation failed'
        assert spend(port, fingerprint, context, 'TokenAlice', fourth)[0] == BAD, 'a token worked after its successor'
        assert spend(port, fingerprint, context, 'TokenAlice', sixth)[0] == BAD, 'a stolen token left the line alive'
    with net.server(binary, data, root, tls) as (port, fingerprint):
        with contextlib.closing(tls_peer(port, fingerprint, context)) as alice:
            alice.login('TokenAlice', secrets)
        seventh = alice.token
        status, following = spend(port, fingerprint, context, 'TokenAlice', seventh, 'AuthRevoke')
        assert status == OK and following is None, 'a revoke was not answered, or earned a new token'
        assert spend(port, fingerprint, context, 'TokenAlice', seventh)[0] == BAD, 'a revoked token worked'
        with contextlib.closing(tls_peer(port, fingerprint, context)) as alice:
            alice.login('TokenAlice', secrets)
        survivor = alice.token
    with net.server(binary, data, root, tls) as (port, fingerprint):
        assert spend(port, fingerprint, context, 'TokenAlice', survivor)[0] == BAD, 'a token outlived the server'
    with net.server(binary, data, root, tls) as (port, fingerprint):
        environment = {**os.environ, 'SSL_CERT_FILE': str(cert)}
        subprocess.run([str(client_tool), '--live', 'tls://localhost', str(port), f'{fingerprint:016x}'],
                       env=environment, check=True, timeout=60)
    # Failed token attempts meet the login throttle: 5 free failures, a 6th that locks.
    with net.server(binary, data, root, tls) as (port, fingerprint):
        attempts = [spend(port, fingerprint, context, 'TokenAlice', os.urandom(32))[0] for _ in range(7)]
        assert attempts == [BAD] * 6 + [THROTTLED], attempts
        log = (root / 'server.log').read_text(errors='replace')
    assert 'failed token sign-in' in log
    for token in (first, second, third, fourth, fifth, sixth, seventh, survivor):
        assert token.hex() not in log and token not in (root / 'accounts.conf').read_bytes()


def run(binary, data, tls_tool, client_tool):
    secrets = []
    with tempfile.TemporaryDirectory(prefix='tak-account-auth-') as temporary:
        root = Path(temporary)
        with net.server(binary, data, root) as (port, fingerprint):
            with contextlib.closing(net.Peer(port)) as alice:
                alice.hello(fingerprint)
                alice.login('Alice', secrets)            # unknown name: registers (status 1)
            with contextlib.closing(net.Peer(port)) as alice:
                alice.hello(fingerprint)
                alice.login('ALICE', secrets)            # known name, any case: logs in (status 0)
                assert alice.token is None, 'a plaintext login earned a sign-in token'
            with contextlib.closing(net.Peer(port)) as plain:
                plain.hello(fingerprint)
                plain.send('AuthToken', net.field('Alice') + net.field(os.urandom(32)))
                assert b'encrypted' in plain.receive('Reject').field(), 'a token was entertained in the clear'
            with contextlib.closing(net.Peer(port)) as failed:
                failed.hello(fingerprint)
                failed.login('Alice', secrets, wrong=True)   # wrong proof: status 2
            with contextlib.closing(net.Peer(port)) as pending:
                pending.hello(fingerprint)
                pending.send('AuthBegin', net.field('NotRegistered') + net.field(os.urandom(32)))
                challenge = pending.receive('AuthChallenge')
                assert challenge.num('<B') == 1, 'an unregistered name was not offered registration'
        # The account survives a restart.
        with net.server(binary, data, root) as (port, fingerprint):
            with contextlib.closing(net.Peer(port)) as alice:
                alice.hello(fingerprint)
                alice.login('Alice', secrets)
        stored = (root / 'accounts.conf').read_bytes()
        password = secrets[0]
        assert password not in stored, 'the password reached the accounts file'
        # An option the server does not know (here a Crusades campaign-service flag) stops
        # it before it binds or reads anything, like any unknown option.
        for flag in ('--crusades-db', '--crusades-definition'):
            refused = subprocess.run([str(binary), flag, str(root / 'campaign.sqlite')], capture_output=True, timeout=30)
            assert refused.returncode == 1 and b'unknown option or missing value: ' + flag.encode() in refused.stderr, \
                refused.stderr
        assert not (root / 'campaign.sqlite').exists()
        usage = subprocess.run([str(binary), '--help'], capture_output=True, timeout=30)
        assert usage.returncode == 0 and b'crusades' not in usage.stdout.lower(), 'takserver --help lists Crusades options'
        subprocess.run([str(tls_tool), '--fixtures', str(root)], check=True, timeout=10)
        token_checks(binary, data, root, client_tool, secrets)
    subprocess.run([str(client_tool)], check=True, timeout=60)   # the hostile-server scenarios
    print('PASS: live SCRAM register, login, wrong proof, pending registration and restart; unknown options '
          'refused; sign-in tokens over TLS')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--tls-tool', type=Path, required=True)
    parser.add_argument('--client-tool', type=Path, required=True)
    args = parser.parse_args()
    run(args.server.resolve(), args.data.resolve(), args.tls_tool.resolve(), args.client_tool.resolve())


if __name__ == '__main__':
    main()
