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

# ---- automatic fixing --------------------------------------------------------------
# Off with TAK_REVIEW_AUTOFIX=0. Four gates, because this edits code unattended:
#
#   1. ONLY ON A CLEAN TREE. Editing files underneath someone mid-change is hostile and
#      makes the result impossible to tell apart from their own work. A dirty tree means
#      the review is left for them and nothing is touched.
#   2. IT MUST BUILD. An unattended fix that breaks the build is worse than the bug it
#      fixed, because it surfaces later and somewhere else. If it does not build, the
#      changes are reverted.
#   3. NEVER COMMIT. Changes are left in the working tree to read, amend or discard.
#      Committing would also LOOP: the commit triggers a review, which finds something,
#      which fixes and commits again.
#   4. ALWAYS RECOVERABLE. The diff is saved beside the review first, so even a reverted
#      or unwanted fix can still be read afterwards.
[ "${TAK_REVIEW_AUTOFIX:-1}" = "0" ] && exit 0

if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
  printf 'codex autofix %s: skipped, working tree is dirty -- review at %s\n' "$SHA" "$OUT" >&2
  exit 0
fi

PATCHFILE="$OUTDIR/$SHA.autofix.patch"
codex exec -s workspace-write -c model="$MODEL" - >>"$OUT" 2>&1 <<PROMPT
A code review of commit $SHA reported the findings below. Fix them in the working
tree. Change only what the findings require: no refactoring, no unrelated edits, no
new features. Do not commit anything. If a finding is wrong, leave that code alone
and say so rather than changing it.

$(cat "$OUT")
PROMPT

if git diff --quiet 2>/dev/null && [ -z "$(git status --porcelain 2>/dev/null)" ]; then
  printf 'codex autofix %s: no changes made\n' "$SHA" >&2
  exit 0
fi
git diff >"$PATCHFILE" 2>/dev/null   # gate 4: save it BEFORE anything can revert it

# Gate 2: it has to BUILD and PASS THE TESTS.
#
# Both trees, because src/sim is linked into every target and a debug-only build would
# miss a break in the release one. And "it compiles" is a weak bar for this codebase --
# a change can build perfectly and still break lockstep determinism, which is the one
# property everything here rests on. The suite runs in about two seconds and covers
# exactly that, including navblock (the two world-build paths must agree cell for cell)
# and detmath's cross-build golden hash, so there is no reason not to spend it.
_gate_fail=""
cmake --build build-dbg >/dev/null 2>&1 || _gate_fail="debug build"
[ -n "$_gate_fail" ] || cmake --build build >/dev/null 2>&1 || _gate_fail="release build"
[ -n "$_gate_fail" ] || (cd build && ctest --output-on-failure >/dev/null 2>&1) || _gate_fail="tests"

if [ -z "$_gate_fail" ]; then
  printf 'codex autofix %s: applied, BUILDS and TESTS PASS -- review %s, patch %s\n' \
         "$SHA" "$OUT" "$PATCHFILE" >&2
  printf '  left UNCOMMITTED in your working tree; `git checkout -- .` discards it\n' >&2
else
  git checkout -- . 2>/dev/null
  printf 'codex autofix %s: REVERTED -- %s failed. Patch kept at %s\n' \
         "$SHA" "$_gate_fail" "$PATCHFILE" >&2
fi
exit 0
