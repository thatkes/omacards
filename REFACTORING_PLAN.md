# Refactoring Plan — omacards

Tracks the remaining structural work. Completed items are recorded in the
progress table only; their implementation notes are not kept here.

## Current state (measured 2026-11-19)

- **4,921 LOC** of C++/Qt 6 across `src/` (14 `.cpp` + 14 `.h`), plus
  `src/Main.qml` and i18n resources.
- **1,677 LOC** across 24 C++/header files in `tests/` (11 class pairs, one
  runner, one support header), holding **52 test slots**; largest class is
  `test_backend.cpp` at **592** LOC.
- Largest units: `backend.cpp` **1,058**, `cli.cpp` **856**,
  `mathrender.cpp` **446**, `deck.cpp` **376**, `importer.cpp` **324**,
  `store.cpp` **274**, `backend.h` **241**.
- `main.cpp` is **87 LOC**; tray code lives in `traycontroller.cpp` (64) /
  `traycontroller.h` (26).
- Prior refactors already separated the study queue, file history, import
  parsing, domain enums and shared scheduling reads.
- Tests run offscreen through `./bin/test` (74/74); paths are injectable with
  `OMACARDS_DECKS_DIR` / `OMACARDS_STATE_DB` and the CLI has headless parity.
- The snapshot includes committed day-start-hour work (`bed92cf`), the tray
  extraction (`2ab7293`) and the test split (`b0ec959`, `3f9177f`), all local
  only. Re-check these numbers before starting #1.

## Progress

| # | Item | Status |
|---|------|--------|
| 1 | Decompose `Backend` | **Planned** — staged implementation below |
| 2 | Extract tray + app bootstrap from `main.cpp` | **Done** (`2ab7293`) |
| 3 | Centralize configuration constants | **Done** (`db98d7b`, `bed92cf`) |
| 4 | Type safety at boundaries | **Deferred** — parked below, not in this pass |
| 5 | Split the test file | **Done** (`b0ec959`, `3f9177f`) |
| 6 | Tooling & hygiene | **Done** (`db98d7b`, `2ab7293`) |
| 7 | Math renderer cleanup | **Done** (`db98d7b`) |

## 1. Decompose `Backend`

### Scope and constraints

`Backend` currently holds four unrelated jobs in one object: deck/card files
and SQLite sidecar writes, scheduling-state reads and availability counting,
the in-study session (queue, timer, current card, status, counters), and the
QML/CLI façade. The public surface is the contract: 29 `Q_PROPERTY` entries,
the `Q_INVOKABLE` slots, the headless helpers used by `src/cli.cpp`, all
signal names, every translated status/error string, and the keys of
`statsSummary()` (consumed as JSON by the CLI). None of those change here.

