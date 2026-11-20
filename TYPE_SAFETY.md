# Type Safety Plan — omacards

This document owns the last deferred item from the Backend refactor. The rest
of that refactor is complete (`8188e2c`), so `REFACTORING_PLAN.md` is removed.

## Goal

Keep `QString`/`int` at the boundaries that require them — QML properties,
`Q_INVOKABLE` slots, CLI flags and JSON, deck files and the SQLite sidecar —
but parse those values into `Domain` enums at the edge and use the enums for
every internal decision. Remove raw string comparisons such as
`mode == "review"` and `card.type == "cloze"` from application logic.

This is a behavior-preserving refactor. No QML, CLI, file-format, database or
test-visible contract changes are allowed.

## Current state

`src/domain.{h,cpp}` already defines:

- `StudyMode { Review, Cram }`
- `Rating { Invalid = 0, Again = 1, Good = 3 }`
- `CardType { Invalid, Basic, Cloze }`
- Parsers and stringifiers for all three.

They are used at some decision points, but raw values still remain in:

- `Backend::m_mode` and `StudySession::m_mode` (`QString`)
- `StudySession::rate(int)` before it parses a `Rating`
- `Card::type`, `CardDraft::type` and CLI `Options::type` (`QString`)
- deck parsing, serialization, hashing and display logic
- `cardList()` and CLI JSON output

## Semantics that must not change

| Value | Boundary representation | Parse rule | Invalid behavior |
|---|---|---|---|
| Study mode | `QString` in `Q_PROPERTY mode`, `setMode(QString)`, QML, tray | Exact `"cram"` becomes `Cram`; every other string becomes `Review` | Unknown input is normalized to `"review"` |
| Rating | `int` in `rate(int)`, CLI `-r/--rating`, review log | Exact `1` is Again, exact `3` is Good | Backend no-ops; CLI fails with `Rating must be 1 (Again) or 3 (Good)` |
| Card type, files | `type:` text in deck files | Parser trims then lowercases; empty/`basic` is Basic, `cloze` is Cloze | Unknown lowercased text is preserved as raw text |
| Card type, API | `QString` in CLI flags/JSON, `createCard`, `modifyCard`, `addCardDetails`, `updateCard`, QML editor | No lowercasing at this edge | `createCards` rejects with `cards[%1] has an invalid type: %2`; `modifyCard` normalizes invalid to `"basic"` |

Additional invariants:

- `Q_PROPERTY` names, `Q_INVOKABLE` signatures, signal names and notification
  behavior stay unchanged. `mode` remains a `QString` property.
- CLI JSON keys and values stay unchanged, including `type`, `rating`, the
  `cardList()` `type` field and `statsSummary()` output.
- Deck serialization keeps the text format: no `type:` line for basic cards,
  one `type: <text>` line otherwise. `DeckIO::cardId()` excludes type, while
  `DeckIO::contentHash()` includes the exact type text; the hash bytes must not
  change.
- Unknown file types must survive load → serialize and load → JSON unchanged.
  They display as basic cards but must not be silently rewritten to `basic`.
- `setMode()` keeps its current raw-argument dedupe: passing `"anything"` while
  the mode is `"review"` still resets session counters, emits `modeChanged()`
  then `statsChanged()`, and rebuilds the queue. Compare the raw argument to
  `Domain::toString(currentMode)`, not to the parsed enum.
- `Domain::modeFromString()` must remain total: unknown text is Review.
- `Domain::ratingFromInt()` must keep `Rating::Invalid`; only `StudySession`
  and CLI decide what invalid means.
- `Store::logReview()` and `Fsrs::review()` keep `int` ratings; `StudyQueue`
  and `StudyPlanner` already use `Rating`.
- Status, error and translated strings, deck ordering, daily limits, repeat
  timing, FSRS weights and sidecar schema are out of scope.

## Target model

### 1. Domain additions

Add a small carrier in `src/domain.h` for card types so typed decisions and
lossless boundary text coexist:

```cpp
struct CardTypeValue {
    CardType kind = CardType::Basic;
    QString text = QStringLiteral("basic");
};

namespace Domain {
CardTypeValue parseCardType(const QString &text);
QString toString(const CardTypeValue &value);
}
```

`parseCardType()` keeps the existing `cardTypeFromString()` rules without
lowercasing:

- empty or `basic` → `{ Basic, "basic" }`
- `cloze` → `{ Cloze, "cloze" }`
- anything else → `{ Invalid, text }`

File parsing trims and lowercases before calling it; API/CLI callers pass text
as-is. `toString(CardTypeValue)` returns `value.text`, so an unknown file type
round-trips, while `toString(CardType)` keeps its existing Invalid → `basic`
collapse. Keep `cardTypeFromString()` and `toString(CardType)` as the typed
helpers used by validation and canonical output, implemented in terms of the
new value where that avoids drift. No new source files are needed;
`domain.h/.cpp` are already listed in both `.pro` files.

