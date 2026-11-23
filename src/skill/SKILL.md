---
name: omacards
description: Create and maintain omacards decks, cards and imports through the headless CLI. Use when asked to add flashcards, create or bulk-author a deck, preview card rendering, validate a deck, import Anki/CSV, check due cards, or simulate cram sessions.
---

# omacards

`omacards` is the Omarchy flashcards app. Deck files are plain text under
`~/.local/share/omacards/decks/` (or `$OMACARDS_DECKS_DIR`), one `.txt` file
per deck. Scheduling state lives in a SQLite sidecar (or `$OMACARDS_STATE_DB`)
and is regenerated if a card's content changes.

## Rules

- Prefer the CLI over editing the SQLite database.
- Headless commands accept `--json` and exit 1 on failure. Without it, errors
  go to stderr; with it, failures print `{"ok":false,"error":"..."}` to stdout
  and leave stderr empty. `help` / `--help` and `open` ignore `--json`.
- Deck names and card text are the source of truth. Keep questions short.
- Deck files are plain text: loaders and linter accept `.txt` and `.deck`, while
  new writes use `.txt`. All JSON fields are additive.
- `.apkg` imports need the external `unzip` command, keep Anki cloze types and
  tags but drop cloze hints, and put media in `media/<safe-name>/` beside the
  deck files (`safe-name` is `DeckIO::safeFileName(deckName)`).

## Finding the CLI

```sh
command -v omacards
```

- If `omacards` is on `PATH`, use it directly.
- If it is missing, either run `bin/link` in a source checkout (this builds,
  symlinks the command, installs the launcher entry, icon and agent skill), or
  invoke `<checkout>/build/omacards` directly.
- `OMACARDS_BIN` is only a convention for your own scripts; no repo script
  reads it. For scripts: `BIN="${OMACARDS_BIN:-omacards}"; "$BIN" check --json`.
- Use `./bin/with-temp-env omacards <command>` for isolated probes in a
  checkout. It points decks, state and cache at a temporary directory removed
  on exit. Without a checkout, use the recipe in **Isolated probes** below.
- `bin/link` also registers the desktop launcher entry; see **Development**.

### Isolated probes (no checkout needed)

Set all three paths before probing; no checkout, build or helper script is
involved:

```sh
probe="$(mktemp -d)"
trap 'rm -rf "$probe"' EXIT
export OMACARDS_DECKS_DIR="$probe/decks"
export OMACARDS_STATE_DB="$probe/state.db"
export XDG_CACHE_HOME="$probe/cache"
omacards decks --json
```

This recipe leaves `$OMACARDS_DECKS_DIR` absent. Because the directory does
not exist yet, the first run creates it and seeds a `Welcome` Deck with two
Cards, so probes start from a known non-empty state. If the directory already
exists, seeding is skipped and the Deck list is empty; the checkout helper
`./bin/with-temp-env` takes that case because it pre-creates the directory.

## Command list

```sh
omacards add-card [--deck <deck>] -q <question> -a <answer> [--tags <tags>] [--json]
omacards add-cards [--deck <deck>] [--description <text>] --input <file> [--json]
omacards add-deck <deck> [--description <text>] [--json]
omacards cards [--deck <deck>] [--json]
omacards check [--deck <deck>] [--math] [--strict] [--json]
omacards config [--new-card-allowance <n>] [--day-start-hour <h>] [--retention <r>] [--json]
omacards cram [--deck <deck>] [--steps <n>] [--json]
omacards decks [--json]
omacards decks current [--json]
omacards delete-card -c <card-id> [--json]
omacards delete-deck <deck> [--json]
omacards due [--deck <deck>] [--json]
omacards edit-card -c <card-id> -a <answer> [--type <type>] [--tags <tags>] [--json]
omacards help  # aliases: --help, -h
omacards import <file> [--deck <deck>] [--json]
omacards open  # start the GUI; ignores --json
omacards preview [--deck <deck>] [--card <card-id>] [--format <format>] [--json]
omacards rename-deck <old-name> <new-name> [--json]
omacards review -c <card-id> -r <rating> [--json]
omacards search <term> [--deck <deck>] [--json]
omacards skill <subcommand> [--json]
omacards skill install [--json]
omacards stats [--json]
omacards use-deck <deck> [--json]
omacards warm [--deck <deck>] [--json]
```

`add-card` without `--deck` writes to the Current deck, so check
`omacards decks current --json` first.

## Bulk deck creation (preferred)

For any deck larger than a couple of cards, create it in one atomic call with
`add-cards`. It validates every card first: invalid input writes nothing, and
the command creates the deck (and optional description) if it does not exist.

```sh
omacards add-cards --deck "Calculus" \
  --description "Maclaurin and Taylor series" \
  --input cards.json --json
# returns {"ok":true,"deck":"Calculus","created":N,"ids":[...],
#          "path":".../Calculus.txt","issueCounts":{"error":0,"warning":0}}
```

