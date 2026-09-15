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
#        [--only NAME[,NAME...]]
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
VALONLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --hosts)    HOSTS_SPEC="$2"; shift 2;;
    --minutes)  MINUTES="$2"; shift 2;;
    --jobs)     JOBS="$2"; shift 2;;
    --data)     LDATA="$2"; shift 2;;
    --validate) VALIDATE=1; shift;;
    --dry-run)  DRYRUN=1; shift;;
    --validate-only) VALIDATE=1; VALONLY=1; shift;;
    --only)     ONLY="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

CLIENT=./build-dbg/takclient
[ -x "$CLIENT" ] || { echo "build-dbg/takclient missing" >&2; exit 2; }


OUT="${TMPDIR:-/tmp}/desync-remote-$$"; mkdir -p "$OUT"
CTL="$OUT/ctl-%C"
# -n on EVERY ssh here (stdin from /dev/null) is load-bearing, not tidiness. Without it
# ssh inherits the loop's stdin and DRAINS it, so a `while read` loop that runs ssh in its
# body processes its FIRST entry and then finds the input exhausted -- the remaining hosts
# are silently skipped, the loop body never runs for them, and nothing reports a failure
# because no code ran to fail.
#
# That cost five rounds of misdiagnosis here. Teardown left the second host shaped every
# single time while the first was always clean, and it looked like a WireGuard-specific
# race because that host happened to be the tunnel one. It was alphabetical: the registry
# is sorted, ssh ate the rest of the pipe after the first line, and the second host's
# entry was never read. The same bug was sitting in restore_moved, where it meant only the
# first host's override file was ever put back.
#
# No ssh call in this script feeds anything on stdin, so -n is safe everywhere and stops
# the next loop anyone adds from inheriting the same trap.
SSH=(ssh -n -o ControlMaster=auto -o ControlPath="$CTL" -o ControlPersist=15m -o BatchMode=yes)
# Two sweeps running at once would otherwise fight over the same ports: the table is
# indexed from PORT_BASE, so a second invocation hands its clients the first one's
# referees. Overridable rather than fixed.
PORT_BASE="${TAK_PORT_BASE:-7900}"
# Shaping interface is DETECTED per host (see run_one); this only overrides it.
MYIP=$(hostname -I 2>/dev/null | awk '{print $1}')
# Every referee this run starts is recorded here as "host<TAB>pid", and cleanup kills
# exactly those. `pkill -x takserver` would kill every server owned by the account --
# a concurrent sweep, or somebody's live game. A test harness must not be able to take
# down what it is testing alongside. (This bit during development: a stray pkill in a
# monitor killed servers out from under a running sweep and cost two confused runs.)
PIDFILE="$OUT/started-servers"
: >"$PIDFILE"
note_server() { printf '%s\t%s\n' "$1" "$2" >>"$PIDFILE"; }

# Files moved aside on a remote for the negative test, as "host<TAB>path<TAB>backup".
# Recorded BEFORE the move and restored by cleanup() however we exit -- an interrupted
# run must not leave a host missing a gameplay override, or every later full-tier run
# there fails for a reason that has nothing to do with what it is testing.
MOVEDFILE="$OUT/moved-aside"
: >"$MOVEDFILE"
# Interfaces this sweep has shaped, as "host<TAB>iface", so cleanup can unshape them
# even when the worker that applied the shaping never got to run its own teardown.
SHAPEDFILE="$OUT/shaped-ifaces"
: >"$SHAPEDFILE"
NETEM_EXPIRY="${TAK_NETEM_EXPIRY:-1800}"   # remote self-revert, seconds
# Keep a record until its file is VERIFIABLY back. Clearing the list regardless of
# whether the restore worked destroys the only thing that could retry it: an ssh drop
# mid-restore would leave the shared install missing a gameplay override and no record
# that it ever moved. Entries that fail are kept, so the EXIT trap tries again, and the
# failure is propagated rather than swallowed.
restore_moved() {
  [ -s "$MOVEDFILE" ] || return 0
  local _left="$MOVEDFILE.retry" _rc=0
  : >"$_left"
  while IFS=$'\t' read -r _h _orig _bak; do
    [ -n "$_h" ] || continue
    # Restore, then CONFIRM: "mv exited 0" over ssh is not proof the file is there.
    if "${SSH[@]}" "$RUSER@$_h" "[ -f '$_bak' ] && mv -f '$_bak' '$_orig'; [ -f '$_orig' ]" >/dev/null 2>&1; then
      continue
    fi
    printf '%s\t%s\t%s\n' "$_h" "$_orig" "$_bak" >>"$_left"
    _rc=1
  done <"$MOVEDFILE"
  mv -f "$_left" "$MOVEDFILE"
  return $_rc
}

