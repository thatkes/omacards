#!/bin/sh
set -eu

# Self-test for bin/merge-backstop. The fake integration checkout has real git
# worktrees and stub checks, so merging, logging and removal run without
# touching the omacards build or the issue tracker.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TOOL="$ROOT/bin/merge-backstop"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

make_integration() {
    repo=$1
    mkdir -p "$repo/bin"
    git -C "$repo" init -q -b integration
    git -C "$repo" config user.email test@example.invalid
    git -C "$repo" config user.name Test
    printf 'base\n' > "$repo/README.md"
    cp "$TOOL" "$repo/bin/merge-backstop"
    chmod +x "$repo/bin/merge-backstop"
    for check in lint build test; do
        printf '#!/bin/sh\nexit 0\n' > "$repo/bin/$check"
        chmod +x "$repo/bin/$check"
    done
    git -C "$repo" add README.md bin
    git -C "$repo" commit -q -m 'base tooling'
}

make_integration "$tmp/integration"
wt="$tmp/ticket-42"
git -C "$tmp/integration" worktree add -q -b ticket-42 "$wt"
printf 'ticket work\n' > "$wt/ticket.txt"
git -C "$wt" add ticket.txt
git -C "$wt" commit -q -m 'ticket work'

expect_command_result success_merges_and_removes 0 'MERGED ticket-42' 'CONFLICT' \
    sh -c "cd '$tmp' && '$tmp/integration/bin/merge-backstop' ticket-42"
assert_has success_lint_ok 'OK lint build/merge-42-lint.log' "$last_output"
assert_has success_build_ok 'OK build build/merge-42-build.log' "$last_output"
assert_has success_test_ok 'OK test build/merge-42-test.log' "$last_output"
assert_lacks success_code_merge_has_no_docs_marker \
    'DOCS_ONLY_TEST_DEFERRED' "$last_output"
if [ ! -d "$wt" ]; then
    printf 'PASS success_worktree_removed\n'
else
    printf 'FAIL success_worktree_removed\n'
    failures=$((failures + 1))
fi
if [ -f "$tmp/integration/ticket.txt" ] &&
    [ "$(git -C "$tmp/integration" rev-parse HEAD)" = \
       "$(git -C "$tmp/integration" rev-parse HEAD^1)" ]; then
    printf 'FAIL success_created_merge_commit\n'
    failures=$((failures + 1))
else
    printf 'PASS success_created_merge_commit\n'
fi
for check in lint build test; do
    if [ -f "$tmp/integration/build/merge-42-$check.log" ]; then
        printf 'PASS success_%s_log\n' "$check"
    else
        printf 'FAIL success_%s_log\n' "$check"
        failures=$((failures + 1))
    fi
done

# A docs-only diff still gets lint and build, but defers the compiled suite
# and reports a marker the wave tooling can read.
make_integration "$tmp/docs-only"
wtd="$tmp/docs-only-wt"
git -C "$tmp/docs-only" worktree add -q -b ticket-43 "$wtd"
mkdir -p "$wtd/docs"
printf 'Ticket doc.\n' > "$wtd/docs/guide.md"
printf 'Ticket glossary.\n' > "$wtd/GLOSSARY.md"
git -C "$wtd" add docs/guide.md GLOSSARY.md
git -C "$wtd" commit -q -m 'ticket docs'
expect_command_result docs_only_defers_suite 0 'DOCS_ONLY_TEST_DEFERRED ticket-43' \
    'CONFLICT' sh -c "cd '$tmp' && '$tmp/docs-only/bin/merge-backstop' ticket-43"
assert_has docs_only_lint_ok 'OK lint build/merge-43-lint.log' "$last_output"
assert_has docs_only_build_ok 'OK build build/merge-43-build.log' "$last_output"
assert_lacks docs_only_skips_test 'OK test' "$last_output"
if [ -f "$tmp/docs-only/build/merge-43-test.log" ]; then
    printf 'FAIL docs_only_has_no_test_log\n'
    failures=$((failures + 1))
else
    printf 'PASS docs_only_has_no_test_log\n'
fi
if [ ! -d "$wtd" ]; then
    printf 'PASS docs_only_worktree_removed\n'
else
    printf 'FAIL docs_only_worktree_removed\n'
    failures=$((failures + 1))
fi

# A markdown file outside the docs-only allowlist is a code diff: the full
# backstop runs and no deferral marker is printed.
make_integration "$tmp/nested-md"
wtm="$tmp/nested-md-wt"
git -C "$tmp/nested-md" worktree add -q -b ticket-44 "$wtm"
mkdir -p "$wtm/guides"
printf 'Ticket guide.\n' > "$wtm/guides/extra.md"
git -C "$wtm" add guides/extra.md
git -C "$wtm" commit -q -m 'ticket guide'
expect_command_result nested_markdown_keeps_suite 0 'MERGED ticket-44' \
    'DOCS_ONLY_TEST_DEFERRED' \
    sh -c "cd '$tmp' && '$tmp/nested-md/bin/merge-backstop' ticket-44"
