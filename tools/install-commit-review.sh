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

# MIGRATE HOOKS FROM THE UNMARKED VERSION. The first installer wrote the invocation
# with no markers, so strip_section cannot see it: an upgraded user would get a second
# (marked) copy, and --uninstall would leave the old line still firing after they
# believed they had removed it. Drop any bare invocation and its comment block too.
strip_legacy() {
    grep -vF '(setsid tools/commit-review.sh' \
      | grep -vF '# Codex review of the commit just made -- see tools/commit-review.sh.' \
      | grep -vF '# setsid + background + closed stdio: the commit must not wait for a network review,' \
      | grep -vF '# and must not fail if one is impossible. `|| true` so this can never return non-zero.' \
      | grep -vE '^\s*exit 0\s*$'
}

if [ "${1:-}" = "--uninstall" ]; then
    if [ ! -f "$HOOK" ] || ! grep -qF "$BEGIN" "$HOOK" 2>/dev/null; then
        echo "no codex review hook installed"; exit 0
    fi
    rest=$(strip_section <"$HOOK" | strip_legacy)
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
    # APPENDING SHELL IS ONLY SAFE IF THE HOOK IS SHELL. A python (or any other)
    # interpreter hook would be made syntactically invalid by our fragment, breaking a
    # hook that worked -- and a shell hook that ends in `exit` would silently never
    # reach our line, so the review would never run and nothing would say why. Refuse
    # both, as the original installer refused foreign hooks, rather than quietly
    # producing a broken or inert hook.
    # Is this hook OURS (marked, or the unmarked first version)? Decide that BEFORE the
    # compatibility checks: our own legacy hook ends in `exit 0`, so checking
    # compatibility first refused to upgrade the very hooks this migration exists for.
    _ours=0
    grep -qF "$BEGIN" "$HOOK" 2>/dev/null && _ours=1
    grep -qF '(setsid tools/commit-review.sh' "$HOOK" 2>/dev/null && _ours=1
    if [ "$_ours" = "0" ]; then
        _sb=$(head -1 "$HOOK")
        case "$_sb" in
            '#!'*sh|'#!'*sh\ *|'#!'*bash|'#!'*bash\ *) ;;
            *)  echo "existing post-commit hook is not a shell script: $HOOK" >&2
                echo "refusing to append -- it would break that hook. Move it aside," >&2
                echo "or call tools/commit-review.sh from it yourself." >&2
                exit 1;;
        esac
        if grep -qE '^\s*exit\b' "$HOOK" 2>/dev/null; then
            echo "existing post-commit hook exits before the end: $HOOK" >&2
            echo "appending would leave the review unreachable and silent. Move it" >&2
            echo "aside, or call tools/commit-review.sh from it yourself." >&2
            exit 1
        fi
    fi
    # Re-install: drop any previous section of ours (and any unmarked legacy line),
    # keep everything else verbatim.
    strip_section <"$HOOK" | strip_legacy >"$HOOK.tmp$$"
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
echo "  model   -> ${TA_REVIEW_MODEL:-gpt-6-astra}  (override with TA_REVIEW_MODEL)"
echo "  skip one commit with TA_NO_COMMIT_REVIEW=1"
echo
echo "NOTE: every commit's diff is sent to OpenAI. Assets and the retail binary are"
echo "      gitignored so they never travel, but RE detail in comments and commit"
echo "      messages does."
