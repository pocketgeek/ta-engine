#!/usr/bin/env bash
# Visual review sheets for the navigation modes (observed positions only).
#
#   tools/navigation_visual.sh CROWDBENCH OUT_DIR [CPU_LIST] [TICKS] [SCENARIOS...]
#
# For every scenario (default: doors maze opposingcolumns sharedgoal exploration)
# and population (VISUAL_UNITS, default "200 2000"; one player, all moving, seed
# VISUAL_SEED=0), runs Retail and Legion (MODES overrides) with --trace,
# then writes OUT_DIR/<scenario>-<units>.png: one row per mode, columns at fixed
# ticks plus a time-in-place heatmap (tools/navigation_plot.py). Per-tick traces
# are large (~100 MB at 2000 units) and are deleted after plotting unless
# KEEP_TRACES=1. The JSON results are kept next to the sheets. Pinned with
# taskset, one process per cpu in CPU_LIST (default 2-23).
set -euo pipefail
bench=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
out=${2:?usage: navigation_visual.sh CROWDBENCH OUT_DIR [CPU_LIST] [TICKS] [SCENARIOS...]}
cpus=${3:-2-23}
ticks=${4:-6000}
shift $(( $# < 4 ? $# : 4 ))
scenarios=("$@")
[[ ${#scenarios[@]} -gt 0 ]] || scenarios=(doors maze opposingcolumns sharedgoal exploration)
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out/traces"
cpu_list=()
IFS=, read -ra parts <<< "$cpus"
for part in "${parts[@]}"; do
  if [[ "$part" == *-* ]]; then for ((c=${part%-*}; c<=${part#*-}; c++)); do cpu_list+=("$c"); done
  else cpu_list+=("$part"); fi
done
modes=(${MODES:-retail legion})
jobs=()
for scenario in "${scenarios[@]}"; do
  for units in ${VISUAL_UNITS:-200 2000}; do
    for mode in "${modes[@]}"; do jobs+=("$scenario $units $mode"); done
  done
done
running=0; index=0
for job in "${jobs[@]}"; do
  read -r scenario units mode <<< "$job"
  cpu=${cpu_list[$((index % ${#cpu_list[@]}))]}
  name="$scenario-$units-$mode"
  taskset -c "$cpu" "$bench" --mode "$mode" --scenario "$scenario" --units "$units" --players 1 \
    --moving-percent 100 --ticks "$ticks" --seed "${VISUAL_SEED:-0}" \
    --trace "$out/traces/$name.trace.csv" > "$out/$name.json" &
  index=$((index + 1)); running=$((running + 1))
  if [[ $running -ge ${#cpu_list[@]} ]]; then wait -n; running=$((running - 1)); fi
done
wait
plot_ticks=(0 $((ticks / 10)) $((ticks / 4)) $((ticks / 2)) "$ticks")
for scenario in "${scenarios[@]}"; do
  for units in ${VISUAL_UNITS:-200 2000}; do
    traces=(); labels=()
    for mode in "${modes[@]}"; do
      traces+=("$out/traces/$scenario-$units-$mode.trace.csv")
      labels+=("$mode $scenario $units")
    done
    python3 "$here/navigation_plot.py" "${traces[@]}" --labels "${labels[@]}" --heatmap \
      --ticks "${plot_ticks[@]}" --title "$scenario, $units units, seed ${VISUAL_SEED:-0}, $(basename "$bench")" \
      --output "$out/$scenario-$units.png"
  done
done
[[ "${KEEP_TRACES:-0}" == 1 ]] || rm -rf "$out/traces"
