#!/bin/sh
set -eu

# Self-test for bin/check-package-skill-install. Each case replays a synthetic
# package install, so the check's assertion is exercised without makepkg or a
# compiled binary and never touches the real installed skill.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-package-skill-install"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

populate_root() {
    root=$1
    mkdir -p "$root/pkgbuild" "$root/src/skill" "$root/build"
    cp "$ROOT/pkgbuild/PKGBUILD" "$root/pkgbuild/PKGBUILD"
    cp "$ROOT/pkgbuild/omacards.svg" "$ROOT/pkgbuild/omacards.desktop" \
        "$root/pkgbuild/"
    cp "$ROOT/src/skill/SKILL.md" "$root/src/skill/SKILL.md"
    cp "$ROOT/LICENSE" "$root/LICENSE"
    : > "$root/build/omacards"
    chmod +x "$root/build/omacards"
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
