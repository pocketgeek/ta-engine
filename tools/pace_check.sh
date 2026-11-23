#!/bin/bash
# pace_check.sh -- the IN-04 pacing run: an 8x Legion game on Inner Circle against a local takserver, the
# human seat forming armies (TAK_AUTOPLAY_FORMATION), the client logging every frame's ticks (TAK_PACELOG).
# Prints the client's PACE summary: ticks-per-frame histogram, zero-tick frames, frames at or over twice the
# nominal count, the longest burst and its tick, p99 frame ms, inbox p99 and the 512-tick fast-forward count.
#
#   tools/pace_check.sh <debug build dir> [--seconds GAME_SECONDS] [--cpus A-B] [--data DIR] [--speed TENTHS]
#                       [--frame-ms N] [--map NAME] [--port N] [--no-formation] [--no-thread] [--log DIR] [--allow-busy]
#                       [--vsync HZ] [--retail] [--bench LEVEL] [--slow-us N]
#
# Debug builds only (every hook it sets is a dev.h env var, compiled out of a release takclient). The run
# is REPORT-ONLY: nothing reads the numbers back as a gate. Defaults follow the audit: 8x (speed 80), 150
# game-seconds worth of ticks is --seconds 150 of client clock, two pinned cores (the client and the local
# server share them), the sim worker thread ON (the interactive default; --no-thread runs it inline), and a
# 50 ms frame (20 frames/s, the rate of the audited interactive run, whose mode was 12 ticks per frame).
#
# --vsync HZ replaces the fixed frame sleep with an HZ deadline (TAK_VSYNC_HZ, a fake vsync); the PRESENT line
# then gives the displayed-time CV and freeze share at that refresh rate (WE E3.2). --retail plays Retail
# pathfinding instead of Legion; --bench LEVEL watches the 8-AI Ulasem benchmark (TAK_BENCH) instead of --map.
# --slow-us N plays a slow client (TAK_SIM_SLOW_US: N us of sleep per simulated tick); the PACE line's
# backlog_p99 is then the flow-control backlog the WE S3 gate reads (40 ticks or fewer).
#
# The pace is only comparable run to run on idle cores: the script refuses a pinned core that is more than 2%
# busy, the same rule as tools/legion_timing.sh (--allow-busy runs anyway and says so).
set -u
BUILD=${1:-}; shift || true
[ -n "$BUILD" ] && [ -x "$BUILD/takclient" ] || { echo "usage: $0 <debug build dir> [options]" >&2; exit 2; }
SECS=460; CPUS=${PACE_CPUS:-22-23}; DATA=${TAK_DATA:-assets/game}; SPEED=80; FRAME=50; MAP="Inner Circle"; PORT=7793
FORMATION=1; THREAD=1; LOGDIR=""; BUSY_OK=0; VSYNC=""; LEGION=1; BENCH=""; SLOW=""
while [ $# -gt 0 ]; do
  case "$1" in
    --seconds) SECS=$2; shift 2;; --cpus) CPUS=$2; shift 2;; --data) DATA=$2; shift 2;;
    --speed) SPEED=$2; shift 2;; --frame-ms) FRAME=$2; shift 2;; --map) MAP=$2; shift 2;;
    --port) PORT=$2; shift 2;; --no-formation) FORMATION=0; shift;; --no-thread) THREAD=0; shift;;
    --log) LOGDIR=$2; shift 2;; --allow-busy) BUSY_OK=1; shift;;
    --vsync) VSYNC=$2; shift 2;; --retail) LEGION=0; shift;; --bench) BENCH=$2; shift 2;;
    --slow-us) SLOW=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
done
here=$(cd "$(dirname "$0")" && pwd)
if ! "$here/legion_timing.sh" --check-idle "$CPUS"; then
  [ "$BUSY_OK" = 1 ] && echo "pace_check: running on busy cores (--allow-busy); the numbers are not comparable" >&2 || exit 3
fi
work=${LOGDIR:-$(mktemp -d)}; mkdir -p "$work/xdg"
export XDG_DATA_HOME="$work/xdg" TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy
export TAK_SPEED=$SPEED TAK_PACELOG="$work/pace.txt" TAK_FRAME_MS=$FRAME
[ "$LEGION" = 1 ] && export TAK_LEGION=1
[ -n "$VSYNC" ] && export TAK_VSYNC_HZ=$VSYNC
[ -n "$BENCH" ] && export TAK_BENCH=$BENCH
[ -n "$SLOW" ] && export TAK_SIM_SLOW_US=$SLOW
[ "$FORMATION" = 1 ] && export TAK_AUTOPLAY_FORMATION=1
[ "$THREAD" = 1 ] && export TAK_SIM_THREAD=1
taskset -c "$CPUS" "$BUILD/takserver" --port "$PORT" --data "$DATA" --no-auth --seed 1 --local > "$work/server.log" 2>&1 &
SP=$!
trap 'kill $SP 2>/dev/null; wait $SP 2>/dev/null' EXIT
for _ in $(seq 1 90); do grep -qi listening "$work/server.log" && break; sleep 1; done
taskset -c "$CPUS" "$BUILD/takclient" game "$MAP" --data "$DATA" --server 127.0.0.1 --serverport "$PORT" --mpai --time "$SECS" > "$work/client.log" 2>&1
grep -E '^(PACE|PACEWORK|PRESENT|mp-headless done)' "$work/client.log"
echo "logs: $work (pace.txt: wall_ms ticks tick inbox buffered rate fast_forward shown td lock_wait_us world_ms capture_ms hash_ms job_wall_ms job_cpu_ms dtd nominal freeze gate snap lag_target backlog, one line per frame)"
