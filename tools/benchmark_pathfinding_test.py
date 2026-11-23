#!/usr/bin/env python3
"""Settings -> Benchmark's PATHFINDING choice, from the screen to the running worlds.

The screen: TAK_BENCH_MENU_TEST clicks the row where it is drawn and reads the settings
file back. The launch: TAK_MENU_PICK=benchmark starts the same menu launch a RELEASE
build makes, with the pick only in the settings file. A Legion game is told from a
Retail one by the sims themselves: only a world running Legion prints LPROBE lines
(TAK_LPROBE), and the client and its local server (the referee) both must. TAK_LEGION
still forces Legion. The run's stats panel and results must name the mode the sims ran,
and DONE must fit the window: TAK_BENCH_STOP_TICK ends each run with a full run's results,
and TAK_BENCH_REPORT prints what those two screens drew. The results also name the
SIMULATION THREADS the run was opened with, as the client logged them: the game's half and
its local server's, then AUTO or the saved count, and under it PIN SIM THREADS ON or OFF:
OFF unless the run's settings file turned PIN SIMULATION THREADS on. The AUTO run does
(simPin = 1, and TAK_SIM_PIN=1 over the 0 ctest gives every client): the game and its
server each log the CPUs they were handed, and no CPU is in both. The profile is scratch and
the settings file is named outright: SDL_GetPrefPath ignores XDG_* on macOS and Windows.

usage: benchmark_pathfinding_test.py <takclient> <retail-install>
"""
import os, re, subprocess, sys, tempfile

client, data = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])


def fail(why, log):
    sys.stdout.write(log)
    print("FAIL: " + why)
    sys.exit(1)


