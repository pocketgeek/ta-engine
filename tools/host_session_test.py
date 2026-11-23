#!/usr/bin/env python3
"""The host's powers and the live settings across reconnects, as real MpClients see them:
runs tools/host_session_test.cpp against a real takserver with a short disconnect wait,
and checks the referee saw no desync."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from server_public_test import server


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--client', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    a = ap.parse_args()
    os.environ['TAK_STALL_MS'], os.environ['TAK_DROP_WAIT_MS'] = '3000', '8000'
    with tempfile.TemporaryDirectory(prefix='tak-host-session-') as tmp:
        root = Path(tmp)
        with server(a.server.resolve(), a.data.resolve(), root, '--no-auth', '--seed', '1') as (port, _h, process):
            result = subprocess.run([str(a.client.resolve()), str(port), str(a.data.resolve())],
                                    capture_output=True, text=True, timeout=170)
            print(result.stdout, end='')
            log = (root / 'log').read_text(errors='replace')
            assert result.returncode == 0, result.stderr + '\n' + log[-4000:]
            assert process.poll() is None, 'server exited'
            assert 'DESYNCED' not in log and 'REFEREE SUSPECT' not in log, log[-4000:]
            assert log.count('RESEATED') == 3, log[-4000:]


if __name__ == '__main__':
    main()
