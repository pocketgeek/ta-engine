#!/usr/bin/env python3
"""Drive a live retail Kingdoms window through its matching Proton runtime.

Usage: python3 tools/re/retail_driver.py [--game-pid LINUX_PID]
       [--window-pid WINDOWS_PID] info|move|click|doubleclick|drag|key|hotkey|text|sequence|close ...

Examples:
  tools/re/retail_driver.py info
  tools/re/retail_driver.py move 320 240 500
  tools/re/retail_driver.py click 320 240 left
  tools/re/retail_driver.py doubleclick 320 240 left
  tools/re/retail_driver.py key 27
  tools/re/retail_driver.py text +atm
  tools/re/retail_driver.py sequence tools/re/example.sequence

The target Linux game's mapped binary identifies the Proton build and
compatibility prefix. The Win32 bridge then enumerates visible Kingdoms
windows and selects the only one, or asks for an explicit Windows PID.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


REPO = Path(__file__).resolve().parents[2]
BRIDGE_SOURCE = REPO / "tools/re/retail_input.c"
BRIDGE = Path(tempfile.gettempdir()) / f"tak_retail_input_{os.getuid()}.exe"


def mapped_retail_pids():
    found = []
    for proc in Path("/proc").glob("[0-9]*"):
        try:
            maps = (proc / "maps").read_text(errors="ignore").splitlines()
            if any("KINGDOMS.icd" in line and line.startswith("00400000-")
                   for line in maps):
                found.append(int(proc.name))
        except (OSError, ValueError):
            continue
    return found


def target_pid(requested):
    candidates = [requested] if requested is not None else mapped_retail_pids()
    if len(candidates) != 1:
        raise RuntimeError(
            f"expected one mapped retail process, found {candidates}; "
            "choose one with --game-pid LINUX_PID"
        )
    return candidates[0]


def runtime_for(pid):
    proc = Path(f"/proc/{pid}")
    executable = os.readlink(proc / "exe")
    # Proton's wine preloader lives in files/bin/ (older builds) or in
    # files/lib/wine/<arch>-unix/ (umu + Proton-CachyOS and newer).
    marker = next((m for m in ("/files/bin/", "/files/lib/") if m in executable), None)
    if marker is None:
        raise RuntimeError(f"game process does not appear to use Proton: {executable}")
    proton = Path(executable.split(marker, 1)[0]) / "proton"
    if not proton.is_file():
        raise RuntimeError(f"matching Proton launcher not found: {proton}")

    selected = {}
    for entry in (proc / "environ").read_bytes().split(b"\0"):
        key, separator, value = entry.partition(b"=")
        if separator and key in {b"STEAM_COMPAT_DATA_PATH", b"WINEPREFIX",
                                 b"STEAM_COMPAT_CLIENT_INSTALL_PATH"}:
            selected[key.decode()] = value.decode(errors="replace")
    if not selected.get("STEAM_COMPAT_DATA_PATH") or not selected.get("WINEPREFIX"):
        raise RuntimeError("could not read the game's Proton compatibility prefix")
    env = os.environ.copy()
    env.update(selected)
    return proton, env


def bridge_path():
    if not BRIDGE.exists() or BRIDGE.stat().st_mtime < BRIDGE_SOURCE.stat().st_mtime:
        compiler = "x86_64-w64-mingw32-gcc"
        try:
            subprocess.run(
                [compiler, "-Wall", "-Wextra", "-Werror", "-O2", "-o",
                 str(BRIDGE), str(BRIDGE_SOURCE)], check=True
            )
        except FileNotFoundError as error:
            raise RuntimeError(f"required compiler not found: {compiler}") from error
        except subprocess.CalledProcessError as error:
            raise RuntimeError("failed to compile the retail input bridge") from error
    return BRIDGE


def run_bridge(proton, env, helper, args, capture=False, input_data=None):
    return subprocess.run(
        [str(proton), "runinprefix", str(helper), *args], env=env,
        check=False, text=True, stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None, input=input_data,
    )


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--game-pid", type=int,
                        help="Linux PID of the retail game (auto-detected if unique)")
    parser.add_argument("--window-pid", type=int,
                        help="Win32 PID shown by the windows command if several windows exist")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="retail_input command and its arguments")
    args = parser.parse_args()
    if not args.command:
        parser.error("provide a bridge command, for example: info")

    try:
        pid = target_pid(args.game_pid)
        proton, env = runtime_for(pid)
        helper = bridge_path()
        bridge_command = args.command
        sequence_input = None
        if args.command[0] == "sequence":
            if len(args.command) != 2:
                raise RuntimeError("sequence expects a script path, or '-' to read the script from stdin")
            sequence_path = args.command[1]
            if sequence_path == "-":
                sequence_input = sys.stdin.read()
            else:
                sequence_input = Path(sequence_path).read_text()
            if not sequence_input.strip():
                raise RuntimeError("sequence script is empty")
            bridge_command = ["sequence"]
        windows = run_bridge(proton, env, helper, ["windows"], capture=True)
        if windows.returncode:
            sys.stderr.write(windows.stderr)
            return windows.returncode
        lines = [line for line in windows.stdout.splitlines() if line.startswith("pid=")]
        pids = [int(match.group(1)) for line in lines
                if (match := re.match(r"pid=(\d+)\b", line))]
        if args.window_pid is not None:
            if args.window_pid not in pids:
                raise RuntimeError(
                    f"Windows PID {args.window_pid} is not a visible Kingdoms window; "
                    f"available: {pids}"
                )
            selected_window = args.window_pid
        elif len(pids) == 1:
            selected_window = pids[0]
        else:
            sys.stderr.write(windows.stdout)
            raise RuntimeError(
                "several Kingdoms windows are visible; select one with --window-pid"
            )

        result = run_bridge(
            proton, env, helper, ["--pid", str(selected_window), *bridge_command],
            input_data=sequence_input,
        )
        return result.returncode
    except (OSError, RuntimeError) as error:
        print(f"retail_driver: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
