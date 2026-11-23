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
# THE REFEREE IS BUILT IN AN ubuntu:24.04 CONTAINER AND SHIPPED. This host is glibc
# 2.43 and emits sqrtf/remainderf at GLIBC_2.43; the remotes are Ubuntu 24.04 on 2.39
# and have no compiler to build it themselves, so a binary built here will not start
# there. Container-built tops out at GLIBC_2.38. Keep DEBUG test hooks enabled --
# TAK_GODS and the rest of the harness hooks are #ifndef NDEBUG, so a release referee
# ignores them and the runs that depend on them desync by construction.
# Use Debug with -O2 -g for stress sweeps: this retains the hooks while avoiding
# unoptimized startup/tick times being misreported as network timeouts.
#
#   podman run --rm -v $PWD:/src:z -v /tmp/ctr-out:/out:z docker.io/library/ubuntu:24.04 \
#     bash -c 'apt-get update && apt-get install -y cmake ninja-build g++ git nasm ... &&
#              cd /src && PREFIX=/src/third_party/static-deps-u2404 ./tools/build-static-deps.sh &&
#              cmake -B /out/bdbg -G Ninja -DCMAKE_BUILD_TYPE=Debug \
#                    -DCMAKE_CXX_FLAGS_DEBUG="-O2 -g" \
#                    -DTAK_STATIC_DEPS_PREFIX=/src/third_party/static-deps-u2404 &&
#              cmake --build /out/bdbg --target takserver'
#
# build-static-deps.sh reads PREFIX, NOT TAK_STATIC_DEPS_PREFIX. Setting only the
# latter makes it report "already built" and silently reuse whatever this host left in
# third_party/static-deps -- Fedora archives, linked into an Ubuntu binary.
#
# THIS MEANS THE REFEREE AND THE CLIENT ARE BUILT BY DIFFERENT COMPILERS -- Ubuntu's
# GCC 13.3 against whatever Fedora ships here. An earlier version of this comment
# claimed the opposite, that shipping a static build kept both on the SAME GCC and
# that this was what made a hash mismatch unambiguous. That was never quite true and
# is now plainly false, so do not lean on it: a mismatch here is a cross-toolchain
# difference until you have ruled one out.
#
# What makes that acceptable rather than a confound is that the pairing is now TESTED
# directly: tools/check-determinism.sh golden-hashes the detmath shim across gcc and
# clang at -O0/-O2/-O3, and the sim itself has been run gcc-referee against
# clang-client to a byte-identical state hash. Cross-compiler agreement is a property
# this engine is supposed to have -- it is the point of the fixed-point sim -- so
# exercising it here is coverage, not noise. Treat a mismatch as real, but confirm it
# reproduces with both sides on one compiler before calling it a logic bug.
#
# usage: tools/desync-hunt-remote.sh [--host H] [--minutes N] [--jobs N] [--validate]
#        [--only NAME[,NAME...]]
# Set TAK_CLIENT=./build-o2/takclient to use an optimized client with Debug hooks.
# Set TAK_SERVER=/home/pocket_geek/takserver.sweep170 to test an isolated remote binary.
# TAK_REMOTE_REPLAY_DIR and TAK_REMOTE_CACHE_DIR isolate writable test data.
# TAK_VALIDATE_PORT / TAK_NEGATIVE_PORT isolate the two validation listeners.
# --minutes is simulated time. TAK_WALL_TIMEOUT can set a per-client real-time
# deadline in seconds (default: simulated seconds + 300). Dense Flowfield stress
# runs can need a longer deadline without changing their tick or hash checks.
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
RREPLAY="${TAK_REMOTE_REPLAY_DIR:-/home/pocket_geek/tak_replay}"
RCACHE="${TAK_REMOTE_CACHE_DIR:-$RDATA}"
RBIN="${TAK_SERVER:-/home/pocket_geek/takserver}"
LDATA="assets/game"
MINUTES=45
# Per-host concurrency normally comes from HOSTS_SPEC's middle field. --jobs, when
# given, overrides it for EVERY host: an explicit flag on the command line should beat
# a built-in default. Empty means "not given" -- distinguishable from --jobs 0, which
# is rejected below rather than silently meaning something.
#
# This used to be JOBS=12, assigned by --jobs and then never read by anything. The
# flag was documented in the usage line and silently did nothing, so `--jobs 2` on a
# host spec'd at 10 still ran 10 at a time. That cost a wrong conclusion here: five
# stress runs were timing out, --jobs 2 was used to rule out resource contention, it
# did not actually reduce concurrency, and the runs had to be isolated a second time
# with --hosts before contention could honestly be excluded.
JOBS=""
VALIDATE=0
DRYRUN=0
VALONLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --hosts)    HOSTS_SPEC="$2"; shift 2;;
    --minutes)  MINUTES="$2"; shift 2;;
    --jobs)     JOBS="$2"
                # Digits only FIRST -- the value is interpolated into arithmetic below,
                # and $(( )) evaluates what it is given.
                case "$JOBS" in ''|*[!0-9]*) echo "--jobs needs a positive integer" >&2; exit 2;; esac
                # Then normalise with 10# and test the VALUE, not the string. Testing the
                # string caught "0" but not "00", which sailed through as a concurrency of
                # zero: the dispatch throttle is `while [ running -ge H_JOBS ]`, and
                # `-ge 00` is true even with nothing running, so the sweep waits forever
                # for a slot that cannot open. Leading zeros were the other half -- bash
                # reads 08 as octal and dies with "value too great for base" mid-assignment.
                JOBS=$((10#$JOBS))
                [ "$JOBS" -gt 0 ] || { echo "--jobs needs a positive integer" >&2; exit 2; }
                shift 2;;
    --data)     LDATA="$2"; shift 2;;
    --validate) VALIDATE=1; shift;;
    --dry-run)  DRYRUN=1; shift;;
    --validate-only) VALIDATE=1; VALONLY=1; shift;;
    --only)     ONLY="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

