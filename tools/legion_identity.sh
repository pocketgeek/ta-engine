#!/bin/bash
# Legion hash-identity harness (I2): does a candidate build compute exactly the
# same simulation as a base build? For exact (hash-identical) steps such as the
# W1 perf work and instrument-only commits.
#
#   tools/legion_identity.sh --base <build-dir> --cand <build-dir>
#       [--quick] [--replays <dir>] [--data <install>] [--cores a-b]
#       [--verify] [--out <dir>] [--skip <section,...>] [--mpai-time <s>]
#       [-j <n>] [--no-cache] [--cache-dir <dir>]
#
# Speed: the base side is deterministic, so every base result is cached under
# ~/.cache/tak-identity/<base commit>/<build type>-v<harness version>/ (a chain of
# steps runs each base only once). A run that passes with every row SAME also stores
# the candidate's results under the CANDIDATE's commit (if its tree is clean and its
# build dir builds): they are what a base run of it would produce, so in a chain A->B,
# B->C the second step finds its base already cached. The
# cache is used only when the base tree is CLEAN and its build dir is up to date
# with the sources (otherwise "base cache disabled: <why>" is printed and nothing
# is read or written); only complete, successful base results are stored (a
# replay that diverged, a failed ctest, an --mpai with err, are never cached).
# Every hit is printed ("cache hit: <row>") and listed in meta.txt. --no-cache
# neither reads nor writes the cache; --cache-dir (or TAK_IDENTITY_CACHE) moves
# it. The candidate is never read from the cache, so a difference is always
# detected against real, fresh candidate output. Bump HARNESS_VERSION in legion_identity.py
# when a base row's definition changes. -j <n> sets the job width (default: the
# number of CPUs in --cores, e.g. --cores 20-23 -j 4); crowdbench starts as soon
# as every replay/golden job has been launched instead of after the longest one.
#
# Both build dirs need takclient, takserver, crowdbench, navigation_checkpoint_test
# and the test executables of the ctest selection below. Debug builds
# (e.g. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-O2) run every section; a
# Release takclient reads no TAK_* variables and has no replay/--mpai harness,
# so those rows are SKIP for it. Sections, in run order:
#
#   mpai        --mpai Legion (TAK_LEGION=1) and Retail on "Inner Circle": takserver
#               --local --no-auth --seed 1 on a free port, client TAK_HEADLESS=1
#               with a fresh empty XDG_DATA_HOME, --time 300 (60 with --quick), each
#               twice per build; must be reproducible run-to-run and equal across
#               builds. Runs in the background beside the other sections.
#   replays     L-bench, L-2h, R-bench, R-2h (--quick: L-bench, R-bench) from
#               --replays, copied to the output dir with the protocol at byte 8
#               patched to each build's kNetVersion (the recordings must be the
#               current replay format). Final hash plus the per-checkpoint
#               section hashes (TAK_HASHDETAIL, every 300 ticks).
#   golden      navigation_checkpoint_test legion|retail serial: all 24 checkpoints.
#   crowdbench  the matrix in legion_identity.py (full: 504 rows per build, both
#               modes, seeds 0/1/2, serial and --workers; --quick: 12 rows).
#   ctest       ctest -R '^(legion_|retail|sim_driver_equiv$|navigation_determinism$)'
#               on each build (with --quick also -LE nightly): pass/fail, and any
#               hash= values a test prints.
#   determinism tools/check-determinism.sh in each build's source tree.
#
# --verify sets TAK_LEGION_VERIFY=1 on every candidate run (an unknown variable
# is ignored by builds without the verify scaffold). Output: a table on stdout,
# <out>/identity.txt (every row), <out>/identity.json, and all raw logs. Exit 0
# only if every row is SAME (or SKIP) and every test passes. Wall time on 4 cores
# (Debug -O2), cold: full about 77 min (the crowdbench matrix is 67 of them);
# --quick is bounded by its ctest section. With the base cached, the base half of
# every section (about half the CPU time) is skipped.
#
# Processes: every process this script starts gets its own process group, and on
# exit or interrupt only those groups are signalled (by exact id). Nothing is
# killed by name or pattern.
set -u

