#!/bin/sh
set -eu

# Self-test for the per-invocation log path in bin/test. It runs the real
# bin/test against a fake test binary in an isolated checkout, twice at once,
# so a fixed log path shows up as mixed or missing summaries.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

repo="$tmp/repo"
fakebin="$tmp/fake-bin"
mkdir -p "$repo/bin" "$repo/build/tests" "$fakebin"
cp "$ROOT/bin/test" "$ROOT/bin/check-test-summary" "$repo/bin/"
chmod +x "$repo/bin/test" "$repo/bin/check-test-summary"

printf '#!/bin/sh\nexit 0\n' > "$fakebin/qmake6"
printf '#!/bin/sh\nexit 0\n' > "$fakebin/make"
chmod +x "$fakebin/qmake6" "$fakebin/make"
printf '#!/bin/sh\nexit 0\n' > "$repo/build/omacards"
chmod +x "$repo/build/omacards"

cat > "$repo/build/tests/omacards_tests" <<'EOF'
#!/bin/sh
set -eu
mode="${1:-}"
if [ "$mode" = info ]; then
    printf 'OMACARDS_TEST_MODE: info\n'
    exit 0
fi
case "$mode" in
    run-a) suites=2; totals=2; marked=1 ;;
    run-b) suites=3; totals=3; marked=1 ;;
    fail) suites=1; totals=1; marked=0 ;;
    partial) suites=9; totals=2; marked=0 ;;
    *) exit 2 ;;
esac
if [ "$marked" -eq 1 ]; then
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

report_failure() {
    printf 'FAIL %s: %s\n' "$1" "$2"
    failures=$((failures + 1))
}

assert_status() {
    name=$1
    want=$2
    got=$3
    if [ "$got" -eq "$want" ]; then
        printf 'PASS %s\n' "$name"
    else
        report_failure "$name" "status $got, wanted $want"
    fi
}

assert_contains() {
    name=$1
    file=$2
    text=$3
    if grep -qF -- "$text" "$file"; then
        printf 'PASS %s\n' "$name"
    else
        report_failure "$name" "output did not contain $text"
    fi
}

assert_not_contains() {
    name=$1
    file=$2
    text=$3
    if grep -qF -- "$text" "$file"; then
        report_failure "$name" "output unexpectedly contained $text"
    else
        printf 'PASS %s\n' "$name"
    fi
}

# Two overlapping runs in one checkout must each see only their own results.
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
assert_contains run_a_own_marker "$tmp/out-a.log" 'MARKER: run-a'
assert_contains run_b_own_marker "$tmp/out-b.log" 'MARKER: run-b'
assert_contains run_a_own_count "$tmp/out-a.log" 'across 2 suites'
assert_contains run_b_own_count "$tmp/out-b.log" 'across 3 suites'
assert_not_contains run_a_has_no_other_marker "$tmp/out-a.log" 'MARKER: run-b'
assert_not_contains run_b_has_no_other_marker "$tmp/out-b.log" 'MARKER: run-a'

log_count=0
a_log_count=0
b_log_count=0
for log in "$repo"/build/tests/run.*; do
    [ -f "$log" ] || continue
    log_count=$((log_count + 1))
    if grep -qF 'MARKER: run-a' "$log"; then
        a_log_count=$((a_log_count + 1))
    fi
    if grep -qF 'MARKER: run-b' "$log"; then
        b_log_count=$((b_log_count + 1))
    fi
done
assert_status distinct_log_count 2 "$log_count"
assert_status one_log_per_run_a 1 "$a_log_count"
assert_status one_log_per_run_b 1 "$b_log_count"

set +e
PATH="$fakebin:$PATH" "$repo/bin/test" fail > "$tmp/out-fail.log" 2>&1
status_fail=$?
set -e
assert_status failing_suite_status 1 "$status_fail"
assert_contains failing_suite_reported "$tmp/out-fail.log" 'Failures:'

set +e
PATH="$fakebin:$PATH" "$repo/bin/test" partial > "$tmp/out-partial.log" 2>&1
status_partial=$?
set -e
assert_status partial_run_status 1 "$status_partial"
assert_contains partial_run_guard "$tmp/out-partial.log" 'expected 9'

PATH="$fakebin:$PATH" "$repo/bin/test" info > "$tmp/out-info.log" 2>&1
assert_contains info_mode_summary "$tmp/out-info.log" 'across 0 suites'

if [ "$failures" -ne 0 ]; then
    printf '%s bin/test log self-test case(s) failed\n' "$failures"
    exit 1
fi
printf 'bin/test log self-test passed\n'
