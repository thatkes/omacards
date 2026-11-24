#!/bin/sh
set -eu

# Self-test for the per-invocation log path in bin/test. It runs the real
# bin/test against a fake test binary in an isolated checkout, twice at once,
# so a fixed log path shows up as mixed or missing summaries. A plain run now
# splits the suites into two concurrent groups; each group gets its own log and
# environment, and the runner presents one combined summary.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# Keep the scaling guard's synthetic Deck tiny; this suite exercises the log
# path, not CLI timing.
OMACARDS_SCALING_SMALL=20
OMACARDS_SCALING_LARGE=80
export OMACARDS_SCALING_SMALL OMACARDS_SCALING_LARGE

repo="$tmp/repo"
fakebin="$tmp/fake-bin"
mkdir -p "$repo/bin" "$repo/build/tests" "$fakebin"
cp "$ROOT/bin/test" "$ROOT/bin/check-test-summary" "$ROOT/bin/check-scaling" "$repo/bin/"
chmod +x "$repo/bin/test" "$repo/bin/check-test-summary" "$repo/bin/check-scaling"

printf '#!/bin/sh\nexit 0\n' > "$fakebin/qmake6"
printf '#!/bin/sh\nexit 0\n' > "$fakebin/make"
chmod +x "$fakebin/qmake6" "$fakebin/make"
printf '#!/bin/sh\nexit 0\n' > "$repo/build/omacards"
chmod +x "$repo/build/omacards"

cat > "$repo/build/tests/omacards_tests" <<'EOF'
#!/bin/sh
set -eu
mode=
group=
all_suites=
for arg in "$@"; do
    if [ "$arg" = "--list-suites" ]; then
        printf 'OMACARDS_TEST_MODE: info\n'
        printf 'OMACARDS_TEST_SUITE: %s\n' TestCli TestBackend TestDynamic
        exit 0
    fi
    case "$arg" in
        run-a|run-b|fail|partial|info) mode=$arg ;;
        Test*)
            all_suites="$all_suites $arg"
            if [ -z "$group" ]; then group=$arg; fi
            ;;
    esac
done
printf 'GROUP: %s\n' "$group"
printf 'ALL_SUITES:%s\n' "$all_suites"
printf 'ENV: HOME=%s XDG_CACHE_HOME=%s OMACARDS_STATE_DB=%s TMPDIR=%s\n' \
    "${HOME:-unset}" "${XDG_CACHE_HOME:-unset}" "${OMACARDS_STATE_DB:-unset}" "${TMPDIR:-unset}"
if [ "$mode" = info ]; then
    printf 'OMACARDS_TEST_MODE: info\n'
    exit 0
fi
case "$group" in
    TestCli) suites=2; totals=2; marked=1 ;;
    TestBackend) suites=3; totals=3; marked=1 ;;
esac
case "$mode" in
    run-a|run-b) ;;
    fail) suites=1; totals=1; marked=0 ;;
    partial) suites=9; totals=2; marked=0 ;;
    *) exit 2 ;;
esac
if [ "${marked:-0}" -eq 1 ]; then
    sleep 0.2
    printf 'MARKER: %s\n' "$mode"
fi
printf 'OMACARDS_TEST_SUITES: %s\n' "$suites"
i=1
while [ "$i" -le "$totals" ]; do
    printf 'Totals: 1 passed, 0 failed, 0 skipped, 0 blacklisted\n'
    i=$((i + 1))
done
if [ "$mode" = fail ]; then
    printf 'FAIL! Fake::case\n'
    exit 1
fi
EOF
chmod +x "$repo/build/tests/omacards_tests"

failures=0

# Two overlapping runs in one checkout must each see only their own results,
# and a plain run must show both of its groups and their combined summary.
PATH="$fakebin:$PATH" "$repo/bin/test" run-a > "$tmp/out-a.log" 2>&1 &
pid_a=$!
PATH="$fakebin:$PATH" "$repo/bin/test" run-b > "$tmp/out-b.log" 2>&1 &
pid_b=$!
set +e
wait "$pid_a"
status_a=$?
wait "$pid_b"
status_b=$?
set -e