usage() { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }

BASE= CAND= QUICK=0 VERIFY=0 OUT= SKIP= MPAI_TIME= JOBS= NOCACHE=0 CACHE_DIR=
REPLAYS=/home/pocket_geek/tak-tmp/we-replays
DATA=
CORES=$(taskset -cp $$ 2>/dev/null | sed 's/.*: //')
while [ $# -gt 0 ]; do
    case "$1" in
        --base) BASE=$2; shift 2;;
        --cand) CAND=$2; shift 2;;
        --quick) QUICK=1; shift;;
        --verify) VERIFY=1; shift;;
        --replays) REPLAYS=$2; shift 2;;
        --data) DATA=$2; shift 2;;
        --cores) CORES=$2; shift 2;;
        --out) OUT=$2; shift 2;;
        --skip) SKIP=$2; shift 2;;
        --mpai-time) MPAI_TIME=$2; shift 2;;
        -j) JOBS=$2; shift 2;;
        --no-cache) NOCACHE=1; shift;;
        --cache-dir) CACHE_DIR=$2; shift 2;;
        -h|--help) usage;;
        *) echo "unknown argument: $1" >&2; usage;;
    esac
done
[ -n "$BASE" ] && [ -n "$CAND" ] || usage
HERE=$(cd "$(dirname "$0")" && pwd)
PY="python3 $HERE/legion_identity.py"
BASE=$(cd "$BASE" && pwd) || exit 2
CAND=$(cd "$CAND" && pwd) || exit 2
[ -n "$OUT" ] || OUT=$(mktemp -d "${TMPDIR:-/tmp}/legion-identity.XXXXXX")
mkdir -p "$OUT/base" "$OUT/cand" "$OUT/replays" "$OUT/xdg" || exit 2
OUT=$(cd "$OUT" && pwd)
NCORES=$(python3 -c "import sys;print(sum(int(b)-int(a)+1 if '-' in p else 1 for p in sys.argv[1].split(',') if p for a,b in [p.split('-') if '-' in p else (p,p)]))" "$CORES")
[ -z "$JOBS" ] || NCORES=$JOBS
[ -n "$MPAI_TIME" ] || { [ $QUICK = 1 ] && MPAI_TIME=60 || MPAI_TIME=300; }
skipped() { case ",$SKIP," in *",$1,"*) return 0;; esac; return 1; }

cache() { sed -n "s/^$2:[A-Z]*=//p" "$1/CMakeCache.txt" | head -n 1; }
declare -A BUILD SRC DEBUG PROTO TYPE
BUILD[base]=$BASE BUILD[cand]=$CAND
for role in base cand; do
    b=${BUILD[$role]}
    [ -f "$b/CMakeCache.txt" ] || { echo "$b: not a CMake build dir" >&2; exit 2; }
    SRC[$role]=$(cache "$b" CMAKE_HOME_DIRECTORY)
    TYPE[$role]=$(cache "$b" CMAKE_BUILD_TYPE)
    upper=$(echo "${TYPE[$role]}" | tr a-z A-Z)
    case "$(cache "$b" "CMAKE_CXX_FLAGS_$upper") $(cache "$b" CMAKE_CXX_FLAGS)" in
        *NDEBUG*) DEBUG[$role]=0;; *) DEBUG[$role]=1;;
    esac
    PROTO[$role]=$(sed -n 's/^constexpr uint32_t kNetVersion = \([0-9]*\);.*/\1/p' "${SRC[$role]}/src/net/protocol.h")
    [ -n "${PROTO[$role]}" ] || { echo "${SRC[$role]}: no kNetVersion" >&2; exit 2; }
