#!/usr/bin/env bash
# desync-hunt-remote.sh -- the desync sweep with the REFEREE ON ANOTHER MACHINE,
# and with SEATED HUMAN PLAYERS so the referee actually compares hashes.
#
# WHY A SEATED PLAYER IS MANDATORY, NOT A VARIANT.
# Server::checkHashes opens with:
#       for (...) if (r.slots[i].type == 1 && r.slotClient[i] >= 0) ++live;
#       if (int(it->second.size()) < live || live == 0) return;
# `live` counts SEATED HUMAN slots only. A spectator watching 8 AIs leaves live == 0,
# so checkHashes returns on entry and NOTHING is ever compared -- and the spectator is
# sending a literal 0 anyway (gameview_net.cpp: `isSpectator() ? 0 : world_.stateHash()`),
# because its hash is documented as a pure progress ack. An all-spectator sweep is
# therefore incapable of reporting a desync, however long it runs. Every run below
# seats at least one human (--mphost WITHOUT TAK_MP_WATCH, TAK_MP_AIS=7).
#
# WHY REMOTE. On one box, client and referee are the same binary, so the only
# divergence reachable is client/server code asymmetry. Split across machines the
# sweep also exercises a real link: latency and jitter drive the adaptive jitter
# buffer (netDelay_ self-sizes to RTT), the kMaxLeadTicks=120 flow-control window and
# the 0.4s spectator heartbeat -- the same window that turned a client hang into a
# server-side wedge earlier in this work.
#
# The server binary is built STATIC and shipped: this host needs GLIBC_2.43 and the
# remote is Ubuntu 24.04 on 2.39, so a dynamically linked copy would not start. Static
# also keeps the referee on the SAME GCC as the client, which is what makes a hash
# mismatch unambiguous -- a real logic bug rather than a cross-toolchain artifact.
#
# usage: tools/desync-hunt-remote.sh [--host H] [--minutes N] [--jobs N] [--validate]
set -u

HOST="tak.pgnet.us"
RUSER="pocket_geek"
RDATA="/home/pocket_geek/tak_data"
RREPLAY="/home/pocket_geek/tak_replay"
RBIN="/home/pocket_geek/takserver"
LDATA="assets/game"
MINUTES=45
JOBS=12
VALIDATE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --host)     HOST="$2"; shift 2;;
    --minutes)  MINUTES="$2"; shift 2;;
    --jobs)     JOBS="$2"; shift 2;;
    --data)     LDATA="$2"; shift 2;;
    --validate) VALIDATE=1; shift;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

CLIENT=./build-dbg/takclient
[ -x "$CLIENT" ] || { echo "build-dbg/takclient missing" >&2; exit 2; }

OUT="${TMPDIR:-/tmp}/desync-remote-$$"; mkdir -p "$OUT"
CTL="$OUT/ctl-%C"
SSH=(ssh -o ControlMaster=auto -o ControlPath="$CTL" -o ControlPersist=15m -o BatchMode=yes)
rsh() { "${SSH[@]}" "$RUSER@$HOST" "$@"; }

