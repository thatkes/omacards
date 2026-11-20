#include "deckrepository.h"
#include "domain.h"
#include "importer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <algorithm>

namespace {
// Keep the original translation context: these strings already have entries
// under "Backend" and QML clients expect their wording to stay identical.
QString trBackend(const char *text) {
    return QCoreApplication::translate("Backend", text);
}
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
    m_deckNames.clear();
    for (const Deck &deck : m_decks)
        m_deckNames << deck.name;
    if (!m_deckNames.contains(*currentDeck))
        *currentDeck = m_deckNames.isEmpty() ? QString() : m_deckNames.first();

    // Only prune sidecar rows after a clean, conclusive load. loadAll() skips
    // deck files it cannot read; pruning then would silently erase their FSRS
    // progress. An intentional last-deck deletion prunes in removeDeck().
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
    for (Deck &deck : m_decks)
        for (Card &card : deck.cards)
            if (card.id == id)
                return &card;
    return nullptr;
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

bool DeckRepository::commitDeckEdit(Deck *deck, const Deck &original, const QByteArray &before,
                                    const QString &description, QString *error) {
    if (!DeckIO::save(*deck, error)) {
        *deck = original;
        return false;
    }
    recordHistory(deck->path, before, true, description);
    return true;
}

bool DeckRepository::undo(QString *error, QString *description) {
    if (!m_history.undo(error, description)) {
        emit errorOccurred(*error);
        return false;
    }
    emit historyChanged();
    return true;
}

bool DeckRepository::redo(QString *error, QString *description) {
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
    const QByteArray before = existed ? FileHistory::readBytes(deck.path) : QByteArray();
    if (!DeckIO::save(deck, error))
        return false;
    recordHistory(deck.path, before, existed, trBackend("creating deck %1").arg(clean));
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
        if (draft.question.trimmed().isEmpty()) {
            if (error)
                *error = trBackend("cards[%1] needs a question").arg(i);
            return false;
        }
        if (Domain::cardTypeFromString(draft.type) == CardType::Invalid) {
            if (error)
                *error = trBackend("cards[%1] has an invalid type: %2").arg(i).arg(draft.type);
            return false;
        }
    }

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
        card.type = Domain::toString(Domain::cardTypeFromString(draft.type));
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
    card->type = Domain::toString(Domain::cardTypeFromString(type));
    card->tags.clear();
    for (const QString &tag : tags.split(',', Qt::SkipEmptyParts)) {
        const QString clean = tag.trimmed();
        if (!clean.isEmpty() && !card->tags.contains(clean))
            card->tags << clean;
    }
    // An explicit id pinned in the file survives edits; a generated id follows
    // the content so the card resets its schedule when it changes.
    if (!card->explicitId)
        card->id = DeckIO::cardId(*card);
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
    if (!QFile::remove(path)) {
        if (error)
            *error = trBackend("Cannot remove deck file: %1").arg(path);
        return false;
    }
    recordHistory(path, before, true, trBackend("deleting deck %1").arg(name));
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
    // Only a new deck receives an imported description; an existing deck keeps
    // the description its owner wrote.
    QString description;
    const QString *descriptionPtr = nullptr;
    if (!deckByName(result.deckName) && !result.description.isEmpty()) {
        description = result.description;
        descriptionPtr = &description;
    }
    if (!createCards(result.deckName, currentDeck, result.cards, descriptionPtr, nullptr, error))
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
