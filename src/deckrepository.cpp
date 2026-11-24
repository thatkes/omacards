#include "deckrepository.h"
#include "domain.h"
#include "importer.h"

#include <QCoreApplication>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {
// Keep the original translation context: these strings already have entries
// under "Backend" and QML clients expect their wording to stay identical.
QString trBackend(const char *text) {
    return QCoreApplication::translate("Backend", text);
}

void setError(QString *error, const QString &message) {
    if (error)
        *error = message;
}

// Compare errors by meaning, not by line number: appending or editing a card
// can legitimately move a pre-existing issue further down the file.
QString errorSignature(const DeckIssue &issue) {
    QString message = issue.message;
    static const QRegularExpression lineNumber(R"re(\b(?:near )?line \d+\b)re");
    message.replace(lineNumber, QStringLiteral("line N"));
    return issue.severity + QChar(0x1f) + issue.card + QChar(0x1f) + message;
}

QSet<QString> errorSignatures(const QVector<DeckIssue> &issues) {
    QSet<QString> signatures;
    for (const DeckIssue &issue : issues) {
        if (issue.severity == QLatin1String("error"))
            signatures.insert(errorSignature(issue));
    }
    return signatures;
}

const DeckIssue *firstNewError(const QVector<DeckIssue> &issues, const QSet<QString> &baseline) {
    for (const DeckIssue &issue : issues) {
        if (issue.severity == QLatin1String("error") && !baseline.contains(errorSignature(issue)))
            return &issue;
    }
    return nullptr;
}

QString validationError(const DeckIssue &issue) {
    QString message = trBackend("Deck would be invalid after write: %1").arg(issue.message);
    if (!issue.hint.isEmpty())
        message += QStringLiteral(" ") + trBackend("Hint: %1").arg(issue.hint);
    return message;
}

QSet<QString> baselineErrors(const QString &path, const QByteArray &bytes, bool existed) {
    return existed ? errorSignatures(DeckIO::lintBytes(path, bytes)) : QSet<QString>();
}

// A write made by this process must not echo back as a reload request. The
// repository watches one directory, so dropping that watch around a write
// suppresses only its own event; external changes keep the watch the rest of
// the time and still reach the debounce timer.
class WatcherPause {
public:
    WatcherPause(QFileSystemWatcher &watcher, const QString &path)
        : m_watcher(watcher), m_path(path), m_watched(watcher.directories().contains(path)) {
        if (m_watched)
            m_watcher.removePath(path);
    }
    ~WatcherPause() {
        if (m_watched)
            m_watcher.addPath(m_path);
    }
    Q_DISABLE_COPY(WatcherPause)

private:
    QFileSystemWatcher &m_watcher;
    QString m_path;
    bool m_watched;
};
} // namespace

DeckRepository::DeckRepository(const QString &decksDirectory, Store &store, FileHistory &history, QObject *parent)
    : QObject(parent), m_decksDir(decksDirectory), m_store(store), m_history(history) {
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(120);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reloadTimer.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_reloadTimer.start(); });
    connect(&m_reloadTimer, &QTimer::timeout, this, &DeckRepository::reloadRequested);
}

bool DeckRepository::initialize(QString *error) {
    const bool fresh = !QFileInfo::exists(m_decksDir);
    if (!QDir().mkpath(m_decksDir)) {
        const QString message = trBackend("Cannot create deck directory: %1").arg(m_decksDir);
        emit errorOccurred(message);
        if (error)
            *error = message;
        return false;
    }
    if (fresh) {
        WatcherPause pause(m_watcher, m_decksDir);
        Deck welcome;
        welcome.name = trBackend("Welcome");
        welcome.path = QDir(m_decksDir).filePath("Welcome.txt");
        Card card;
        card.deck = welcome.name;
        card.question = trBackend("What are the two study modes in omacards?");
        card.answer = trBackend("FSRS review (long-term) and cram (endless Leitner).");
        card.id = DeckIO::cardId(card);
        welcome.cards << card;
        Card mathCard;
        mathCard.deck = welcome.name;
        mathCard.question = trBackend("Use a keyboard shortcut to flip a card. Math like \\(x^2\\) works too.");
        mathCard.answer = trBackend("Press Space or the right arrow. Every shortcut is in the ? overlay.");
        mathCard.id = DeckIO::cardId(mathCard);
        welcome.cards << mathCard;
        DeckIO::save(welcome);
    }
    return true;
}

