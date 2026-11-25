#!/bin/sh
set -eu

# Self-test for the per-invocation log path in bin/test. It runs the real
# bin/test against a fake test binary in an isolated checkout. Five invocations
# share one fixture: two overlapping plain runs prove that concurrent runs keep
# their own group logs and environments, one of them carries --timing, one
# failing partial run covers both the failure report and the partial-run guard,
# one info-mode run covers the zero-suite summary, and one focused CLI run
# covers a source edit reaching the rebuilt app binary. Running them together
# keeps the log-shape assertions while cutting the runner startup cost.
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
# Several invocations build at once, so the stamp is swapped into place.
cat > "$fakebin/make" <<EOF
#!/bin/sh
set -eu
if [ "\$PWD" = "$repo/build" ]; then
    stamp="$repo/build/omacards.\$\$"
    {
        printf '#!/bin/sh\\n'
        printf '# app source: '
        cat "$repo/src/cli.cpp"
        printf 'exit 0\\n'
    } > "\$stamp"
    chmod +x "\$stamp"
    mv -f "\$stamp" "$repo/build/omacards"
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
        run-a|run-b|fail-partial|info|edited) mode=$arg ;;
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
    TestCli|TestBackend|TestDynamic) suites=$suite_count totals=$suite_count marked=1 ;;
esac
case "$mode" in
    run-a|run-b|edited) ;;
    # Each of the three groups announces six suites, so the combined plan
    # stays at 18 while the Totals lines show a truncated six-suite run.
    fail-partial) suites=6; totals=2; marked=0 ;;
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
if [ "$mode" = fail-partial ]; then
    printf 'FAIL! Fake::case\n'
    exit 1
fi
EOF
chmod +x "$repo/build/tests/omacards_tests"

failures=0

# The focused run below doubles as the rebuild case, so the edit is in place
# before any invocation starts. The fake make still stamps the binary from the
# source, so a runner that skips the app build leaves an unmarked binary.
printf 'cli-source-edited\n' > "$repo/src/cli.cpp"

# Two overlapping plain runs keep only their own results. --timing rides on
# run-a, while the remaining behaviours each need their own runner state.
PATH="$fakebin:$PATH" "$repo/bin/test" --timing run-a > "$tmp/out-a.log" 2>&1 &
pid_a=$!
PATH="$fakebin:$PATH" "$repo/bin/test" run-b > "$tmp/out-b.log" 2>&1 &
pid_b=$!
PATH="$fakebin:$PATH" "$repo/bin/test" fail-partial > "$tmp/out-fail.log" 2>&1 &
pid_fail=$!
PATH="$fakebin:$PATH" "$repo/bin/test" info > "$tmp/out-info.log" 2>&1 &
pid_info=$!
PATH="$fakebin:$PATH" "$repo/bin/test" --suite TestCli edited > "$tmp/out-rebuild.log" 2>&1 &
pid_rebuild=$!

set +e
wait "$pid_a"
status_a=$?
wait "$pid_b"
status_b=$?
wait "$pid_fail"
status_fail=$?
wait "$pid_info"
status_info=$?
wait "$pid_rebuild"
status_rebuild=$?
set -e

assert_status concurrent_run_a_status 0 "$status_a"
assert_status concurrent_run_b_status 0 "$status_b"
assert_file_has_text run_a_own_marker "$tmp/out-a.log" 'MARKER: run-a'
assert_file_has_text run_b_own_marker "$tmp/out-b.log" 'MARKER: run-b'
assert_file_lacks_text run_a_has_no_other_marker "$tmp/out-a.log" 'MARKER: run-b'
assert_file_lacks_text run_b_has_no_other_marker "$tmp/out-b.log" 'MARKER: run-a'
assert_equal run_a_has_three_groups "$(count_matches "$tmp/out-a.log" 'GROUP: ')" 3
assert_equal run_b_has_three_groups "$(count_matches "$tmp/out-b.log" 'GROUP: ')" 3
assert_file_has_text run_a_has_cli_group "$tmp/out-a.log" 'GROUP: TestCli'
assert_file_has_text run_a_has_backend_group "$tmp/out-a.log" 'GROUP: TestBackend'
assert_file_has_text run_a_has_other_group "$tmp/out-a.log" 'GROUP: TestDynamic'
assert_file_has_text run_a_uses_binary_suite_list "$tmp/out-a.log" 'ALL_SUITES: TestDynamic'
assert_file_has_text run_a_aggregated_announcement "$tmp/out-a.log" 'OMACARDS_TEST_SUITES: 3'
assert_file_has_text run_a_combined_summary "$tmp/out-a.log" 'across 3 suites'
assert_file_has_text run_b_combined_summary "$tmp/out-b.log" 'across 3 suites'

# --timing is observational: one phase per build/guard/group, one line per
# registered suite, and the same three-group shape as a plain run. The flag is
# stripped before the test binary sees the arguments. It rides on run-a so the
# timing log is also the plain-run log above.
assert_equal timing_has_three_groups \
    "$(count_matches "$tmp/out-a.log" 'GROUP: ')" 3
assert_file_has_text timing_has_cli_group "$tmp/out-a.log" 'GROUP: TestCli'
assert_file_has_text timing_has_backend_group "$tmp/out-a.log" 'GROUP: TestBackend'
assert_file_has_text timing_has_other_group "$tmp/out-a.log" 'GROUP: TestDynamic'
assert_equal timing_has_six_phases \
    "$(count_matches "$tmp/out-a.log" 'TIMING phase ')" 6
assert_file_has_text timing_phase_app "$tmp/out-a.log" \
    'TIMING phase app-build: '
