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
SHA=$(git rev-parse --short HEAD 2>/dev/null) || exit 0

# A root commit has no HEAD~1 to diff against; nothing to review.
git rev-parse --verify -q HEAD~1 >/dev/null 2>&1 || exit 0

OUTDIR=".git/codex-reviews"
mkdir -p "$OUTDIR" || exit 0
OUT="$OUTDIR/$SHA.md"

# Model: the account default is gpt-6-astra, but pin it so a later change to
# ~/.codex/config.toml cannot silently alter what reviews this repo. TAK_REVIEW_MODEL
# overrides for a one-off.
MODEL="${TAK_REVIEW_MODEL:-gpt-6-astra}"

{
  echo "# codex review -- $SHA"
  echo
  echo "    $(git log -1 --format='%s' 2>/dev/null)"
  echo
  echo "model: $MODEL   started: $(date '+%Y-%m-%d %H:%M:%S')"
  echo
} >"$OUT"

# --base takes a rev, and CANNOT be combined with a prompt argument (codex rejects
# `--base X "prompt"`), so review instructions have to go through config, not argv.
codex review --base HEAD~1 -c model="$MODEL" >>"$OUT" 2>&1
status=$?

{
  echo
  echo "---"
  echo "finished: $(date '+%Y-%m-%d %H:%M:%S')  exit: $status"
} >>"$OUT"

# A findings-shaped line is worth surfacing; otherwise stay quiet. The terminal that
# ran `git commit` is usually long gone by now, so this is best-effort only.
if grep -qE '^\s*-\s*\[P[0-9]\]' "$OUT" 2>/dev/null; then
  n=$(grep -cE '^\s*-\s*\[P[0-9]\]' "$OUT")
  printf 'codex review %s: %d finding(s) -- %s\n' "$SHA" "$n" "$OUT" >&2
fi
exit 0
