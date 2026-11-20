# Refactoring Plan — omacards

Companion to the code-quality review. Goal: pay down structural debt **before** adding new features, so new work lands on a cleaner foundation.

## Current state (snapshot, re-measured 2026-11-19)

- **4,921 LOC** of C++/Qt 6 across `src/` (14 `.cpp` + 14 `.h`), plus `src/Main.qml` and i18n resources.
- **1,677 LOC** across 24 C++/header files in `tests/` (11 class pairs, one runner, one support header), holding **52 test slots**; largest class is `test_backend.cpp` at **592** LOC. The 15-line `tests/omacards_tests.pro` stays.
- Largest units: `backend.cpp` **1,058**, `cli.cpp` **856**, `mathrender.cpp` **446**, `deck.cpp` **376**, `importer.cpp` **324**, `store.cpp` **274**, `backend.h` **241**.
- `main.cpp` is now **87 LOC** (down from 135); tray code lives in `traycontroller.cpp` (64) / `traycontroller.h` (26).
- Active, disciplined refactoring history (study queue, file history, import parsing, domain enums, shared scheduling reads).
- Strong test suite (`./bin/test`, offscreen Qt Test), injectable paths, env isolation, headless CLI parity.
- The snapshot includes committed day-start-hour work (`bed92cf`), the tray extraction (`2ab7293`) and the test split (`b0ec959`, `3f9177f`), all local only. Re-check these numbers before starting #1.

Overall rating: **8.5/10**. See strengths/weaknesses below.

## Progress

| # | Item | Status |
|---|------|--------|
| 1 | Decompose `Backend` | Not started (epic — split into sub-items) |
| 2 | Extract tray + app bootstrap from `main.cpp` | **Done** (`src/traycontroller.{h,cpp}`, `2ab7293`) |
| 3 | Centralize configuration constants | **Done** (`src/config.h`, `Config::againDelayMs()`, `Backend::fromEnvironment()`) |
| 4 | Type safety at boundaries | **Deferred** — explicitly out of scope for this pass; revisit later |
| 5 | Split the test file | **Done** (11 classes + runner, `b0ec959`/`3f9177f`) |
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
- Tray icon/menu lifecycle isolated in `TrayController`; `main()` is startup and QML wiring only.
- Tests split into 11 area classes with a shared support header and single runner; no test file exceeds 592 LOC.

## Weaknesses / debt

- `Backend` is a god object (**1,058 LOC cpp / 241 LOC header**): UI state, scheduling, persistence, file I/O, math, history.
- `QString` is the universal currency, including at the QML boundary; enums round-trip through strings, losing type safety outside `Domain`. **Deferred with #4** — the concrete string comparisons in `backend.cpp` are already routed through `Domain::*`.
- Hardcoded constants: Again delay 60000 ms, new-per-day 20, retention 0.9, Leitner boxes 1/2/4/8/16 — largely centralized by #3 (day-start-hour is the newest addition).
- `Backend` constructor defaults to empty paths, relying on env vars — implicit and surprising; mitigated by `Backend::fromEnvironment()`.
- ~~`main.cpp` mixes tray icon drawing, CLI dispatch, QML engine setup.~~ Resolved by #2.
- ~~Minor ownership leak: `new QMenu` without parent in tray code.~~ Resolved by #6 and preserved by #2 (`m_menu` declared before `m_tray`).
- ~~No lint/format/CI config.~~ Resolved by #6.
- ~~Tests are a single monolithic file (1,386 LOC, 52 slots).~~ Resolved by #5.
- ~~Optional toolchain detection (math) is runtime and ad hoc.~~ Resolved by #7.

## Near-term improvements (do before new features)

Priority order. Only #1 remains (epic, split into sub-items). #2, #3, #5, #6 and #7 are done; #4 (type safety) is intentionally deferred to a later pass and is not part of this work.

