#!/usr/bin/env python3
"""Local-only RTMPS upload test: the stream must stay LIVE and keep uploading.

The receiver is stream_network_test's own --sink (the same static FFmpeg as the game),
behind a Python TLS proxy with a throwaway localhost certificate, so this needs only
python3 and openssl -- no ffmpeg program -- and runs on macOS CI. Like YouTube, the sink
sends nothing while the first megabytes arrive. That caught 0.9.0's macOS SecureTransport
bug: RTMP's non-blocking check for server messages blocked, every upload stalled out the
5 s timeout after a few frames and went RECONNECTING. The server certificate is issued by
a throwaway root CA the client is given; a certificate for another host name and one from
a root outside the system trust store must both be refused. A working upload must
barely hold the encoder up, and one slower than the bitrate must (UPLOAD CAN'T KEEP UP). Usage: python3 tools/check-stream-rtmps.py build/stream_network_test [encoder]
Exits 77 (skipped) without openssl. Never contacts a real service.
"""
import pathlib
import select
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time

SECONDS = 10


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


# Every extension comes from this file, never from the openssl CLI's own openssl.cnf:
# OpenSSL 1.1.1 (the macOS runners' `openssl`) appends -addext extensions to its config's
# v3_ca ones, and the duplicate basicConstraints made OpenSSL 3 refuse the root as an
# invalid certificate ("unable to get local issuer certificate"). The same certificates
# come out of OpenSSL 1.1.1, OpenSSL 3 and LibreSSL.
CONFIG = '''[req]
distinguished_name = subject
[subject]
[root]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
'''


