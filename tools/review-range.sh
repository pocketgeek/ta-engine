#!/usr/bin/env bash
# review-range.sh -- run the Codex reviewer over every commit in a range, not just the
# last one. Intended as a release gate: before tagging, look at everything that landed
# since the previous tag.
#
#   tools/review-range.sh [base-ref] [jobs]      default: v0.6.5 4
#
# WHY A WORKTREE PER COMMIT. `codex review --base <rev>` diffs the WORKING TREE against
# that rev -- it does not take a commit range. So reviewing history means checking each
# commit out somewhere. A detached worktree per commit does that without disturbing
# your tree, and lets several run at once.
#
# Reviews land in the same place the post-commit hook uses, and an already-finished
# review is never redone: this is resumable, which matters when it takes tens of
# minutes and something interrupts it.
set -u

BASE="${1:-v0.6.5}"
JOBS="${2:-4}"

cd "$(git rev-parse --show-toplevel)" || exit 1
git rev-parse --verify -q "$BASE" >/dev/null || { echo "no such ref: $BASE" >&2; exit 2; }
command -v codex >/dev/null 2>&1 || { echo "codex not installed" >&2; exit 2; }

OUTDIR=$(git rev-parse --git-path codex-reviews)
mkdir -p "$OUTDIR"
MODEL="${TAK_REVIEW_MODEL:-gpt-6-astra}"

mapfile -t SHAS < <(git rev-list --reverse "$BASE..HEAD")
echo "reviewing ${#SHAS[@]} commits since $BASE, $JOBS at a time, model $MODEL"
echo "output: $OUTDIR"

review_one() {
    local sha="$1"
    local short; short=$(git rev-parse --short "$sha")
    local out="$OUTDIR/$short.md"
    # Resumable: a finished review is evidence, never redone or overwritten.
    if [ -s "$out" ] && grep -q '^finished: ' "$out" 2>/dev/null; then
        echo "  skip $short (already reviewed)"; return 0
    fi
    local parent; parent=$(git rev-parse --verify -q "$sha^") || { echo "  skip $short (root)"; return 0; }

    local wt; wt=$(mktemp -d "${TMPDIR:-/tmp}/tak-review-XXXXXX")
    if ! git worktree add --detach "$wt" "$sha" >/dev/null 2>&1; then
        rm -rf "$wt"; echo "  FAIL $short (worktree)"; return 1
    fi
    {
        echo "# codex review -- $short"
        echo
        echo "    $(git log -1 --format='%s' "$sha")"
        echo
        echo "model: $MODEL   started: $(date '+%Y-%m-%d %H:%M:%S')"
        echo
    } >"$out"
    ( cd "$wt" && codex review --base "$parent" -c model="$MODEL" ) >>"$out" 2>&1
    local rc=$?
    { echo; echo "---"; echo "finished: $(date '+%Y-%m-%d %H:%M:%S')  exit: $rc"; } >>"$out"
    git worktree remove --force "$wt" >/dev/null 2>&1 || rm -rf "$wt"

    local n; n=$(grep -cE '^\s*-\s*\[P[0-9]\]' "$out" 2>/dev/null || echo 0)
    printf '  %-10s %s  %s\n' "$short" "$([ "$n" -gt 0 ] && echo "$n finding(s)" || echo "clean     ")" \
           "$(git log -1 --format='%s' "$sha" | cut -c1-58)"
}

i=0
for sha in "${SHAS[@]}"; do
    review_one "$sha" &
    i=$((i + 1))
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 3; done
done
wait

echo
echo "==== FINDINGS ACROSS $BASE..HEAD ===="
# Severity first: a P1 in a release range is the thing to see, whichever commit it is.
for sev in P1 P2 P3; do
    for f in "$OUTDIR"/*.md; do
        [ -f "$f" ] || continue
        sha=$(basename "$f" .md)
        grep -hE "^\s*-\s*\[$sev\]" "$f" 2>/dev/null | sort -u | while read -r line; do
            printf '%s  %s\n' "$sha" "$line"
        done
    done
done
echo
echo "reviews: $OUTDIR"
