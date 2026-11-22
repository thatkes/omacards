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
- Use `./bin/with-temp-env omacards <command>` for isolated probes: it points
  decks, state and cache at a temporary directory removed on exit.
- `bin/link` also registers the desktop launcher entry; see **Development**.

## Common commands

```sh
omacards check --json                        # Deck issues (incl. math warnings)
omacards check --deck Spanish --json         # scope decks, cards and Deck issues to one deck
omacards check --math --json                 # also add a mathWarnings array
omacards check --strict --json               # exit 1 if any Deck issue exists
omacards decks --json
omacards decks active --json                 # Current deck (alias: decks current)
omacards use-deck Spanish --json             # choose the Current deck
omacards cards --deck Spanish --json
omacards search gato --json
omacards preview --deck Spanish --card <card-id> --format text --json
omacards warm --deck Spanish --json          # pre-render the Deck's math SVGs
omacards add-deck "Spanish" --description "Vocab" --json
omacards add-cards --deck Spanish --input cards.json --json
omacards add-card --deck Spanish -q 'What is "gato"?' -a 'cat' --tags vocab,animals --json
omacards add-card --deck Physics -q 'Solve \(x^2+1=0\)' -a '\(x=\pm i\)' --json
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
omacards config --new-card-allowance 10 --day-start-hour 3 --retention 0.9 --json
omacards help                                # usage text; ignores --json
omacards open                                # start the GUI; ignores --json
omacards skill install
```

`add-card` without `--deck` writes to the Current deck, so check
`omacards decks active --json` first.

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

No renderer is bundled, so math falls back to Unicode unless a toolchain is
detected on `PATH`: `tectonic` + `pdftocairo`, `latex` + `dvisvgm`, or
`pdflatex` + `pdftocairo`. This checkout has `tectonic` and `pdftocairo`, so
`omacards check --json` reports `"math":"LaTeX (SVG)"`. Use notation that
survives the fallback:

- Fractions as `(a)/(b)` and powers as `x^(2n+1)` — these render legibly.
- Write one backslash in shell arguments and deck files: `\(...\)`, not
  `\\(...\\)`. JSON examples need `\\(` because JSON escapes backslashes.
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
(`--new-per-day N` is an accepted alias, `0..9999`), `--day-start-hour H`
(`0..23`) and `--retention R` (`0.70..0.99`) to change them. Pass `-1` for any
of the three to leave it unchanged.

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