def run_openssl(*args):
    subprocess.run(['openssl', *map(str, args)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def authority(root):
    """A throwaway root CA: the client trusts it as its ca_file, like a public root."""
    cert, key, config = root / 'ca.crt', root / 'ca.key', root / 'openssl.cnf'
    config.write_text(CONFIG)
    run_openssl('req', '-x509', '-config', config, '-extensions', 'root', '-newkey', 'rsa:2048',
                '-nodes', '-sha256', '-keyout', key, '-out', cert, '-days', '1', '-subj', '/CN=TAK test root')
    return cert, key


def certificate(root, ca, name='localhost'):
    """A server certificate for `name` issued by `ca` (a proper chain, not self-signed)."""
    cert, key, request, ext = (root / (name + suffix) for suffix in ('.crt', '.key', '.csr', '.ext'))
    run_openssl('req', '-new', '-config', root / 'openssl.cnf', '-newkey', 'rsa:2048', '-nodes',
                '-keyout', key, '-out', request, '-subj', '/CN=' + name)
    # serverAuth: macOS rejected a TLS server certificate without it (-9807).
    ext.write_text(f'subjectAltName=DNS:{name}\nextendedKeyUsage=serverAuth\n'
                   'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n'
                   'subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n')
    run_openssl('x509', '-req', '-in', request, '-CA', ca[0], '-CAkey', ca[1], '-set_serial',
                str(2 + sum(map(ord, name))), '-days', '1', '-sha256', '-extfile', ext, '-out', cert)
    return cert, key


def refused(exe, cert, key, ca_file, encoder, why):
    """The client must refuse this server's certificate, with the TLS CERTIFICATE reason."""
    proxy = Proxy(cert, key, free_port())
    try:
        client = subprocess.run([exe, f'rtmps://localhost:{proxy.port}/live/test-key', str(ca_file),
                                 '3', encoder], capture_output=True, text=True, timeout=30)
        print(client.stdout + tls_errors(client), end='')
        assert 'LIVE' not in client.stdout and proxy.accepted == 0, why + ' accepted'
        assert 'RECONNECTING 1/3 - TLS CERTIFICATE' in client.stdout, 'no certificate reason: ' + why
        assert 'test-key' not in client.stdout + client.stderr, 'stream key leaked into logs'
    finally:
        proxy.close()


def tls_errors(client):
    """The client's TLS error lines (stream_network_test prints them to stderr)."""
    return ''.join(l + '\n' for l in client.stderr.splitlines() if l.startswith('TLS ERROR'))


def check_chain(cert, key, ca_file):
    """Python's own TLS must accept the test chain, so a refusal names its verify error
    and test certificates the client rightly rejects are not mistaken for an engine bug."""
    proxy = Proxy(cert, key, free_port())  # nothing listens behind it
    try:
        context = ssl.create_default_context(cafile=str(ca_file))
        with socket.create_connection(('127.0.0.1', proxy.port), timeout=5) as raw:
            with context.wrap_socket(raw, server_hostname='localhost'):
                pass
    except ssl.SSLCertVerificationError as error:
        raise AssertionError(f'{ssl.OPENSSL_VERSION} refuses the test certificates: '
                             f'{error.verify_message}') from error
    finally:
        proxy.close()


class Proxy:
    """TLS in, plain RTMP out to the sink; counts what the stream uploaded. With `kbps`
    it takes the upload no faster than that, like a slow home connection."""
    def __init__(self, cert, key, backend, kbps=0):
        self.backend, self.done, self.accepted, self.up, self.kbps = backend, False, 0, 0, kbps
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(cert, key)
        self.socket = socket.socket()
        self.socket.bind(('127.0.0.1', 0))
        self.port = self.socket.getsockname()[1]
        self.socket.listen()
        self.socket.settimeout(.2)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.done:
            try:
                raw, _ = self.socket.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self.forward, args=(raw,), daemon=True).start()

    def forward(self, raw):
        try:
            raw.settimeout(3)
            with self.context.wrap_socket(raw, server_side=True) as client:
                self.accepted += 1
                with socket.create_connection(('127.0.0.1', self.backend), timeout=3) as server:
                    due = time.monotonic()
                    while not self.done:
                        ready, _, _ = select.select([client, server], [], [], .2)
                        if client.pending() and client not in ready:
                            ready.append(client)
                        for source in ready:
                            data = source.recv(16384 if self.kbps else 65536)
                            if not data:
                                return
                            (server if source is client else client).sendall(data)
                            if source is client:
                                self.up += len(data)
                                if self.kbps:
                                    # Pace against the clock, not a sleep per read: each read is
                                    # one TLS record, and FFmpeg sends every 128-byte RTMP chunk
                                    # (the sink's chunk size) and its 1-byte header as records of
                                    # their own. A 0.3 ms sleep per record overslept to ~2 ms on
                                    # the macOS runners, a 0.27 Mbit/s link that held one packet
                                    # past the 5 s write timeout. At most 0.1 s of idle credit.
                                    now = time.monotonic()
                                    due = max(due, now - .1) + len(data) * 8 / (self.kbps * 1000)
                                    if due > now:
                                        time.sleep(due - now)
        except (OSError, ssl.SSLError):
            raw.close()

    def close(self):
        self.done = True
        self.socket.close()
        self.thread.join(timeout=1)


def slow_upload(exe, cert, key, ca_file):
    """An upload slower than the bitrate: libx264's padded 6 Mbit/s into 1.5 Mbit/s. Once
    the socket buffers fill, the encoder must be seen waiting to send (what the panel's
    UPLOAD CAN'T KEEP UP watches) and skipping frames, without the stream failing."""
    seconds = 12
    backend = free_port()
    sink = subprocess.Popen([exe, '--sink', str(backend), str(seconds + 10)],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    proxy = Proxy(cert, key, backend, kbps=1500)
    try:
        assert sink.stdout.readline().strip() == 'SINK LISTENING'
        time.sleep(1)
        client = subprocess.run([exe, f'rtmps://localhost:{proxy.port}/live/test-key', str(ca_file),
                                 str(seconds), 'libx264'], capture_output=True, text=True,
                                timeout=seconds + 30)
        print(client.stdout + tls_errors(client), end='', flush=True)
        done = [l for l in client.stdout.splitlines() if l.startswith('DONE ')]
        assert client.returncode == 0 and done, f'client exit {client.returncode}'
        frames, sent, dropped, waited = (int(field.split('=')[1]) for field in done[0].split()[1:5])
        print(f'slow upload: {frames} frames, {dropped} dropped, waited {waited / 1e6:.1f}s of {seconds}s to send')
        assert any(l.startswith('LIVE') for l in client.stdout.splitlines()), 'never went LIVE'
        assert waited > seconds * 1e6 / 4, f'waited only {waited} us to send over a slow upload'
        assert dropped > 0, 'a slow upload dropped no frames'
    finally:
        if sink.poll() is None:
            sink.kill()
            sink.wait()
        proxy.close()


def main():
    exe = sys.argv[1]
    encoder = sys.argv[2] if len(sys.argv) > 2 else 'libx264'
    if not shutil.which('openssl'):
        print('SKIP: openssl not found')
        return 77
    with tempfile.TemporaryDirectory(prefix='tak-rtmps-') as directory:
        root = pathlib.Path(directory)
        ca = authority(root)
        cert, key = certificate(root, ca)
        check_chain(cert, key, ca[0])
        backend = free_port()
        sink = subprocess.Popen([exe, '--sink', str(backend), str(SECONDS + 10)],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        proxy = Proxy(cert, key, backend)
        try:
            assert sink.stdout.readline().strip() == 'SINK LISTENING'
            time.sleep(1)  # the sink's listen() follows that line
            started = time.monotonic()
            client = subprocess.run([exe, f'rtmps://localhost:{proxy.port}/live/test-key', str(ca[0]),
                                     str(SECONDS), encoder], capture_output=True, text=True,
                                    timeout=SECONDS + 30)
            wall = time.monotonic() - started
            print(client.stdout + tls_errors(client), end='', flush=True)
            lines = client.stdout.splitlines()
            done = [l for l in lines if l.startswith('DONE ')]
            assert client.returncode == 0 and done, f'client exit {client.returncode}'
            frames, sent, _, waited = (int(field.split('=')[1]) for field in done[0].split()[1:5])
            sink_out, _ = sink.communicate(timeout=15)
            print(sink_out, end='')
            received = [l for l in sink_out.splitlines() if l.startswith('SINK BYTES')]
            assert received, 'sink reported nothing'
            got, flv = int(received[0].split()[2]), received[0].split()[4] == '1'
            print(f'{encoder}: {frames} frames in {wall:.1f}s, uploaded {proxy.up} bytes, sink got {got}')
            assert any(l.startswith('LIVE') for l in lines), 'never went LIVE'
            bad = [l for l in lines if l.startswith(('RECONNECTING', 'FAILED'))]
            assert not bad, f'the upload stalled: {bad}'
            assert proxy.accepted == 1, f'{proxy.accepted} TLS connections, expected one'
            assert frames >= SECONDS * 30 // 2, f'only {frames} frames encoded in {SECONDS}s'
            # Nearly all the stream sent arrived (the last packets may still be in flight
            # when the client stops). libx264 pads its
            # 6 Mbit/s CBR, so there a working upload moves megabytes; hardware encoders
            # send the test's flat grey frames in a few bytes.
            assert flv and sent > 0 and got >= sent * 9 // 10, f'sink received {got} of {sent} bytes (FLV header {flv})'
            assert encoder != 'libx264' or got >= 1000000, f'sink received only {got} bytes'
            assert 'test-key' not in client.stdout + client.stderr, 'stream key leaked into logs'
            # An upload that keeps up never holds the encoder up for long (the panel's
            # UPLOAD CAN'T KEEP UP counts over half a second in a second as behind).
            print(f'{encoder}: waited {waited / 1e6:.2f}s of {wall:.1f}s to send')
            assert waited < wall * 1e6 / 4, f'waited {waited} us to send over a working upload'
        finally:
            if sink.poll() is None:
                sink.kill()
                sink.wait()
            proxy.close()
        if encoder == 'libx264':
            slow_upload(exe, cert, key, ca[0])
        # A certificate from the trusted root, but for another host name.
        wrong_cert, wrong_key = certificate(root, ca, 'wrong.invalid')
        refused(exe, wrong_cert, wrong_key, ca[0], encoder, 'host name mismatch')
        # The right host name from a root the system does not trust: '-' leaves the
        # client on its production trust store (the macOS system anchors, or the
        # distribution's CA bundle on Linux), exactly as a real stream uses it.
        refused(exe, cert, key, '-', encoder, 'untrusted root')
        if sys.platform == 'darwin':
            # Streaming's OpenSSL never reads the keychain: it must be handed every system
            # anchor, including the roots YouTube's ingest chains to.
            roots = subprocess.run([exe, '--system-roots'], capture_output=True, text=True, timeout=30)
            print(roots.stdout, end='')
            assert roots.returncode == 0, 'system trust anchors missing'
        print('PASS: RTMPS upload stayed live through a CA-issued chain; wrong host and untrusted root refused with their reason')
        return 0


if __name__ == '__main__':
    sys.exit(main())
