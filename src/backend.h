#pragma once

#include "deck.h"
#include "fsrs.h"
#include "mathrender.h"
#include "store.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVector>

struct CramEntry {
    QString cardId;
    int box = 0; // 0..4, intervals of 1,2,4,8,16 cards
    int next = 0;
};

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList decks READ decks NOTIFY decksChanged)
    Q_PROPERTY(QString currentDeck READ currentDeck WRITE selectDeck NOTIFY currentDeckChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY modeChanged)
    Q_PROPERTY(QString question READ question NOTIFY cardChanged)
    Q_PROPERTY(QString answer READ answer NOTIFY cardChanged)
    Q_PROPERTY(bool showingAnswer READ showingAnswer NOTIFY cardChanged)
    Q_PROPERTY(bool hasCard READ hasCard NOTIFY cardChanged)
    Q_PROPERTY(int dueCount READ dueCount NOTIFY dueCountChanged)
    Q_PROPERTY(int reviewed READ reviewed NOTIFY statsChanged)
    Q_PROPERTY(int correct READ correct NOTIFY statsChanged)
    Q_PROPERTY(int box READ box NOTIFY statsChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString mathEngine READ mathEngine CONSTANT)
    Q_PROPERTY(int totalCards READ totalCards NOTIFY decksChanged)

public:
    explicit Backend(const QString &decksDirectory = {}, const QString &databasePath = {},
                     QObject *parent = nullptr);

    bool initialize(QString *error = nullptr);

    QStringList decks() const { return m_deckNames; }
    QString currentDeck() const { return m_currentDeck; }
    QString mode() const { return m_mode; }
    QString question() const { return m_question; }
    QString answer() const { return m_answer; }
    bool showingAnswer() const { return m_showingAnswer; }
    bool hasCard() const { return !m_currentId.isEmpty(); }
    int dueCount() const { return m_dueCount; }
    int reviewed() const { return m_reviewed; }
    int correct() const { return m_correct; }
    int box() const { return m_box; }
    QString status() const { return m_status; }
    QString mathEngine() const;
    int totalCards() const;
    QString decksDirectory() const { return m_decksDir; }
    QString databasePath() const { return m_store.path(); }

    QVector<Deck> allDecks() const { return m_decks; }
    int countDue(const QString &deckName = {}) const;

    // Headless helpers used by the agent CLI.
    bool createDeck(const QString &name, QString *error = nullptr);
    bool createCard(const QString &deckName, const QString &question, const QString &answer,
                    const QString &type = "basic", const QString &tags = {}, QString *error = nullptr);
    bool deleteCard(const QString &deckName, const QString &cardId, QString *error = nullptr);
    bool importFile(const QString &path, const QString &deckName, QString *error = nullptr);
    bool importCsv(const QString &path, const QString &deckName, QString *error = nullptr);
    bool importApkg(const QString &path, const QString &deckName, QString *error = nullptr);

public slots:
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void selectDeck(const QString &name);
    Q_INVOKABLE void setMode(const QString &mode);
    Q_INVOKABLE void flip();
    Q_INVOKABLE void answer(int rating);
    Q_INVOKABLE void showNext();
    Q_INVOKABLE void newDeck(const QString &name);
    Q_INVOKABLE void addCard(const QString &question, const QString &answer);
    Q_INVOKABLE void removeCurrentCard();
    Q_INVOKABLE void editDeckFile();
    Q_INVOKABLE void requestShow();
    Q_INVOKABLE QString statsText() const;

signals:
    void decksChanged();
    void currentDeckChanged();
    void modeChanged();
    void cardChanged();
    void dueCountChanged();
    void statsChanged();
    void statusChanged();
    void showWindowRequested();
    void errorOccurred(const QString &message);

private:
    void load();
    void rebuildQueue();
    void presentCard();
    void setStatus(const QString &status);
    void reportError(const QString &message);
    Deck *deckByName(const QString &name);
    const Deck *deckByName(const QString &name) const;
    Card *cardById(const QString &id);
    void answerCram(int rating);
    void answerReview(int rating);
    static QString safeFileName(const QString &name);
    static QStringList splitDelimited(const QString &line, QChar delimiter);
    static QString stripHtml(const QString &html);

    QString m_decksDir;
    QString m_currentDeck;
    QString m_mode = "review";
    QString m_question;
    QString m_answer;
    QString m_status;
    QString m_currentId;
    QStringList m_deckNames;
    QVector<Deck> m_decks;
    QVector<CramEntry> m_cramQueue;
    QStringList m_reviewQueue;
    Store m_store;
    MathRenderer m_math;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
    int m_dueCount = 0;
    int m_reviewed = 0;
    int m_correct = 0;
    int m_box = 0;
    int m_cramReviews = 0;
    bool m_showingAnswer = false;
};
