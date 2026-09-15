#!/usr/bin/env bash
# commit-review.sh -- ask Codex (GPT-6, signed in with the user's ChatGPT account) to
# review the commit that was just made, in the background.
#
# Installed as a post-commit hook by tools/install-commit-review.sh. The hook is a
# one-line wrapper so the logic lives here, in version control, rather than in
# .git/hooks where it is untracked and lost on a fresh clone.
#
# THREE RULES, because this runs on every commit:
#
#   1. NEVER BLOCK A COMMIT. The review is a network round trip taking minutes; a
#      commit must not wait for it. The hook detaches and returns immediately.
#   2. NEVER FAIL A COMMIT. Codex being down, logged out, rate-limited or absent must
#      not stop work. Every path here exits 0.
#   3. NEVER LOSE THE OUTPUT. A hook cannot usefully interrupt anyone, so reviews are
#      written to .git/codex-reviews/<sha>.md and accumulate to be read on demand.
#
# It REVIEWS ONLY -- it never edits the tree. See the end of this file.
#
# WHAT LEAVES THE MACHINE: the commit's diff, to OpenAI. The retail binary and assets
# are gitignored and never enter a commit, so they cannot be sent -- but code comments
# and commit messages in this repo do carry reverse-engineering detail about
# KINGDOMS.icd (addresses, RE'd behaviour). That is the deliberate trade for automatic
# review. Set TAK_NO_COMMIT_REVIEW=1 to skip a commit, or remove the hook entirely
# with tools/install-commit-review.sh --uninstall.
set -u

[ "${TAK_NO_COMMIT_REVIEW:-0}" = "1" ] && exit 0
command -v codex >/dev/null 2>&1 || exit 0

cd "$(git rev-parse --show-toplevel 2>/dev/null)" || exit 0

# Capture the FULL sha now and review that commit explicitly. `--base HEAD~1` reviews
# a moving target: this runs detached for minutes, so another commit shifts the base
# and uncommitted edits leak into the diff -- the report would then describe changes
# that are not in the sha naming the file, which is worse than no report.
FULL=$(git rev-parse HEAD 2>/dev/null) || exit 0
SHA=$(git rev-parse --short HEAD 2>/dev/null) || exit 0
PARENT=$(git rev-parse --verify -q "$FULL^" 2>/dev/null) || exit 0   # root commit: nothing to diff

# Resolve through git, not a literal .git/: in a linked worktree or submodule .git is a
# FILE, so `mkdir -p .git/...` fails and -- because this must never fail a commit --
# every review would vanish silently. The installer already resolves this way.
OUTDIR=$(git rev-parse --git-path codex-reviews 2>/dev/null) || exit 0
mkdir -p "$OUTDIR" || exit 0
OUT="$OUTDIR/$SHA.md"

# Never clobber a finished review. Re-running this by hand (or an amend landing on the
# same short sha) used to truncate the file and start over, destroying a completed
# report -- which happened: a 30KB review became 1.2KB because the script was run again
# to test something unrelated. A finished review is evidence; keep it.
if [ -s "$OUT" ] && grep -q '^finished: ' "$OUT" 2>/dev/null; then
  printf 'codex review %s: already reviewed -- %s\n' "$SHA" "$OUT" >&2
  exit 0
fi

# Model: the account default is gpt-6-astra, but pin it so a later change to
# ~/.codex/config.toml cannot silently alter what reviews this repo. TAK_REVIEW_MODEL
# overrides for a one-off.
MODEL="${TAK_REVIEW_MODEL:-gpt-6-astra}"

{
  echo "# codex review -- $SHA"
  echo
  echo "    $(git log -1 --format='%s' "$FULL" 2>/dev/null)"
  echo
  echo "model: $MODEL   started: $(date '+%Y-%m-%d %H:%M:%S')"
  echo
} >"$OUT"

# --base takes a rev, and CANNOT be combined with a prompt argument (codex rejects
# `--base X "prompt"`), so review instructions have to go through config, not argv.
# The base is the captured PARENT sha, so the diff stays exactly this commit however
# much the tree moves on underneath.
codex review --base "$PARENT" -c model="$MODEL" >>"$OUT" 2>&1
status=$?

{
  echo
  echo "---"
  echo "finished: $(date '+%Y-%m-%d %H:%M:%S')  exit: $status"
} >>"$OUT"

# Nothing found: stop here. (The terminal that ran `git commit` is usually long gone,
# so anything printed from now on is best-effort.)
grep -qE '^\s*-\s*\[P[0-9]\]' "$OUT" 2>/dev/null || exit 0
n=$(grep -cE '^\s*-\s*\[P[0-9]\]' "$OUT")
printf 'codex review %s: %d finding(s) -- %s\n' "$SHA" "$n" "$OUT" >&2

# NO AUTOMATIC FIXING. Codex reviews; fixing is a person's job (or the assistant's,
# under review). The machinery for it existed here briefly -- generate in a throwaway
# worktree, gate on build plus the test suite, never commit -- and is deliberately gone
# rather than defaulted off, because a flag that turns unattended edits back on is a
# flag someone eventually flips.
#
# The argument for removing it is not that the gates were weak. It is that "the build
# passes and 15 tests are green" is a long way from "this change is right", and a review
# that proposes a fix has already done the valuable part: naming the defect precisely
# enough to act on. Applying it is where judgement belongs.
exit 0