if [ -n "${TAK_WALL_TIMEOUT:-}" ]; then
  if ! [[ "$TAK_WALL_TIMEOUT" =~ ^[1-9][0-9]{0,4}$ ]] ||
      [ "$TAK_WALL_TIMEOUT" -gt 86400 ]; then
    echo "TAK_WALL_TIMEOUT needs an integer from 1 to 86400 seconds" >&2
    exit 2
  fi
fi

BUILD="${BUILD:-build-dbg}"   # the local build dir
CLIENT="${TAK_CLIENT:-./$BUILD/takclient}"
[ -x "$CLIENT" ] || { echo "$CLIENT missing" >&2; exit 2; }


OUT="${TMPDIR:-/tmp}/desync-remote-$$"; mkdir -p "$OUT"
# Test hosts must not inherit a player's remembered local packs or UI settings.
export XDG_CONFIG_HOME="$OUT/config" XDG_DATA_HOME="$OUT/preferences"
export SDL_AUDIODRIVER=dummy
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
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
VALIDATE_PORT="${TAK_VALIDATE_PORT:-7890}"
NEGATIVE_PORT="${TAK_NEGATIVE_PORT:-7891}"
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
# run must not leave a host missing a game-mod file, or every later game-mods run
# there fails for a reason that has nothing to do with what it is testing.
MOVEDFILE="$OUT/moved-aside"
: >"$MOVEDFILE"
# Interfaces this sweep has shaped, as "host<TAB>iface", so cleanup can unshape them
# even when the worker that applied the shaping never got to run its own teardown.
SHAPEDFILE="$OUT/shaped-ifaces"
: >"$SHAPEDFILE"
NETEM_EXPIRY="${TAK_NETEM_EXPIRY:-1800}"   # remote self-revert, seconds
# Remote file naming who currently owns an interface's shaping (see run_one).
NETEM_OWNER="/tmp/tak-netem-owner"
# Keep a record until its file is VERIFIABLY back. Clearing the list regardless of
# whether the restore worked destroys the only thing that could retry it: an ssh drop
# mid-restore would leave the shared install missing a game-mod file and no record
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
    sort -u "$SHAPEDFILE" | while IFS=$'\t' read -r _h _if _tok; do
      [ -n "$_h" ] && [ -n "$_if" ] || continue
      # TAKE THE SAME LOCK THE RUNS TAKE, and only unshape what is still OURS. Without
      # this, cleanup walks a HISTORICAL registry and deletes unconditionally: a second
      # sweep that legitimately acquired the interface after our shaping_down released it
      # would have its live shaping torn out from under it, and -- netem being verified
      # only at setup -- would then run unshaped and report a pass. That is the same
      # silent-pass failure this whole file keeps being about, so cleanup has to honour
      # the serialisation the runs use rather than bypassing it.
      #
      # -w so a stuck peer cannot wedge our exit. A failed lock never authorises a
      # deletion: another run can acquire ownership immediately after our query.
      exec 8>"${TMPDIR:-/tmp}/tak-netem.$_h.lock"
      _locked=0
      flock -w 30 8 2>/dev/null && _locked=1
      _owner=$("${CLEANSSH[@]}" "$RUSER@$_h" "if [ -e '$NETEM_OWNER.$_if' ]; then cat '$NETEM_OWNER.$_if'; else echo __TAK_NO_OWNER__; fi" 2>/dev/null) || _owner=""
      if [ -n "$_owner" ] && [ "$_owner" != "$_tok" ]; then
        [ "$_locked" = 0 ] || flock -u 8 2>/dev/null || true
        continue    # missing claim or another owner -- not ours to clear
      fi
      if [ "$_locked" = 0 ] || [ -z "$_tok" ] || [ "$_owner" != "$_tok" ]; then
        echo "WARN cleanup: cannot confirm locked ownership of $_if on $_h -- leaving watchdog armed" >&2
        printf '%s\t%s\n' "$_h" "$_if" >>"$SHAPEDFILE.unclean"
        [ "$_locked" = 0 ] || flock -u 8 2>/dev/null || true
        continue
      fi
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
      # Same rule as shaping_down: the claim is what keeps the remote watchdog willing to
      # retry, so it is only given up on a confirmed-clean interface.
      if [ "$_streak" -ge 2 ]; then
        "${CLEANSSH[@]}" "$RUSER@$_h" "rm -f '$NETEM_OWNER.$_if'" >/dev/null 2>&1 || true
      fi
      flock -u 8 2>/dev/null || true
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
    # Match the exact executable: renamed sweep binaries have a different comm,
    # and killing a stranger after its pid was recycled would break isolation.
    "${SSH[@]}" "$RUSER@$h" "for p in $pids; do \
         c=\$(readlink /proc/\$p/exe 2>/dev/null); \
         [ \"\$c\" = '$RBIN' ] && kill \$p 2>/dev/null; \
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
  "h-unitcap|Ulasem Arena|TAK_UNITCAP=2000||human|light"
  # h-cramped keeps the Monarch expendable: since the 263 air-combat work the Zhon god's
  # storms land on the idle seat's Monarch and end the Retail game at tick ~1,300 (retail-
  # faithful, not a desync), which left this entry testing almost nothing.
  "h-cramped|Inner Circle|TAK_GODS=1 TAK_MONARCH_EXPENDABLE=1||human|light"
  "h-naval|Aibel's Seaport|||human|light"
  "h-naval-crus|Aibel's Seaport||--crusades|human|light"
  "h-lake|Lake Lokken|TAK_STRESS=1||human|heavy"
  "h-random-starts|Sand River Plain|TAK_RANDOM_STARTS=1||human|light"
  "h-monarch-exp|Ulasem Arena|TAK_MONARCH_EXPENDABLE=1 TAK_GODS=1||human|light"
  "h-mods-all|Ulasem Arena||--mods all|human|light"
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

