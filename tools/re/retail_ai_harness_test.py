"""Teardown of retail_ai_harness.Run with a fake kwin_wayland (no retail, no
Proton, no real display): the display-holder `sleep` must be stopped by the PID
it recorded, whether kwin is still running or has already exited (the case that
used to leave one `sleep 1000000` behind per run), and nothing carrying the
run's private XDG_RUNTIME_DIR may outlive teardown.

    python3 tools/re/retail_ai_harness_test.py
"""
import os
import signal
import sys
import tempfile
import time
import types
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import retail_ai_harness  # noqa: E402

# Like kwin, runs its last argument as the X client, but in a session of its
# own (so killing kwin's process group does not reach it); then either exits
# at once or stays up the way a live compositor does.
FAKE_KWIN = '''#!/bin/sh
for a; do last=$a; done
DISPLAY=:77 setsid -f sh -c "$last" </dev/null >/dev/null 2>&1
[ "$FAKE_KWIN_EXIT" = 1 ] && exit 0
exec sleep 1000002
'''


def run_processes(xdg):
    """Live processes whose XDG_RUNTIME_DIR is the run's private one (found
    independently of the harness's own bookkeeping)."""
    mark = b'XDG_RUNTIME_DIR=' + str(xdg).encode()
    found = []
    for proc in Path('/proc').glob('[0-9]*'):
        try:
            if mark in (proc / 'environ').read_bytes().split(b'\0') and alive(int(proc.name)):
                found.append(int(proc.name))
        except OSError:
            pass
    return found


def alive(pid):
    try:
        state = Path(f'/proc/{pid}/stat').read_text().split(') ')[-1]
    except OSError:
        return False
    return not state.startswith('Z')


class Teardown(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        root = Path(self.tmp.name)
        bin_dir = root / 'bin'
        bin_dir.mkdir()
        kwin = bin_dir / 'kwin_wayland'
        kwin.write_text(FAKE_KWIN)
        kwin.chmod(0o755)
        self.saved = dict(os.environ)
        os.environ['PATH'] = f'{bin_dir}:{os.environ["PATH"]}'
        (root / 'run').mkdir()
        args = types.SimpleNamespace(cores='', tmpdir=root)
        self.run = retail_ai_harness.Run(args, root / 'run')
        retail_ai_harness.log = lambda message: None

    def tearDown(self):
        # Never leave a test's own processes behind, even when an assertion failed.
        for pid in run_processes(self.run.xdg):
            if pid and alive(pid):
                os.kill(pid, signal.SIGKILL)
        os.environ.clear()
        os.environ.update(self.saved)
        self.tmp.cleanup()

    def check(self, kwin_exits):
        os.environ['FAKE_KWIN_EXIT'] = '1' if kwin_exits else '0'
        self.run.start_display()
        self.assertEqual(self.run.display, ':77')
        started = run_processes(self.run.xdg)
        self.assertTrue(started, 'the fake display is up')
        if kwin_exits:
            self.run.kwin.wait(10)
        self.run.teardown()
        deadline = time.monotonic() + 5
        while run_processes(self.run.xdg) and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertEqual(run_processes(self.run.xdg), [], 'nothing of the run is left (the holder included)')
        self.assertIsNotNone(self.run.kwin.poll(), 'kwin is stopped and reaped')
        self.assertIn(self.run.holder_pid, started, 'the holder PID was recorded')

    def test_kwin_running(self):
        self.check(kwin_exits=False)

    def test_kwin_already_gone(self):
        self.check(kwin_exits=True)


if __name__ == '__main__':
    unittest.main()
