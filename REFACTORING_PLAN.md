# Refactoring Plan — omacards

Tracks structural work that remains. Completed work is summarized here, not
replayed stage by stage.

## Current state (measured 2026-11-19)

- `Backend` is now a thin QML/CLI façade over three collaborators:
  `DeckRepository` (deck files, watcher, CRUD, history),
  `StudyPlanner` (one scheduling snapshot, counts and summaries), and
  `StudySession` (queue, repeat timer, card presentation, ratings and status).
- `backend.cpp` is **373 LOC** and `backend.h` is **181 LOC**; there are no
  scheduling structs or private session helpers left in the façade.
- The full source tree is **5,344 LOC** across 34 C++/header files; tests are
  **1,760 LOC** across 24 files with 55 slots / 77 runnable cases.
- `./bin/test` runs offscreen and is **77/77**. Paths are injectable with
  `OMACARDS_DECKS_DIR` / `OMACARDS_STATE_DB`, and the CLI keeps headless parity.
- Type safety at the QML/CLI boundary is still deliberately deferred (#4).

## Progress

| # | Item | Status |
|---|------|--------|
| 1 | Decompose `Backend` | **Done** — staged in `cdb5289`, `1f6b628`, `1f91a2e`, `fc27c9d`, `da1fa0f`; final tightening in this commit |
| 2 | Extract tray + app bootstrap from `main.cpp` | **Done** (`2ab7293`) |
| 3 | Centralize configuration constants | **Done** (`db98d7b`, `bed92cf`) |
| 4 | Type safety at boundaries | **Deferred** — parked below |
| 5 | Split the test file | **Done** (`b0ec959`, `3f9177f`) |
| 6 | Tooling & hygiene | **Done** (`db98d7b`, `2ab7293`) |
| 7 | Math renderer cleanup | **Done** (`db98d7b`) |

## Contracts preserved by the decomposition

- Public surface unchanged: all `Q_PROPERTY`, `Q_INVOKABLE` slots, signals,
  headless helpers, translated status/error strings, CLI JSON keys and
  `statsSummary()` / `cardList()` field formats.
- One scheduling snapshot drives every availability query and card view; a
  store read error clears it, reports the error and makes cards unavailable.
  Hash mismatches still mean “new”, with due-by-day and new-card ordering
  unchanged.
- A pending Again repeat survives a refresh and an explicit deck switch in
  Review mode; a cram rebuild clears it. The repeat timer stays single-shot
  and uses `Config::againDelayMs()`.
- Rating validation happens before counters increment; only Good increments
  `correct`, only cram advances `box`, and review writes state, log and
  snapshot together.
- Deck-file safety is unchanged: no silent overwrites or path collisions,
  failed saves restore in-memory decks, sidecar pruning happens only after a
  clean load, and undo/redo refresh and report the recorded description.
- `databasePath()`, `fromEnvironment()`, `streak`, `reviewSummary()` and
  `statsSummary()` keep their existing CLI/GUI behavior.

## Deferred — 4. Type safety at boundaries

Not in this pass. The parked intent:

- Keep `QString` at QML/CLI/file boundaries (required), but parse into `Domain`
  enums at the edge instead of storing raw strings internally.
- Replace string comparisons such as `mode == "review"` with enum checks;
  decisions already route through `Domain::modeFromString`,
  `Domain::ratingFromInt` and `Domain::cardTypeFromString`.
- Scope before starting: changing `m_mode` to `StudyMode` is local; changing
  `Card::type` to `CardType` ripples into `deck.cpp`, `cli.cpp`, the importer
  and tests.

## Explicitly out of scope

- Type safety at boundaries (#4) — deferred to a later pass.
- New study modes, new card types, sync/cloud, mobile.
- Changing the deck file format or card id scheme.
- FSRS weight tuning (keep reference weights).