done
[ -n "$DATA" ] || DATA=$(cache "$BASE" TAK_TEST_DATA)
[ -n "$DATA" ] || DATA=${SRC[base]}/assets/game
DATA=$(cd "$DATA" && pwd) || { echo "no game data dir; pass --data" >&2; exit 2; }
if [ $QUICK = 1 ]; then REPLAY_NAMES="L-bench R-bench"; else REPLAY_NAMES="L-bench L-2h R-bench R-2h"; fi
MPAI_RUNS=2
# --quick leaves out the tests labelled nightly (each of the 150 selected tests is a scenario or
# situation run of up to minutes: they were 90% of a quick run); the full run keeps all of them.
CT_ROW=ctest CT_LABELS=()
[ $QUICK = 1 ] && { CT_ROW=ctest-quick; CT_LABELS=(-LE nightly); }
skipped mpai && MPAI_RUNS=0
skipped replays && REPLAY_NAMES=
# A release takclient cannot run the replay or --mpai harness; the report shows SKIP rows.
BOTH_DEBUG=$(( DEBUG[base] && DEBUG[cand] ))
# --- base result cache ----------------------------------------------------------
BKEY= CACHE_HITS=() CACHE_STORED=0
[ -z "$CACHE_DIR" ] || export TAK_IDENTITY_CACHE=$CACHE_DIR
if [ $NOCACHE = 0 ]; then
    BKEY=$($PY cache-key --src "${SRC[base]}" --build "$BASE" --type "${TYPE[base]}-d${DEBUG[base]}-data$(printf %s "$DATA" | sha256sum | cut -c1-6)") || BKEY=
    [ -n "$BKEY" ] && mkdir -p "$BKEY"
fi
cache_get() {  # row id -> 0 on a hit (the files are restored into $OUT/base)
    [ -n "$BKEY" ] && $PY cache-get --dir "$BKEY" --row "$1" --out "$OUT/base" || return 1
    CACHE_HITS+=("$1"); echo "legion_identity: cache hit: $1"
}
put_all() {  # key dir -- store every complete, valid result found in <dir> (a no-op for rows already cached)
    local key=$1 dir=$2 mode run name
    for mode in legion retail; do
        for run in $(seq 1 $MPAI_RUNS); do $PY cache-put --dir "$key" --row "mpai$MPAI_TIME-$mode-$run" --out "$dir" || true; done
        $PY cache-put --dir "$key" --row "golden-$mode" --out "$dir" || true
    done
    for name in $REPLAY_NAMES; do
        [ -f "$REPLAYS/$name.takrep" ] && $PY cache-put --dir "$key" --row "replay-$name-$(sha256sum "$REPLAYS/$name.takrep" | cut -c1-12)" --out "$dir" || true
    done
    $PY cache-put --dir "$key" --row "$CT_ROW" --out "$dir" || true
    $PY cache-put --dir "$key" --row determinism --out "$dir" || true
}
store_base() { [ -z "$BKEY" ] || put_all "$BKEY" "$OUT/base"; }
{
    echo "base=$BASE"; echo "cand=$CAND"
    for role in base cand; do
        echo "${role}_src=${SRC[$role]}"; echo "${role}_type=${TYPE[$role]}"
        echo "${role}_debug=${DEBUG[$role]}"; echo "${role}_protocol=${PROTO[$role]}"
        echo "${role}_rev=$(git -C "${SRC[$role]}" describe --always --dirty 2>/dev/null)"
    done
    echo "quick=$QUICK"; echo "verify=$VERIFY"; echo "cores=$CORES"; echo "data=$DATA"
    echo "replays=$(echo $REPLAY_NAMES | tr ' ' ,)"; echo "replay_dir=$REPLAYS"
    echo "mpai_runs=$MPAI_RUNS"; echo "mpai_time=$MPAI_TIME"; echo "jobs=$NCORES"
    echo "base_cache=${BKEY:-off}"
} > "$OUT/meta.txt"
: > "$OUT/timings.txt"
echo "legion_identity: base ${TYPE[base]} $BASE"
echo "legion_identity: cand ${TYPE[cand]} $CAND"
echo "legion_identity: output $OUT (cores $CORES, quick=$QUICK, verify=$VERIFY)"

