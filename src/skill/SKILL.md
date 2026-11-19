---
name: omacards
description: Create and maintain omacards decks, cards and imports through the headless CLI. Use when asked to add flashcards, edit a deck, import Anki/CSV, check due cards, or simulate cram sessions.
---

# omacards

`omacards` is the Omarchy flashcards app. Deck files are plain text under
`~/.local/share/omacards/decks/` (or `$OMACARDS_DECKS_DIR`), one `.txt` file
per deck. Scheduling state lives in a SQLite sidecar and is regenerated if a
card's content changes.

## Rules

- Prefer the CLI over editing the SQLite database.
- Every command accepts `--json`; errors go to stderr and exit 1.
- Deck names and card text are the source of truth. Keep questions short.

## Common commands

```sh
omacards check --json
omacards decks --json
omacards cards --deck Spanish --json
omacards search gato --json
omacards add-deck "Spanish" --json
omacards add-card --deck Spanish -q 'What is "gato"?' -a 'cat' --tags vocab,animals --json
omacards add-card --deck Physics -q 'Solve \\(x^2+1=0\\)' -a '\\(x=\\pm i\\)' --json
omacards edit-card -c <card-id> -a 'a cat' --tags vocab --json
omacards delete-card -c <card-id> --json
omacards delete-deck Spanish --json
omacards import ~/Downloads/deck.apkg --deck Spanish --json
omacards import ~/Downloads/words.csv --deck Spanish --json
omacards due --json
omacards review -c <card-id> -r 3 --json
omacards cram --deck Spanish --steps 20 --json
omacards stats --json
omacards config --json
omacards config --new-per-day 10 --retention 0.9 --json
```

## Hand-edited deck format

```text
# Deck: Spanish
---
Q: What is "gato"?
A: cat
tags: vocab, animals
---
Q: Solve \(x^2+1=0\)
A: \(x=\pm i\)
---
Q: The {{c1::cat}} sleeps.
type: cloze
```

After editing a file by hand, run `omacards check --json` to validate it.

## Ratings

- `1` = Again (forgot)
- `3` = Good (recalled)

## Scheduling settings

`omacards config` reads the daily new-card limit and FSRS desired retention.
Use `--new-per-day N` and `--retention R` (0.70–0.99) to change them. `due`,
`check` and `stats` already account for the daily new-card limit. `totalDue`
in their JSON is the unclamped count.

Editing a card without an explicit `id:` line changes its id, so use the id
returned by `edit-card` for any follow-up `review` or `delete-card`.
