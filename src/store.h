#pragma once

#include "fsrs.h"

#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QString>

struct ReviewSummary {
    int total = 0;
    int today = 0;
    int todayCorrect = 0;
    int streakDays = 0;
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
    void logReview(const QString &cardId, int rating, qint64 at,
                   const FsrsState &state, int intervalDays);
    void removeExcept(const QSet<QString> &ids);
    ReviewSummary summary() const;

    QString path() const { return m_path; }

private:
    QString m_path;
    QString m_connection;
    QSqlDatabase m_db;
};
