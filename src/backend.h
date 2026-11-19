#pragma once

#include "deck.h"
#include "fsrs.h"
#include "mathrender.h"
#include "store.h"

#include <QByteArray>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVector>

struct CramEntry {
    QString cardId;
    int box = 0; // 0..4, intervals of 1,2,4,8,16 cards
    int next = 0;
};

struct CardDraft {
    QString question;
    QString answer;
    QString type = "basic"; // basic | cloze
    QStringList tags;
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
    Q_PROPERTY(bool waitingForRepeat READ waitingForRepeat NOTIFY cardChanged)
    Q_PROPERTY(int dueCount READ dueCount NOTIFY dueCountChanged)
    Q_PROPERTY(int newCount READ newCount NOTIFY dueCountChanged)
    Q_PROPERTY(int deckDue READ deckDue NOTIFY dueCountChanged)
    Q_PROPERTY(int deckNew READ deckNew NOTIFY dueCountChanged)
    Q_PROPERTY(int streak READ streak NOTIFY dueCountChanged)
    Q_PROPERTY(QVariantList deckList READ deckSummaries NOTIFY decksChanged)
    Q_PROPERTY(int reviewed READ reviewed NOTIFY statsChanged)
    Q_PROPERTY(int correct READ correct NOTIFY statsChanged)
    Q_PROPERTY(int box READ box NOTIFY statsChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString mathEngine READ mathEngine CONSTANT)
    Q_PROPERTY(int totalCards READ totalCards NOTIFY decksChanged)
    Q_PROPERTY(QVariantList cardList READ cardList NOTIFY cardsChanged)
    Q_PROPERTY(QString cardFilter READ cardFilter WRITE setCardFilter NOTIFY cardFilterChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString currentCardId READ currentCardId NOTIFY cardChanged)
    Q_PROPERTY(int newPerDay READ newPerDay WRITE setNewPerDay NOTIFY settingsChanged)
    Q_PROPERTY(double desiredRetention READ desiredRetention WRITE setDesiredRetention NOTIFY settingsChanged)
    Q_PROPERTY(QString decksDirectory READ decksDirectory CONSTANT)

public:
    explicit Backend(const QString &decksDirectory = {}, const QString &databasePath = {},
                     QObject *parent = nullptr);

    bool initialize(QString *error = nullptr);

    // Keeps equations in the same ink as the surrounding QML labels.
    void setMathColors(const QString &questionColor, const QString &answerColor);

    QStringList decks() const { return m_deckNames; }
    QString currentDeck() const { return m_currentDeck; }
    QString mode() const { return m_mode; }
    QString question() const { return m_question; }
    QString answer() const { return m_answer; }
    bool showingAnswer() const { return m_showingAnswer; }
    bool hasCard() const { return !m_currentId.isEmpty(); }
    bool waitingForRepeat() const { return m_waitingForRepeat; }
    int dueCount() const { return m_dueCount; }
    int newCount() const;
    int deckDue() const;
    int deckNew() const;
    int streak() const { return m_store.summary().streakDays; }
    int reviewed() const { return m_reviewed; }
    int correct() const { return m_correct; }
    int box() const { return m_box; }
    QString status() const { return m_status; }
    QString mathEngine() const;
    int totalCards() const;
    QString decksDirectory() const { return m_decksDir; }
    QString databasePath() const { return m_store.path(); }
    QString cardFilter() const { return m_cardFilter; }
    QString currentCardId() const { return m_currentId; }
    bool canUndo() const { return !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }
    int newPerDay() const { return m_newPerDay; }
    double desiredRetention() const { return m_desiredRetention; }
    int availableCount(const QString &deckName = {}) const;
    QVariantList deckSummaries() const;
    QVariantList cardList() const;
    ReviewSummary reviewSummary() const { return m_store.summary(); }

    QVector<Deck> allDecks() const { return m_decks; }
    int countDue(const QString &deckName = {}) const;
    QVector<Card> dueCards(const QString &deckName = {}) const;
    QVector<Card> searchCards(const QString &text, const QString &deckName = {}) const;