`path` is the stored deck file, the same value `decks --json` reports.
`issueCounts` covers only this deck after the write, with the same severity
keys as `check --json`.

`cards.json` accepts either a bare array or an object with a `cards` array and
an optional `description`. Each card supports `question`, `answer`, `type`
(`basic` or `cloze`), and `tags` (array or comma-separated string):

```json
{
  "description": "Maclaurin and Taylor series",
  "cards": [
    {"question": "Expand \\(e^x\\) about 0.", "answer": "\\(\\sum_{n=0}^{\\infty} x^n/n!\\)", "tags": ["series", "exp"]},
    {"question": "The {{c1::cosine}} series contains only even powers.", "type": "cloze", "tags": "series, trig"}
  ]
}
```

The input file can also be positional:
`omacards add-cards --deck Calculus cards.json --json`. Use `--input -` (or
pipe stdin with no `--input`) to read JSON from standard input. With a TTY and
no input file, the command fails clearly instead of blocking. `import FILE.json`
routes to the same parser and derives the deck name from the filename unless
`--deck` is given.

## Deck creation and descriptions

`add-card --deck NEW` creates the Deck if it does not exist, so a one-off Card
needs no separate `add-deck`:

```sh
omacards add-card --deck BrandNew -q 'Question?' -a 'Answer' --json
```

`add-cards --deck NAME --description TEXT` updates an existing Deck's
description as well as adding the Cards. On a new Deck, the same flag sets the
initial description.

## Worked multi-line Answer recipes

For a one-off Card, use ANSI-C single quotes so `\n` becomes a real newline.
Plain single quotes leave `\n` as a literal backslash and n; the Answer then
contains two characters instead of a line break. Double LaTeX backslashes
inside `$'...'` (`\\(` for `\(`, `\\frac` for `\frac`) because bash otherwise
reads `\f` and `\t` as its own escapes:

```sh
omacards add-card --deck Calculus \
  -q 'Write the geometric series.' \
  -a $'For |x| < 1:\n\\(\\sum_{n=0}^{\\infty} x^n = \\frac{1}{1-x}\\)' \
  --json
```

For `add-cards`, write the line break as the JSON escape `\n` inside the Answer
string and keep LaTeX backslashes doubled for JSON. In `cards.json`:

```json
{
  "cards": [
    {
      "question": "Write the geometric series.",
      "answer": "For |x| < 1:\n\\(\\sum_{n=0}^{\\infty} x^n = \\frac{1}{1-x}\\)"
    }
  ]
}
```

```sh
omacards add-cards --deck Calculus --input cards.json --json
```

The same JSON works on stdin. A quoted heredoc leaves the backslashes for the
JSON parser:

```sh
omacards add-cards --deck Calculus --input - --json <<'JSON'
{"cards":[{"question":"Write the geometric series.","answer":"For |x| < 1:\n\\(\\sum_{n=0}^{\\infty} x^n = \\frac{1}{1-x}\\)"}]}
JSON
```

## Hand-edited deck format

When editing a `.txt` file directly, use this format:

```text
# Deck: Spanish
# Description: Vocabulary and phrases
---
Q: What is "gato"?
A: cat
tags: vocab, animals
---
Q: Expand \(\frac{1}{1-x}\) about 0
A: \(\sum_{n=0}^{\infty} x^n\), for \(|x|<1\)
---
Q: The {{c1::cat}} sleeps.
type: cloze
id: a1b2c3d4e5f6
```

- `# Deck:` names the deck, `# Description:` documents it.
- `---` separates cards.
- Keys: `Q:` / `A:` (question / answer), `Answer:` is an alias for `A:`,
  `type: cloze`, `tags: a, b`, and an explicit `id:`.
- **Continuation lines must start with exactly two spaces.** One leading space
  or three or more is reported as a Deck issue by `check`.
- A Card id is a hash of its Deck, question and answer: editing the question or
  answer changes it, while tag and type edits keep it. Add an explicit `id:`
  line to pin it across edits.

After editing a file by hand, run the **validation loop** below.

## Math notation

`$$...$$` and `\(...\)` are the supported delimiters. Single-dollar `$...$`
is not rendered: it stays literal in both text and HTML output.

No renderer is bundled, so math falls back to Unicode unless a toolchain is
detected on `PATH`: `tectonic` + `pdftocairo`, `latex` + `dvisvgm`, or
`pdflatex` + `pdftocairo`. Run `omacards check --json` on the machine you are
probing: it reports `"math":"LaTeX (SVG)"` when a toolchain is detected and
`"math":"Unicode fallback"` otherwise. Do not assume any particular toolchain
is installed. Use notation that survives the fallback:

