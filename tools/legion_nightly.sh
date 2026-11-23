#!/usr/bin/env bash
# legion_nightly.sh -- the local nightly Legion job (LEGION-PLAN decision 9, 3.0, 3.8).
#
#   legion_nightly.sh            run once (what the systemd timer does)
#   legion_nightly.sh --install  copy this script + tools/nightly/*.{service,timer} into place and
#                                enable the user timer (no linger, no system units)
#   legion_nightly.sh --uninstall
#
# It keeps its own detached worktree (never the main checkout's files), builds a
# Release crowdbench and an optimized Debug (-O2) legion_scenario/legion_world_test
# there, and runs, niced and pinned to a CPU list:
#   1. the crowdbench screen (tools/crowdbench_screen.py) against the committed
#      screen baseline, and against the frozen screen anchor when one is committed;
#   2. every tools/scenarios/*.scn (data-gated ones with --data) in both modes, the
#      baseline gate on those lines (tools/legion_check.py check -- the logic behind
#      `legion_scenario --check`), then the anchor-b8a4110 cumulative drift report;
#   3. ctest -L nightly (the cases stage 2 did not already run).
# Output: $OUT_ROOT/<UTC stamp>/ with summary.txt (PASS/FAIL, RATCHET lines, drift),
# summary.json and the deterministic files (screen.jsonl, scenarios.jsonl, check.txt,
# anchor.txt); digest.txt holds their sha256 and is equal between two runs of one
# commit. The newest $KEEP report directories are kept. A lock file keeps two runs
# from overlapping; only this run's own recorded PIDs are ever signalled.
#
# Environment (all optional):
#   TAK_NIGHTLY_REPO      repository the worktree belongs to          (default /home/pocket_geek/TAK)
#   TAK_NIGHTLY_WORKTREE  default $REPO/.claude/worktrees/legion-nightly
#   TAK_NIGHTLY_REF       default origin/main (fetched first; set a local ref to skip the fetch)
#   TAK_NIGHTLY_CORES     taskset list, default 16-23
#   TAK_NIGHTLY_DATA      retail install for the data-gated scenarios, default $REPO/assets/game
#   TAK_NIGHTLY_OUT       default ~/.local/share/tak-legion-nightly
#   TAK_NIGHTLY_KEEP      reports kept, default 30
#   TAK_NIGHTLY_BASELINE  baseline.json to gate on (default: the worktree's tools/scenarios/baseline.json)
#   TAK_NIGHTLY_SCREEN_BASELINE / TAK_NIGHTLY_SCREEN_ANCHOR / TAK_NIGHTLY_ANCHOR
#                         screen baseline JSONL, frozen screen anchor JSONL, frozen anchor-b8a4110.json
#   TAK_NIGHTLY_SCN_GLOB  shell glob of scenario names to run (default '*'; for trial runs)
#   TAK_NIGHTLY_SCREEN_ARGS  extra crowdbench_screen.py arguments, e.g. "--scenarios open --seeds 0"
#   TAK_NIGHTLY_OFFSETS   --offsets list for legion_scenario (default `gate`: the gate's
#                         eleven 0,+-1..+-5, or a file's own `gateoffsets`; legion_check
#                         reads the small-count keys over them and the rest over 0,+-1,+-2)
#   TAK_NIGHTLY_STAGE_TIMEOUT  seconds per stage, default 3600 (builds get twice)
set -u
umask 022

SELF=$(readlink -f "${BASH_SOURCE[0]}")
REPO=${TAK_NIGHTLY_REPO:-/home/pocket_geek/TAK}
WT=${TAK_NIGHTLY_WORKTREE:-$REPO/.claude/worktrees/legion-nightly}
REF=${TAK_NIGHTLY_REF:-origin/main}
CORES=${TAK_NIGHTLY_CORES:-16-23}
DATA=${TAK_NIGHTLY_DATA:-$REPO/assets/game}
OUT_ROOT=${TAK_NIGHTLY_OUT:-$HOME/.local/share/tak-legion-nightly}
KEEP=${TAK_NIGHTLY_KEEP:-30}
STAGE_TIMEOUT=${TAK_NIGHTLY_STAGE_TIMEOUT:-3600}
SCN_GLOB=${TAK_NIGHTLY_SCN_GLOB:-*}
SCREEN_EXTRA=${TAK_NIGHTLY_SCREEN_ARGS:-}
OFFSETS=${TAK_NIGHTLY_OFFSETS-gate}

