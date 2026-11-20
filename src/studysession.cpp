#include "studysession.h"

#include <QDateTime>

namespace {
// TeX typesets at 12pt (~16px at 96dpi) while the QML labels use 30px/24px.
// Scaling the SVG keeps inline math legible next to the surrounding text.
constexpr double kQuestionMathScale = 2.0;
constexpr double kAnswerMathScale = 1.6;
} // namespace

StudySession::StudySession(DeckRepository &decks, StudyPlanner &planner, QObject *parent)
    : QObject(parent), m_decks(decks), m_planner(planner), m_repeatDelayMs(Config::againDelayMs()) {
    m_repeatTimer.setSingleShot(true);
    connect(&m_repeatTimer, &QTimer::timeout, this, &StudySession::presentCard);
}

void StudySession::setMode(const QString &mode) {
    if (mode == m_mode)
        return;
    m_mode = Domain::toString(Domain::modeFromString(mode));
    m_reviewed = 0;
    m_correct = 0;
    emit modeChanged();
    emit statsChanged();
}

void StudySession::rebuild(const QString &deckName) {
    // A watcher refresh can rebuild the queue while a repeat is pending. Keep
    // those deadlines for cards that are still available; a cram queue is
    // rebuilt from scratch and clears them instead.
    const QHash<QString, qint64> pending = m_queue.pendingRepeats();
    m_repeatTimer.stop();
    m_currentId.clear();

    const Deck *deck = m_decks.deckByName(deckName);
    if (deck && Domain::modeFromString(m_mode) == StudyMode::Cram) {
        m_queue.buildCram(deck->cards);
    } else if (deck) {
        QStringList dueIds;
        QStringList freshIds;
        m_planner.reviewIds(deckName, &dueIds, &freshIds);
        m_queue.buildReview(dueIds, freshIds, pending);
    } else {
        m_queue.clear();
    }

    refreshDueCount();
    presentCard();
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

bool StudySession::rate(int rating, double desiredRetention) {
    if (!hasCard())
        return false;
    const Rating parsed = Domain::ratingFromInt(rating);
    if (parsed == Rating::Invalid)
        return false;
    ++m_reviewed;
    if (parsed == Rating::Good)
        ++m_correct;
    if (Domain::modeFromString(m_mode) == StudyMode::Cram)
        answerCram(parsed);
    else
        answerReview(parsed, desiredRetention);
    emit statsChanged();
    presentCard();
    return true;
}

void StudySession::answerReview(Rating rating, double desiredRetention) {
    Card *card = m_decks.cardById(m_currentId);
    if (!card)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString storeError;
    if (!m_planner.recordReview(*card, rating, desiredRetention, now, &storeError))
        emit errorOccurred(tr("Cannot save review: %1").arg(storeError));

    m_queue.remove(card->id);
    if (rating == Rating::Again) {
        // Keep the FSRS due date above, but give the card one more appearance
        // in this sitting after a short break.
        m_queue.repeatAfter(card->id, now + m_repeatDelayMs);
    }
    refreshDueCount();
}

void StudySession::answerCram(Rating rating) {
    m_queue.advanceCram(m_currentId, rating, &m_box);
}

void StudySession::scheduleNextRepeat() {
    m_repeatTimer.stop();
    if (Domain::modeFromString(m_mode) != StudyMode::Review)
        return;
    const qint64 wait = m_queue.nextWait(QDateTime::currentMSecsSinceEpoch());
    if (wait > 0)
        m_repeatTimer.start(wait);
}

void StudySession::presentCard() {
    m_showingAnswer = false;
    m_waitingForRepeat = false;
    const bool cram = Domain::modeFromString(m_mode) == StudyMode::Cram;
    QString nextId;
    int nextBox = 0;
    const StudyQueue::Next next = m_queue.next(QDateTime::currentMSecsSinceEpoch(), &nextId, nullptr, &nextBox);

    if (next == StudyQueue::Next::Finished) {
        m_currentId.clear();
        m_question.clear();
        m_answer.clear();
        m_repeatTimer.stop();
        setStatus(cram ? tr("No cards yet. Press N to add one.")
                       : tr("All caught up. Press C to cram or N to add a card."));
        emit cardChanged();
        return;
    }
    if (next == StudyQueue::Next::Waiting) {
        scheduleNextRepeat();
        m_waitingForRepeat = true;
        m_currentId.clear();
        m_question.clear();
        m_answer.clear();
        setStatus(tr("Repeating a card in a moment…"));
        emit cardChanged();
        return;
    }

    m_currentId = nextId;
    if (cram)
        m_box = nextBox;
    scheduleNextRepeat();

    const Card *card = m_decks.cardById(m_currentId);
    if (!card) {
        m_queue.remove(m_currentId);
        m_currentId.clear();
        presentCard();
        return;
    }
    m_question = m_math.render(DeckIO::displayQuestion(*card), m_questionInk, kQuestionMathScale);
    m_answer = m_math.render(DeckIO::displayAnswer(*card), m_answerInk, kAnswerMathScale);

    if (Domain::modeFromString(m_mode) == StudyMode::Review) {
        const FsrsState state = m_planner.stateFor(*card);
        const double recall = state.reps > 0 ? Fsrs::retrievability(state, QDateTime::currentMSecsSinceEpoch()) : 1.0;
        setStatus(
            tr("Review · predicted recall %1% · FSRS box %2").arg(qRound(recall * 100)).arg(qMax(1, m_reviewed + 1)));
    } else {
        setStatus(tr("Cram · box %1 of %2 · next in %3 cards")
                      .arg(m_box)
                      .arg(Config::kLeitnerBoxes)
                      .arg(Config::kLeitnerIntervals[m_box - 1]));
    }
    emit statsChanged();
    emit cardChanged();
}

void StudySession::refreshDueCount() {
    m_dueCount = m_planner.availableCount() + m_queue.pendingRepeatCount();
    emit dueCountChanged();
}

MathRenderer::Engine StudySession::mathEngine() const {
    return m_math.detect();
}

QString StudySession::statsText() const {
    const double accuracy = m_reviewed > 0 ? 100.0 * m_correct / m_reviewed : 0.0;
    return tr("%1 reviewed · %2% correct · box %3").arg(m_reviewed).arg(qRound(accuracy)).arg(m_box);
}
