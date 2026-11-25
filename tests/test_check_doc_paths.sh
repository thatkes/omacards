#!/bin/sh
set -eu

# Self-test for bin/check-doc-paths. Fixtures drive both fenced-block parsers
# and the Layout matcher, so no tracked doc or source file is modified.
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CHECKER="$ROOT/bin/check-doc-paths"
. "$ROOT/tests/checker_test_support.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

failures=0

populate_root() {
    root=$1
    mkdir -p "$root/src/skill" "$root/src/i18n" "$root/tests" \
        "$root/pkgbuild"
    cat > "$root/src/command-paths.txt" <<'EOF'
--help
-h
check
decks
decks current
help
skill
skill install
EOF
    cat > "$root/README.md" <<'EOF'
## Agent CLI

The CLI is built for agents and works from a shell too.

```sh
omacards check [--json]
omacards decks [--json]
omacards decks current [--json]
omacards help  # aliases: --help, -h
omacards skill <subcommand> [--json]
omacards skill install [--json]
```

## Layout

```text
src/backend.{h,cpp}  facade
src/theme.* src/translator.*  theme
src/i18n/  catalogue
tests/  suite
```

Cloze rules: add-card uses [--type <basic|cloze>] with lowercase type values.
A marker alone sets the type; an explicit --type wins. The answer may be
omitted when the marker supplies it. {{c1::answer}} reveals at display time,
multiple markers keep their numbers, and {{c1::answer::hint}} drops ::hint for
Anki imports and hand-written cards.
EOF
    cat > "$root/src/skill/SKILL.md" <<'EOF'
## Command list

```sh
omacards check [--json]
omacards decks [--json]
omacards decks current [--json]
omacards help  # aliases: --help, -h
omacards skill <subcommand> [--json]
omacards skill install [--json]
```

Cloze rules: add-card uses [--type <basic|cloze>] with lowercase type values.
A marker alone sets the type; an explicit --type wins. The answer may be
omitted when the marker supplies it. {{c1::answer}} reveals at display time,
multiple markers keep their numbers, and {{c1::answer::hint}} drops ::hint for
Anki imports and hand-written cards.
EOF
    : > "$root/src/backend.h"
    : > "$root/src/backend.cpp"
    : > "$root/src/theme.h"
    : > "$root/src/theme.cpp"
    : > "$root/src/translator.h"
    : > "$root/src/translator.cpp"
    : > "$root/src/i18n/en.json"
    cat > "$root/src/command-help.txt" <<'EOF'
# Help wording for the command-path registry.
Path: check
Purpose: Show deck issues.
Flags: [--json], --help, -h
Example: omacards check --json

Path: decks
Purpose: List decks.
Flags: [--json], --help, -h
Example: omacards decks --json

Path: decks current
Purpose: Show the Current deck.
Flags: [--json], --help, -h
Example: omacards decks current --json

Path: skill
Purpose: Manage the agent skill.
Flags: [install], [--json], --help, -h
Example: omacards skill --help

Path: skill install
Purpose: Install the agent skill.
Flags: [--json], --help, -h
Example: omacards skill install --json
EOF
    mkdir -p "$root/reference" "$root/lessons"
    cat > "$root/reference/0001-sample.html" <<'EOF'
<p>See <code>Cli::run()</code> in src/cli.cpp.</p>
EOF
    cat > "$root/lessons/0001-sample.html" <<'EOF'
<p>See <code>Cli::write()</code> in src/cli.cpp.</p>
EOF
}

matching="$tmp/matching"
make_root "$matching"
expect_result matching_docs_pass 0 'Doc references OK' "$matching" 'FAIL'

# Every command in the three-document contract must appear in each document.
missing_skill_command="$tmp/missing-skill-command"
make_root "$missing_skill_command"
sed '/^omacards skill install /d' \
    "$missing_skill_command/src/skill/SKILL.md" > "$tmp/no-skill-command.md"
mv "$tmp/no-skill-command.md" "$missing_skill_command/src/skill/SKILL.md"
expect_result command_missing_from_one_doc_fails 1 \
    'SKILL.md: command missing from doc contract: skill install' \
    "$missing_skill_command" 'Doc references OK'

