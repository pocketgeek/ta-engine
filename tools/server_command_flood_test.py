#!/usr/bin/env python3
"""Command flood, queue saturation, ownership and logging on a local server."""
import argparse
import contextlib
from pathlib import Path
import struct
import tempfile
import time
import server_net as auth
from server_net import lobby
from server_public_test import server,login,options


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--server',type=Path,required=True);ap.add_argument('--data',type=Path,required=True);a=ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-command-flood-') as tmp:
        root=Path(tmp)
        with server(a.server.resolve(),a.data.resolve(),root) as (port,h,process):
            with contextlib.closing(login(port,h,'Flood')) as p,contextlib.closing(login(port,h,'Observer')) as observer:
                recipe='~gen1~'+(struct.pack('<HQBHH',10,1234,0,128,128)+bytes([2,0,0,1,0,0,0,0,0,0,0])).hex()
                p.send('CreateGame',auth.field('flood')+auth.field('')+auth.field(recipe)+auth.field('')+options()+bytes([2,0,0]))
                p.receive('JoinResult');baseline=lobby(p.receive('LobbyState'))
                values=baseline[3][0][0]
                auth.slot_update(p,0,1,values[1],values[2],values[3],1,values[5])
                auth.slot_update(p,1,2,1,1,1,1,0)
                p.send('StartGame');p.receive('GameStarting');p.send('Loaded')
                # More commands than a queue can hold. The whole excess batch
                # must be discarded before command decoding, and logs coalesced.
                command=struct.pack('<BBiiffB16s',0,7,123456,0,0,0,0,b'')
                batch=struct.pack('<I',512)+command*512
                frame=struct.pack('<IB',len(batch)+1,auth.MSG['PlayerCommands'])+batch
                p.socket.sendall(frame*300)
                observer.send('ListGames');assert observer.receive('GameList').num('<I')==1
                p.send('Ping');p.receive('Pong')
                assert process.poll() is None
                lines=(root/'log').read_text().splitlines()
                overflow=[line for line in lines if 'command budget/queue exceeded' in line]
                assert 1<=len(overflow)<=2,overflow
                # Normal commands remain accepted once quota replenishes.
                time.sleep(1.1)
                p.send('PlayerCommands',struct.pack('<I',1)+command)
                p.send('Ping');p.receive('Pong')
                found=False
                for _ in range(150):
                    tick=p.receive('TickBundle');tick.num('<I');count=tick.num('<I')
                    for _ in range(count):
                        kind,owner,unit=struct.unpack_from('<BBi',tick.data,tick.pos)
                        tick.pos+=43 if kind==23 else 35
                        if unit==123456:
                            assert owner==0,'client forged another player ownership'
                            found=True
                    if found:break
                assert found,'normal commands did not reach a tick'
                assert process.poll() is None
    print('PASS local command flooding: bounded queue/work, coalesced logs, responsive other account and recovery')

if __name__=='__main__':main()
