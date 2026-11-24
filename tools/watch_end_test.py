#!/usr/bin/env python3
"""A watched game ends for its watcher when the server ends it, on the winner's plate.

Real taserver (--seed 1, --replaydir) and headless taclients on Ashap Plateau, each in
its own scratch profile. The watcher runs the watch probe (TA_WATCH_END_TEST): it plays
on past the result until the server ends the game (Msg::GameOver), then through the
three-second results transition, and reports the result plate it would show.

  allai   The game the fix is for: the client hosts a room of two AIs and watches it
          (as single-player's WATCH does), the AIs starting with stress armies so the
          match is decided within minutes of game time. Nobody is seated, so the server
          ends the game on the very tick the result lands, instead of simulating the
          winner for as long as the watcher stays.
  leaver  A player (slot 1, Arm) against one server AI (slot 2, Core), and a
          watcher that joined as a spectator. The player destroys its own Commander and
          its client leaves at the result, as a player's does; with no human seated any
          more the server ends the game moments later.

In both:
  - the watcher is told the game is over once it has played its last tick, and three
    seconds later goes to the result screen, as a player does;
  - its plate is the winner's, not slot 1's: VICTORY on the winner's side, the winner
    named and its row the highlighted one, the match length the result's tick;
  - its recording and the server's own replay both end on the game's last tick, and
    both play back (TA_REPLAY_VERIFY) to that tick and to the hash the watcher ended on;
    its log does not say the recording continues past the result, as a player's does;
  - played back, each ends under the banner naming the winner (the verify run's
    "replay result" line), as the game did for its watcher.

  undecided  the control for that banner: a game stopped by its time limit with both
          sides alive plays back to its end with no winner named.
"""
import argparse
import concurrent.futures
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from replay_always_test import MAP, environment, free_port, replays, verify

PLATE = re.compile(r"PASS: watched game over: results after (\d+) ms at tick (\d+), result tick (\d+); "
                   r"plate victory=(\d) faction=(-?\d+) line='([^']*)' band='([^']*)' matchSec=(\d+) gold=(.*)")


def text(root, name='client.log'):
    return (root / name).read_text(errors='replace')


def wait_for(path, needle, process, seconds):
    deadline = time.monotonic() + seconds
    while needle not in path.read_text(errors='replace'):
        if process.poll() is not None or time.monotonic() > deadline:
            raise AssertionError('never saw %r in %s:\n%s' % (needle, path, path.read_text(errors='replace')))
        time.sleep(0.1)


class Session:
    """A server and the clients of one game; everything it started is stopped on close."""

    def __init__(self, args, base):
        self.args, self.base = args, base
        self.roots = {}
        self.started, self.logs = [], []
        self.served = base / 'server-replays'
        self.served.mkdir(parents=True)
        self.port = free_port()
        root = self.root('server')
        log = open(root / 'server.log', 'w')
        self.logs.append(log)
        self.server = subprocess.Popen(
            [args.server, '--port', str(self.port), '--data', args.data, '--no-auth', '--local', '--seed', '1',
             '--replaydir', str(self.served)], stdout=log, stderr=subprocess.STDOUT, env=environment(root))
        self.started.append(self.server)
        wait_for(root / 'server.log', 'listening', self.server, 90)

    def root(self, name):
        if name not in self.roots:
            self.roots[name] = self.base / name
            self.roots[name].mkdir(parents=True)
        return self.roots[name]

    def client(self, name, mode, extra, limit):
        root = self.root(name)
        env = environment(root)
        env.update(extra)
        log = open(root / 'client.log', 'w')
        self.logs.append(log)
        process = subprocess.Popen(
            [self.args.client, 'game', MAP, '--data', self.args.data, '--server', '127.0.0.1',
             '--serverport', str(self.port), mode, '--time', str(limit)], stdout=log, stderr=subprocess.STDOUT, env=env)
        self.started.append(process)
        return process

    def server_log(self):
        return text(self.roots['server'], 'server.log')

    def close(self):
        for process in self.started:
            if process.poll() is None:
                process.kill()
                process.wait()
        for log in self.logs:
            log.close()


