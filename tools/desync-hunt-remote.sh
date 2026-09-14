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

# RUN FROM A SNAPSHOT, NOT FROM THE LIVE FILE -- and do it before ANY argument is
# consumed, so the re-exec forwards them intact. bash reads a script incrementally from
# disk, so editing this file mid-sweep makes the shell resume at a byte offset that now
# holds different text: that produced "line 376: to: command not found", re-entered the
# dispatch loop so all 20 runs reported twice, and cost a 45-minute sweep. A sweep runs
# long enough that wanting to edit it is normal, so make editing safe rather than rely
# on remembering not to.
if [ -z "${TAK_SWEEP_SNAPSHOT:-}" ]; then
  _snap=$(mktemp "${TMPDIR:-/tmp}/desync-hunt-remote.XXXXXX.sh")
  cat "$0" >"$_snap"; chmod +x "$_snap"
  export TAK_SWEEP_SNAPSHOT="$_snap"
  exec "$_snap" "$@"
fi
# NOTE: no `trap ... EXIT` here. The cleanup() registered further down would REPLACE
# it -- bash traps do not accumulate -- so the snapshot is removed inside cleanup()
# instead. Sweep up anything an earlier run leaked (a SIGPIPE death, from piping the
# output into grep or head, skips traps entirely). The snapshot cannot be unlinked
# early: bash is still reading the script from that path.
find "${TMPDIR:-/tmp}" -maxdepth 1 -name 'desync-hunt-remote.*.sh' -mmin +120 -delete 2>/dev/null || true

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
DRYRUN=0
while [ $# -gt 0 ]; do
  case "$1" in
    --hosts)    HOSTS_SPEC="$2"; shift 2;;
    --minutes)  MINUTES="$2"; shift 2;;
    --jobs)     JOBS="$2"; shift 2;;
    --data)     LDATA="$2"; shift 2;;
    --validate) VALIDATE=1; shift;;
    --dry-run)  DRYRUN=1; shift;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

CLIENT=./build-dbg/takclient
[ -x "$CLIENT" ] || { echo "build-dbg/takclient missing" >&2; exit 2; }


OUT="${TMPDIR:-/tmp}/desync-remote-$$"; mkdir -p "$OUT"
CTL="$OUT/ctl-%C"
SSH=(ssh -o ControlMaster=auto -o ControlPath="$CTL" -o ControlPersist=15m -o BatchMode=yes)
# Two sweeps running at once would otherwise fight over the same ports: the table is
# indexed from PORT_BASE, so a second invocation hands its clients the first one's
# referees. Overridable rather than fixed.
PORT_BASE="${TAK_PORT_BASE:-7900}"
# Every referee this run starts is recorded here as "host<TAB>pid", and cleanup kills
# exactly those. `pkill -x takserver` would kill every server owned by the account --
# a concurrent sweep, or somebody's live game. A test harness must not be able to take
# down what it is testing alongside. (This bit during development: a stray pkill in a
# monitor killed servers out from under a running sweep and cost two confused runs.)
PIDFILE="$OUT/started-servers"
: >"$PIDFILE"
note_server() { printf '%s\t%s\n' "$1" "$2" >>"$PIDFILE"; }