void DeckRepository::watchDirectory() {
    m_watcher.addPath(m_decksDir);
}

void DeckRepository::load(QString *currentDeck) {
    QString error;
    const bool hadDecks = !m_deckNames.isEmpty();
    const auto decks = DeckIO::loadAll(m_decksDir, &error);
    if (!error.isEmpty())
        emit errorOccurred(error);
    m_decks = decks;
    rebuildCardIndex();
    m_deckNames.clear();
    for (const Deck &deck : m_decks)
        m_deckNames << deck.name;
    if (!m_deckNames.contains(*currentDeck))
        *currentDeck = m_deckNames.isEmpty() ? QString() : m_deckNames.first();

    // Only prune sidecar rows after a clean, conclusive load. loadAll() skips
    // deck files it cannot read; pruning then would silently erase their
    // Scheduling state. An intentional last-deck deletion prunes in removeDeck().
    if (error.isEmpty() && (!m_decks.isEmpty() || !hadDecks)) {
        QSet<QString> ids;
        for (const Deck &deck : m_decks)
            for (const Card &card : deck.cards)
                ids << card.id;
        m_store.removeExcept(ids);
        if (!m_store.lastError().isEmpty())
            emit errorOccurred(trBackend("Cannot clean scheduling state: %1").arg(m_store.lastError()));
    }
}

QString DeckRepository::deckPath(const QString &name) const {
    const Deck *deck = deckByName(name);
    return deck ? deck->path : QString();
}

Deck *DeckRepository::deckByName(const QString &name) {
    for (Deck &deck : m_decks)
        if (deck.name == name)
            return &deck;
    return nullptr;
}

const Deck *DeckRepository::deckByName(const QString &name) const {
    for (const Deck &deck : m_decks)
        if (deck.name == name)
            return &deck;
    return nullptr;
}

int DeckRepository::deckIndex(const QString &name) const {
    for (int i = 0; i < m_decks.size(); ++i)
        if (m_decks.at(i).name == name)
            return i;
    return -1;
}

Card *DeckRepository::cardById(const QString &id) {
    const auto it = m_cardIndex.constFind(id);
    if (it == m_cardIndex.constEnd())
        return nullptr;
    const CardLocation location = *it;
    if (location.deck < 0 || location.deck >= m_decks.size())
        return nullptr;
    QVector<Card> &cards = m_decks[location.deck].cards;
    if (location.card < 0 || location.card >= cards.size() || cards.at(location.card).id != id)
        return nullptr;
    return &cards[location.card];
}

void DeckRepository::rebuildCardIndex() {
    m_cardIndex.clear();
    for (int deck = 0; deck < m_decks.size(); ++deck) {
        const QVector<Card> &cards = m_decks.at(deck).cards;
        for (int card = 0; card < cards.size(); ++card) {
            const QString &id = cards.at(card).id;
            // Keep the first occurrence: duplicate explicit ids resolve to
            // the card nearest the start of the deck file, as before.
            if (!m_cardIndex.contains(id))
                m_cardIndex.insert(id, CardLocation{deck, card});
        }
    }
}

int DeckRepository::totalCards() const {
    int total = 0;
    for (const Deck &deck : m_decks)
        total += deck.cards.size();
    return total;
}

QVector<Card> DeckRepository::searchCards(const QString &text, const QString &deckName) const {
    QVector<Card> result;
    const QString needle = text.trimmed().toLower();
    for (const Deck &deck : m_decks) {
        if (!deckName.isEmpty() && deck.name != deckName)
            continue;
        for (const Card &card : deck.cards) {
            const QString haystack = (card.question + " " + card.answer + " " + card.tags.join(' ')).toLower();
            if (needle.isEmpty() || haystack.contains(needle))
                result << card;
        }
    }
    return result;
}

void DeckRepository::recordHistory(const QString &path, const QByteArray &before, bool existedBefore,
                                   const QString &description) {
    if (m_history.record(path, before, existedBefore, description))
        emit historyChanged();
}

void DeckRepository::recordHistory(const QString &path, const QByteArray &before, bool existedBefore,
                                   const QByteArray &after, const QString &description) {
    if (m_history.record(path, before, existedBefore, after, description))
        emit historyChanged();
}

void DeckRepository::recordRename(const QString &from, const QByteArray &before, const QString &to,
                                  const QByteArray &after, const QString &description) {
    if (m_history.recordRename(from, before, to, after, description))
        emit historyChanged();
}

