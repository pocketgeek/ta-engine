#!/usr/bin/env python3
"""Exercise public admission and work budgets on a real authenticated server."""
import argparse
import hashlib
import contextlib
from pathlib import Path
import re
import socket
import ssl
import struct
import subprocess
import tempfile
import time
import server_net as auth
from server_net import BufferedPeer as Peer, lobby

@contextlib.contextmanager
def server(binary, data, root, *options):
    with socket.socket() as s:
        s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    with (root/'log').open('w') as log:
        p=subprocess.Popen([str(binary),'--local','--data',str(data),'--port',str(port),
            '--accounts',str(root/'accounts'),'--map-cache-dir',str(root/'maps'),*options],stdout=log,stderr=log)
        try:
            deadline=time.monotonic()+40
            while time.monotonic()<deadline:
                text=(root/'log').read_text()
                if 'listening on' in text:
                    yield port,int(re.search(r'retail gameplay hash ([0-9a-f]+)',text)[1],16),p
                    return
                assert p.poll() is None,text
                time.sleep(.05)
            raise AssertionError('server startup timeout')
        finally:
            p.terminate()
            try:p.wait(timeout=10)
            except subprocess.TimeoutExpired:p.kill();p.wait()

def login(port, fingerprint, name, context=None):
    p=Peer(port)
    if context:p.socket=context.wrap_socket(p.socket,server_hostname="localhost")
    p.hello(fingerprint);p.login(name,[]);return p

def options(stress=0,benchmark=0,spectators=1,scenario=0,random_starts=0,power_codes=0):
    # GameOptions wire bytes: ... pathMode, then allowSpectators, mapScenario, powerCodes
    return bytes([0,1,0,10,0])+struct.pack('<I',2000)+bytes([0,stress,0,benchmark,random_starts,0,0,spectators,scenario,power_codes])

def create(p,stress=0,benchmark=0,mission='',map_id='missing-test-map'):
    p.send('CreateGame',auth.field('test')+auth.field('')+auth.field(map_id)+
        auth.field(mission)+options(stress,benchmark)+bytes([8,0,0]))

