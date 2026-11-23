#!/usr/bin/env bash
# featburn_timing.sh -- swapped base/candidate pairs of tools/featburn_bench (feature burning
# and fading). Run it under the timing-core lock, e.g.
#   /home/pocket_geek/tak-tmp/tools/timing.sh tools/featburn_timing.sh BASE_BENCH CAND_BENCH DATA [ROUNDS] [TICKS] [CORE]
# BASE_BENCH/CAND_BENCH: featburn_bench binaries built against each tree. Every run is pinned to
# CORE (default 2); within a round the order alternates per workload. Prints one line per run.
set -euo pipefail
base=$1 cand=$2 data=$3 rounds=${4:-3} ticks=${5:-3000} core=${6:-2}
workloads=("--map|Ulasem Arena" "--gen|1024")
for r in $(seq 1 "$rounds"); do
  for w in "${workloads[@]}"; do
    flag=${w%%|*} val=${w#*|}
    for scene in nofire sparse fire fade; do
      for mode in routine tick; do
        [ $mode = tick ] && [ $scene != nofire ] && [ $scene != fire ] && continue   # keep the round short
        if (( r % 2 )); then order=(base cand); else order=(cand base); fi
        for who in "${order[@]}"; do
          bin=$base; [ "$who" = cand ] && bin=$cand
          out=$(taskset -c "$core" "$bin" --data "$data" "$flag" "$val" --scene $scene --mode $mode --ticks "$ticks")
          echo "round=$r who=$who map=${val// /_} scene=$scene mode=$mode $(echo "$out" | grep -o 'features=[0-9]*') $(echo "$out" | grep '^ticks=') $(echo "$out" | grep -o 'mean_burning=[^ ]*') $(echo "$out" | grep -o 'hash=[0-9a-f]*')"
        done
      done
    done
  done
done