cleanup() {
  rm -f "${TAK_SWEEP_SNAPSHOT:-}" 2>/dev/null || true
  [ -s "$PIDFILE" ] || return 0
  local hosts; hosts=$(cut -f1 "$PIDFILE" | sort -u)
  for h in $hosts; do
    local pids; pids=$(awk -v h="$h" -F'\t' '$1==h {printf "%s ", $2}' "$PIDFILE")
    [ -n "$pids" ] || continue
    # Confirm each pid is still OUR takserver before signalling: pids get recycled, and
    # killing a stranger because a number came round again is the same class of bug.
    "${SSH[@]}" "$RUSER@$h" "for p in $pids; do \
         c=\$(cat /proc/\$p/comm 2>/dev/null); \
         [ \"\$c\" = takserver ] && kill \$p 2>/dev/null; \
       done; true" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT INT TERM

echo "desync hunt: ${MINUTES}m per run"
echo "hosts: $HOSTS_SPEC"
echo "logs: $OUT"

# Run table. Each entry: NAME|MAP|ENVS|FLAGS|SEAT|WEIGHT[|HUMANS]
#
# HUMANS (default 1) is how many SEATED PLAYERS the run uses. Two or more is not a
# bigger version of one -- it reaches code one human cannot:
#
#   * REFEREE SUSPECT is gated on `live >= 2` (server.cpp): with a single client there
#     is no consensus to appeal with, so that whole branch is unreachable. This script
#     greps for the string; until now it could never have been produced.
#   * One human only ever proves client-agrees-with-referee. TWO independent client
#     sims agreeing with EACH OTHER is the property a real match depends on, and it is
#     what catches a divergence the referee happens to share.
#   * canAdvance paces to the slowest of several humans, and the tick-0 load gate waits
#     for every seated human. Both are no-ops with one.
#
# The host seats its AIs in the TOP slots ("leaving the low slots for human joiners"),
# joiners come in with --mpjoin, and TAK_MP_WAIT holds the start until the table is
# full -- so N humans means TAK_MP_AIS=(8-N) and TAK_MP_WAIT=8.
#
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
  # --- multi-human: two independent client sims, compared to each other and to the
  # referee. These are the only entries that can reach the live>=2 consensus logic.
  "2h-baseline|Ulasem Arena|||human|light|2"
  "2h-gods|Ulasem Arena|TAK_GODS=1||human|light|2"
  "2h-crusades|Tarosian Plain||--crusades|human|light|2"
  "2h-stress|Ulasem Arena|TAK_STRESS=1||human|heavy|2"
  "3h-baseline|Ulasem Arena|||human|light|3"
  "4h-gods|Ulasem Arena|TAK_GODS=1||human|light|4"
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
  if [ "$_n" != "6" ] && [ "$_n" != "7" ]; then
    echo "BAD RUN TABLE ENTRY ($_n fields, want 6 or 7 -- name|map|envs|flags|seat|weight[|humans]):" >&2
    echo "  $_spec" >&2
    exit 2
  fi
done

run_one() {
  local host="$1" idx="$2" spec="$3"
  local name map envs flags seat weight humans
  name="${spec%%|*}"; spec="${spec#*|}"
  map="${spec%%|*}";  spec="${spec#*|}"
  envs="${spec%%|*}"; spec="${spec#*|}"
  flags="${spec%%|*}"; spec="${spec#*|}"
  seat="${spec%%|*}"; spec="${spec#*|}"
  weight="${spec%%|*}"
  # HUMANS is optional; a 6-field entry means one seated player.
  if [ "$spec" = "$weight" ]; then humans=1; else humans="${spec#*|}"; fi
  case "$humans" in ''|*[!0-9]*) humans=1;; esac
  local port=$((PORT_BASE + idx)) seed=$((2000 + idx))
  local clog="$OUT/$name.client.log"
  local SSHH=(ssh -o ControlMaster=auto -o ControlPath="$OUT/ctl-%C" -o ControlPersist=15m -o BatchMode=yes)
  rsh1() { "${SSHH[@]}" "$RUSER@$host" "$@"; }

  # Start the referee on the remote. No --local and no tunnel: this is a LAN, so the
  # client reaches it over a real NIC, which is the point of running it remotely.
  local spid
  spid=$(rsh1 "nohup $RBIN --port $port --data $RDATA --replaydir $RREPLAY --no-auth \
         --seed $seed >/tmp/tak-srv-$port.log 2>&1 </dev/null & echo \$!" 2>/dev/null | tr -d '\r')
  [ -n "$spid" ] && note_server "$host" "$spid"
  local up=0
  for _ in $(seq 60); do
    rsh1 "grep -q listening /tmp/tak-srv-$port.log 2>/dev/null" && { up=1; break; }
    sleep 2
  done
  [ "$up" = "1" ] || { echo "FAIL $name ($host): remote server never came up (port $port)"; return 1; }

  # SEAT: watch -> spectator (TAK_MP_WATCH=1, 8 AIs). human -> a real player slot with
  # 7 AIs alongside, which is what makes the referee compare hashes at all.
  # Seat the table. N humans -> (8-N) AIs, and the host waits for a full 8 before
  # starting so the joiners are actually in the game rather than racing its first tick.
  local nai=$((8 - humans))
  local seatenv="TAK_MP_AIS=$nai TAK_MP_WAIT=8"
  [ "$seat" = "watch" ] && seatenv="TAK_MP_WATCH=1 TAK_MP_AIS=8"

  local secs=$((MINUTES * 60))
  # shellcheck disable=SC2086
  env TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy $seatenv $SPEED_DEFAULT $envs \
      timeout -k 30 $((secs + 300)) $CLIENT game "$map" --data "$LDATA" \
      --server "$host" --serverport "$port" --mphost --time "$secs" $flags \
      >"$clog" 2>&1 &
  local hostpid=$!

  # Joiners take the low slots. Give the host a moment to create the room first --
  # joining before the game exists just burns the list-poll.
  local jpids=() j
  if [ "$humans" -gt 1 ]; then
    sleep 8
    for ((j = 2; j <= humans; j++)); do
      # shellcheck disable=SC2086
      env TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy $SPEED_DEFAULT $envs \
          timeout -k 30 $((secs + 300)) $CLIENT game "$map" --data "$LDATA" \
          --server "$host" --serverport "$port" --mpjoin --time "$secs" $flags \
          >"$OUT/$name.client$j.log" 2>&1 &
      jpids+=($!)
    done
  fi
  wait "$hostpid"; local rc=$?
  for j in "${jpids[@]}"; do wait "$j" || true; done

  # STOP THE REFEREE BEFORE READING ITS LOG. It keeps writing as the client goes away
  # ("client N dropped", "game ended"), so fetching while it runs and then measuring
  # the source compares a snapshot against a file that has since grown -- which reads
  # as a truncated transfer when nothing went wrong. Kill first, then read a file with
  # no writer.
  #
  # Kill by pid, not by pattern: a pattern would also hit a concurrent sweep on the
  # same port, and pkill -x would hit every server on the account.
  if [ -n "$spid" ]; then
    rsh1 "kill $spid 2>/dev/null; for _ in 1 2 3 4 5 6 7 8 9 10; do
            [ -d /proc/$spid ] || break; sleep 0.5; done; true" >/dev/null 2>&1
  fi

  # Fetch the referee log and KEEP THE TRANSFER'S EXIT STATUS. A dropped ssh yields a
  # short file, and "the file is not empty" would accept a truncated copy whose missing
  # tail is exactly where a late desync line would have been. Absence of evidence here
  # is not evidence of absence -- it is a failed download. The size cross-check catches
  # a transfer that ended early without a nonzero status.
  local fetchrc=0 remote_sz local_sz
  rsh1 "cat /tmp/tak-srv-$port.log" >"$OUT/$name.server.log" 2>/dev/null || fetchrc=$?
  remote_sz=$(rsh1 "stat -c%s /tmp/tak-srv-$port.log 2>/dev/null || echo -1" 2>/dev/null | tr -d '\r')
  local_sz=$(stat -c%s "$OUT/$name.server.log" 2>/dev/null || echo -2)

  # THE VERDICT. A run only passes if it actually ran: the completion line alone is
  # not enough, because a client that lost its connection still prints one. A real
  # example from this harness: "tick=0 hash=... units=0 err=peer closed" -- a run that
  # never ticked once, next to a referee log with no complaints in it, which reads as
  # a pass if you only check that the line exists. Require the exit status, the error
  # field, and a server log we actually retrieved.
  local hit=""
  grep -qi "DESYNCED"        "$OUT/$name.server.log" 2>/dev/null && hit="${hit}DESYNC "
  grep -qi "REFEREE SUSPECT" "$OUT/$name.server.log" 2>/dev/null && hit="${hit}REFEREE-SUSPECT "
  grep -qi "desync"          "$clog" 2>/dev/null && hit="${hit}client-desync "
  local done_line; done_line=$(grep -E "mp-headless done" "$clog" | tail -1)

  # EVERY EXTRA HUMAN MUST FINISH CLEANLY TOO -- a joiner that died or errored would
  # otherwise be invisible, since only the host's line is parsed above.
  #
  # ON COMPARING CLIENT HASHES DIRECTLY: only do it when the clients stopped on the
  # SAME TICK. They frequently do not -- the headless loop drains a BATCH of bundles
  # per frame and breaks once netTick passes the limit, so eight clients asked for 5400
  # ticks stopped at 5419, 5431, 5436 and 5438. Those hashes describe different world
  # states and differ for entirely healthy reasons; comparing them reports a desync
  # that is not there (it did exactly that on the first all-human run).
  #
  # Cross-client agreement is not lost by skipping it: the referee compares EVERY
  # client at MATCHING ticks, so A==referee and B==referee at tick T gives A==B at T.
  # The one case that transitivity misses -- all clients agreeing with each other but
  # not the referee -- is precisely what REFEREE SUSPECT detects, and that is grepped
  # for above. So the tick-aligned comparison here is a bonus check, not the mechanism.
  if [ "$humans" -gt 1 ]; then
    local h1 hn t1 tn jl
    h1=$(grep -oE 'hash=[0-9a-f]+' "$clog" | tail -1)
    t1=$(grep -oE 'tick=[0-9]+' "$clog" | tail -1)
    for ((j = 2; j <= humans; j++)); do
      jl="$OUT/$name.client$j.log"
      grep -q "mp-headless done" "$jl" 2>/dev/null || { hit="${hit}client$j-no-completion "; continue; }
      grep -q "err=none" "$jl" 2>/dev/null || hit="${hit}client$j-error "
      hn=$(grep -oE 'hash=[0-9a-f]+' "$jl" | tail -1)
      tn=$(grep -oE 'tick=[0-9]+' "$jl" | tail -1)
      if [ "$t1" = "$tn" ] && [ -n "$t1" ]; then
        [ "$h1" = "$hn" ] || hit="${hit}client-mismatch@$t1(1=$h1 $j=$hn) "
      fi
    done
  fi
  [ -z "$done_line" ] && hit="${hit}no-completion "
  [ "$rc" = "0" ] || hit="${hit}rc=$rc "
  # err= carries the client's own verdict ("peer closed", "desync detected", ...).
  [ -n "$done_line" ] && { echo "$done_line" | grep -q "err=none" || hit="${hit}client-error "; }
  # No server log -- or a partial one -- means we cannot say what the referee saw, so
  # we must not claim it saw nothing wrong.
  [ -s "$OUT/$name.server.log" ] || hit="${hit}no-server-log "
  [ "$fetchrc" = "0" ] || hit="${hit}server-log-fetch-failed(rc=$fetchrc) "
  [ "$remote_sz" = "$local_sz" ] || hit="${hit}server-log-truncated($local_sz/$remote_sz) "

  # A spectator run compares NOTHING (checkHashes returns on `live == 0`, and the
  # spectator sends a zero hash by design), so its clean finish is a flow-control
  # result, not a determinism one. Label it rather than let "ok" imply verification.
  # TAK_BENCH forces spectator mode too, whatever the seat says.
  local nohash=0
  [ "$seat" = "watch" ] && nohash=1
  case "$envs" in *TAK_BENCH*) nohash=1;; esac

  if [ -n "$hit" ]; then echo "HIT  $name @$host [seat=$seat seed=$seed map=$map $envs $flags] -- $hit"
                         echo "     $done_line"
  elif [ "$nohash" = "1" ]; then
    echo "flow $name @$host [seat=$seat seed=$seed] -- NO HASH COMPARISON -- $done_line"
  else echo "ok   $name @$host [seat=$seat seed=$seed] -- $done_line"; fi
}

