#include "studysession.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace {
QVector<Card> shuffledCramOrder(const QVector<Card> &cards) {
    QVector<Card> ordered = cards;
    std::shuffle(ordered.begin(), ordered.end(), *QRandomGenerator::global());
    return ordered;
}
} // namespace

StudySession::StudySession(DeckRepository &decks, StudyPlanner &planner, RatingRecording &ratings, CramOrder cramOrder,
                           QObject *parent)
    : QObject(parent), m_decks(decks), m_planner(planner), m_ratings(ratings),
      m_cramOrder(cramOrder ? std::move(cramOrder) : CramOrder(shuffledCramOrder)),
      m_repeatDelayMs(Config::againDelayMs()) {
    m_repeatTimer.setSingleShot(true);
    connect(&m_repeatTimer, &QTimer::timeout, this, [this] { presentCard(); });
    connect(&m_math, &MathRenderer::mathRendered, this, &StudySession::applyMathRenderResult);
}

void StudySession::setMode(const QString &mode) {
    if (mode == Domain::toString(m_mode))
        return;
    m_mode = Domain::modeFromString(mode);
    // A mode switch is a new sitting for this deck: nothing from the old mode's
    // queue, Pending repeats or timer survives it. rebuildQueue() presents the
    // new mode's first Flashcard; the counters settle before the one stats
    // notification so subscribers never see a half-switched session.
    m_repeatTimer.stop();
    m_progression.clear();
    m_sessionRatedIds.clear();
    m_sessionAddedIds.clear();
    m_reviewed = 0;
    m_good = 0;
    emit modeChanged();
    rebuildQueue(m_deckName, false);
    emit statsChanged();
}

void StudySession::rebuild(const QString &deckName) {
    rebuildQueue(deckName, true);
}

void StudySession::rebuildQueue(const QString &deckName, bool notifyStats) {
    if (deckName != m_deckName)
        m_sessionAddedIds.clear();
    m_deckName = deckName;
    // A watcher refresh can rebuild the queue while a repeat is pending. Keep
    // those deadlines for cards that are still available; a cram queue is
    // rebuilt from scratch and clears them instead.
    const QHash<QString, qint64> pending = m_progression.pendingRepeats();
    m_repeatTimer.stop();
    m_math.clearPendingMath();
    m_currentId.clear();

    const Deck *deck = m_decks.deckByName(deckName);
    if (deck && m_mode == StudyMode::Cram) {
        // The module only orders the Cards it is given; the injected policy
        // decides whether a rebuild shuffles them or keeps deck-file order.
        m_progression.buildCram(m_cramOrder(deck->cards));
    } else if (deck) {
        QStringList dueIds;
        QStringList newIds;
        m_planner.reviewIds(deckName, &dueIds, &newIds);
        m_progression.buildReview(dueIds, newIds, pending);
        // Add 5 cards live only in this sitting. A watcher refresh rebuilds
        // the queue from the Store, so put unrated session cards back in their
        // original order. Rated cards stay out; their Pending repeats, if
        // any, were restored above from the progression's deadlines.
        QStringList restored;
        QStringList stillInDeck;
        for (const QString &id : m_sessionAddedIds) {
            const bool present = std::any_of(deck->cards.cbegin(), deck->cards.cend(),
                                             [&id](const Card &card) { return card.id == id; });
            if (!present)
                continue;
            stillInDeck << id;
            if (!m_sessionRatedIds.contains(id) && !m_progression.contains(id))
                restored << id;
        }
        m_sessionAddedIds = stillInDeck;
        m_progression.appendReview(restored);
    } else {
        m_progression.clear();
        m_sessionAddedIds.clear();
    }

    refreshDueCount();
    presentCard(notifyStats);
}

void StudySession::startFresh(const QString &deckName) {
    // A different Deck is a new sitting. Keep the Study mode, but drop the
    // previous sitting's Pending repeats, Leitner boxes and live counters.
    m_repeatTimer.stop();
    m_progression.clear();
    m_sessionRatedIds.clear();
    m_sessionAddedIds.clear();
    m_reviewed = 0;
    m_good = 0;
    rebuild(deckName);
    emit statsChanged();
}

void StudySession::updateDueCount() {
    // Pending Again repeats are not "due" in the database yet, but they are
    // still work waiting in this session.
    refreshDueCount();
}

