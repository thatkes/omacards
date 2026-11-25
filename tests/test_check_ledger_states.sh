#!/bin/sh
set -eu

# Self-test for bin/check-ledger-states. Fixtures drive the only input, so the
# checker is exercised without a network call or a change to the real ledger.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-ledger-states"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

write_ledger() {
    target=$1
    shift
    {
        printf '%s\n' 'Preamble outside the markers.'
        printf '%s\n' '<!-- ledger-current-ids-start -->'
        printf '%s\n' '| id | state | worktree | merged at / reason |'
        printf '%s\n' '| --- | --- | --- | --- |'
        for row do
            printf '%s\n' "$row"
        done
        printf '%s\n' '<!-- ledger-current-ids-end -->'
        printf '%s\n' 'Outside: | #999 | `deferred` | — | ignored |'
    } > "$target"
}

write_ledger "$tmp/canonical.md" \
    '| #1 | `queued` | — | next |' \
    '| #2 | `running` | ticket-2 | in flight |' \
    '| #3 | `deferred: awaiting recon` | — | waiting on recon |' \
    '| #4 | `deferred: complicated` | — | blocked by another edit |' \
    '| #5 | `deferred: user decision` | — | await maintainer |' \
    '| #6 | `needs-triage` | — | fresh issue |' \
    '| #7 | `merged` | — | landed |'

expect_result all_canonical_states_pass 0 'LEDGER STATES OK' 'ERROR' \
    --ledger "$tmp/canonical.md"

write_ledger "$tmp/bare.md" '| #42 | `deferred` | — | blocked |'
expect_result bare_deferred_fails 1 "ERROR #42: invalid state cell 'deferred'" \
    'LEDGER STATES OK' --ledger "$tmp/bare.md"

noncanonical=0
for value in 'deferred: out of wave 5' 'open: blocked by #173' \
    'deferred: blocked by #180' 'deferred: blocked by #190'; do
    noncanonical=$((noncanonical + 1))
    write_ledger "$tmp/noncanonical-$noncanonical.md" \
        "| #42 | \`$value\` | — | blocked |"
    expect_result "noncanonical_${noncanonical}_fails" 1 \
        "ERROR #42: invalid state cell '$value'" 'LEDGER STATES OK' \
        --ledger "$tmp/noncanonical-$noncanonical.md"
done

write_ledger "$tmp/empty-reason.md" \
    '| #42 | `deferred: complicated` | — |  |'
expect_result empty_reason_fails 1 \
    "ERROR #42: missing reason for 'deferred: complicated'" \
    'LEDGER STATES OK' --ledger "$tmp/empty-reason.md"

write_ledger "$tmp/dash-reason.md" \
    '| #42 | `deferred: complicated` | — | — |'
expect_result dash_reason_fails 1 \
    "ERROR #42: missing reason for 'deferred: complicated'" \
    'LEDGER STATES OK' --ledger "$tmp/dash-reason.md"

write_ledger "$tmp/reason.md" \
    '| #42 | `deferred: complicated` | — | blocked by #190; edits overlap |'
expect_result deferred_with_reason_passes 0 'LEDGER STATES OK' 'ERROR' \
    --ledger "$tmp/reason.md"

grep -v 'ledger-current-ids-start' "$tmp/canonical.md" > "$tmp/no-start.md"
expect_result missing_start_marker_fails 1 'malformed ledger current-id section' \
    'LEDGER STATES OK' --ledger "$tmp/no-start.md"

grep -v 'ledger-current-ids-end' "$tmp/canonical.md" > "$tmp/no-end.md"
expect_result missing_end_marker_fails 1 'malformed ledger current-id section' \
    'LEDGER STATES OK' --ledger "$tmp/no-end.md"

awk '{ if ($0 ~ /ledger-current-ids-start/) print; print }' \
    "$tmp/canonical.md" > "$tmp/two-starts.md"
expect_result duplicate_start_marker_fails 1 'malformed ledger current-id section' \
    'LEDGER STATES OK' --ledger "$tmp/two-starts.md"

awk '{ if ($0 ~ /ledger-current-ids-end/) print; print }' \
    "$tmp/canonical.md" > "$tmp/two-ends.md"
expect_result duplicate_end_marker_fails 1 'malformed ledger current-id section' \
    'LEDGER STATES OK' --ledger "$tmp/two-ends.md"

write_ledger "$tmp/bad-id.md" '| #42x | `queued` | — | next |'
expect_result non_numeric_id_fails 1 "ERROR #42x: invalid id cell '#42x'" \
    'LEDGER STATES OK' --ledger "$tmp/bad-id.md"

if [ "$failures" -ne 0 ]; then
    printf '%s check-ledger-states case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-ledger-states self-test passed\n'