bool DeckRepository::commitDeckEdit(Deck *deck, const Deck &original, const QByteArray &before,
                                    const QString &description, QString *error) {
    const QSet<QString> baseline = baselineErrors(deck->path, before, true);
    WatcherPause pause(m_watcher, m_decksDir);
    QByteArray written;
    if (!DeckIO::save(*deck, &written, error)) {
        *deck = original;
        rebuildCardIndex();
        return false;
    }

    const QVector<DeckIssue> issues = DeckIO::lintBytes(deck->path, written);
    if (const DeckIssue *invalid = firstNewError(issues, baseline)) {
        QString restoreError;
        if (!FileHistory::restore(deck->path, before, true, &restoreError)) {
            setError(error, trBackend("Deck write failed validation and could not be restored: %1").arg(restoreError));
            *deck = original;
            rebuildCardIndex();
            return false;
        }
        *deck = original;
        setError(error, validationError(*invalid));
        rebuildCardIndex();
        return false;
    }

    recordHistory(deck->path, before, true, written, description);
    rebuildCardIndex();
    return true;
}

bool DeckRepository::undo(QString *error, QString *description) {
    WatcherPause pause(m_watcher, m_decksDir);
    if (!m_history.undo(error, description)) {
        emit errorOccurred(*error);
        return false;
    }
    emit historyChanged();
    return true;
}

bool DeckRepository::redo(QString *error, QString *description) {
    WatcherPause pause(m_watcher, m_decksDir);
    if (!m_history.redo(error, description)) {
        emit errorOccurred(*error);
        return false;
    }
    emit historyChanged();
    return true;
}

bool DeckRepository::createDeck(const QString &name, QString *currentDeck, QString *error, const QString &description) {
    const QString clean = name.trimmed();
    if (clean.isEmpty()) {
        if (error)
            *error = trBackend("Deck name cannot be empty");
        return false;
    }
    if (m_deckNames.contains(clean)) {
        if (error)
            *error = trBackend("Deck already exists: %1").arg(clean);
        return false;
    }
    Deck deck;
    deck.name = clean;
    deck.description = description.trimmed();
    deck.path = QDir(m_decksDir).filePath(DeckIO::safeFileName(clean) + ".txt");
    for (const Deck &existing : m_decks) {
        if (QFileInfo(existing.path).absoluteFilePath() == QFileInfo(deck.path).absoluteFilePath()) {
            if (error)
                *error = trBackend("Deck file already used by %1").arg(existing.name);
            return false;
        }
    }
    // An unreadable file is not in m_decks; never silently overwrite it.
    if (QFileInfo::exists(deck.path)) {
        if (error)
            *error = trBackend("A deck file already exists at %1").arg(deck.path);
        return false;
    }
    const bool existed = false;
    const QByteArray before;
    WatcherPause pause(m_watcher, m_decksDir);
    QByteArray written;
    if (!DeckIO::save(deck, &written, error))
        return false;

    const QVector<DeckIssue> issues = DeckIO::lintBytes(deck.path, written);
    if (const DeckIssue *invalid = firstNewError(issues, QSet<QString>())) {
        QString restoreError;
        if (!FileHistory::restore(deck.path, before, existed, &restoreError))
            setError(error, trBackend("Deck write failed validation and could not be restored: %1").arg(restoreError));
        else
            setError(error, validationError(*invalid));
        return false;
    }

    recordHistory(deck.path, before, existed, written, trBackend("creating deck %1").arg(clean));
    // The old Backend::createDeck() called refresh() here, so callers could
    // immediately look the new deck up (createCards(), imports, CLI). Reload
    // our own view; Backend still reloads and selects it afterwards.
    QString selected = clean;
    if (currentDeck)
        *currentDeck = selected;
    load(&selected);
    if (currentDeck)
        *currentDeck = selected;
    return true;
}

