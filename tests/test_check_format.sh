#!/bin/sh
set -eu

# Self-test for bin/check-format. Each case runs a copy of the checker in a
# synthetic repo root, so the clang-format flags and C++ paths are exercised
# without touching tracked sources.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-format"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

expect_result() {
    name=$1
    want_status=$2
    want_text=$3
    got_status=$4
    output=$5
    if [ "$got_status" -ne "$want_status" ]; then
        printf 'FAIL %s: status %s, wanted %s\n%s\n' \
            "$name" "$got_status" "$want_status" "$output"
        failures=$((failures + 1))
        return
    fi
    if [ -z "$want_text" ]; then
        if [ -n "$output" ]; then
            printf 'FAIL %s: expected no output, got\n%s\n' "$name" "$output"
            failures=$((failures + 1))
            return
        fi
    else
        case "$output" in
            *"$want_text"*) ;;
            *)
                printf 'FAIL %s: output did not contain %s\n%s\n' \
                    "$name" "$want_text" "$output"
                failures=$((failures + 1))
                return
                ;;
        esac
    fi
    printf 'PASS %s\n' "$name"
}

make_root() {
    root=$1
    mkdir -p "$root/bin" "$root/src" "$root/tests"
    cp "$CHECKER" "$root/bin/check-format"
    chmod +x "$root/bin/check-format"
    cp "$ROOT/.clang-format" "$root/.clang-format"
    for file in src/sample.h src/sample.cpp tests/sample.h tests/sample.cpp; do
        printf 'int value = 0;\n' > "$root/$file"
    done
}

check_case() {
    name=$1
    want_status=$2
    want_text=$3
    root=$4
    set +e
    output="$("$root/bin/check-format" 2>&1)"
    got_status=$?
    set -e
    expect_result "$name" "$want_status" "$want_text" "$got_status" "$output"
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
