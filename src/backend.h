#pragma once

#include "config.h"
#include "deck.h"
#include "deckrepository.h"
#include "domain.h"
#include "filehistory.h"
#include "ratingrecording.h"
#include "store.h"
#include "studyplanner.h"
#include "studysession.h"

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <memory>

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
    Q_PROPERTY(QVariantList deckSelector READ deckSelector NOTIFY decksChanged)
    Q_PROPERTY(int reviewed READ reviewed NOTIFY statsChanged)
    Q_PROPERTY(int good READ good NOTIFY statsChanged)
    Q_PROPERTY(int currentBox READ currentBox NOTIFY statsChanged)
    Q_PROPERTY(QVariantList boxCounts READ boxCounts NOTIFY statsChanged)
    Q_PROPERTY(int cramNextInCards READ cramNextInCards NOTIFY statsChanged)
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
    Q_PROPERTY(int dayStartHour READ dayStartHour WRITE setDayStartHour NOTIFY settingsChanged)
    Q_PROPERTY(QString decksDirectory READ decksDirectory CONSTANT)

public:
    // Paths are required so callers cannot accidentally fall back to implicit
    // environment-driven locations. Use fromEnvironment() for that case.
    explicit Backend(const QString &decksDirectory, const QString &databasePath, QObject *parent = nullptr);
    static std::unique_ptr<Backend> fromEnvironment(QObject *parent = nullptr);

    bool initialize(QString *error = nullptr);

    // Keeps equations in the same ink as the surrounding QML labels.
    void setMathColors(const QString &questionColor, const QString &answerColor) {
        m_session.setMathColors(questionColor, answerColor);
    }

    QStringList decks() const { return m_deckRepo.deckNames(); }
    QString currentDeck() const { return m_currentDeck; }
    QString mode() const { return Domain::toString(m_mode); }
    QString question() const { return m_session.question(); }
    QString answer() const { return m_session.answer(); }
    bool showingAnswer() const { return m_session.showingAnswer(); }
    bool hasCard() const { return m_session.hasCard(); }
    bool waitingForRepeat() const { return m_session.waitingForRepeat(); }
    int dueCount() const { return m_session.dueCount(); }
    int newCount() const { return m_planner.newCount(); }
    int deckDue() const { return m_planner.deckDue(m_deckRepo.deckIndex(m_currentDeck)); }
    int deckNew() const { return m_planner.deckNew(m_deckRepo.deckIndex(m_currentDeck)); }
    int streak() const { return m_store.summary(m_dayStartHour).streak; }
    int reviewed() const { return m_session.reviewed(); }
    int good() const { return m_session.good(); }
    int currentBox() const { return m_session.currentBox(); }
    QVariantList boxCounts() const {
        QVariantList counts;
        for (int count : m_session.boxCounts())
            counts << count;
        return counts;
    }
    int cramNextInCards() const { return m_session.cramNextInCards(); }
    QString status() const { return m_session.status(); }
    QString mathEngine() const;
    int totalCards() const { return m_deckRepo.totalCards(); }
    QString decksDirectory() const { return m_deckRepo.decksDirectory(); }
    QString databasePath() const { return m_store.path(); }
    QString cardFilter() const { return m_cardFilter; }
    QString currentCardId() const { return m_session.currentCardId(); }
    bool canUndo() const { return m_history.canUndo(); }
    bool canRedo() const { return m_history.canRedo(); }
    int newPerDay() const { return m_newPerDay; }
    double desiredRetention() const { return m_desiredRetention; }
    int dayStartHour() const { return m_dayStartHour; }
    int availableCount(const QString &deckName = {}) const { return m_planner.availableCount(deckName); }
    QVariantList deckSummaries() const { return m_planner.deckSummaries(m_currentDeck); }
    // Same summaries as deckSummaries(), ordered for the deck selector:
    // Current deck first, then last-studied order, then file order.
    QVariantList deckSelector() const;
    QVariantList cardList() const { return m_planner.cardList(m_currentDeck, m_cardFilter, tr("new"), tr("due now")); }
    ReviewSummary reviewSummary() const { return m_store.summary(m_dayStartHour); }

    QVector<Deck> allDecks() const { return m_deckRepo.decks(); }
    int countDue(const QString &deckName = {}) const { return m_planner.countDue(deckName); }
    QVector<Card> dueCards(const QString &deckName = {}) const { return m_planner.dueCards(deckName); }
    QVector<Card> searchCards(const QString &text, const QString &deckName = {}) const {
        return m_deckRepo.searchCards(text, deckName);
    }

    // Headless helpers used by the agent CLI.
    bool createDeck(const QString &name, QString *error = nullptr, const QString &description = {});
    bool setDeckDescription(const QString &deckName, const QString &description, QString *error = nullptr);
    bool createCards(const QString &deckName, const QVector<CardDraft> &drafts, const QString *description,
                     QStringList *createdIds, QString *error = nullptr);
    bool createCard(const QString &deckName, const QString &question, const QString &answer,
                    const QString &type = "basic", const QString &tags = {}, QString *error = nullptr);
    bool modifyCard(const QString &deckName, const QString &cardId, const QString &question, const QString &answer,
                    const QString &type, const QString &tags, QString *error = nullptr, QString *finalId = nullptr);
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
    Q_INVOKABLE void addCardDetails(const QString &question, const QString &answer, const QString &type,
                                    const QString &tags);
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
    Q_INVOKABLE void setDayStartHour(int hour);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void editDeckFile();
    Q_INVOKABLE void requestShow();
    Q_INVOKABLE QString statsText() const;
    Q_INVOKABLE QVariantMap statsSummary() const {
        return m_planner.statsSummary({m_store.summary(m_dayStartHour), dueCount(), totalCards(),
                                       static_cast<int>(m_deckRepo.deckNames().size()), reviewed(), good(),
                                       m_desiredRetention});
    }

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
    void recordDeckSelection(const QString &name);

    Store m_store;
    FileHistory m_history;
    StoreRatingAdapter m_ratingStore;
    RatingRecording m_ratings;
    DeckRepository m_deckRepo;
    StorePlannerAdapter m_plannerStore;
    StudyPlanner m_planner;
    StudySession m_session;
    int m_newPerDay = Config::kDefaultNewPerDay;
    double m_desiredRetention = Config::kDefaultRetention;
    int m_dayStartHour = Config::kDefaultDayStartHour;
    QString m_currentDeck;
    QString m_persistedDeck;
    QHash<QString, qint64> m_lastStudied;
    StudyMode m_mode = StudyMode::Review;
    QString m_cardFilter;
};
