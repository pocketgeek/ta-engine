#!/usr/bin/env python3
"""Local HTTPS ACME fixture for ACME Renewal Information (RFC 9773).
Checks that a CA window in the past renews at once with "replaces", that an
alreadyReplaced rejection falls back to an ordinary order, and that a future
window or a CA without renewalInfo leaves the certificate alone.
Requires Python cryptography; never contacts a public CA.
"""
import argparse
import base64
import datetime as dt
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import ssl
import socket
import subprocess
import tempfile
import threading
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization


def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode()


def cert_id(pem):
    cert = x509.load_pem_x509_certificate(pem)
    aki = cert.extensions.get_extension_for_class(x509.AuthorityKeyIdentifier).value
    n = cert.serial_number
    return b64(aki.key_identifier) + '.' + b64(n.to_bytes((n.bit_length() + 8) // 8, 'big'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--driver', type=Path, required=True)
    ap.add_argument('--tls-tool', type=Path, required=True)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-acme-ari-') as tmp:
        root = Path(tmp)
        subprocess.run([str(a.tls_tool.resolve()), '--fixtures', str(root)], check=True)
        key = serialization.load_pem_private_key((root/'key.pem').read_bytes(), password=None)
        issuer = x509.load_pem_x509_certificate((root/'cert.pem').read_bytes()).subject
        ca = {'ari': True, 'window': 'past', 'reject_replaces': False}
        orders, polls, issued = [], [], []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args): pass

            def finish(self):
                super().finish()
                try:
                    self.connection.settimeout(1)
                    self.connection.unwrap()
                except (OSError, ssl.SSLError): pass

            def reply(self, body, status=200, location=None, retry=None):
                if isinstance(body, dict): body = json.dumps(body).encode()
                self.send_response(status)
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Replay-Nonce', 'local-test-nonce')
                if location: self.send_header('Location', origin+location)
                if retry: self.send_header('Retry-After', retry)
                self.end_headers()
                if self.command != 'HEAD': self.wfile.write(body)

            def do_HEAD(self): self.reply(b'')

            def do_GET(self):
                if self.path == '/dir':
                    directory = {'newNonce': origin+'/nonce', 'newAccount': origin+'/account', 'newOrder': origin+'/new-order'}
                    if ca['ari']: directory['renewalInfo'] = origin+'/ari/'
                    self.reply(directory)
                elif self.path.startswith('/ari/') and ca['ari']:
                    polls.append(self.path[5:])
                    now = dt.datetime.now(dt.timezone.utc)
                    start = now - dt.timedelta(days=2) if ca['window'] == 'past' else now + dt.timedelta(days=20)
                    fmt = lambda t: t.strftime('%Y-%m-%dT%H:%M:%SZ')
                    self.reply({'suggestedWindow': {'start': fmt(start), 'end': fmt(start + dt.timedelta(days=1))},
                                'explanationURL': 'https://ca.example.test/why'}, retry='3600')
                else: self.reply({}, 404)

            def do_POST(self):
                raw = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                payload = base64.urlsafe_b64decode(raw['payload']+'===')
                if self.path == '/account': self.reply({'status': 'valid'}, location='/account/1')
                elif self.path == '/new-order':
                    order = json.loads(payload); orders.append(order)
                    if 'replaces' in order and ca['reject_replaces']:
                        self.reply({'type': 'urn:ietf:params:acme:error:alreadyReplaced'}, 409)
                    else: self.reply({'authorizations': [origin+'/auth']}, location='/order')
                elif self.path == '/auth': self.reply({'identifier': {'type': 'dns', 'value': 'acme.example.test'}, 'status': 'valid'})
                elif self.path == '/order': self.reply({'status': 'ready', 'finalize': origin+'/finalize'})
                elif self.path == '/finalize':
                    csr = x509.load_der_x509_csr(base64.urlsafe_b64decode(json.loads(payload)['csr']+'==='))
                    now = dt.datetime.now(dt.timezone.utc)
                    cert = (x509.CertificateBuilder().subject_name(csr.subject).issuer_name(issuer)
                            .public_key(csr.public_key()).serial_number(x509.random_serial_number())
                            .not_valid_before(now-dt.timedelta(minutes=1)).not_valid_after(now+dt.timedelta(days=30))
                            .add_extension(x509.SubjectAlternativeName([x509.DNSName('acme.example.test')]), False)
                            .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(key.public_key()), False)
                            .sign(key, hashes.SHA256()))
                    self.server.certificate = cert.public_bytes(serialization.Encoding.PEM)
                    issued.append(self.server.certificate)
                    self.reply({'status': 'valid', 'certificate': origin+'/certificate'})
                elif self.path == '/certificate': self.reply(self.server.certificate)
                else: self.reply({}, 404)

        class LocalServer(ThreadingHTTPServer): address_family = socket.AF_INET6
        http = LocalServer(('::1', 0), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(root/'cert.pem', root/'key.pem')
        http.socket = ctx.wrap_socket(http.socket, server_side=True)
        origin = f'https://localhost:{http.server_port}'
        thread = threading.Thread(target=http.serve_forever, daemon=True); thread.start()
        env = dict(os.environ, SSL_CERT_FILE=str(root/'cert.pem'))

        def run(state, hold):
            r = subprocess.run([str(a.driver.resolve()), str(state), 'acme.example.test', origin+'/dir', '0', str(hold)],
                               env=env, capture_output=True, text=True, timeout=120)
            assert r.returncode == 0, (r.stdout, r.stderr)
            return r
        try:
            # A window in the past (as for a revoked certificate) renews at once and
            # names the replaced certificate.
            r = run(root/'past', 25)
            assert 'certificate renewed' in r.stdout and 'renewal information schedules' in r.stderr, (r.stdout, r.stderr)
            assert len(orders) >= 2 and 'replaces' not in orders[0], orders
            assert orders[1].get('replaces') == cert_id(issued[0]) == polls[0], (orders, polls)
            # alreadyReplaced: the order is retried without replaces.
            ca['reject_replaces'] = True; del orders[:]; del issued[:]
            r = run(root/'replaced', 25)
            assert 'certificate renewed' in r.stdout and 'already replaced' in r.stderr, (r.stdout, r.stderr)
            assert 'replaces' in orders[1] and 'replaces' not in orders[2], orders
            # A future window, or no renewalInfo, keeps the fresh certificate.
            ca['reject_replaces'] = False
            for name, ari in (('future', True), ('fixed', False)):
                ca['ari'], ca['window'] = ari, 'future'; del orders[:]; del polls[:]
                r = run(root/name, 15)
                assert 'certificate renewed' not in r.stdout and len(orders) == 1, (name, r.stdout, r.stderr, orders)
                assert bool(polls) == ari, (name, polls)
            print('PASS local ACME ARI: past window renews with replaces, alreadyReplaced fallback, future window and fixed rule keep the certificate')
        finally:
            http.shutdown(); http.server_close(); thread.join()


if __name__ == '__main__': main()
