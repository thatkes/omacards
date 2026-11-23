#!/bin/sh
set -eu

# Self-test for bin/check-sync-worktree-local-docs. Each case runs the checker
# against a synthetic repo root, so the parser is exercised without changing
# tracked files.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-sync-worktree-local-docs"
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

write_sync_script() {
    cat > "$1/bin/sync-worktree-local-docs" <<'EOF'
#!/bin/sh
for path in AGENTS.md docs/agents MISSION.md assets \
    learning-records; do
    printf 'Copy %s\n' "$path"
done
EOF
}

make_root() {
    root=$1
    mkdir -p "$root/bin"
    cp "$CHECKER" "$root/bin/check-sync-worktree-local-docs"
    chmod +x "$root/bin/check-sync-worktree-local-docs"
    cat > "$root/.gitignore" <<'EOF'
/build/
/pkgbuild/pkg/
/pkgbuild/src/
*.pkg.tar.*

# Local-only agent instructions; keep out of the remote.
/AGENTS.md
/docs/agents/
# src/skill/SKILL.md is product code: it is bundled and installed by the CLI.

# Local-only maintainer teaching workspace; keep out of the remote.
/MISSION.md
/assets/
/learning-records/

# Inert while this file is tracked; applies if it is ever untracked.
/.gitignore
EOF
    write_sync_script "$root"
}

check_case() {
    name=$1
    want_status=$2
    want_text=$3
    root=$4
    set +e
    output="$("$root/bin/check-sync-worktree-local-docs" "$root" 2>&1)"
    got_status=$?
    set -e
    expect_result "$name" "$want_status" "$want_text" "$got_status" "$output"
}

matching="$tmp/matching"
make_root "$matching"
check_case matching_lists_pass 0 'Local-only path lists match' "$matching"

# Build output ignores stay ignored even when placed inside a Local-only block.
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

# Local-only maintainer teaching workspace; keep out of the remote.
/MISSION.md
/assets/
/learning-records/
EOF
check_case build_output_ignores_skipped 0 'Local-only path lists match' \
    "$with_build"

missing_in_script="$tmp/missing-in-script"
make_root "$missing_in_script"
sed 's/[[:space:]]*learning-records//' \
    "$missing_in_script/bin/sync-worktree-local-docs" \
    > "$tmp/sync-without-learning-records"
mv "$tmp/sync-without-learning-records" \
    "$missing_in_script/bin/sync-worktree-local-docs"
check_case script_missing_path_fails 1 'diverge' "$missing_in_script"

extra_in_script="$tmp/extra-in-script"
make_root "$extra_in_script"
sed 's/learning-records/learning-records extra-notes/' \
    "$extra_in_script/bin/sync-worktree-local-docs" \
    > "$tmp/sync-with-extra-path"
mv "$tmp/sync-with-extra-path" \
    "$extra_in_script/bin/sync-worktree-local-docs"
check_case script_extra_path_fails 1 'diverge' "$extra_in_script"

extra_in_gitignore="$tmp/extra-in-gitignore"
make_root "$extra_in_gitignore"
awk '/\/learning-records\// { print; print "/coaching-notes/"; next } { print }' \
    "$extra_in_gitignore/.gitignore" > "$tmp/gitignore-with-extra-path"
mv "$tmp/gitignore-with-extra-path" "$extra_in_gitignore/.gitignore"
check_case gitignore_extra_path_fails 1 'diverge' "$extra_in_gitignore"

unanchored_in_gitignore="$tmp/unanchored-in-gitignore"
make_root "$unanchored_in_gitignore"
awk '/\/learning-records\// { print; print "coaching-notes/"; next } { print }' \
    "$unanchored_in_gitignore/.gitignore" > "$tmp/gitignore-with-unanchored-path"
mv "$tmp/gitignore-with-unanchored-path" \
    "$unanchored_in_gitignore/.gitignore"
check_case unanchored_gitignore_path_fails 1 'diverge' \
    "$unanchored_in_gitignore"

if [ "$failures" -ne 0 ]; then
    printf '%s check-sync-worktree-local-docs case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-sync-worktree-local-docs self-test passed\n'