def run(root, name, saved, extra, args):
    """One client process on its own profile; returns its whole log."""
    home = os.path.join(root, name)
    os.makedirs(home)
    settings = os.path.join(home, "settings.ini")
    with open(settings, "w") as f:
        f.write(saved)
    env = dict(os.environ, XDG_CONFIG_HOME=home, XDG_DATA_HOME=home, SDL_VIDEODRIVER="dummy",
               SDL_AUDIODRIVER="dummy", TAK_SETTINGS_FILE=settings, **extra)
    if "TAK_LEGION" not in extra:
        env.pop("TAK_LEGION", None)
    shot = os.path.join(home, "shot.png")
    done = subprocess.run([client, "menu", "--data", data, "--shot", shot] + args, cwd=home, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
    if done.returncode != 0:
        fail("%s: exit code %d" % (name, done.returncode), done.stdout)
    return done.stdout


def named(log, name, mode, size):
    """The screens of a finished run name `mode`, and all of DONE is inside a `size` window."""
    if "BENCHSTATS PATHFINDER %s\n" % mode not in log:
        fail("%s: the stats panel must name %s" % (name, mode), log)
    if "BENCHRESULT PATHFINDING %s\n" % mode not in log:
        fail("%s: the results must name %s" % (name, mode), log)
    done = re.search(r"^BENCHDONE (\d+) (\d+) (\S+) (\S+) (\S+) (\S+)$", log, re.M)
    if not done:
        fail("%s: the results never drew DONE" % name, log)
    w, h, x, y, dw, dh = (float(v) for v in done.groups())
    if (w, h) != size or x < 0 or y < 0 or x + dw > w or y + dh > h:
        fail("%s: DONE at %s must be inside a %s window" % (name, done[0], size), log)


PINNED = r"( pinned to cpus ([\d,]+)(?:; \d+ threads? not pinned)?)?"


def threads(log, name, saved, pinned=False):
    """The results' SIM THREADS row is what the client logged for this run: its own simulation's half
    and its local server's, of `saved` threads or of AUTO (one for each performance core). With
    `pinned` (the run's settings turned PIN SIMULATION THREADS on), both processes also log the CPUs
    their threads are pinned to, and share none; the PIN SIM THREADS row reads ON then, else OFF."""
    row = re.search(r"^BENCHRESULT SIM THREADS (.*)$", log, re.M)
    if not row:
        fail("%s: the results must list SIM THREADS" % name, log)
    counts = re.fullmatch(r"CLIENT (\d+)  SERVER (\d+)  \((AUTO|\d+)\)", row[1])
    if not counts:
        fail("%s: SIM THREADS must read CLIENT n  SERVER n  (AUTO or the count), got '%s'" % (name, row[1]), log)
    game, server, total = int(counts[1]), int(counts[2]), counts[3]
    if total != (str(saved) if saved else "AUTO"):
        fail("%s: SIM THREADS names '%s', the run was opened with %s" % (name, total, saved or "AUTO"), log)
    if saved and (game, server) != ((saved + 1) // 2, max(1, saved // 2)):
        fail("%s: %d threads must be halved, got '%s'" % (name, saved, row[1]), log)
    if abs(game - server) > 1 or game < server:
        fail("%s: the two halves must be equal or the game's one more, got '%s'" % (name, row[1]), log)
    # Both processes logged it: the client its half and the server's, the server what it runs.
    if saved:
        logged = r"sim threads: %d \(of %d, half; local server %d\)" % (game, saved, server)
    elif "TAK_SIM_WORKERS" in os.environ:   # a debug build's override of AUTO
        logged = r"sim threads: %d \(auto: TAK_SIM_WORKERS; local server %d\)" % (game, server)
    else:   # AUTO is T threads, halved: the game's half is the bigger, and each is at least 1
        logged = r"sim threads: %d \(auto: (?:%d|%d), half; local server %d\)" % (game, game + server, 2 * game - 1, server)
    mine = re.search("^" + logged + PINNED + "$", log, re.M)
    if not mine:
        fail("%s: SIM THREADS reads '%s'; want the log line '%s'" % (name, row[1], logged), log)
    served = re.search(r"^sim threads: (\d+) \((?:auto: TAK_SIM_WORKERS|\d+ over 1 game)\)" + PINNED + "$", log, re.M)
    if not served or int(served[1]) != server:
        fail("%s: the local server must log the %d threads the row names" % (name, server), log)
    # Pinned (not on macOS, which pins no thread): the game has the first CPU of the hand-out,
    # its server the others' turn, and no CPU is in both.
    if pinned and sys.platform != "darwin":
        game_cpus = set((mine[2] or "").split(",")) - {""}
        server_cpus = set((served[3] or "").split(",")) - {""}
        if not game_cpus or game_cpus & server_cpus or (game + server > 2 and not server_cpus):
            fail("%s: pinned, the game and its server must each log their own CPUs (got '%s' and '%s')"
                 % (name, mine[0], served[0]), log)
    elif mine[1] or served[2]:
        fail("%s: nothing is pinned here, got '%s' and '%s'" % (name, mine[0], served[0]), log)
    # The row under SIM THREADS says which it was, as the run latched it.
    pin = re.search(r"^BENCHRESULT PIN SIM THREADS (.*)$", log, re.M)
    want = "ON" if pinned and sys.platform != "darwin" else "OFF"
    if not pin or pin[1] != want:
        fail("%s: the results must list PIN SIM THREADS %s, got '%s'" % (name, want, pin[0] if pin else "no row"), log)


def probes(root, name, saved, size, extra=None):
    """A menu Benchmark in a `size` window, up to its results; {tick: LPROBE lines printed} over both sims."""
    log = run(root, name, saved + "fullscreen = 0\n", dict({"TAK_MENU_PICK": "benchmark", "TAK_LPROBE": "30", "TAK_SPEED": "80",
                                                          "TAK_QUIT_TICK": "100000", "TAK_SHOT_MS": "100000", "TAK_BENCH_STOP_TICK": "45",
                                                          "TAK_BENCH_REPORT": "1"}, **(extra or {})),
              ["--winsize", str(size[0]), str(size[1])])
    ticks = {}
    for m in re.finditer(r"^LPROBE tick=(\d+) ", log, re.M):
        ticks[int(m[1])] = ticks.get(int(m[1]), 0) + 1
    return ticks, log


with tempfile.TemporaryDirectory(prefix="tak-bench-path-") as root:
    log = run(root, "screen", "", {"TAK_BENCH_MENU_TEST": "1"}, [])
    if "PASS: Benchmark pathfinding row picks and saves" not in log:
        fail("the Benchmark screen's row", log)

    ticks, log = probes(root, "legion", "benchmarkPathfinding = 4\nsimPin = 1\n", (1024, 768), {"TAK_SIM_PIN": "1"})
    if ticks.get(30, 0) < 2:
        fail("a saved Legion pick must run Legion on the client and the referee (got %s)" % ticks, log)
    named(log, "legion", "LEGION", (1024, 768))
    threads(log, "legion", 0, pinned=True)

    ticks, log = probes(root, "retail", "benchmarkPathfinding = 0\nsimThreads = 1\n", (1280, 800))
    if ticks:
        fail("a saved Retail pick must run Retail (got %s)" % ticks, log)
    if "connected to 127.0.0.1" not in log:
        fail("the Retail benchmark never reached its server", log)
    named(log, "retail", "RETAIL", (1280, 800))
    threads(log, "retail", 1)

    ticks, log = probes(root, "flag", "benchmarkPathfinding = 0\n", (640, 480), {"TAK_LEGION": "1"})
    if ticks.get(30, 0) < 2:
        fail("TAK_LEGION must still force Legion (got %s)" % ticks, log)
    named(log, "flag", "LEGION", (640, 480))
print("PASS: the Benchmark's pathfinding pick reaches the running worlds")
