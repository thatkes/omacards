#!/bin/sh
set -eu

# Self-test for .githooks/commit-msg and bin/check-commit-scope.
# Each case stages files in an isolated temporary repo, writes a synthetic
# message, and asserts the hook's exit status and a distinctive output line.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
HOOK="$ROOT/.githooks/commit-msg"
CHECKER="$ROOT/bin/check-commit-scope"
SUBJECT_CHECKER="$ROOT/bin/check-commit-subject"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

report_failure() {
    printf 'FAIL %s: status %s, wanted %s\n%s\n' \
        "$1" "$2" "$3" "$4"
    failures=$((failures + 1))
}

expect_result() {
    name=$1
    want_status=$2
    want_text=$3
    got_status=$4
    output=$5
    if [ "$got_status" -ne "$want_status" ]; then
        report_failure "$name" "$got_status" "$want_status" "$output"
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

make_repo() {
    repo=$1
    mkdir -p "$repo/.githooks" "$repo/bin" "$repo/docs" "$repo/src"
    cp "$HOOK" "$repo/.githooks/commit-msg"
    cp "$SUBJECT_CHECKER" "$repo/bin/check-commit-subject"
    cp "$CHECKER" "$repo/bin/check-commit-scope"
    chmod +x "$repo/.githooks/commit-msg" \
        "$repo/bin/check-commit-subject" "$repo/bin/check-commit-scope"
    git -C "$repo" init -q
    printf 'Guide notes.\n' > "$repo/docs/guide.md"
    printf 'int main(void) { return 0; }\n' > "$repo/src/cli.cpp"
    printf 'Project README.\n' > "$repo/README.md"
    printf 'docs: update guide\n' > "$repo/message.txt"
}

check_hook() {
    name=$1
    want_status=$2
    want_text=$3
    repo=$4
    set +e
    output="$("$repo/.githooks/commit-msg" "$repo/message.txt" 2>&1)"
    got_status=$?
    set -e
    expect_result "$name" "$want_status" "$want_text" "$got_status" "$output"
}

# A docs: subject must not carry source or config changes.
mixed="$tmp/mixed-docs"
make_repo "$mixed"
git -C "$mixed" add docs/guide.md src/cli.cpp
check_hook mixed_docs_rejected 1 \
    'docs: commit must only include docs files' "$mixed"

# Documentation-only changes remain allowed.
clean="$tmp/clean-docs"
make_repo "$clean"
git -C "$clean" add docs/guide.md README.md
check_hook clean_docs_accepted 0 '' "$clean"

if [ "$failures" -ne 0 ]; then
    printf '%s check-commit-scope case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-commit-scope self-test passed\n'
