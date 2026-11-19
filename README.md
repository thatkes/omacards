# omacards

![omacards review screen](docs/screenshot.png)

![omacards card browser](docs/browser.png)

A keyboard-first flashcards app for Omarchy. It has two study modes:

- **Review** — FSRS-5 long-term scheduling with Again and Good. Again moves the card to a future due date but also brings it back for one more try after a short break in the same session.
- **Cram** — endless in-session Leitner boxes (1, 2, 4, 8, 16 cards) that reset on Again and re-surface weak cards until you quit.

Deck content lives in plain text files under `~/.local/share/omacards/decks/`, so agents and editors can change it directly. Scheduling state lives in a SQLite sidecar at `~/.local/share/omacards/state.db`; deleting the database never deletes a card.

## Install

```sh
./bin/build     # build to build/omacards
./bin/test      # offscreen Qt Test suite
./bin/install   # makepkg -fsi, adds the binary, launcher entry and icon
```

Requirements: Qt 6 (base, declarative, wayland, SQL), qmake6, gcc/make. Optional: `unzip` for Anki `.apkg` imports, `tectonic`/`pdftocairo` or `latex`/`dvisvgm` for LaTeX math. Without a TeX toolchain, math is rendered as Unicode.

## LaTeX math

Math inside `\(...\)` or `$$...$$` becomes a cached SVG when a TeX toolchain is available. On Omarchy/Arch:

```sh
sudo pacman -S tectonic poppler   # poppler provides pdftocairo
mkdir -p /tmp/omacards-tex
printf '\\documentclass{article}\n\\begin{document}$x^2$\\end{document}\n' > /tmp/omacards-tex/warm.tex
tectonic --untrusted --outdir=/tmp/omacards-tex /tmp/omacards-tex/warm.tex
```

The one-off `tectonic` run downloads the TeX bundle into `~/.cache/tectonic`; doing it ahead of time avoids a long pause on the first card. Normal renders then use Tectonic's `--only-cached` mode. Restart omacards after installing so it detects the toolchain, and confirm with `omacards check --json` (`"math": "LaTeX (SVG)"`). SVGs are cached under `~/.cache/omacards/math`; delete that directory to force a re-render.

## Keyboard shortcuts

| Key | Action |
| --- | --- |
| `Space`, `↑`, `↓`, `Enter` | Flip the card / main action |
| `1`, `A`, `←` | Again |
| `2`, `G`, `→` | Good |
| `N` | New card |
| `D` | New deck |
| `B` | Browse and search cards |
| `J` / `K` | Move the card selection in the library (wraps around) |
| `Enter` | Edit the selected card in the library |
| `Delete` / `Backspace` | Delete the selected card in the library |
| `/` | Focus library search |
| `S` | Stats for today and this session |
| `,` | Settings: new cards per day and desired retention |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo a card or deck edit |
| `I` | Import a file |
| `E` | Open the current deck file in your editor |
| `C` | Cram mode |
| `R` | Review mode |
| `?`, `F1` | Shortcut overlay |
| `Q` | Quit |

Closing the window quits the app. While omacards is open, the tray tooltip shows the number of due cards; clicking it opens a review session.

`B` opens the card browser. It searches questions, answers and tags, and each card can be edited or deleted there. The selected row is highlighted; `J`/`K` (or the arrow keys) move it, `Enter` edits it and `Delete` removes it. Clicking a row selects it, and `/` jumps to the search field. Card and deck edits are undoable with `Ctrl+Z`; `Ctrl+Shift+Z` (or `Ctrl+Y`) redoes them. The same history covers cards added from the GUI, CLI imports and deck deletions. Review answers are intentionally not undoable, so a lapse cannot be erased by accident.

## Settings

Press `,` (or the gear button) to open settings. Both values are stored in the SQLite sidecar and apply immediately after saving.

![omacards settings](docs/settings.png)

- **New cards per day** (default 20) caps how many never-reviewed cards enter a review session. This keeps a large Anki import manageable. Set it to 0 to study only cards that are already in review.
- **Desired retention** (default 90%) is the FSRS target used when intervals are calculated. Higher values shorten intervals and mean more reviews; lower values stretch intervals. The change affects future answers, not existing due dates.

Again repeats come back after a 60-second break in the same session. Set `OMACARDS_AGAIN_DELAY_MS` (milliseconds) to adjust that break; `0` shows the card immediately.

Due reviews are shown before new cards, and new cards follow deck-file order. The tray count and due badge reflect the cards actually available now, after the daily limit. `omacards config` exposes the same two settings to agents.

## Deck format

```text
# Deck: Spanish
# Description: Words and phrases
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

A card's id is a hash of its deck, question and answer, so editing a card resets its FSRS state but keeps the file readable. An explicit `id:` line pins the identity across edits.

Anki `.apkg` imports preserve HTML entities, tags, and cloze note types (`{{c1::answer}}`; `::hint` is dropped). Referenced media is extracted next to the deck files under `media/<deck>/` and image sources are rewritten to local `file://` URLs, so images still render in the study view. CSV/TSV files with a `question,answer,tags` header are imported as basic cards.

## Agent CLI

Every command accepts `--json`, prints errors to stderr and exits 0 or 1.

```sh
omacards check --json
omacards decks --json
omacards cards --deck Spanish --json
omacards search gato --deck Spanish --json
omacards add-deck Spanish --json
omacards add-card --deck Spanish -q 'What is "gato"?' -a cat --tags vocab --json
omacards edit-card -c <card-id> -a 'a cat' --json
omacards delete-card -c <card-id> --json
omacards delete-deck Spanish --json
omacards import ~/Downloads/deck.apkg --deck Spanish --json
omacards import ~/Downloads/words.csv --deck Spanish --json
omacards due --deck Spanish --json
omacards review -c <card-id> -r 3 --json
omacards cram --deck Spanish --steps 20 --json
omacards stats --json
omacards config --json
omacards config --new-per-day 10 --retention 0.9 --json
omacards skill install --json
```

`omacards skill install` writes a short agent skill to `~/.agents/skills/omacards/SKILL.md`. Set `OMACARDS_DECKS_DIR` and `OMACARDS_STATE_DB` to isolate data for tests or scripts.

## Why Leitner for cram?

Cram needs no persistent state, must never end, and should re-show weak cards quickly. A five-box Leitner queue gives exactly that: correct answers promote a card to a longer card-count interval, Again resets it to box 1, and the queue is refilled forever. FSRS remains the long-term scheduler where spaced intervals and retention targets matter.

## Layout

```text
src/backend.{h,cpp}      cards, files, SQLite, modes, study sessions
src/importer.{h,cpp}     CSV/TSV and Anki parsing into card drafts
src/domain.{h,cpp}       shared mode, rating and card-type values
src/fsrs.{h,cpp}         FSRS-5 model (Again=1, Good=3)
src/deck.{h,cpp}         plain-text deck parser/writer
src/store.{h,cpp}        SQLite sidecar
src/studyqueue.{h,cpp}   in-session review/cram ordering
src/mathrender.{h,cpp}   LaTeX to SVG with Unicode fallback
src/main.cpp             app setup, system tray, CLI dispatch
src/Main.qml             window, keyboard, layout
src/theme.* src/translator.*  live Omarchy theme and language
tests/                   Qt Test suite
pkgbuild/                Arch package, desktop entry, icon
```

Theme watching follows Omacut and Monologue (MIT, David Heinemeier Hansson).