def watched(session, watcher, seconds):
    """Wait for the watcher to leave by itself; returns what its plate said."""
    try:
        code = watcher.wait(seconds)
    except subprocess.TimeoutExpired:
        raise AssertionError('the watcher never left the game the server should have ended:\n' +
                             text(session.roots['watcher']) + '\n--- server\n' + session.server_log()[-2500:])
    log = text(session.roots['watcher'])
    assert code == 0, log
    wait_for(session.roots['server'] / 'server.log', 'ended (match over)', session.server, 10)
    assert 'spectat' in log.lower(), log
    over = re.search(r'watch: the server ended the game after (\d+) ticks \(team (\d+) wins\)', log)
    assert over, 'the watcher was not told the game is over, with its winner:\n' + log
    plate = PLATE.search(log)
    assert plate, 'the watcher did not go to its result screen:\n' + log
    done = re.search(r'mp-headless done: tick=(\d+) hash=(\w+)', log)
    assert done, log
    out = dict(ticks=int(over.group(1)), team=int(over.group(2)), at=int(plate.group(2)), result=int(plate.group(3)),
               victory=int(plate.group(4)), faction=int(plate.group(5)), line=plate.group(6), band=plate.group(7),
               seconds=int(plate.group(8)), gold=plate.group(9).strip(), end_tick=int(done.group(1)), hash=done.group(2))
    assert out['at'] == out['ticks'] == out['end_tick'], ('the watcher did not play the game to its last tick', out)
    assert ('game 1 over after %d ticks (match over): team %d wins' % (out['ticks'], out['team'])) in session.server_log(), \
        session.server_log()[-2000:]
    # The winner's plate: VICTORY, one winner named and highlighted, timed at the result.
    assert out['victory'] == 1 and not out['band'], ('the watcher\'s plate is not a victory plate', out)
    assert out['gold'] and ',' not in out['gold'] and out['line'] == out['gold'] + ' wins', ('the plate does not name the winner', out)
    assert out['seconds'] == out['result'] // 30, ('the match length is not the result\'s tick', out)
    # A watcher's recording goes on only while the game does: its log says where the server
    # ended it, or (the result seen before the server's word) that it continues that long.
    said = re.search(r'replay: result at tick \d+, \d+ ticks recorded; (.*)', log)
    ends = 'the server ended the game after %d ticks, and the recording ends there' % out['ticks']
    assert said and said.group(1) in (ends, 'recording continues while the game does'), \
        ('the watcher\'s log says its recording goes on past the game', said and said.group(1))
    return out


def replays_end(session, out):
    """Both recordings end on the game's last tick and play back to the watcher's end state."""
    kept = sorted(session.served.glob('*.tarep'))
    own = replays(session.roots['watcher'], '.tarep')
    assert len(kept) == 1 and len(own) == 1 and not replays(session.roots['watcher'], '.tarep.part'), (kept, own)
    for what, path in (('server', kept[0]), ('watcher', own[0])):
        ticks, played = verify(session.args, session.root('verify'), path)
        end = re.search(r'replay done: tick=\d+ hash=(\w+)', played).group(1)
        assert ticks == out['ticks'], ('the %s\'s replay does not end on the game\'s last tick' % what, ticks, out['ticks'])
        assert end == out['hash'], ('the %s\'s replay does not end as the game did' % what, end, out['hash'])
        assert 'replay result: team %d wins' % out['team'] in played, \
            'the %s\'s replay does not end naming the winner:\n%s' % (what, played[-600:])