### 1. Decompose `Backend` (highest impact, epic)
- Extract `StudySession` (queue advancement, repeat scheduling, reviewed/correct/box counters, stats).
- Extract `CardRepository` (deck/card CRUD, file read/write, watcher, reload).
- Keep `Backend` as a thin QML/CLI façade that delegates and re-emits signals.
- Target: stage it — first get `backend.cpp` < 600 LOC, then < 400 LOC. Moving ~650 LOC in one step is not realistic.
- Preserve the `StudySnapshot` invariant while splitting: availability queries and card views must still read one consistent snapshot.
- Do not pull type-safety changes (#4) into this item; keep `m_mode` as-is for now.

### 2. Extract tray + app bootstrap from `main.cpp` — ✅ Done (`2ab7293`)

Moved tray icon painting, menu construction and signal wiring into
`src/traycontroller.{h,cpp}`; `main()` now only handles startup, QML wiring
and `app.exec()`. `TrayController` holds `m_menu` as a `std::unique_ptr<QMenu>`
declared before `m_tray` so the tray is destroyed first and never owns a
dangling context menu. Kept the existing `"review"` / `"cram"` boundary
strings (no type-safety changes). Verified with `./bin/build`, a clean
`clang-format --dry-run --Werror` and `./bin/test` at 54/54; manual tray
checks (icon, theme recolor, tooltip, menu actions, right-click) still need
a desktop run.

### 3. Centralize configuration constants — ✅ Done (`db98d7b`, `bed92cf`)

`src/config.h` holds the named defaults and `Backend::fromEnvironment()` reads
env overrides in one place; day-start-hour was layered in later.

### 4. Type safety at boundaries — deferred

Not in this pass. The original intent is parked here for later:

- Keep `QString` at QML/CLI/file boundaries (required), but convert at the edge: parse into `Domain` enums inside `Backend` slots, never store mode/rating/type as raw strings internally.
- Audit `backend.cpp` for string comparisons like `mode == "review"` and replace with enum checks.

Reality check for whoever picks this up: the audit is already largely satisfied — `backend.cpp` routes every decision through `Domain::modeFromString`, `Domain::ratingFromInt` and `Domain::cardTypeFromString`. The remaining decision is whether to change the `m_mode` member to `StudyMode` (small) and/or `Card::type` to `CardType` (ripples into `deck.cpp`, `cli.cpp`, importer and tests). Define that scope before starting.

### 5. Split the test file — ✅ Done (`b0ec959`, `3f9177f`)

Replaced the 1,386-line `tests/omacards_tests.cpp` with 11 area classes
(`TestCli` 10 slots, `TestBackend` 19, `TestStudyQueue` 5, `TestStore` 4,
`TestFsrs` 3, `TestTheme` 3, `TestDeck` 2, `TestMathRender` 2,
`TestFileHistory` 2, `TestConfig` 1, `TestDomain` 1), plus
`tests/test_support.h` and a single `tests/test_main.cpp` runner.

- All 52 slot bodies moved verbatim — only a `void TestX::` qualifier and a
  one-level dedent were applied; a token-level comparison against the old
  file found zero semantic diffs.
- `TestSupport` holds `EnvGuard` plus inline `appBinary()`, `runCli()` and
  `writeFile()`; each test `.cpp` has `using namespace TestSupport;` so call
  sites are unchanged. All env/permission guards and SQLite connection
  cleanups were carried over intact.
- `Q_OBJECT` lives in the new headers so qmake AUTOMOC generates
  `moc_test_*.cpp` with no manual `.moc` includes. The runner creates one
  `QGuiApplication` (the test `.pro` has no widgets module, matching the old
  `QTEST_MAIN`) and calls `QTest::qExec` once per class.
- Landed in two commits: `b0ec959` extracted the support header with the
  monolith still green at 54/54, then `3f9177f` moved all slots atomically.
  A half-migrated tree either has two `main()`s or silently drops tests.
- Verified from a clean `build/tests`: `./bin/test` reports **74 passed,
  0 failed** — 52 slots plus two init/cleanup passes per class (up from
  54 when one runner owned them). `clang-format --dry-run --Werror` clean.

### 6. Tooling & hygiene — ✅ Done (`db98d7b`, `2ab7293`)

`.clang-format` committed and applied, CI runs build + offscreen tests, the
tray `QMenu` ownership bug fixed, and `Backend::fromEnvironment()` added as the
named factory for injected paths.

### 7. Math renderer cleanup — ✅ Done (`db98d7b`)

Toolchain detection consolidated into `MathRenderer::detect()` returning an
enum (`None`, `Unicode`, `LatexSvg`).

## Explicitly out of scope (do not bundle)

- Type safety at boundaries (#4) — deferred to a later pass.
- New study modes, new card types, sync/cloud, mobile.
- Changing deck file format or card id scheme.
- FSRS weight tuning (keep reference weights).

## Definition of done for this plan

- [ ] `backend.cpp` < 400 LOC (staged via #1: interim target < 600); no type-safety changes bundled.
- [x] Tray/bootstrap extracted; `main.cpp` contains no tray drawing or menu wiring. (#2, `2ab7293`)
- [x] Tests split into 11 per-area classes with a shared support header and single runner; 74/74 green (52 slots). (#5, `b0ec959`/`3f9177f`)
- [x] `.clang-format` committed and applied.
- [x] CI runs build + tests; last remote run succeeded on `c21b67a` (later local commits unpushed).
- [x] No behavior change so far: `./bin/test` was 54/54 before and after #2 and is 74/74 after #5 (new total from 11 runners).
