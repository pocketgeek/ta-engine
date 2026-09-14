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

# HOSTS: "name:jobs:weight". WEIGHT picks which runs a box is allowed to take --
# `heavy` boxes get everything, `light` boxes only the runs that stay small.
#
# vpn3 has 2 cores and 3.9GB against tak's 32 and 31GB, so it takes NONE of the
# stress/benchmark configurations: those field 5k-15k units per game, and enough of
# them at once would push a small box into swap, which does not fail cleanly -- it
# just makes a run crawl and look like a stall, the same false signal that cost hours
# when a wedged client looked like a slow one.
#
# The job count is MEASURED, not guessed: a light referee holds ~67MB RSS and 2 of
# them left the box at 0.13 load, so 6 fits in well under 500MB of 3.9GB with CPU to
# spare. An earlier guess of 2 was over-cautious by 3x and would have turned 3 waves
# into 8 for no reason -- the clients run here, so a remote box only carries referees.
HOSTS_SPEC="${TAK_HOSTS:-tak.pgnet.us:10:heavy vpn3.pgnet.us:6:light}"
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
    --hosts)    HOSTS_SPEC="$2"; shift 2;;
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
PORT_BASE=7900
cleanup() {
  for hspec in $HOSTS_SPEC; do
    "${SSH[@]}" "$RUSER@${hspec%%:*}" 'pkill -x takserver 2>/dev/null; true' >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT INT TERM

echo "desync hunt: ${MINUTES}m per run"
echo "hosts: $HOSTS_SPEC"
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
  "h-baseline|Ulasem Arena|||human|light"
  "h-gods|Ulasem Arena|TAK_GODS=1||human|light"
  "h-crusades|Ulasem Arena||--crusades|human|light"
  "h-stress|Ulasem Arena|TAK_STRESS=1||human|heavy"
  "h-absurd|Ulasem Arena|TAK_AI_LEVEL=4||human|light"
  "h-fog-explored|Ulasem Arena|TAK_FOG=1||human|light"
  "h-fog-full|Ulasem Arena|TAK_FOG=2||human|light"
  "h-unitcap|Ulasem Arena|TAK_UNITCAP=5000||human|light"
  "h-cramped|Inner Circle|TAK_GODS=1||human|light"
  "h-naval|Aibel's Seaport|||human|light"
  "h-naval-crus|Aibel's Seaport||--crusades|human|light"
  "h-lake|Lake Lokken|TAK_STRESS=1||human|heavy"
  "h-random-starts|Sand River Plain|TAK_RANDOM_STARTS=1||human|light"
  "h-monarch-exp|Ulasem Arena|TAK_MONARCH_EXPENDABLE=1 TAK_GODS=1||human|light"
  "h-overrides-full|Ulasem Arena||--overrides full|human|light"
  "h-everything|Tarosian Plain|TAK_GODS=1 TAK_STRESS=1 TAK_AI_LEVEL=4 TAK_FOG=1|--crusades|human|heavy"
  "h-speed-1x|Ulasem Arena|TAK_SPEED=10||human|light"
  "h-two-castles|Two Castles|TAK_GODS=1|--crusades|human|light"
  "w-allai-stress|Ulasem Arena|TAK_STRESS=1||watch|heavy"
  "w-allai-bench|Ulasem Arena|TAK_BENCH=3||watch|heavy"
)

SPEED_DEFAULT="TAK_SPEED=40"

# Validate the table before running anything. An entry with the wrong field count
# shifts every field right: --overrides full once landed in the SEAT slot, so the run
# never mounted the overrides it was named for and still reported "ok". A sweep that
# quietly tests the wrong configuration is worse than one that fails.
for _spec in "${RUNS[@]}"; do
  _n=$(awk -F'|' '{print NF}' <<<"$_spec")
  if [ "$_n" != "6" ]; then
    echo "BAD RUN TABLE ENTRY ($_n fields, want 6 -- name|map|envs|flags|seat|weight):" >&2
    echo "  $_spec" >&2
    exit 2
  fi
