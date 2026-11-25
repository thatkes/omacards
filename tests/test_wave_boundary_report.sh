#!/bin/sh
set -eu

# Self-test for bin/wave-boundary-report. All tracker reads are fixtures: a
# fake gh intercepting PATH proves the fixture path makes no gh call and a live
# path makes exactly one issue-list call.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
REPORT="$ROOT/bin/wave-boundary-report"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

mkdir -p "$tmp/bin"
cat > "$tmp/bin/gh" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$WBR_GH_LOG"
case "$*" in
    "issue list --state open --limit 100 --json number,title,labels,author,blockedBy")
        cat "$WBR_FIXTURE"
        ;;
    *)
        printf 'unexpected gh call: %s\n' "$*" >&2
        exit 99
        ;;
esac
EOF
chmod +x "$tmp/bin/gh"
PATH="$tmp/bin:$PATH"
export PATH
export WBR_GH_LOG="$tmp/gh-calls"

reset_gh_log() {
    : > "$WBR_GH_LOG"
}

gh_call_count() {
    wc -l < "$WBR_GH_LOG" | tr -d '[:space:]'
}

cat > "$tmp/clean.json" <<'EOF'
[
  {"number":11,"title":"Clean issue","labels":[{"name":"ready-for-agent"}],"author":{"login":"alice"},"blockedBy":{"nodes":[]}},
  {"number":204,"title":"Wave report","labels":[],"author":null,"blockedBy":{"nodes":[{"number":99,"state":"CLOSED"}]}}
]
EOF

cat > "$tmp/blocked.json" <<'EOF'
[
  {"number":11,"title":"Clean issue","labels":[{"name":"ready-for-agent"}],"author":{"login":"alice"},"blockedBy":{"nodes":[]}},
  {"number":207,"title":"Blocked issue","labels":[{"name":"ready-for-agent"}],"author":{"login":"bob"},"blockedBy":{"nodes":[{"number":301,"state":"OPEN"}]}}
]
EOF

cat > "$tmp/ledger.md" <<'EOF'
<!-- ledger-current-ids-start -->
| id | state | worktree | merged at / reason |
| --- | --- | --- | --- |
| #11 | `queued` | — | keep |
| #204 | `ready-for-agent` | — | keep |
<!-- ledger-current-ids-end -->
EOF

cat > "$tmp/ledger-blocked.md" <<'EOF'
<!-- ledger-current-ids-start -->
| id | state | worktree | merged at / reason |
| --- | --- | --- | --- |
| #11 | `queued` | — | keep |
| #207 | `ready-for-agent` | — | keep |
<!-- ledger-current-ids-end -->
EOF

grep -v '^| #204 ' "$tmp/ledger.md" > "$tmp/ledger-mismatch.md"
printf '#207\n' > "$tmp/scope.txt"

export WBR_FIXTURE="$tmp/clean.json"
reset_gh_log
expect_command_result clean_fixture_prints_all_sections 0 'OPEN_IDS' '' \
    "$REPORT" --issues-file "$tmp/clean.json" --ledger "$tmp/ledger.md"
assert_has clean_ledger_ok 'LEDGER OK 2 open ids' "$last_output"
assert_has clean_closed_blocker_counts_zero '#204 0' "$last_output"
assert_has clean_author_and_labels 'labels=ready-for-agent' "$last_output"
assert_has clean_status_section 'STATUS' "$last_output"
assert_has clean_worktrees_section 'WORKTREES' "$last_output"
assert_equal clean_fixture_makes_no_gh_call "$(gh_call_count)" 0

export WBR_FIXTURE="$tmp/clean.json"
reset_gh_log
expect_command_result one_tracker_read_without_fixture 0 'LEDGER OK 2 open ids' '' \
    "$REPORT" --ledger "$tmp/ledger.md"
assert_equal exactly_one_gh_call "$(gh_call_count)" 1
assert_file_has_text single_issue_list_read "$WBR_GH_LOG" \
    'issue list --state open --limit 100 --json number,title,labels,author,blockedBy'
assert_file_lacks_text no_per_id_gh_call "$WBR_GH_LOG" 'issue view'

export WBR_FIXTURE="$tmp/blocked.json"
reset_gh_log
expect_command_result open_blocker_fails_scope_check 1 'BLOCKERS' '' \
    "$REPORT" --issues-file "$tmp/blocked.json" --ledger "$tmp/ledger-blocked.md" \
    --scope-file "$tmp/scope.txt" --check
assert_has open_blocker_counts_one '#207 1' "$last_output"
assert_has open_blocker_ledger_ok 'LEDGER OK 2 open ids' "$last_output"
assert_has open_blocker_prints_status 'STATUS' "$last_output"
assert_has open_blocker_prints_worktrees 'WORKTREES' "$last_output"
assert_equal open_blocker_fixture_makes_no_gh_call "$(gh_call_count)" 0

export WBR_FIXTURE="$tmp/clean.json"
reset_gh_log
expect_command_result ledger_mismatch_fails_check 1 'LEDGER MISMATCH' '' \
    "$REPORT" --issues-file "$tmp/clean.json" --ledger "$tmp/ledger-mismatch.md" \
    --scope-file "$tmp/scope.txt" --check
assert_has mismatch_prints_status 'STATUS' "$last_output"
assert_has mismatch_prints_worktrees 'WORKTREES' "$last_output"
assert_equal mismatch_fixture_makes_no_gh_call "$(gh_call_count)" 0

reset_gh_log
expect_command_result mismatch_without_check_exits_zero 0 'LEDGER MISMATCH' '' \
    "$REPORT" --issues-file "$tmp/clean.json" --ledger "$tmp/ledger-mismatch.md"

if [ "$failures" -ne 0 ]; then
    printf '%s wave-boundary-report case(s) failed\n' "$failures"
    exit 1
fi
printf 'wave-boundary-report self-test passed\n'