def case_allai(args, base):
    session = Session(args, base)
    try:
        # TA_MP_WATCH: the host takes no seat; TA_STRESS + TA_UNITCAP=250: each AI opens
        # with an army, so they are at each other at once.
        watcher = session.client('watcher', '--mphost', {
            'TA_WATCH_END_TEST': '1', 'TA_MP_WATCH': '1', 'TA_MP_AIS': '2', 'TA_STRESS': '1',
            'TA_UNITCAP': '250', 'TA_SPEED': '80'}, 1800)
        out = watched(session, watcher, 420)
    finally:
        session.close()
    # Nobody was ever seated: the game ends on the tick its result landed.
    assert out['ticks'] == out['result'], ('the server did not end an all-AI game on its result\'s tick', out)
    assert out['faction'] == out['team'] - 1, ('the plate is not the winning AI\'s side', out)   # AI n plays side n-1
    replays_end(session, out)
    return ('decided at tick %(result)d and ended there by the server; plate: victory for %(gold)s (faction '
            '%(faction)d), %(seconds)d s; both replays end at tick %(ticks)d, hash %(hash)s' % out)


def case_leaver(args, base):
    session = Session(args, base)
    try:
        # The watcher first: it polls the game list and watches the game from its start.
        watcher = session.client('watcher', '--mpspectate', {'TA_WATCH_END_TEST': '1'}, 600)
        wait_for(session.roots['server'] / 'server.log', 'joined lobby', session.server, 90)   # listed before the game exists
        # TA_DEFEAT_TEST=2: the player self-destroys its Commander and the run ends at its result.
        player = session.client('player', '--mpai', {'TA_DEFEAT_TEST': '2'}, 120)
        assert player.wait(150) == 0, 'the player did not finish:\n' + text(session.roots['player'])
        out = watched(session, watcher, 60)
    finally:
        session.close()
    played = re.search(r'mp-headless done: tick=(\d+) .* end=concluded result=(\d+)', text(session.roots['player']))
    assert played, text(session.roots['player'])
    result = int(played.group(2))
    assert out['result'] == result, ('player and watcher saw the result at different ticks', result, out)
    # The server ends the game as the last human leaves, moments after the result.
    assert result <= out['ticks'] <= result + 150, ('the server did not end the game when the player left', result, out)
    assert out['team'] == 2 and out['faction'] == 1, ('the watcher\'s plate is not the winner\'s (Core, slot 2)', out)
    replays_end(session, out)
    return ('result at tick %(result)d, ended by the server after %(ticks)d ticks; plate: victory for %(gold)s '
            '(faction %(faction)d), %(seconds)d s; both replays end at tick %(ticks)d, hash %(hash)s' % out)


def case_undecided(args, base):
    session = Session(args, base)
    try:
        player = session.client('player', '--mpai', {}, 5)
        assert player.wait(120) == 0, text(session.roots['player'])
    finally:
        session.close()
    assert 'end=timelimit' in text(session.roots['player']), text(session.roots['player'])
    own = replays(session.roots['player'], '.tarep')
    assert len(own) == 1, own
    ticks, played = verify(args, session.root('verify'), own[0])
    assert 'replay result' not in played, 'an undecided game\'s replay ends naming a winner:\n' + played[-600:]
    return 'a replay of %d ticks that ended undecided names no winner' % ticks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', required=True)
    ap.add_argument('--client', required=True)
    ap.add_argument('--data', required=True)
    ap.add_argument('--case', action='append', help='run only these cases')
    args = ap.parse_args()
    cases = {'allai': case_allai, 'leaver': case_leaver, 'undecided': case_undecided}
    names = args.case or list(cases)
    base = Path(tempfile.mkdtemp(prefix='ta-watch-end-'))
    failed = False
    try:
        with concurrent.futures.ThreadPoolExecutor(len(names)) as pool:
            jobs = {name: pool.submit(cases[name], args, base / name) for name in names}
            for name, job in jobs.items():
                try:
                    print('PASS %s: %s' % (name, job.result()), flush=True)
                except Exception as e:   # report every case, then fail
                    failed = True
                    print('FAIL %s: %s' % (name, e), flush=True)
    finally:
        if not failed:
            shutil.rmtree(base, ignore_errors=True)
        else:
            print('logs kept in', base)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
