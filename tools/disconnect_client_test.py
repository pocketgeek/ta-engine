#!/usr/bin/env python3
"""The disconnect wait (protocol 289) end to end: real takserver, two headless takclients.

A host (--mphost) and a joiner (--mpjoin) play Inner Circle (--seed 1, 4x speed) on a
server with a short stall threshold and wait (TAK_STALL_MS / TAK_DROP_WAIT_MS):

  takeover   the joiner is frozen (SIGSTOP) past the wait: the server hands its seat to a
             Normal AI and the host plays on to the end with no desync against the
             referee; the host's replay then plays back to the hashes it recorded
             (TAK_REPLAY_VERIFY), takeover included.
  resume     the joiner is frozen past the stall threshold but not past the wait: the game
             waits, goes on when it thaws, and both clients finish on the same state hash.
  reconnect  the joiner loses its link mid-game (TAK_DROP_LINK_AT_TICK): it reconnects inside
             the wait, catches up from the first tick it lacked, and both clients finish on
             the same state hash; its own replay, recorded across the reconnect, verifies.
  reconnect-login  the same on a server that requires accounts: the reconnect signs in again
             with the keys kept from the first login (the password is gone by then).

POSIX only (SIGSTOP); the harness flags are debug builds' only.
"""
import argparse
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
import traceback

from replay_always_test import complete_ticks, replays