void StudySession::setStatus(const QString &status) {
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

void StudySession::reportError(const QString &message) {
    setStatus(message);
    emit errorOccurred(message);
}

void StudySession::setMathColors(const QString &questionColor, const QString &answerColor) {
    if (m_questionInk == questionColor && m_answerInk == answerColor)
        return;
    m_questionInk = questionColor;
    m_answerInk = answerColor;
    m_math.clearPendingMath();
    const Card *card = m_decks.cardById(m_currentId);
    if (!card)
        return;
    m_question = m_math.renderWithPlaceholders(DeckIO::displayQuestion(*card), m_questionInk, MathScale::kQuestion);
    m_answer = m_math.renderWithPlaceholders(DeckIO::displayAnswer(*card), m_answerInk, MathScale::kAnswer);
    // Only the data URIs change on a theme switch; upcoming cards already have
    // their SVGs, so there is nothing to prefetch again.
    emit cardChanged();
}

void StudySession::applyMathRenderResult(const QString &key, const QString &html) {
    const QString marker = MathRenderer::mathPlaceholderMarker(key);
    // The marker is followed by the neutral span that reserves the slot. Both
    // must go, or the finished image keeps an empty box in front of it.
    const QRegularExpression placeholder(QRegularExpression::escape(marker) + QStringLiteral(".*?</span>"));
    bool changed = false;
    for (QString *field : {&m_question, &m_answer}) {
        if (!field->contains(placeholder))
            continue;
        field->replace(placeholder, html);
        changed = true;
    }
    // Results from cards already left behind have no marker in the live text.
    if (changed)
        emit cardChanged();
}

void StudySession::reveal() {
    if (!hasCard())
        return;
    m_showingAnswer = !m_showingAnswer;
    emit cardChanged();
}

void StudySession::showNext() {
    if (m_showingAnswer)
        reveal();
}

bool StudySession::canAddAdditionalNew() const {
    if (m_mode != StudyMode::Review)
        return false;
    const Deck *deck = m_decks.deckByName(m_deckName);
    if (!deck)
        return false;
    for (const Card &card : deck->cards) {
        if (card.id == m_currentId || m_progression.contains(card.id) || m_sessionRatedIds.contains(card.id))
            continue;
        if (m_planner.stateFor(card).reps == 0)
            return true;
    }
    return false;
}

bool StudySession::reviewWhenEmpty(int count) {
    if (m_mode != StudyMode::Review || hasCard())
        return false;
    // Small Pending-repeat seam: presentation already owns the deadline and
    // the waiting state. #182 can remove this branch without touching Add 5.
    if (m_waitingForRepeat) {
        m_progression.presentPendingNow(QDateTime::currentMSecsSinceEpoch());
        presentCard(false);
        return true;
    }
    return addAdditionalNew(count);
}

bool StudySession::addAdditionalNew(int count) {
    if (count <= 0 || m_mode != StudyMode::Review)
        return false;
    const Deck *deck = m_decks.deckByName(m_deckName);
    if (!deck)
        return false;

    QStringList added;
    for (const Card &card : deck->cards) {
        if (added.size() >= count)
            break;
        if (card.id == m_currentId || m_progression.contains(card.id) || m_sessionRatedIds.contains(card.id))
            continue;
        // A changed card whose current content has never been rated is New
        // again under the glossary definition, so stateFor() is the test.
        if (m_planner.stateFor(card).reps == 0)
            added << card.id;
    }
    if (added.isEmpty())
        return false;
    m_sessionAddedIds << added;

    // No Store write: these cards are queue content for this sitting only.
    m_progression.appendReview(added);
    presentCard(false);
    return true;
}

bool StudySession::rate(Rating rating, double desiredRetention) {
    // Backend parses the int boundary; keep the sentinel guard so a direct
    // caller cannot push an ungraded review into the scheduler.
    if (rating == Rating::Invalid || !hasCard())
        return false;
    // Queue content is session-only, but a rated card must never come back
    // through Add 5 even if its stored content has changed since the rating.
    m_sessionRatedIds.insert(m_currentId);
    ++m_reviewed;
    if (rating == Rating::Good)
        ++m_good;
    if (m_mode == StudyMode::Cram)
        answerCram(rating);
    else
        answerReview(rating, desiredRetention);
    presentCard(false);
    // One settled notification after presentation: subscribers see the next
    // Flashcard's Leitner box, not the rated one. Presentation itself emits
    // stats only for callers that do not rate a Card.
    emit statsChanged();
    return true;
}

void StudySession::answerReview(Rating rating, double desiredRetention) {
    Card *card = m_decks.cardById(m_currentId);
    if (!card)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const RatingRecording::Result result = m_ratings.record(*card, rating, desiredRetention, now);
    if (result.ok) {
        // The module writes straight to the Store; planning must re-read it so
        // counts, allowance and predicted recall see the new Scheduling state.
        QString storeError;
        if (!m_planner.refresh(&storeError))
            reportError(tr("Cannot read scheduling state: %1").arg(storeError));
    } else {
        // The raw Store error stays intact; each phase has its frozen text.
        const QString message = result.failurePhase == RatingRecording::FailurePhase::SchedulingRead
                                    ? tr("Cannot read scheduling state: %1").arg(result.storeError)
                                    : tr("Cannot save review: %1").arg(result.storeError);
        reportError(message);
    }

    m_progression.remove(card->id);
    if (rating == Rating::Again) {
        // Keep the FSRS due date above, but give the card one more appearance
        // in this sitting after a short break.
        m_progression.repeatAfter(card->id, now + m_repeatDelayMs);
    }
    refreshDueCount();
}

void StudySession::answerCram(Rating rating) {
    m_progression.advanceCram(m_currentId, rating);
}

void StudySession::startRepeatTimer(qint64 waitMs) {
    m_repeatTimer.stop();
    if (m_mode == StudyMode::Review && waitMs > 0)
        m_repeatTimer.start(waitMs);
}

void StudySession::presentCard(bool notifyStats) {
    m_showingAnswer = false;
    m_waitingForRepeat = false;
    const bool cram = m_mode == StudyMode::Cram;
    const InSessionProgression::NextResult next = m_progression.next(QDateTime::currentMSecsSinceEpoch());

    if (next.kind == InSessionProgression::Next::Finished) {
        m_currentId.clear();
        m_question.clear();
        m_answer.clear();
        m_repeatTimer.stop();
        setStatus(cram ? tr("No cards yet. Press N to add one.")
                       : tr("All caught up. Press C to cram or N to add a card."));
        emit cardChanged();
        return;
    }
    if (next.kind == InSessionProgression::Next::Waiting) {
        startRepeatTimer(next.waitMs);
        m_waitingForRepeat = true;
        m_currentId.clear();
        m_question.clear();
        m_answer.clear();
        setStatus(tr("Repeating a card in a moment…"));
        emit cardChanged();
        return;
    }

    m_currentId = next.cardId;
    startRepeatTimer(next.waitMs);

    const Card *card = m_decks.cardById(m_currentId);
    if (!card) {
        m_progression.remove(m_currentId);
        m_currentId.clear();
        presentCard(notifyStats);
        return;
    }
    m_question = m_math.renderWithPlaceholders(DeckIO::displayQuestion(*card), m_questionInk, MathScale::kQuestion);
    m_answer = m_math.renderWithPlaceholders(DeckIO::displayAnswer(*card), m_answerInk, MathScale::kAnswer);

    if (m_mode == StudyMode::Review) {
        const FsrsState state = m_planner.stateFor(*card);
        const double recall = state.reps > 0 ? Fsrs::retrievability(state, QDateTime::currentMSecsSinceEpoch()) : 1.0;
        setStatus(tr("Review · predicted recall %1%").arg(qRound(recall * 100)));
    } else {
        setStatus(tr("Cram · box %1 of %2 · next in %3 cards")
                      .arg(currentLeitnerBox())
                      .arg(Config::kLeitnerBoxes)
                      .arg(cramNextInCards()));
    }
    // Non-rating callers keep the notification at this point in the
    // card-changed and math-prefetch order.
    if (notifyStats)
        emit statsChanged();
    emit cardChanged();
    queueMathPrefetch();
}

void StudySession::queueMathPrefetch() {
    for (const QString &id : m_progression.upcoming(3)) {
        const Card *card = m_decks.cardById(id);
        if (!card)
            continue;
        m_math.prefetch(DeckIO::displayQuestion(*card), m_questionInk, MathScale::kQuestion);
        m_math.prefetch(DeckIO::displayAnswer(*card), m_answerInk, MathScale::kAnswer);
    }
}

void StudySession::refreshDueCount() {
    m_dueCount = m_planner.availableCount() + m_progression.pendingRepeatCount();
    emit dueCountChanged();
}

MathRenderer::Engine StudySession::mathEngine() const {
    return m_math.detect();
}

QString StudySession::statsText() const {
    const double accuracy = m_reviewed > 0 ? 100.0 * m_good / m_reviewed : 0.0;
    return tr("%1 reviewed · %2% Good").arg(m_reviewed).arg(qRound(accuracy));
}