PORT_BASE=7900
cleanup() { rsh 'pkill -x takserver 2>/dev/null; true' >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM

echo "desync hunt (remote referee on $HOST): ${MINUTES}m per run, ${JOBS} parallel"
echo "logs: $OUT"

# Run table. Each entry: NAME|MAP|ENVS|FLAGS|SEAT
#   SEAT=human -> the local client takes a PLAYER slot (7 AIs + 1 human). Its hash is
#                 compared against the referee every kHashPeriod ticks. This is the only
#                 configuration that can actually detect a desync.
#   SEAT=watch -> spectator (8 AIs). Cannot detect a desync; kept for a few entries only
#                 because it stresses the all-AI flow-control path (canAdvance's anySpec
#                 branch), which is where the earlier server wedge lived.
#
# A human run ENDS when that player's team is eliminated (outcome_ != 0 stops the drain),
# so these are usually shorter than the clock allows. That is reported, not hidden.
RUNS=(
  "h-baseline|Ulasem Arena||human"
  "h-gods|Ulasem Arena|TAK_GODS=1||human"
  "h-crusades|Ulasem Arena||--crusades|human"
  "h-stress|Ulasem Arena|TAK_STRESS=1||human"
  "h-absurd|Ulasem Arena|TAK_AI_LEVEL=4||human"
  "h-fog-explored|Ulasem Arena|TAK_FOG=1||human"
  "h-fog-full|Ulasem Arena|TAK_FOG=2||human"
  "h-unitcap|Ulasem Arena|TAK_UNITCAP=5000||human"
  "h-cramped|Inner Circle|TAK_GODS=1||human"
  "h-naval|Aibel's Seaport|||human"
  "h-naval-crus|Aibel's Seaport||--crusades|human"
  "h-lake|Lake Lokken|TAK_STRESS=1||human"
  "h-random-starts|Sand River Plain|TAK_RANDOM_STARTS=1||human"
  "h-monarch-exp|Ulasem Arena|TAK_MONARCH_EXPENDABLE=1 TAK_GODS=1||human"
  "h-overrides-full|Ulasem Arena|||--overrides full|human"
  "h-everything|Tarosian Plain|TAK_GODS=1 TAK_STRESS=1 TAK_AI_LEVEL=4 TAK_FOG=1|--crusades|human"
  "h-speed-1x|Ulasem Arena|TAK_SPEED=10||human"
  "h-two-castles|Two Castles|TAK_GODS=1|--crusades|human"
  "w-allai-stress|Ulasem Arena|TAK_STRESS=1||watch"
  "w-allai-bench|Ulasem Arena|TAK_BENCH=3||watch"
)

SPEED_DEFAULT="TAK_SPEED=40"

run_one() {
  local idx="$1" spec="$2"
  local name map envs flags seat
  name="${spec%%|*}"; spec="${spec#*|}"
  map="${spec%%|*}";  spec="${spec#*|}"
  envs="${spec%%|*}"; spec="${spec#*|}"
  flags="${spec%%|*}"; seat="${spec##*|}"
  local port=$((PORT_BASE + idx)) seed=$((2000 + idx))
  local clog="$OUT/$name.client.log"

  # Start the referee on the remote. No --local and no tunnel: this is a LAN, so the
  # client reaches it over a real NIC, which is the point of running it remotely.
  rsh "nohup $RBIN --port $port --data $RDATA --replaydir $RREPLAY --no-auth \
       --seed $seed >/tmp/tak-srv-$port.log 2>&1 & sleep 1" >/dev/null 2>&1
  local up=0
  for _ in $(seq 60); do
    rsh "grep -q listening /tmp/tak-srv-$port.log 2>/dev/null" && { up=1; break; }
    sleep 2
  done
  [ "$up" = "1" ] || { echo "FAIL $name: remote server never came up (port $port)"; return 1; }

  # SEAT: watch -> spectator (TAK_MP_WATCH=1, 8 AIs). human -> a real player slot with
  # 7 AIs alongside, which is what makes the referee compare hashes at all.
  local seatenv="TAK_MP_AIS=7"
  [ "$seat" = "watch" ] && seatenv="TAK_MP_WATCH=1 TAK_MP_AIS=8"

  local secs=$((MINUTES * 60))
  # shellcheck disable=SC2086
  env TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy $seatenv $SPEED_DEFAULT $envs \
      timeout -k 30 $((secs + 300)) $CLIENT game "$map" --data "$LDATA" \
      --server "$HOST" --serverport "$port" --mphost --time "$secs" $flags \
      >"$clog" 2>&1
  local rc=$?

  rsh "cp /tmp/tak-srv-$port.log $OUT/ 2>/dev/null; pkill -f \"takserver --port $port\" 2>/dev/null; true" >/dev/null 2>&1
  rsh "cat /tmp/tak-srv-$port.log" >"$OUT/$name.server.log" 2>/dev/null

  local hit=""
  grep -qi "DESYNCED"        "$OUT/$name.server.log" 2>/dev/null && hit="${hit}DESYNC "
  grep -qi "REFEREE SUSPECT" "$OUT/$name.server.log" 2>/dev/null && hit="${hit}REFEREE-SUSPECT "
  grep -qi "desync"          "$clog" 2>/dev/null && hit="${hit}client-desync "
  local done_line; done_line=$(grep -E "mp-headless done" "$clog" | tail -1)
  [ -z "$done_line" ] && hit="${hit}no-completion(rc=$rc) "

  if [ -n "$hit" ]; then echo "HIT  $name [seat=$seat seed=$seed map=$map $envs $flags] -- $hit"
                         echo "     $done_line"
  else echo "ok   $name [seat=$seat seed=$seed] -- $done_line"; fi
}
export -f run_one
export OUT CLIENT LDATA MINUTES PORT_BASE SPEED_DEFAULT HOST RUSER RDATA RREPLAY RBIN SPEED_DEFAULT
export -f rsh 2>/dev/null || true

# --validate: plant a KNOWN divergence and require the referee to catch it. If this
# does not report DESYNC, the sweep's verdict is worthless and we stop rather than
# hand back a "clean" result from a detector that never fires.
if [ "$VALIDATE" = "1" ]; then
  echo "== validating the detector with a PLANTED desync (TAK_FAKE_DESYNC=900) =="
  port=7890; seed=999
  rsh "nohup $RBIN --port $port --data $RDATA --no-auth --seed $seed >/tmp/tak-val.log 2>&1 & sleep 1" >/dev/null 2>&1
  for _ in $(seq 60); do rsh "grep -q listening /tmp/tak-val.log 2>/dev/null" && break; sleep 2; done
  env TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy TAK_MP_AIS=7 TAK_SPEED=40 TAK_FAKE_DESYNC=900 \
      timeout -k 30 400 $CLIENT game "Ulasem Arena" --data "$LDATA" \
      --server "$HOST" --serverport "$port" --mphost --time 120 >"$OUT/validate.client.log" 2>&1
  rsh "cat /tmp/tak-val.log" >"$OUT/validate.server.log" 2>/dev/null
  rsh 'pkill -x takserver 2>/dev/null; true' >/dev/null 2>&1
  if grep -qi "DESYNCED" "$OUT/validate.server.log"; then
    echo "   PASS -- referee reported: $(grep -i DESYNCED "$OUT/validate.server.log" | head -1)"
  else
    echo "   FAIL -- planted desync NOT detected. The sweep cannot prove anything; stopping." >&2
    echo "   server log: $OUT/validate.server.log" >&2
    exit 1
  fi
fi

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
if [ -n "$hits" ]; then echo "DESYNCS FOUND in:"; echo "$hits"; else echo "no desyncs reported"; fi
echo "runs that did not complete:"
for f in "$OUT"/*.client.log; do
  grep -q "mp-headless done" "$f" 2>/dev/null || echo "  $(basename "$f")"
done
echo "logs: $OUT"
