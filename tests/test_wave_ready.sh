#!/bin/sh
set -eu

# Self-test for bin/wave-ready. Every case builds a synthetic repository and
# worktree parent, so the real helper never touches a real ticket worktree.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
HELPER="$ROOT/bin/wave-ready"
SYNC="$ROOT/bin/sync-worktree-local-docs"
PARSER="$ROOT/bin/local-only-sync-paths.sh"
. "$ROOT/tests/checker_test_support.sh"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

run_ready() {
    case_name=$1
    shift
    set +e
    # Always invoke the fixture copy: the helper must never see real worktrees.
    ready_out=$("$src/bin/wave-ready" "$@" 2>"$tmp/$case_name.err")
    ready_status=$?
    set -e
    ready_err=$(cat "$tmp/$case_name.err")
}

prove_worktree() {
    case_name=$1
    id=$2
    want_head=$3
    path="$worktrees/ticket-$id"
    assert_equal "${case_name}_top_level" \
        "$(git -C "$path" rev-parse --show-toplevel 2>/dev/null || true)" "$path"
    assert_equal "${case_name}_head" \
        "$(git -C "$path" rev-parse HEAD 2>/dev/null || true)" "$want_head"
    assert_equal "${case_name}_tracked_clean" \
        "$(git -C "$path" status --porcelain --untracked-files=no 2>/dev/null || true)" ""
    assert_file "${case_name}_local_docs" "$path/AGENTS.md"
    assert_file "${case_name}_local_agent_docs" "$path/docs/agents/note.md"
}

src="$tmp/source"
worktrees="$tmp/worktrees"
mkdir -p "$src/bin" "$src/docs/agents" "$worktrees"

cat > "$src/.gitignore" <<'EOF'
/build/

# Local-only agent instructions; keep out of the remote.
/AGENTS.md
/docs/agents/
/reference/

# Local-only maintainer teaching workspace; keep out of the remote.
/MISSION.md
/assets/
/learning-records/
EOF

git -C "$src" init -q
git -C "$src" config user.name 'Wave Ready Test'
git -C "$src" config user.email 'wave-ready@example.test'
printf 'base\n' > "$src/tracked.txt"
cat > "$src/bin/lint" <<'EOF'
#!/bin/sh
# Fixture lint: read the synced docs, then fail on request.
[ -e docs/agents/note.md ] || exit 1
[ ! -e docs/agents/lint-fails ]
EOF
chmod +x "$src/bin/lint"
git -C "$src" add .gitignore tracked.txt bin/lint
git -C "$src" commit -q -m base
base=$(git -C "$src" rev-parse HEAD)
printf 'next\n' > "$src/tracked.txt"
git -C "$src" commit -qam next
other=$(git -C "$src" rev-parse HEAD)

printf 'local agent instructions\n' > "$src/AGENTS.md"
printf 'agent note\n' > "$src/docs/agents/note.md"

for script in wave-ready sync-worktree-local-docs local-only-sync-paths.sh; do
    cp "$ROOT/bin/$script" "$src/bin/$script"
    chmod +x "$src/bin/$script"
done

# Missing worktrees: three ids are created and proved in one call.
run_ready three_missing "$base" --root "$worktrees" \
    --suites '#201=SuiteA' --suites '#202=SuiteB,SuiteC' \
    --suites '203=SuiteD' 201 202 203