def room(p):
    r=p.receive('JoinResult');assert r.num('<B')==1;r.num('<B');r.field()
    return p.receive('LobbyState').num('<I')

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--server',type=Path,required=True);ap.add_argument('--data',type=Path,required=True);ap.add_argument('--tls-tool',type=Path,required=True);ap.add_argument('--map-tool',type=Path,required=True);a=ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-public-') as tmp:
        root=Path(tmp)
        subprocess.run([str(a.tls_tool.resolve()),"--fixtures",str(root)],check=True)
        context=ssl.create_default_context(cafile=str(root/"cert.pem"))
        with server(a.server.resolve(),a.data.resolve(),root,'--max-games','1','--map-memory-mib','1') as (port,h,process):
            with contextlib.closing(login(port,h,'Alice')) as p,contextlib.closing(login(port,h,'Bob')) as q:
                for stress,bench,mission in [(1,0,''),(0,1,''),(0,0,'mission')]:
                    create(p,stress,bench,mission)
                    assert b'public servers' in p.receive('Reject').field()
                create(p);rid=room(p)
                # The game limit refuses the request and keeps the connection (JoinResult, not Reject).
                create(q);q.send('Ping');q.receive('Pong')
                assert not q.pending[auth.MSG['Reject']],'the game limit was answered with a Reject, which ends the connection'
                r=q.receive('JoinResult');assert r.num('<B')==0;r.num('<B');assert r.field()==b'server game capacity reached'
                q.send('ListGames');assert q.receive('GameList').num('<I')==1
                p.send('SetGameOptions',options(benchmark=1))
                assert b'disabled' in p.receive('Reject').field()
                p.send('MapOffer',struct.pack('<I',rid)+auth.field('missing-test-map')+auth.field('0'*64)+struct.pack('<I',1<<20))
                error=p.receive('MapError');assert error.num('<I')==rid;assert b'budget' in error.field()
                # Cache admission uses actual bytes, not a forged smaller offer.
                cache=root/'maps'/'MapCache'/('0'*64+'.takmap')
                cache.parent.mkdir(parents=True,exist_ok=True);cache.write_bytes(bytes(600000))
                p.send('MapOffer',struct.pack('<I',rid)+auth.field('missing-test-map')+auth.field('0'*64)+struct.pack('<I',1000))
                error=p.receive('MapError');assert error.num('<I')==rid
                assert b'mismatch' in error.field() and cache.exists()
                cache.unlink()
                # A smaller offer is admitted; mere offers reserve aggregate memory.
                p.send('MapOffer',struct.pack('<I',rid)+auth.field('missing-test-map')+auth.field('0'*64)+struct.pack('<I',1000))
                request=p.receive('MapRequest');assert request.num('<I')==rid and request.field()==b'0'*64
                assert request.num('<I')>0,'a MapRequest carries the server\'s transfer id'
                q.send('ListGames');assert q.receive('GameList').num('<I')==1
                p.send('LeaveGame');p.send('Ping');p.receive('Pong')
                create(q);room(q) # capacity and upload reservation released on leave
                assert process.poll() is None
        stock=root/'stock.takmap'
        subprocess.run([str(a.map_tool.resolve()),'--export-package',str(a.data.resolve()),str(stock)],check=True)
        digest=hashlib.sha256(stock.read_bytes()).hexdigest()
        assert stock.stat().st_size>1000
        with server(a.server.resolve(),a.data.resolve(),root,'--map-memory-mib','1') as (port,h,process):
            with contextlib.closing(login(port,h,'Alice')) as p:
                create(p,map_id='Ulasem Arena');rid=room(p)
                # Installed map with the correct hash but a forged smaller size:
                # cache miss must request bounded upload, never build/accept local.
                p.send('MapOffer',struct.pack('<I',rid)+auth.field('Ulasem Arena')+auth.field(digest)+struct.pack('<I',1000))
                request=p.receive('MapRequest');assert request.num('<I')==rid and request.field()==digest.encode()
                xfer=request.num('<I')
                p.send('MapChunk',struct.pack('<III',rid,xfer,0)+bytes(1001))
                error=p.receive('MapError');assert error.num('<I')==rid
                assert b'chunk' in error.field()
                p.send('LeaveGame');p.send('Ping');p.receive('Pong')
                create(p);rid=room(p)
                offer=lambda c,size=1000:c.send('ModOffer',struct.pack('<I',rid)+auth.field('mods')+auth.field('1'*64)+struct.pack('<I',size))
                # Game mods off (only a debug --mods launch sends it; the lobby always
                # sends on): even the host's game packs are refused.
                offer(p);error=p.receive('ModError');assert error.num('<I')==rid;assert b'off' in error.field()
                mods=bytearray(options());mods[2]=1
                p.send('SetGameOptions',mods)
                cache=root/'maps'/'ModCache'/('1'*64+'.takmods')
                cache.parent.mkdir(parents=True,exist_ok=True);cache.write_bytes(bytes(600000))
                offer(p)
                error=p.receive('ModError');assert error.num('<I')==rid
                assert b'mismatch' in error.field() and cache.exists()
                # Only the host selects game mods: a guest's offer (and chunk) is refused.
                with contextlib.closing(login(port,h,'Bob')) as q:
                    q.send('JoinGame',struct.pack('<I',rid)+auth.field(''))
                    assert q.receive('JoinResult').num('<B')==1
                    q.send('ModChunk',struct.pack('<III',rid,1,0)+bytes(16))
                    error=q.receive('ModError');assert error.num('<I')==rid;assert b'only the host' in error.field()
                    # A stream of refused offers and chunks is answered (and logged)
                    # once per 5 s: the next reply here must be the Pong.
                    for _ in range(200):q.send('ModChunk',struct.pack('<III',rid,1,0)+bytes(16))
                    offer(q);q.send('Ping');q.receive('Pong')
                    assert (root/'log').read_text().count('refused game-mod upload from client')==2,'refused uploads are not rate-limited'
                p.send('Ping');p.receive('Pong');assert process.poll() is None
        with server(a.server.resolve(),a.data.resolve(),root,'--closed-registration') as (port,h,process):
            with contextlib.closing(login(port,h,'Alice')) as p:
                p.send('ListGames');assert p.receive('GameList').num('<I')==0
                # Sub-poll-size floods must still hit a wall-clock budget.
                p.socket.sendall((struct.pack('<IB',1,auth.MSG['ListGames']))*300)
                p.socket.settimeout(3)
                try:
                    while p.socket.recv(65536):pass
                except ConnectionResetError:pass
            with contextlib.closing(Peer(port)) as p:
                p.hello(h);p.send('AuthBegin',auth.field('Carol')+auth.field(bytes(32)))
                p.receive('AuthChallenge');p.send('AuthRegister',auth.field(bytes(32))+auth.field(bytes(32)))
                r=p.receive('AuthResult');assert r.num('<B') not in (0,1)
                assert process.poll() is None
        with server(a.server.resolve(),a.data.resolve(),root,'--max-accounts','2') as (port,h,process):
            with contextlib.closing(Peer(port)) as p:
                p.hello(h);p.send('AuthBegin',auth.field('Carol')+auth.field(bytes(32)))
                p.receive('AuthChallenge');p.send('AuthRegister',auth.field(bytes(32))+auth.field(bytes(32)))
                assert p.receive('AuthResult').num('<B') not in (0,1)
                assert process.poll() is None
        with server(a.server.resolve(),a.data.resolve(),root,'--tls-cert',str(root/'cert.pem'),'--tls-key',str(root/'key.pem')) as (port,h,process):
            with contextlib.closing(login(port,h,'Alice',context)) as p:
                create(p);rid=room(p)
                data=bytes(8<<20)
                p.send('MapOffer',struct.pack('<I',rid)+auth.field('missing-test-map')+auth.field(hashlib.sha256(data).hexdigest())+struct.pack('<I',len(data)))
                request=p.receive('MapRequest');assert request.num('<I')==rid;request.field();xfer=request.num('<I')
                # More than a second's upload budget: pace this, do not disconnect.
                for offset in range(0,len(data),65536):
                    p.send('MapChunk',struct.pack('<III',rid,xfer,offset)+data[offset:offset+65536])
                error=p.receive('MapError');assert error.num('<I')==rid
                assert b'budget' not in error.field() # fully received; invalid map payload rejected
                p.send('Ping');p.receive('Pong');assert process.poll() is None
        with server(a.server.resolve(),a.data.resolve(),root,'--tls-cert',str(root/'cert.pem'),'--tls-key',str(root/'key.pem')) as (port,h,process):
            with contextlib.closing(Peer(port)) as p:
                p.socket=context.wrap_socket(p.socket,server_hostname='localhost')
                p.hello(h);p.login('Alice',[])
                p.send('ListGames');assert p.receive('GameList').num('<I')==0
                p.send('Ping');p.receive('Pong')
                assert process.poll() is None
        # Campaign rooms never run game mods: not at creation, not by SetGameOptions,
        # and an upload is refused (a local test server, where missions are allowed).
        local=root/'local-campaign';local.mkdir()
        with server(a.server.resolve(),a.data.resolve(),local,'--no-auth') as (port,h,process):
            with contextlib.closing(Peer(port)) as p:
                p.send('Hello',struct.pack('<I',auth.VERSION)+auth.field('mods-test')+struct.pack('<Q',h)+auth.field('Local'))
                p.receive('Welcome')
                mods=bytearray(options());mods[2]=1
                p.send('CreateGame',auth.field('test')+auth.field('')+auth.field('King of the Hill')+auth.field('camp01')+bytes(mods)+bytes([8,0,0]))
                r=p.receive('JoinResult');assert r.num('<B')==1;r.num('<B');r.field()
                state=lobby(p.receive('LobbyState'));rid=state[0]
                assert state[2][2]==0,'a campaign room adopted GAME MODS at creation'
                p.send('SetGameOptions',bytes(mods))
                state=lobby(p.receive('LobbyState'));assert state[2][2]==0,'a campaign room adopted GAME MODS later'
                p.send('ModOffer',struct.pack('<I',rid)+auth.field('mods')+auth.field('1'*64)+struct.pack('<I',1000))
                error=p.receive('ModError');assert error.num('<I')==rid;assert b'campaigns' in error.field()
                p.send('Ping');p.receive('Pong');assert process.poll() is None
        denied=subprocess.run([str(a.server.resolve()),'--data',str(a.data.resolve())],capture_output=True,timeout=10)
        assert denied.returncode!=0 and b'public listeners require' in denied.stderr
        for value in ['0','-1','not-a-number','18446744073709551616']:
            denied=subprocess.run([str(a.server.resolve()),'--max-games',value,'--local','--data',str(a.data.resolve())],capture_output=True,timeout=10)
            assert denied.returncode!=0 and b'invalid server resource limit' in denied.stderr
    print('PASS: public game modes, room admission (the game limit refuses with JoinResult and keeps the connection), upload reservation, registration, request floods and ordinary lobby traffic, TLS login and secure startup defaults, game mods host-only and never in campaigns')
if __name__=='__main__':main()