- Fractions as `(a)/(b)` and powers as `x^(2n+1)` — these render legibly.
- Backslashes are consumed by each layer. In single-quoted shell arguments
  and deck files write one: `\(...\)`. In a shell double-quoted argument
  write `\\(`, for example `-a "Expand \\(x\\)"`, which reaches the card
  as `\(x\)`. In a JSON file write `\\(`; when that JSON sits inside a
  shell double-quoted argument, write `\\\\(` so the shell halves the
  backslashes before JSON parses them.
- Safe macros: `\sum`, `\infty`, `\frac`, `\binom`, `\sqrt`, `\sin`, `\cos`,
  `\ln`, `\pi`, `\pm`, `\leq`, `\to`, `\int`, `\cdots`, inside `\(...\)` or
  `$$...$$`. The fallback maps nested `\frac{a}{b}` to `(a)/(b)`,
  `\binom{r}{n}` to `C(r,n)`, and `\sqrt{1+x}` to `√(1+x)`. Unknown macros
  keep their name but lose the backslash in fallback output.
- Math warnings are Deck issues with `severity: warning` in `issues` even
  without `--math`. They leave `ok` true but make `--strict` exit 1. Adding
  `--math` also emits a `mathWarnings` array of
  `{card, deck, field, message}` entries.
- On Arch/Omarchy, install a toolchain with
  `sudo pacman -S tectonic poppler` (poppler provides `pdftocairo`), then
  prewarm the one-time Tectonic bundle download with
  `printf '\\documentclass{article}\n\\begin{document}$x^2$\\end{document}\n' > /tmp/warm.tex && tectonic --untrusted --outdir=/tmp /tmp/warm.tex`.
  Restart the app after installing. One neutral SVG per expression is cached
  under an `eq5-` expression-only key in `$XDG_CACHE_HOME/omacards/math` (or
  the generic cache location when that variable is unset) and recolored for
  the active theme in a data URI at display time.

`preview --format text` uses the Unicode fallback even when LaTeX is active;
`--format html` emits `<img>` references. Text items in the JSON have `id`,
`question` and `answer`; html items have `id`, `questionHtml` and `answerHtml`.
Check a card before authoring many similar ones.

The agent wiki page `docs/agents/wiki/subsystem-math-rendering.md` is a
historical pre-#36 snapshot (`eq4-` keys, per-theme SVGs written to disk).
Trust the code and this file for current keys and data-URI recoloring.

## Validation loop

After authoring or editing a deck:

```sh
omacards preview --deck "My Deck" --format text --json   # Unicode rendering
omacards check --json                                    # syntax + Deck issues
omacards check --math --json                             # also list mathWarnings
```

Read `issues` from `check --json`: each entry has `path`, optional `line` and
`card`, a `severity` (`error` or `warning`), `message`, and a concrete `hint`.
Math warnings are already there as Warnings; `issueCounts` summarizes Errors
and Warnings. `ok` is false only for Errors, but `--strict` exits 1 whenever
any Deck issue exists, including a warning. Fix line-numbered issues and re-run
until the deck is clean.

## Development

- `./bin/build` — compile the dev binary into `build/omacards`.
- `./bin/test` — run the QtTest suite.
- `./bin/link` — build, then register the command, launcher entry, icon and
  agent skill in `~/.local`; safe to re-run. Use it after changing the skill or
  desktop entry so the installed copies stay in sync.
- `src/skill/SKILL.md` is the source of truth for this skill. Edit it there,
  install with `omacards skill install` (or `bin/link`, which builds first),
  then run `./bin/check-skill-install` to confirm the installed copy matches.
  The generated `~/.agents/skills/omacards/SKILL.md` is not the source of truth;
  never edit it by hand.

## Ratings

- `1` = Again (forgot)
- `3` = Good (recalled)

## Scheduling settings

`omacards config` reads the New-card allowance, the Day-start hour and Desired
retention. Defaults are `20`, `0` and `0.9`. Use `--new-card-allowance N`
(`0..9999`), `--day-start-hour H` (`0..23`) and `--retention R` (`0.70..0.99`)
to change them. Pass `-1` for any of the three to leave it unchanged.

A card is due once the Study day containing its FSRS due time has begun:
`studyDay(due) <= studyDay(now)`. With `--day-start-hour 3`, a card due at
22:49 on November 19 is due from 03:00 on November 19, not the next day. The
same boundary governs Ratings on the Study day, Streaks and the New-card
allowance. `due`, `check` and `stats` already account for the New-card
allowance. `totalDue` in their JSON is the unclamped count.

A Card's id is a hash of its Deck, question and answer: editing the question
or answer changes it, while tag and type edits keep it. Use the id returned by
`edit-card` for follow-up `review` or `delete-card`, or pin an explicit `id:`
in the deck file.