# Every descendant of a pid, deepest first. pgrep -P walks the actual parent links, so
# this matches on PROCESS ANCESTRY, never on a command string -- a pattern match here
# would find this script itself, and the sweep would kill its own cleanup.
descendants() {
  local p; for p in $(pgrep -P "$1" 2>/dev/null); do descendants "$p"; echo "$p"; done
}

# Stop the per-host worker subshells BEFORE unshaping. On an interrupt only the top-level
# script gets the signal -- the workers are background jobs and carry on -- so unshaping
# first just races them: cleanup removes the qdisc, a worker that is mid-run installs it
# again, and the host is left shaped with the sweep already gone. (Observed exactly that:
# one host clean, the other still shaped after a mid-flight interrupt.)
reap_workers() {
  local pids="" p
  for p in "${HOST_PIDS[@]:-}"; do
    [ -n "$p" ] || continue
    pids="$pids $(descendants "$p") $p"
  done
  [ -n "${pids// /}" ] || return 0
  kill $pids 2>/dev/null || true
  local i; for i in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 $pids 2>/dev/null || break
    sleep 0.5
  done
  kill -9 $pids 2>/dev/null || true
}

cleanup() {
  rm -f "${TAK_SWEEP_SNAPSHOT:-}" 2>/dev/null || true
  reap_workers
  restore_moved
  # Unshape anything this sweep shaped. The per-run teardown handles the normal case;
  # this catches a worker that was killed before it could.
  # UNSHAPE WHAT THIS SWEEP SHAPED.
  #
  # The bug that made this necessary was NOT a race, though it looked like one for five
  # rounds: ssh inherits the `while read` loop's stdin and drains the pipe, so the body
  # ran for the first registry entry and never for the second. The second host stayed
  # shaped and nothing warned, because no code ran to warn. `ssh -n` (set on every ssh in
  # this script) is the actual fix; everything below is about not LYING when it fails.
  #
  # Cleanup gets its OWN ssh options rather than the shared mux: it runs at exit, when the
  # ControlMaster may already be going away, and a cleanup that blocks forever on a dead
  # mux socket is as bad as one that skips a host. Connect timeouts and keepalives bound
  # it; no multiplexing means it does not depend on a socket that is being torn down.
  local CLEANSSH=(ssh -n -T -o BatchMode=yes -o ControlMaster=no -o ControlPath=none
                  -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=2)
  if [ -s "${SHAPEDFILE:-/nonexistent}" ]; then
    sort -u "$SHAPEDFILE" | while IFS=$'\t' read -r _h _if; do
      [ -n "$_h" ] && [ -n "$_if" ] || continue
      # The interface name is interpolated into a remote shell command; keep it to the
      # characters an interface can actually have.
      case "$_if" in *[!A-Za-z0-9_.:-]*)
        echo "WARN cleanup: refusing odd interface name '$_if' on $_h" >&2; continue ;;
      esac
      _streak=0
      for _try in 1 2 3 4 5 6 7 8; do
        "${CLEANSSH[@]}" "$RUSER@$_h" "sudo -n /usr/sbin/tc qdisc del dev '$_if' root" >/dev/null 2>&1 || true
        sleep 1
        # POSITIVE PROOF ONLY. `ssh "tc show | grep -q netem"` returns non-zero both when
        # the interface is clean and when the ssh could not run at all, so testing its
        # status counts an unreachable host as a cleaned one. Read the OUTPUT instead, and
        # treat "could not read it" as not-clean.
        #
        # And check for the harness's own `prio` root as well as netem: deleting the root
        # takes the whole tree, so if only netem is gone the delete did not do what it was
        # supposed to, and the leftover root is still ours to remove.
        if _out=$("${CLEANSSH[@]}" "$RUSER@$_h" "LC_ALL=C /usr/sbin/tc qdisc show dev '$_if' && echo __TCOK__" 2>/dev/null); then
          case "$_out" in
            *__TCOK__*)
              case "$_out" in
                *netem*|*"qdisc prio 1:"*) _streak=0 ;;
                *) _streak=$((_streak + 1)); [ "$_streak" -ge 2 ] && break ;;
              esac ;;
            *) _streak=0 ;;    # ran, but not the output we expect -- unknown, not clean
          esac
        else
          _streak=0            # query failed -- unknown, NOT proof of clean
        fi
      done
      if [ "$_streak" -lt 2 ]; then
        echo "WARN cleanup: $_h still shaped on $_if -- clear it by hand" >&2
        printf '%s\t%s\n' "$_h" "$_if" >>"$SHAPEDFILE.unclean"
      fi
    done
    # Surface a teardown failure in the sweep's OWN result, not only in a log line that
    # scrolls past. A harness that leaves a host shaped has to say so where it is seen.
    if [ -s "$SHAPEDFILE.unclean" ]; then
      echo "CLEANUP FAILED -- these interfaces are still shaped:" >&2
      sort -u "$SHAPEDFILE.unclean" | sed 's/^/  /' >&2
    fi
  fi
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
HOST_PIDS=()
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
  # ALL ON AN 8-SEAT MAP: these hold the start until the table is full (TAK_MP_WAIT),
  # and a map with fewer start positions can never fill it.
  "2h-baseline|Ulasem Arena|||human|light|2"
  "2h-gods|Ulasem Arena|TAK_GODS=1||human|light|2"
  "2h-crusades|Ulasem Arena||--crusades|human|light|2"
  "2h-stress|Ulasem Arena|TAK_STRESS=1||human|heavy|2"
  # ORDER-ISSUING humans (TAK_AUTOPLAY). Everything above has its humans standing
  # still, so several clients landing commands on the SAME tick -- the ordinary case in
  # a real match, and where the server's per-tick command buffer interleaves them -- is
  # never exercised. These do that.
  #
  # They are CONSENSUS tests, not golden-hash tests: the server assigns a command the
  # tick it arrives on, so the same seed legitimately produces a different hash each
  # run. Judge them by "all clients and the referee agreed", never by comparing a hash
  # against a previous sweep.
  "2h-orders|Ulasem Arena|TAK_AUTOPLAY=10||human|light|2"
  "2h-orders-stress|Ulasem Arena|TAK_AUTOPLAY=20 TAK_STRESS=1||human|heavy|2"
  "4h-orders|Ulasem Arena|TAK_AUTOPLAY=15||human|light|4"
  "3h-baseline|Ulasem Arena|||human|light|3"
  "4h-gods|Ulasem Arena|TAK_GODS=1||human|light|4"
  # --- over a LATENT link. TAK_RTT/TAK_JITTER put a delaying relay in front of the
  # server (tools/netdelay.py); the client is otherwise unchanged. These exercise the
  # adaptive jitter buffer, the kMaxLeadTicks flow-control window and -- with orders in
  # flight -- command bucketing by arrival tick, none of which do anything on a LAN.
  #
  # The order-issuing ones are CONSENSUS tests twice over: live commands already make a
  # run unreproducible, and latency changes which tick each command lands on, so two
  # runs legitimately differ. Judge them by "everyone agreed", never by hash.
  "net-rtt50|Ulasem Arena|TAK_RTT=50||human|light"
  "net-rtt100|Ulasem Arena|TAK_RTT=100||human|light"
  "net-jitter|Ulasem Arena|TAK_RTT=100 TAK_JITTER=30||human|light"
  "net-rtt500|Ulasem Arena|TAK_RTT=500||human|light"
  "net-loss1|Ulasem Arena|TAK_RTT=100 TAK_LOSS=1||human|light"
  "net-loss3-orders|Ulasem Arena|TAK_RTT=100 TAK_LOSS=3 TAK_AUTOPLAY=15||human|light"
  "net-orders100|Ulasem Arena|TAK_RTT=100 TAK_AUTOPLAY=15||human|light"
  "net-2h-rtt100|Ulasem Arena|TAK_RTT=100 TAK_AUTOPLAY=15||human|light|2"
  "w-allai-stress|Ulasem Arena|TAK_STRESS=1||watch|heavy"
  "w-allai-bench|Ulasem Arena|TAK_BENCH=3||watch|heavy"
)

