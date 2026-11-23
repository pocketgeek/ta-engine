#!/usr/bin/env python3
"""takserver --seed N pins every game's seed, N = 0 included; no --seed rolls one per game.

Regression for the Ulasem benchmark "irreproducible hash" report: `--seed 0` used to mean
"unset" (a random seed per game), so a seed-0 benchmark run was a different game each time.
The seed is the u32 just before the u64 resume token and the u32 history length at the
tail of the GameStarting payload.
"""
import argparse
import contextlib
from pathlib import Path
import struct
import tempfile
import server_net as auth
from server_public_test import server,login,options


def game_seed(port,h,name):
    with contextlib.closing(login(port,h,name)) as p:
        recipe='~gen1~'+(struct.pack('<HQBHH',10,1234,0,128,128)+bytes([2,0,0,1,0,0,0,0,0,0,0])).hex()
        p.send('CreateGame',auth.field('seed')+auth.field('')+auth.field(recipe)+auth.field('')+options()+bytes([2,0,0]))
        p.receive('JoinResult')
        values=auth.lobby(p.receive('LobbyState'))[3][0][0]
        auth.slot_update(p,0,1,values[1],values[2],values[3],1,values[5])
        auth.slot_update(p,1,2,1,1,1,1,0)
        p.send('StartGame')
        d=p.receive('GameStarting').data
        return struct.unpack_from('<I',d,len(d)-16)[0]


def seeds(binary,data,*options_):
    with tempfile.TemporaryDirectory(prefix='tak-seed-') as tmp:
        with server(binary,data,Path(tmp),*options_) as (port,h,process):
            return [game_seed(port,h,'Seed%d'%i) for i in range(3)]


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--server',type=Path,required=True);ap.add_argument('--data',type=Path,required=True);a=ap.parse_args()
    b,d=a.server.resolve(),a.data.resolve()
    s=seeds(b,d,'--seed','0');assert s==[0,0,0],s
    s=seeds(b,d,'--seed','7');assert s==[7,7,7],s
    s=seeds(b,d);assert len(set(s))==3,s
    print('PASS server seeds: --seed 0 and --seed N pin every game; no --seed rolls per game')

if __name__=='__main__':main()