# Flag sets are compared per command, in both directions, across all three.
missing_readme_flag="$tmp/missing-readme-flag"
make_root "$missing_readme_flag"
sed 's/^omacards skill install \[--json\]$/omacards skill install/' \
    "$missing_readme_flag/README.md" > "$tmp/no-install-json.md"
mv "$tmp/no-install-json.md" "$missing_readme_flag/README.md"
expect_result flag_missing_from_one_doc_fails 1 \
    'README.md: command skill install is missing flag --json' \
    "$missing_readme_flag" 'Doc references OK'

extra_skill_flag="$tmp/extra-skill-flag"
make_root "$extra_skill_flag"
sed 's/^omacards decks \[--json\]$/omacards decks [--json] [--strict]/' \
    "$extra_skill_flag/src/skill/SKILL.md" > "$tmp/extra-decks-flag.md"
mv "$tmp/extra-decks-flag.md" "$extra_skill_flag/src/skill/SKILL.md"
expect_result flag_extra_in_one_doc_fails 1 \
    'SKILL.md: command decks has extra flag --strict' \
    "$extra_skill_flag" 'Doc references OK'

missing_help_flag="$tmp/missing-help-flag"
make_root "$missing_help_flag"
awk '
    /^Path: / { path = $2 }
    /^Flags: / && path == "check" { print "Flags: --help, -h"; next }
    { print }
' "$missing_help_flag/src/command-help.txt" > "$tmp/no-check-json.txt"
mv "$tmp/no-check-json.txt" "$missing_help_flag/src/command-help.txt"
expect_result flag_missing_from_help_fails 1 \
    'src/command-help.txt: command check is missing flag --json' \
    "$missing_help_flag" 'Doc references OK'

# The cloze contract is checked by tokens, not by exact prose.
no_cloze_hint="$tmp/no-cloze-hint"
make_root "$no_cloze_hint"
sed 's/{{c1::answer::hint}}/{{c1::answer}}/g' \
    "$no_cloze_hint/src/skill/SKILL.md" > "$tmp/no-cloze-hint.md"
mv "$tmp/no-cloze-hint.md" "$no_cloze_hint/src/skill/SKILL.md"
expect_result cloze_hint_rule_required 1 \
    'SKILL.md: missing cloze contract text: {{c1::answer::hint}}' \
    "$no_cloze_hint" 'Doc references OK'

# Teaching pages point at symbols, not line numbers.
numbered_reference="$tmp/numbered-reference"
make_root "$numbered_reference"
cat > "$numbered_reference/reference/0001-sample.html" <<'EOF'
<p><a href="../src/cli.cpp#L12">Cli::run()</a></p>
EOF
expect_result numeric_reference_anchor_fails 1 \
    'reference/0001-sample.html:1: numeric source anchor: #L12' \
    "$numbered_reference" 'Doc references OK'

numbered_lesson="$tmp/numbered-lesson"
make_root "$numbered_lesson"
cat > "$numbered_lesson/lessons/0001-sample.html" <<'EOF'
<p>Cli::write() is in src/cli.cpp:12.</p>
EOF
expect_result numeric_lesson_anchor_fails 1 \
    'lessons/0001-sample.html:1: numeric source anchor: src/cli.cpp:12' \
    "$numbered_lesson" 'Doc references OK'

# A bare subcommand that is not in the registry is a drift regression, not
# an argument to be ignored.
unknown_readme="$tmp/unknown-readme"
make_root "$unknown_readme"
awk '{ print; if ($0 ~ /decks current/) print "omacards decks active [--json]" }' \
    "$unknown_readme/README.md" > "$tmp/unknown-readme.md"
mv "$tmp/unknown-readme.md" "$unknown_readme/README.md"
expect_result readme_unknown_path_reports_line 1 \
    'README.md:9: command path not in src/command-paths.txt: decks active' \
    "$unknown_readme" 'Doc references OK'

unknown_skill="$tmp/unknown-skill"
make_root "$unknown_skill"
awk '{ print; if ($0 ~ /decks current/) print "omacards help active" }' \
    "$unknown_skill/src/skill/SKILL.md" > "$tmp/unknown-skill.md"
