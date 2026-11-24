#!/bin/sh
set -eu

# Self-test for .githooks/commit-msg and bin/check-commit-scope.
# Each case stages files in an isolated temporary repo, writes a synthetic
# message, and asserts the hook's exit status and a distinctive output line.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
HOOK="$ROOT/.githooks/commit-msg"
CHECKER="$ROOT/bin/check-commit-scope"
SUBJECT_CHECKER="$ROOT/bin/check-commit-subject"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

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
    printf '// Old entry point header.\nint main(void) { return 0; }\n' \
        > "$repo/src/cli.cpp"
    printf 'Project README.\n' > "$repo/README.md"
    printf 'Prose license.\n' > "$repo/LICENSE"
    printf 'docs: update guide\n' > "$repo/message.txt"
}

make_baseline_repo() {
    repo=$1
    make_repo "$repo"
    git -C "$repo" add src/cli.cpp
    git -C "$repo" -c user.name=Tester \
        -c user.email=tester@example.invalid commit -qm baseline
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

# LICENSE is prose and belongs in a docs: commit.
license="$tmp/license-docs"
make_repo "$license"
git -C "$license" add docs/guide.md LICENSE
check_hook license_accepted 0 '' "$license"

# Added and removed comment lines, in both .cpp and .h, are allowed.
comments="$tmp/comments-docs"
make_baseline_repo "$comments"
cat > "$comments/src/cli.cpp" <<'EOF'
// New entry point header.
/*
 * Explanation of the entry point.
 */
int main(void) { return 0; }
EOF
cat > "$comments/src/theme.h" <<'EOF'
// Theme declarations.
EOF
git -C "$comments" add docs/guide.md src/cli.cpp src/theme.h
check_hook comment_only_src_accepted 0 '' "$comments"

# Changing only a block-comment continuation line stays allowed.
continuation="$tmp/continuation-docs"
make_repo "$continuation"
cat > "$continuation/src/cli.cpp" <<'EOF'
// Old entry point header.
/*
 * Old explanation.
 */
int main(void) { return 0; }
EOF
git -C "$continuation" add src/cli.cpp
git -C "$continuation" -c user.name=Tester \
    -c user.email=tester@example.invalid commit -qm baseline
cat > "$continuation/src/cli.cpp" <<'EOF'
// Old entry point header.
/*
 * New explanation.
 */
int main(void) { return 0; }
EOF
git -C "$continuation" add docs/guide.md src/cli.cpp
check_hook block_comment_continuation_accepted 0 '' "$continuation"

# Blank lines and every comment opener or closer still pass on their own.
forms="$tmp/comment-forms-docs"
make_baseline_repo "$forms"
cat > "$forms/src/cli.cpp" <<'EOF'

// New entry point header.
/*
 * Explanation of the entry point.
 */
int main(void) { return 0; }
EOF
git -C "$forms" add docs/guide.md src/cli.cpp
check_hook comment_forms_accepted 0 '' "$forms"

# A non-comment source line fails even when it sits beside comment lines.
code="$tmp/code-docs"
make_baseline_repo "$code"
cat > "$code/src/cli.cpp" <<'EOF'
// New entry point header.
int main(void) { return 1; }
EOF
git -C "$code" add docs/guide.md src/cli.cpp
check_hook code_src_rejected 1 \
    'docs: commit must only include docs files' "$code"

# A star-prefixed dereference is source code, not a block-comment line.
deref="$tmp/deref-docs"
make_repo "$deref"
cat > "$deref/src/cli.cpp" <<'EOF'
// Old entry point header.
void set(int *p) {
}
EOF
git -C "$deref" add src/cli.cpp
git -C "$deref" -c user.name=Tester \
    -c user.email=tester@example.invalid commit -qm baseline
cat > "$deref/src/cli.cpp" <<'EOF'
// Old entry point header.
void set(int *p) {
    *p = 1;
}
EOF
git -C "$deref" add docs/guide.md src/cli.cpp
check_hook star_deref_src_rejected 1 \
    'docs: commit must only include docs files' "$deref"

# Lines that look like diff headers are still source lines, not headers.
operators="$tmp/operators-docs"
make_repo "$operators"
printf '// Old header.\n-- counter;\n' > "$operators/src/cli.cpp"
git -C "$operators" add src/cli.cpp
git -C "$operators" -c user.name=Tester \
    -c user.email=tester@example.invalid commit -qm baseline
printf '// New header.\n++ counter;\n' > "$operators/src/cli.cpp"
git -C "$operators" add docs/guide.md src/cli.cpp
check_hook operator_src_rejected 1 \
    'docs: commit must only include docs files' "$operators"

# Deleting a source file is a source change even when every deleted line is
# only a comment, so a docs: commit cannot quietly remove code documentation.
deleted_comments="$tmp/deleted-comments-docs"
make_repo "$deleted_comments"
printf '// Old entry point header.\n' > "$deleted_comments/src/theme.h"
git -C "$deleted_comments" add src/theme.h
git -C "$deleted_comments" -c user.name=Tester \
    -c user.email=tester@example.invalid commit -qm baseline
git -C "$deleted_comments" rm -q src/theme.h
git -C "$deleted_comments" add docs/guide.md
check_hook deleted_comment_src_rejected 1 \
    'docs: commit must only include docs files' "$deleted_comments"

# Deleting a source file with code lines is rejected as well.
deleted_src="$tmp/deleted-src-docs"
make_baseline_repo "$deleted_src"
git -C "$deleted_src" rm -q src/cli.cpp
git -C "$deleted_src" add docs/guide.md
check_hook deleted_src_rejected 1 \
    'docs: commit must only include docs files' "$deleted_src"

# Documentation outside the allowlist is still rejected.
outside="$tmp/outside-docs"
make_repo "$outside"
mkdir -p "$outside/guides"
printf 'More notes.\n' > "$outside/guides/extra.md"
git -C "$outside" add docs/guide.md guides/extra.md
check_hook docs_outside_allowlist_rejected 1 \
    'docs: commit must only include docs files' "$outside"

if [ "$failures" -ne 0 ]; then
    printf '%s check-commit-scope case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-commit-scope self-test passed\n'