# --- process bookkeeping ------------------------------------------------------
# Every command is started as `setsid <cmd>` from a background subshell, so its PID
# is also the id of a fresh process group holding it and its children (ctest and
# its tests, timeout and takclient). Cleanup signals only those groups, and only
# while they are still unreaped children of this shell (`jobs -p`), so a PID can
# never have been recycled to someone else's process.
PIDS=()
cleanup() {
    trap - EXIT INT TERM
    local live pid
    live=" $(jobs -p | tr '\n' ' ') "
    for pid in "${PIDS[@]}"; do
        case "$live" in *" $pid "*) kill -TERM -- "-$pid" "$pid" 2>/dev/null;; esac
    done
    wait 2>/dev/null
}
trap cleanup EXIT
trap 'echo "legion_identity: interrupted" >&2; exit 130' INT TERM
launch() {  # log cwd cmd... -> $LAUNCHED (the PID and process-group id)
    local log=$1 cwd=$2; shift 2
    ( cd "$cwd" && exec setsid "$@" > "$log" 2>&1 ) &
    LAUNCHED=$!
    PIDS+=($LAUNCHED)
}

role_env() {  # the per-role environment, as env(1) arguments
    printf '%s\n' -u TAK_LEGION_VERIFY
    [ "$1" = cand ] && [ $VERIFY = 1 ] && printf '%s\n' TAK_LEGION_VERIFY=1
}
T0=$(date +%s)
section_time() { echo "$1 $(( $(date +%s) - $2 ))" >> "$OUT/timings.txt"; }

# --- mpai (background) --------------------------------------------------------
free_port() { python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1])"; }
mpai_game() {  # role mode run -- one server+client pair; signals only its own two groups
    local role=$1 mode=$2 run=$3 b=${BUILD[$1]} log=$OUT/$1/mpai-$2-$3.log
    local xdg spid= cpid= port attempt legion=()
    trap '[ -n "$cpid" ] && kill -TERM -- -$cpid 2>/dev/null; [ -n "$spid" ] && kill -TERM -- -$spid 2>/dev/null; exit 1' TERM
    [ "$mode" = legion ] && legion=(TAK_LEGION=1)
    mapfile -t renv < <(role_env "$role")
    for attempt in 1 2 3; do
        xdg=$(mktemp -d "$OUT/xdg/$role-$mode-$run.XXXX")
        port=$(free_port)
        ( exec setsid env "${renv[@]}" "${legion[@]}" XDG_DATA_HOME="$xdg" taskset -c "$CORES" \
            "$b/takserver" --port "$port" --data "$DATA" --no-auth --local --seed 1 \
            > "$log.server" 2>&1 ) &
        spid=$!
        for _ in $(seq 1 240); do
            grep -q listening "$log.server" 2>/dev/null && break
            kill -0 $spid 2>/dev/null || break
            sleep 0.5
        done
        grep -q listening "$log.server" && break
        kill -TERM -- -$spid 2>/dev/null; wait $spid 2>/dev/null; spid=
    done
    [ -n "$spid" ] || { echo "takserver did not start" > "$log"; return 1; }
    ( exec setsid env "${renv[@]}" "${legion[@]}" XDG_DATA_HOME="$xdg" TAK_HEADLESS=1 \
        SDL_VIDEODRIVER=dummy timeout -k 10 $((MPAI_TIME * 4 + 300)) taskset -c "$CORES" \
        "$b/takclient" game "Inner Circle" --data "$DATA" --server 127.0.0.1 \
        --serverport "$port" --mpai --time "$MPAI_TIME" > "$log" 2>&1 ) &
    cpid=$!
    wait $cpid; cpid=
    kill -TERM -- -$spid 2>/dev/null; wait $spid 2>/dev/null; spid=
}
MPAI_PIDS=()
if [ $MPAI_RUNS -gt 0 ] && [ $BOTH_DEBUG = 1 ]; then
    TM=$(date +%s)
    for role in base cand; do
        for mode in legion retail; do
            for run in $(seq 1 $MPAI_RUNS); do
                [ $role = base ] && cache_get "mpai$MPAI_TIME-$mode-$run" && continue
                mpai_game $role $mode $run &
                PIDS+=($!); MPAI_PIDS+=($!)
            done
        done
    done
    echo "legion_identity: mpai: ${#MPAI_PIDS[@]} games started in the background"
