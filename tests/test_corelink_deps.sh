#!/bin/sh
set -eu

# Regression test for src/corelink.pri. The app's Make target is the static
# library file itself; without a FORCE prerequisite make skips the sub-make
# whenever the archive exists, so edits to core headers never reach objects.
# Generate a real Makefile from the shipped .pri and from a copy with that
# prerequisite removed; only the shipped one may force the library target.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

make_fixture() {
    name=$1
    pri=$2
    fixture="$tmp/$name"
    mkdir -p "$fixture/src" "$fixture/build"
    printf 'TEMPLATE = app\nQT += core\nSOURCES += $$PWD/src/main.cpp\n' \
        > "$fixture/omacards.pro"
    printf 'include(src/corelink.pri)\n' >> "$fixture/omacards.pro"
    printf 'TEMPLATE = lib\nCONFIG += staticlib\nTARGET = omacardscore\n' \
        > "$fixture/src/core.pro"
    printf 'SOURCES += core.cpp\n' >> "$fixture/src/core.pro"
    : > "$fixture/src/main.cpp"
    : > "$fixture/src/core.cpp"
    cp "$pri" "$fixture/src/corelink.pri"
    (cd "$fixture/build" && qmake6 "$fixture/omacards.pro" > /dev/null)
}

check_fixture() {
    name=$1
    expected=$2
    if grep -Eq '^[^#]*libomacardscore\.a:.*FORCE' \
        "$tmp/$name/build/Makefile"; then
        found=1
    else
        found=0
    fi
    if [ "$found" -ne "$expected" ]; then
        printf '%s: expected FORCE prerequisite %s, got %s\n' \
            "$name" "$expected" "$found" >&2
        failures=$((failures + 1))
    fi
}

make_fixture shipped "$ROOT/src/corelink.pri"
check_fixture shipped 1

unforced="$tmp/unforced.pri"
grep -v 'core_library.depends = FORCE' "$ROOT/src/corelink.pri" > "$unforced"
if cmp -s "$unforced" "$ROOT/src/corelink.pri"; then
    printf 'corelink.pri no longer declares core_library.depends = FORCE\n' >&2
    failures=$((failures + 1))
fi
make_fixture unforced "$unforced"
check_fixture unforced 0

if [ "$failures" -ne 0 ]; then
    printf '%s corelink dependency case(s) failed\n' "$failures"
    exit 1
fi
printf 'corelink dependency self-test passed\n'
