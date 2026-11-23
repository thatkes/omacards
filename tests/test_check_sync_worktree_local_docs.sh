#!/bin/sh
set -eu

# Self-test for the shared Local-only parser. Each case runs the checker and
# the sync helper against a synthetic repo root, so the parser is exercised
# without changing tracked files or creating real worktrees.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-sync-worktree-local-docs"
HELPER="$ROOT/bin/sync-worktree-local-docs"
PARSER="$ROOT/bin/local-only-sync-paths.sh"
. "$ROOT/tests/checker_test_support.sh"
CHECKER_TAKES_ROOT=1
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

populate_root() {
    root=$1
    cp "$HELPER" "$root/bin/sync-worktree-local-docs"
    cp "$PARSER" "$root/bin/local-only-sync-paths.sh"
    chmod +x "$root/bin/check-sync-worktree-local-docs" \
        "$root/bin/sync-worktree-local-docs" \
        "$root/bin/local-only-sync-paths.sh"
    cat > "$root/.gitignore" <<'EOF'
/build/
/pkgbuild/pkg/
/pkgbuild/src/
*.pkg.tar.*

# Local-only agent instructions; keep out of the remote.
/AGENTS.md
/docs/agents/
/reference/
# src/skill/SKILL.md is product code: it is bundled and installed by the CLI.

# Local-only maintainer teaching workspace; keep out of the remote.
/MISSION.md
/assets/
/learning-records/

# Inert while this file is tracked; applies if it is ever untracked.
/.gitignore
EOF
}

check_both_cases() {
    case_name=$1
    case_status=$2
    case_text=$3
    case_root=$4
    check_case "${case_name}_checker" "$case_status" "$case_text" "$case_root"
    set +e
    output="$("$case_root/bin/sync-worktree-local-docs" "$tmp/target" 2>&1)"
    got_status=$?
    set -e
    expect_result "${case_name}_helper" "$case_status" "$case_text" \
        "$got_status" "$output"
}

with_build="$tmp/with-build"
make_root "$with_build"
cat > "$with_build/.gitignore" <<'EOF'
# Local-only agent instructions; keep out of the remote.
/build/
/AGENTS.md
/pkgbuild/pkg/
/docs/agents/
/pkgbuild/src/
*.pkg.tar.*
/reference/

# Local-only maintainer teaching workspace; keep out of the remote.
/MISSION.md
/assets/
/learning-records/
EOF
check_case build_output_ignores_skipped 0 \
    'Local-only sync list OK (6 paths)' "$with_build"

missing_header="$tmp/missing-header"
make_root "$missing_header"
sed -i 's/^# Local-only maintainer /# Maintainer /' \
    "$missing_header/.gitignore"
check_both_cases missing_header_fails 1 'Local-only header missing' \
    "$missing_header"

empty_block="$tmp/empty-block"
make_root "$empty_block"
sed -i '/^\/AGENTS\.md$/d; /^\/docs\/agents\/$/d; /^\/reference\/$/d' \
    "$empty_block/.gitignore"
check_both_cases empty_block_fails 1 'Local-only block 1 is empty' \
    "$empty_block"

missing_sentinel="$tmp/missing-sentinel"
make_root "$missing_sentinel"
sed -i '/^\/reference\/$/d' "$missing_sentinel/.gitignore"
check_both_cases missing_sentinel_fails 1 \
    'Local-only sentinel missing' "$missing_sentinel"

unreadable="$tmp/unreadable"
make_root "$unreadable"
rm "$unreadable/.gitignore"
check_both_cases unreadable_source_fails 1 'Cannot read local-only source' \
    "$unreadable"

if [ "$failures" -ne 0 ]; then
    printf '%s check-sync-worktree-local-docs case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-sync-worktree-local-docs self-test passed\n'
