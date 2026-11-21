# ADR 0001 - Rating recording is separate from planning

Date: 2026-11-21

## Context

Issue #13 began with the same work in two places. CLI review read the Store,
called Fsrs, wrote Scheduling state and appended to Review history. The GUI did
the same inside `StudyPlanner::recordReview()`. A change to one path could leave
the other behind.

The glossary calls this act Rating recording. It covers the content hash guard,
the Fsrs call, the Scheduling state write and the Review history entry. Planning
asks a different question: given the stored state, when should a Card come back,
and how many New cards may start today?

Deepening `StudyPlanner` to own both looked tempting because it already held a
snapshot of states and hashes. It would still leave CLI review with its own copy
of the same sequence, so a fix to the hash guard would need two changes and two
sets of tests. The planner would also have to patch its snapshot after every
write, including its own copy of the allowance arithmetic.

## Decision

`RatingRecording` is the only writer. It reads the starting state from the
Store, applies the hash guard, calls Fsrs, saves the new state and appends to
Review history. CLI review and `StudySession` are adapters over it.

`StudyPlanner` only reads. `stateFor()` stays for predicted recall display.
Availability, due ordering and New-card allowance come from a snapshot refreshed
from the Store. The allowance is recomputed from Review history on refresh, not
decremented by a write.

## Consequences

There is one place to change and verify Rating recording. Planner tests cover
planning; the write-failure case lives with the Rating recording tests.

The cost is one extra refresh after a GUI rating. That is a cheap re-read, and
it keeps the allowance arithmetic in one place.