Type safety (#4) stays out: `m_mode`, `Card::type` and rating values keep their
current string/int forms at this boundary.

### Target structure

Three collaborators, owned by `Backend`, split the responsibilities:

| New unit | Owns | Absorbs from `backend.cpp` |
|---|---|---|
| `DeckRepository` (`src/deckrepository.{h,cpp}`, `QObject`) | `m_decksDir`, `m_decks`, `m_deckNames`, watcher, reload timer, references to `Store` and `FileHistory` | `initialize` (directory/welcome deck), `load`, `deckByName`/`deckIndex`/`cardById`, all deck/card CRUD, `commitDeckEdit`, `recordHistory`, undo/redo, import commit, `searchCards`, `totalCards`, deck-file path lookup. Emits `reloadRequested()` from the watcher timer (Backend still calls `refresh()`), `changed()` after a successful load/write and `errorOccurred(QString)`. `Backend::initialize()` keeps opening the store and loading/clamping settings. |
| `StudyPlanner` (`src/studyplanner.{h,cpp}`, plain class) | `StudySnapshot`, `Availability`, a reference to `Store`, current limits | `refreshSnapshot`, `effectiveState`, `availability`, `countDue`/`availableCount`/`newCount`/`deckDue`/`deckNew`, per-deck counts for summaries, snapshot update after a review, card-list scheduling fields. |
| `StudySession` (`src/studyqueue`-based, `src/studysession.{h,cpp}`, `QObject`) | `StudyQueue`, review-again timer, `MathRenderer` and ink colors, current card/question/answer, `showingAnswer`, `waitingForRepeat`, status, `reviewed`/`correct`/`box`, `dueCount` | `rebuildQueue`, `scheduleNextRepeat`, `presentCard`, `setStatus`, `flip`, `rate`, `answerCram`, `answerReview` (through `StudyPlanner`), `showNext`, `setMathColors`, `statsText`, session counters. Emits `cardChanged`, `statusChanged`, `statsChanged`, `dueCountChanged`, `errorOccurred`. |

`Backend` keeps the `Q_PROPERTY` declarations, `Q_INVOKABLE` slots, `Store`,
`FileHistory`, settings (`newPerDay`, `desiredRetention`, `dayStartHour`),
deck selection, `fromEnvironment()` and the signal hub. Getters delegate to
the collaborators; slots validate as today and forward; collaborator signals
are connected signal-to-signal in the constructor so QML sees no difference.
`m_mode` stays a `QString` parsed via `Domain::modeFromString()`.

### Implementation stages

Land each stage as its own green commit; do not combine stages. The test
binary compiles `../src/*.cpp` directly, so every stage adds new files to
both `omacards.pro` and `tests/omacards_tests.pro` (`HEADERS` and `SOURCES`);
new `Q_OBJECT` headers must be listed in both for AUTOMOC. End every stage
with `./bin/build`, `clang-format --dry-run --Werror` on touched files and
`./bin/test` (74/74, or the new total if regression slots are added).

1. **Prepare the seams.** `TestBackend` already covers the risky paths:
   `refreshKeepsPendingAgainRepeat`, `backendRequeuesAgainInSameSession`,
   undo/redo after deck edits and deletion, unreadable decks/state, failed
   writes, count consistency, path-collision rejection, and both QML tests.
   Add slots only for uncovered seams: a pending Again repeat surviving an
   explicit `selectDeck()`/`setMode()` switch (this is current behavior),
   `setMathColors()` recoloring the current card, and the `statsSummary()`
   key set. No production change; the new tests pass against current code.

2. **Extract `DeckRepository`.** Move the welcome-deck/directory setup,
   `load()` including sidecar pruning, the watcher and 120 ms debounce,
   lookups, all create/edit/delete/import paths and history recording.
   `Backend::load()` becomes `m_decks.load()` followed by the existing
   snapshot refresh; `Backend::refresh()` still orchestrates load + queue
   rebuild. The repository takes explicit deck names, so `Backend` keeps
   resolving an empty name to `m_currentDeck`. Keep every validation message
   and error string verbatim — `TestCli` asserts them. Target:
   `backend.cpp` under ~700 LOC.

3. **Extract `StudyPlanner`.** Move `StudySnapshot`/`Availability`,
   `refreshSnapshot`, `effectiveState`, `availability` and every count/summary
   query that reads them. The planner receives the deck list and limits from
   `Backend`; `answerReview()` calls `planner.recordReview(...)` to persist and
   advance the in-memory snapshot together. `setNewPerDay()` must still update
   `newBudget` in place, while `setDayStartHour()` triggers a full refresh and
   `cardsChanged()`. Target: `backend.cpp` under ~550 LOC.

4. **Extract `StudySession`.** Move the queue/timer/card presentation, rating
   paths, math rendering, status and session counters. Backend getters
   delegate (e.g. `question()` → `m_session.question()`), and Backend
   connects the session signals to its own. `dueCount()` remains the sum of
   planner availability and `StudyQueue::pendingRepeatCount()`. Target:
   `backend.cpp` under 400 LOC.

5. **Tighten the façade.** Delete leftover private helpers and the
   `StudySnapshot`/`Availability` declarations from `backend.h`; leave only
   properties, slots, signals, the three collaborators, `Store`, `FileHistory`,
   settings and selection state. Run the full suite again and confirm the CLI
   JSON/stat output is unchanged.

### Invariants to preserve

- A single consistent scheduling snapshot: all availability queries and card
  views read it; a store read error clears it, reports the error and makes
  cards unavailable; a hash mismatch means "new".
- Due-by-day (`Config::isDueByDay`), due cards ordered by due time then id,
  new cards in deck-file order under the global `newPerDay` budget, and
  per-deck vectors indexed by deck order.
- The watcher's 120 ms debounce; `rebuildQueue()` passes
  `StudyQueue::pendingRepeats()` into `buildReview()`, so a pending Again
  repeat survives both a refresh and `selectDeck()` in Review mode while a
  cram rebuild clears it; the timer is single-shot, wired to card
  presentation, and Again uses `Config::againDelayMs()`.
- Rating validation before counters increment; `Good` increments `correct`;
  cram advances the Leitner box and only cram updates `box`; review writes
  state, log and snapshot in one step.
- `presentCard()` skips vanished cards, handles Finished/Waiting statuses, and
  renders math with the 2.0 question / 1.6 answer scales and the configured
  ink colors; status strings stay identical.
- Bulk creation may create a deck, single-card creation may not; imported
  descriptions apply only to new decks; pinned ids survive edits while
  generated ids follow content; failed saves restore the in-memory deck;
  file collisions and existing paths are never overwritten silently.
- Sidecar pruning happens only after a clean load with decks, plus the
  explicit prune after deleting the last deck; undo/redo refresh and report
  the recorded description.
- `databasePath()` stays on `Backend` for CLI `review`, which writes through
  its own `Store`; `streak`, `reviewSummary()` and `statsSummary()` keep
  reading SQLite on demand.
- Default paths ignore `organizationName()`, and `statsSummary()`/`cardList()`
  field names and formats stay stable for the CLI and QML.

## Deferred — 4. Type safety at boundaries

Not in this pass. The parked intent:

- Keep `QString` at QML/CLI/file boundaries (required), but parse into
  `Domain` enums at the edge instead of storing raw strings internally.
- Replace string comparisons such as `mode == "review"` with enum checks;
  `backend.cpp` already routes decisions through `Domain::modeFromString`,
  `Domain::ratingFromInt` and `Domain::cardTypeFromString`.
- Decide the scope before starting: changing `m_mode` to `StudyMode` is local;
  changing `Card::type` to `CardType` ripples into `deck.cpp`, `cli.cpp`,
  the importer and tests.

## Explicitly out of scope (do not bundle)

- Type safety at boundaries (#4) — deferred to a later pass.
- New study modes, new card types, sync/cloud, mobile.
- Changing deck file format or card id scheme.
- FSRS weight tuning (keep reference weights).

## Definition of done for this plan

- [ ] `backend.cpp` < 400 LOC, staged through the targets above; no type-safety
      changes bundled. (#1)
- [ ] Public QML/CLI API, status strings and JSON keys unchanged; full suite
      green at every stage. (#1)
- [x] Tray/bootstrap extracted. (#2, `2ab7293`)
- [x] Configuration constants centralized. (#3, `db98d7b`/`bed92cf`)
- [x] Tests split into 11 per-area classes with a shared support header and
      single runner. (#5, `b0ec959`/`3f9177f`)
- [x] Formatting, CI and explicit path injection in place. (#6, `db98d7b`/`2ab7293`)
- [x] Math renderer detection consolidated into `Engine`. (#7, `db98d7b`)
