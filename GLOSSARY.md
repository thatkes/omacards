# omacards

omacards is a keyboard-first flashcards app for Omarchy, built for agent-driven study. This context covers card content, the two study modes, and scheduling.

## Language

### Content

**Deck**:
A named collection of cards studied together.
_Avoid_: collection, folder, set

**Current deck**:
The Deck chosen for study in the current and next Study session. It is moved ahead of the Previous deck when chosen.
_Avoid_: active deck, selected deck, last studied deck

**Previous deck**:
The Deck studied most recently before the Current deck; it appears next in the Deck selector.
_Avoid_: prior deck, earlier deck

**Candidate deck**:
A Deck under consideration in the open Deck selector before it is committed as the Current deck.
_Avoid_: selected deck, highlighted deck, pending deck

**Deck selector**:
The control that orders Decks and commits a Candidate deck as the Current deck.
_Avoid_: deck picker, picker

**Deck file**:
The plain-text file in the decks directory that holds a Deck's description and Cards.
_Avoid_: deck source, deck text file

**Deck description**:
The optional prose stored in a Deck file's `# Description:` line and shown with the Deck.
_Avoid_: deck metadata, description text

**Deck id**:
The canonical name that identifies a Deck wherever one is requested. A Deck file's filename may differ from it.
_Avoid_: deck name, deck path, deck filename

**Card**:
A single study item with a question and an answer.
_Avoid_: note, item

**Card type**:
The classification of a Card as Basic or Cloze, stored with the card and deciding how its Question and Answer are shown.
_Avoid_: card kind, kind

**Card id**:
The identifier of a Card, generated from its Deck, Question and Answer unless an `id:` line in the Deck file pins it.
_Avoid_: card key, card hash

**Question**:
The text shown when a card is presented.
_Avoid_: front, prompt

**Answer**:
The text revealed after a question.
_Avoid_: back, response

**Tag**:
A searchable word or phrase attached to a card.
_Avoid_: label, category

**Basic card**:
A card whose answer is stated directly rather than hidden in the question.
_Avoid_: standard card, Q&A card

**Cloze card**:
A card whose answer is a passage hidden inside the question.
_Avoid_: fill-in-the-blank card

**Import**:
The act of bringing Cards from an Anki package, CSV/TSV file or JSON file into a Deck.
_Avoid_: ingest, load cards

**Math expression**:
A LaTeX fragment inside a Question or Answer that the app displays as an image rather than as text.
_Avoid_: formula, equation, TeX snippet

**Rendered math**:
The image inserted into rich text in place of a Math expression.
_Avoid_: math image, formula graphic, math picture

### Study

**Study mode**:
The rule set used for a session: Review or Cram.
_Avoid_: study type, learning mode

**Review**:
The study mode that schedules each card over the long term according to predicted recall.
_Avoid_: scheduled study, FSRS mode

**Cram**:
The study mode that reorders cards within one session and keeps no scheduling state.
_Avoid_: drill, sprint

**Study screen**:
The screen that presents the current Flashcard and its Study footer.
_Avoid_: study view, study pane

**Study session**:
One continuous stretch of study with a queue of cards, a currently shown card, and ratings.
_Avoid_: sitting, run

**All-decks session**:
A Study session whose Study queue is drawn from every Deck. The Current Deck stays unchanged.
_Avoid_: global session, cross-deck session

**Study queue**:
The Cards still to be presented in a Study session, in the order the session will show them.
_Avoid_: card queue, pending cards

**Additional New cards**:
New cards added to the Current Deck's Study queue for the current Study session only. They do not change the New-card allowance or any daily count.
_Avoid_: extra new cards, additional allowance, bonus cards

**Flashcard**:
A Card presented in the Study screen.
_Avoid_: card

**Reveal**:
Showing the Answer for the current Flashcard in a Study session.
_Avoid_: flip

**Flashcard viewport**:
The vertically scrollable area of the Study screen that shows the Question and, when revealed, the Answer.
_Avoid_: scroll view, card pane

**Scroll boundary**:
The top or bottom limit of scrollable content in the Flashcard viewport where further scrolling is not possible.
_Avoid_: edge

**End-of-scroll hint**:
A Study screen cue shown when revealed scrollable content of the Flashcard viewport is at its bottom Scroll boundary, indicating how Up/Down behave under current key rules.
_Avoid_: flip hint

**Rating**:
The learner's judgement of a presented card: Again or Good.
_Avoid_: grade, score, answer

**Rating recording**:
The act of recording a Rating for a Card, producing an updated Scheduling state and an entry in Review history.
_Avoid_: applying a Rating, saving a review, rating write

**Again**:
A rating that means recall failed and creates a pending repeat later in the same session.
_Avoid_: fail, retry

