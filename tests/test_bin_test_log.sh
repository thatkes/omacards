#!/bin/sh
set -eu

# Self-test for the per-invocation log path in bin/test. It runs the real
# bin/test against a fake test binary in an isolated checkout, twice at once,
# so a fixed log path shows up as mixed or missing summaries. A plain run now
# splits the suites into two concurrent groups; each group gets its own log and
# environment, and the runner presents one combined summary. A CLI source edit
# is checked against the app binary the fake CLI tests spawn. A --timing run
# must strip the flag before the test binary sees it and name each phase and
# registered suite without turning any elapsed value into a gate.
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
mkdir -p "$repo/bin" "$repo/build/tests" "$repo/src" "$fakebin"
cp "$ROOT/bin/test" "$ROOT/bin/build" "$ROOT/bin/check-test-summary" \
    "$ROOT/bin/check-scaling" "$repo/bin/"
chmod +x "$repo/bin/test" "$repo/bin/build" "$repo/bin/check-test-summary" \
    "$repo/bin/check-scaling"

printf '#!/bin/sh\nexit 0\n' > "$fakebin/qmake6"
# The app build stamps the binary with the current CLI source; the test build
# leaves it alone. A stale binary therefore shows up as stale source below.
cat > "$fakebin/make" <<EOF
#!/bin/sh
set -eu
if [ "\$PWD" = "$repo/build" ]; then
    {
        printf '#!/bin/sh\\n'
        printf '# app source: '
        cat "$repo/src/cli.cpp"
        printf 'exit 0\\n'
    } > "$repo/build/omacards"
    chmod +x "$repo/build/omacards"
fi
exit 0
EOF
chmod +x "$fakebin/qmake6" "$fakebin/make"
printf 'cli-source-original\n' > "$repo/src/cli.cpp"
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
        run-a|run-b|fail|partial|info|edited|timing) mode=$arg ;;
        Test*)
            all_suites="$all_suites $arg"
            if [ -z "$group" ]; then group=$arg; fi
            ;;
    esac
done
printf 'GROUP: %s\n' "$group"
printf 'ALL_SUITES:%s\n' "$all_suites"
printf 'ARGS:'
for arg in "$@"; do printf ' %s' "$arg"; done
printf '\n'
printf 'ENV: HOME=%s XDG_CACHE_HOME=%s OMACARDS_STATE_DB=%s TMPDIR=%s\n' \
    "${HOME:-unset}" "${XDG_CACHE_HOME:-unset}" "${OMACARDS_STATE_DB:-unset}" "${TMPDIR:-unset}"
printf 'APP_SOURCE: %s\n' "$(sed -n '2p' "$(dirname -- "$0")/../omacards")"
if [ "$mode" = info ]; then
    printf 'OMACARDS_TEST_MODE: info\n'
    exit 0
fi
suite_count=0
for suite in $all_suites; do
    suite_count=$((suite_count + 1))
done
case "$group" in
    TestCli|TestBackend) suites=$suite_count totals=$suite_count marked=1 ;;
esac
case "$mode" in
    run-a|run-b|edited|timing) ;;
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
    printf 'Totals: 1 passed, 0 failed, 0 skipped, 0 blacklisted, 25ms\n'
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
assert_file_has_text run_a_aggregated_announcement "$tmp/out-a.log" 'OMACARDS_TEST_SUITES: 3'
assert_file_has_text run_a_combined_summary "$tmp/out-a.log" 'across 3 suites'
assert_file_has_text run_b_combined_summary "$tmp/out-b.log" 'across 3 suites'

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
assert_file_has_text focused_keeps_smaller_count "$tmp/out-focused.log" 'across 1 suites'

# --timing is observational: five phase lines and one line per registered
# suite, with the flag stripped before the test binary sees the arguments.
set +e
PATH="$fakebin:$PATH" "$repo/bin/test" --timing timing > "$tmp/out-timing.log" 2>&1
status_timing=$?
set -e
assert_status timing_status 0 "$status_timing"
assert_equal timing_has_five_phases \
    "$(count_matches "$tmp/out-timing.log" 'TIMING phase ')" 5
assert_file_has_text timing_phase_app "$tmp/out-timing.log" \
    'TIMING phase app-build: '
assert_file_has_text timing_phase_scaling "$tmp/out-timing.log" \
    'TIMING phase scaling-guard: '
assert_file_has_text timing_phase_test_build "$tmp/out-timing.log" \
    'TIMING phase test-binary-build: '
assert_file_has_text timing_phase_cli_group "$tmp/out-timing.log" \
    'TIMING phase cli-group: '
assert_file_has_text timing_phase_other_group "$tmp/out-timing.log" \
    'TIMING phase other-group: '
assert_equal timing_has_three_suite_lines \
    "$(count_matches "$tmp/out-timing.log" 'TIMING suite ')" 3
assert_file_has_text timing_suite_cli "$tmp/out-timing.log" \
    'TIMING suite TestCli: 25 ms'
assert_file_has_text timing_suite_backend "$tmp/out-timing.log" \
    'TIMING suite TestBackend: 25 ms'
assert_file_has_text timing_suite_dynamic "$tmp/out-timing.log" \
    'TIMING suite TestDynamic: 25 ms'
assert_file_lacks_text timing_flag_stripped "$tmp/out-timing.log" '--timing'
assert_file_has_text timing_keeps_combined_summary "$tmp/out-timing.log" \
    'across 3 suites'

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

# A CLI source edit must reach the app binary the CLI tests spawn. The fake
# make stamps it only during the app build, so a runner that skips that build
# leaves the tests looking at the previous source.
printf 'cli-source-edited\n' > "$repo/src/cli.cpp"
set +e
PATH="$fakebin:$PATH" "$repo/bin/test" --suite TestCli edited > "$tmp/out-rebuild.log" 2>&1
status_rebuild=$?
set -e
assert_status rebuild_after_cli_edit_status 0 "$status_rebuild"
assert_file_has_text rebuild_runs_app_build "$tmp/out-rebuild.log" "Built $repo/build/omacards"
assert_file_has_text rebuild_cli_tests_see_edited_source "$tmp/out-rebuild.log" \
    'APP_SOURCE: # app source: cli-source-edited'

if [ "$failures" -ne 0 ]; then
    printf '%s bin/test log self-test case(s) failed\n' "$failures"
    exit 1
fi
printf 'bin/test log self-test passed\n'
