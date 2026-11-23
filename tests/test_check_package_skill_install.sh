#!/bin/sh
set -eu

# Self-test for bin/check-package-skill-install. Each case replays a synthetic
# package install, so the check's assertion is exercised without makepkg or a
# compiled binary and never touches the real installed skill.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-package-skill-install"
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

make_root() {
    root=$1
    mkdir -p "$root/bin" "$root/pkgbuild" "$root/src/skill" "$root/build"
    cp "$CHECKER" "$root/bin/check-package-skill-install"
    chmod +x "$root/bin/check-package-skill-install"
    cp "$ROOT/pkgbuild/PKGBUILD" "$root/pkgbuild/PKGBUILD"
    cp "$ROOT/pkgbuild/omacards.svg" "$ROOT/pkgbuild/omacards.desktop" \
        "$root/pkgbuild/"
    cp "$ROOT/src/skill/SKILL.md" "$root/src/skill/SKILL.md"
    cp "$ROOT/LICENSE" "$root/LICENSE"
    : > "$root/build/omacards"
    chmod +x "$root/build/omacards"
}

check_case() {
    name=$1
    want_status=$2
    want_text=$3
    root=$4
    set +e
    output="$("$root/bin/check-package-skill-install" 2>&1)"
    got_status=$?
    set -e
    expect_result "$name" "$want_status" "$want_text" "$got_status" "$output"
}

present="$tmp/present"
make_root "$present"
check_case package_skill_present 0 \
    'Package install placed the agent skill' "$present"

missing="$tmp/missing"
make_root "$missing"
sed -i '/agents\/skills\/omacards\/SKILL.md/d' "$missing/pkgbuild/PKGBUILD"
check_case package_skill_missing 1 \
    'Package install did not create' "$missing"

divergent="$tmp/divergent"
make_root "$divergent"
sed -i 's|src/skill/SKILL.md|LICENSE|' "$divergent/pkgbuild/PKGBUILD"
check_case package_skill_diverges 1 \
    'Packaged skill differs from src/skill/SKILL.md' "$divergent"

if [ "$failures" -ne 0 ]; then
    printf '%s check-package-skill-install case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-package-skill-install self-test passed\n'
