#!/usr/bin/env python3
"""Real ACME issuance/renewal tests against a separately running Pebble CA.
Requires Python cryptography only for constructing an already-due test certificate.
Never points at production Let's Encrypt. See docs/public-server.md.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID


def due_certificate(path, domain):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    now = datetime.datetime.now(datetime.timezone.utc)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, domain)])
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now-datetime.timedelta(days=4))
            .not_valid_after(now+datetime.timedelta(days=1))
            .add_extension(x509.SubjectAlternativeName([x509.DNSName(domain)]), critical=False)
            .sign(key, hashes.SHA256()))
    path.write_bytes(cert.public_bytes(serialization.Encoding.PEM) + key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--driver', type=Path, required=True)
    ap.add_argument('--ca', type=Path, required=True)
    ap.add_argument('--directory', default='https://localhost:14000/dir')
    ap.add_argument('--domain', default='acme.tak.test')
    ap.add_argument('--port', type=int, default=5002)
    a = ap.parse_args()
    assert a.directory.startswith('https://localhost:'), 'local test CA only'
    env = dict(os.environ, SSL_CERT_FILE=str(a.ca.resolve()))
    with tempfile.TemporaryDirectory(prefix='tak-acme-integration-') as tmp:
        state = Path(tmp)/'state'
        def run(hold=1):
            return subprocess.run([str(a.driver.resolve()), str(state), a.domain,
                                   a.directory, str(a.port), str(hold)], env=env,
                                  text=True, capture_output=True, timeout=120)
        def closed():
            with socket.socket() as s:
                s.settimeout(1)
                assert s.connect_ex(('127.0.0.1', a.port)) != 0, 'challenge listener leaked'
        first = run()
        assert first.returncode == 0, first.stderr
        assert 'HTTP-01 listening' in first.stderr, first.stderr
        closed()
        account = hashlib.sha256((state/'account.pem').read_bytes()).digest()
        cert = (state/'current.pem').read_bytes()
        second = run()
        assert second.returncode == 0 and 'HTTP-01' not in second.stderr, second.stderr
        assert (state/'current.pem').read_bytes() == cert
        closed()
        due_certificate(state/'current.pem', a.domain)
        renewed = run(15)
        assert renewed.returncode == 0 and 'certificate renewed' in renewed.stdout, renewed.stderr
        assert hashlib.sha256((state/'account.pem').read_bytes()).digest() == account
        closed()
        # Configure Pebble with PEBBLE_AUTHZREUSE=0 so every renewal validates.
        due_certificate(state/'current.pem', a.domain)
        old = (state/'current.pem').read_bytes()
        with socket.socket() as occupied:
            occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            occupied.bind(('0.0.0.0', a.port)); occupied.listen()
            failed = run(5)
        assert failed.returncode == 0 and 'cannot listen' in failed.stderr, failed.stderr
        assert (state/'current.pem').read_bytes() == old, 'failed renewal replaced certificate'
        retry = json.loads((state/'retry.json').read_text())
        assert retry['next'] > datetime.datetime.now().timestamp()
        deferred = run()
        assert deferred.returncode == 0 and 'obtaining certificate' not in deferred.stderr
        closed()
        print('PASS: issuance, saved certificate/account reuse, background replacement, failed renewal retention, persistent backoff, and listener cleanup')

if __name__ == '__main__':
    main()