MAP = 'Inner Circle'
STALL_MS, WAIT_MS = 1500, 5000   # the budget counts from the last contact: the stall spends 1.5 s
PASSWORD = 'local synthetic disconnect test password'
TIME = 30                      # game seconds each client plays
DONE = re.compile(r'mp-headless done: tick=(\d+) hash=(\w+) units=(\d+) err=(\S+)')


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def environment(root, name):
    env = {k: v for k, v in os.environ.items() if not k.startswith('TAK_')}
    for sub in ('data', 'config', 'home'):
        (root / name / sub).mkdir(parents=True, exist_ok=True)
    env.update(XDG_DATA_HOME=str(root / name / 'data'), XDG_CONFIG_HOME=str(root / name / 'config'),
               HOME=str(root / name / 'home'), TAK_HEADLESS='1', SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    return env


class Match:
    def __init__(self, args, root, joiner_env=None, login=False):
        self.args, self.root, self.login = args, root, login
        self.port = free_port()
        env = environment(root, 'server')
        env.update(TAK_STALL_MS=str(STALL_MS), TAK_DROP_WAIT_MS=str(WAIT_MS))
        self.server_log = open(root / 'server.log', 'w')
        self.server = subprocess.Popen(
            [args.server, '--port', str(self.port), '--data', args.data, '--local', '--seed', '1',
             *(['--accounts', str(root / 'accounts.conf')] if login else ['--no-auth'])],
            stdout=self.server_log, stderr=subprocess.STDOUT, env=env)
        self.wait_log('listening', 90)
        self.host = self.client('host', ['--mphost'], {'TAK_SPEED': '40'})
        self.wait_log('created game', 60)
        self.joiner = self.client('joiner', ['--mpjoin'], joiner_env or {})
        self.wait_log('starting with 2 players', 90)

    def client(self, name, mode, extra):
        env = environment(self.root, name)
        env.update(extra)
        log = open(self.root / (name + '.log'), 'w')
        return subprocess.Popen(
            [self.args.client, 'game', MAP, '--data', self.args.data, '--server', '127.0.0.1',
             '--serverport', str(self.port), *mode, '--time', str(TIME),
             *(['--user', name, '--pass', PASSWORD] if self.login else [])],
            stdout=log, stderr=subprocess.STDOUT, env=env)

    def log(self, name='server'):
        return (self.root / (name + '.log')).read_text(errors='replace')

    def wait_log(self, text, timeout, name='server'):
        deadline = time.monotonic() + timeout
        while text not in self.log(name):
            if time.monotonic() > deadline or self.server.poll() is not None:
                raise AssertionError('never saw %r in the %s log:\n%s' % (text, name, self.log(name)[-3000:]))
            time.sleep(0.1)

    def finish(self, proc, name, timeout=180):
        try:
            proc.wait(timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            raise AssertionError(name + ' did not finish:\n' + self.log(name)[-3000:])
        m = DONE.search(self.log(name))
        assert m, name + ' printed no result:\n' + self.log(name)[-3000:]
        return int(m.group(1)), m.group(2), m.group(4), int(m.group(3))

    def no_desync(self):
        text = self.log()
        assert 'DESYNCED' not in text and 'suspect' not in text.lower(), 'desync:\n' + text[-3000:]

    def close(self):
        for p in (getattr(self, 'host', None), getattr(self, 'joiner', None), self.server):
            if p and p.poll() is None:
                try:
                    p.send_signal(signal.SIGCONT)
                except OSError:
                    pass
                p.kill()
                p.wait()
        self.server_log.close()


def verify_replay(args, root, name):
    found = sorted((root / name / 'data').rglob('*.takrep'))
    assert len(found) == 1, ('replays', found)
    # The recording player's own profile, as a player watches: it holds the verified
    # game mods a data folder with game mods recorded into the replay.
    env = environment(root, name)
    env['TAK_REPLAY_VERIFY'] = '1'
    out = subprocess.run([args.client, 'replay', str(found[0]), '--data', args.data], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                         timeout=300).stdout
    m = re.search(r'replay done: tick=(\d+) hash=\w+ units=\d+ framed=\d+(.*)', out)
    assert m and 'DIVERGED' not in m.group(2), 'replay did not play back cleanly:\n' + out[-3000:]
    return out


def started(match, name, ticks=150):
    """Wait until `name` has played `ticks` ticks (its replay recording holds them)."""
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        parts = replays(match.root / name, '.takrep.part')
        if parts and complete_ticks(parts[0]) >= ticks:
            return
        time.sleep(0.1)
    raise AssertionError(name + ' never got going:\n' + match.log(name)[-3000:])


def case_takeover(args, root):
    m = Match(args, root)
    try:
        started(m, 'joiner')
        m.joiner.send_signal(signal.SIGSTOP)
        m.wait_log('waiting', 20)
        m.wait_log('AI (Normal) takes over', 20)
        tick, _hash, err, units = m.finish(m.host, 'host')
        assert err == 'none' and tick == TIME * 30, (tick, err)
        # Nobody else gives orders in this match: the units past the two Monarchs are the
        # AI's, built for the seat it took over.
        assert units > 2, ('the AI never built anything for the seat it took over', units)
        assert 'AI takeover at tick' in m.log('host'), 'the host never saw the takeover event'
        m.no_desync()
        out = verify_replay(args, root, 'host')
        assert 'AI takeover at tick' in out, 'the replay did not carry the takeover'
    finally:
        m.close()


def case_resume(args, root):
    m = Match(args, root)
    try:
        started(m, 'joiner')
        m.joiner.send_signal(signal.SIGSTOP)
        m.wait_log('stopped responding', 20)
        time.sleep(0.5)
        m.joiner.send_signal(signal.SIGCONT)
        m.wait_log('is back', 20)
        host, joiner = m.finish(m.host, 'host'), m.finish(m.joiner, 'joiner')
        assert host[2] == 'none' and joiner[2] == 'none', (host, joiner)
        assert host[0] == joiner[0] == TIME * 30 and host[1] == joiner[1], ('the peers disagree', host, joiner)
        assert 'AI (Normal) takes over' not in m.log(), 'a returning player was taken over'
        m.no_desync()
    finally:
        m.close()


def case_reconnect(args, root, login=False):
    m = Match(args, root, {'TAK_DROP_LINK_AT_TICK': '300'}, login)
    try:
        m.wait_log('RESEATED', 60)
        host, joiner = m.finish(m.host, 'host'), m.finish(m.joiner, 'joiner')
        assert 'reconnected -- catching up from tick' in m.log('joiner'), m.log('joiner')[-3000:]
        assert host[2] == 'none' and joiner[2] == 'none', (host, joiner)
        assert host[0] == joiner[0] == TIME * 30 and host[1] == joiner[1], ('the peers disagree', host, joiner)
        assert 'AI (Normal) takes over' not in m.log(), 'a reconnected player was taken over'
        m.no_desync()
        verify_replay(args, root, 'joiner')
    finally:
        m.close()


CASES = {'takeover': case_takeover, 'resume': case_resume, 'reconnect': case_reconnect,
         'reconnect-login': lambda args, root: case_reconnect(args, root, login=True)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', type=Path, required=True)
    ap.add_argument('--client', type=Path, required=True)
    ap.add_argument('--data', type=Path, required=True)
    ap.add_argument('--case', choices=sorted(CASES), action='append')
    a = ap.parse_args()
    if os.name == 'nt':
        print('SKIP: needs SIGSTOP')
        return
    a.server, a.client, a.data = a.server.resolve(), a.client.resolve(), a.data.resolve()
    failed = []
    for name in a.case or list(CASES):
        with tempfile.TemporaryDirectory(prefix='tak-disconnect-client-') as tmp:
            root = Path(tmp)
            try:
                CASES[name](a, root)
                print('ok   ' + name)
            except Exception:
                failed.append(name)
                print('FAIL ' + name)
                traceback.print_exc()
                for log in ('server', 'host', 'joiner'):
                    p = root / (log + '.log')
                    if p.exists():
                        print('---- %s (tail)\n%s' % (log, p.read_text(errors='replace')[-2500:]))
    if failed:
        raise SystemExit('FAIL: ' + ', '.join(failed))
    print('PASS: disconnect wait end to end -- AI takeover with no desync and a replay that verifies, '
          'resume inside the wait, reconnect inside the wait (same final hash on both peers), '
          'with and without an account login')


if __name__ == '__main__':
    sys.exit(main())