assert_file_has_text timing_phase_scaling "$tmp/out-a.log" \
    'TIMING phase scaling-guard: '
assert_file_has_text timing_phase_test_build "$tmp/out-a.log" \
    'TIMING phase test-binary-build: '
assert_file_has_text timing_phase_cli_group "$tmp/out-a.log" \
    'TIMING phase cli-group: '
assert_file_has_text timing_phase_backend_group "$tmp/out-a.log" \
    'TIMING phase backend-group: '
assert_file_has_text timing_phase_other_group "$tmp/out-a.log" \
    'TIMING phase other-group: '
assert_equal timing_has_three_suite_lines \
    "$(count_matches "$tmp/out-a.log" 'TIMING suite ')" 3
assert_file_has_text timing_suite_cli "$tmp/out-a.log" \
    'TIMING suite TestCli: 25 ms'
assert_file_has_text timing_suite_backend "$tmp/out-a.log" \
    'TIMING suite TestBackend: 25 ms'
assert_file_has_text timing_suite_dynamic "$tmp/out-a.log" \
    'TIMING suite TestDynamic: 25 ms'
assert_file_lacks_text timing_flag_stripped "$tmp/out-a.log" '--timing'
assert_file_has_text timing_keeps_combined_summary "$tmp/out-a.log" \
    'across 3 suites'
assert_status timing_status 0 "$status_a"

# A failing partial run produces both the failure report and the guard error
# in one log: FAIL! sets the status, and six Totals against an announced 18
# trips the partial-run guard.
assert_status failing_suite_status 1 "$status_fail"
assert_file_has_text failing_suite_reported "$tmp/out-fail.log" 'Failures:'
assert_status partial_run_status 1 "$status_fail"
assert_file_has_text partial_run_guard "$tmp/out-fail.log" 'expected 18'

assert_status info_mode_status 0 "$status_info"
assert_equal info_has_three_groups "$(count_matches "$tmp/out-info.log" 'GROUP: ')" 3
assert_file_has_text info_mode_summary "$tmp/out-info.log" 'across 0 suites'

# A CLI source edit must reach the app binary the CLI tests spawn. The focused
# run doubles as the rebuild case: one group, the smaller combined count, and
# the edited source stamp from the app build.
assert_status focused_status 0 "$status_rebuild"
assert_equal focused_has_one_group \
    "$(count_matches "$tmp/out-rebuild.log" 'GROUP: ')" 1
assert_file_has_text focused_keeps_smaller_count "$tmp/out-rebuild.log" \
    'across 1 suites'
assert_status rebuild_after_cli_edit_status 0 "$status_rebuild"
assert_file_has_text rebuild_runs_app_build "$tmp/out-rebuild.log" \
    "Built $repo/build/omacards"
assert_file_has_text rebuild_cli_tests_see_edited_source "$tmp/out-rebuild.log" \
    'APP_SOURCE: # app source: cli-source-edited'

# Other concurrent invocations also leave run.* and group.* files, so scope the
# isolation assertions to the two overlapping plain runs.
combined_log_count=0
for log in "$repo"/build/tests/run.*; do
    [ -f "$log" ] || continue
    if grep -qF 'MARKER: run-a' "$log" || grep -qF 'MARKER: run-b' "$log"; then
        combined_log_count=$((combined_log_count + 1))
    fi
done
assert_status distinct_combined_log_count 2 "$combined_log_count"

a_group_logs=0
b_group_logs=0
mixed_group_logs=0
group_log_count=0
: > "$tmp/isolated-group-logs"
for log in "$repo"/build/tests/group.*; do
    [ -f "$log" ] || continue
    a_log=0
    b_log=0
    if grep -qF 'MARKER: run-a' "$log"; then a_log=1; fi
    if grep -qF 'MARKER: run-b' "$log"; then b_log=1; fi
    if [ "$a_log" -eq 0 ] && [ "$b_log" -eq 0 ]; then continue; fi
    printf '%s\n' "$log" >> "$tmp/isolated-group-logs"
    group_log_count=$((group_log_count + 1))
    if [ "$a_log" -eq 1 ]; then
        a_group_logs=$((a_group_logs + 1))
    fi
    if [ "$b_log" -eq 1 ]; then
        b_group_logs=$((b_group_logs + 1))
    fi
    if [ "$a_log" -eq 1 ] && [ "$b_log" -eq 1 ]; then
        mixed_group_logs=$((mixed_group_logs + 1))
    fi
done
assert_status six_group_logs 6 "$group_log_count"
assert_status three_group_logs_for_run_a 3 "$a_group_logs"
assert_status three_group_logs_for_run_b 3 "$b_group_logs"
assert_status no_mixed_group_logs 0 "$mixed_group_logs"

count_env_variants() {
    cev_file="$tmp/env-$1.txt"
    : > "$cev_file"
    while IFS= read -r log; do
        grep -hoE "(^| )$1=[^ ]*" "$log" >> "$cev_file" || true
    done < "$tmp/isolated-group-logs"
    sort -u "$cev_file" | wc -l | tr -d '[:space:]'
}

for key in HOME OMACARDS_STATE_DB TMPDIR; do
    assert_status "distinct_group_$key" 6 "$(count_env_variants "$key")"
done
cache_variants=$(count_env_variants XDG_CACHE_HOME)
assert_status shared_warm_xdg_cache 1 "$cache_variants"

if [ "$failures" -ne 0 ]; then
    printf '%s bin/test log self-test case(s) failed\n' "$failures"
    exit 1
fi
printf 'bin/test log self-test passed\n'
