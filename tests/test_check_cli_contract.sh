#!/bin/sh
set -eu

# Self-test for bin/check-cli-contract. Fixtures hold the four frozen literals
# in synthetic sources; removing one at a time proves each direction is checked.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-cli-contract"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

populate_root() {
    root=$1
    mkdir -p "$root/src" "$root/tests"
    cat > "$root/src/cli.cpp" <<'EOF'
void report() {
    qCritical("Cannot read scheduling state: %1", error);
    qCritical("Cannot record rating: %1", error);
}
EOF
    cat > "$root/tests/test_cli.cpp" <<'EOF'
void testWrappers() {
    QVERIFY(output.contains("Cannot read scheduling state: "));
    QVERIFY(output.contains("Cannot record rating: "));
}
EOF
}

matching="$tmp/matching"
make_root "$matching"
check_case cli_contract_fixture_pass 0 '' "$matching"

missing_source="$tmp/missing-source"
make_root "$missing_source"
sed '/Cannot read scheduling state/d' "$missing_source/src/cli.cpp" \
    > "$tmp/source.cpp"
mv "$tmp/source.cpp" "$missing_source/src/cli.cpp"
check_case missing_source_literal_reported 1 \
    'Missing frozen CLI literal in src/cli.cpp: Cannot read scheduling state: %1' \
    "$missing_source"

missing_test="$tmp/missing-test"
make_root "$missing_test"
sed '/Cannot record rating/d' "$missing_test/tests/test_cli.cpp" \
    > "$tmp/test.cpp"
mv "$tmp/test.cpp" "$missing_test/tests/test_cli.cpp"
check_case missing_test_pin_reported 1 \
    'Missing frozen CLI literal in tests/test_cli.cpp: Cannot record rating: ' \
    "$missing_test"

if [ "$failures" -ne 0 ]; then
    printf '%s check-cli-contract case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-cli-contract self-test passed\n'
