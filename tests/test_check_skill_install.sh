#!/bin/sh
set -eu

# Self-test for bin/check-skill-install. Each case runs the checker from a
# throwaway main checkout or linked worktree with an isolated HOME.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-skill-install"
tmp="$(mktemp -d)"
cleanup() {
    git -C "$tmp/main" worktree remove --force "$tmp/linked" >/dev/null 2>&1 || true
    rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

failures=0

check_case() {
    name=$1
    want_status=$2
    want_text=$3
    checker=$4
    set +e
    output="$(HOME="$tmp/home" "$checker" 2>&1)"
    got_status=$?
    set -e
    if [ "$got_status" -ne "$want_status" ]; then
        printf 'FAIL %s: status %s, wanted %s\n%s\n' \
            "$name" "$got_status" "$want_status" "$output"
        failures=$((failures + 1))
        return
    fi
    # A dash means the checker is expected to say nothing.
    if [ "$want_text" = '-' ]; then
        if [ -n "$output" ]; then
            printf 'FAIL %s: expected no output\n%s\n' "$name" "$output"
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

mkdir -p "$tmp/main/bin" "$tmp/main/src/skill" \
    "$tmp/home/.agents/skills/omacards"
printf '# omacards skill\n' > "$tmp/main/src/skill/SKILL.md"
cp "$CHECKER" "$tmp/main/bin/check-skill-install"
chmod +x "$tmp/main/bin/check-skill-install"

git init -q -b main "$tmp/main"
git -C "$tmp/main" config user.name Test
git -C "$tmp/main" config user.email test@example.invalid
git -C "$tmp/main" add src/skill/SKILL.md bin/check-skill-install
git -C "$tmp/main" commit -qm initial
git -C "$tmp/main" worktree add -q "$tmp/linked" -b linked
cp "$CHECKER" "$tmp/linked/bin/check-skill-install"
chmod +x "$tmp/linked/bin/check-skill-install"

printf 'different skill\n' > "$tmp/home/.agents/skills/omacards/SKILL.md"
check_case main_mismatch_fails 1 \
    'Installed omacards skill differs from' \
    "$tmp/main/bin/check-skill-install"

cp "$tmp/main/src/skill/SKILL.md" "$tmp/home/.agents/skills/omacards/SKILL.md"
check_case main_match_passes 0 '-' "$tmp/main/bin/check-skill-install"

rm -f "$tmp/home/.agents/skills/omacards/SKILL.md"
check_case main_missing_installed_skips 0 \
    'Installed omacards skill not found at' \
    "$tmp/main/bin/check-skill-install"

printf 'different skill\n' > "$tmp/home/.agents/skills/omacards/SKILL.md"
check_case linked_mismatch_skips 0 \
    'Linked worktree: skipping installed skill check' \
    "$tmp/linked/bin/check-skill-install"

if [ "$failures" -ne 0 ]; then
    printf '%s check-skill-install case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-skill-install self-test passed\n'
