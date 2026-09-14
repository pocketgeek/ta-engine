#!/usr/bin/env bash
# install-commit-review.sh -- install (or remove) the post-commit Codex review hook.
#
#   tools/install-commit-review.sh              install
#   tools/install-commit-review.sh --uninstall  remove
#
# Hooks are NOT version controlled, so this exists to make the hook reproducible: the
# reviewing logic lives in tools/commit-review.sh (tracked), and the hook is a wrapper
# that detaches it. Anyone cloning the repo gets the script but no hook until they run
# this -- which is the right default, since it sends diffs to a third party.
set -u

cd "$(git rev-parse --show-toplevel)" || exit 1
HOOK="$(git rev-parse --git-path hooks/post-commit)"

if [ "${1:-}" = "--uninstall" ]; then
    if [ -f "$HOOK" ] && grep -q "commit-review.sh" "$HOOK" 2>/dev/null; then
        rm -f "$HOOK"; echo "removed $HOOK"
    else
        echo "no codex review hook installed"
    fi
    exit 0
fi

# Refuse to clobber somebody else's hook rather than silently replacing it.
if [ -f "$HOOK" ] && ! grep -q "commit-review.sh" "$HOOK" 2>/dev/null; then
    echo "a post-commit hook already exists and is not ours: $HOOK" >&2
    echo "move it aside first, or add this line to it:" >&2
    echo '  (setsid tools/commit-review.sh >/dev/null 2>&1 </dev/null &) || true' >&2
    exit 1
fi

mkdir -p "$(dirname "$HOOK")"
cat >"$HOOK" <<'EOF'
#!/usr/bin/env bash
# Codex review of the commit just made -- see tools/commit-review.sh.
# setsid + background + closed stdio: the commit must not wait for a network review,
# and must not fail if one is impossible. `|| true` so this can never return non-zero.
(setsid tools/commit-review.sh >/dev/null 2>&1 </dev/null &) || true
exit 0
EOF
chmod +x "$HOOK"
echo "installed $HOOK"
echo "  reviews -> .git/codex-reviews/<sha>.md"
echo "  model   -> ${TAK_REVIEW_MODEL:-gpt-6-astra}  (override with TAK_REVIEW_MODEL)"
echo "  skip one commit with TAK_NO_COMMIT_REVIEW=1"
echo
echo "NOTE: every commit's diff is sent to OpenAI. Assets and the retail binary are"
echo "      gitignored so they never travel, but RE detail in comments and commit"
echo "      messages does."
