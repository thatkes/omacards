#pragma once

#include "config.h"
#include "deckrepository.h"
#include "domain.h"
#include "mathrender.h"
#include "insessionprogression.h"
#include "ratingrecording.h"
#include "studyplanner.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>

// Owns one study sitting: card presentation, ratings, status and the live
// counters. InSessionProgression owns card order, Cram Leitner boxes and
// Pending repeat deadlines; the Study session supplies the Cram order and Ratings.
// Deck files stay in DeckRepository, Rating recording writes Scheduling state
// and Review history, and StudyPlanner reads them for planning. Backend remains
// the QML facade and forwards this object's signals.
class StudySession : public QObject {
    Q_OBJECT

public:
    // Supplies the order of Cards handed to the Cram queue. Production shuffles
    // on every rebuild; tests inject an identity order to assert the exact
    // Flashcard that follows a rating.
    using CramOrder = std::function<QVector<Card>(const QVector<Card> &)>;

    explicit StudySession(DeckRepository &decks, StudyPlanner &planner, RatingRecording &ratings,
                          CramOrder cramOrder = {}, QObject *parent = nullptr);

    // One operation: stop the repeat timer, clear the old mode's progression and
    // counters, rebuild this deck's queue and present its first Flashcard.
    void setMode(const QString &mode);
    void rebuild(const QString &deckName);
    // Rebuild for a freshly chosen deck: no pending repeats, Leitner boxes or
    // session counters from the previous deck survive the switch.
    void startFresh(const QString &deckName);
    void updateDueCount();
    void setStatus(const QString &status);
    void reportError(const QString &message);
    void setMathColors(const QString &questionColor, const QString &answerColor);
    void applyMathRenderResult(const QString &key, const QString &html);

    void reveal();
    bool rate(Rating rating, double desiredRetention);
    void showNext();
    bool canAddAdditionalNew() const;
    // One session-only Review action: with no ready card it either presents a
    // waiting Pending repeat or adds up to count Additional New cards.
    bool reviewWhenEmpty(int count);

    StudyMode mode() const { return m_mode; }
    QString question() const { return m_question; }
    QString answer() const { return m_answer; }
    bool showingAnswer() const { return m_showingAnswer; }
    bool hasCard() const { return !m_currentId.isEmpty(); }
    bool waitingForRepeat() const { return m_waitingForRepeat; }
    int dueCount() const { return m_dueCount; }
    int reviewed() const { return m_reviewed; }
    int good() const { return m_good; }
    int currentLeitnerBox() const { return m_progression.currentLeitnerBox(); }
    QVector<int> boxDistribution() const { return m_progression.boxDistribution(); }
    int cramNextInCards() const {
        const int leitnerBox = currentLeitnerBox();
        return leitnerBox >= 1 && leitnerBox <= Config::kLeitnerBoxes ? Config::kLeitnerIntervals[leitnerBox - 1] : 0;
    }
    QString status() const { return m_status; }
    QString currentCardId() const { return m_currentId; }

    MathRenderer::Engine mathEngine() const;
    QString statsText() const;

signals:
    void modeChanged();
    void cardChanged();
    void dueCountChanged();
    void statsChanged();
    void statusChanged();
    void errorOccurred(const QString &message);

private:
    void rebuildQueue(const QString &deckName, bool notifyStats);
    void startRepeatTimer(qint64 waitMs);
    // Ratings own their single settled notification; other callers keep the
    // notification presentation has always emitted.
    void presentCard(bool notifyStats = true);
    void answerCram(Rating rating);
    void answerReview(Rating rating, double desiredRetention);
    bool addAdditionalNew(int count);
    void refreshDueCount();
    void queueMathPrefetch();

    DeckRepository &m_decks;
    StudyPlanner &m_planner;
    RatingRecording &m_ratings;
    InSessionProgression m_progression;
    CramOrder m_cramOrder;
    QTimer m_repeatTimer;
    MathRenderer m_math;
    qint64 m_repeatDelayMs;
    int m_dueCount = 0;
    int m_reviewed = 0;
    int m_good = 0;
    StudyMode m_mode = StudyMode::Review;
    QString m_question;
    QString m_answer;
    QString m_questionInk = "#000000";
    QString m_answerInk = "#000000";
    QString m_status;
    QString m_currentId;
    QString m_deckName;
    QSet<QString> m_sessionRatedIds;
    bool m_showingAnswer = false;
    bool m_waitingForRepeat = false;
};