mv "$tmp/unknown-skill.md" "$unknown_skill/src/skill/SKILL.md"
expect_result skill_unknown_path_reports_line 1 \
    'SKILL.md:7: command path not in src/command-paths.txt: help active' \
    "$unknown_skill" 'Doc references OK'

undocumented="$tmp/undocumented"
make_root "$undocumented"
sed '/omacards skill install/d' "$undocumented/README.md" > "$tmp/no-install-readme.md"
mv "$tmp/no-install-readme.md" "$undocumented/README.md"
sed '/omacards skill install/d' "$undocumented/src/skill/SKILL.md" > "$tmp/no-install-skill.md"
mv "$tmp/no-install-skill.md" "$undocumented/src/skill/SKILL.md"
expect_result registry_path_missing_from_both_blocks 1 \
    'Registry command path not documented in README Agent CLI or skill Command list: skill install' \
    "$undocumented" 'Doc references OK'

no_aliases="$tmp/no-aliases"
make_root "$no_aliases"
sed 's/  # aliases: --help, -h//' "$no_aliases/README.md" > "$tmp/no-alias-readme.md"
mv "$tmp/no-alias-readme.md" "$no_aliases/README.md"
sed 's/  # aliases: --help, -h//' "$no_aliases/src/skill/SKILL.md" > "$tmp/no-alias-skill.md"
mv "$tmp/no-alias-skill.md" "$no_aliases/src/skill/SKILL.md"
expect_result aliases_without_comment_are_undocumented 1 \
    'Registry command path not documented in README Agent CLI or skill Command list: --help' \
    "$no_aliases" 'Doc references OK'

markdown="$tmp/markdown"
make_root "$markdown"
sed 's/^## Command list$/## Common commands/' "$markdown/src/skill/SKILL.md" \
    > "$tmp/old-heading.md"
mv "$tmp/old-heading.md" "$markdown/src/skill/SKILL.md"
expect_result skill_command_list_heading_required 1 '## Command list' \
    "$markdown" 'Doc references OK'

missing_brace="$tmp/missing-brace"
make_root "$missing_brace"
rm "$missing_brace/src/backend.cpp"
expect_result brace_alternative_must_exist 1 \
    'README.md:17: missing Layout path: src/backend.{h,cpp}' \
    "$missing_brace" 'Doc references OK'

missing_glob="$tmp/missing-glob"
make_root "$missing_glob"
rm "$missing_glob/src/theme.h" "$missing_glob/src/theme.cpp"
expect_result glob_without_match_fails 1 \
    'README.md:18: missing Layout path: src/theme.*' \
    "$missing_glob" 'Doc references OK'

matched_glob="$tmp/matched-glob"
make_root "$matched_glob"
sed 's|src/theme\.\*|src/theme.h|' "$matched_glob/README.md" > "$tmp/glob-token.md"
mv "$tmp/glob-token.md" "$matched_glob/README.md"
expect_result concrete_path_instead_of_glob_passes 0 'Doc references OK' \
    "$matched_glob" 'FAIL'

# A Layout path token after prose is still a path and must exist.
stale_after_prose="$tmp/stale-after-prose"
make_root "$stale_after_prose"
awk '{ print; if ($1 == "tests/") print "notes src/stale.cpp" }' \
    "$stale_after_prose/README.md" > "$tmp/stale-layout.md"
mv "$tmp/stale-layout.md" "$stale_after_prose/README.md"
expect_result stale_layout_path_after_prose_reported 1 \
    'README.md:21: missing Layout path: src/stale.cpp' \
    "$stale_after_prose" 'Doc references OK'

# Help metadata is parsed from src/command-help.txt as text. A valid file must
# stay clean, and a malformed block must fail even when the docs are correct.
valid_help_metadata="$tmp/valid-help-metadata"
make_root "$valid_help_metadata"
cat > "$valid_help_metadata/src/command-help.txt" <<'EOF'
# Help wording for the command-path registry.
Path: check
Purpose: Show deck issues.
Flags: [--json], --help, -h
Example: omacards check --help

