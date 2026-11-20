# Refactoring Plan — omacards

Companion to the code-quality review. Goal: pay down structural debt **before** adding new features, so new work lands on a cleaner foundation.

## Current state (snapshot, re-measured 2026-11-19)

- **4,879 LOC** of C++/Qt 6 across `src/` (13 `.cpp` + 13 `.h`), plus `src/Main.qml` and i18n resources.
- **1,386 LOC** in one test file (`tests/omacards_tests.cpp`), containing **52 test slots**, plus a 15-line `tests/omacards_tests.pro`.
- Largest units: `backend.cpp` **1,058**, `cli.cpp` **856**, `mathrender.cpp` **446**, `deck.cpp` **376**, `importer.cpp` **324**, `store.cpp` **274**, `backend.h` **241**.
- Active, disciplined refactoring history (study queue, file history, import parsing, domain enums, shared scheduling reads).
- Strong test suite (`./bin/test`, offscreen Qt Test), injectable paths, env isolation, headless CLI parity.
- The snapshot above includes uncommitted day-start-hour work (`config.h`, `backend.*`, `store.*`, tests). Land that feature before starting #1 or #5, then re-check these numbers.

Overall rating: **8.5/10**. See strengths/weaknesses below.

## Progress

| # | Item | Status |
|---|------|--------|
| 1 | Decompose `Backend` | Not started (epic — split into sub-items) |
| 2 | Extract tray + app bootstrap from `main.cpp` | Not started (detailed plan below) |
| 3 | Centralize configuration constants | **Done** (`src/config.h`, `Config::againDelayMs()`, `Backend::fromEnvironment()`) |
| 4 | Type safety at boundaries | **Deferred** — explicitly out of scope for this pass; revisit later |
| 5 | Split the test file | Not started (detailed plan below) |
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

- `Backend` is a god object (**1,058 LOC cpp / 241 LOC header**): UI state, scheduling, persistence, file I/O, math, history.
- `QString` is the universal currency, including at the QML boundary; enums round-trip through strings, losing type safety outside `Domain`. **Deferred with #4** — the concrete string comparisons in `backend.cpp` are already routed through `Domain::*`.
- Hardcoded constants: Again delay 60000 ms, new-per-day 20, retention 0.9, Leitner boxes 1/2/4/8/16 — largely centralized by #3 (day-start-hour is the newest addition).
- `Backend` constructor defaults to empty paths, relying on env vars — implicit and surprising; mitigated by `Backend::fromEnvironment()`.
- `main.cpp` mixes tray icon drawing, CLI dispatch, QML engine setup.
- ~~Minor ownership leak: `new QMenu` without parent in tray code.~~ Resolved by #6; keep the fix when moving the code in #2.
- ~~No lint/format/CI config.~~ Resolved by #6.
- Tests are a single monolithic file (**1,386 LOC, 52 slots**).
- ~~Optional toolchain detection (math) is runtime and ad hoc.~~ Resolved by #7.

## Near-term improvements (do before new features)

Priority order. #2 and #5 are independent, single-commit changes. #1 is an epic and should be split into sub-items. #4 (type safety) is intentionally deferred to a later pass and is not part of this work.