fi

# --- replays and golden (job pool, one job per core) -------------------------
POOL=()
pool_wait() {  # block until fewer than $1 pool jobs are running
    while :; do
        local running=" $(jobs -pr | tr '\n' ' ') " live=() pid
        for pid in "${POOL[@]}"; do case "$running" in *" $pid "*) live+=("$pid");; esac; done
        POOL=("${live[@]}")
        [ ${#POOL[@]} -lt "$1" ] && return
        wait -n "${POOL[@]}" 2>/dev/null
    done
}
pool_run() {  # log cwd cmd...
    pool_wait "$NCORES"
    launch "$@"
    POOL+=($LAUNCHED)
}
TR=$(date +%s)
if [ -n "$REPLAY_NAMES" ] && [ $BOTH_DEBUG = 1 ]; then
    # Longest first: the 2h replays carry 15k units.
    ORDER=$(for n in $REPLAY_NAMES; do case $n in *2h*) echo "0 $n";; *) echo "1 $n";; esac; done | sort | cut -d' ' -f2)
    for name in $ORDER; do
        for role in base cand; do
            src=$REPLAYS/$name.takrep
            [ -f "$src" ] || { echo "missing replay $src" > "$OUT/$role/replay-$name.log"; continue; }
            [ $role = base ] && cache_get "replay-$name-$(sha256sum "$src" | cut -c1-12)" && continue
            dst=$OUT/replays/$name-p${PROTO[$role]}.takrep
            [ -f "$dst" ] || $PY patch-replay "$src" "$dst" "${PROTO[$role]}" >> "$OUT/replays/patch.log" || continue
            xdg=$(mktemp -d "$OUT/xdg/replay-$role-$name.XXXX")
            mapfile -t renv < <(role_env "$role")
            pool_run "$OUT/$role/replay-$name.log" "$OUT" env "${renv[@]}" TAK_REPLAY_VERIFY=1 \
                TAK_HASHDETAIL=4294967295 XDG_DATA_HOME="$xdg" TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy \
                timeout -k 10 5400 taskset -c "$CORES" "${BUILD[$role]}/takclient" replay "$dst" --data "$DATA"
        done
    done
fi
if ! skipped golden; then
    for role in base cand; do
        mapfile -t renv < <(role_env "$role")
        for mode in legion retail; do
            [ $role = base ] && cache_get "golden-$mode" && continue
            pool_run "$OUT/$role/golden-$mode.txt" "$OUT" env "${renv[@]}" timeout -k 10 1200 \
                taskset -c "$CORES" "${BUILD[$role]}/navigation_checkpoint_test" $mode serial
        done
    done
fi
# Every replay/golden job is launched (the long 2h replays first); the crowdbench
# matrix starts now, beside the tail of the pool, instead of idling cores behind it.
CB_PID=
if ! skipped crowdbench; then
    TC=$(date +%s)
    args=(crowdbench --out "$OUT" --base "$BASE/crowdbench" --cand "$CAND/crowdbench" --cores "$CORES" --jobs "$NCORES")
    [ $QUICK = 1 ] && args+=(--quick)
    [ $VERIFY = 1 ] && args+=(--verify)
    [ -n "$BKEY" ] && args+=(--base-cache "$BKEY")
    launch "$OUT/crowdbench.log" "$OUT" $PY "${args[@]}"
    CB_PID=$LAUNCHED
