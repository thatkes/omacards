#include "studysession.h"

#include <QDateTime>
#include <QRandomGenerator>

#include <algorithm>

namespace {
// TeX typesets at 12pt (~16px at 96dpi) while the QML labels use 30px/24px.
// Scaling the SVG keeps inline math legible next to the surrounding text.
constexpr double kQuestionMathScale = 2.0;
constexpr double kAnswerMathScale = 1.6;
} // namespace

StudySession::StudySession(DeckRepository &decks, StudyPlanner &planner, RatingRecording &ratings, QObject *parent)
    : QObject(parent), m_decks(decks), m_planner(planner), m_ratings(ratings), m_repeatDelayMs(Config::againDelayMs()) {
    m_repeatTimer.setSingleShot(true);
    connect(&m_repeatTimer, &QTimer::timeout, this, &StudySession::presentCard);
}

void StudySession::setMode(const QString &mode) {
    if (mode == Domain::toString(m_mode))
        return;
    m_mode = Domain::modeFromString(mode);
    m_reviewed = 0;
    m_good = 0;
    emit modeChanged();
    emit statsChanged();
}

void StudySession::rebuild(const QString &deckName) {
    // A watcher refresh can rebuild the queue while a repeat is pending. Keep
    // those deadlines for cards that are still available; a cram queue is
    // rebuilt from scratch and clears them instead.
    const QHash<QString, qint64> pending = m_progression.pendingRepeats();
    m_repeatTimer.stop();
    m_currentId.clear();

    const Deck *deck = m_decks.deckByName(deckName);
    if (deck && m_mode == StudyMode::Cram) {
        // A rebuilt Cram session gets a fresh shuffle; the module only orders
        // the Cards it is given.
        QVector<Card> ordered = deck->cards;
        std::shuffle(ordered.begin(), ordered.end(), *QRandomGenerator::global());
        m_progression.buildCram(ordered);
    } else if (deck) {
        QStringList dueIds;
        QStringList newIds;
        m_planner.reviewIds(deckName, &dueIds, &newIds);
        m_progression.buildReview(dueIds, newIds, pending);
    } else {
        m_progression.clear();
    }

    refreshDueCount();
    presentCard();
}

void StudySession::startFresh(const QString &deckName) {
    // A different Deck is a new sitting. Keep the Study mode, but drop the
    // previous sitting's Pending repeats, Leitner boxes and live counters.
    m_repeatTimer.stop();
    m_progression.clear();
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
    const Card *card = m_decks.cardById(m_currentId);
    if (!card)
        return;
    m_question = m_math.render(DeckIO::displayQuestion(*card), m_questionInk, kQuestionMathScale);
    m_answer = m_math.render(DeckIO::displayAnswer(*card), m_answerInk, kAnswerMathScale);
    emit cardChanged();
}

void StudySession::flip() {
    if (!hasCard())
        return;
    m_showingAnswer = !m_showingAnswer;
    emit cardChanged();
}

void StudySession::showNext() {
    if (m_showingAnswer)
        flip();
}

bool StudySession::rate(Rating rating, double desiredRetention) {
    // Backend parses the int boundary; keep the sentinel guard so a direct
    // caller cannot push an ungraded review into the scheduler.
    if (rating == Rating::Invalid || !hasCard())
        return false;
    ++m_reviewed;
    if (rating == Rating::Good)
        ++m_good;
    if (m_mode == StudyMode::Cram)
        answerCram(rating);
    else
        answerReview(rating, desiredRetention);
    emit statsChanged();
    presentCard();
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

void StudySession::presentCard() {
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
        presentCard();
        return;
    }
    m_question = m_math.render(DeckIO::displayQuestion(*card), m_questionInk, kQuestionMathScale);
    m_answer = m_math.render(DeckIO::displayAnswer(*card), m_answerInk, kAnswerMathScale);

    if (m_mode == StudyMode::Review) {
        const FsrsState state = m_planner.stateFor(*card);
        const double recall = state.reps > 0 ? Fsrs::retrievability(state, QDateTime::currentMSecsSinceEpoch()) : 1.0;
        setStatus(tr("Review · predicted recall %1%").arg(qRound(recall * 100)));
    } else {
        setStatus(tr("Cram · box %1 of %2 · next in %3 cards")
                      .arg(currentBox())
                      .arg(Config::kLeitnerBoxes)
                      .arg(Config::kLeitnerIntervals[currentBox() - 1]));
    }
    emit statsChanged();
    emit cardChanged();
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
