#!/bin/sh
set -eu

# Self-test for bin/check-ledger-open-ids. Fixtures drive both inputs, so the
# checker is exercised without a network call or a change to the real ledger.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-ledger-open-ids"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

cat > "$tmp/ledger.md" <<'EOF'
Preamble text outside the markers.
<!-- ledger-current-ids-start -->
| id | state | worktree | merged at / reason |
| --- | --- | --- | --- |
| #59 | `needs-triage` | — | keep untriaged |
| #75 | `deferred: awaiting user decision` | — | blocked |
| #27 | `merged` | — | landed |
<!-- ledger-current-ids-end -->
A stale row outside the markers:
| #999 | `queued` | — | ignore this |
EOF
printf '59\n75\n' > "$tmp/issues.txt"

expect_result matching_open_ids 0 'LEDGER OK 2 open ids' 'MISMATCH' \
    --ledger "$tmp/ledger.md" --issues-file "$tmp/issues.txt"

printf '59\n78\n' > "$tmp/mismatch-issues.txt"
expect_result mismatch_lists_both_directions 1 'LEDGER MISMATCH' 'LEDGER OK' \
    --ledger "$tmp/ledger.md" --issues-file "$tmp/mismatch-issues.txt"
assert_has missing_from_ledger 'missing from ledger: 78' "$last_output"
assert_has stale_in_ledger 'stale in ledger: 75' "$last_output"

printf '  #59  \n`75`\n' > "$tmp/prefixed-issues.txt"
expect_result prefixed_issue_ids_are_normalised 0 'LEDGER OK 2 open ids' \
    'MISMATCH' --ledger "$tmp/ledger.md" --issues-file "$tmp/prefixed-issues.txt"

printf '#27\n#59\n#75\n' > "$tmp/merged-issue.txt"
expect_result merged_rows_are_not_open 1 'missing from ledger: 27' \
    'LEDGER OK' --ledger "$tmp/ledger.md" --issues-file "$tmp/merged-issue.txt"

grep -v 'ledger-current-ids-start' "$tmp/ledger.md" > "$tmp/no-start.md"
expect_result missing_start_marker_fails 1 'malformed ledger' 'LEDGER OK' \
    --ledger "$tmp/no-start.md" --issues-file "$tmp/issues.txt"

awk '{ if ($0 ~ /ledger-current-ids-end/) print "<!-- ledger-current-ids-start -->"; print }' \
    "$tmp/ledger.md" > "$tmp/two-starts.md"
expect_result duplicate_start_marker_fails 1 'malformed ledger' 'LEDGER OK' \
    --ledger "$tmp/two-starts.md" --issues-file "$tmp/issues.txt"

if [ "$failures" -ne 0 ]; then
    printf '%s check-ledger-open-ids case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-ledger-open-ids self-test passed\n'
