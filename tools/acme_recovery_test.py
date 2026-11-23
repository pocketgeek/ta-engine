#!/usr/bin/env python3
"""Local HTTPS ACME fixture: real persisted backoff and eventual initial issuance.
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
import time
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--driver',type=Path,required=True)
    ap.add_argument('--tls-tool',type=Path,required=True)
    a=ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-acme-recovery-') as tmp:
        root=Path(tmp)
        subprocess.run([str(a.tls_tool.resolve()),'--fixtures',str(root)],check=True)
        key=serialization.load_pem_private_key((root/'key.pem').read_bytes(),password=None)
        issuer=x509.load_pem_x509_certificate((root/'cert.pem').read_bytes()).subject
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def finish(self):
                super().finish()
                try:
                    self.connection.settimeout(1)
                    self.connection.unwrap()
                except (OSError,ssl.SSLError):pass
            def reply(self,body,status=200,location=None):
                if isinstance(body,dict):body=json.dumps(body).encode()
                self.send_response(status)
                self.send_header('Content-Length',str(len(body)))
                self.send_header('Replay-Nonce','local-test-nonce')
                if location:self.send_header('Location',origin+location)
                if status==503:self.send_header('Retry-After','1')
                self.end_headers()
                if self.command!='HEAD':self.wfile.write(body)
            def do_HEAD(self):self.reply(b'')
            def do_GET(self):
                attempts.append(time.monotonic())
                if len(attempts)==1:self.reply({},503)
                else:self.reply({'newNonce':origin+'/nonce','newAccount':origin+'/account','newOrder':origin+'/new-order'})
            def do_POST(self):
                raw=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                payload=base64.urlsafe_b64decode(raw['payload']+'===')
                if self.path=='/account':self.reply({'status':'valid'},location='/account/1')
                elif self.path=='/new-order':self.reply({'authorizations':[origin+'/auth']},location='/order')
                elif self.path=='/auth':self.reply({'identifier':{'type':'dns','value':'acme.example.test'},'status':'valid'})
                elif self.path=='/order':self.reply({'status':'ready','finalize':origin+'/finalize'})
                elif self.path=='/finalize':
                    csr=x509.load_der_x509_csr(base64.urlsafe_b64decode(json.loads(payload)['csr']+'==='))
                    now=dt.datetime.now(dt.timezone.utc)
                    cert=(x509.CertificateBuilder().subject_name(csr.subject).issuer_name(issuer)
                          .public_key(csr.public_key()).serial_number(x509.random_serial_number())
                          .not_valid_before(now-dt.timedelta(minutes=1)).not_valid_after(now+dt.timedelta(days=30))
                          .add_extension(x509.SubjectAlternativeName([x509.DNSName('acme.example.test')]),False)
                          .sign(key,hashes.SHA256()))
                    self.server.certificate=cert.public_bytes(serialization.Encoding.PEM)
                    self.reply({'status':'valid','certificate':origin+'/certificate'})
                elif self.path=='/certificate':self.reply(self.server.certificate)
                else:self.reply({},404)
        class LocalServer(ThreadingHTTPServer):address_family=socket.AF_INET6
        http=LocalServer(('::1',0),Handler)
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(root/'cert.pem',root/'key.pem')
        http.socket=ctx.wrap_socket(http.socket,server_side=True)
        origin=f'https://localhost:{http.server_port}'
        attempts=[]
        thread=threading.Thread(target=http.serve_forever,daemon=True);thread.start()
        env=dict(os.environ,SSL_CERT_FILE=str(root/'cert.pem'))
        def run(state):
            r=subprocess.run([str(a.driver.resolve()),str(state),'acme.example.test',origin+'/dir','0','0'],
                             env=env,capture_output=True,text=True,timeout=100)
            assert r.returncode==0,(r.stdout,r.stderr)
            assert 'certificate ready' in r.stdout
        try:
            state=root/'initial'
            run(state)
            assert len(attempts)==2 and attempts[1]-attempts[0]>=59,attempts
            assert json.loads((state/'retry.json').read_text())['next']==0
            # A new process must honor a future persisted deadline, then recover
            # without another restart, even though no current certificate exists.
            state=root/'persisted';state.mkdir()
            (state/'retry.json').write_text(json.dumps({'next':int(time.time())+3,'failures':2}))
            start=time.monotonic();before=len(attempts)
            run(state)
            assert len(attempts)==before+1 and attempts[-1]-start>=2
            print('PASS local ACME: transient initial failure, CA backoff, persisted startup backoff, successful recovery')
        finally:
            http.shutdown();http.server_close();thread.join()

if __name__=='__main__':main()
