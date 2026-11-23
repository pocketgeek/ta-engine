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
#        (BUILD=<dir> picks the build dir, default build-dbg)
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

# BUILD names the build dir (default build-dbg; e.g. BUILD=build-o2 for the optimized Debug build).
BUILD="${BUILD:-build-dbg}"
CLIENT=./$BUILD/takclient
SERVER=./$BUILD/takserver
[ -x "$CLIENT" ] && [ -x "$SERVER" ] || { echo "$BUILD binaries missing -- cmake --build $BUILD" >&2; exit 2; }

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

# MONARCH EXPENDABLE BY DEFAULT, to buy back some of the sweep's lost game time.
# With it off (the wire default) losing your Monarch loses the game, and 14 of 30 runs
# concluded before the clock at a mean of 8,760 ticks against the 36,000 they were given.
# The time lost is the valuable kind: a game that ends early never reaches the late-game
# state where armies are large and the map is built out, which is where the nav stalls
# lived and where a desync is likeliest to hide.
#
# WHAT IT ACTUALLY BUYS, measured by running the whole sweep both ways (29 comparable
# cases, same seeds): 662,663 -> 708,297 ticks, +6.9%. Ten runs got longer, fifteen were
# unchanged, and FOUR got SHORTER. Most of the gain is two cases that stopped finishing
# early at all -- cramped-stress 6,372 -> 36,000 and overrides-cosmetic 14,344 -> 36,000
# -- which is the late-game coverage worth having.
#
# Two earlier guesses about this are recorded because both were wrong. A first pass put
# it at 1.5x, having assumed every early finish was a Monarch death; most are
# eliminations, so it was out by about seven times. The second guess was that a run could
# never come out SHORTER, on the reasoning that eliminating a team only when everything it
# owns is dead is a strict superset of sudden death. That is true of the ending condition
# and false of the game: units that would have died with their Monarch keep fighting, so
# every later decision and RNG draw differs and the run that results is a different game
# -- four of them ended sooner. The claim to make is "more coverage on average", not
# "never shorter".
#
# Two runs below deliberately set it back to 0: the monarch-death win condition is sim
# logic in its own right (updateOutcome reading hadMonarch_), and a spectator whose game
# ended once wedged the referee, so "the game concludes" must stay covered.
MONARCH_DEFAULT="TAK_MONARCH_EXPENDABLE=1"

