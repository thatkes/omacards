#!/bin/sh
set -eu

# Self-test for bin/check-format. Each case runs a copy of the checker in a
# synthetic repo root, so the clang-format flags and C++ paths are exercised
# without touching tracked sources.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-format"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

populate_root() {
    root=$1
    mkdir -p "$root/src" "$root/tests"
    cp "$ROOT/.clang-format" "$root/.clang-format"
    for file in src/sample.h src/sample.cpp tests/sample.h tests/sample.cpp; do
        printf 'int value = 0;\n' > "$root/$file"
    done
}

matching="$tmp/matching"
make_root "$matching"
check_case formatted_sources_pass 0 '' "$matching"

badly_formatted="$tmp/badly-formatted"
make_root "$badly_formatted"
printf 'int  value=0;\n' > "$badly_formatted/tests/sample.cpp"
check_case badly_formatted_source_fails 1 \
    'tests/sample.cpp:1:4: error: code should be clang-formatted' \
    "$badly_formatted"

if [ "$failures" -ne 0 ]; then
    printf '%s check-format case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-format self-test passed\n'