**Pending repeat**:
A Card rated Again that has not been shown again in the same Study session.
_Avoid_: retry, requeued card

**Pending-repeat delay**:
The time a Pending repeat waits before it can be shown again while other cards remain available in the same Study session.
_Avoid_: again delay, retry delay, wait time

**Good**:
A rating that means recall succeeded.
_Avoid_: pass, correct, easy

**Leitner box**:
A stage in Cram that orders when a Card returns relative to the other cards in the session.
_Avoid_: box, level, bucket

**Cram return interval**:
The number of other cards shown before a Card returns in a Cram session.
_Avoid_: cards until repeat, next-in count

**Box distribution**:
The count of Cards in each Leitner box during a Cram session.
_Avoid_: box histogram, box breakdown, level counts

**Box distribution display**:
The view in the Study footer that shows the Box distribution for the current Cram session.
_Avoid_: box histogram view, Leitner chart

**Predicted recall**:
The estimated probability of recall shown for the Flashcard during Review.
_Avoid_: retrievability, recall chance

**Study footer**:
The fixed bottom area of the Study screen that holds the Rating controls and session status.
_Avoid_: status bar, bottom bar

**Study statistics**:
The counts and rates describing Ratings in a Study session or Study day, shown in the Study footer and in Headless command output.
_Avoid_: stats summary, review summary, stats

**Accuracy**:
The percentage of Ratings in a stats scope that were Good, rounded to an integer and zero when there are no Ratings.
_Avoid_: percent correct, Good rate

**Stats panel**:
The overlay opened with `S` that shows Study statistics for the current Study day and Study session.
_Avoid_: stats view, stats overlay, stats screen

**Review history**:
The record of ratings given to cards over time.
_Avoid_: review log, activity

**Streak**:
The number of consecutive study days with at least one rated review.
_Avoid_: day streak

### Math rendering

**Math scale**:
The multiplier applied to Rendered math for the surface showing it, so Questions and Answers can use different sizes.
_Avoid_: zoom, magnification, font size

**Math logical size**:
The width and height Rendered math reports as its own before Math scale is applied.
_Avoid_: intrinsic size, natural size, SVG size

**Math reserved box**:
The space held for Rendered math before its image is ready, kept afterwards so surrounding text does not move.
_Avoid_: placeholder size, estimated box, reservation

**Unicode fallback**:
The plain-text approximation of a Math expression shown while its Rendered math is unavailable or still being prepared.
_Avoid_: approximation, plain math, text substitute

### Scheduling

**Scheduling state**:
The per-card record of past ratings and the next time a card is scheduled to return, separate from its content.
_Avoid_: progress, FSRS state

**Study day**:
The local day used for daily limits and due dates, beginning at the configured hour.
_Avoid_: calendar day, today

**New card**:
A card whose current content has never been rated.
_Avoid_: fresh card, unseen card

**Due card**:
A Card that will be shown in the current Study session: one whose scheduled return falls on or before the current Study day, a New card within the New-card allowance, or a Pending repeat.
_Avoid_: due now, ready card

**Available card**:
A Due card that can be shown right now.
_Avoid_: due now, ready card

**New-card allowance**:
The number of New cards that may start in Review for a Deck during one Study day.
_Avoid_: new cards per day, daily cap, new budget

**Desired retention**:
The probability of recall that Review scheduling aims to maintain.
_Avoid_: retention target, target recall

### Settings

**Settings**:
The learner-chosen values that govern the New-card allowance, Day-start hour and Desired retention.
_Avoid_: preferences, options, configuration

**Settings screen**:
The screen that shows the Settings a learner can change.
_Avoid_: settings dialog, settings window, preferences pane

**Day-start hour**:
The local hour at which a Study day begins.
_Avoid_: day start, day starts at, rollover hour

### Validation

**Deck issue**:
A problem detected in a Deck's content or file. An Error blocks a write; a Warning does not.
_Avoid_: problem, diagnostic, lint finding

**Error**:
A Deck issue that blocks a write and rolls the change back.
_Avoid_: failure, hard issue

**Warning**:
A Deck issue that does not block a write.
_Avoid_: advisory, soft issue

**Math warning**:
A Warning about a Math expression in a Card, reported by `check`.
_Avoid_: math issue, math error

### Interface

**Headless command**:
A command that runs without the GUI and reports its outcome through exit status, with an optional JSON result.
_Avoid_: CLI command, agent command

**Command path**:
The command and any subcommand that identify what a Headless command does, without flags or arguments.
_Avoid_: command line, invocation

### Workflow

**GitHub Issue**:
A ticket in the project's GitHub issue tracker; a bare "issue" in project conversation means this, not a Deck issue.
_Avoid_: ticket, tracker item

**Deck edit history**:
The record of whole-file Deck edits that lets a learner undo or redo a change.
_Avoid_: file history, undo stack