### 1. Decompose `Backend` (highest impact, epic)
- Extract `StudySession` (queue advancement, repeat scheduling, reviewed/correct/box counters, stats).
- Extract `CardRepository` (deck/card CRUD, file read/write, watcher, reload).
- Keep `Backend` as a thin QML/CLI façade that delegates and re-emits signals.
- Target: stage it — first get `backend.cpp` < 600 LOC, then < 400 LOC. Moving ~650 LOC in one step is not realistic.
- Preserve the `StudySnapshot` invariant while splitting: availability queries and card views must still read one consistent snapshot.
- Do not pull type-safety changes (#4) into this item; keep `m_mode` as-is for now.

### 2. Extract tray + app bootstrap from `main.cpp`

**Goal.** After this change `main()` does only: argument parsing, offscreen dispatch, `QApplication`/style setup, translator/theme/backend construction, QML engine load, the `activate` callback, and `app.exec()`. All tray icon drawing, menu construction and signal wiring moves into a new `TrayController`.

**Files**
- New `src/traycontroller.h` — class declaration.
- New `src/traycontroller.cpp` — icon painting (moved from `main.cpp:30-45`) plus implementation.
- Edit `src/main.cpp` — remove `trayIcon()` and the whole `if (QSystemTrayIcon::isSystemTrayAvailable()) { ... }` block (`main.cpp:101-131`); instantiate the controller.
- Edit `omacards.pro` — add both files to `HEADERS` / `SOURCES`.

**API sketch** (`src/traycontroller.h`)

```cpp
#pragma once
#include <QObject>
#include <QSystemTrayIcon>
#include <functional>
#include <memory>

class Backend;
class Theme;
class QMenu;

class TrayController : public QObject {
public:
    TrayController(Backend &backend, const Theme &theme, std::function<void()> activate,
                   QObject *parent = nullptr);

private:
    Backend &m_backend;
    const Theme &m_theme;
    std::function<void()> m_activate;
    std::unique_ptr<QMenu> m_menu;   // declared BEFORE m_tray on purpose
    QSystemTrayIcon m_tray;
};
```

No `Q_OBJECT` macro is needed: the class only uses base-`QObject` `QObject::connect` with `this` as context. (If own signals are added later, add `Q_OBJECT`; qmake AUTOMOC picks it up from `HEADERS`.)

**Implementation steps**

1. Move `trayIcon(const QString &accent)` verbatim into an anonymous namespace at the top of `traycontroller.cpp` (theme lookup, `QPixmap`/`QPainter` "C" fallback).
2. Constructor initializer list, in member-declaration order: `m_backend(backend)`, `m_theme(theme)`, `m_activate(std::move(activate))`, `m_menu(std::make_unique<QMenu>())`, `m_tray(trayIcon(theme.accent()))`.
   - **Destruction order matters:** members are destroyed in reverse declaration order, so `m_tray` dies before `m_menu`. This preserves the ownership fix from #6 (`main.cpp:102-104`) — `QSystemTrayIcon` does not own its context menu.
3. Constructor body, reproducing today's behavior exactly:
   - `m_tray.setToolTip(QObject::tr("omacards — %1 due").arg(m_backend.dueCount()));`
   - Build menu in the current order: `Open` → `m_activate`; `Review` → `{ m_backend.setMode("review"); m_activate(); }`; `Cram` → `{ m_backend.setMode("cram"); m_activate(); }`; `addSeparator()`; `Quit` → `&QApplication::quit`. Keep `QObject::tr` for all labels. Functor-added actions are owned by the menu — no extra cleanup.
   - `m_tray.setContextMenu(m_menu.get());`
   - Connections, all with `this` as the context object so they auto-disconnect:
     - `Backend::dueCountChanged` → lambda: refresh tooltip from `m_backend.dueCount()`.
     - `Theme::changed` → lambda: `m_tray.setIcon(trayIcon(m_theme.accent()))`.
     - `QSystemTrayIcon::activated` → lambda: ignore `QSystemTrayIcon::Context`, otherwise `m_backend.setMode("review")` and `m_activate()`.
   - `m_tray.show();`
4. In `main.cpp`, delete `trayIcon()` and the tray block, drop now-unused includes (`<QColor>`, `<QFont>`, `<QMenu>`, `<QPainter>`, `<QPixmap>`; keep `<QIcon>` for `app.setWindowIcon`, keep `<QSystemTrayIcon>` for the availability check). After the QML window/activate wiring (`main.cpp:93-99`), add:

   ```cpp
   std::unique_ptr<TrayController> tray;
   if (QSystemTrayIcon::isSystemTrayAvailable())
       tray = std::make_unique<TrayController>(backend, theme, activate);
   return app.exec();
   ```

   **Trap:** a stack-local controller declared inside the `if` would be destroyed before `app.exec()`. Use the `unique_ptr` (or declare it before the `if`).
5. Leave the offscreen CLI dispatch (`main.cpp:52-59`) untouched — it must still run before any `QApplication` exists.
6. Keep the existing `"review"` / `"cram"` strings at the boundary; do not pull #4 into this item.

**Verification**
- `./bin/build` succeeds; `./bin/test` stays green (52 slots).
- Manual, on a machine with a system tray: icon appears; theme switch recolors it; tooltip updates when the due count changes; Open/Review/Cram/Quit work; a right-click does not re-open the window.
- Diff review: every removed line from `main.cpp` should have an equivalent line in `traycontroller.cpp`.

### 3. Centralize configuration constants — ✅ Done
- Create `config.h` with named constants: `kDefaultNewPerDay`, `kDefaultRetention`, `kAgainDelayMs`, `kLeitnerBoxes`, `kMaxIntervalDays`.
- Read env overrides (`OMACARDS_AGAIN_DELAY_MS`) in one place; expose via `Backend`/settings.
- Remove scattered literals.

### 4. Type safety at boundaries — deferred

Not in this pass. The original intent is parked here for later:

- Keep `QString` at QML/CLI/file boundaries (required), but convert at the edge: parse into `Domain` enums inside `Backend` slots, never store mode/rating/type as raw strings internally.
- Audit `backend.cpp` for string comparisons like `mode == "review"` and replace with enum checks.

Reality check for whoever picks this up: the audit is already largely satisfied — `backend.cpp` routes every decision through `Domain::modeFromString`, `Domain::ratingFromInt` and `Domain::cardTypeFromString`. The remaining decision is whether to change the `m_mode` member to `StudyMode` (small) and/or `Card::type` to `CardType` (ripples into `deck.cpp`, `cli.cpp`, importer and tests). Define that scope before starting.

### 5. Split the test file

**Goal.** Replace the 1,386-line monolith with one test class per area, a shared support header, and one runner `main()`. All 52 slot bodies move verbatim — this is a file-movement refactor, not a rewrite.

**Files and slot map** (counts total 52)

| New class / files | Slots | Tests moved |
|---|---|---|
| `TestCli` — `test_cli.h/.cpp` | 10 | `helpIsHeadless`, `addCardsCreatesDeckBulk`, `addCardsKeepsExistingDescription`, `addCardsRejectsInvalidInputWithoutWriting`, `addDeckDescriptionAndJsonImport`, `cliConfigReportsWriteFailure`, `cliEditsAndDeletesCardsAcrossDecks`, `previewRendersMathAsText`, `checkMathReportsUnsupportedCommands`, `checkReportsLineNumberedIssues` |
| `TestBackend` — `test_backend.h/.cpp` | 19 | `defaultPathsIgnoreOrganizationName`, `backendCreatesCardsAndCramsForever`, `qmlCanRateCards`, `createCardRejectsInvalidType`, `backendEditsCardsWithUndo`, `backendDeletesDeckAndUndoes`, `backendFiltersCardList`, `backendImportsAnkiClozeAndMedia`, `backendAppliesDayStartToDueCards`, `backendReportsNewCountAndDeckSummaries`, `qmlCanNavigateLibraryCards`, `backendAppliesNewCardLimit`, `backendRequeuesAgainInSameSession`, `unreadableDeckKeepsSchedulingState`, `failedCardWriteDoesNotMutateBackend`, `unreadableStateDoesNotTreatEveryCardAsNew`, `deckCountsMatchLibrarySummaries`, `deckCreationRejectsSanitizedPathCollision`, `refreshKeepsPendingAgainRepeat` |
| `TestStudyQueue` — `test_studyqueue.h/.cpp` | 5 | `studyQueueReviewOrder`, `studyQueueRepeatWaitsThenReturns`, `studyQueueRebuildKeepsPending`, `studyQueueCramBoxes`, `studyQueueRemove` |
| `TestStore` — `test_store.h/.cpp` | 4 | `storeRoundTripsState`, `storeRemembersSettings`, `storeCountsNewCardsToday`, `storeUsesConfiguredDayStart` |
| `TestFsrs` — `test_fsrs.h/.cpp` | 3 | `fsrsInitialState`, `fsrsReviewMovesDueForward`, `fsrsRetentionShapesIntervals` |
| `TestTheme` — `test_theme.h/.cpp` | 3 | `themeFallsBack`, `themeReadsAccent`, `translatorFollowsLanguage` (theme + translator) |
| `TestDeck` — `test_deck.h/.cpp` | 2 | `deckParsesAndSerializes`, `clozeCardsHideAndReveal` |
| `TestMathRender` — `test_mathrender.h/.cpp` | 2 | `mathFallsBackToUnicode`, `tectonicRendererUsesSupportedFlags` |
| `TestFileHistory` — `test_filehistory.h/.cpp` | 2 | `fileHistoryUndoRedoText`, `fileHistoryUndoRemovesCreatedFile` |
| `TestConfig` — `test_config.h/.cpp` | 1 | `configDayStartHelpers` |
| `TestDomain` — `test_domain.h/.cpp` | 1 | `domainParsesSharedValues` |

Placement notes: `preview*`/`check*` are CLI commands → `TestCli`; `defaultPathsIgnoreOrganizationName` constructs `Backend::fromEnvironment()` → `TestBackend`; `backendImportsAnkiClozeAndMedia` builds a `.apkg` fixture but drives `Backend` → `TestBackend`. The three 1–2-slot classes may be merged into a `test_core.cpp` if 11 files feel excessive; keep one class per area otherwise.

**Shared support** — new `tests/test_support.h`

- Move `EnvGuard` out of the anonymous namespace (`omacards_tests.cpp:32-55`) into `namespace TestSupport`.
- Move all three private static helpers: `appBinary()` (`:1359`), `runCli()` (`:1361-1377`), `writeFile()` (`:1379-1382`).
- Mark the free functions `inline` (the header is included by many translation units). Keep the named namespace — an anonymous namespace in a header would give each TU a distinct type.
- `appBinary()` uses `OMACARDS_SOURCE_DIR` from the `.pro` `DEFINES`; that define applies to every test source, so the logic is unchanged.
- Each test `.cpp` starts with `using namespace TestSupport;` so existing `EnvGuard env(...)`, `writeFile(...)` and `runCli(...)` call sites need no edits.

**Per-area class pattern** — e.g. `test_fsrs.h` / `test_fsrs.cpp`

```cpp
// test_fsrs.h
#pragma once
#include <QObject>
class TestFsrs : public QObject {
    Q_OBJECT
private slots:
    void fsrsInitialState();
    void fsrsReviewMovesDueForward();
    void fsrsRetentionShapesIntervals();
};
```

```cpp
// test_fsrs.cpp
#include "test_fsrs.h"
#include "test_support.h"
#include <QtTest>
#include <QDateTime>
#include "fsrs.h"

void TestFsrs::fsrsInitialState() { /* body copied verbatim */ }
// ...
```

Put `Q_OBJECT` in the **headers**, not the `.cpp` files: qmake AUTOMOC then generates `moc_test_*.cpp` automatically and no `#include "*.moc"` lines are needed. The monolith only needs `#include "omacards_tests.moc"` today because its `Q_OBJECT` lives in a `.cpp`.

**Single runner** — new `tests/test_main.cpp`

```cpp
#include <QApplication>
#include <QtTest>
#include "test_backend.h" // ... all 11 headers

int main(int argc, char **argv) {
    QApplication app(argc, argv); // matches QTEST_MAIN; QML/QQuickControls tests need a GUI app
    int failed = 0;
    TestCli cli;                       failed += QTest::qExec(&cli, argc, argv);
    TestBackend backend;               failed += QTest::qExec(&backend, argc, argv);
    // ... one qExec per class, 11 total
    return failed == 0 ? 0 : 1;
}
```

`QApplication` is declared first so it outlives every test object. Passing `argc/argv` keeps Qt Test flags working; note that `-o <file>` is applied per `qExec` call (later classes overwrite earlier output) — use console output for the normal `./bin/test` run. The test objects must be complete types here, which is why the class declarations live in headers.

**Migration steps (keep the suite green)**

1. Commit A — support extraction only: add `test_support.h`, include it from the monolith, delete the monolith's local `EnvGuard` and static helpers, add the header to `.pro` `HEADERS`. `./bin/test` → 52 green.
2. Commit B — the split, done atomically:
   - Create all 11 header/source pairs; move slot bodies verbatim (only adding the `void TestX::` qualifier).
   - Delete `tests/omacards_tests.cpp`, including its `QTEST_MAIN` and `.moc` include.
   - Add `test_main.cpp` with the 11 `qExec` calls.
   - Rewrite `tests/omacards_tests.pro`: `HEADERS` gains `test_support.h` + all `test_*.h`; `SOURCES` becomes `test_main.cpp` + all `test_*.cpp` + the existing `../src/*.cpp` list; remove `omacards_tests.cpp`. Keep `QT += ... testlib`, `CONFIG`, `TARGET`, `INCLUDEPATH`, `DEFINES` unchanged.
   - `./bin/test` → 52 green. Do this in one commit: a half-migrated tree either has two `main()` functions or silently stops running moved tests.
3. Optional follow-up: order `qExec` calls fast-unit-first (DOMAIN/CONFIG/FSRS before QML/CLI) for quicker feedback.

**Invariants and traps**

- Env isolation: each test creates its own `QTemporaryDir`; `EnvGuard` restores both env vars in its destructor and must stay alive for the whole slot. `backendRequeuesAgainInSameSession` and `refreshKeepsPendingAgainRepeat` restore `OMACARDS_AGAIN_DELAY_MS` manually; `tectonicRendererUsesSupportedFlags` restores `PATH` and unsets `OMACARDS_TEST_ARGS` / `OMACARDS_TEST_MISSING_BUNDLE`. Copy these guards unchanged.
- Fixed SQLite connection names (`apkg-fixture`, `omacards-unreadable-test`) are created and removed inside their own tests. Keep them unique/cleaned up now that cross-class execution order changes.
- QTest runs slots in declaration order within a class, so splitting changes global order. All tests are self-contained, but run the whole suite to confirm; a clean `build/tests` directory forces moc regeneration.
- Keep the `QSKIP` branches for permission-restricted environments.
- Distribute Qt includes to the area that uses them; `<QtTest>` in each `.cpp` is simplest. CLI tests still skip if `../build/omacards` is absent — `appBinary()` logic is unchanged.

**Acceptance**
- `./bin/test` reports 52 passes, 0 failures, with no skips beyond the permission/binary ones already present.
- Each moved slot's diff is only a qualification change; no assertion or setup is altered.
- `qmake6` from a clean `build/tests` generates all `moc_test_*.cpp` files without manual `.moc` includes.

### 6. Tooling & hygiene — ✅ Done
- Add `.clang-format` (match existing style) and run it once.
- Add a CI step: build + `./bin/test` offscreen.
- Fix the `QMenu` parent (pass `&tray` or use `std::unique_ptr`).
- Make `Backend` constructor require explicit paths (remove empty defaults) or provide a named factory `Backend::fromEnvironment()`.

### 7. Math renderer cleanup — ✅ Done
- Make toolchain detection a single `MathRenderer::detect()` returning an enum (`None`, `Unicode`, `LatexSvg`) instead of ad hoc checks scattered in `mathEngine()`.

## Explicitly out of scope (do not bundle)

- Type safety at boundaries (#4) — deferred to a later pass.
- New study modes, new card types, sync/cloud, mobile.
- Changing deck file format or card id scheme.
- FSRS weight tuning (keep reference weights).

## Definition of done for this plan

- [ ] `backend.cpp` < 400 LOC (staged via #1: interim target < 600); no type-safety changes bundled.
- [ ] Tray/bootstrap extracted; `main.cpp` contains no tray drawing or menu wiring. (#2)
- [ ] Tests split into per-area classes with a shared support header and single runner; 52/52 green. (#5)
- [x] `.clang-format` committed and applied.
- [x] CI runs build + tests. (workflow added; first remote run pending a push)
- [x] No behavior change so far: `./bin/test` was 51/51 green at the last commit; the uncommitted day-start-hour work adds one slot (52).
