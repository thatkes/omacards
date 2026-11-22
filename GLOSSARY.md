# omacards

omacards is a keyboard-first flashcards app for Omarchy, built for agent-driven study. This context covers card content, the two study modes, and scheduling.

## Language

### Content

**Deck**:
A named collection of cards studied together.
_Avoid_: collection, folder, set

**Current deck**:
The Deck chosen for study in the current and next Study session. It is moved to the top of the last-studied order when chosen.
_Avoid_: active deck, selected deck, last studied deck

**Card**:
A single study item with a question and an answer.
_Avoid_: note, item

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

**Study session**:
One continuous stretch of study with a queue of cards, a currently shown card, and ratings.
_Avoid_: sitting, run

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
A card rated Again that returns later in the same study session.
_Avoid_: retry, requeued card

**Good**:
A rating that means recall succeeded.
_Avoid_: pass, correct, easy

**Leitner box**:
A stage in Cram that decides how many other cards appear before a card returns.
_Avoid_: box, level, bucket

**Review history**:
The record of ratings given to cards over time.
_Avoid_: review log, activity

**Streak**:
The number of consecutive study days with at least one rated review.
_Avoid_: day streak

### Scheduling

**Scheduling state**:
The per-card record of past ratings and the next time a card is scheduled to return, separate from its content.
_Avoid_: progress, FSRS state

**Study day**:
The local day used for daily limits and due dates, beginning at the configured hour.
_Avoid_: calendar day, today

**New card**:
A card that has never been rated.
_Avoid_: fresh card, unseen card

**Due card**:
A card whose scheduled return falls on the current study day.
_Avoid_: ready card, available card, due now

**Available card**:
A card that can be shown next: a due card, a new card within the daily allowance, or a pending repeat whose delay has elapsed.
_Avoid_: due now, ready card

**New-card allowance**:
The number of new cards that may start in Review during one study day.
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

### Workflow

**GitHub Issue**:
A ticket in the project's GitHub issue tracker; a bare "issue" in project conversation means this, not a Deck issue.
_Avoid_: ticket, tracker item
