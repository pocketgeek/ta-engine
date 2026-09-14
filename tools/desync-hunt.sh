#!/usr/bin/env bash
# desync-hunt.sh -- long-running 8-AI games across many setting combinations, in
# parallel, looking for one thing: a client whose state hash stops matching the
# server's referee.
#
# WHY THIS SHAPE. A desync needs three things to surface: enough SIM SURFACE that
# divergent code actually runs (8 AIs fighting exercise production, combat, pathing,
# gods, reclaim, transports), enough TIME that a rare divergence has a chance to
# happen, and enough SETTING VARIETY that option-gated code paths are covered. The
# usual 60s two-unit smoke run has none of those, which is why it reported a clean
# hash right through the god-summoning desync.
#
# HOW A DESYNC IS DETECTED. The server refs its own sim and compares every client's
# reported hash against it (Server::checkHashes), logging "client N DESYNCED at tick
# T". That comparison only happens when a HUMAN SLOT IS SEATED -- checkHashes returns
# on entry when `live == 0`, and a spectator sends a zero hash by design -- so every
# run here seats the client as a player (7 AIs + 1 human). An all-spectator sweep
# reports "no desyncs" having compared nothing at all. "REFEREE SUSPECT" is the opposite finding -- every client agrees and the SERVER
# is the odd one out. Both are failures here and both are grepped for; a run that
# ends any way other than cleanly is also reported, because a crash or a dropped
# connection hides whatever it was about to tell us.
#
# Each combination gets its own server on its own port and its own log pair, so runs
# are independent and can be read after the fact.
#
# usage: tools/desync-hunt.sh [--minutes N] [--jobs N] [--data DIR] [--quick]
set -u

DATA="assets/game"
MINUTES=20
JOBS=4
QUICK=0
while [ $# -gt 0 ]; do
  case "$1" in
    --minutes) MINUTES="$2"; shift 2;;
    --jobs)    JOBS="$2"; shift 2;;
    --data)    DATA="$2"; shift 2;;
    --quick)   QUICK=1; MINUTES=3; shift;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

CLIENT=./build-dbg/takclient
SERVER=./build-dbg/takserver
[ -x "$CLIENT" ] && [ -x "$SERVER" ] || { echo "build-dbg binaries missing -- cmake --build build-dbg" >&2; exit 2; }

OUT="${TMPDIR:-/tmp}/desync-hunt-$$"
mkdir -p "$OUT"
echo "desync hunt: ${MINUTES}m per run, ${JOBS} in parallel, logs in $OUT"

# One line per combination: NAME then the env/flags that define it. Chosen to cover
# the option-gated paths rather than to enumerate the full cross product, which would
# be thousands of runs: each line turns on something that has its own sim code.
#
# Maps are varied too -- Ulasem is roomy (placement never caps), Inner Circle is
# cramped (it does), and the water maps exercise transports and naval pathing.
# Every run is cadenced at 4x (TAK_SPEED=40) unless it overrides it. The harness is
# WALL-CLOCK paced, not CPU bound -- a 24-core box sat at 0.68 load running one game --
# so the limit on how much game time a hunt covers is the clock, not the machine.
# Speed re-cadences how fast ticks are ISSUED; the per-tick dt is fixed and the sim is
# bit-identical either way, so this buys 4x the coverage for nothing. One run below
# deliberately stays at 1x as a control, in case the cadence itself ever matters.
SPEED_DEFAULT="TAK_SPEED=40"

RUNS=(
  "baseline|Ulasem Arena||"
  "crusades|Ulasem Arena||--crusades"
  "gods|Ulasem Arena|TAK_GODS=1|"
  "stress|Ulasem Arena|TAK_STRESS=1|"
  "unitcap-250|Ulasem Arena|TAK_UNITCAP=250|"
  "unitcap-5000|Ulasem Arena|TAK_UNITCAP=5000|"
  "fog-explored|Ulasem Arena|TAK_FOG=1|"
  "fog-full|Ulasem Arena|TAK_FOG=2|"
  "ai-absurd|Ulasem Arena|TAK_AI_LEVEL=4|"
  "ai-passive|Ulasem Arena|TAK_AI_LEVEL=0|"
  "speed-4x|Ulasem Arena|TAK_SPEED=40|"
  "bench-high|Ulasem Arena|TAK_BENCH=3|"
  "bench-absurd|Ulasem Arena|TAK_BENCH=6|"
  "cramped|Inner Circle||"
  "cramped-stress|Inner Circle|TAK_STRESS=1|"
  "naval|Aibel's Seaport||"
  "naval-crusades|Aibel's Seaport||--crusades"
  "lake|Lake Lokken|TAK_STRESS=1|"
  "overrides-full|Ulasem Arena||--overrides full"
  "crusades-absurd|Tarosian Plain|TAK_AI_LEVEL=4|--crusades"
  "everything|Tarosian Plain|TAK_GODS=1 TAK_STRESS=1 TAK_AI_LEVEL=4 TAK_FOG=1|--crusades"
  "gods-random-starts|Ulasem Arena|TAK_GODS=1 TAK_RANDOM_STARTS=1|"
  "gods-cramped|Inner Circle|TAK_GODS=1|"
  "gods-crusades|Rift of Grief|TAK_GODS=1|--crusades"
  "monarch-expendable|Ulasem Arena|TAK_MONARCH_EXPENDABLE=1 TAK_GODS=1|"
  "forfeit-selfdestruct|Ulasem Arena|TAK_FORFEIT_SELFDESTRUCT=1|"
  "random-starts|Sand River Plain|TAK_RANDOM_STARTS=1|"
  "speed-1x-control|Ulasem Arena|TAK_SPEED=10|"
  "overrides-cosmetic|Ulasem Arena||--overrides cosmetic"
  "blood-and-roses|Blood and Roses|TAK_GODS=1 TAK_STRESS=1|"
  "two-castles|Two Castles|TAK_GODS=1|--crusades"
  "everything-2|Lake Lokken|TAK_GODS=1 TAK_STRESS=1 TAK_RANDOM_STARTS=1 TAK_AI_LEVEL=4 TAK_FOG=2|--crusades"
)
[ "$QUICK" = "1" ] && RUNS=("${RUNS[@]:0:4}")