    // Headless helpers used by the agent CLI.
    bool createDeck(const QString &name, QString *error = nullptr, const QString &description = {});
    bool setDeckDescription(const QString &deckName, const QString &description, QString *error = nullptr);
    bool createCards(const QString &deckName, const QVector<CardDraft> &drafts,
                     const QString *description, QStringList *createdIds, QString *error = nullptr);
    bool createCard(const QString &deckName, const QString &question, const QString &answer,
                    const QString &type = "basic", const QString &tags = {}, QString *error = nullptr);
    bool modifyCard(const QString &deckName, const QString &cardId, const QString &question,
                    const QString &answer, const QString &type, const QString &tags,
                    QString *error = nullptr);
    bool deleteCard(const QString &deckName, const QString &cardId, QString *error = nullptr);
    bool removeDeck(const QString &name, QString *error = nullptr);
    bool importFile(const QString &path, const QString &deckName, QString *error = nullptr);
    bool importCsv(const QString &path, const QString &deckName, QString *error = nullptr);
    bool importApkg(const QString &path, const QString &deckName, QString *error = nullptr);

public slots:
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void selectDeck(const QString &name);
    Q_INVOKABLE void setMode(const QString &mode);
    Q_INVOKABLE void flip();
    // Named `rate` rather than `answer` because `answer` is also the Q_PROPERTY
    // holding the revealed answer text; QML resolves the property first.
    Q_INVOKABLE void rate(int rating);
    Q_INVOKABLE void showNext();
    Q_INVOKABLE void newDeck(const QString &name);
    Q_INVOKABLE void addCard(const QString &question, const QString &answer);
    Q_INVOKABLE void addCardDetails(const QString &question, const QString &answer,
                                    const QString &type, const QString &tags);
    Q_INVOKABLE void removeCurrentCard();
    Q_INVOKABLE void removeCard(const QString &cardId);
    Q_INVOKABLE void deleteDeck(const QString &name);
    Q_INVOKABLE void deleteCurrentDeck();
    Q_INVOKABLE void updateCard(const QString &cardId, const QString &question, const QString &answer,
                                const QString &type, const QString &tags);
    Q_INVOKABLE void importFrom(const QString &path, const QString &deckName = {});
    Q_INVOKABLE void setCardFilter(const QString &filter);
    Q_INVOKABLE void setNewPerDay(int count);
    Q_INVOKABLE void setDesiredRetention(double retention);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void editDeckFile();
    Q_INVOKABLE void requestShow();
    Q_INVOKABLE QString statsText() const;
    Q_INVOKABLE QVariantMap statsSummary() const;

signals:
    void decksChanged();
    void currentDeckChanged();
    void modeChanged();
    void cardChanged();
    void dueCountChanged();
    void statsChanged();
    void statusChanged();
    void cardsChanged();
    void cardFilterChanged();
    void historyChanged();
    void settingsChanged();
    void showWindowRequested();
    void errorOccurred(const QString &message);

private:
    struct FileSnapshot {
        QString path;
        QByteArray before;
        QByteArray after;
        bool existedBefore = false;
        bool existsAfter = true;
        QString description;
    };

    // A single consistent read of the scheduling sidecar. Every availability
    // query and every card view uses this snapshot instead of re-reading the
    // database, so counts cannot disagree mid-action.
    struct StudySnapshot {
        QHash<QString, FsrsState> states;
        QHash<QString, QString> hashes;
        int newBudget = 0;
        qint64 now = 0;
    };

    struct Availability {
        QVector<Card> due;           // ordered by due date, then id
        QVector<Card> fresh;         // deck-file order, up to the daily cap
        QVector<int> duePerDeck;
        QVector<int> freshPerDeck;
        QVector<int> dueIncludingNewPerDeck; // totalDue in CLI stats includes new cards
    };

    void load();
    bool refreshSnapshot();
    FsrsState effectiveState(const Card &card) const;
    Availability availability(const QString &deckName = {}) const;
    void scheduleNextRepeat();
    void rebuildQueue();
    void refreshDueCount();
    void presentCard();
    void setStatus(const QString &status);
    void reportError(const QString &message);
    Deck *deckByName(const QString &name);
    const Deck *deckByName(const QString &name) const;
    int deckIndex(const QString &name) const;
    Card *cardById(const QString &id);
    void answerCram(int rating);
    void answerReview(int rating);
    void recordHistory(const QString &path, const QByteArray &before, bool existedBefore,
                       const QString &description);
    bool applySnapshot(const FileSnapshot &snapshot, bool restoreBefore, QString *error);
    static QByteArray readFileBytes(const QString &path);
    static QString safeFileName(const QString &name);
    static QStringList splitDelimited(const QString &line, QChar delimiter);
    static QString stripHtml(const QString &html);
    static QString decodeEntities(const QString &text);
    static QString rewriteAnkiMedia(const QString &html, const QHash<QString, QString> &media);
    static QHash<QString, QString> apkgMedia(const QString &path, const QString &directory,
                                              const QString &unzip, QString *error);

    QString m_decksDir;
    QString m_currentDeck;
    QString m_mode = "review";
    QString m_question;
    QString m_answer;
    QString m_questionInk = "#000000";
    QString m_answerInk = "#000000";
    QString m_status;
    QString m_currentId;
    QString m_cardFilter;
    QStringList m_deckNames;
    QVector<Deck> m_decks;
    QVector<CramEntry> m_cramQueue;
    QStringList m_reviewQueue;
    QHash<QString, qint64> m_reviewAgainNotBefore;
    QTimer m_reviewAgainTimer;
    QVector<FileSnapshot> m_undoStack;
    QVector<FileSnapshot> m_redoStack;
    Store m_store;
    MathRenderer m_math;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
    StudySnapshot m_snapshot;
    bool m_snapshotValid = false;
    qint64 m_reviewAgainDelayMs = 60000;
    int m_dueCount = 0;
    int m_reviewed = 0;
    int m_correct = 0;
    int m_box = 0;
    int m_cramReviews = 0;
    int m_newPerDay = 20;
    double m_desiredRetention = 0.9;
    bool m_showingAnswer = false;
    bool m_waitingForRepeat = false;
};
