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
- Every command accepts `--json`; errors go to stderr and exit 1.
- Deck names and card text are the source of truth. Keep questions short.
- `.apkg` imports keep Anki cloze types and tags; media lands in `media/<deck>/` beside the deck files.
- Deck files remain the source of truth; all new JSON fields are additive.

## Finding the CLI

```sh
command -v omacards
```

- If `omacards` is on `PATH`, use it directly.
- If it is missing, either run `bin/link` in a source checkout (this builds,
  symlinks the command, installs the launcher entry, icon and agent skill), or
  invoke `<checkout>/build/omacards` directly.
- For scripts, allow an `OMACARDS_BIN` override and default to `omacards`:
  `BIN="${OMACARDS_BIN:-omacards}"; "$BIN" check --json`.
- `bin/link` also registers the desktop launcher entry; see **Development**.

## Common commands

```sh
omacards check --json
omacards check --deck Spanish --json         # scope decks, cards and Deck issues to one deck
omacards check --math --json                 # math Warnings
omacards check --strict --json               # exit 1 if any Deck issues exist
omacards decks --json
omacards cards --deck Spanish --json
omacards search gato --json
omacards preview --deck Spanish --format text --json
omacards add-deck "Spanish" --description "Vocab" --json
omacards add-cards --deck Spanish --input cards.json --json
omacards add-card --deck Spanish -q 'What is "gato"?' -a 'cat' --tags vocab,animals --json
omacards add-card --deck Physics -q 'Solve \\(x^2+1=0\\)' -a '\\(x=\\pm i\\)' --json
omacards edit-card -c <card-id> -a 'a cat' --tags vocab --json
omacards delete-card -c <card-id> --json
omacards delete-deck Spanish --json
omacards import ~/Downloads/deck.apkg --deck Spanish --json
omacards import ~/Downloads/words.csv --deck Spanish --json
omacards import ~/Downloads/deck.json --json
omacards due --json
omacards review -c <card-id> -r 3 --json
omacards cram --deck Spanish --steps 20 --json
omacards stats --json
omacards config --json
omacards config --new-per-day 10 --day-start-hour 3 --retention 0.9 --json
omacards skill install
```

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

Use `--input -` (or pipe stdin with no `--input`) to read JSON from standard
input. With a TTY and no input file, the command fails clearly instead of
blocking. `import FILE.json` routes to the same parser and derives the deck name
from the filename unless `--deck` is given.

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
Q: Expand \\(\\frac{1}{1-x}\\) about 0
A: \\(\\sum_{n=0}^{\\infty} x^n\\), for \\(|x|<1\\)
---
Q: The {{c1::cat}} sleeps.
type: cloze
id: a1b2c3d4e5f6
```

- `# Deck:` names the deck, `# Description:` documents it.
- `---` separates cards.
- Keys: `Q:` / `A:` (question / answer), `Answer:` is an alias for `A:`,
  `type: cloze`, `tags: a, b`, and an explicit `id:`.
- **Continuation lines must start with exactly two spaces.** Three or more
  leading spaces are reported as a Deck issue by `check`.
- Editing a card without an explicit `id:` line changes its id.

After editing a file by hand, run the **validation loop** below.

## Math notation

No LaTeX renderer is installed by default, so math falls back to Unicode. Use
a notation that survives that fallback:

- Fractions as `(a)/(b)` and powers as `x^(2n+1)` — these render legibly.
- Safe macros via `\\(...\\)` or `$$...$$`: `\\sum`, `\\infty`, `\\frac`,
  `\\binom`, `\\sqrt`, `\\sin`, `\\cos`, `\\ln`, `\\pi`, `\\pm`, `\\leq`,
  `\\to`, `\\int`, `\\cdots`. The fallback maps nested `\\frac{a}{b}` to
  `(a)/(b)`, `\\binom{r}{n}` to `C(r,n)`, and `\\sqrt{1+x}` to `√(1+x)`.
- Unknown macros stay visible and are reported by `check --math`; they are
  Warnings and do not flip `ok`.
- Installing `tectonic` + `pdftocairo` enables real LaTeX SVG rendering; omit
  those dependencies to keep the Unicode fallback as the default. On Arch/Omarchy:
  `sudo pacman -S tectonic poppler` (poppler provides `pdftocairo`), then prewarm
  the TeX bundle once with
  `printf '\\documentclass{article}\n\\begin{document}$x^2$\\end{document}\n' > /tmp/warm.tex && tectonic --untrusted --outdir=/tmp /tmp/warm.tex`.
  Restart the app after installing. `omacards check --json` reports
  `math: LaTeX (SVG)` when the renderer is active; one neutral SVG per
  expression caches in `~/.cache/omacards/math` and is recolored to the
  active theme at display time.

Check how a card actually renders with `omacards preview --deck X --format text
--json` before authoring many similar cards. Use `--format html` with a running
LaTeX toolchain to inspect the generated `<img>` references.

## Validation loop

After authoring or editing a deck:

```sh
omacards preview --deck "My Deck" --format text --json   # see rendered math
omacards check --math --json                             # math Warnings
omacards check --json                                    # syntax + Deck issues
```

Read `issues` from `check --json`: each entry has `path`, optional `line` and
`card`, a `severity` (`error` or `warning`), `message`, and a concrete `hint`.
The `issueCounts` object summarizes Errors and Warnings. `ok` is false only for
Errors; `--strict` exits 1 whenever any Deck issue exists, which is useful in
scripts. Fix line-numbered Deck issues and re-run until the deck is clean.

## Development

- `./bin/build` — compile the dev binary into `build/omacards`.
- `./bin/test` — run the QtTest suite.
- `./bin/link` — build, then register the command, launcher entry, icon and
  agent skill in `~/.local`; safe to re-run. Use it after changing the skill or
  desktop entry so the installed copies stay in sync.

## Ratings

- `1` = Again (forgot)
- `3` = Good (recalled)

## Scheduling settings

`omacards config` reads the New-card allowance, the Day-start hour and Desired
retention. Use `--new-per-day N`, `--day-start-hour H` (0–23, default 0) and
`--retention R` (0.70–0.99) to change them. A card is due once the Study day
containing its FSRS due time has begun: with `--day-start-hour 3`, a card due at
22:00 waits until 03:00 the next Study day. The same boundary governs Ratings on
the Study day, Streaks and the New-card allowance. `due`, `check` and `stats`
already account for the New-card allowance. `totalDue` in their JSON is the
unclamped count.

Editing a card without an explicit `id:` line changes its id, so use the id
returned by `edit-card` for any follow-up `review` or `delete-card`.