# Keep a scenario's seed stable when --only filters the table. Otherwise a failed
# seed-2021 run becomes seed 2000 on retry and does not reproduce the original.
declare -A RUN_SEEDS=()
for _idx in "${!RUNS[@]}"; do
  _name=${RUNS[$_idx]%%|*}
  RUN_SEEDS[$_name]=$((2000 + _idx))
done

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
# shifts every field right: --overrides full (now --mods all) once landed in the SEAT
# slot, so the run never mounted the packs it was named for and still reported "ok". A sweep that
# quietly tests the wrong configuration is worse than one that fails.
for _spec in "${RUNS[@]}"; do
  _n=$(awk -F'|' '{print NF}' <<<"$_spec")
  if [ "$_n" != "6" ] && [ "$_n" != "7" ]; then
    echo "BAD RUN TABLE ENTRY ($_n fields, want 6 or 7 -- name|map|envs|flags|seat|weight[|humans]):" >&2
    echo "  $_spec" >&2
    exit 2
  fi
done

# tc u32 prints a 16-bit source-port match in the high half of the word at
# IPv4 offset 20. Require the key in the SAME terminal rule as flowid 1:3.
netem_proof_valid() {
  local qdisc="$1" filter="$2" port="$3" key
  key=$(printf '%04x0000/ffff0000' "$port")
  grep -Eq '^qdisc prio 1: root' "$qdisc" &&
  grep -Eq '^qdisc netem 30: .*parent 1:3([[:space:]]|$)' "$qdisc" &&
  awk -v key="$key" '
    /^filter / { rule = /protocol ip / && /pref 3 / && /flowid 1:3([[:space:]]|$)/ }
    rule && $1 == "match" && $2 == key && $3 == "at" && $4 == "20" { found = 1 }
    END { exit !found }
  ' "$filter"
}

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
  local port=$((PORT_BASE + idx)) seed=${RUN_SEEDS[$name]}
  # Defined HERE, before anything calls it. It was defined 50 lines further down, so the
  # netem capability probe ran against an undefined function, took the failure branch and
  # silently fell back to the relay -- every "shaped" run was the relay, and netem was
  # never used at all. The fallback is what hid it: those runs still passed. Only the
  # loss runs, which the relay cannot do, surfaced it by skipping. (Second time in this
  # file: the build-id gate had the same shape with VSSH.)
  local SSHH=(ssh -n -o ControlMaster=auto -o ControlPath="$OUT/ctl-%C" -o ControlPersist=15m -o BatchMode=yes)  # -n: see SSH above
  rsh1() { "${SSHH[@]}" "$RUSER@$host" "$@"; }
  local replay_dir="$RREPLAY/$port"
  rsh1 "mkdir -p '$replay_dir' '$RCACHE'" || {
    echo "FAIL $name ($host): cannot create isolated writable directories"; return 1; }

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
  local rtt=0 jit=0 loss=0 thost="$host" tport="$port" proxypid="" shaped_netem="" shaped_iface="" shaped_token=""
  case "$envs" in *TAK_RTT=*)    rtt=$(printf '%s' "$envs" | grep -oE 'TAK_RTT=[0-9]+' | cut -d= -f2);; esac
  case "$envs" in *TAK_JITTER=*) jit=$(printf '%s' "$envs" | grep -oE 'TAK_JITTER=[0-9]+' | cut -d= -f2);; esac
  case "$envs" in *TAK_LOSS=*)   loss=$(printf '%s' "$envs" | grep -oE 'TAK_LOSS=[0-9.]+' | cut -d= -f2);; esac
  envs=$(printf '%s' "$envs" | sed -E 's/TAK_(RTT|JITTER|LOSS)=[0-9.]+//g')

  # Defined before setup so partial tc failures take the same teardown as a game.
  local shaping_failed=0
  shaping_down() {
    [ -n "$proxypid" ] && kill "$proxypid" 2>/dev/null
    if [ -n "$shaped_netem" ]; then
      local _clean=0 _out _owner _try
      _owner=$(rsh1 "cat '$NETEM_OWNER.$shaped_iface'" 2>/dev/null) || _owner=""
      if [ -n "$shaped_token" ] && [ "$_owner" = "$shaped_token" ]; then
        for _try in 1 2; do
          rsh1 "sudo -n /usr/sbin/tc qdisc del dev '$shaped_iface' root" >>"$OUT/$name.netem-teardown.log" 2>&1 || true
          if _out=$(rsh1 "LC_ALL=C /usr/sbin/tc qdisc show dev '$shaped_iface' && echo __TCOK__" 2>&1); then
            printf '%s\n' "$_out" >>"$OUT/$name.netem-teardown.log"
            case "$_out" in
              *__TCOK__*) case "$_out" in
                *netem*|*"qdisc prio 1:"*) ;;
                *) _clean=1; break ;;
              esac ;;
            esac
          else
            printf 'query failed: %s\n' "$_out" >>"$OUT/$name.netem-teardown.log"
          fi
          sleep 1
        done
      fi
      if [ "$_clean" = 1 ]; then
        rsh1 "rm -f '$NETEM_OWNER.$shaped_iface'" >/dev/null 2>&1 || true
      else
        echo "WARN $name ($host): could not prove shaping removed -- leaving watchdog armed"
        shaping_failed=1
      fi
      shaped_netem=""; shaped_token=""
    fi
    flock -u 9 2>/dev/null || true
  }

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
        echo "SKIP $name ($host): cannot determine the interface back to $myaddr"; flock -u 9; return 1
      fi
      # Record BEFORE touching the interface, and arm a remote expiry. If this worker is
      # killed -- interrupt, timeout, the sweep stopped -- neither shaping_down call is
      # reached, and without a record the top-level cleanup has no idea the host was ever
      # shaped. Sweeps get interrupted constantly; a host left permanently latent is the
      # worst outcome here, because everything afterwards just looks inexplicably slow.
      #
      # OWNERSHIP TOKEN. The expiry below, and the exit cleanup, must only ever remove
      # THIS run's shaping. An unscoped timer is worse than no timer: it fires 1800s
      # later regardless, so it would delete whatever qdisc happens to be on the
      # interface by then -- a LATER run's shaping -- and since netem is verified only at
      # setup, that run would sail on unshaped and report a pass. A run of its own that
      # outlived the timer would lose its shaping the same way.
      #
      # So the owner file is the authority: the watchdog and the cleanup both re-read it
      # and act only if it still names them. Teardown removes the file, which makes any
      # surviving watchdog a no-op without having to hunt the process down.
      local token="$$-$name-$RANDOM"
      shaped_token="$token"
      printf '%s\t%s\t%s\n' "$host" "$iface" "$token" >>"$SHAPEDFILE"
      # CONFIRM THE CLAIM BEFORE SHAPING. If this write silently fails, shaping still
      # goes on -- and now NEITHER recovery path works: the watchdog finds no matching
      # token and declines, and exit cleanup sees a mismatch and skips the interface. The
      # ownership file is what makes recovery possible, so a host must never end up shaped
      # without one. Read it back rather than trusting the exit status.
      if [ "$(rsh1 "echo '$token' > '$NETEM_OWNER.$iface' 2>/dev/null; cat '$NETEM_OWNER.$iface' 2>/dev/null" 2>/dev/null)" != "$token" ]; then
        echo "SKIP $name ($host): could not claim $iface for shaping"; flock -u 9; return 1
      fi
      rsh1 "nohup sh -c 'sleep ${NETEM_EXPIRY}
            [ \"\$(cat \"$NETEM_OWNER.$iface\" 2>/dev/null)\" = \"$token\" ] || exit 0
            sudo /usr/sbin/tc qdisc del dev $iface root 2>/dev/null
            rm -f \"$NETEM_OWNER.$iface\"' >/dev/null 2>&1 </dev/null &" >/dev/null 2>&1
      shaped_netem="$host"; shaped_iface="$iface"
      # Fail on ANY setup command, including the filter; archive proof of both the
      # qdisc and the exact source-port rule instead of merely finding "netem".
      if ! rsh1 "set -e
            sudo -n /usr/sbin/tc qdisc del dev $iface root 2>/dev/null || true
            sudo -n /usr/sbin/tc qdisc add dev $iface root handle 1: prio bands 3 \
                 priomap 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
            _a='delay ${rtt:-0}ms'
            if [ '${jit:-0}' != '0' ]; then _a=\"\$_a ${jit}ms distribution normal\"; fi
            if [ '${loss:-0}' != '0' ]; then _a=\"\$_a loss ${loss}%\"; fi
            sudo -n /usr/sbin/tc qdisc add dev $iface parent 1:3 handle 30: netem \$_a
            sudo -n /usr/sbin/tc filter add dev $iface protocol ip parent 1:0 prio 3 \
                 u32 match ip sport $port 0xffff flowid 1:3
            echo __SETUP_OK__" >"$OUT/$name.netem-setup.log" 2>&1 ||
         ! grep -q '^__SETUP_OK__$' "$OUT/$name.netem-setup.log" ||
         ! rsh1 "LC_ALL=C /usr/sbin/tc qdisc show dev '$iface'" >"$OUT/$name.netem-qdisc.log" 2>&1 ||
         ! rsh1 "LC_ALL=C /usr/sbin/tc filter show dev '$iface' parent 1:0" >"$OUT/$name.netem-filter.log" 2>&1 ||
         ! netem_proof_valid "$OUT/$name.netem-qdisc.log" "$OUT/$name.netem-filter.log" "$port"; then
        shaping_down
        echo "FAIL $name ($host): could not prove netem and exact port $port filter on $iface"; return 1
      fi
    elif [ "${loss:-0}" != "0" ]; then
      echo "SKIP $name ($host): TAK_LOSS needs netem, and sudo tc is not available there"
      flock -u 9; return 1
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
  # TAK_GODS MUST REACH THE REFEREE TOO -- the same forwarding tools/desync-hunt.sh
  # already does, which this script never got. Gods used to be a room option carried
  # on the wire, so the referee learned them from the lobby; they are now decided
  # inside setupMatch, which the referee and the client run INDEPENDENTLY. Setting the
  # env on the client alone enables gods in one sim and not the other, and the run
  # desyncs at tick 0 -- a divergence manufactured by the harness and then reported as
  # a finding. That is exactly what the first post-redeploy sweep produced: five hits,
  # every one of them a TAK_GODS run, every clean run without it.
  #
  # Every other option in the table still travels as a room setting; this is the one
  # that does not, so it is forwarded explicitly rather than by passing $envs through
  # (which would also ship client-only display vars to a headless referee).
  local srv_env=""
  case "$envs" in *TAK_GODS=1*) srv_env="TAK_GODS=1";; esac
  local spid
  spid=$(rsh1 "nohup env $srv_env $RBIN --port $port --data $RDATA --map-cache-dir $RCACHE --replaydir $replay_dir --no-auth --allow-plaintext --allow-benchmarks \
         --seed $seed >/tmp/tak-srv-$port.log 2>&1 </dev/null & echo \$!" 2>/dev/null | tr -d '\r')
  [ -n "$spid" ] && note_server "$host" "$spid"
  local up=0
  for _ in $(seq 60); do
    rsh1 "grep -q listening /tmp/tak-srv-$port.log 2>/dev/null" && { up=1; break; }
    sleep 2
  done

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
  local wall_secs=${TAK_WALL_TIMEOUT:-$((secs + 300))}
  # shellcheck disable=SC2086
  env XDG_DATA_HOME="$OUT/preferences/$name/host" XDG_CONFIG_HOME="$OUT/config/$name/host" \
      TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy $seatenv $SPEED_DEFAULT $envs \
      timeout -k 30 "$wall_secs" $CLIENT game "$map" --data "$LDATA" \
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
      env XDG_DATA_HOME="$OUT/preferences/$name/client$j" XDG_CONFIG_HOME="$OUT/config/$name/client$j" \
          TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy $SPEED_DEFAULT $envs \
          timeout -k 30 "$wall_secs" $CLIENT game "$map" --data "$LDATA" \
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
  [ "$shaping_failed" = 0 ] || hit="${hit}netem-teardown-unproved "
  grep -qi "DESYNCED"        "$OUT/$name.server.log" 2>/dev/null && hit="${hit}DESYNC "
  grep -qi "REFEREE SUSPECT" "$OUT/$name.server.log" 2>/dev/null && hit="${hit}REFEREE-SUSPECT "
  grep -Eqi "DESYNCED|err=[^ ]*desync|desync at" "$clog" 2>/dev/null && hit="${hit}client-desync "
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
  [ -z "$hit" ]
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
  H_JOBS+=("${JOBS:-${_rest%%:*}}")
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
        _name=${RUNS[$j]%%|*}
        printf -- "     %-18s %-6s %s human(s) seed=%s\n" "$_name" "$_w" "$_hn" "${RUN_SEEDS[$_name]}"
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
  vpid=$("${VSSH[@]}" "$RUSER@$vhost" "nohup $RBIN --port $VALIDATE_PORT --data $RDATA --map-cache-dir $RCACHE --no-auth --allow-plaintext --allow-benchmarks --seed 999 >/tmp/tak-val-$VALIDATE_PORT.log 2>&1 </dev/null & echo \$!" 2>/dev/null | tr -d '\r')
  [ -n "$vpid" ] && note_server "$vhost" "$vpid"
  for _ in $(seq 60); do "${VSSH[@]}" "$RUSER@$vhost" "grep -q listening /tmp/tak-val-$VALIDATE_PORT.log 2>/dev/null" && break; sleep 2; done
  env TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy TAK_MP_AIS=7 TAK_SPEED=40 TAK_FAKE_DESYNC=900 \
      timeout -k 30 400 $CLIENT game "Ulasem Arena" --data "$LDATA" \
      --server "$vhost" --serverport "$VALIDATE_PORT" --mphost --time 120 >"$OUT/validate.client.log" 2>&1
  "${VSSH[@]}" "$RUSER@$vhost" "cat /tmp/tak-val-$VALIDATE_PORT.log" >"$OUT/validate.server.log" 2>/dev/null
  [ -n "$vpid" ] && "${VSSH[@]}" "$RUSER@$vhost" "kill $vpid 2>/dev/null; true" >/dev/null 2>&1
  if grep -qi "DESYNCED" "$OUT/validate.server.log"; then
    echo "   PASS -- referee reported: $(grep -i DESYNCED "$OUT/validate.server.log" | head -1)"
  else
    echo "   FAIL -- planted desync NOT detected. The sweep cannot prove anything; stopping." >&2
    exit 1
  fi

  # Game mods transfer automatically. Test the load-hash gate after that
  # transfer, rather than moving files in anybody's installed mods.
  echo "== validating game mod transfer and loaded-hash rejection on $vhost =="
  _negdata="$OUT/mod-gate"
  mkdir -p "$_negdata/host" "$_negdata/peer"
  for _side in host peer; do
    for _e in "$LDATA"/*; do
      case "$(basename "$_e" | tr '[:upper:]' '[:lower:]')" in overrides|mods|modcache|overridecache|mapcache) continue;; esac
      ln -s "$(realpath "$_e")" "$_negdata/$_side/$(basename "$_e")"
    done
  done
  npid=$("${VSSH[@]}" "$RUSER@$vhost" "nohup $RBIN --port $NEGATIVE_PORT --data $RDATA --map-cache-dir $RCACHE --no-auth --allow-plaintext --seed 999 >/tmp/tak-neg-$NEGATIVE_PORT.log 2>&1 </dev/null & echo \$!" 2>/dev/null | tr -d '\r')
  [ -n "$npid" ] && note_server "$vhost" "$npid"
  for _ in $(seq 60); do "${VSSH[@]}" "$RUSER@$vhost" "grep -q listening /tmp/tak-neg-$NEGATIVE_PORT.log 2>/dev/null" && break; sleep 2; done
  _gate=0
  timeout -k 20 150 "$(dirname "$CLIENT")/mod_transfer_test" "$NEGATIVE_PORT" "$_negdata/host" "$_negdata/peer" "$vhost" --bad-loaded >"$OUT/negative.client.log" 2>&1 && _gate=1
  "${VSSH[@]}" "$RUSER@$vhost" "cat /tmp/tak-neg-$NEGATIVE_PORT.log" >"$OUT/negative.server.log" 2>/dev/null
  [ -n "$npid" ] && "${VSSH[@]}" "$RUSER@$vhost" "kill $npid 2>/dev/null; true" >/dev/null 2>&1
  if [ "$_gate" != 1 ] || ! grep -q 'rejected at load' "$OUT/negative.server.log"; then
    echo "   FAIL -- game mod load-hash gate failed; see $OUT/negative.*.log" >&2
    exit 1
  fi
  echo "   PASS -- verified game mods transferred and incorrect loaded gameplay rejected"

  if [ "$VALONLY" = "1" ]; then echo "all gates passed (build id, desync detector, game mod mismatch)"; exit 0; fi
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
      (
        run_one "${H_NAME[$h]}" "${myidx[$k]}" "$spec"
        printf '%s\n' "$?" >"$OUT/${spec%%|*}.status"
      ) &
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
# Count the runs that produced a usable verdict BEFORE claiming anything, so the
# headline can say what it is actually based on. "no desyncs" over 32 of 37 runs is
# a different statement from "no desyncs" over 37, and the reader cannot tell them
# apart unless this line does it for them.
# SKIP THE PLANTED RUNS. validate.* and negative.* are SUPPOSED to fail -- one
# plants a desync and requires the referee to catch it, the other plants an override
# mismatch and requires rejection at load. The hits grep above already excludes them
# for exactly this reason; counting them here reported the harness's own passing
# self-tests as coverage gaps and then declared "this is NOT a clean sweep" over a
# sweep in which every real seat finished.
#
# The count is SEATS, not runs: a 4-human run writes client.log plus client2..4, and
# every one of them has to finish for the run to mean anything. Say "seat" so the
# number lines up with something the reader can count, rather than looking like a
# run total that disagrees with the table.
_bad_cases=0
for _spec in "${RUNS[@]}"; do
  _name=${_spec%%|*}
  if [ "$(cat "$OUT/$_name.status" 2>/dev/null)" != 0 ]; then
    echo "incomplete or failed case: $_name"
    _bad_cases=$((_bad_cases + 1))
  fi
done
_ok=0; _bad=0
for f in "$OUT"/*.client*.log; do
  case "$(basename "$f")" in validate.*|negative.*) continue ;; esac
  case "$(grep -h "mp-headless done" "$f" 2>/dev/null | tail -1)" in
    *err=none*) _ok=$((_ok + 1)) ;;
    *)          _bad=$((_bad + 1)) ;;
  esac
done
if [ -n "$hits" ]; then
  echo "DESYNCS FOUND in:"; echo "$hits"
elif [ "$_bad" -gt 0 ] || [ "$_bad_cases" -gt 0 ]; then
  echo "no desyncs in the $_ok seat(s) that completed -- but $_bad seat(s) and $_bad_cases case(s) lack a clean result, see below."
  echo "This is NOT a clean sweep: a run that errored proves nothing either way."
else
  echo "no desyncs reported ($_ok seats, all completed)"
fi
echo "note: runs marked 'flow' seated no human, so no hashes were compared in them --"
echo "      they cover flow control only and prove nothing about determinism."
echo "note: TAK_AUTOPLAY runs issue live commands, so the server buckets them by the"
echo "      tick they ARRIVE on and the same seed gives a different hash each run."
echo "      They prove consensus, not reproducibility -- do not diff their hashes."
# A RUN THAT ERRORED IS NOT A RUN THAT PASSED.
#
# This used to test for "mp-headless done" alone, and that string is printed on the
# way out of a FAILED run too: five stress runs died three minutes into a 37-run
# sweep with
#     mp-headless done: tick=0 ... units=0 err=server timeout
# and were counted as complete, because the line was there. They also contain no
# "DESYNCED", so the hits grep above skipped them as well. They fell through both
# gates and the sweep printed "no desyncs reported" over a third of the heavy tier
# never having simulated a single tick -- the worst outcome available to a harness,
# a clean bill of health for work it did not do.
#
# So completion is now "done AND err=none", and anything else is named with its
# error rather than merely omitted. tick=0 is called out separately: a run that
# never started is a coverage hole, not a weak pass, and it should not take reading
# the tick counts to notice.
echo "runs that did not complete:"
for f in "$OUT"/*.client*.log; do
  case "$(basename "$f")" in validate.*|negative.*) continue ;; esac
  d=$(grep -h "mp-headless done" "$f" 2>/dev/null | tail -1)
  case "$d" in
    "")            echo "  $(basename "$f") -- NEVER RAN (no result line)" ;;
    *err=none*)    : ;;
    *)             echo "  $(basename "$f") -- ${d#*err=}" ;;
  esac
done
echo "logs: $OUT"

[ -z "$hits" ] && [ "$_bad" = 0 ] && [ "$_bad_cases" = 0 ]