assert_status three_missing_status 0 "$ready_status" "$ready_out$ready_err"
assert_line_count three_missing_lines 3 "$ready_out"
assert_equal three_missing_no_errors "$ready_err" ""
for spec in 201:SuiteA 202:SuiteB,SuiteC 203:SuiteD; do
    id=${spec%%:*}
    suites=${spec#*:}
    assert_has "three_missing_line_$id" \
        "ticket-$id $worktrees/ticket-$id $suites" "$ready_out"
    prove_worktree "three_missing_$id" "$id" "$base"
done

short=$(git -C "$src" rev-parse --short=7 "$base")
fingerprint="$src/build/equivalence/$short.fingerprint"
assert_file fingerprint_captured "$fingerprint"
assert_line_count fingerprint_fields 6 "$(cat "$fingerprint")"
for field in 'qmake -v' 'g++ --version' 'Qt version' \
    "fc-match 'Noto Sans Math'" QT_QPA_PLATFORM locale; do
    assert_file_has_text "fingerprint_field_$field" "$fingerprint" "$field"
done

# Existing correct worktree: reuse it instead of trying to create it again.
git -C "$src" worktree add -q -b ticket-204 "$worktrees/ticket-204" "$base"
before=$(git -C "$src" worktree list --porcelain | grep -c '^worktree ' || true)
run_ready existing_correct "$base" --root "$worktrees" \
    --suites '#204=SuiteE' 204
assert_status existing_correct_status 0 "$ready_status" \
    "$ready_out$ready_err"
assert_has existing_correct_line \
    "ticket-204 $worktrees/ticket-204 SuiteE" "$ready_out"
after=$(git -C "$src" worktree list --porcelain | grep -c '^worktree ' || true)
assert_equal existing_correct_reused "$after" "$before"
prove_worktree existing_correct 204 "$base"

# Wrong HEAD: report and leave the worktree untouched.
git -C "$src" worktree add -q -b ticket-205 "$worktrees/ticket-205" "$other"
run_ready wrong_head "$base" --root "$worktrees" --suites '#205=SuiteF' 205
assert_status wrong_head_status 1 "$ready_status" "$ready_out$ready_err"
assert_equal wrong_head_stdout "$ready_out" ""
assert_has wrong_head_error 'ERROR ticket-205 wrong HEAD' "$ready_err"
assert_equal wrong_head_unchanged \
    "$(git -C "$worktrees/ticket-205" rev-parse HEAD)" "$other"
assert_absent_file wrong_head_not_synced \
    "$worktrees/ticket-205/AGENTS.md"

# Dirty tracked checkout: report and leave the tracked edit in place.
git -C "$src" worktree add -q -b ticket-206 "$worktrees/ticket-206" "$base"
printf 'dirty\n' >> "$worktrees/ticket-206/tracked.txt"
run_ready dirty_checkout "$base" --root "$worktrees" \
    --suites '#206=SuiteG' 206
assert_status dirty_checkout_status 1 "$ready_status" \
    "$ready_out$ready_err"
assert_equal dirty_checkout_stdout "$ready_out" ""
assert_has dirty_checkout_error \
    'ERROR ticket-206 dirty tracked checkout' "$ready_err"
assert_has dirty_checkout_untouched dirty \
    "$(cat "$worktrees/ticket-206/tracked.txt")"
assert_absent_file dirty_checkout_not_synced \
    "$worktrees/ticket-206/AGENTS.md"

# A stale branch at the wrong SHA is reported without creating a worktree.
git -C "$src" branch ticket-207 "$other"
run_ready stale_branch "$base" --root "$worktrees" \
    --suites '#207=SuiteH' 207
assert_status stale_branch_status 1 "$ready_status" "$ready_out$ready_err"
assert_equal stale_branch_stdout "$ready_out" ""
assert_has stale_branch_error \
    "ERROR ticket-207 branch ticket-207 exists at $other" "$ready_err"
assert_absent_file stale_branch_no_worktree "$worktrees/ticket-207"

# Stray untracked file: reject it by name, then promote once it is gone. A
# synced local-only doc stays untracked and is allowed.
git -C "$src" worktree add -q -b ticket-208 "$worktrees/ticket-208" "$base"
printf 'stray\n' > "$worktrees/ticket-208/stray.txt"
run_ready stray_untracked "$base" --root "$worktrees" \
    --suites '#208=SuiteI' 208
assert_status stray_untracked_status 1 "$ready_status" \
    "$ready_out$ready_err"
assert_equal stray_untracked_stdout "$ready_out" ""
assert_has stray_untracked_error \
    'ERROR ticket-208 untracked file outside Local-only allowlist: stray.txt' \
    "$ready_err"
assert_file_has_text stray_untracked_untouched \
    "$worktrees/ticket-208/stray.txt" 'stray'
rm "$worktrees/ticket-208/stray.txt"
run_ready stray_removed "$base" --root "$worktrees" \
    --suites '#208=SuiteI' 208
assert_status stray_removed_status 0 "$ready_status" "$ready_out$ready_err"
assert_has stray_removed_line \
    "ticket-208 $worktrees/ticket-208 SuiteI" "$ready_out"
assert_file stray_removed_local_doc \
    "$worktrees/ticket-208/docs/agents/note.md"

# Lint failure: an allowlisted marker makes this worktree's lint fail.
git -C "$src" worktree add -q -b ticket-209 "$worktrees/ticket-209" "$base"
run_ready lint_setup "$base" --root "$worktrees" --suites '#209=SuiteJ' 209
assert_status lint_setup_status 0 "$ready_status" "$ready_out$ready_err"
: > "$worktrees/ticket-209/docs/agents/lint-fails"
run_ready lint_failure "$base" --root "$worktrees" --suites '#209=SuiteJ' 209
assert_status lint_failure_status 1 "$ready_status" "$ready_out$ready_err"
assert_equal lint_failure_stdout "$ready_out" ""
assert_has lint_failure_error \
    'ERROR ticket-209 ./bin/lint failed in worktree' "$ready_err"
assert_file lint_failure_untouched \
    "$worktrees/ticket-209/docs/agents/lint-fails"

# Fingerprint mismatch: the error names the field that differs.
git -C "$src" worktree add -q -b ticket-210 "$worktrees/ticket-210" "$base"
awk -F '\t' -v OFS='\t' '
    $1 == "QT_QPA_PLATFORM" { $2 = "CHANGED"; print; next }
    { print }
' "$fingerprint" > "$tmp/fingerprint.changed"
mv "$tmp/fingerprint.changed" "$fingerprint"
run_ready fingerprint_mismatch "$base" --root "$worktrees" \
    --suites '#210=SuiteK' 210
assert_status fingerprint_mismatch_status 1 "$ready_status" \
    "$ready_out$ready_err"
assert_equal fingerprint_mismatch_stdout "$ready_out" ""
assert_has fingerprint_mismatch_error \
    'ERROR ticket-210 toolchain fingerprint mismatch: QT_QPA_PLATFORM' \
    "$ready_err"
assert_file fingerprint_mismatch_untouched \
    "$worktrees/ticket-210/tracked.txt"

if [ "$failures" -ne 0 ]; then
    printf '%s wave-ready case(s) failed\n' "$failures"
    exit 1
fi
printf 'wave-ready self-test passed\n'
