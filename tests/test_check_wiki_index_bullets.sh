#!/bin/sh
set -eu

# Self-test for bin/check-wiki-index-bullets. Fixtures drive the index path, so
# the checker is exercised without touching the real (local-only) wiki index.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-wiki-index-bullets"
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

if [ "$failures" -ne 0 ]; then
    printf '%s check-wiki-index-bullets case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-wiki-index-bullets self-test passed\n'