done

run_one() {
  local host="$1" idx="$2" spec="$3"
  local name map envs flags seat weight
  name="${spec%%|*}"; spec="${spec#*|}"
  map="${spec%%|*}";  spec="${spec#*|}"
  envs="${spec%%|*}"; spec="${spec#*|}"
  flags="${spec%%|*}"; spec="${spec#*|}"
  seat="${spec%%|*}"; weight="${spec##*|}"
  local port=$((PORT_BASE + idx)) seed=$((2000 + idx))
  local clog="$OUT/$name.client.log"
  local SSHH=(ssh -o ControlMaster=auto -o ControlPath="$OUT/ctl-%C" -o ControlPersist=15m -o BatchMode=yes)
  rsh1() { "${SSHH[@]}" "$RUSER@$host" "$@"; }

  # Start the referee on the remote. No --local and no tunnel: this is a LAN, so the
  # client reaches it over a real NIC, which is the point of running it remotely.
  rsh1 "nohup $RBIN --port $port --data $RDATA --replaydir $RREPLAY --no-auth \
        --seed $seed >/tmp/tak-srv-$port.log 2>&1 & sleep 1" >/dev/null 2>&1
  local up=0
  for _ in $(seq 60); do
    rsh1 "grep -q listening /tmp/tak-srv-$port.log 2>/dev/null" && { up=1; break; }
    sleep 2
  done
  [ "$up" = "1" ] || { echo "FAIL $name ($host): remote server never came up (port $port)"; return 1; }

  # SEAT: watch -> spectator (TAK_MP_WATCH=1, 8 AIs). human -> a real player slot with
  # 7 AIs alongside, which is what makes the referee compare hashes at all.
  local seatenv="TAK_MP_AIS=7"
  [ "$seat" = "watch" ] && seatenv="TAK_MP_WATCH=1 TAK_MP_AIS=8"

  local secs=$((MINUTES * 60))
  # shellcheck disable=SC2086
  env TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy $seatenv $SPEED_DEFAULT $envs \
      timeout -k 30 $((secs + 300)) $CLIENT game "$map" --data "$LDATA" \
      --server "$host" --serverport "$port" --mphost --time "$secs" $flags \
      >"$clog" 2>&1
  local rc=$?

  rsh1 "cat /tmp/tak-srv-$port.log" >"$OUT/$name.server.log" 2>/dev/null
  rsh1 "pkill -f \"takserver --port $port\" 2>/dev/null; true" >/dev/null 2>&1

  local hit=""
  grep -qi "DESYNCED"        "$OUT/$name.server.log" 2>/dev/null && hit="${hit}DESYNC "
  grep -qi "REFEREE SUSPECT" "$OUT/$name.server.log" 2>/dev/null && hit="${hit}REFEREE-SUSPECT "
  grep -qi "desync"          "$clog" 2>/dev/null && hit="${hit}client-desync "
  local done_line; done_line=$(grep -E "mp-headless done" "$clog" | tail -1)
  [ -z "$done_line" ] && hit="${hit}no-completion(rc=$rc) "

  if [ -n "$hit" ]; then echo "HIT  $name @$host [seat=$seat seed=$seed map=$map $envs $flags] -- $hit"
                         echo "     $done_line"
  else echo "ok   $name @$host [seat=$seat seed=$seed] -- $done_line"; fi
}

