#!/bin/sh
set -eu

# Self-test for bin/check-test-summary. Each case writes a synthetic test log
# and asserts the checker's exit status and a distinctive part of its output.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-test-summary"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

write_announcement() {
    printf 'OMACARDS_TEST_SUITES: %s\n' "$1" > "$2"
}

write_totals() {
    count=$1
    log=$2
    i=1
    while [ "$i" -le "$count" ]; do
        printf 'Totals: 1 passed, 0 failed, 0 skipped, 0 blacklisted\n' >> "$log"
        i=$((i + 1))
    done
}

check_case() {
    name=$1
    want_status=$2
    want_text=$3
    log=$4
    set +e
    output="$("$CHECKER" "$log" 2>&1)"
    got_status=$?
    set -e
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
    printf 'PASS %s\n' "$name"
}

write_announcement 17 "$tmp/full.log"
write_totals 17 "$tmp/full.log"
check_case full_run_passes 0 'across 17 suites' "$tmp/full.log"

write_announcement 17 "$tmp/partial.log"
write_totals 6 "$tmp/partial.log"
check_case partial_run_fails 1 'expected 17' "$tmp/partial.log"

write_totals 17 "$tmp/no-marker.log"
check_case missing_marker_fails 1 'OMACARDS_TEST_SUITES' "$tmp/no-marker.log"

write_announcement 1 "$tmp/focused.log"
write_totals 1 "$tmp/focused.log"
check_case focused_run_passes 0 'across 1 suites' "$tmp/focused.log"

write_announcement 17 "$tmp/failing.log"
write_totals 17 "$tmp/failing.log"
printf 'FAIL! TestCli::frozenContract\n' >> "$tmp/failing.log"
check_case reported_failure_fails 1 'Failures:' "$tmp/failing.log"

write_announcement 1 "$tmp/totals-failure.log"
printf 'Totals: 0 passed, 1 failed, 0 skipped, 0 blacklisted\n' >> "$tmp/totals-failure.log"
check_case totals_failure_fails 1 '1 failed tests' "$tmp/totals-failure.log"

printf 'OMACARDS_TEST_MODE: info\n' > "$tmp/info.log"
check_case informational_mode_passes 0 'across 0 suites' "$tmp/info.log"

write_announcement 17 "$tmp/info-marker-mix.log"
printf 'OMACARDS_TEST_MODE: info\n' >> "$tmp/info-marker-mix.log"
check_case info_marker_with_announcement_fails 1 'conflicts' "$tmp/info-marker-mix.log"

printf 'OMACARDS_TEST_MODE: info\n' > "$tmp/info-totals-mix.log"
write_totals 17 "$tmp/info-totals-mix.log"
check_case info_marker_with_totals_fails 1 'conflicts' "$tmp/info-totals-mix.log"

if [ "$failures" -ne 0 ]; then
    printf '%s check-test-summary case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-test-summary self-test passed\n'