SPEED_DEFAULT="TAK_SPEED=40"

# --only name[,name...] keeps just those runs. For working on ONE behaviour (the latency
# shaping and its teardown, say) without sitting through the other 30-odd runs first.
#
# It fails loudly on a name that matches nothing. A filter that silently selects an empty
# set would run a sweep of zero runs and report no failures -- and that is exactly how
# this got tested wrong: an invented `TAK_ONLY=...` that this script never read was passed
# twice, and both runs quietly executed the FULL sweep while appearing to be targeted.
if [ -n "${ONLY:-}" ]; then
  _keep=()
  for _spec in "${RUNS[@]}"; do
    _name=${_spec%%|*}
    case ",$ONLY," in *",$_name,"*) _keep+=("$_spec");; esac
  done
  [ "${#_keep[@]}" -gt 0 ] || {
    echo "--only '$ONLY' matched none of the ${#RUNS[@]} runs" >&2; exit 2; }
  RUNS=("${_keep[@]}")
  echo "--only '$ONLY': ${#RUNS[@]} run(s) selected"
fi

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
  # Defined HERE, before anything calls it. It was defined 50 lines further down, so the
  # netem capability probe ran against an undefined function, took the failure branch and
  # silently fell back to the relay -- every "shaped" run was the relay, and netem was
  # never used at all. The fallback is what hid it: those runs still passed. Only the
  # loss runs, which the relay cannot do, surfaced it by skipping. (Second time in this
  # file: the build-id gate had the same shape with VSSH.)
  local SSHH=(ssh -n -o ControlMaster=auto -o ControlPath="$OUT/ctl-%C" -o ControlPersist=15m -o BatchMode=yes)  # -n: see SSH above
  rsh1() { "${SSHH[@]}" "$RUSER@$host" "$@"; }

  # LATENCY SHAPING. TAK_RTT / TAK_JITTER ride in the ENVS column rather than adding
  # positional fields -- a wider spec is how --overrides full once ended up in the seat
  # slot and silently tested nothing. They are stripped before the client sees them:
  # they configure a relay in front of the server, not the game.
  #
  # Every run so far has been sub-millisecond LAN, which leaves the latency-sensitive
  # machinery untested: netDelay_ sizes itself from measured RTT, the server may not
  # lead its slowest consumer by more than kMaxLeadTicks, and a spectator heartbeats to
  # keep that fed. Measured: flat 4x through 100ms, 3.41x at 500ms, no desync at any of
  # them -- the knee is where the round trip starts eating the 120-tick lead window.
  local rtt=0 jit=0 loss=0 thost="$host" tport="$port" proxypid="" shaped_netem="" shaped_iface=""
  case "$envs" in *TAK_RTT=*)    rtt=$(printf '%s' "$envs" | grep -oE 'TAK_RTT=[0-9]+' | cut -d= -f2);; esac
  case "$envs" in *TAK_JITTER=*) jit=$(printf '%s' "$envs" | grep -oE 'TAK_JITTER=[0-9]+' | cut -d= -f2);; esac
  case "$envs" in *TAK_LOSS=*)   loss=$(printf '%s' "$envs" | grep -oE 'TAK_LOSS=[0-9.]+' | cut -d= -f2);; esac
  envs=$(printf '%s' "$envs" | sed -E 's/TAK_(RTT|JITTER|LOSS)=[0-9.]+//g')

  if [ "${rtt:-0}" != "0" ] || [ "${jit:-0}" != "0" ] || [ "${loss:-0}" != "0" ]; then
    # PREFER NETEM. It delays real packets and can DROP them; the relay delays a TCP
    # byte stream and structurally cannot lose anything, so a loss run is only
    # meaningful under netem. Fall back to the relay when the host has no passwordless
    # tc -- but never silently pretend a loss figure was applied.
    # ONE SHAPED RUN PER HOST AT A TIME. netem is per-INTERFACE state, not per-run: a
    # second `tc qdisc add` on the same device replaces the first. With several jobs in
    # flight on one host, a run asking for 50ms could silently be handed a concurrent
    # run's 500ms, and whichever teardown fired last decided whether anything was left
    # behind at all -- which is how a sweep finished with netem still applied to vpn3.
    # Unshaped runs are unaffected and stay parallel; only shaped ones queue.
    # Lock in a FIXED place, not under $OUT: that path carries this sweep's pid, so two
    # sweeps against the same host took different locks, fought over the one interface
    # and each could tear down the other's shaping.
    exec 9>"${TMPDIR:-/tmp}/tak-netem.$host.lock"
    flock 9
    if rsh1 "sudo -n /usr/sbin/tc -V >/dev/null 2>&1"; then
      # SHAPE THE INTERFACE THAT ROUTES TO THIS CLIENT, detected per host -- never a
      # hardcoded name. tak answers over its LAN port; vpn3 answers through a WireGuard
      # tunnel (wg0) whose packets then leave via enp1s0. A fixed name would have run tc
      # against a device that does not exist there, and since tc's output is silenced
      # the run would have gone ahead UNSHAPED and reported a clean pass for a latency
      # test containing no latency.
      #
      # THE TUNNEL, NOT THE NIC UNDER IT. Measured, both give the same thing at the far
      # end -- 113.9ms on wg0 against 113.6ms on enp1s0 over a 13.9ms baseline -- because
      # the game rides the tunnel either way, and a dropped plaintext packet loses the
      # same TCP segment as its dropped carrier. What differs is blast radius: on the
      # tunnel the traffic is plaintext, so it can be filtered down to the game's own
      # port, while on the physical NIC it is encapsulated and the inner port is
      # invisible -- leaving no choice but to shape the whole interface, which slows ssh
      # to that host and everything else it is doing. Shaping the NIC would only be
      # right if WireGuard's own behaviour under loss were what was being tested.
      # ASK THE HOST WHICH ADDRESS WE ARRIVE FROM. `hostname -I` gives this machine's
      # first address, which on a multihomed client need not be the source used to reach
      # THIS host -- and routing the wrong address back can install netem on an
      # interface the game never touches, after which the qdisc check happily passes
      # while every packet runs unshaped. $SSH_CLIENT is what the host actually sees.
      local myaddr; myaddr=$(rsh1 'echo $SSH_CLIENT' 2>/dev/null | awk '{print $1}')
      [ -n "$myaddr" ] || myaddr="$MYIP"
      local iface="${TAK_NETEM_IFACE:-}"
      [ -n "$iface" ] || iface=$(rsh1 "ip -o route get $myaddr 2>/dev/null | grep -oE 'dev [a-z0-9]+' | head -1 | cut -d' ' -f2")
      if [ -z "$iface" ]; then
        echo "SKIP $name ($host): cannot determine the interface back to $myaddr"; flock -u 9; return 0
      fi
      # Record BEFORE touching the interface, and arm a remote expiry. If this worker is
      # killed -- interrupt, timeout, the sweep stopped -- neither shaping_down call is
      # reached, and without a record the top-level cleanup has no idea the host was ever
      # shaped. Sweeps get interrupted constantly; a host left permanently latent is the
      # worst outcome here, because everything afterwards just looks inexplicably slow.
      printf '%s\t%s\n' "$host" "$iface" >>"$SHAPEDFILE"
      rsh1 "nohup sh -c 'sleep ${NETEM_EXPIRY}; sudo /usr/sbin/tc qdisc del dev $iface root' >/dev/null 2>&1 </dev/null &" >/dev/null 2>&1
      rsh1 "sudo /usr/sbin/tc qdisc del dev $iface root 2>/dev/null
            sudo /usr/sbin/tc qdisc add dev $iface root handle 1: prio bands 3 \
                 priomap 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
            _a='delay ${rtt:-0}ms'
            [ '${jit:-0}' != '0' ] && _a=\"\$_a ${jit}ms distribution normal\"
            [ '${loss:-0}' != '0' ] && _a=\"\$_a loss ${loss}%\"
            sudo /usr/sbin/tc qdisc add dev $iface parent 1:3 handle 30: netem \$_a
            sudo /usr/sbin/tc filter add dev $iface protocol ip parent 1:0 prio 3 \
                 u32 match ip sport $port 0xffff flowid 1:3" >/dev/null 2>&1
      # CONFIRM IT TOOK. Every tc call above is silenced, so a failure at any step would
      # otherwise leave the run unshaped and indistinguishable from a passing one.
      if ! rsh1 "/usr/sbin/tc qdisc show dev $iface | grep -q netem"; then
        echo "SKIP $name ($host): netem did not apply on $iface"; flock -u 9; return 0
      fi
      shaped_netem="$host"; shaped_iface="$iface"
    elif [ "${loss:-0}" != "0" ]; then
      echo "SKIP $name ($host): TAK_LOSS needs netem, and sudo tc is not available there"
      flock -u 9; return 0
    else
      local pport=$((PORT_BASE + 200 + idx))
      python3 tools/netdelay.py --listen "$pport" --to "$host:$port" \
              --rtt "${rtt:-0}" --jitter "${jit:-0}" >/dev/null 2>&1 &
      proxypid=$!
      sleep 2
      thost=127.0.0.1; tport="$pport"
    fi
  fi
  local clog="$OUT/$name.client.log"

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
  # Kill the relay on THIS path too. It is started before the readiness check, and an
  # early return skipped the teardown further down -- so a failed server start leaked a
  # listener that outlived the sweep. The next run on that port then cannot bind, or
  # worse, silently connects through the stale one. (Observed: two leaked relays.)
  # ONE teardown, reachable from EVERY exit. It previously sat inside the readiness
  # FAILURE branch below, so unshaping only happened when the server failed to start --
  # on the normal path netem was simply left applied, and the "confirm it is gone" check
  # never ran either, which is why a whole sweep reported zero warnings while both hosts
  # ended up shaped. A cleanup that only runs on the error path is not cleanup.
  shaping_down() {
    [ -n "$proxypid" ] && kill "$proxypid" 2>/dev/null
    if [ -n "$shaped_netem" ]; then
      rsh1 "sudo /usr/sbin/tc qdisc del dev $shaped_iface root 2>/dev/null; true" >/dev/null 2>&1
      # Confirm, then retry once. Leaving netem on would silently apply this run's
      # latency to every later run on that host -- including ones that are not latency
      # tests, which would then be measuring a link nobody configured.
      if rsh1 "/usr/sbin/tc qdisc show dev $shaped_iface | grep -q netem"; then
        echo "WARN $name ($host): netem NOT removed from $shaped_iface -- retrying"
        rsh1 "sudo /usr/sbin/tc qdisc del dev $shaped_iface root 2>/dev/null; true" >/dev/null 2>&1
        rsh1 "/usr/sbin/tc qdisc show dev $shaped_iface | grep -q netem" && \
          echo "WARN $name ($host): STILL shaped -- clear $shaped_iface by hand"
      fi
      shaped_netem=""
    fi
    flock -u 9 2>/dev/null || true
  }

  [ "$up" = "1" ] || {
      shaping_down
      echo "FAIL $name ($host): remote server never came up (port $port)"; return 1; }

  # SEAT: watch -> spectator (TAK_MP_WATCH=1, 8 AIs). human -> a real player slot with
  # 7 AIs alongside, which is what makes the referee compare hashes at all.
  # Seat the table. N humans -> (8-N) AIs.
  #
  # TAK_MP_WAIT IS ONLY SAFE WHEN THE MAP REALLY HAS 8 SEATS. A room holds as many slots
  # as the map has start positions (mpCapacity clamps 2..8), so on a smaller map `ready`
  # can never reach 8 and the host waits for a table that cannot exist -- the game never
  # starts and the run burns its whole timeout. Hardcoding TAK_MP_WAIT=8 did exactly
  # that to every run on Inner Circle, Two Castles, Aibel's Seaport, Lake Lokken and
  # Tarosian Plain: seven runs, no "starting with N players" line between them.
  #
  # A single human needs no wait at all: the AIs seat immediately and the host is ready,
  # so the default (any 2 ready) starts the game correctly on a map of any size. Only a
  # multi-human run has to hold for joiners, and those are pinned to 8-seat maps.
  local nai=$((8 - humans))
  local seatenv="TAK_MP_AIS=$nai"
  [ "$humans" -gt 1 ] && seatenv="TAK_MP_AIS=$nai TAK_MP_WAIT=8"
  [ "$seat" = "watch" ] && seatenv="TAK_MP_WATCH=1 TAK_MP_AIS=8"

  local secs=$((MINUTES * 60))
  # shellcheck disable=SC2086
  env TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy $seatenv $SPEED_DEFAULT $envs \
      timeout -k 30 $((secs + 300)) $CLIENT game "$map" --data "$LDATA" \
      --server "$thost" --serverport "$tport" --mphost --time "$secs" $flags \
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
          --server "$thost" --serverport "$tport" --mpjoin --time "$secs" $flags \
          >"$OUT/$name.client$j.log" 2>&1 &
      jpids+=($!)
    done
  fi
  wait "$hostpid"; local rc=$?
  for j in "${jpids[@]}"; do wait "$j" || true; done
  shaping_down          # the normal path -- this is the one that was missing

  # STOP THE REFEREE BEFORE READING ITS LOG. It keeps writing as the client goes away
  # ("client N dropped", "game ended"), so fetching while it runs and then measuring
  # the source compares a snapshot against a file that has since grown -- which reads
  # as a truncated transfer when nothing went wrong. Kill first, then read a file with
  # no writer.
  #
  # Kill by pid, not by pattern: a pattern would also hit a concurrent sweep on the
  # same port, and pkill -x would hit every server on the account.
  [ -n "$proxypid" ] && kill "$proxypid" 2>/dev/null
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
    # THIRD GATE: the server must be built from the SAME SOURCE as the client.
  #
  # Two peers only stay in lockstep if they run the same simulation code, and nothing
  # else here checks that: the gameplay-data hash covers data, kNetVersion covers the
  # wire format, and neither moves when sim behaviour changes. A test server one commit
  # behind produced five "desyncs" that were nothing of the sort -- its placement code
  # differed, so the worlds differed from tick zero, and two of them came back as
  # REFEREE SUSPECT because both clients agreed against it. Hours went into chasing an
  # engine bug that did not exist.
  #
  # A mismatch is a hard stop rather than a warning. A sweep whose verdict cannot
  # distinguish "the engine diverged" from "you forgot to deploy" is reporting noise.
  VSSH=(ssh -n -o ControlMaster=auto -o ControlPath="$OUT/ctl-%C" -o ControlPersist=15m -o BatchMode=yes)  # -n: see SSH above
  echo "== validating that $vhost runs the same source as this client =="
  _cbuild=$($CLIENT --version 2>/dev/null | grep -oE 'build [^)]+' | cut -d' ' -f2)
  _sbuild=$("${VSSH[@]}" "$RUSER@$vhost" "$RBIN --version" 2>/dev/null | grep -oE 'build [^)]+' | cut -d' ' -f2)
  if [ -z "$_cbuild" ] || [ -z "$_sbuild" ]; then
    echo "   FAIL -- could not read a build id (client='$_cbuild' server='$_sbuild')." >&2
    echo "           Rebuild both; without it a stale server reads as a desync." >&2
    exit 1
  fi
  if [ "$_cbuild" != "$_sbuild" ]; then
    echo "   FAIL -- server and client are built from DIFFERENT SOURCE." >&2
    echo "           client: $_cbuild" >&2
    echo "           server: $_sbuild   ($vhost)" >&2
    echo "           Redeploy before sweeping; any desync reported now would be this." >&2
    exit 1
  fi
  case "$_cbuild" in
    *-dirty) echo "   PASS -- both at $_cbuild"
             echo "           (note: -dirty, so the id does not fully describe what is running)";;
    *)       echo "   PASS -- both at $_cbuild";;
  esac

