#!/bin/sh
set -eu

# Self-test for bin/check-wiki-index-bullets. Fixtures drive the index path, so
# the checker is exercised without touching the real (local-only) wiki index.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-wiki-index-bullets"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

expect_result() {
    name=$1
    want_status=$2
    want_text=$3
    forbidden=${4:-}
    shift 4
    set +e
    output="$("$CHECKER" "$@" 2>&1)"
    got_status=$?
    set -e
    last_output=$output
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
    if [ -n "$forbidden" ]; then
        case "$output" in
            *"$forbidden"*)
                printf 'FAIL %s: output unexpectedly contained %s\n%s\n' \
                    "$name" "$forbidden" "$output"
                failures=$((failures + 1))
                return
                ;;
        esac
    fi
    printf 'PASS %s\n' "$name"
}

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
