#!/bin/bash
# speed_stall_check.sh -- does a game-speed change reach its rate after a client stall? (0.9.1 fix D)
# A client's own stall (alt-tab, a menu, a hitch, the load gap of a watch game) used to be read as
# network jitter; the jitter-buffer target then outgrew what the server's flow control lets a client
# hold, and the playout sat at its 0.7 floor: +10 (2x) played at 1.40x ("+4") for a minute or more.
#
#   tools/speed_stall_check.sh <debug build dir> [--watch | --seat] [--stall TICK:MS] [--set TICK]
#                              [--speed TENTHS] [--by-tick N | --within-s S] [--min-rate X]
#                              [--data DIR] [--cpus A-B] [--port N] [--seconds N] [--log DIR]
#
# --seat (default) plays Inner Circle --mpai against a local takserver; --watch spectates an 8-AI
# Legion game on a generated 8-player map as the host (the user's case). The client stalls once at
# --stall (TAK_STALL_AT), the host sets --speed at tick --set (TAK_SPEED_AT), and SPEEDLOG's
# once-a-second lines give the real playout rate. PASS when a 1-second window starting after the
# change plays at --min-rate (default 0.95 x the requested speed) or more, ending by tick --by-tick
# or within --within-s seconds of the change. Debug builds only (every hook is a dev.h env var).
#
# Defaults: --seat --stall 2000:1500 --set 2100 --speed 20 --by-tick 2400.
# The early-game watch case: --watch --stall 1:1200 --set 900 --within-s 3 (the stall at tick 1
# models the load gap of a watch game, which the server does not wait for).
set -u
BUILD=${1:-}; shift || true
[ -n "$BUILD" ] && [ -x "$BUILD/takclient" ] || { echo "usage: $0 <debug build dir> [options]" >&2; exit 2; }
MODE=seat; STALL=2000:1500; SET=2100; SPEED=20; BYTICK=2400; WITHIN=""; MINRATE=""
DATA=${TAK_DATA:-assets/game}; CPUS=${PACE_CPUS:-4-23}; PORT=7795; SECS=""; LOGDIR=""
while [ $# -gt 0 ]; do
  case "$1" in
    --watch) MODE=watch; shift;; --seat) MODE=seat; shift;;
    --stall) STALL=$2; shift 2;; --set) SET=$2; shift 2;; --speed) SPEED=$2; shift 2;;
    --by-tick) BYTICK=$2; WITHIN=""; shift 2;; --within-s) WITHIN=$2; BYTICK=""; shift 2;;
    --min-rate) MINRATE=$2; shift 2;;
    --data) DATA=$2; shift 2;; --cpus) CPUS=$2; shift 2;; --port) PORT=$2; shift 2;;
    --seconds) SECS=$2; shift 2;; --log) LOGDIR=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
done
[ -n "$MINRATE" ] || MINRATE=$(awk -v s="$SPEED" 'BEGIN{printf "%.2f", 0.95 * s / 10}')
# Client clock: the set tick at 1x, the stall, then room to show the rate (or its absence).
[ -n "$SECS" ] || SECS=$(( SET / 30 + 25 ))
work=${LOGDIR:-$(mktemp -d)}; mkdir -p "$work/xdg" "$work/home"
export HOME="$work/home" XDG_DATA_HOME="$work/xdg" TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
export TAK_SIM_THREAD=1 TAK_VSYNC_HZ=120 TAK_SPEED_AT="$SET:$SPEED"
[ -n "$STALL" ] && [ "$STALL" != none ] && export TAK_STALL_AT="$STALL"
taskset -c "$CPUS" "$BUILD/takserver" --port "$PORT" --data "$DATA" --no-auth --seed 1 --local > "$work/server.log" 2>&1 &
SP=$!
trap 'kill $SP 2>/dev/null; wait $SP 2>/dev/null' EXIT
for _ in $(seq 1 90); do grep -qi listening "$work/server.log" && break; sleep 1; done
if [ "$MODE" = watch ]; then
  TAK_MP_WATCH=1 TAK_MP_AIS=8 TAK_AI_LEVEL=4 TAK_LEGION=1 TAK_FOG=2 TAK_RANDOM_STARTS=2 \
    timeout $(( SECS * 3 + 120 )) taskset -c "$CPUS" "$BUILD/takclient" game \
    "~gen1~0a000100000000000000038001800108ffffffffff0004000000" --data "$DATA" --server 127.0.0.1 \
    --serverport "$PORT" --mphost --time "$SECS" > "$work/client.log" 2>&1
else
  timeout $(( SECS * 3 + 120 )) taskset -c "$CPUS" "$BUILD/takclient" game "Inner Circle" --data "$DATA" \
    --server 127.0.0.1 --serverport "$PORT" --mpai --time "$SECS" > "$work/client.log" 2>&1
fi
awk -v want="$SPEED" -v minr="$MINRATE" -v bytick="$BYTICK" -v within="$WITHIN" -v mode="$MODE" '
  $1 == "SPEEDLOG" && $2 == "stall" { stall = "stall " $7 " ms at tick " $5 }
  $1 == "SPEEDLOG" && $2 == "set" && $7 == want { setms = $3; settick = $5 }
  $1 == "SPEEDLOG" && $2 == "wall" {
    if (setms && pms >= setms) {
      r = ($5 - ptick) / (($3 - pms) / 1000.0) / 30.0
      rates = rates sprintf(" %.2f", r); ++n
      if (n <= 60) { sum += r; ++cnt }
      if (!hit && r >= minr) { hit = 1; hitTick = $5; hitS = ($3 - setms) / 1000.0 }
    }
    pms = $3; ptick = $5
  }
  END {
    if (!setms) { print "SPEEDSTALL FAIL: the speed change never happened (see client.log)"; exit 1 }
    ok = hit && (bytick != "" ? hitTick <= bytick + 0 : hitS <= within + 0)
    printf "SPEEDSTALL %s %s: %s, speed %d at tick %d; ", ok ? "PASS" : "FAIL", mode, stall ? stall : "no stall", want, settick
    if (hit) printf "%.2fx reached at tick %d, %.1f s after the change", minr, hitTick, hitS
    else printf "never reached %.2fx", minr
    printf " (limit %s); mean rate over the first %d s %.2fx\n", bytick != "" ? "tick " bytick : within " s", cnt, cnt ? sum / cnt : 0
    print "rates:" rates
    exit ok ? 0 : 1
  }' "$work/client.log"
rc=$?
grep -E '^mp-headless done|hash=' "$work/client.log" | tail -1
echo "logs: $work"
exit $rc