echo "== validating the detector with a PLANTED desync (TAK_FAKE_DESYNC=900) on $vhost =="
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

  # SECOND SAFETY NET: a client whose gameplay data differs from the host's must be
  # REJECTED AT LOAD, not let in to desync ten minutes later.
  #
  # The mismatch is made by MOVING THE HOST'S OVERRIDE ASIDE, not by handing the client
  # doctored data. That tests strictly more: if the server were silently ignoring the
  # override file, removing it would change nothing and the client would be admitted --
  # so a PASS here also proves the host was really reading it. The file goes back
  # immediately afterwards, and cleanup() restores it however this exits.
  echo "== validating the gameplay-override mismatch rejection on $vhost =="
  _ovr="$RDATA/overrides/units/arasword.fbi"
  _bak="$_ovr.negtest-bak"
  # An unreachable host must NOT read as "no override": that would skip the gate for a
  # connectivity fault and still report a pass.
  if ! "${VSSH[@]}" "$RUSER@$vhost" "true" >/dev/null 2>&1; then
    echo "   FAIL -- cannot reach $vhost to run the mismatch gate; stopping." >&2; exit 1
  fi
  _haveovr=0
  "${VSSH[@]}" "$RUSER@$vhost" "[ -f '$_ovr' ]" >/dev/null 2>&1 && _haveovr=1
  _negdata="$LDATA"

  if [ "$_haveovr" = "0" ]; then
    # NO SKIPPING. With no host override there is nothing to remove, but the gate still
    # has to run -- so manufacture the mismatch on the CLIENT side: a data dir of
    # symlinks to the real install plus one changed gameplay value. Weaker than the
    # removal test (it cannot also prove the host reads its override, there being none)
    # but it still proves the rejection fires, which is what the gate is for.
    echo "   (host has no override; manufacturing the mismatch client-side instead)"
    _negdata="$OUT/negdata"; rm -rf "$_negdata"; mkdir -p "$_negdata/overrides/units"
    for _e in "$LDATA"/*; do
      [ "$(basename "$_e")" = overrides ] && continue
      ln -s "$(realpath "$_e")" "$_negdata/$(basename "$_e")" 2>/dev/null || true
    done
    ./build/hpitool cat "$LDATA"/V3Rocket.hpi units/arasword.fbi 2>/dev/null \
      | sed 's/maxdamage = [0-9]*;/maxdamage = 4242;/' >"$_negdata/overrides/units/arasword.fbi"
    if [ ! -s "$_negdata/overrides/units/arasword.fbi" ]; then
      echo "   FAIL -- could not build a mismatching data dir, so the rejection gate" >&2
      echo "           cannot be exercised; stopping rather than reporting a pass." >&2
      exit 1
    fi
  else
    # Record the move BEFORE making it: a crash between the two must still restore.
    printf '%s\t%s\t%s\n' "$vhost" "$_ovr" "$_bak" >>"$MOVEDFILE"
    "${VSSH[@]}" "$RUSER@$vhost" "mv -f '$_ovr' '$_bak'" >/dev/null 2>&1
    if "${VSSH[@]}" "$RUSER@$vhost" "[ -f '$_ovr' ]" 2>/dev/null; then
      echo "   FAIL -- could not move the host override aside; stopping." >&2; exit 1
    fi
  fi
  {

    "${VSSH[@]}" "$RUSER@$vhost" "nohup $RBIN --port 7891 --data $RDATA --no-auth --seed 999 >/tmp/tak-neg.log 2>&1 </dev/null & echo \$!" >/dev/null 2>&1
    for _ in $(seq 60); do "${VSSH[@]}" "$RUSER@$vhost" "grep -q listening /tmp/tak-neg.log 2>/dev/null" && break; sleep 2; done
    env TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy TAK_MP_AIS=7 TAK_SPEED=40 \
        timeout -k 20 180 $CLIENT game "Ulasem Arena" --data "$_negdata" \
        --server "$vhost" --serverport 7891 --mphost --time 60 --overrides full \
        >"$OUT/negative.client.log" 2>&1
    "${VSSH[@]}" "$RUSER@$vhost" "cat /tmp/tak-neg.log" >"$OUT/negative.server.log" 2>/dev/null
    "${VSSH[@]}" "$RUSER@$vhost" "ps -o pid,args -C takserver --no-headers | awk '/7891/{print \$1}' | xargs -r kill" >/dev/null 2>&1

    _rejected=0
    grep -qi "override mismatch" "$OUT/negative.server.log" "$OUT/negative.client.log" 2>/dev/null && _rejected=1

    # PUT IT BACK before judging, so a failure cannot leave the host altered.
    # PUT IT BACK before judging, so a failing test cannot also leave the host altered.
    # restore_moved now returns non-zero and KEEPS its record when a restore fails.
    if [ "$_haveovr" = "1" ]; then
      if ! restore_moved; then
        echo "   FAIL -- could not restore the host override. cleanup() will retry on" >&2
        echo "           exit; if that also fails, fix $vhost by hand: $_bak" >&2
        exit 1
      fi
      if ! "${VSSH[@]}" "$RUSER@$vhost" "[ -f '$_ovr' ]" >/dev/null 2>&1; then
        echo "   FAIL -- the host override was NOT restored. Fix $vhost before running" >&2
        echo "           anything else: $_bak" >&2
        exit 1
      fi
    fi

    if [ "$_rejected" = "1" ]; then
      echo "   PASS -- $(grep -ohi 'rejected at load.*' "$OUT/negative.server.log" | head -1)"
      [ "$_haveovr" = "1" ] && \
        echo "           (host override removed and restored; the server was reading it)"
    else
      echo "   FAIL -- a client with gameplay data the host lacks was NOT rejected." >&2
      echo "           Either the check is broken or the host never read its override;" >&2
      echo "           that client would have desynced mid-game. Stopping." >&2
      exit 1
    fi
  }

  if [ "$VALONLY" = "1" ]; then echo "all gates passed (build id, desync detector, override mismatch)"; exit 0; fi
fi

# Dispatch. Each host drains its own queue at its own concurrency, so a 2-core box
# never gates a 32-core one: the small host simply takes fewer, lighter runs and
# finishes when it finishes.

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
echo "note: TAK_AUTOPLAY runs issue live commands, so the server buckets them by the"
echo "      tick they ARRIVE on and the same seed gives a different hash each run."
echo "      They prove consensus, not reproducibility -- do not diff their hashes."
echo "runs that did not complete:"
for f in "$OUT"/*.client.log; do
  grep -q "mp-headless done" "$f" 2>/dev/null || echo "  $(basename "$f")"
done
echo "logs: $OUT"
