# omacards maintainer Resources

## Knowledge

- [Repo: omacards README](../README.md)
  The canonical product overview: install, shortcuts, settings, deck format,
  agent CLI, and a layout map. Use for: orienting in the project and checking
  user-visible behaviour before diving into code.
- [Repo: omacards GLOSSARY](../GLOSSARY.md)
  The canonical domain vocabulary. Use for: naming concepts in explanations,
  tests, issues and commits. Never invent synonyms the glossary explicitly avoids.
- [Repo: agent docs](../docs/agents/)
  How engineering skills consume domain docs, the issue tracker (`gh` CLI), and
  triage labels. Use for: the maintainer workflow around the code.
- [Repo: `src/`](../src/)
  The implementation itself, and the highest-trust source for how things work.
  Use for: verifying every architectural claim with a file and line number.
- [Repo: `tests/`](../tests/)
  Qt Test suite. Use for: seeing intended behaviour as executable examples.
- [Qt 6: Overview — QML and C++ Integration](https://doc.qt.io/qt-6/qtqml-cppintegration-overview.html)
  Official explanation of `QObject`, properties/signals/methods, context
  properties, and the boundary between UI in QML and logic in C++. Use for:
  understanding how `src/main.cpp` exposes `backend`, `theme` and `translator`.
- [Qt 6: Getting started with Qt Quick applications](https://doc.qt.io/qt-6/qmlapplications.html)
  Official intro to Qt Quick / QML. Use for: reading `src/Main.qml` without
  guessing about QML syntax.
- [Qt 6: qmake manual](https://doc.qt.io/qt-6/qmake-manual.html)
  Official reference for `.pro` files. Use for: understanding `omacards.pro`
  and `tests/omacards_tests.pro`.
- [FSRS wiki: The Algorithm (open-spaced-repetition)](https://github.com/open-spaced-repetition/awesome-fsrs/wiki/The-Algorithm)
  Maintained by the FSRS creators; includes the FSRS-5 formulas and defaults.
  Use for: verifying the model behind `src/fsrs.cpp` (the code implements
  FSRS-5, not a newer version).
- [FSRS wiki: ABC of FSRS](https://github.com/open-spaced-repetition/awesome-fsrs/wiki/ABC-of-FSRS)
  Short conceptual introduction to Stability, Difficulty and Retrievability.
  Use for: building intuition before reading formulas.
- [py-fsrs (open-spaced-repetition)](https://github.com/open-spaced-repetition/py-fsrs)
  Reference implementation of FSRS. Use for: cross-checking porting details
  when the formulas are ambiguous.

## Wisdom (Communities)

- [GitHub Issues — thatkes/omacards](https://github.com/thatkes/omacards/issues)
  The project's own issue tracker; the canonical place where bugs, trade-offs
  and design decisions are argued out. Use for: learning what a maintainer is
  actually asked to decide, and for asking questions as a new maintainer.
- [Qt Forum](https://forum.qt.io/)
  Official Qt community forum. Use for: Qt-specific questions that are not
  omacards-specific (e.g. why a QML binding behaves a certain way).
- [r/Omarchy](https://www.reddit.com/r/Omarchy/)
  Community around the desktop omacards targets. Use for: understanding the
  theme/language conventions the app is expected to follow.

## Gaps

- `docs/adr/` does not yet exist in this repo. Architecture decisions are
  currently recoverable only from code comments and README. If a non-obvious
  decision matters during a lesson, record it as a learning record rather than
  pretending an ADR exists.
