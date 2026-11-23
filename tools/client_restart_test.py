#!/usr/bin/env python3
"""takclient's RESTART NOW for a new GAME DATA FOLDER, end to end (POSIX exec).

The debug menu pick TAK_MENU_PICK=restart:<folder> does what Options does on a valid
pick and RESTART NOW: saves the folder to the settings file and leaves the menu. main()
then tears down and execs itself with the same arguments. The new process must mount
the NEW folder (it was started without --data, from a folder that is not an install, so
the saved setting decides) and must not restart again. The profile is a scratch
XDG_* tree and the settings file is named by TAK_SETTINGS_FILE (inherited across the
exec): SDL_GetPrefPath ignores XDG_* on macOS, so XDG alone would reach the player's.

usage: client_restart_test.py <takclient> <retail-install>
"""
import os, subprocess, sys, tempfile

client, data = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
with tempfile.TemporaryDirectory(prefix="tak-client-restart-") as root:
    alt = os.path.join(root, "alt-install")   # the same install under another name
    os.symlink(data, alt)
    settings = os.path.join(root, "settings.ini")
    with open(settings, "w") as f:
        f.write("dataDir = %s\n" % data)
    env = dict(os.environ, XDG_CONFIG_HOME=root, XDG_DATA_HOME=root, SDL_VIDEODRIVER="dummy",
               SDL_AUDIODRIVER="dummy", TAK_MENU_PICK="restart:" + alt, TAK_SETTINGS_FILE=settings)
    shot = os.path.join(root, "after-restart.png")
    run = subprocess.run([client, "--winsize", "640", "480", "--shot", shot], cwd=root, env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=150)
    log = run.stdout
    def fail(why):
        sys.stdout.write(log)
        print("FAIL: " + why)
        sys.exit(1)
    if run.returncode != 0: fail("exit code %d" % run.returncode)
    if log.count("restart: relaunching") != 1: fail("expected exactly one relaunch")
    first, _, second = log.partition("restart: relaunching")
    if "from %s/Movies/Gui" % data not in first: fail("the first process did not mount the saved folder")
    if "from %s/Movies/Gui" % alt not in second: fail("the restarted process did not mount the new folder")
    if not os.path.exists(shot): fail("the restarted process did not reach the menu")
    if ("dataDir = %s\n" % alt) not in open(settings).read(): fail("the new folder is not the saved setting")
print("PASS: RESTART NOW relaunches once and the new process mounts the new data folder")
