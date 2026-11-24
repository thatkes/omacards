#!/bin/sh
set -eu

# Self-test for bin/wave-timing. A fixture ledger supplies the timing rows, so
# window, totals and chain waits are checked without live tracker data.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/wave-timing"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

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
assert_has agent_total 'AGENT total 1221s' "$last_output"
assert_has merge_lag_total 'MERGE_LAG total 478s' "$last_output"

# Rows arrive out of chain order, so the checker must sort by chain/order
# before comparing a link with its predecessor.
expect_result chain_breach_is_reported 0 \
    'CHAIN math #45 waited 335s after #46 merged (threshold 120s)' \
    '' --ledger "$tmp/ledger.md"
assert_lacks short_chain_wait_is_quiet 'CHAIN math #44' "$last_output"
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
assert_has missing_times_agent_total 'AGENT total 10s' "$last_output"
assert_has missing_times_merge_lag 'MERGE_LAG total 0s' "$last_output"

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