bool DeckRepository::renameDeck(const QString &oldName, const QString &newName, QString *currentDeck,
                                DeckRename::Result *result, QString *error) {
    const QString from = oldName.trimmed();
    const QString to = newName.trimmed();
    if (from.isEmpty() || to.isEmpty()) {
        setError(error, trBackend("Give both the old and new deck names"));
        return false;
    }
    Deck *deck = deckByName(from);
    if (!deck) {
        setError(error, trBackend("No such deck: %1").arg(from));
        return false;
    }
    if (m_deckNames.contains(to)) {
        setError(error, trBackend("Deck already exists: %1").arg(to));
        return false;
    }

    const QString oldPath = deck->path;
    const QString newPath = QDir(m_decksDir).filePath(DeckIO::safeFileName(to) + ".txt");
    const bool samePath = QFileInfo(oldPath).absoluteFilePath() == QFileInfo(newPath).absoluteFilePath();
    if (!samePath) {
        for (int i = 0; i < m_decks.size(); ++i) {
            if (&m_decks[i] == deck)
                continue;
            if (QFileInfo(m_decks.at(i).path).absoluteFilePath() == QFileInfo(newPath).absoluteFilePath()) {
                setError(error, trBackend("Deck file already used by %1").arg(m_decks.at(i).name));
                return false;
            }
        }
        // An unreadable file is not in m_decks; never silently overwrite it.
        if (QFileInfo::exists(newPath)) {
            setError(error, trBackend("A deck file already exists at %1").arg(newPath));
            return false;
        }
    }

    const Deck original = *deck;
    const QByteArray before = FileHistory::readBytes(oldPath);
    const QSet<QString> baseline = baselineErrors(oldPath, before, true);
    Deck updated = original;
    updated.name = to;
    updated.path = newPath;
    int preserved = 0;
    for (Card &card : updated.cards) {
        card.deck = to;
        // cardId() hashes the Deck name, so a generated id would change. Pin
        // the existing id before writing the new file. A Card whose id was
        // already explicit needs no second pin, but its id is still preserved.
        if (card.explicitId) {
            ++preserved;
            continue;
        }
        if (card.id != DeckIO::cardId(card)) {
            card.explicitId = true;
            ++preserved;
        }
    }

    const QString action = trBackend("renaming deck %1 to %2").arg(from, to);
    const auto rollback = [&](const QString &message) {
        QString restoreError;
        if (!FileHistory::restore(samePath ? oldPath : newPath, samePath ? before : QByteArray(), samePath,
                                  &restoreError))
            setError(error, trBackend("Deck rename failed and could not be restored: %1").arg(restoreError));
        else
            setError(error, message);
        return false;
    };

    WatcherPause pause(m_watcher, m_decksDir);
    QByteArray written;
    if (!DeckIO::save(updated, &written, error))
        return false;
    const QVector<DeckIssue> issues = DeckIO::lintBytes(newPath, written);
    if (const DeckIssue *invalid = firstNewError(issues, baseline))
        return rollback(validationError(*invalid));

    if (!samePath && !QFile::remove(oldPath))
        return rollback(trBackend("Cannot remove deck file: %1").arg(oldPath));

    *deck = updated;
    rebuildCardIndex();
    const int nameIndex = m_deckNames.indexOf(from);
    if (nameIndex >= 0)
        m_deckNames[nameIndex] = to;
    if (currentDeck && *currentDeck == from)
        *currentDeck = to;
    m_store.clearError();
    if (!m_store.moveDeckSettings(from, to) || !m_store.lastError().isEmpty())
        emit errorOccurred(trBackend("Cannot move deck settings for %1: %2").arg(to, m_store.lastError()));
    if (samePath)
        recordHistory(newPath, before, true, written, action);
    else
        recordRename(oldPath, before, newPath, written, action);

    if (result) {
        result->name = to;
        result->path = newPath;
        result->cards = static_cast<int>(updated.cards.size());
        result->preservedIds = preserved;
    }
    return true;
}

bool DeckRepository::setDeckDescription(const QString &deckName, const QString &description, QString *error) {
    Deck *deck = deckByName(deckName);
    if (!deck) {
        if (error)
            *error = trBackend("No such deck: %1").arg(deckName);
        return false;
    }
    const QString clean = description.trimmed();
    if (deck->description == clean)
        return true;
    const Deck original = *deck;
    const QByteArray before = FileHistory::readBytes(deck->path);
    deck->description = clean;
    return commitDeckEdit(deck, original, before, trBackend("editing deck description"), error);
}

