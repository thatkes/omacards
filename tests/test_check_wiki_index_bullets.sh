#!/bin/sh
set -eu

# Self-test for bin/check-wiki-index-bullets. Fixtures drive the index path, so
# the checker is exercised without touching the real (local-only) wiki index.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-wiki-index-bullets"
FIXTURE="$ROOT/tests/wiki-index-fixture.md"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

cat > "$tmp/short.md" <<'EOF'
# Fixture index

- short
- also short
EOF

expect_result all_bullets_within_cap 0 'WIKI INDEX OK 2 bullets within 20' \
    'TOO LONG' --index "$tmp/short.md" --max 20

cat > "$tmp/over-cap.md" <<'EOF'
# Fixture index

- 1234567890
EOF

expect_result over_cap_fails_with_line_and_count 1 'line 3: 10 characters' \
    'WIKI INDEX OK' --index "$tmp/over-cap.md" --max 8

expect_result absent_index_skips 0 'skipping' \
    'WIKI INDEX OK' --index "$tmp/not-there.md" --max 8

cat > "$tmp/multi-line.md" <<'EOF'
# Fixture index

- 12345
  67890
EOF

expect_result continuation_counts_as_one_bullet 1 'line 3: 11 characters' \
    'line 4:' --index "$tmp/multi-line.md" --max 8

# bin/lint runs this self-test on every run. Checking the tracked fixture with
# the checker's default cap makes a bullet that grows past 150 characters fail
# lint, without depending on the gitignored live index.
expect_result tracked_fixture_within_default_cap 0 \
    'WIKI INDEX OK 3 bullets within 150 characters' 'skipping' \
    --index "$FIXTURE"

# Grow a copy of the tracked fixture past the default cap. This is the failure
# the guard above would report if that fixture were edited to exceed 150.
{
    cat "$FIXTURE"
    awk 'BEGIN { printf "- "; for (i = 0; i < 151; i++) printf "x"; printf "\n" }'
} > "$tmp/grown-fixture.md"
expect_result grown_fixture_exceeds_default_cap 1 \
    '151 characters (max 150)' 'WIKI INDEX OK' \
    --index "$tmp/grown-fixture.md"

if [ "$failures" -ne 0 ]; then
    printf '%s check-wiki-index-bullets case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-wiki-index-bullets self-test passed\n'
