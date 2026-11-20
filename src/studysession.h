#pragma once

#include "config.h"
#include "deckrepository.h"
#include "domain.h"
#include "mathrender.h"
#include "studyplanner.h"
#include "studyqueue.h"

#include <QObject>
#include <QString>
#include <QTimer>

// Owns one study sitting: queue order, the Again timer, card presentation,
// ratings, status and the live session counters. Deck files and scheduling
// state stay in DeckRepository and StudyPlanner; Backend remains the QML/CLI
// facade and only forwards this object's signals.
class StudySession : public QObject {
    Q_OBJECT

public:
    explicit StudySession(DeckRepository &decks, StudyPlanner &planner, QObject *parent = nullptr);

    void setMode(const QString &mode);
    void rebuild(const QString &deckName);
    void updateDueCount();
    void setStatus(const QString &status);
    void setMathColors(const QString &questionColor, const QString &answerColor);

    void flip();
    bool rate(int rating, double desiredRetention);
    void showNext();

    QString mode() const { return m_mode; }
    QString question() const { return m_question; }
    QString answer() const { return m_answer; }
    bool showingAnswer() const { return m_showingAnswer; }
    bool hasCard() const { return !m_currentId.isEmpty(); }
    bool waitingForRepeat() const { return m_waitingForRepeat; }
    int dueCount() const { return m_dueCount; }
    int reviewed() const { return m_reviewed; }
    int correct() const { return m_correct; }
    int box() const { return m_box; }
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
    void scheduleNextRepeat();
    void presentCard();
    void answerCram(Rating rating);
    void answerReview(Rating rating, double desiredRetention);
    void refreshDueCount();

    DeckRepository &m_decks;
    StudyPlanner &m_planner;
    StudyQueue m_queue;
    QTimer m_repeatTimer;
    MathRenderer m_math;
    qint64 m_repeatDelayMs;
    int m_dueCount = 0;
    int m_reviewed = 0;
    int m_correct = 0;
    int m_box = 0;
    QString m_mode = "review";
    QString m_question;
    QString m_answer;
    QString m_questionInk = "#000000";
    QString m_answerInk = "#000000";
    QString m_status;
    QString m_currentId;
    bool m_showingAnswer = false;
    bool m_waitingForRepeat = false;
};