# DISPATCH. Assign every run to exactly one host, then let each host drain its own
# queue at its own concurrency.
#
# The previous version PARTITIONED by weight -- heavy hosts took only heavy runs,
# light hosts only light -- so configuring a single heavy host silently skipped every
# light case, and the sweep reported on a fraction of its table without saying so. A
# skipped test that never announces itself is indistinguishable from a passing one.
#
# The rule is a capability, not a partition: a HEAVY run needs a heavy host; a LIGHT
# run will go anywhere. Runs are placed on the eligible host with the lowest projected
# load (assigned / jobs), so a big box absorbs the bulk without starving a small one.
H_NAME=(); H_JOBS=(); H_WEIGHT=(); H_COUNT=()
for hspec in $HOSTS_SPEC; do
  H_NAME+=("${hspec%%:*}")
  _rest="${hspec#*:}"
  H_JOBS+=("${_rest%%:*}")
  H_WEIGHT+=("${_rest##*:}")
  H_COUNT+=(0)
done
NH=${#H_NAME[@]}
[ "$NH" -gt 0 ] || { echo "no hosts configured" >&2; exit 2; }

declare -a ASSIGN=()
for j in "${!RUNS[@]}"; do
  # Field 6 is the weight. NOT ${...##*|} -- the optional 7th (humans) field would make
  # that read "2" as a weight, and a heavy run would quietly become eligible for a
  # small host. Position, not last-field convenience.
  w=$(cut -d'|' -f6 <<<"${RUNS[$j]}")
  best=-1
  for ((h = 0; h < NH; h++)); do
    # Capability check: only a heavy host may take a heavy run. Everything else is fair
    # game for any host.
    if [ "$w" = "heavy" ] && [ "${H_WEIGHT[$h]}" != "heavy" ]; then continue; fi
    if [ "$best" -lt 0 ]; then best=$h; continue; fi
    # Lower projected load wins: (count+1)/jobs, compared by cross-multiplication so
    # this stays integer arithmetic.
    if [ $(( (H_COUNT[h] + 1) * H_JOBS[best] )) -lt $(( (H_COUNT[best] + 1) * H_JOBS[h] )) ]; then
      best=$h
    fi
  done
  if [ "$best" -lt 0 ]; then
    echo "NO HOST CAN RUN '$(cut -d'|' -f1 <<<"${RUNS[$j]}")' (weight=$w)." >&2
    echo "Configure at least one host with weight 'heavy', or drop the run." >&2
    exit 2
  fi
  ASSIGN[$j]=$best
  H_COUNT[$best]=$(( H_COUNT[best] + 1 ))
done

# Every run must be placed. Refuse to start a sweep that would quietly cover less than
# its table.
_placed=0
for ((h = 0; h < NH; h++)); do _placed=$(( _placed + H_COUNT[h] )); done
[ "$_placed" = "${#RUNS[@]}" ] || {
  echo "assignment covered $_placed of ${#RUNS[@]} runs -- refusing to run a partial sweep" >&2
  exit 2
}

# --dry-run: print the assignment and stop. Lets the placement rules be checked
# without starting 20 games -- which is how "a single heavy host silently skips every
# light run" should have been caught before it shipped.
if [ "$DRYRUN" = "1" ]; then
  for ((h = 0; h < NH; h++)); do
    printf -- "-> %s: %d runs, %s at a time (%s)\n" "${H_NAME[$h]}" "${H_COUNT[$h]}" "${H_JOBS[$h]}" "${H_WEIGHT[$h]}"
    for j in "${!RUNS[@]}"; do
      if [ "${ASSIGN[$j]}" = "$h" ]; then
        _w=$(cut -d'|' -f6 <<<"${RUNS[$j]}")
        _hn=$(cut -d'|' -f7 <<<"${RUNS[$j]}"); [ -n "$_hn" ] || _hn=1
        printf -- "     %-18s %-6s %s human(s)\n" "$(cut -d'|' -f1 <<<"${RUNS[$j]}")" "$_w" "$_hn"
      fi
    done
  done
  echo "placed $_placed of ${#RUNS[@]} runs"
  exit 0
fi

# Validation runs AFTER the dry-run exit on purpose: --dry-run must have no side
# effects at all. It used to sit ahead of the assignment, so `--dry-run --validate`
# started a real referee and a real client before printing a plan and stopping -- a
# flag whose whole point is "show me what you would do" quietly doing something.
#
# --validate: plant a KNOWN divergence and require the referee to catch it. If this
# does not report DESYNC, the sweep's verdict is worthless and we stop rather than
# hand back a "clean" result from a detector that never fires. Runs on the FIRST host.
if [ "$VALIDATE" = "1" ]; then
  vhost="${HOSTS_SPEC%%:*}"; vhost="${vhost%% *}"
  echo "== validating the detector with a PLANTED desync (TAK_FAKE_DESYNC=900) on $vhost =="
  VSSH=(ssh -o ControlMaster=auto -o ControlPath="$OUT/ctl-%C" -o ControlPersist=15m -o BatchMode=yes)
  vpid=$("${VSSH[@]}" "$RUSER@$vhost" "nohup $RBIN --port 7890 --data $RDATA --no-auth --seed 999 >/tmp/tak-val.log 2>&1 </dev/null & echo \$!" 2>/dev/null | tr -d '\r')
  [ -n "$vpid" ] && note_server "$vhost" "$vpid"
  for _ in $(seq 60); do "${VSSH[@]}" "$RUSER@$vhost" "grep -q listening /tmp/tak-val.log 2>/dev/null" && break; sleep 2; done
  env TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy TAK_MP_AIS=7 TAK_SPEED=40 TAK_FAKE_DESYNC=900 \
      timeout -k 30 400 $CLIENT game "Ulasem Arena" --data "$LDATA" \
      --server "$vhost" --serverport 7890 --mphost --time 120 >"$OUT/validate.client.log" 2>&1
  "${VSSH[@]}" "$RUSER@$vhost" "cat /tmp/tak-val.log" >"$OUT/validate.server.log" 2>/dev/null
  [ -n "$vpid" ] && "${VSSH[@]}" "$RUSER@$vhost" "kill $vpid 2>/dev/null; true" >/dev/null 2>&1
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

declare -a HOST_PIDS=()
for ((h = 0; h < NH; h++)); do
  mine=(); myidx=()
  for j in "${!RUNS[@]}"; do
    [ "${ASSIGN[$j]}" = "$h" ] && { mine+=("${RUNS[$j]}"); myidx+=("$j"); }
  done
  echo "-> ${H_NAME[$h]}: ${#mine[@]} runs, ${H_JOBS[$h]} at a time (${H_WEIGHT[$h]})"
  [ "${#mine[@]}" -gt 0 ] || continue
  (
    k=0
    for spec in "${mine[@]}"; do
      run_one "${H_NAME[$h]}" "${myidx[$k]}" "$spec" &
      k=$((k + 1))
      while [ "$(jobs -rp | wc -l)" -ge "${H_JOBS[$h]}" ]; do sleep 5; done
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
echo "note: runs marked 'flow' seated no human, so no hashes were compared in them --"
echo "      they cover flow control only and prove nothing about determinism."
echo "runs that did not complete:"
for f in "$OUT"/*.client.log; do
  grep -q "mp-headless done" "$f" 2>/dev/null || echo "  $(basename "$f")"
done
echo "logs: $OUT"