assert_status concurrent_run_a_status 0 "$status_a"
assert_status concurrent_run_b_status 0 "$status_b"
assert_file_has_text run_a_own_marker "$tmp/out-a.log" 'MARKER: run-a'
assert_file_has_text run_b_own_marker "$tmp/out-b.log" 'MARKER: run-b'
assert_file_lacks_text run_a_has_no_other_marker "$tmp/out-a.log" 'MARKER: run-b'
assert_file_lacks_text run_b_has_no_other_marker "$tmp/out-b.log" 'MARKER: run-a'
assert_equal run_a_has_two_groups "$(count_matches "$tmp/out-a.log" 'GROUP: ')" 2
assert_equal run_b_has_two_groups "$(count_matches "$tmp/out-b.log" 'GROUP: ')" 2
assert_file_has_text run_a_has_cli_group "$tmp/out-a.log" 'GROUP: TestCli'
assert_file_has_text run_a_has_backend_group "$tmp/out-a.log" 'GROUP: TestBackend'
assert_file_has_text run_a_uses_binary_suite_list "$tmp/out-a.log" 'ALL_SUITES: TestBackend TestDynamic'
assert_file_has_text run_a_aggregated_announcement "$tmp/out-a.log" 'OMACARDS_TEST_SUITES: 5'
assert_file_has_text run_a_combined_summary "$tmp/out-a.log" 'across 5 suites'
assert_file_has_text run_b_combined_summary "$tmp/out-b.log" 'across 5 suites'

combined_log_count=0
for log in "$repo"/build/tests/run.*; do
    [ -f "$log" ] || continue
    combined_log_count=$((combined_log_count + 1))
done
assert_status distinct_combined_log_count 2 "$combined_log_count"

a_group_logs=0
b_group_logs=0
mixed_group_logs=0
group_log_count=0
for log in "$repo"/build/tests/group.*; do
    [ -f "$log" ] || continue
    group_log_count=$((group_log_count + 1))
    if grep -qF 'MARKER: run-a' "$log"; then
        a_group_logs=$((a_group_logs + 1))
    fi
    if grep -qF 'MARKER: run-b' "$log"; then
        b_group_logs=$((b_group_logs + 1))
    fi
    if grep -qF 'MARKER: run-a' "$log" && grep -qF 'MARKER: run-b' "$log"; then
        mixed_group_logs=$((mixed_group_logs + 1))
    fi
done
assert_status four_group_logs 4 "$group_log_count"
assert_status two_group_logs_for_run_a 2 "$a_group_logs"
assert_status two_group_logs_for_run_b 2 "$b_group_logs"
assert_status no_mixed_group_logs 0 "$mixed_group_logs"

for key in HOME OMACARDS_STATE_DB TMPDIR; do
    variants=$(grep -hoE "(^| )${key}=[^ ]*" "$repo"/build/tests/group.* | sort -u | wc -l)
    assert_status "distinct_group_$key" 4 "$variants"
done
cache_variants=$(grep -hoE '(^| )XDG_CACHE_HOME=[^ ]*' "$repo"/build/tests/group.* | sort -u | wc -l)
assert_status shared_warm_xdg_cache 1 "$cache_variants"

set +e
PATH="$fakebin:$PATH" "$repo/bin/test" --suite TestCli run-a > "$tmp/out-focused.log" 2>&1
status_focused=$?
set -e
assert_status focused_status 0 "$status_focused"
assert_equal focused_has_one_group "$(count_matches "$tmp/out-focused.log" 'GROUP: ')" 1
assert_file_has_text focused_keeps_smaller_count "$tmp/out-focused.log" 'across 2 suites'

set +e
PATH="$fakebin:$PATH" "$repo/bin/test" fail > "$tmp/out-fail.log" 2>&1
status_fail=$?
set -e
assert_status failing_suite_status 1 "$status_fail"
assert_file_has_text failing_suite_reported "$tmp/out-fail.log" 'Failures:'

set +e
PATH="$fakebin:$PATH" "$repo/bin/test" partial > "$tmp/out-partial.log" 2>&1
status_partial=$?
set -e
assert_status partial_run_status 1 "$status_partial"
assert_file_has_text partial_run_guard "$tmp/out-partial.log" 'expected 18'

PATH="$fakebin:$PATH" "$repo/bin/test" info > "$tmp/out-info.log" 2>&1
assert_equal info_has_two_groups "$(count_matches "$tmp/out-info.log" 'GROUP: ')" 2
assert_file_has_text info_mode_summary "$tmp/out-info.log" 'across 0 suites'

if [ "$failures" -ne 0 ]; then
    printf '%s bin/test log self-test case(s) failed\n' "$failures"
    exit 1
fi
printf 'bin/test log self-test passed\n'