assert_has nested_markdown_test_ok 'OK test build/merge-44-test.log' "$last_output"

make_integration "$tmp/no-worktree"
expect_command_result missing_worktree_fails 1 'no worktree on ticket-7' 'MERGED' \
    "$tmp/no-worktree/bin/merge-backstop" ticket-7

printf 'dirty\n' >> "$tmp/no-worktree/README.md"
expect_command_result dirty_checkout_fails 1 'integration checkout has tracked changes' \
    'MERGED' "$tmp/no-worktree/bin/merge-backstop" ticket-7

make_integration "$tmp/nested"
mkdir -p "$tmp/nested/subdir/bin"
cp "$TOOL" "$tmp/nested/subdir/bin/merge-backstop"
chmod +x "$tmp/nested/subdir/bin/merge-backstop"
expect_command_result nested_script_rejects_non_root 1 'not the integration checkout root' \
    'MERGED' "$tmp/nested/subdir/bin/merge-backstop" ticket-7

make_integration "$tmp/conflict"
wt7="$tmp/conflict-wt"
git -C "$tmp/conflict" worktree add -q -b ticket-7 "$wt7"
printf 'ticket\n' > "$wt7/README.md"
git -C "$wt7" add README.md
git -C "$wt7" commit -q -m 'ticket edit'
printf 'integration\n' > "$tmp/conflict/README.md"
git -C "$tmp/conflict" add README.md
git -C "$tmp/conflict" commit -q -m 'integration edit'
expect_command_result conflict_reports_and_keeps_worktree 1 'CONFLICT ticket-7' 'MERGED' \
    "$tmp/conflict/bin/merge-backstop" ticket-7
if [ -d "$wt7" ]; then
    printf 'PASS conflict_worktree_kept\n'
else
    printf 'FAIL conflict_worktree_kept\n'
    failures=$((failures + 1))
fi

make_integration "$tmp/red"
wt9="$tmp/red-wt"
git -C "$tmp/red" worktree add -q -b ticket-9 "$wt9"
printf 'ticket\n' > "$wt9/ticket.txt"
git -C "$wt9" add ticket.txt
git -C "$wt9" commit -q -m 'ticket work'
printf '#!/bin/sh\nprintf "BUILD_FAILED\\n"\nexit 1\n' > "$tmp/red/bin/build"
chmod +x "$tmp/red/bin/build"
git -C "$tmp/red" add bin/build
git -C "$tmp/red" commit -q -m 'failing build stub'
expect_command_result red_check_reports_and_keeps_worktree 1 'RED build build/merge-9-build.log' \
    'MERGED' "$tmp/red/bin/merge-backstop" ticket-9
assert_has red_lint_ok 'OK lint build/merge-9-lint.log' "$last_output"
assert_lacks red_has_no_build_ok 'OK build' "$last_output"
assert_lacks red_has_no_test_run 'OK test' "$last_output"
if [ -d "$wt9" ] &&
    grep -qF 'BUILD_FAILED' "$tmp/red/build/merge-9-build.log"; then
    printf 'PASS red_worktree_kept_and_log_captured\n'
else
    printf 'FAIL red_worktree_kept_and_log_captured\n'
    failures=$((failures + 1))
fi

make_integration "$tmp/no-side-effects"
wt11="$tmp/no-side-effects-wt"
git -C "$tmp/no-side-effects" worktree add -q -b ticket-11 "$wt11"
printf 'ticket\n' > "$wt11/ticket.txt"
git -C "$wt11" add ticket.txt
git -C "$wt11" commit -q -m 'ticket work'
mkdir -p "$tmp/fakebin"
real_git=$(command -v git)
cat > "$tmp/fakebin/git" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$tmp/git-calls"
exec "$real_git" "\$@"
EOF
cat > "$tmp/fakebin/gh" <<EOF
#!/bin/sh
printf 'gh called: %s\n' "\$*" >> "$tmp/gh-calls"
exit 1
EOF
chmod +x "$tmp/fakebin/git" "$tmp/fakebin/gh"
expect_command_result no_push_or_tracker_calls 0 'MERGED ticket-11' '' \
    env PATH="$tmp/fakebin:$PATH" "$tmp/no-side-effects/bin/merge-backstop" ticket-11
if [ -s "$tmp/git-calls" ] &&
    ! grep -Eq '(^| )(push|issue)( |$)' "$tmp/git-calls" &&
    [ ! -s "$tmp/gh-calls" ] &&
    [ -z "$(git -C "$tmp/no-side-effects" status --porcelain --untracked-files=no)" ]; then
    printf 'PASS no_side_effects\n'
else
    printf 'FAIL no_side_effects\n%s\n' "$(cat "$tmp/git-calls" 2>/dev/null)"
    failures=$((failures + 1))
fi

if [ "$failures" -ne 0 ]; then
    printf '%s merge-backstop case(s) failed\n' "$failures"
    exit 1
fi
printf 'merge-backstop self-test passed\n'