bool DeckRepository::createCards(const QString &deckName, QString *currentDeck, const QVector<CardDraft> &drafts,
                                 const QString *description, QStringList *createdIds, QString *error) {
    const QString target = deckName.isEmpty() ? (currentDeck ? *currentDeck : QString()) : deckName.trimmed();
    if (target.isEmpty()) {
        if (error)
            *error = trBackend("Give a deck name");
        return false;
    }
    if (drafts.isEmpty()) {
        if (error)
            *error = trBackend("No cards to add");
        return false;
    }
    for (int i = 0; i < drafts.size(); ++i) {
        const CardDraft &draft = drafts.at(i);
        const CardType type = Domain::cardTypeFromString(draft.type);
        if (draft.question.trimmed().isEmpty()) {
            if (error)
                *error = trBackend("cards[%1] needs a question").arg(i);
            return false;
        }
        if (type == CardType::Invalid) {
            if (error)
                *error = trBackend("cards[%1] has an invalid type: %2").arg(i).arg(draft.type);
            return false;
        }
        // Cloze answers are derived from the question, so an empty answer is
        // valid there; basic Cards need one to be reviewable.
        if (draft.answer.trimmed().isEmpty() && type != CardType::Cloze) {
            if (error)
                *error = trBackend("cards[%1] needs an answer").arg(i);
            return false;
        }
    }

    // Each process loads Deck files at startup. Refresh under an exclusive
    // lock so a writer appends to the latest Deck file instead of its stale
    // snapshot; contention fails explicitly rather than losing a Card.
    const QString lockPath = QDir(m_decksDir).filePath(DeckIO::safeFileName(target) + ".txt.lock");
    QLockFile lock(lockPath);
    if (!lock.tryLock(5000)) {
        setError(error, trBackend("Cannot lock deck %1").arg(target));
        return false;
    }
    QString selected = currentDeck ? *currentDeck : QString();
    load(&selected);
    if (currentDeck)
        *currentDeck = selected;

    if (!deckByName(target) && !createDeck(target, currentDeck, error, description ? *description : QString()))
        return false;
    Deck *deck = deckByName(target);
    if (!deck) {
        if (error)
            *error = trBackend("No such deck: %1").arg(target);
        return false;
    }

    const Deck original = *deck;
    const QByteArray before = FileHistory::readBytes(deck->path);
    // An absent or empty description must not erase one already on the deck.
    if (description && !description->trimmed().isEmpty())
        deck->description = description->trimmed();

    QVector<Card> cards;
    cards.reserve(drafts.size());
    for (const CardDraft &draft : drafts) {
        Card card;
        card.deck = deck->name;
        card.question = draft.question.trimmed();
        card.answer = draft.answer.trimmed();
        // Validated above; toString() collapses Invalid to the basic text,
        // keeping the stored kind and text canonical.
        card.type = Domain::parseCardType(Domain::toString(Domain::cardTypeFromString(draft.type)));
        for (const QString &tag : draft.tags) {
            const QString clean = tag.trimmed();
            if (!clean.isEmpty() && !card.tags.contains(clean))
                card.tags << clean;
        }
        card.id = DeckIO::cardId(card);
        cards << card;
    }

    deck->cards << cards;
    const QString action =
        cards.size() == 1 ? trBackend("adding a card") : trBackend("adding %1 cards").arg(cards.size());
    if (!commitDeckEdit(deck, original, before, action, error))
        return false;
    if (createdIds)
        for (const Card &card : cards)
            createdIds->append(card.id);
    return true;
}

bool DeckRepository::createCard(const QString &deckName, QString *currentDeck, const QString &question,
                                const QString &answer, const QString &type, const QString &tags, QString *error) {
    const QString target = deckName.isEmpty() ? (currentDeck ? *currentDeck : QString()) : deckName;
    // Adding a single card from the UI still requires an existing deck; only
    // the bulk/import path is allowed to create one implicitly.
    if (!deckByName(target)) {
        if (error)
            *error = trBackend("No such deck: %1").arg(deckName);
        return false;
    }
    CardDraft draft;
    draft.question = question;
    draft.answer = answer;
    draft.type = type;
    draft.tags = tags.split(',', Qt::SkipEmptyParts);
    return createCards(target, currentDeck, {draft}, nullptr, nullptr, error);
}

