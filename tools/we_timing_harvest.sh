#!/usr/bin/env bash
# we_timing_harvest.sh <takclient> <replay.takrep> <tick> <out.scn.gz> [--data DIR] [--cores LIST]
# Cuts one WE timing situation (docs/development.md "WE timing") out of a recording: runs the replay
# with TAK_SITUATION=<tick>:<file>, stops it the moment the file is written, and gzips the .scn.
# <takclient> is a DEBUG/o2 build (the hook is debug-only) of the tree the replay plays back on
# (protocol must match: tools/legion_identity.py patch-replay re-stamps a current-format recording).
# The command window is TAK_SITUATION_CMDS (default 3000 ticks): a timing run replays the recording's
# commands (human and AI) for as long as it simulates.
set -euo pipefail
CLIENT=$1; REPLAY=$2; TICK=$3; OUT=$4; shift 4
DATA=${TAK_DATA:-$HOME/TAK/assets/game}; CORES=${TAK_HARVEST_CORES:-}
while [ $# -gt 0 ]; do case $1 in --data) DATA=$2; shift 2;; --cores) CORES=$2; shift 2;; *) echo "unknown $1" >&2; exit 2;; esac; done
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export XDG_DATA_HOME=$T/xdg TAK_SITUATION="$TICK:$T/s.scn" TAK_SITUATION_CMDS=${TAK_SITUATION_CMDS:-3000} \
       TAK_REPLAY_VERIFY=1 TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy
${CORES:+taskset -c $CORES} "$CLIENT" replay "$REPLAY" --data "$DATA" > "$T/log" 2>&1 &
pid=$!
until grep -q "situation: \(wrote\|map\|cannot\)" "$T/log" 2>/dev/null; do
  kill -0 $pid 2>/dev/null || { cat "$T/log" >&2; echo "replay ended without a situation" >&2; exit 1; }
  sleep 2
done
kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true
grep "situation:" "$T/log"
[ -s "$T/s.scn" ] || exit 1
gzip -9 -n -c "$T/s.scn" > "$OUT"
