#!/usr/bin/env python3
"""A won TA campaign mission is saved, end to end; a lost one is not.

TA's title screen CAMPAIGN opens the campaign picker; TA_FRONT_SCRIPT picks Arm Campaign
mission 1 (A Hero Returns, the next-up row), starts it through MSNBRIEF, and the mission
runs on the client and the local server it starts. TA_FORCE_WIN=<s> (debug builds) gives
the mission VictoryTimerRunsOut=<s> as its only victory rule, so the sim's condition
manager wins it -- the countdown, the outcome the server broadcasts -- and the game ends
itself into ENDMSN as retail's does. ENDMSN's Main Menu goes back to the picker, which
reads the saved settings again: the mission's row reads DONE (the picker logs its marks,
and its screenshot is kept), and settings.ini holds campaigndone.arm campaign = 0.
TA_FORCE_LOSE=<s> adds DeathTimerRunsOut=<s> instead: ENDMSN marks it Lost, and nothing
is saved. Each run has its own scratch profile, and names its settings file outright
(SDL_GetPrefPath ignores XDG_* on macOS and Windows).

usage: campaign_win_test.py <taclient> <retail-install> [<keep-dir>]
"""
import os, re, shutil, subprocess, sys, tempfile

client, data = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
keep = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 else None


def fail(why, log):
    sys.stdout.write(log)
    print("FAIL: " + why)
    sys.exit(1)


def run(root, name, force):
    """One client: the picker, mission 1, ENDMSN, the picker again, the title's EXIT."""
    home = os.path.join(root, name)
    os.makedirs(home)
    settings = os.path.join(home, "settings.ini")
    with open(settings, "w") as f:
        f.write("fullscreen = 0\n")
    shot = os.path.join(home, "picker.png")
    script = ",".join(["CAMPAIGN", "START", "START",           # picker -> MSNBRIEF -> the mission
                       "WAIT", "SHOT:" + os.path.join(home, "endmsn.png"),
                       "MAINMENU", "WAIT", "SHOT:" + shot,      # ENDMSN -> the picker
                       "PREVMENU", "EXIT"])                     # the title screen, and out
    env = dict(os.environ, XDG_CONFIG_HOME=home, XDG_DATA_HOME=home, SDL_VIDEODRIVER="dummy",
               SDL_AUDIODRIVER="dummy", TA_SETTINGS_FILE=settings, TA_FRONT_SCRIPT=script, **force)
    for other in ("TA_FORCE_WIN", "TA_FORCE_LOSE"):
        if other not in force:
            env.pop(other, None)
    try:
        done = subprocess.run([client, "--data", data, "--winsize", "1024", "768"], cwd=home, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240)
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        fail("%s: the client did not finish (stuck in the mission or on a screen)" % name, out)
    log = done.stdout
    if done.returncode != 0:
        fail("%s: exit code %d" % (name, done.returncode), log)
    with open(settings) as f:
        saved = f.read()
    if keep:
        os.makedirs(keep, exist_ok=True)
        for f in ("endmsn.png", "picker.png"):
            if os.path.exists(os.path.join(home, f)):
                shutil.copy(os.path.join(home, f), os.path.join(keep, name + "-" + f))
    return log, saved, shot


def expect(log, name, *lines):
    """Each line (a regex) is in the log, in this order."""
    at = 0
    for line in lines:
        m = re.compile(line, re.M).search(log, at)
        if not m:
            fail("%s: no '%s' after the earlier steps" % (name, line), log)
        at = m.end()


with tempfile.TemporaryDirectory(prefix="ta-campaign-win-") as root:
    # The win: recorded, and the picker reads DONE.
    log, saved, shot = run(root, "win", {"TA_FORCE_WIN": "5"})
    if len(re.findall(r"^mission AC01\.ota:\d: TA_FORCE_WIN=5 ", log, re.M)) != 2:
        fail("win: the client and its local server must both build the forced mission", log)
    expect(log, "win",
           r"^campaign picker: arm campaign: DONE \[\], PLAY 0$",       # a fresh profile
           r"^front-end script: START -> Start$",                        # MSNBRIEF's Start
           r"^game 1 mission AC01\.ota:\d: VICTORY$",                    # the server's referee
           r"^front end: ENDMSN -- Arm Campaign mission 1 won$",
           r"^front-end script: MAINMENU -> MainMenu$",
           r"^campaign picker: arm campaign: DONE \[0\], PLAY 1$",
           r"^front-end shot: .*picker\.png$",
           r"^front-end script: EXIT -> EXIT$")
    if not re.search(r"^campaigndone\.arm campaign = 0$", saved, re.M):
        fail("win: settings.ini must record Arm Campaign mission 1, got:\n" + saved, log)
    if not os.path.getsize(shot):
        fail("win: no picker screenshot", log)

    # The loss: ENDMSN says Lost, and nothing is saved.
    log, saved, _ = run(root, "loss", {"TA_FORCE_LOSE": "5"})
    expect(log, "loss",
           r"^campaign picker: arm campaign: DONE \[\], PLAY 0$",
           r"^game 1 mission AC01\.ota:\d: DEFEAT$",
           r"^front end: ENDMSN -- Arm Campaign mission 1 lost$",
           r"^front-end script: MAINMENU -> MainMenu$",
           r"^campaign picker: arm campaign: DONE \[\], PLAY 0$",
           r"^front-end script: EXIT -> EXIT$")
    if "VICTORY" in log:
        fail("loss: the mission must not be won", log)
    if "campaigndone" in saved:
        fail("loss: settings.ini must record nothing, got:\n" + saved, log)
print("PASS: a won campaign mission is saved and the picker marks it DONE; a lost one is not")
