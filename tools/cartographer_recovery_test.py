#!/usr/bin/env python3
"""Exercise real autosave, abrupt process exit and startup recovery."""
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import time

binary, data = sys.argv[1:]
env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_SHUTDOWN_DBUS_ON_QUIT='1')
with tempfile.TemporaryDirectory(prefix='tak-editor-recovery-') as folder:
    root = Path(folder)
    # The fallback destination is isolated: a regression must never overwrite
    # the user's installed map while testing restored output-directory metadata.
    install = root / 'installation'
    install.mkdir()
    for entry in Path(data).iterdir():
        if entry.name.lower() == 'maps':
            maps = install / entry.name
            maps.mkdir()
            for child in entry.iterdir():
                (maps / child.name).symlink_to(child.resolve(), target_is_directory=child.is_dir())
        else:
            (install / entry.name).symlink_to(entry.resolve(), target_is_directory=entry.is_dir())
    def command(phase):
        return [binary, str(install), 'recovery', folder, phase]
    writer = subprocess.Popen(command('hold'), env=env)
    try:
        deadline = time.monotonic() + 25
        while not (root / 'writer-ready').exists():
            assert writer.poll() is None, 'writer exited before recovery snapshot'
            assert time.monotonic() < deadline, 'autosave did not finish'
            time.sleep(.05)
        subprocess.run(command('probe'), env=env, check=True, timeout=30)
        assert list(root.rglob('recovery-*.kmp')), 'second editor removed live recovery'
    finally:
        writer.kill()
        writer.wait(timeout=5)
    snapshot = next(root.rglob('recovery-*.kmp'))
    shutil.copy2(snapshot, str(snapshot) + '.bak')
    snapshot.write_bytes(b'HAPI')  # Simulate a damaged latest generation.
    subprocess.run(command('restore'), env=env, check=True, timeout=30)
    assert (root / 'original-destination' / 'Ulasem Arena.kmp').is_file()
print('PASS: live-session exclusion, killed editor recovery with corrupt-primary fallback, original save destination, cleanup')