RUNS=(
  "baseline|Ulasem Arena||"
  "crusades|Ulasem Arena||--crusades"
  "gods|Ulasem Arena|TAK_GODS=1|"
  "stress|Ulasem Arena|TAK_STRESS=1|"
  "unitcap-250|Ulasem Arena|TAK_UNITCAP=250|"
  "unitcap-2000|Ulasem Arena|TAK_UNITCAP=2000|"
  "fog-explored|Ulasem Arena|TAK_FOG=1|"
  "fog-full|Ulasem Arena|TAK_FOG=2|"
  "ai-absurd|Ulasem Arena|TAK_AI_LEVEL=4|"
  "ai-passive|Ulasem Arena|TAK_AI_LEVEL=0|"
  "ai-absurdly-defensive|Ulasem Arena|TAK_AI_LEVEL=5|"
  "speed-4x|Ulasem Arena|TAK_SPEED=40|"
  "flow-bench-high|Ulasem Arena|TAK_BENCH=3|"
  "flow-bench-absurd|Ulasem Arena|TAK_BENCH=6|"
  "cramped|Inner Circle||"
  "cramped-sudden-death|Inner Circle|TAK_MONARCH_EXPENDABLE=0|"
  "cramped-stress|Inner Circle|TAK_STRESS=1|"
  "naval|Aibel's Seaport||"
  "naval-crusades|Aibel's Seaport||--crusades"
  "lake|Lake Lokken|TAK_STRESS=1|"
  "mods-all|Ulasem Arena||--mods all"
  "crusades-absurd|Tarosian Plain|TAK_AI_LEVEL=4|--crusades"
  "everything|Tarosian Plain|TAK_GODS=1 TAK_STRESS=1 TAK_AI_LEVEL=4 TAK_FOG=1|--crusades"
  "gods-random-starts|Ulasem Arena|TAK_GODS=1 TAK_RANDOM_STARTS=1|"
  "gods-cramped|Inner Circle|TAK_GODS=1|"
  "gods-crusades|Rift of Grief|TAK_GODS=1|--crusades"
  "monarch-sudden-death|Ulasem Arena|TAK_MONARCH_EXPENDABLE=0 TAK_GODS=1|"
  "forfeit-selfdestruct|Ulasem Arena|TAK_FORFEIT_SELFDESTRUCT=1|"
  "random-starts|Sand River Plain|TAK_RANDOM_STARTS=1|"
  "speed-1x-control|Ulasem Arena|TAK_SPEED=10|"
  "mods-cosmetic|Ulasem Arena||--mods cosmetic"
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
  # TAK_GODS MUST REACH THE SERVER TOO. Gods used to be a room option carried on the
  # wire, so the referee learned them from the lobby; they are now decided inside
  # setupMatch, which BOTH the referee and the client run independently. Setting the
  # env on the client alone would enable gods in one sim and not the other -- a
  # guaranteed desync manufactured by the harness, which would then be reported as a
  # finding. Every other option here still travels as a room setting; this is the one
  # that does not, so it is forwarded explicitly.
  # The server keeps its map cache under XDG_DATA_HOME, so it gets a per-run
  # folder too, never the real per-user one.
  local srv_env=""
  case "$envs" in *TAK_GODS=1*) srv_env="TAK_GODS=1";; esac
  # RETRY THE BIND. A port left in TIME_WAIT by an earlier sweep, or any unrelated
  # listener, used to cost the whole run: the server exited with "bind failed", no
  # client log was ever written, and the case silently vanished from a sweep whose
  # entire job is coverage. Walk a few ports before giving up.
  local spid="" tries=0
  while [ $tries -lt 5 ]; do
    # shellcheck disable=SC2086
    env $srv_env XDG_DATA_HOME="$OUT/server/$name" \
        $SERVER --port "$port" --data "$DATA" --no-auth --local --seed "$seed" >"$slog" 2>&1 &
    spid=$!
    for _ in $(seq 120); do
      grep -q listening "$slog" 2>/dev/null && break
      grep -q "bind failed" "$slog" 2>/dev/null && break
      sleep 1
    done
    grep -q listening "$slog" 2>/dev/null && break
    kill "$spid" 2>/dev/null; wait "$spid" 2>/dev/null; spid=""
    port=$((port + 100)); tries=$((tries + 1))
  done
  if [ -z "$spid" ] || ! grep -q listening "$slog" 2>/dev/null; then
    echo "FAIL $name: server never came up" ; [ -n "$spid" ] && kill "$spid" 2>/dev/null
    return 1
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
  # TAK_BENCH FORCES SPECTATOR MODE, whatever TAK_MP_AIS says:
  #     bool watch = (autoMode == 1 && devEnv("TAK_MP_WATCH")) || benchmarkMode_;
  # so a benchmark run seats no human, the referee's checkHashes returns on `live == 0`
  # and the client sends a zero hash anyway. These cases CANNOT detect a desync. They
  # are kept because they are still worth running -- thousands of units exercise the
  # all-AI flow-control path in canAdvance, which is where a server-side wedge lived --
  # but they are labelled so their "ok" is never mistaken for a verified simulation.
  local flowonly=0
  case "$envs" in *TAK_BENCH*) flowonly=1;; esac

  local secs=$((MINUTES * 60))
  # A per-run profile: the client saves its --data folder as the data folder setting
  # and writes a replay, which must never land in the player's own settings.ini.
  # shellcheck disable=SC2086
  env XDG_DATA_HOME="$OUT/preferences/$name" XDG_CONFIG_HOME="$OUT/config/$name" \
      TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy TAK_MP_AIS=7 $SPEED_DEFAULT $MONARCH_DEFAULT $envs \
      timeout -k 30 $((secs + 300)) $CLIENT game "$map" --data "$DATA" \
      --server 127.0.0.1 --serverport "$port" --mphost --time "$secs" $flags \
      >"$clog" 2>&1
  local rc=$?
  sleep 2; kill "$spid" 2>/dev/null; sleep 1; kill -9 "$spid" 2>/dev/null; wait "$spid" 2>/dev/null

  # The verdict. Anything other than a clean finish is worth a human look.
  local hit=""
  grep -qi "DESYNCED" "$slog" && hit="${hit}server-reported-desync "
  grep -qi "REFEREE SUSPECT" "$slog" && hit="${hit}referee-suspect "
  # The messages themselves, never a bare "desync": log paths carry the desync-hunt-<pid> directory name.
  grep -Eqi "desync detected|DESYNCED|desync at" "$clog" && hit="${hit}client-reported-desync "
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
  elif [ "$flowonly" = "1" ]; then
    # Not "ok": nothing was compared. Say so on the line itself.
    echo "flow $name [seed=$seed] -- NO HASH COMPARISON (benchmark forces spectator) -- $done_line"
  else
    echo "ok   $name [seed=$seed] -- $done_line"
  fi
}
export -f run_one
export OUT CLIENT SERVER DATA MINUTES PORT_BASE SPEED_DEFAULT MONARCH_DEFAULT

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
echo "note: benchmark cases (TAK_BENCH) run as spectators and compare NO hashes --"
echo "      they cover flow control only; their result is not a determinism result."
# CHECK EVERY EXPECTED RUN, not every log that happens to exist. This used to walk
# "$OUT"/*.client.log, so a run whose SERVER never came up -- which never writes a
# client log at all -- was invisible here: the sweep printed "no desyncs in any run"
# and an empty failure list while one case had not executed. A determinism sweep that
# quietly drops a case reports absence of evidence as evidence of absence.
echo "runs that did not finish cleanly:"
for spec in "${RUNS[@]}"; do
  rname="${spec%%|*}"
  rlog="$OUT/$rname.client.log"
  if [ ! -f "$rlog" ]; then
    echo "  $rname -- NEVER RAN (no client log; see $rname.server.log)"
  elif ! grep -q "err=none" "$rlog" 2>/dev/null; then
    echo "  $rname -- did not end cleanly"
  fi
done
echo "logs: $OUT"
