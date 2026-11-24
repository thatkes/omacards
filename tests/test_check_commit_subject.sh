#!/bin/sh
set -eu

# Self-test for bin/check-commit-subject and the commit-msg hook wrapper.
# Each case writes a synthetic message and asserts the exit status and a
# distinctive part of the checker's output.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-commit-subject"
HOOK="$ROOT/.githooks/commit-msg"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

write_message() {
    printf '%s\n' "$2" > "$1"
}

check_file() {
    name=$1
    want_status=$2
    want_text=$3
    message_file=$4
    set +e
    output="$("$CHECKER" "$message_file" 2>&1)"
    got_status=$?
    set -e
    expect_result "$name" "$want_status" "$want_text" "$got_status" "$output"
}

check_stdin() {
    name=$1
    want_status=$2
    want_text=$3
    input=$4
    set +e
    output="$(printf '%s\n' "$input" | "$CHECKER" --stdin 2>&1)"
    got_status=$?
    set -e
    expect_result "$name" "$want_status" "$want_text" "$got_status" "$output"
}

check_hook() {
    name=$1
    want_status=$2
    want_text=$3
    message_file=$4
    set +e
    output="$("$HOOK" "$message_file" 2>&1)"
    got_status=$?
    set -e
    expect_result "$name" "$want_status" "$want_text" "$got_status" "$output"
}

write_message "$tmp/no-type.txt" 'Refresh the plan'
check_file no_type_rejected 1 'type prefix' "$tmp/no-type.txt"

write_message "$tmp/valid.txt" 'docs: update README'
check_file valid_type_accepted 0 '' "$tmp/valid.txt"

write_message "$tmp/unknown-type.txt" 'doctype: update README'
check_file unknown_type_rejected 1 'unknown commit subject type' "$tmp/unknown-type.txt"

write_message "$tmp/uppercase.txt" 'docs: Update README'
check_file uppercase_description_rejected 1 'lowercase' "$tmp/uppercase.txt"

write_message "$tmp/merge.txt" 'chore: merge ticket-42 into integration'
check_file merge_subject_typed_accepted 0 '' "$tmp/merge.txt"

write_message "$tmp/untyped-merge.txt" 'Merge ticket-42 into integration'
check_file untyped_merge_rejected 1 'type prefix' "$tmp/untyped-merge.txt"

write_message "$tmp/revert-type.txt" 'revert: "drop stale card count"'
check_file revert_type_accepted 0 '' "$tmp/revert-type.txt"

write_message "$tmp/git-revert.txt" \
    'Revert "fix: skip skill check in linked worktrees"'
check_file git_revert_subject_rejected 1 'unknown commit subject type' \
    "$tmp/git-revert.txt"

write_message "$tmp/revert-long.txt" \
    'revert: "this quoted original subject is deliberately longer than fifty characters"'
check_file revert_subject_over_50_rejected 1 'over 50 characters' \
    "$tmp/revert-long.txt"

write_message "$tmp/long-subject.txt" \
    'fix: this commit subject is deliberately longer than fifty characters'
check_file subject_over_50_rejected 1 'over 50 characters' "$tmp/long-subject.txt"

printf 'fix: short subject\n' > "$tmp/long-line.txt"
awk 'BEGIN { for (i = 0; i < 73; i++) printf "x" }' >> "$tmp/long-line.txt"
printf '\n' >> "$tmp/long-line.txt"
check_hook line_over_72_rejected 1 'line over 72 characters' "$tmp/long-line.txt"

check_stdin stdin_valid_and_merge_accepted 0 '' \
    'docs: update README
chore: merge ticket-42 into integration'
check_stdin stdin_no_type_rejected 1 'stdin line 3' \
    'docs: update README
chore: merge ticket-42 into integration
Refresh the plan'
check_stdin stdin_untyped_merge_rejected 1 'type prefix' \
    'Merge ticket-42 into integration'
check_stdin stdin_unknown_type_rejected 1 'unknown commit subject type' \
    'doctype: update README'
check_stdin stdin_revert_type_accepted 0 '' \
    'revert: "drop stale card count"'
check_stdin stdin_over_50_rejected 1 'over 50 characters' \
    'fix: this commit subject is deliberately longer than fifty characters'

if [ "$failures" -ne 0 ]; then
    printf '%s check-commit-subject case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-commit-subject self-test passed\n'