bool DeckRepository::modifyCard(const QString &deckName, const QString &cardId, const QString &question,
                                const QString &answer, const QString &type, const QString &tags, QString *error,
                                QString *finalId) {
    Deck *deck = deckByName(deckName);
    if (!deck) {
        if (error)
            *error = trBackend("No such deck: %1").arg(deckName);
        return false;
    }
    const auto found =
        std::find_if(deck->cards.begin(), deck->cards.end(), [&](const Card &card) { return card.id == cardId; });
    if (found == deck->cards.end()) {
        if (error)
            *error = trBackend("No such card: %1").arg(cardId);
        return false;
    }
    Card *card = &*found;
    if (question.trimmed().isEmpty()) {
        if (error)
            *error = trBackend("A card needs a question");
        return false;
    }

    const Deck original = *deck;
    card->question = question.trimmed();
    card->answer = answer.trimmed();
    // Invalid input normalizes to { Basic, "basic" } as before.
    card->type = Domain::parseCardType(Domain::toString(Domain::cardTypeFromString(type)));
    card->tags.clear();
    for (const QString &tag : tags.split(',', Qt::SkipEmptyParts)) {
        const QString clean = tag.trimmed();
        if (!clean.isEmpty() && !card->tags.contains(clean))
            card->tags << clean;
    }
    // Keep the id callers already have. When the content no longer matches the
    // generated hash, serialize() pins the existing id with an `id:` line.
    if (!card->explicitId && card->id != DeckIO::cardId(*card))
        card->explicitId = true;
    if (finalId)
        *finalId = card->id;

    const QByteArray before = FileHistory::readBytes(deck->path);
    return commitDeckEdit(deck, original, before, trBackend("editing a card"), error);
}

bool DeckRepository::deleteCard(const QString &deckName, const QString &cardId, QString *error) {
    Deck *deck = deckByName(deckName);
    if (!deck) {
        if (error)
            *error = trBackend("No such deck: %1").arg(deckName);
        return false;
    }
    const auto found =
        std::find_if(deck->cards.begin(), deck->cards.end(), [&](const Card &card) { return card.id == cardId; });
    if (found == deck->cards.end()) {
        if (error)
            *error = trBackend("No such card: %1").arg(cardId);
        return false;
    }
    const Deck original = *deck;
    const QByteArray before = FileHistory::readBytes(deck->path);
    deck->cards.erase(found);
    return commitDeckEdit(deck, original, before, trBackend("removing a card"), error);
}

bool DeckRepository::removeDeck(const QString &name, QString *currentDeck, QString *error) {
    const Deck *deck = deckByName(name);
    if (!deck) {
        if (error)
            *error = trBackend("No such deck: %1").arg(name);
        return false;
    }
    const QString path = deck->path;
    const QByteArray before = FileHistory::readBytes(path);
    WatcherPause pause(m_watcher, m_decksDir);
    if (!QFile::remove(path)) {
        if (error)
            *error = trBackend("Cannot remove deck file: %1").arg(path);
        return false;
    }
    recordHistory(path, before, true, trBackend("deleting deck %1").arg(name));
    // The in-memory decks stay until the caller's load(); keep the index
    // aligned with them in the meantime.
    rebuildCardIndex();
    if (currentDeck && *currentDeck == name)
        currentDeck->clear();
    // An intentional deletion may remove the last deck; load() deliberately
    // skips pruning in that case, so do it here instead.
    QSet<QString> ids;
    for (const Deck &remaining : m_decks)
        for (const Card &card : remaining.cards)
            ids << card.id;
    m_store.removeExcept(ids);
    if (!m_store.lastError().isEmpty())
        emit errorOccurred(trBackend("Cannot clean scheduling state: %1").arg(m_store.lastError()));
    return true;
}

bool DeckRepository::commitImport(const DeckImport::Result &result, QString *currentDeck, QString *error) {
    if (result.cards.isEmpty()) {
        emit statusMessage(trBackend("Imported 0 cards"));
        return true;
    }
    if (!createCards(result.deckName, currentDeck, result.cards, nullptr, nullptr, error))
        return false;
    emit statusMessage(trBackend("Imported %1 cards").arg(result.cards.size()));
    return true;
}

bool DeckRepository::importCsv(const QString &path, const QString &deckName, QString *currentDeck, QString *error) {
    DeckImport::Result result;
    return DeckImport::csv(path, deckName, &result, error) && commitImport(result, currentDeck, error);
}

bool DeckRepository::importApkg(const QString &path, const QString &deckName, QString *currentDeck, QString *error) {
    DeckImport::Result result;
    return DeckImport::apkg(path, deckName, m_decksDir, &result, error) && commitImport(result, currentDeck, error);
}

bool DeckRepository::importFile(const QString &path, const QString &deckName, QString *currentDeck, QString *error) {
    DeckImport::Result result;
    return DeckImport::file(path, deckName, m_decksDir, &result, error) && commitImport(result, currentDeck, error);
}
