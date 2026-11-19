# Refactoring Plan — omacards

Companion to the code-quality review. Goal: pay down structural debt **before** adding new features, so new work lands on a cleaner foundation.

## Current state (snapshot)

- ~6k LOC of C++/Qt 6 across `src/` + ~1.3k LOC tests.
- Active, disciplined refactoring history (study queue, file history, import parsing, domain enums, shared scheduling reads).
- Strong test suite (`./bin/test`, offscreen Qt Test), injectable paths, env isolation, headless CLI parity.

Overall rating: **8.5/10**. See strengths/weaknesses below.

## Progress

| # | Item | Status |
|---|------|--------|
| 1 | Decompose `Backend` | Not started |
| 2 | Extract tray + app bootstrap from `main.cpp` | Not started |
| 3 | Centralize configuration constants | **Done** (`src/config.h`, `Config::againDelayMs()`, `Backend::fromEnvironment()`) |
| 4 | Type safety at boundaries | Not started |
| 5 | Split the test file | Not started |
| 6 | Tooling & hygiene | **Done** (`clang-format` applied, CI, explicit paths, tray menu ownership) |
| 7 | Math renderer cleanup | **Done** (`MathRenderer::detect()` → `Engine` enum) |

## Strengths

- Clear modular split: `deck`, `store`, `fsrs`, `studyqueue`, `filehistory`, `importer`, `mathrender`, `cli`, `domain`.
- Centralized domain enums (`StudyMode`, `Rating`, `CardType`) instead of scattered magic strings.
- Consistent scheduling snapshot (`StudySnapshot`) so counts and card views cannot disagree mid-action.
- Testability by design: injectable `decksDirectory`/`databasePath`, `OMACARDS_DECKS_DIR`/`OMACARDS_STATE_DB`.
- Atomic file-level undo/redo (`FileHistory`) covering GUI, CLI imports, deck deletions.
- Faithful, well-commented FSRS-5 implementation with configurable retention.
- Agent-first ergonomics: JSON CLI, plain-text decks, headless commands, skill install.
- Card identity by content hash with optional pinned `id:`.

## Weaknesses / debt

- `Backend` is a god object (~1025 LOC cpp, ~234 LOC header): UI state, scheduling, persistence, file I/O, math, history.
- `QString` is the universal currency, including at the QML boundary; enums round-trip through strings, losing type safety outside `Domain`.
- Hardcoded constants: Again delay 60000 ms, new-per-day 20, retention 0.9, Leitner boxes 1/2/4/8/16 (only some env-overridable).
- `Backend` constructor defaults to empty paths, relying on env vars — implicit and surprising.
- `main.cpp` mixes tray icon drawing, CLI dispatch, QML engine setup.
- Minor ownership leak: `new QMenu` without parent in tray code.
- No lint/format/CI config (`.clang-format`, static analysis).
- Tests are a single monolithic file (1279 LOC).
- Optional toolchain detection (math) is runtime and ad hoc.

## Near-term improvements (do before new features)

Priority order. Each item is small and independently shippable; aim for one commit per item, matching the existing refactor style.

### 1. Decompose `Backend` (highest impact)
- Extract `StudySession` (queue advancement, repeat scheduling, reviewed/correct/box counters, stats).
- Extract `CardRepository` (deck/card CRUD, file read/write, watcher, reload).
- Keep `Backend` as a thin QML/CLI façade that delegates and re-emits signals.
- Target: `backend.cpp` < 400 LOC.

### 2. Extract tray + app bootstrap from `main.cpp`
- Move tray icon/menu construction into `TrayController` (or `main.cpp` helper) taking `Backend&`/`Theme&`.
- Keep `main()` to: arg parsing, offscreen dispatch, app setup, engine load, exec.

### 3. Centralize configuration constants — ✅ Done
- Create `config.h` with named constants: `kDefaultNewPerDay`, `kDefaultRetention`, `kAgainDelayMs`, `kLeitnerBoxes`, `kMaxIntervalDays`.
- Read env overrides (`OMACARDS_AGAIN_DELAY_MS`) in one place; expose via `Backend`/settings.
- Remove scattered literals.

### 4. Type safety at boundaries
- Keep `QString` at QML/CLI/file boundaries (required), but convert at the edge: parse into `Domain` enums inside `Backend` slots, never store mode/rating/type as raw strings internally.
- Audit `backend.cpp` for string comparisons like `mode == "review"` and replace with enum checks.

### 5. Split the test file
- Move into `tests/` subfiles by area: `test_fsrs.cpp`, `test_deck.cpp`, `test_store.cpp`, `test_cli.cpp`, `test_backend.cpp`.
- Update `tests/omacards_tests.pro` accordingly; keep shared `EnvGuard` in a common header.

### 6. Tooling & hygiene — ✅ Done
- Add `.clang-format` (match existing style) and run it once.
- Add a CI step: build + `./bin/test` offscreen.
- Fix the `QMenu` parent (pass `&tray` or use `std::unique_ptr`).
- Make `Backend` constructor require explicit paths (remove empty defaults) or provide a named factory `Backend::fromEnvironment()`.

### 7. Math renderer cleanup — ✅ Done
- Make toolchain detection a single `MathRenderer::detect()` returning an enum (`None`, `Unicode`, `LatexSvg`) instead of ad hoc checks scattered in `mathEngine()`.

## Explicitly out of scope (do not bundle)

- New study modes, new card types, sync/cloud, mobile.
- Changing deck file format or card id scheme.
- FSRS weight tuning (keep reference weights).

## Definition of done for this plan

- [ ] `backend.cpp` < 400 LOC; no stringly-typed mode/rating/type comparisons inside. (pending #1, #4)
- [ ] Tests split and green (`./bin/test` passes). (green now; split pending #5)
- [x] `.clang-format` committed and applied.
- [x] CI runs build + tests. (workflow added; first remote run pending a push)
- [x] No behavior change: `./bin/test` is 51/51 green (test edits limited to the new factory + detection assertions).
