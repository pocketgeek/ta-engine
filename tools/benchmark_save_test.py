#!/usr/bin/env python3
"""The Benchmark results' SAVE AS JPG button, clicked where it is drawn.

Each run is a menu Benchmark on a scratch profile, ended early with a full run's results
(TAK_BENCH_STOP_TICK, as benchmark_pathfinding does). TAK_BENCH_CLICKS then works the
pointer on the results screen with real mouse events, and TAK_BENCH_REPORT prints the
button and the note under it whenever either changes: BENCHSAVE (LIVE or GREYED, and
the label, both as the button was drawn) and BENCHNOTE (PLAIN or WARNING, and the text
as drawn).

A click saves one picture into the profile's `benchmarks` folder (made on first use): a
JPEG of the window's size, colour at full resolution, complete, with no temporary file
left beside it. The button then reads SAVED, greyed, and a second click saves nothing.
A click on the panel beside the button does not leave the screen. In a folder that
cannot be written the note is a warning, no file appears and the button stays live:
the same button saves once the folder can be written. That folder already holds a file
of every name the save could take, and of its -2: the picture goes in as -3 and none of
them is written over. Both buttons and the note must be inside the window, at the
smallest size too. The settings file is named outright (SDL_GetPrefPath ignores XDG_*
on macOS and Windows); the pictures go beside it.

usage: benchmark_save_test.py <takclient> <retail-install>
"""
import os, re, struct, subprocess, sys, tempfile, time

client, data = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])


def fail(why, log):
    sys.stdout.write(log)
    print("FAIL: " + why)
    sys.exit(1)


def start(root, name, size, clicks):
    """A menu Benchmark in a `size` window up to its results, then `clicks`; (process, log path, profile)."""
    home = os.path.join(root, name)
    os.makedirs(home, exist_ok=True)
    settings = os.path.join(home, "settings.ini")
    with open(settings, "w") as f:
        f.write("fullscreen = 0\n")
    env = dict(os.environ, XDG_CONFIG_HOME=home, XDG_DATA_HOME=home, SDL_VIDEODRIVER="dummy",
               SDL_AUDIODRIVER="dummy", TAK_SETTINGS_FILE=settings, TAK_MENU_PICK="benchmark", TAK_SPEED="80",
               TAK_QUIT_TICK="100000", TAK_SHOT_MS="100000", TAK_BENCH_STOP_TICK="45", TAK_BENCH_REPORT="1",
               TAK_BENCH_CLICKS=clicks)
    env.pop("TAK_LEGION", None)
    path = os.path.join(home, "log.txt")
    with open(path, "w") as out:
        proc = subprocess.Popen([client, "menu", "--data", data, "--shot", os.path.join(home, "shot.png"),
                                 "--winsize", str(size[0]), str(size[1])],
                                cwd=home, env=env, stdout=out, stderr=subprocess.STDOUT)
    return proc, path, home


def read(path):
    with open(path, errors="replace") as f:
        return f.read()


def finish(proc, path, name):
    try:
        code = proc.wait(timeout=120)
    except subprocess.TimeoutExpired:
        proc.kill()
        fail("%s: the run did not end" % name, read(path))
    if code != 0:
        fail("%s: exit code %d" % (name, code), read(path))
    return read(path)


def states(log, name, size):
    """The BENCHSAVE and BENCHNOTE lines in order, a repeat of the last one of its kind left
    out; each checked to be inside a `size` window and clear of DONE."""
    done = re.search(r"^BENCHDONE (\d+) (\d+) (\S+) (\S+) (\S+) (\S+)$", log, re.M)
    if not done:
        fail("%s: the results never drew DONE" % name, log)
    dx, dy, dw, dh = (float(v) for v in done.groups()[2:])
    out = []
    for m in re.finditer(r"^BENCH(SAVE \d+ \d+|NOTE) (\S+) (\S+) (\S+) (\S+) (LIVE|GREYED|PLAIN|WARNING) (.*)$", log, re.M):
        x, y, w, h = (float(v) for v in m.groups()[1:5])
        if x < 0 or y < 0 or x + w > size[0] or y + h > size[1]:
            fail("%s: '%s' must be inside a %s window" % (name, m[0], size), log)
        if m[1] != "NOTE":
            if m[1] != "SAVE %d %d" % size or (w, h) != (dw, dh) or y != dy or x + w > dx:
                fail("%s: '%s' must be DONE's size, beside it (%s)" % (name, m[0], done[0]), log)
        elif y < dy + dh:
            fail("%s: '%s' must be below the buttons (%s)" % (name, m[0], done[0]), log)
        state = ("NOTE" if m[1] == "NOTE" else "SAVE", m[6], m[7])
        if state not in [s for s in out if s[0] == state[0]][-1:]:
            out.append(state)
    return out


def button(seen):
    return [s[1:] for s in seen if s[0] == "SAVE"]


def pictures(home):
    folder = os.path.join(home, "benchmarks")
    return sorted(os.listdir(folder)) if os.path.isdir(folder) else []