### 2. Internal storage

- `Backend::m_mode` and `StudySession::m_mode` become `StudyMode`, defaulting
  to `StudyMode::Review`.
- `Card::type` becomes `CardTypeValue`. Add `cardType()` and `cardTypeText()`
  accessors if they make call sites clearer; otherwise use `.kind` and `.text`.
- `CardDraft::type` and CLI `Options::type` stay `QString`: they are inputs,
  not stored state.
- `StudySession::rate()` takes `Rating`. `Backend::rate(int)` parses first and
  returns early for `Rating::Invalid`.
- `StudyPlanner::recordReview()` and `StudyQueue::advanceCram()` keep their
  current `Rating` signatures.

### 3. Boundary conversions

- `Backend::mode()` returns `Domain::toString(m_mode)`.
- `Backend::setMode(QString)` first compares the raw argument to
  `Domain::toString(m_mode)`. On a difference it parses to `StudyMode`, stores
  it, forwards the original raw string to `StudySession::setMode()`, and calls
  `rebuild()` as today.
- `StudySession::setMode(QString)` also compares the raw argument to
  `Domain::toString(m_mode)` before parsing and resetting counters; `mode()`
  returns `StudyMode`. This keeps the raw-vs-canonical dedupe semantics.
- `DeckIO::load()` trims and lowercases the `type:` value, calls
  `Domain::parseCardType()` and stores both fields in `Card::type`.
- `DeckIO::serialize()`, `DeckIO::contentHash()`, `StudyPlanner::cardList()`
  and CLI `cardJson()` use `Domain::toString(card.type)` / `cardTypeText()`.
- `DeckIO::displayQuestion()` and `displayAnswer()` check `CardType::Cloze`.
- `DeckRepository::createCards()` and `modifyCard()` validate with
  `Domain::cardTypeFromString()` as today, then store the canonical
  `CardTypeValue` for valid input. `modifyCard()` still maps invalid input to
  `{ Basic, "basic" }`.
- CLI `--rating` keeps the original `int` for JSON output; call sites convert
  with `Domain::toInt(rating)` before `Fsrs` and `Store`.

## Call-site map

| Area | Files and key sites |
|---|---|
| Mode boundary | `backend.h:24,67,123,179`; `backend.cpp:41,187-193`; `studysession.h:24,35,76`; `studysession.cpp:18-26,37,104,137,147,188`; `traycontroller.cpp:42,46,59`; `Main.qml:40-45,380,384,414,418,563,569,619,633,634,640,641,666,754` |
| Rating boundary | `backend.h:127`; `backend.cpp:199-204`; `studysession.h:32`; `studysession.cpp:95-133`; `cli.cpp:40,96-97,768-793`; `Main.qml:404,410,782,790`; `studyqueue.cpp:102-113` and `studyplanner.cpp:49-58` already use `Rating` |
| Rating storage | `store.h:29`; `store.cpp:47,140-156,188,202`; `fsrs.h:38,41-42`; `fsrs.cpp:18-26,42-92` — keep `int` |
| Card type storage | `deck.h:8-30`; `deck.cpp:60-61,86-88,296-314,352-354,364-374` |
| Card type boundaries | `deckrepository.cpp:262-291,312-366`; `backend.h:106-113,131-132,137-138`; `backend.cpp:244-298`; `cli.cpp:33,82-84,161-167,229-257,610-677`; `studyplanner.cpp:216`; `importer.cpp:192-196,263-305`; `Main.qml:103,1671,1700,1754,1775` |
| Tests | `test_domain.cpp:9-19`; `test_deck.cpp:10-53`; `test_backend.cpp` mode/rating/type cases; `test_cli.cpp:33-51`; `test_studyqueue.cpp:63-84`; `test_fsrs.cpp`; `test_store.cpp` |

`domain.{h,cpp}` is already listed in `omacards.pro` and
`tests/omacards_tests.pro`; the plan adds no new `.cpp` or `Q_OBJECT` header,
so no `.pro` changes are expected.

## Work stages

Each stage is its own green commit: format touched C++ files, run
`./bin/build`, run `./bin/test`, then commit. Do not combine stages.

### Stage 0 — Characterization tests (no production changes)

Add regression tests first so the refactor has a behavioral baseline.

- `tests/test_domain.cpp`: pin mode fallback, rating validity and card-type
  parse rules, including empty, uppercase and unknown text.
