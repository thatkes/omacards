#pragma once

#include "fsrs.h"

#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

struct ReviewSummary {
    int total = 0;
    int studyDayRatings = 0;
    int studyDayGood = 0;
    int streak = 0;
    int newCardsOnStudyDay = 0;
};

// One row of the scheduling read: a Card's Scheduling state plus the content
// hash that guards it. Both come from the same cards row.
struct SchedulingRecord {
    FsrsState state;
    QString hash;
};

// The SQLite sidecar. Deck files own card content; this database owns only
// scheduling state, so deleting it never loses a card.
class Store {
public:
    explicit Store(const QString &path = {});
    ~Store();

    bool open(QString *error = nullptr);
    QHash<QString, FsrsState> states() const;
    QHash<QString, QString> hashes() const;
    QHash<QString, SchedulingRecord> schedulingRecords() const;
    void save(const QString &cardId, const QString &hash, const FsrsState &state);
    void recordRating(const QString &cardId, int rating, qint64 at, const FsrsState &state, int intervalDays,
                      bool wasNew = false);
    void removeExcept(const QSet<QString> &ids);
    ReviewSummary summary(int dayStartHour = 0) const;
    int newCardsOnStudyDay(int dayStartHour = 0) const;

    QString setting(const QString &key, const QString &fallback = {}) const;
    bool setSetting(const QString &key, const QString &value);

    // Current deck and per-deck last-studied records live in the same settings
    // table as the other sidecar state; no extra file or module is involved.
    QString currentDeck() const;
    bool setCurrentDeck(const QString &name);
    QHash<QString, qint64> deckLastStudied() const;
    bool setDeckLastStudied(const QString &name, qint64 at);
    bool moveDeckSettings(const QString &from, const QString &to);

    QString path() const { return m_path; }

    // Errors are sticky until clearError() so a caller can check once after a
    // batch of reads instead of every query. Silent failures used to make all
    // cards look new.
    QString lastError() const { return m_lastError; }
    void clearError() { m_lastError.clear(); }

private:
    int countNewCardsOnStudyDay(int dayStartHour = 0) const;
    bool removeSetting(const QString &key);

    QString m_path;
    QString m_connection;
    QSqlDatabase m_db;
    mutable QSqlQuery m_schedulingQuery;
    mutable bool m_schedulingPrepared = false;
    mutable QString m_lastError;
};
