#!/bin/sh
set -eu

# Self-test for bin/check-lint-coverage. Each fixture has a small lint runner;
# the only difference between pass and fail is whether a new guard is wired in.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-lint-coverage"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

populate_root() {
    root=$1
    mkdir -p "$root/bin" "$root/tests"
    printf '#!/bin/sh\nexit 0\n' > "$root/bin/check-sample"
    printf '#!/bin/sh\nexit 0\n' > "$root/bin/check-test-summary"
    printf '#!/bin/sh\nexit 0\n' > "$root/tests/test_check_sample.sh"
    chmod +x "$root/bin/check-sample" "$root/bin/check-test-summary" \
        "$root/tests/test_check_sample.sh"
    cat > "$root/bin/lint" <<'EOF'
#!/bin/sh
./bin/check-sample
./tests/test_check_sample.sh
./bin/check-lint-coverage
EOF
}

covered="$tmp/covered"
make_root "$covered"
check_case covered_checkers_pass 0 '' "$covered"

uncalled_checker="$tmp/uncalled-checker"
make_root "$uncalled_checker"
printf '#!/bin/sh\nexit 0\n' > "$uncalled_checker/bin/check-fresh"
chmod +x "$uncalled_checker/bin/check-fresh"
check_case new_checker_must_be_called 1 \
    'bin/lint does not call bin/check-fresh' "$uncalled_checker"

uncalled_test="$tmp/uncalled-test"
make_root "$uncalled_test"
printf '#!/bin/sh\nexit 0\n' > "$uncalled_test/tests/test_check_fresh.sh"
chmod +x "$uncalled_test/tests/test_check_fresh.sh"
check_case new_self_test_must_be_called 1 \
    'bin/lint does not call tests/test_check_fresh.sh' "$uncalled_test"

wired="$tmp/wired"
make_root "$wired"
printf '#!/bin/sh\nexit 0\n' > "$wired/bin/check-fresh"
printf '#!/bin/sh\nexit 0\n' > "$wired/tests/test_check_fresh.sh"
chmod +x "$wired/bin/check-fresh" "$wired/tests/test_check_fresh.sh"
cat > "$wired/bin/lint" <<'EOF'
#!/bin/sh
./bin/check-sample
./tests/test_check_sample.sh
./bin/check-lint-coverage
./bin/check-fresh
./tests/test_check_fresh.sh
EOF
check_case newly_wired_checks_pass 0 '' "$wired"

if [ "$failures" -ne 0 ]; then
    printf '%s check-lint-coverage case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-lint-coverage self-test passed\n'