- `tests/test_backend.cpp`:
  - `setMode("banana")` reports `mode() == "review"` but still emits
    `modeChanged()`/`statsChanged()` and rebuilds when called from Review;
  - `setMode("CRAM")` reports `mode() == "review"` with the same side effects;
  - identical canonical input remains a no-op;
  - `setMode("cram")` still switches and resets session counters;
  - `rate(0)`, `rate(2)` and `rate(4)` do not change counters or status;
  - invalid `createCard` type is rejected with the existing error text;
  - `modifyCard` with an invalid type normalizes the card to basic.
- `tests/test_deck.cpp`:
  - `type: CLOZE` in a file loads as Cloze and serializes as `type: cloze`;
  - `type: future` loads with raw text `future`, displays as basic, keeps its
    `cardId()` (type is not part of identity), and round-trips as
    `type: future`;
  - `contentHash()` value is pinned for omitted, `basic`, `cloze` and unknown
    type text so sidecar matching cannot change.
- `tests/test_cli.cpp`: keep assertions on JSON `type` and `rating` values and
  the exact invalid-rating error.

Run the full suite before touching production code.

### Stage 1 — Domain carrier and parser

- Add `CardTypeValue`, `Domain::parseCardType()` and
  `Domain::toString(CardTypeValue)` in `src/domain.{h,cpp}`.
- Reimplement `cardTypeFromString()` in terms of the new parser without
  changing return values.
- Extend `tests/test_domain.cpp` for the new carrier, especially that unknown
  text is retained and uppercase text is not accepted as Cloze.

### Stage 2 — Typed study mode

- Change `m_mode` in `Backend` and `StudySession` to `StudyMode`.
- Keep both `setMode(QString)` signatures as raw-text boundaries; compare the
  argument with `Domain::toString(m_mode)` before parsing, then store the enum.
  Do not switch the dedupe check to parsed enums.
- Keep `Backend::mode()` string-based; make `StudySession::mode()` return
  `StudyMode`.
- Replace every `Domain::modeFromString(m_mode)` decision with a direct enum
  check.
- Preserve `modeChanged()` then `statsChanged()` emission order, and keep
  `Backend::setMode()` calling `rebuild()` after forwarding.

### Stage 3 — Typed ratings

- Parse in `Backend::rate(int)` and pass `Rating` to `StudySession::rate()`.
- Keep the `hasCard()` check and counter increments in `StudySession`.
- Leave CLI parsing, `Fsrs`, `Store::logReview()`, `Store::summary()` and the
  SQLite schema on `int`, with `Domain::toInt()` at each conversion.
- Keep CLI JSON `"rating"` as the original `int` argument and the exact failure
  text for invalid values.
- Verify invalid ratings still produce no signal side effects beyond the
  existing checks.

### Stage 4 — Typed card types

- Change `Card::type` to `CardTypeValue` in `src/deck.h`.
- Update `src/deck.cpp` parsing, serialization, hashing and cloze display.
- Update `src/deckrepository.cpp` create/modify paths to store canonical
  values and keep the current error messages.
- Update `src/studyplanner.cpp` `cardList()`, `src/cli.cpp` `cardJson()` and
  the `edit-card` default type to emit the boundary text so QML string
  comparisons and CLI JSON remain unchanged.
- Leave `CardDraft::type`, importer output and QML string literals unchanged;
  QML and CLI continue to send `"basic"`/`"cloze"`.
- Update existing tests that compare `card.type` to a string: internal tests
  assert the enum, while JSON/file tests assert the text.

### Stage 5 — Sweep and verification

- Search `src/` for remaining raw comparisons:
  `mode == "`, `mode != "`, `type == "`, `type != "`, `rating == 1/3`, and
  literal `1`/`3` passed into study code. Allowed residues are `domain.cpp`,
  file/CLI parsers, serializers, `fsrs.cpp`, `store.cpp` and tests.
- Confirm no public string, signal or JSON key changed.
- Run `./bin/build`, `clang-format --dry-run --Werror` on all touched files,
  and `./bin/test`.
- Update `README.md` only if file layout or public behavior changed; none is
  expected.

## Done criteria

- No raw mode/card-type string comparisons remain in application logic except
  the intentional raw-argument dedupe in the two `setMode()` boundaries, file
  parsing and serialization.
- Ratings are `Rating` from `Backend::rate()` through `StudyPlanner` and
  `StudyQueue`; only persistence and FSRS use `int`.
- Unknown deck-file card types round-trip byte-for-byte and keep their ids.
- All 77 existing cases still pass, plus the new characterization tests.
- `./bin/build` is warning-free and `clang-format --dry-run --Werror` is clean.
- CLI smoke checks retain the existing keys and values for `cards --json`,
  `add-card --json`, `edit-card --json` and `review --json`.

## Out of scope

- New study modes, card types, sync/cloud or mobile support.
- Changing deck files, card ids, FSRS weights, the SQLite schema or QML enums.
- Making other string-like fields typed (`DeckIssue::severity`, CLI `--format`,
  math engine names) unless a separate need is filed.
