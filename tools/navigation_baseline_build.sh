#!/usr/bin/env bash
# Reconstruct the FROZEN navigation baseline crowdbench, deterministically.
#
#   tools/navigation_baseline_build.sh OUT_DIR [CANDIDATE_SOURCE_DIR] [BASE_REVISION]
#
# What goes into the baseline binary, and nothing else:
#   1. Every file of BASE_REVISION (default: main 86673b4, the pre-investigation
#      simulation) exported with `git archive` -- no working-tree state.
#   2. tools/navigation_baseline_telemetry.patch: the observation-only route
#      lifecycle hooks in PathService (native searches) and in the base
#      revision's FlowNavigator (its since-removed Flowfield/Cooperative modes,
#      which the harness no longer selects). Tokens are never read by the
#      simulation, saved, or hashed; the patch is taken from the candidate tree.
#   3. The CANDIDATE's harness, copied verbatim so both binaries measure with
#      identical code: tools/crowdbench.cpp, tools/crowdbench_matrix.h,
#      tools/crowdbench_telemetry.h, tools/crowdbench_acceptance.h and src/sim/navigationtelemetry.h.
# Built Release with the same compiler and the project's static dependencies,
# only the crowdbench target, with source paths remapped so the binary does not
# depend on OUT_DIR. OUT_DIR/baseline-provenance.json records every input hash,
# the compiler, flags and the resulting binary's sha256.
#
# The CANDIDATE binary should be built the same way, from its committed tree
# with no overlay, so both executables share flags and path remapping:
#   OVERLAY=0 tools/navigation_baseline_build.sh OUT_DIR . HEAD
# (the binary is still written as OUT_DIR/crowdbench-baseline; rename it).
#
# Environment: TAK_STATIC_DEPS_PREFIX / TAK_FFMPEG_PREFIX (passed to CMake when
# set), CMAKE_GENERATOR (default Ninja), JOBS (default 8), TASKSET_CPUS (optional
# cpu list for the compile, e.g. 2-23).
set -euo pipefail
out=${1:?usage: navigation_baseline_build.sh OUT_DIR [CANDIDATE_SOURCE_DIR] [BASE_REVISION]}
candidate=${2:-$(cd "$(dirname "$0")/.." && pwd)}
base=${3:-86673b413a4747e2b19a128777c2223656cd0681}
candidate=$(cd "$candidate" && pwd)
patch_file="$candidate/tools/navigation_baseline_telemetry.patch"
harness=(tools/crowdbench.cpp tools/crowdbench_matrix.h tools/crowdbench_telemetry.h tools/crowdbench_acceptance.h src/sim/navigationtelemetry.h)

if [[ -e "$out/src" ]]; then
  echo "error: $out/src exists; use a fresh OUT_DIR" >&2; exit 1
fi
mkdir -p "$out/src"
out=$(cd "$out" && pwd)
src="$out/src"
base=$(git -C "$candidate" rev-parse --verify "$base^{commit}")
git -C "$candidate" archive --format=tar "$base" | tar -x -C "$src"
# The candidate is built from committed sources; copying uncommitted harness
# edits would silently give the two binaries different harnesses.
if [[ "${ALLOW_DIRTY_HARNESS:-0}" != 1 ]] &&
   [[ -n "$(git -C "$candidate" status --porcelain -- "${harness[@]}" tools/navigation_baseline_telemetry.patch)" ]]; then
  echo "error: harness files have uncommitted changes; commit them or set ALLOW_DIRTY_HARNESS=1" >&2; exit 1
fi
if [[ "${OVERLAY:-1}" == 1 ]]; then
  (cd "$src" && patch -p1 --forward --batch < "$patch_file")
  for file in "${harness[@]}"; do cp "$candidate/$file" "$src/$file"; done
fi

cmake_args=(-S "$src" -B "$out/build" -G "${CMAKE_GENERATOR:-Ninja}" -DCMAKE_BUILD_TYPE=Release
            "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=$src=." "-DCMAKE_C_FLAGS=-ffile-prefix-map=$src=.")
[[ -n "${TAK_STATIC_DEPS_PREFIX:-}" ]] && cmake_args+=("-DTAK_STATIC_DEPS_PREFIX=$TAK_STATIC_DEPS_PREFIX")
[[ -n "${TAK_FFMPEG_PREFIX:-}" ]] && cmake_args+=("-DTAK_FFMPEG_PREFIX=$TAK_FFMPEG_PREFIX")
cmake "${cmake_args[@]}" > "$out/configure.log" 2>&1 || { tail -40 "$out/configure.log"; exit 1; }
runner=()
[[ -n "${TASKSET_CPUS:-}" ]] && runner=(taskset -c "$TASKSET_CPUS")
"${runner[@]}" cmake --build "$out/build" --target crowdbench -j "${JOBS:-8}" > "$out/build.log" 2>&1 \
  || { tail -40 "$out/build.log"; exit 1; }
cp "$out/build/crowdbench" "$out/crowdbench-baseline"

# Exact compile/link commands of the crowdbench target (Ninja), for the record.
if [[ -f "$out/build/build.ninja" ]]; then
  ninja -C "$out/build" -t commands crowdbench > "$out/build-commands.txt" 2>/dev/null || true
fi
dirty=$(git -C "$candidate" status --porcelain -- "${harness[@]}" tools/navigation_baseline_telemetry.patch)
python3 - "$out" "$base" "$candidate" "$(git -C "$candidate" rev-parse HEAD)" "${dirty:+true}" \
    "$patch_file" "${harness[@]}" <<'PY'
import hashlib, json, os, subprocess, sys
from pathlib import Path
out, base, candidate, revision, dirty, patch, *harness = sys.argv[1:]
out = Path(out)
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
cache = (out / "build" / "CMakeCache.txt").read_text()
setting = lambda k: next((l.split("=", 1)[1] for l in cache.splitlines() if l.startswith(k + ":")), None)
compiler = setting("CMAKE_CXX_COMPILER")
commands = out / "build-commands.txt"
record = {
    "base_revision": base, "overlay_applied": os.environ.get("OVERLAY", "1") == "1",
    "candidate_source": candidate, "candidate_revision": revision,
    "candidate_harness_or_patch_dirty": dirty == "true",
    "telemetry_patch": patch, "telemetry_patch_sha256": sha(patch),
    "harness_sha256": {f: sha(out / "src" / f) for f in harness},
    "compiler": subprocess.run([compiler, "--version"], capture_output=True, text=True).stdout.splitlines()[0],
    "cmake_build_type": setting("CMAKE_BUILD_TYPE"),
    "cmake_cxx_flags": setting("CMAKE_CXX_FLAGS"), "cmake_cxx_flags_release": setting("CMAKE_CXX_FLAGS_RELEASE"),
    "crowdbench_build_commands": commands.read_text().splitlines()[-2:] if commands.exists() else None,
    "binary": str(out / "crowdbench-baseline"), "binary_sha256": sha(out / "crowdbench-baseline"),
}
(out / "baseline-provenance.json").write_text(json.dumps(record, indent=2) + "\n")
print(json.dumps(record, indent=2))
PY