UNIT_DIR=$HOME/.config/systemd/user
BIN_DIR=$OUT_ROOT/bin

# ---------------------------------------------------------------- install
if [ "${1:-}" = "--install" ]; then
    src=$(dirname "$SELF")
    [ -f "$src/nightly/tak-legion-nightly.service" ] || { echo "no unit files next to $SELF" >&2; exit 2; }
    mkdir -p "$BIN_DIR" "$UNIT_DIR"
    # The unit runs a private copy, so the job never executes a file it is about to check out over.
    install -m 755 "$SELF" "$BIN_DIR/legion_nightly.sh"
    install -m 644 "$src/nightly/tak-legion-nightly.service" "$src/nightly/tak-legion-nightly.timer" "$UNIT_DIR/"
    systemctl --user daemon-reload && systemctl --user enable --now tak-legion-nightly.timer || exit 2
    systemctl --user list-timers tak-legion-nightly.timer --no-pager
    exit 0
fi
if [ "${1:-}" = "--uninstall" ]; then
    systemctl --user disable --now tak-legion-nightly.timer
    rm -f "$UNIT_DIR/tak-legion-nightly.service" "$UNIT_DIR/tak-legion-nightly.timer"
    systemctl --user daemon-reload
    exit 0
fi
[ $# -eq 0 ] || { echo "usage: legion_nightly.sh [--install|--uninstall]" >&2; exit 2; }

mkdir -p "$OUT_ROOT" || exit 2
exec 9>"$OUT_ROOT/lock"
if ! flock -n 9; then
    echo "legion_nightly: another run holds $OUT_ROOT/lock; not starting" >&2
    exit 75
fi

STAMP=$(date -u +%Y-%m-%dT%H%M%SZ)
OUT=$OUT_ROOT/$STAMP
mkdir -p "$OUT/scn" "$OUT/log" || exit 2
: > "$OUT/pids"
exec > >(tee -a "$OUT/log/nightly.log") 2>&1

NICE=(nice -n 10 taskset -c "$CORES")
JOBS=$(python3 -c '
import sys
n = 0
for part in sys.argv[1].split(","):
    a, _, b = part.partition("-")
    n += (int(b) - int(a) + 1) if b else 1
print(n)' "$CORES")
T0=$SECONDS
declare -A STAGE_STATUS STAGE_SECS
STAGES=()

# ------------------------------------------------------- own-PID bookkeeping
# Every stage runs as `setsid timeout ...`; the PID of that wrapper is recorded
# (it leads a process group of its own and `timeout` signals the group on expiry).
# On exit or a signal only recorded PIDs still alive are signalled.
kill_own() {
    local pid
    while read -r pid; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then kill -TERM -- "-$pid" 2>/dev/null; fi
    done < "$OUT/pids"
}
trap 'kill_own; exit 143' INT TERM
trap 'kill_own' EXIT

# run_stage NAME TIMEOUT CMD... : stdout+stderr to log/NAME.log, exit code in STAGE_STATUS[NAME].
run_stage() {
    local name=$1 limit=$2; shift 2
    local t=$SECONDS rc
    STAGES+=("$name")
    echo "== stage $name"
    setsid timeout -k 30 "$limit" "$@" > "$OUT/log/$name.log" 2>&1 &
    local pid=$!
    echo "$pid" >> "$OUT/pids"
    wait "$pid"; rc=$?
    STAGE_STATUS[$name]=$rc
    STAGE_SECS[$name]=$((SECONDS - t))
    echo "   $name exit $rc in ${STAGE_SECS[$name]}s"
    return 0
}

# ---------------------------------------------------------------- worktree
prepare_worktree() {
    set -e
    local sha
    case "$REF" in origin/*) git -C "$REPO" fetch -q origin || echo "fetch failed; using the existing $REF";; esac
    sha=$(git -C "$REPO" rev-parse --verify "$REF^{commit}")
    if [ ! -e "$WT/.git" ]; then
        mkdir -p "$(dirname "$WT")"
        git -C "$REPO" worktree add --detach "$WT" "$sha"
    else
        git -C "$WT" checkout -q --detach "$sha"
        git -C "$WT" reset -q --hard "$sha"
        git -C "$WT" clean -fdq -e 'build*'
    fi
    echo "$sha" > "$OUT/commit"
    if [ -d "$REPO/assets" ] && [ ! -e "$WT/assets" ]; then ln -s "$REPO/assets" "$WT/assets"; fi
}
run_stage worktree 600 bash -c "$(declare -f prepare_worktree); REPO='$REPO' WT='$WT' REF='$REF' OUT='$OUT'; prepare_worktree"
if [ "${STAGE_STATUS[worktree]}" != 0 ]; then
    cat "$OUT/log/worktree.log"
    printf 'FAIL\nworktree stage failed (see %s/log/worktree.log)\n' "$OUT" | tee "$OUT/summary.txt"
    exit 1
fi
COMMIT=$(cat "$OUT/commit")

# ------------------------------------------------------------------- builds
build() {   # DIR BUILD_TYPE CXX_FLAGS TARGETS...
    local dir=$1 type=$2 flags=$3; shift 3
    cmake -S "$WT" -B "$dir" -G Ninja \
        -DTAK_STATIC_DEPS_PREFIX=/home/pocket_geek/TAK/third_party/static-deps \
        -DTAK_FFMPEG_PREFIX=/home/pocket_geek/TAK/third_party/ffmpeg-bink \
        -DTAK_TEST_DATA="$DATA" -DCMAKE_BUILD_TYPE="$type" -DCMAKE_CXX_FLAGS="$flags" &&
        cmake --build "$dir" -j"$JOBS" --target "$@"
}
BODY="$(declare -f build); WT='$WT'; DATA='$DATA'; JOBS=$JOBS"
run_stage build-release $((STAGE_TIMEOUT * 2)) "${NICE[@]}" bash -c "$BODY; build '$WT/build-nightly-rel' Release '' crowdbench"
run_stage build-o2 $((STAGE_TIMEOUT * 2)) "${NICE[@]}" bash -c "$BODY; build '$WT/build-nightly-o2' Debug -O2 legion_scenario legion_world_test"

REL=$WT/build-nightly-rel
O2=$WT/build-nightly-o2
SCN_DIR=$WT/tools/scenarios
BASELINE=${TAK_NIGHTLY_BASELINE:-$SCN_DIR/baseline.json}
SCREEN_BASE=${TAK_NIGHTLY_SCREEN_BASELINE:-$SCN_DIR/crowdbench_screen_baseline.jsonl}
SCREEN_ANCHOR=${TAK_NIGHTLY_SCREEN_ANCHOR:-$SCN_DIR/crowdbench_screen_anchor.jsonl}
ANCHOR=${TAK_NIGHTLY_ANCHOR:-$SCN_DIR/anchor-b8a4110.json}

# ------------------------------------------------------------------- screen
if [ "${STAGE_STATUS[build-release]}" = 0 ]; then
    screen_args=(--binary "$REL/crowdbench" --output "$OUT/screen.jsonl" --cpus "$CORES" --quiet)
    [ -f "$SCREEN_BASE" ] && screen_args+=(--baseline "$SCREEN_BASE")
    [ -f "$SCREEN_ANCHOR" ] && screen_args+=(--anchor "$SCREEN_ANCHOR")
    run_stage screen "$STAGE_TIMEOUT" nice -n 10 python3 "$WT/tools/crowdbench_screen.py" "${screen_args[@]}" $SCREEN_EXTRA
fi

# ---------------------------------------------------------------- scenarios
# One legion_scenario per file (plus the wanderers-on probe), JOBS at a time, each
# pinned to its own core of the list. Exit codes land in scn/NAME.rc.
run_scenarios() {
    local cores=() part a b i f n idx=0 slot
    local -a pids=() parts
    IFS=, read -ra parts <<< "$CORES"
    for part in "${parts[@]}"; do
        a=${part%-*}; b=${part#*-}
        for ((i = a; i <= b; i++)); do cores+=("$i"); done
    done
    local -a extra=()
    [ -d "$DATA" ] && extra=(--data "$DATA")
    [ -n "$OFFSETS" ] && extra+=(--offsets "$OFFSETS")
    local -a list=()
    for f in "$SCN_DIR"/$SCN_GLOB.scn; do list+=("$f"); done
    case "motion-wall" in $SCN_GLOB) list+=("$SCN_DIR/motion-wall.scn@wanderers-on");; esac
    for f in "${list[@]}"; do
        slot=$((idx % ${#cores[@]}))
        if [ "$idx" -ge "${#cores[@]}" ]; then wait "${pids[$slot]}"; fi
        local flags=()
        n=$(basename "${f%@*}" .scn)
        if [ "$f" != "${f%@*}" ]; then
            [ -d "$DATA" ] || continue
            flags=(--wanderers on); n=$n-wanderers-on
        fi
        (
            nice -n 10 taskset -c "${cores[$slot]}" "$O2/legion_scenario" "${f%@*}" --mode both --json \
                "${extra[@]}" "${flags[@]}" > "$OUT/scn/$n.jsonl" 2> "$OUT/scn/$n.err"
            echo $? > "$OUT/scn/$n.rc"
        ) &
        pids[$slot]=$!
        idx=$((idx + 1))
    done
    wait
}
if [ "${STAGE_STATUS[build-o2]}" = 0 ]; then
    run_stage scenarios "$STAGE_TIMEOUT" bash -c "$(declare -f run_scenarios); \
        CORES='$CORES'; SCN_GLOB='$SCN_GLOB'; OFFSETS='$OFFSETS'; SCN_DIR='$SCN_DIR'; O2='$O2'; OUT='$OUT'; DATA='$DATA'; run_scenarios"
    # Join in file-name order so the result is a pure function of the simulation.
    # The wanderers-on variant repeats motion-wall's name (a probe, not a gated run): kept out.
    : > "$OUT/scenarios.jsonl"
    for f in $(ls "$OUT/scn"/*.jsonl | grep -v wanderers-on | LC_ALL=C sort); do cat "$f" >> "$OUT/scenarios.jsonl"; done
    : > "$OUT/scn-problems.txt"
    for f in "$OUT/scn"/*.rc; do
        n=$(basename "$f" .rc); rc=$(cat "$f")
        if [ "$rc" != 0 ] && [ "$rc" != 77 ]; then
            echo "$n: exit $rc $(head -c 300 "$OUT/scn/$n.err" | tr '\n' ' ')" >> "$OUT/scn-problems.txt"
        fi
    done
    grep -h '"skipped"' "$OUT/scenarios.jsonl" > "$OUT/scn-skipped.txt" || true
    run_stage check 600 bash -c "python3 '$WT/tools/legion_check.py' check --baseline '$BASELINE' --results '$OUT/scenarios.jsonl' > '$OUT/check.txt' 2>&1; rc=\$?; cat '$OUT/check.txt'; exit \$rc"
    if [ -f "$ANCHOR" ]; then
        run_stage anchor 600 bash -c "python3 '$WT/tools/legion_check.py' anchor --anchor '$ANCHOR' --results '$OUT/scenarios.jsonl' > '$OUT/anchor.txt' 2>&1; rc=\$?; cat '$OUT/anchor.txt'; exit \$rc"
    else
        echo "anchor-b8a4110: $ANCHOR is not committed; drift not reported" > "$OUT/anchor.txt"
    fi
fi

# -------------------------------------------------------------------- ctest
# Skip what the scenarios stage already ran (the same files with the same data); the
# probes, the cost battles (their window tables) and any other nightly case stay.
if [ "${STAGE_STATUS[build-o2]}" = 0 ]; then
    names=$(cd "$SCN_DIR" && ls *.scn | sed 's/\.scn$//' | grep -v '^battle-' | paste -sd'|')
    run_stage ctest "$STAGE_TIMEOUT" "${NICE[@]}" ctest --test-dir "$O2" -L nightly -j"$JOBS" --output-on-failure \
        -E "^(legion_motion_|legion_gen1_route_|legion_scenario_($names)\$)"
fi

# ------------------------------------------------------------------- report
(cd "$OUT" && for f in screen.jsonl scenarios.jsonl check.txt anchor.txt; do [ -f "$f" ] && sha256sum "$f"; done) > "$OUT/digest.txt"

overall=PASS
reasons=()
fail() { overall=FAIL; reasons+=("$1"); }
for s in "${STAGES[@]}"; do
    [ "$s" = anchor ] && continue
    [ "${STAGE_STATUS[$s]}" = 0 ] || fail "stage $s exit ${STAGE_STATUS[$s]}"
done
[ -s "$OUT/scn-problems.txt" ] && fail "$(wc -l < "$OUT/scn-problems.txt") scenario run(s) exited nonzero (scn-problems.txt)"

WALL=$((SECONDS - T0))
{
    echo "$overall"
    echo "commit $COMMIT ($REF)  cores $CORES  started $STAMP  wall ${WALL}s"
    for r in "${reasons[@]}"; do echo "  reason: $r"; done
    echo "stages:"
    for s in "${STAGES[@]}"; do printf '  %-14s exit %-4s %5ss\n' "$s" "${STAGE_STATUS[$s]}" "${STAGE_SECS[$s]}"; done
    echo "screen:"
    grep -E '^(screen:|==|  FAILED)' "$OUT/log/screen.log" 2>/dev/null | sed 's/^/  /' | head -40
    echo "scenario check:"
    grep -E 'FAIL|RATCHET' "$OUT/check.txt" 2>/dev/null | sed 's/^/  /' | head -80
    tail -n 2 "$OUT/check.txt" 2>/dev/null | sed 's/^/  | /'
    [ -s "$OUT/scn-skipped.txt" ] && echo "  skipped scenarios: $(wc -l < "$OUT/scn-skipped.txt")"
    [ -s "$OUT/scn-problems.txt" ] && sed 's/^/  PROBLEM /' "$OUT/scn-problems.txt"
    echo "anchor drift (report only):"
    tail -n 4 "$OUT/anchor.txt" 2>/dev/null | sed 's/^/  /'
    echo "files: $OUT"
} > "$OUT/summary.txt"

python3 - "$OUT" "$overall" "$COMMIT" "$WALL" <<'EOF'
import json, sys
out, overall, commit, wall = sys.argv[1:5]
stages = {}
for line in open(out + "/summary.txt"):
    p = line.split()
    if len(p) == 4 and p[1] == "exit" and line.startswith("  ") and p[3].endswith("s"):
        stages[p[0]] = {"exit": int(p[2]), "seconds": int(p[3][:-1])}
reasons = [l.split("reason: ", 1)[1].strip() for l in open(out + "/summary.txt") if "reason: " in l]
json.dump({"result": overall, "commit": commit, "wall_seconds": int(wall), "reasons": reasons,
           "stages": stages}, open(out + "/summary.json", "w"), indent=1, sort_keys=True)
EOF

echo "---"; cat "$OUT/summary.txt"

# keep the newest $KEEP report directories (stamps sort by time)
ls -1d "$OUT_ROOT"/20*T*Z 2>/dev/null | LC_ALL=C sort | head -n -"$KEEP" | while read -r old; do rm -rf -- "$old"; done

[ "$overall" = PASS ]
exit $?
