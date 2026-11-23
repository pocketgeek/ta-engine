#!/usr/bin/env python3
"""Game-mod transfer with distinct host, server and guest data/cache roots."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from server_public_test import server

def main():
    p=argparse.ArgumentParser();p.add_argument('--server',type=Path,required=True);p.add_argument('--client',type=Path,required=True);p.add_argument('--data',type=Path,required=True);a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='tak-mod-net-') as tmp:
        root=Path(tmp)
        def install(name):
            target=root/name;target.mkdir()
            for f in a.data.resolve().iterdir():
                if f.name.lower() in ('mods','modcache','mapcache'):continue
                (target/f.name).symlink_to(f,target_is_directory=f.is_dir())
            return target
        host,peer,referee=[install(n) for n in ('host','peer','server')]
        with server(a.server.resolve(),referee,root,'--no-auth','--seed','1','--map-cache-dir',str(root/'server-cache')) as (port,_,process):
            result=subprocess.run([str(a.client.resolve()),str(port),str(host),str(peer)],capture_output=True,text=True,timeout=110)
            print(result.stdout,end='')
            # The client's first SAVE FAILED line is the test's own (a file named ModCache
            # in the peer's cache); the failure is on its last line.
            assert result.returncode==0,result.stderr+'\n'+(root/'log').read_text()
            assert process.poll() is None
            log=(root/'log').read_text()
            assert 'DESYNCED' not in log and 'REFEREE SUSPECT' not in log,log
            assert list((root/'server-cache'/'ModCache').glob('*.takmods'))
            assert list((root/'peer-cache'/'ModCache').glob('*.takmods'))
            # Downloads go to the per-user folders: no data folder gains a cache.
            for data in (host,peer,referee):
                for name in ('ModCache','MapCache'):
                    assert not (data/name).exists(),data/name
            assert not (referee/'mods').exists()
if __name__=='__main__':main()