def taken(folder, seconds):
    """A file in `folder` of every name a save in the next `seconds` could take (the
    client names a picture for the local second it is saved in), and of its -2;
    {name: its bytes}."""
    old = {}
    now = int(time.time())
    for second in range(now - 2, now + seconds):
        stamp = time.strftime("benchmark-%Y-%m-%d-%H%M%S", time.localtime(second))
        for name in (stamp + ".jpg", stamp + "-2.jpg"):
            old[name] = ("here before the save: " + name).encode()
            with open(os.path.join(folder, name), "wb") as f:
                f.write(old[name])
    return old


def jpeg(path, name, size, log):
    """`path` is a whole JPEG of `size`, its three planes all at that size (4:4:4)."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] != b"\xff\xd8" or d[-2:] != b"\xff\xd9":
        fail("%s: %s must start with SOI and end with EOI" % (name, path), log)
    at = 2
    while at + 4 <= len(d) and d[at] == 0xFF:
        marker, length = d[at + 1], struct.unpack(">H", d[at + 2:at + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2):   # SOF: precision, height, width, components (id, sampling, table)
            bits, h, w, n = struct.unpack(">BHHB", d[at + 4:at + 10])
            sampling = [d[at + 11 + 3 * c] for c in range(n)]
            if (w, h) != size or bits != 8 or sampling != [0x11] * 3:
                fail("%s: %s is %dx%d, %d bits, sampling %s; want %s, 8 bits, 4:4:4" %
                     (name, path, w, h, bits, sampling, size), log)
            return
        if marker == 0xDA:
            break
        at += 2 + length
    fail("%s: %s has no SOF segment" % (name, path), log)


with tempfile.TemporaryDirectory(prefix="tak-bench-save-") as root:
    # A click beside the button, the button, the button again: one picture.
    size = (1280, 720)
    proc, path, home = start(root, "save", size, "miss;save;save")
    log = finish(proc, path, "save")
    seen = states(log, "save", size)
    files = pictures(home)
    if len(files) != 1 or not re.fullmatch(r"benchmark-\d{4}-\d\d-\d\d-\d{6}\.jpg", files[0]):
        fail("save: one click and a second on the greyed button must leave one benchmark-<date>-<time>.jpg, got %s" % files, log)
    jpeg(os.path.join(home, "benchmarks", files[0]), "save", size, log)
    if button(seen) != [("LIVE", "SAVE AS JPG"), ("GREYED", "SAVED")]:
        fail("save: the button must go from SAVE AS JPG to a greyed SAVED and stay there, got %s" % seen, log)
    note = seen[-1]
    full = os.path.join(home, "benchmarks", files[0])
    if note[:2] != ("NOTE", "PLAIN") or not (note[2] == "SAVED: " + full or
                                              (note[2].startswith("SAVED: ...") and note[2].endswith(os.sep + files[0]))):
        fail("save: the note must read SAVED: and the picture's path, cut only from its start, got %s" % (note,), log)

    # The smallest window, and a folder that cannot be written, where every name the save
    # could take is a file already: a warning and no new file, until the folder is opened
    # up and the same button is clicked again. That picture is the -3, and the files that
    # were there are as they were. (The waits below give up after 240 seconds.)
    size = (640, 480)
    folder = os.path.join(root, "taken", "benchmarks")
    os.makedirs(folder)
    old = taken(folder, 300)
    os.chmod(folder, 0o555)
    if os.access(folder, os.W_OK):
        print("SKIP: this user writes in a read-only folder; the failed save is not checked")
        proc, path, home = start(root, "taken", size, "save;save")
    else:
        go = os.path.join(root, "go")
        proc, path, home = start(root, "taken", size, "save;until:%s;save;save" % go)
        deadline = time.time() + 120
        while "WARNING COULD NOT SAVE: " not in read(path):
            if proc.poll() is not None or time.time() > deadline:
                proc.kill()
                fail("taken: a save into a read-only folder must show COULD NOT SAVE", read(path))
            time.sleep(0.05)
        log = read(path)
        seen = states(log, "taken", size)
        new = [f for f in pictures(home) if f not in old]
        if new or button(seen) != [("LIVE", "SAVE AS JPG")]:
            fail("taken: a failed save must leave no file and a live SAVE AS JPG, got %s and %s" % (new, seen), log)
        os.chmod(folder, 0o755)
        open(go, "w").close()
    log = finish(proc, path, "taken")
    seen = states(log, "taken", size)
    for name, was in sorted(old.items()):
        with open(os.path.join(folder, name), "rb") as f:
            if f.read() != was:
                fail("taken: the save wrote over %s, which was there before it" % name, log)
    new = [f for f in pictures(home) if f not in old]
    third = re.fullmatch(r"(benchmark-\d{4}-\d\d-\d\d-\d{6})-3\.jpg", new[0]) if len(new) == 1 else None
    if not third or third[1] + ".jpg" not in old or third[1] + "-2.jpg" not in old:
        fail("taken: beside a benchmark-<date>-<time>.jpg and its -2 the save must add that name's -3 and nothing else, got %s"
             % new, log)
    if button(seen) != [("LIVE", "SAVE AS JPG"), ("GREYED", "SAVED")] or \
            seen[-1][:2] != ("NOTE", "PLAIN") or not seen[-1][2].endswith(os.sep + new[0]):
        fail("taken: once the folder can be written the same button must save one picture, got %s and %s" % (new, seen), log)
    jpeg(os.path.join(folder, new[0]), "taken", size, log)
print("PASS: SAVE AS JPG saves the Benchmark results once")
