#!/bin/sh
set -eu

# Self-test for bin/check-scaling. Timings are injected at the guard's input
# boundary, so the ratio arithmetic, both limits and the pass/fail boundary
# run without building the app, writing synthetic Decks or spawning a CLI.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-scaling"
. "$ROOT/tests/checker_test_support.sh"

failures=0

run_checker() {
    rc_check_ms=$1
    rc_decks_ms=$2
    rc_small=${3:-50}
    rc_large=${4:-200}
    OMACARDS_SCALING_SMALL="$rc_small" \
    OMACARDS_SCALING_LARGE="$rc_large" \
    OMACARDS_SCALING_INJECT_CHECK="$rc_check_ms" \
    OMACARDS_SCALING_INJECT_DECKS="$rc_decks_ms" \
        "$CHECKER" 2>&1
}

set +e
pass_output=$(run_checker '100,800' '100,600')
pass_status=$?
set -e
expect_result both_limits_at_boundary_pass 0 'Scaling OK' \
    "$pass_status" "$pass_output"
assert_has pass_reports_check_timing '100 ms -> 800 ms (8.00x, limit 8x)' \
    "$pass_output"
assert_has pass_reports_decks_timing '100 ms -> 600 ms (6.00x, limit 6x)' \
    "$pass_output"
assert_has pass_reports_both_commands 'Scaling OK: check --json' "$pass_output"

set +e
check_output=$(run_checker '100,801' '100,600')
check_status=$?
set -e
expect_result check_above_8x_fails 1 'check --json scaling regression' \
    "$check_status" "$check_output"
assert_has check_failure_reports_ratio '100 ms -> 801 ms (8.01x, limit 8x)' \
    "$check_output"
assert_lacks check_failure_has_no_success_line 'Scaling OK' "$check_output"

set +e
decks_output=$(run_checker '100,800' '100,601')
decks_status=$?
set -e
expect_result decks_above_6x_fails 1 'decks --json scaling regression' \
    "$decks_status" "$decks_output"
assert_has decks_failure_reports_ratio '100 ms -> 601 ms (6.01x, limit 6x)' \
    "$decks_output"
assert_lacks decks_failure_has_no_success_line 'Scaling OK' "$decks_output"

set +e
zero_output=$(run_checker '100,800' '100,600' 0 200)
zero_status=$?
set -e
expect_result zero_deck_size_rejected 2 \
    'Scaling Deck sizes must be positive integers' "$zero_status" "$zero_output"

set +e
nonnumeric_output=$(run_checker '100,800' '100,600' 50 abc)
nonnumeric_status=$?
set -e
expect_result nonnumeric_deck_size_rejected 2 \
    'Scaling Deck sizes must be positive integers' \
    "$nonnumeric_status" "$nonnumeric_output"

set +e
unordered_output=$(run_checker '100,800' '100,600' 200 200)
unordered_status=$?
set -e
expect_result large_not_greater_rejected 2 \
    'OMACARDS_SCALING_LARGE must exceed OMACARDS_SCALING_SMALL' \
    "$unordered_status" "$unordered_output"

if [ "$failures" -ne 0 ]; then
    printf '%s check-scaling case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-scaling self-test passed\n'
