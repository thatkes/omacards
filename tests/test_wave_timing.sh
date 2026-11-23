#!/bin/sh
set -eu

# Self-test for bin/wave-timing. A fixture ledger supplies the timing rows, so
# window, totals and chain waits are checked without live tracker data.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/wave-timing"
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

expect_lines() {
    name=$1
    text=$2
    case "$last_output" in
        *"$text"*) printf 'PASS %s\n' "$name" ;;
        *)
            printf 'FAIL %s: output did not contain %s\n%s\n' \
                "$name" "$text" "$last_output"
            failures=$((failures + 1))
            ;;
    esac
}

expect_absent() {
    name=$1
    text=$2
    case "$last_output" in
        *"$text"*)
            printf 'FAIL %s: output unexpectedly contained %s\n%s\n' \
                "$name" "$text" "$last_output"
            failures=$((failures + 1))
            ;;
        *) printf 'PASS %s\n' "$name" ;;
    esac
}

cat > "$tmp/ledger.md" <<'EOF'
Preamble outside the timing markers.
<!-- wave-timing-start -->
| id | chain | dispatched | finished | merged |
| --- | --- | --- | --- | --- |
| #46 | math/1 | 16:36:04 | 16:40:28 | 16:47:38 |
| #45 | math/2 | 16:53:13 | 16:59:30 | 16:59:36 |
| #44 | math/3 | 17:00:57 | 17:04:06 | 17:04:12 |
| #77 | - | 16:29:21 | 16:35:52 | 16:36:28 |
<!-- wave-timing-end -->
Rows outside the markers are ignored:
| #999 | - | 00:00:00 | 00:00:01 | 00:00:02 |
EOF

expect_result window_and_totals 0 'WAVE window 16:29:21..17:04:12 (2091s)' \
    '' --ledger "$tmp/ledger.md"
expect_lines agent_total 'AGENT total 1221s'
expect_lines merge_lag_total 'MERGE_LAG total 478s'

# Rows arrive out of chain order, so the checker must sort by chain/order
# before comparing a link with its predecessor.
expect_result chain_breach_is_reported 0 \
    'CHAIN math #45 waited 335s after #46 merged (threshold 120s)' \
    '' --ledger "$tmp/ledger.md"
expect_absent short_chain_wait_is_quiet 'CHAIN math #44'
expect_result check_mode_flags_breach 1 \
    'CHAIN math #45 waited 335s after #46 merged (threshold 120s)' \
    '' --ledger "$tmp/ledger.md" --check
expect_result raised_threshold_passes_check 0 'WAVE window 16:29:21..17:04:12' \
    'CHAIN' --ledger "$tmp/ledger.md" --check --threshold 400
expect_result lowered_threshold_reports_both_links 0 \
    'CHAIN math #44 waited 81s after #45 merged (threshold 60s)' \
    '' --ledger "$tmp/ledger.md" --threshold 60

cat > "$tmp/gaps.md" <<'EOF'
<!-- wave-timing-start -->
| id | chain | dispatched | finished | merged |
| --- | --- | --- | --- | --- |
| #1 | - | 10:00:00 | 10:00:10 | - |
| #2 | - | 10:01:00 | - | - |
<!-- wave-timing-end -->
EOF
expect_result missing_times_are_skipped 0 'WAVE window 10:00:00..10:01:00 (60s)' \
    '' --ledger "$tmp/gaps.md"
expect_lines missing_times_agent_total 'AGENT total 10s'
expect_lines missing_times_merge_lag 'MERGE_LAG total 0s'

sed 's/16:36:04/99:36:04/' "$tmp/ledger.md" > "$tmp/bad-time.md"
expect_result malformed_time_fails 1 'malformed wave timing' 'WAVE window' \
    --ledger "$tmp/bad-time.md"

grep -v 'wave-timing-start' "$tmp/ledger.md" > "$tmp/no-start.md"
expect_result missing_start_marker_fails 1 'malformed wave timing' 'WAVE window' \
    --ledger "$tmp/no-start.md"

if [ "$failures" -ne 0 ]; then
    printf '%s wave-timing case(s) failed\n' "$failures"
    exit 1
fi
printf 'wave-timing self-test passed\n'
