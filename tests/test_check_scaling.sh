#!/bin/sh
set -eu

# Self-test for bin/check-scaling. A fake binary turns the Card count into a
# measured delay, so a linear implementation and a deliberately quadratic one
# can be checked without building the app or waiting on a large real Deck.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-scaling"
. "$ROOT/tests/checker_test_support.sh"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

assert_contains_text() {
    name=$1
    text=$2
    output=$3
    case "$output" in
        *"$text"*) printf 'PASS %s\n' "$name" ;;
        *)
            printf 'FAIL %s: output did not contain %s\n%s\n' "$name" "$text" "$output"
            failures=$((failures + 1))
            ;;
    esac
}

cat > "$tmp/fake-omacards" <<'EOF'
#!/bin/sh
set -eu
cards=$(grep -c '^Q: ' "$OMACARDS_DECKS_DIR"/*.txt || true)
case "$1:${OMACARDS_FAKE_SCALING_MODE:-linear}" in
    check:quadratic-check|decks:quadratic-decks)
        ms=$((cards * cards / 125))
        ;;
    *)
        ms=$((cards / 5))
        ;;
esac
seconds=$(awk -v ms="$ms" 'BEGIN { printf "%.3f\n", ms / 1000 }')
sleep "$seconds"
printf '{"ok":true}\n'
EOF
chmod +x "$tmp/fake-omacards"

run_checker() {
    mode=$1
    OMACARDS_SCALING_SMALL=50 \
    OMACARDS_SCALING_LARGE=200 \
    OMACARDS_FAKE_SCALING_MODE="$mode" \
        "$CHECKER" "$tmp/fake-omacards" 2>&1
}

set +e
linear_output=$(run_checker linear)
linear_status=$?
set -e
expect_result linear_scaling_passes 0 'Scaling OK' "$linear_status" "$linear_output"
assert_contains_text linear_reports_both_commands 'decks --json' "$linear_output"

set +e
check_output=$(run_checker quadratic-check)
check_status=$?
set -e
expect_result quadratic_check_fails 1 'check --json scaling regression' "$check_status" "$check_output"
assert_contains_text failure_reports_times '50 -> 200 cards took' "$check_output"
assert_contains_text failure_reports_limit 'x, limit 8x' "$check_output"

set +e
decks_output=$(run_checker quadratic-decks)
decks_status=$?
set -e
expect_result quadratic_decks_fails 1 'decks --json scaling regression' "$decks_status" "$decks_output"

if [ "$failures" -ne 0 ]; then
    printf '%s check-scaling case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-scaling self-test passed\n'
