#!/usr/bin/env bash
# Run an explicit list of CTest tests, failing if any of them is not registered.
#
#   tools/run-ctest-selection.sh [--exclude-labels REGEX] [--selection FILE] BUILD_DIR [NAME...]
#
# `ctest -R` passes silently for a pattern that matches nothing: a renamed test,
# or one CMake skipped (the Python-driven navigation tests are registered only
# when CMake finds an interpreter) would quietly drop out of CI. Each NAME is an
# exact test name or a glob (`*` matches any run of characters, e.g. legion_acceptance_*
# or legion_acceptance_*_legion), which must match at least one registered test.
# A NAME that starts with `!` EXCLUDES the tests it matches from the run (it is not
# required to match anything). --exclude-labels REGEX drops every test carrying a
# label that matches (ctest -LE), e.g. 'nightly|data' for the per-commit Legion
# selection. --selection FILE adds the names listed in FILE (one per line, `#` starts
# a comment; the per-commit Legion selection is tools/legion_ci_selection.txt, shared by
# the three CI workflows). Parallelism comes from CTEST_PARALLEL_LEVEL, if set.
# Portable to bash 3.2 (macOS) and MSYS2.
set -euo pipefail
usage() { echo "usage: run-ctest-selection.sh [--exclude-labels REGEX] [--selection FILE] BUILD_DIR [NAME...]" >&2; exit 2; }
labels=""
fromfile=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --exclude-labels) [[ $# -ge 2 ]] || usage; labels=$2; shift 2 ;;
    --selection)
      [[ $# -ge 2 && -f "$2" ]] || { echo "run-ctest-selection: no selection file '${2:-}'" >&2; exit 2; }
      while IFS= read -r line || [[ -n "$line" ]]; do
        line=${line%%#*}; line=${line//$'\r'/}
        set -f; for word in $line; do fromfile+=("$word"); done; set +f   # no pathname expansion of globs
      done < "$2"
      shift 2 ;;
    --) shift; break ;;
    -*) usage ;;
    *) break ;;
  esac
done
build=${1:-}
[[ -n "$build" ]] || usage
shift
names=("$@")
if [[ ${#fromfile[@]} -gt 0 ]]; then names=(${names[@]+"${names[@]}"} "${fromfile[@]}"); fi
[[ ${#names[@]} -gt 0 ]] || { echo "run-ctest-selection: no tests named" >&2; exit 2; }
listed=$(ctest --test-dir "$build" -N | sed -n 's/^ *Test *#[0-9]*: *//p' | tr -d '\r')
# glob -> anchored-free ERE: escape the regex metacharacters, then * -> .*
to_regex() { printf '%s' "$1" | sed -e 's/[][\.^$+?(){}|]/\\&/g' -e 's/\*/.*/g'; }
include=""
exclude=""
missing=0
for name in "${names[@]}"; do
  if [[ "$name" == '!'* ]]; then
    exclude="${exclude:+$exclude|}$(to_regex "${name#!}")"
    continue
  fi
  regex=$(to_regex "$name")
  count=$(printf '%s\n' "$listed" | grep -cE "^(${regex})\$" || true)
  if [[ "$count" -eq 0 ]]; then
    echo "::error::ctest test '$name' is not registered in $build" >&2
    missing=1
  else
    echo "selected $name ($count test(s))"
  fi
  include="${include:+$include|}$regex"
done
[[ "$missing" -eq 0 ]] || exit 1
[[ -n "$include" ]] || { echo "run-ctest-selection: only exclusions named" >&2; exit 2; }
args=(--test-dir "$build" -R "^(${include})\$" --no-tests=error --output-on-failure)
[[ -z "$exclude" ]] || args+=(-E "^(${exclude})\$")
[[ -z "$labels" ]] || args+=(-LE "$labels")
ctest "${args[@]}"