# --validate: plant a KNOWN divergence and require the referee to catch it. If this
# does not report DESYNC, the sweep's verdict is worthless and we stop rather than
# hand back a "clean" result from a detector that never fires. Runs on the FIRST host.
if [ "$VALIDATE" = "1" ]; then
  vhost="${HOSTS_SPEC%%:*}"; vhost="${vhost%% *}"
  echo "== validating the detector with a PLANTED desync (TAK_FAKE_DESYNC=900) on $vhost =="
  VSSH=(ssh -o ControlMaster=auto -o ControlPath="$OUT/ctl-%C" -o ControlPersist=15m -o BatchMode=yes)
  "${VSSH[@]}" "$RUSER@$vhost" "nohup $RBIN --port 7890 --data $RDATA --no-auth --seed 999 >/tmp/tak-val.log 2>&1 & sleep 1" >/dev/null 2>&1
  for _ in $(seq 60); do "${VSSH[@]}" "$RUSER@$vhost" "grep -q listening /tmp/tak-val.log 2>/dev/null" && break; sleep 2; done
  env TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy TAK_MP_AIS=7 TAK_SPEED=40 TAK_FAKE_DESYNC=900 \
      timeout -k 30 400 $CLIENT game "Ulasem Arena" --data "$LDATA" \
      --server "$vhost" --serverport 7890 --mphost --time 120 >"$OUT/validate.client.log" 2>&1
  "${VSSH[@]}" "$RUSER@$vhost" "cat /tmp/tak-val.log" >"$OUT/validate.server.log" 2>/dev/null
  "${VSSH[@]}" "$RUSER@$vhost" 'pkill -x takserver 2>/dev/null; true' >/dev/null 2>&1
  if grep -qi "DESYNCED" "$OUT/validate.server.log"; then
    echo "   PASS -- referee reported: $(grep -i DESYNCED "$OUT/validate.server.log" | head -1)"
  else
    echo "   FAIL -- planted desync NOT detected. The sweep cannot prove anything; stopping." >&2
    exit 1
  fi
fi

# Dispatch. Each host drains its own queue at its own concurrency, so a 2-core box
# never gates a 32-core one: the small host simply takes fewer, lighter runs and
# finishes when it finishes.
idx=0
declare -a HOST_PIDS=()
for hspec in $HOSTS_SPEC; do
  hname="${hspec%%:*}"; rest="${hspec#*:}"
  hjobs="${rest%%:*}"; hweight="${rest##*:}"
  # Partition the table: a `light` host takes only light runs; a `heavy` host takes
  # everything it is handed. Indices stay globally unique so ports never collide.
  mine=(); myidx=()
  j=0
  for spec in "${RUNS[@]}"; do
    w="${spec##*|}"
    take=0
    if [ "$hweight" = "heavy" ]; then
      # heavy box takes the heavy runs plus whatever light ones are left over
      [ "$w" = "heavy" ] && take=1
    else
      [ "$w" = "light" ] && take=1
    fi
    [ "$take" = "1" ] && { mine+=("$spec"); myidx+=("$j"); }
    j=$((j + 1))
  done
  echo "-> $hname: ${#mine[@]} runs, ${hjobs} at a time ($hweight)"
  (
    k=0
    for spec in "${mine[@]}"; do
      run_one "$hname" "${myidx[$k]}" "$spec" &
      k=$((k + 1))
      while [ "$(jobs -rp | wc -l)" -ge "$hjobs" ]; do sleep 5; done
    done
    wait
  ) &
  HOST_PIDS+=($!)
done
for pid in "${HOST_PIDS[@]}"; do wait "$pid"; done

echo
echo "==== SUMMARY ===="
# Exclude validate.* -- it contains a DELIBERATELY planted desync, so including it
# made every --validate run end with "DESYNCS FOUND", which trains you to ignore the
# one line that matters.
hits=$(grep -rlEi "DESYNCED|REFEREE SUSPECT" "$OUT" 2>/dev/null | grep -v "/validate\." | sed "s|$OUT/||" | sort -u)
if [ -n "$hits" ]; then echo "DESYNCS FOUND in:"; echo "$hits"; else echo "no desyncs reported"; fi
echo "runs that did not complete:"
for f in "$OUT"/*.client.log; do
  grep -q "mp-headless done" "$f" 2>/dev/null || echo "  $(basename "$f")"
done
echo "logs: $OUT"
