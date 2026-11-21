# Mission: Onboard as an omacards maintainer

## Why
Become a confident maintainer of omacards: able to navigate the code, trace how a
card flows from a plain-text deck through scheduling to the QML study view, and
ship small changes with tests passing — without needing prior Qt experience.

## Success looks like
- Build, run and test the app locally (`./bin/build`, `./bin/test`), and explain what each command does.
- Name the major C++ components and the one QML view, and say what each owns.
- Trace a rating in Review from a keypress in QML to scheduling state in SQLite.
- Explain why Review uses FSRS and Cram uses Leitner, and where to change each.
- Use the agent CLI and deck format fluently enough to reproduce and verify bugs.
- Follow repo conventions: glossary vocabulary, issue tracker, small tested commits.

## Constraints
- General programmer; Qt and QML are new. Explain Qt concepts when they first appear.
- Teaching files live inside the omacards repo alongside the code.
- No fixed time budget per lesson.

## Out of scope
- Deriving the FSRS math from first principles (use the reference implementation as the source of truth).
- Deep Qt internals (render pipelines, event-loop minutiae) beyond what a maintainer needs.
- Packaging/release mechanics beyond knowing that `pkgbuild/` exists.