fi
pool_wait 1
section_time replays+golden $TR
echo "legion_identity: replays and golden done ($(( $(date +%s) - TR )) s)"
store_base

# --- crowdbench ---------------------------------------------------------------
if [ -n "$CB_PID" ]; then
    wait $CB_PID || echo "legion_identity: crowdbench runner failed (see $OUT/crowdbench.log)"
    section_time crowdbench $TC
    echo "legion_identity: crowdbench done ($(( $(date +%s) - TC )) s)"
    grep "from the cache" "$OUT/crowdbench.log" | sed 's/^/legion_identity: cache hit: /'
fi

# --- ctest ----------------------------------------------------------------------
if ! skipped ctest; then
    TT=$(date +%s)
    for role in base cand; do
        [ $role = base ] && cache_get "$CT_ROW" && continue
        mapfile -t renv < <(role_env "$role")
        launch "$OUT/$role/ctest.log" "$OUT" env "${renv[@]}" taskset -c "$CORES" \
            ctest --test-dir "${BUILD[$role]}" -j "$NCORES" --timeout 1800 \
            "${CT_LABELS[@]}" -R '^(legion_|retail|sim_driver_equiv$|navigation_determinism$)' \
            --output-junit "$OUT/$role/ctest.xml" \
            --test-output-size-passed 4000000 --test-output-size-failed 4000000
        wait $LAUNCHED
    done
    store_base
    section_time ctest $TT
    echo "legion_identity: ctest done ($(( $(date +%s) - TT )) s)"
fi

# --- check-determinism.sh (once per distinct source tree) ----------------------
if ! skipped determinism; then
    TD=$(date +%s)
    if ! cache_get determinism; then
        launch "$OUT/base/determinism.log" "$OUT" taskset -c "$CORES" sh "${SRC[base]}/tools/check-determinism.sh"
        wait $LAUNCHED
        store_base
    fi
    if [ "${SRC[cand]}" = "${SRC[base]}" ]; then
        cp "$OUT/base/determinism.log" "$OUT/cand/determinism.log"
    else
        launch "$OUT/cand/determinism.log" "$OUT" taskset -c "$CORES" sh "${SRC[cand]}/tools/check-determinism.sh"
        wait $LAUNCHED
    fi
    section_time determinism $TD
fi

if [ ${#MPAI_PIDS[@]} -gt 0 ]; then
    echo "legion_identity: waiting for the mpai games"
    for pid in "${MPAI_PIDS[@]}"; do wait "$pid"; done
    store_base
    section_time mpai $TM
fi
section_time total $T0
echo "cache_hits=${CACHE_HITS[*]:-none}" >> "$OUT/meta.txt"
rm -rf "$OUT/xdg"
$PY report --out "$OUT"
RC=$?
# Promotion: a fully passing run proves every candidate result equals the base one, so a clean,
# built candidate's results are exactly what a base run of that commit would produce. Store them
# under the candidate's commit: the next step of a chain (this cand as its base) then starts warm.
if [ $RC = 0 ] && [ $NOCACHE = 0 ] && [ $VERIFY = 0 ] && [ $BOTH_DEBUG = 1 ] && [ -n "$BKEY" ] \
        && [ "${SRC[cand]}" != "${SRC[base]}" ]; then
    CKEY=$($PY cache-key --src "${SRC[cand]}" --build "$CAND" --type "${TYPE[cand]}-d${DEBUG[cand]}-data$(printf %s "$DATA" | sha256sum | cut -c1-6)") || CKEY=
    if [ -n "$CKEY" ] && [ "$CKEY" != "$BKEY" ]; then
        mkdir -p "$CKEY"
        put_all "$CKEY" "$OUT/cand"
        $PY cache-promote --dir "$CKEY" --out "$OUT" || true
    fi
fi
exit $RC
