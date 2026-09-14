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

# We own only the text BETWEEN these markers. A plain "does the file mention
# commit-review.sh" test is not ownership: the installer used to tell people to append
# its line to an existing hook, and then --uninstall would delete that whole hook,
# taking their logic with it. Marked section = we can add and remove our part without
# touching anyone else's.
BEGIN="# >>> codex commit-review >>>"
END="# <<< codex commit-review <<<"

strip_section() {   # stdin -> stdout, with our section removed
    awk -v b="$BEGIN" -v e="$END" '
        index($0, b) == 1 { skip = 1; next }
        index($0, e) == 1 { skip = 0; next }
        !skip { print }'
}

if [ "${1:-}" = "--uninstall" ]; then
    if [ ! -f "$HOOK" ] || ! grep -qF "$BEGIN" "$HOOK" 2>/dev/null; then
        echo "no codex review hook installed"; exit 0
    fi
    rest=$(strip_section <"$HOOK")
    # Only OUR section was in there -> remove the file. Otherwise keep the rest.
    if printf '%s\n' "$rest" | grep -qvE '^\s*(#|\s*$|#!/)' ; then
        printf '%s\n' "$rest" >"$HOOK"; chmod +x "$HOOK"
        echo "removed our section from $HOOK (other hook logic kept)"
    else
        rm -f "$HOOK"; echo "removed $HOOK"
    fi
    exit 0
fi

mkdir -p "$(dirname "$HOOK")"
if [ -f "$HOOK" ]; then
    # Re-install: drop any previous section of ours, keep everything else verbatim.
    strip_section <"$HOOK" >"$HOOK.tmp$$"
else
    printf '#!/usr/bin/env bash\n' >"$HOOK.tmp$$"
fi
cat >>"$HOOK.tmp$$" <<EOF
$BEGIN
# Codex review of the commit just made -- see tools/commit-review.sh.
# setsid + background + closed stdio: the commit must not wait for a network review,
# and must not fail if one is impossible. \`|| true\` so this can never return non-zero.
(setsid tools/commit-review.sh >/dev/null 2>&1 </dev/null &) || true
$END
EOF
mv -f "$HOOK.tmp$$" "$HOOK"
chmod +x "$HOOK"
echo "installed $HOOK"
echo "  reviews -> .git/codex-reviews/<sha>.md"
echo "  model   -> ${TAK_REVIEW_MODEL:-gpt-6-astra}  (override with TAK_REVIEW_MODEL)"
echo "  skip one commit with TAK_NO_COMMIT_REVIEW=1"
echo
echo "NOTE: every commit's diff is sent to OpenAI. Assets and the retail binary are"
echo "      gitignored so they never travel, but RE detail in comments and commit"
echo "      messages does."
