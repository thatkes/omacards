#!/bin/sh
set -eu

# Self-test for bin/dispatch-note. Fixtures feed the generator through
# --issue-file; a fake gh on PATH proves that path makes no tracker or
# network call.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
NOTE="$ROOT/bin/dispatch-note"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

mkdir -p "$tmp/bin"
cat > "$tmp/bin/gh" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$DN_GH_LOG"
printf 'unexpected gh call: %s\n' "$*" >&2
exit 99
EOF
chmod +x "$tmp/bin/gh"
PATH="$tmp/bin:$PATH"
export PATH
DN_GH_LOG="$tmp/gh-calls"
export DN_GH_LOG
: > "$DN_GH_LOG"

cat > "$tmp/expected-acceptance" <<'EOF'
## Acceptance criteria

- With a fixture issue body containing an acceptance section, the output
  contains that section byte for byte.
- The output contains every clause listed above, each on its own line.
- `--suites TestBackend TestCli` and `--worktree /abs/path` put those exact
  values in the output.
EOF

{
    printf '%s\n' 'Title: Fixture issue' '' '## Problem' '' 'Fixture body.' ''
    cat "$tmp/expected-acceptance"
    printf '%s\n' '## Checks' '' '`./bin/lint`.'
} > "$tmp/body-with.md"

cat > "$tmp/body-without.md" <<'EOF'
Title: Fixture issue

## Problem

Fixture body with no acceptance section.

## Checks

`./bin/lint`.
EOF

jq -Rs '{number:203,title:"Generate paste-ready dispatch notes",body:.}' \
    < "$tmp/body-with.md" > "$tmp/with.json"
jq -Rs '{number:203,title:"Generate paste-ready dispatch notes",body:.}' \
    < "$tmp/body-without.md" > "$tmp/without.json"

{
    printf '%s\n' \
        'Every edit, commit and check stays in the worktree.' \
        'Never run a check or commit in the integration checkout.' \
        'Report a failing focused suite with evidence, without widening scope or expanding to a full `./bin/test`.' \
        'The issue stays open until the chief closes it after merge.' \
        'Read the full command inventory literally.' \
        'A build-system change needs a rebuild-after-edit test.'
    printf 'The commit body ends with `Closes #%s`.\n' 203
    printf '%s\n' \
        'The focused suites named in this dispatch are the only suites to run: TestBackend TestCli.' \
        'Name every suite whose tests you may edit: TestBackend TestCli.' \
        '`bin/test` strips only the first `--suite` pair, and QtTest needs explicit function names rather than wildcards.' \
        'Ask the chief when the issue body and the existing tests disagree.'
    printf '%s\n' \
        'Every command against the worktree passes an explicit `workdir: /abs/path`, and `git -C /abs/path rev-parse --show-toplevel` must print exactly `/abs/path`.'
} > "$tmp/expected-clauses"

run_note() {
    set +e
    "$NOTE" "$@" > "$tmp/out" 2> "$tmp/err"
    last_status=$?
    set -e
}

run_note 203 --issue-file "$tmp/with.json" \
    --suites TestBackend TestCli --worktree /abs/path
assert_status fixture_note_succeeds 0 "$last_status"

awk '
    /^## Acceptance criteria/ { found = 1 }
    found && /^## Checks/ { exit }
    found { print }
' "$tmp/out" > "$tmp/actual-acceptance"
if cmp -s "$tmp/expected-acceptance" "$tmp/actual-acceptance"; then
    pass acceptance_section_copied_byte_for_byte
else
    fail acceptance_section_copied_byte_for_byte \
        "$(diff -u "$tmp/expected-acceptance" "$tmp/actual-acceptance" || true)"
fi

while IFS= read -r clause; do
    [ -n "$clause" ] || continue
    if grep -qxF -- "$clause" "$tmp/out"; then
        pass "clause on its own line: $clause"
    else
        fail "clause on its own line: $clause" 'missing from generated note'
    fi
done < "$tmp/expected-clauses"

run_note 203 --issue-file "$tmp/without.json" \
    --suites TestBackend --worktree /abs/path
assert_status absent_acceptance_exits_1 1 "$last_status"
assert_file_has_text absent_acceptance_named_error "$tmp/err" \
    'ERROR no acceptance criteria in issue #203'
stdout_bytes=$(wc -c < "$tmp/out" | tr -d '[:space:]')
assert_equal absent_acceptance_writes_no_prompt "$stdout_bytes" 0

run_note 203 --issue-file "$tmp/with.json" --suites TestBackend
assert_status missing_worktree_exits_2 2 "$last_status"
assert_file_has_text missing_worktree_usage "$tmp/err" 'Usage: dispatch-note'

run_note 203 --issue-file "$tmp/with.json" --suites TestBackend \
    --worktree relative/path
assert_status relative_worktree_exits_2 2 "$last_status"
assert_file_has_text relative_worktree_usage "$tmp/err" 'Usage: dispatch-note'

run_note 203 --issue-file "$tmp/with.json" --suites TestBackend --worktree
assert_status valueless_worktree_exits_2 2 "$last_status"
assert_file_has_text valueless_worktree_usage "$tmp/err" 'Usage: dispatch-note'

gh_calls=$(wc -l < "$DN_GH_LOG" | tr -d '[:space:]')
assert_equal fixture_makes_no_gh_call "$gh_calls" 0

if [ "$failures" -ne 0 ]; then
    printf '%s dispatch-note case(s) failed\n' "$failures"
    exit 1
fi
printf 'dispatch-note self-test passed\n'