PORT_BASE=7900
run_one() {
  local idx="$1" spec="$2"
  local name map envs flags
  name="${spec%%|*}";        spec="${spec#*|}"
  map="${spec%%|*}";         spec="${spec#*|}"
  envs="${spec%%|*}";        flags="${spec#*|}"
  local port=$((PORT_BASE + idx))
  local slog="$OUT/$name.server.log" clog="$OUT/$name.client.log"

  # --seed varies per run: a fixed seed would test the same game every time, and the
  # point is coverage. It is still RECORDED here so any hit can be replayed exactly.
  local seed=$((1000 + idx))
  $SERVER --port "$port" --data "$DATA" --no-auth --seed "$seed" >"$slog" 2>&1 &
  local spid=$!
  for _ in $(seq 120); do grep -q listening "$slog" 2>/dev/null && break; sleep 1; done
  if ! grep -q listening "$slog" 2>/dev/null; then
    echo "FAIL $name: server never came up" ; kill "$spid" 2>/dev/null; return 1
  fi

  # THE CLIENT MUST TAKE A PLAYER SLOT. Server::checkHashes counts SEATED HUMAN slots
  # and returns immediately when there are none:
  #     if (int(it->second.size()) < live || live == 0) return;
  # and a spectator sends a literal 0 for its hash by design (gameview_net.cpp:
  # `isSpectator() ? 0 : world_.stateHash()`) because that field is a progress ack,
  # not a checksum. So --mphost WITH TAK_MP_WATCH=1 -- eight AIs watched by a
  # spectator -- compares NOTHING, and this script used to report "no desyncs" for
  # runs in which no two simulation states were ever compared. That is a worse
  # outcome than a failure: it looks like evidence.
  #
  # 7 AIs + this client seated as the 8th player keeps the table full AND gives the
  # referee a real hash to check every kHashPeriod ticks.
  local secs=$((MINUTES * 60))
  # shellcheck disable=SC2086
  env TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy TAK_MP_AIS=7 $SPEED_DEFAULT $envs \
      timeout -k 30 $((secs + 300)) $CLIENT game "$map" --data "$DATA" \
      --server 127.0.0.1 --serverport "$port" --mphost --time "$secs" $flags \
      >"$clog" 2>&1
  local rc=$?
  sleep 2; kill "$spid" 2>/dev/null; sleep 1; kill -9 "$spid" 2>/dev/null; wait "$spid" 2>/dev/null

  # The verdict. Anything other than a clean finish is worth a human look.
  local hit=""
  grep -qi "DESYNCED" "$slog" && hit="${hit}server-reported-desync "
  grep -qi "REFEREE SUSPECT" "$slog" && hit="${hit}referee-suspect "
  grep -qi "desync" "$clog" && hit="${hit}client-reported-desync "
  local done_line
  done_line=$(grep -E "mp-headless done" "$clog" | tail -1)
  [ -z "$done_line" ] && hit="${hit}no-completion(rc=$rc) "
  echo "$done_line" | grep -q "err=none" || [ -z "$done_line" ] || hit="${hit}err "
  [ "$rc" = "0" ] || hit="${hit}rc=$rc "

  echo "$done_line" | grep -q "end=concluded" && \
    echo "     note: $name ended early -- a team won before the ${MINUTES}m clock"
  if [ -n "$hit" ]; then
    echo "HIT  $name [seed=$seed map=$map $envs $flags] -- $hit"
    echo "     $done_line"
  else
    echo "ok   $name [seed=$seed] -- $done_line"
  fi
}
export -f run_one
export OUT CLIENT SERVER DATA MINUTES PORT_BASE SPEED_DEFAULT

i=0
for spec in "${RUNS[@]}"; do
  run_one "$i" "$spec" &
  i=$((i + 1))
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 5; done
done
wait

echo
echo "==== SUMMARY ===="
hits=$(grep -rlEi "DESYNCED|REFEREE SUSPECT" "$OUT" 2>/dev/null | sed "s|$OUT/||" | sort -u)
if [ -n "$hits" ]; then
  echo "DESYNCS FOUND in:"; echo "$hits"
else
  echo "no desyncs in any run"
fi
echo "runs that did not finish cleanly:"
for f in "$OUT"/*.client.log; do
  grep -q "err=none" "$f" 2>/dev/null || echo "  $(basename "$f")"
done
echo "logs: $OUT"
