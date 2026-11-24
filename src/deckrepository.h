#pragma once

#include "deck.h"
#include "filehistory.h"
#include "store.h"

#include <QByteArray>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

namespace DeckImport {
struct Result;
}

namespace DeckRename {
struct Result {
    QString name;         // new Deck name
    QString path;         // file the renamed Deck is stored in
    int cards = 0;        // Cards whose deck reference was changed
    int preservedIds = 0; // ids preserved by this rename (explicit or newly pinned)
};
} // namespace DeckRename

// Owns deck files, in-memory deck data and the file watcher. Scheduling state
// is read and written through the shared Store; Backend still owns settings,
// the Current deck and all QML-facing state.
class DeckRepository : public QObject {
    Q_OBJECT

public:
    explicit DeckRepository(const QString &decksDirectory, Store &store, FileHistory &history,
                            QObject *parent = nullptr);

    bool initialize(QString *error = nullptr);
    void watchDirectory();
    void load(QString *currentDeck);

    QString decksDirectory() const { return m_decksDir; }
    QStringList deckNames() const { return m_deckNames; }
    QVector<Deck> decks() const { return m_decks; }
    const QVector<Deck> &deckData() const { return m_decks; }
    QString deckPath(const QString &name) const;
    Deck *deckByName(const QString &name);
    const Deck *deckByName(const QString &name) const;
    int deckIndex(const QString &name) const;
    Card *cardById(const QString &id);
    int totalCards() const;
    QVector<Card> searchCards(const QString &text, const QString &deckName = {}) const;

    bool createDeck(const QString &name, QString *currentDeck, QString *error, const QString &description = {});
    bool renameDeck(const QString &oldName, const QString &newName, QString *currentDeck, DeckRename::Result *result,
                    QString *error);
    bool setDeckDescription(const QString &deckName, const QString &description, QString *error);
    bool createCards(const QString &deckName, QString *currentDeck, const QVector<CardDraft> &drafts,
                     const QString *description, QStringList *createdIds, QString *error);
    bool createCard(const QString &deckName, QString *currentDeck, const QString &question, const QString &answer,
                    const QString &type, const QString &tags, QString *error);
    bool modifyCard(const QString &deckName, const QString &cardId, const QString &question, const QString &answer,
                    const QString &type, const QString &tags, QString *error, QString *finalId = nullptr);
    bool deleteCard(const QString &deckName, const QString &cardId, QString *error);
    bool removeDeck(const QString &name, QString *currentDeck, QString *error);

    bool importFile(const QString &path, const QString &deckName, QString *currentDeck, QString *error);
    bool importCsv(const QString &path, const QString &deckName, QString *currentDeck, QString *error);
    bool importApkg(const QString &path, const QString &deckName, QString *currentDeck, QString *error);

    bool undo(QString *error, QString *description);
    bool redo(QString *error, QString *description);

signals:
    void reloadRequested();
    void historyChanged();
    void statusMessage(const QString &message);
    void errorOccurred(const QString &message);

private:
    struct CardLocation {
        int deck = -1;
        int card = -1;
    };

    void rebuildCardIndex();
    void recordHistory(const QString &path, const QByteArray &before, bool existedBefore, const QString &description);
    void recordHistory(const QString &path, const QByteArray &before, bool existedBefore, const QByteArray &after,
                       const QString &description);
    void recordRename(const QString &from, const QByteArray &before, const QString &to, const QByteArray &after,
                      const QString &description);
    bool commitDeckEdit(Deck *deck, const Deck &original, const QByteArray &before, const QString &description,
                        QString *error);
    bool commitImport(const DeckImport::Result &result, QString *currentDeck, QString *error);

    QString m_decksDir;
    QStringList m_deckNames;
    QVector<Deck> m_decks;
    // Card id -> position in m_decks. Rebuilt whenever the deck data changes;
    // positions, not pointers, survive QVector reallocation during edits.
    QHash<QString, CardLocation> m_cardIndex;
    Store &m_store;
    FileHistory &m_history;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
};
