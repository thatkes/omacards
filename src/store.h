#pragma once

#include "fsrs.h"

#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QString>

struct ReviewSummary {
    int total = 0;
    int today = 0;
    int todayGood = 0;
    int streakDays = 0;
    int newToday = 0;
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
    void save(const QString &cardId, const QString &hash, const FsrsState &state);
    void logReview(const QString &cardId, int rating, qint64 at, const FsrsState &state, int intervalDays,
                   bool wasNew = false);
    void removeExcept(const QSet<QString> &ids);
    ReviewSummary summary(int dayStartHour = 0) const;
    int newCardsToday(int dayStartHour = 0) const;

    QString setting(const QString &key, const QString &fallback = {}) const;
    bool setSetting(const QString &key, const QString &value);

    QString path() const { return m_path; }

    // Errors are sticky until clearError() so a caller can check once after a
    // batch of reads instead of every query. Silent failures used to make all
    // cards look new.
    QString lastError() const { return m_lastError; }
    void clearError() { m_lastError.clear(); }

private:
    int countNewCardsToday(int dayStartHour = 0) const;

    QString m_path;
    QString m_connection;
    QSqlDatabase m_db;
    mutable QString m_lastError;
};
