#!/bin/sh
set -eu

# Self-test for bin/check-ledger-open-ids. Fixtures drive both inputs, so the
# checker is exercised without a network call or a change to the real ledger.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-ledger-open-ids"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

expect_result() {
    name=$1
    want_status=$2
    want_text=$3
    forbidden=${4:-}
    shift 4
    set +e
    output="$("$CHECKER" "$@" 2>&1)"
    got_status=$?
    set -e
    last_output=$output
    if [ "$got_status" -ne "$want_status" ]; then
        printf 'FAIL %s: status %s, wanted %s\n%s\n' \
            "$name" "$got_status" "$want_status" "$output"
        failures=$((failures + 1))
        return
    fi
    case "$output" in
        *"$want_text"*) ;;
        *)
            printf 'FAIL %s: output did not contain %s\n%s\n' \
                "$name" "$want_text" "$output"
            failures=$((failures + 1))
            return
            ;;
    esac
    if [ -n "$forbidden" ]; then
        case "$output" in
            *"$forbidden"*)
                printf 'FAIL %s: output unexpectedly contained %s\n%s\n' \
                    "$name" "$forbidden" "$output"
                failures=$((failures + 1))
                return
                ;;
        esac
    fi
    printf 'PASS %s\n' "$name"
}

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
case "$last_output" in
    *'missing from ledger: 78'*) printf 'PASS missing_from_ledger\n' ;;
    *) printf 'FAIL missing_from_ledger\n%s\n' "$last_output"
       failures=$((failures + 1)) ;;
esac
case "$last_output" in
    *'stale in ledger: 75'*) printf 'PASS stale_in_ledger\n' ;;
    *) printf 'FAIL stale_in_ledger\n%s\n' "$last_output"
       failures=$((failures + 1)) ;;
esac

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