Path: decks
Purpose: List decks.
Flags: [--json], --help, -h
Example: omacards decks --json

Path: decks current
Purpose: Show the Current deck.
Flags: [--json], --help, -h
Example: omacards decks current --json

Path: skill
Purpose: Manage the agent skill.
Flags: [install], [--json], --help, -h
Example: omacards skill --help

Path: skill install
Purpose: Install the agent skill.
Flags: [--json], --help, -h
Example: omacards skill install --json
EOF
expect_result valid_help_metadata_passes 0 'Doc references OK' \
    "$valid_help_metadata" 'FAIL'

unknown_help_path="$tmp/unknown-help-path"
make_root "$unknown_help_path"
cat > "$unknown_help_path/src/command-help.txt" <<'EOF'
Path: mystery
Purpose: Do something.
Flags: --help, -h
Example: omacards mystery --help
EOF
expect_result help_metadata_path_outside_registry_fails 1 \
    'src/command-help.txt:1: help metadata path not in src/command-paths.txt: mystery' \
    "$unknown_help_path" 'Doc references OK'

empty_help_field="$tmp/empty-help-field"
make_root "$empty_help_field"
cat > "$empty_help_field/src/command-help.txt" <<'EOF'
Path: check
Purpose:
Flags: --help, -h
Example: omacards check --help
EOF
expect_result help_metadata_empty_field_fails 1 \
    'src/command-help.txt:2: empty help metadata field: Purpose' \
    "$empty_help_field" 'Doc references OK'

missing_help_field="$tmp/missing-help-field"
make_root "$missing_help_field"
cat > "$missing_help_field/src/command-help.txt" <<'EOF'
Path: check
Purpose: Show deck issues.
Example: omacards check --help
EOF
expect_result help_metadata_missing_field_fails 1 \
    'src/command-help.txt:1: missing help metadata field: Flags' \
    "$missing_help_field" 'Doc references OK'

duplicate_help_path="$tmp/duplicate-help-path"
make_root "$duplicate_help_path"
cat > "$duplicate_help_path/src/command-help.txt" <<'EOF'
Path: check
Purpose: Show deck issues.
Flags: --help, -h
Example: omacards check --help

Path: check
Purpose: Another purpose.
Flags: --help, -h
Example: omacards check --help
EOF
expect_result help_metadata_duplicate_path_fails 1 \
    'src/command-help.txt:6: duplicate help metadata path: check' \
    "$duplicate_help_path" 'Doc references OK'

# Completeness: help paths route to usage() and need no block, but every other
# registry path must have one even when the blocks that do exist are valid.
complete_help_metadata="$tmp/complete-help-metadata"
make_root "$complete_help_metadata"
cat > "$complete_help_metadata/src/command-help.txt" <<'EOF'
Path: check
Purpose: Show deck issues.
Flags: [--json], --help, -h
Example: omacards check --help

Path: decks
Purpose: List decks.
Flags: [--json], --help, -h
Example: omacards decks --json

Path: decks current
Purpose: Show the Current deck.
Flags: [--json], --help, -h
Example: omacards decks current --json

Path: skill
Purpose: Manage the agent skill.
Flags: [install], [--json], --help, -h
Example: omacards skill --help

Path: skill install
Purpose: Install the agent skill.
Flags: [--json], --help, -h
Example: omacards skill install --json
EOF
expect_result help_metadata_covers_registry_without_help_paths 0 'Doc references OK' \
    "$complete_help_metadata" 'FAIL'

incomplete_help_metadata="$tmp/incomplete-help-metadata"
make_root "$incomplete_help_metadata"
sed '/^Path: skill install$/,$d' "$complete_help_metadata/src/command-help.txt" \
    > "$incomplete_help_metadata/src/command-help.txt"
expect_result help_metadata_missing_command_path_fails 1 \
    'src/command-help.txt: missing help metadata for command path: skill install' \
    "$incomplete_help_metadata" 'Doc references OK'

if [ "$failures" -ne 0 ]; then
    printf '%s check-doc-paths case(s) failed\n' "$failures"
    exit 1
fi
printf 'check-doc-paths self-test passed\n'
