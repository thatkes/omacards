// Backend is the QML/CLI facade: it validates input, keeps the current deck
// and settings, and forwards state from the repository, planner and session.
#include "backend.h"

#include "mathrender.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace {
QString defaultDataDirectory() {
    // Use a deterministic base directory that does not depend on whether
    // QCoreApplication::setOrganizationName() has been called yet: the GUI
    // sets it to "omarchy", while the headless CLI path returns before that,
    // which previously made the two resolve different decks directories.
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/omacards";
}

QString defaultDecksDirectory() {
    const QString fromEnv = qEnvironmentVariable("OMACARDS_DECKS_DIR");
    return fromEnv.isEmpty() ? defaultDataDirectory() + "/decks" : fromEnv;
}

QString defaultDatabasePath() {
    const QString fromEnv = qEnvironmentVariable("OMACARDS_STATE_DB");
    return fromEnv.isEmpty() ? defaultDataDirectory() + "/state.db" : fromEnv;
}

} // namespace

Backend::Backend(const QString &decksDirectory, const QString &databasePath, QObject *parent)
    : QObject(parent), m_store(databasePath.isEmpty() ? defaultDatabasePath() : databasePath), m_ratingStore(m_store),
      m_ratings(m_ratingStore),
      m_deckRepo(decksDirectory.isEmpty() ? defaultDecksDirectory() : decksDirectory, m_store, m_history, this),
      m_plannerStore(m_store), m_planner(m_plannerStore), m_session(m_deckRepo, m_planner, m_ratings, this) {
    connect(&m_deckRepo, &DeckRepository::reloadRequested, this, &Backend::refresh);
    connect(&m_deckRepo, &DeckRepository::errorOccurred, &m_session, &StudySession::reportError);
    connect(&m_deckRepo, &DeckRepository::historyChanged, this, &Backend::historyChanged);
    connect(&m_deckRepo, &DeckRepository::statusMessage, &m_session, &StudySession::setStatus);
    connect(&m_session, &StudySession::modeChanged, this, &Backend::modeChanged);
    connect(&m_session, &StudySession::cardChanged, this, &Backend::cardChanged);
    connect(&m_session, &StudySession::dueCountChanged, this, &Backend::dueCountChanged);
    connect(&m_session, &StudySession::statsChanged, this, &Backend::statsChanged);
    connect(&m_session, &StudySession::statusChanged, this, &Backend::statusChanged);
    connect(&m_session, &StudySession::errorOccurred, this, &Backend::errorOccurred);
}

std::unique_ptr<Backend> Backend::fromEnvironment(QObject *parent) {
    return std::make_unique<Backend>(defaultDecksDirectory(), defaultDatabasePath(), parent);
}

bool Backend::initialize(QString *error) {
    if (!m_deckRepo.initialize(error))
        return false;

    if (!m_store.open(error)) {
        m_session.reportError(tr("Cannot open state database: %1").arg(m_store.path()));
        return false;
    }
    m_persistedDeck = m_store.currentDeck();
    m_currentDeck = m_persistedDeck;
    m_lastStudied = m_store.deckLastStudied();
    // Store::open copies legacy values forward; the nested fallback also
    // covers databases read without that migration step.
    const QString allowance =
        m_store.setting(QStringLiteral("new_card_allowance"),
                        m_store.setting(QStringLiteral("new_per_day"), QString::number(Config::kDefaultNewPerDay)));
    m_newCardAllowance = qBound(0, allowance.toInt(), Config::kMaxNewPerDay);
    m_desiredRetention =
        qBound(Config::kMinRetention,
               m_store.setting("desired_retention", QString::number(Config::kDefaultRetention)).toDouble(),
               Config::kMaxRetention);
    m_dayStartHour = qBound(0, m_store.setting("day_start_hour", QString::number(Config::kDefaultDayStartHour)).toInt(),
                            Config::kMaxDayStartHour);
    m_deckRepo.watchDirectory();
    refresh();
    return true;
}

void Backend::refresh() {
    m_deckRepo.load(&m_currentDeck);
    // A stored Deck can vanish between launches. load() picks a usable one;
    // remember that fallback so the selector and the next launch agree.
    if (m_currentDeck != m_persistedDeck)
        recordDeckSelection(m_currentDeck);
    m_planner.setDecks(m_deckRepo.deckData());
    m_planner.configure(m_newCardAllowance, m_dayStartHour);
    QString storeError;
    if (!m_planner.refresh(&storeError))
        m_session.reportError(tr("Cannot read scheduling state: %1").arg(storeError));

    emit decksChanged();
    emit currentDeckChanged();
    emit cardsChanged();
    m_session.updateDueCount();
    m_session.rebuild(m_currentDeck);
}

void Backend::undo() {
    if (!m_history.canUndo()) {
        m_session.setStatus(tr("Nothing to undo"));
        return;
    }
    QString error;
    QString description;
    if (!m_deckRepo.undo(&error, &description))
        return;
    refresh();
    m_session.setStatus(tr("Undid %1").arg(description));
}

void Backend::redo() {
    if (!m_history.canRedo()) {
        m_session.setStatus(tr("Nothing to redo"));
        return;
    }
    QString error;
    QString description;
    if (!m_deckRepo.redo(&error, &description))
        return;
    refresh();
    m_session.setStatus(tr("Redid %1").arg(description));
}

void Backend::setCardFilter(const QString &filter) {
    if (m_cardFilter == filter)
        return;
    m_cardFilter = filter;
    emit cardFilterChanged();
    emit cardsChanged();
}

void Backend::setNewCardAllowance(int count) {
    const int clean = qBound(0, count, Config::kMaxNewPerDay);
    if (m_newCardAllowance == clean)
        return;
    m_newCardAllowance = clean;
    m_store.setSetting(QStringLiteral("new_card_allowance"), QString::number(clean));
    // Mirror the old key until every consumer reads the canonical one.
    m_store.setSetting(QStringLiteral("new_per_day"), QString::number(clean));
    if (!m_store.lastError().isEmpty())
        m_session.reportError(tr("Cannot save settings: %1").arg(m_store.lastError()));
    m_planner.configure(clean, m_dayStartHour);
    QString storeError;
    m_planner.updateNewBudget(&storeError);
    if (!storeError.isEmpty())
        m_session.reportError(tr("Cannot read scheduling state: %1").arg(storeError));
    emit settingsChanged();
    m_session.rebuild(m_currentDeck);
}

void Backend::setDesiredRetention(double retention) {
    const double clean = qBound(Config::kMinRetention, retention, Config::kMaxRetention);
    if (qFuzzyCompare(m_desiredRetention, clean))
        return;
    m_desiredRetention = clean;
    m_store.setSetting(QStringLiteral("desired_retention"), QString::number(clean, 'f', 2));
    if (!m_store.lastError().isEmpty())
        m_session.reportError(tr("Cannot save settings: %1").arg(m_store.lastError()));
    emit settingsChanged();
}

void Backend::setDayStartHour(int hour) {
    const int clean = qBound(0, hour, Config::kMaxDayStartHour);
    if (m_dayStartHour == clean)
        return;
    m_dayStartHour = clean;
    m_store.setSetting(QStringLiteral("day_start_hour"), QString::number(clean));
    if (!m_store.lastError().isEmpty())
        m_session.reportError(tr("Cannot save settings: %1").arg(m_store.lastError()));
    emit settingsChanged();
    // The rollover changes which cards are due and which reviews count toward
    // today, so rebuild the derived counts and the current card as well.
    refresh();
    emit cardsChanged();
}

QString Backend::mathEngine() const {
    switch (m_session.mathEngine()) {
    case MathRenderer::Engine::LatexSvg:
        return tr("LaTeX (SVG)");
    case MathRenderer::Engine::Unicode:
    case MathRenderer::Engine::None:
        return tr("Unicode fallback");
    }
    return tr("Unicode fallback");
}

void Backend::recordDeckSelection(const QString &name) {
    if (name.isEmpty())
        return;
    // A strictly increasing stamp keeps selector order stable even when a
    // learner or an agent selects several decks within the same millisecond.
    qint64 stamp = QDateTime::currentMSecsSinceEpoch();
    for (qint64 previous : m_lastStudied)
        stamp = qMax(stamp, previous + 1);
    m_lastStudied.insert(name, stamp);
    m_store.clearError();
    const bool recencySaved = m_store.setDeckLastStudied(name, stamp);
    const bool currentSaved = m_store.setCurrentDeck(name);
    m_persistedDeck = name;
    if (!recencySaved || !currentSaved || !m_store.lastError().isEmpty())
        m_session.reportError(tr("Cannot save settings: %1").arg(m_store.lastError()));
}

void Backend::selectDeck(const QString &name) {
    if (name.isEmpty() || name == m_currentDeck || !m_deckRepo.deckByName(name))
        return;
    m_currentDeck = name;
    recordDeckSelection(name);
    emit currentDeckChanged();
    emit cardsChanged();
    emit decksChanged();
    m_session.startFresh(m_currentDeck);
}

QVariantList Backend::deckSelector() const {
    const QVariantList summaries = deckSummaries();
    QVariantList result;
    for (const QVariant &entry : summaries) {
        if (entry.toMap().value("name").toString() == m_currentDeck)
            result << entry;
    }
    QVector<QPair<qint64, int>> studied;
    for (int i = 0; i < summaries.size(); ++i) {
        const QString name = summaries.at(i).toMap().value("name").toString();
        if (name != m_currentDeck && m_lastStudied.contains(name))
            studied << qMakePair(m_lastStudied.value(name), i);
    }
    std::stable_sort(studied.begin(), studied.end(),
                     [](const QPair<qint64, int> &a, const QPair<qint64, int> &b) { return a.first > b.first; });
    for (const auto &entry : studied)
        result << summaries.at(entry.second);
    for (int i = 0; i < summaries.size(); ++i) {
        const QString name = summaries.at(i).toMap().value("name").toString();
        if (name != m_currentDeck && !m_lastStudied.contains(name))
            result << summaries.at(i);
    }
    return result;
}

void Backend::setMode(const QString &mode) {
    // Compare the raw argument, not the parsed enum: noncanonical text still
    // reaches the session boundary and resets the sitting.
    if (mode == Domain::toString(m_mode))
        return;
    m_mode = Domain::modeFromString(mode);
    m_session.setMode(mode);
    m_session.rebuild(m_currentDeck);
}

void Backend::reveal() {
    m_session.reveal();
}

void Backend::rate(int rating) {
    const Rating parsed = Domain::ratingFromInt(rating);
    if (parsed == Rating::Invalid)
        return;
    if (!m_session.rate(parsed, m_desiredRetention))
        return;
    emit cardsChanged();
    emit decksChanged();
}

void Backend::showNext() {
    m_session.showNext();
}

void Backend::newDeck(const QString &name) {
    QString error;
    if (!createDeck(name, &error))
        m_session.reportError(error);
}

bool Backend::createDeck(const QString &name, QString *error, const QString &description) {
    if (!m_deckRepo.createDeck(name, &m_currentDeck, error, description))
        return false;
    refresh();
    selectDeck(m_currentDeck);
    return true;
}

bool Backend::setDeckDescription(const QString &deckName, const QString &description, QString *error) {
    const QString target = deckName.isEmpty() ? m_currentDeck : deckName;
    const Deck *deck = m_deckRepo.deckByName(target);
    if (deck && deck->description == description.trimmed())
        return true;
    if (!m_deckRepo.setDeckDescription(target, description, error))
        return false;
    refresh();
    return true;
}

bool Backend::createCards(const QString &deckName, const QVector<CardDraft> &drafts, const QString *description,
                          QStringList *createdIds, QString *error) {
    if (!m_deckRepo.createCards(deckName, &m_currentDeck, drafts, description, createdIds, error))
        return false;
    refresh();
    selectDeck(m_currentDeck);
    return true;
}

bool Backend::createCard(const QString &deckName, const QString &question, const QString &answer, const QString &type,
                         const QString &tags, QString *error) {
    if (!m_deckRepo.createCard(deckName, &m_currentDeck, question, answer, type, tags, error))
        return false;
    refresh();
    return true;
}

bool Backend::modifyCard(const QString &deckName, const QString &cardId, const QString &question, const QString &answer,
                         const QString &type, const QString &tags, QString *error, QString *finalId) {
    if (!m_deckRepo.modifyCard(deckName.isEmpty() ? m_currentDeck : deckName, cardId, question, answer, type, tags,
                               error, finalId))
        return false;
    refresh();
    return true;
}

bool Backend::deleteCard(const QString &deckName, const QString &cardId, QString *error) {
    if (!m_deckRepo.deleteCard(deckName.isEmpty() ? m_currentDeck : deckName, cardId, error))
        return false;
    refresh();
    return true;
}

bool Backend::removeDeck(const QString &name, QString *error) {
    if (!m_deckRepo.removeDeck(name, &m_currentDeck, error))
        return false;
    refresh();
    return true;
}

bool Backend::renameDeck(const QString &oldName, const QString &newName, DeckRename::Result *result, QString *error) {
    if (!m_deckRepo.renameDeck(oldName, newName, &m_currentDeck, result, error))
        return false;
    refresh();
    return true;
}

void Backend::addCard(const QString &question, const QString &answer) {
    QString error;
    if (!createCard(m_currentDeck, question, answer, Domain::toString(CardType::Basic), {}, &error))
        m_session.reportError(error);
    else
        m_session.setStatus(tr("Card added"));
}

void Backend::addCardDetails(const QString &question, const QString &answer, const QString &type, const QString &tags) {
    QString error;
    if (!createCard(m_currentDeck, question, answer, type, tags, &error))
        m_session.reportError(error);
    else
        m_session.setStatus(tr("Card added"));
}

void Backend::updateCard(const QString &cardId, const QString &question, const QString &answer, const QString &type,
                         const QString &tags) {
    QString error;
    if (!modifyCard(m_currentDeck, cardId, question, answer, type, tags, &error))
        m_session.reportError(error);
    else
        m_session.setStatus(tr("Card updated"));
}

void Backend::removeCurrentCard() {
    const QString cardId = m_session.currentCardId();
    if (cardId.isEmpty())
        return;
    removeCard(cardId);
}

void Backend::removeCard(const QString &cardId) {
    QString error;
    if (!deleteCard(m_currentDeck, cardId, &error))
        m_session.reportError(error);
    else
        m_session.setStatus(tr("Card removed"));
}

void Backend::deleteDeck(const QString &name) {
    QString error;
    if (!removeDeck(name, &error))
        m_session.reportError(error);
    else
        m_session.setStatus(tr("Deck removed"));
}

void Backend::deleteCurrentDeck() {
    if (m_currentDeck.isEmpty())
        return;
    deleteDeck(m_currentDeck);
}

void Backend::importFrom(const QString &path, const QString &deckName) {
    QString error;
    if (!importFile(path, deckName.isEmpty() ? m_currentDeck : deckName, &error))
        m_session.reportError(error);
    else
        m_session.setStatus(tr("Imported %1").arg(QFileInfo(path).fileName()));
}

bool Backend::importCsv(const QString &path, const QString &deckName, QString *error) {
    if (!m_deckRepo.importCsv(path, deckName, &m_currentDeck, error))
        return false;
    refresh();
    selectDeck(m_currentDeck);
    return true;
}

bool Backend::importApkg(const QString &path, const QString &deckName, QString *error) {
    if (!m_deckRepo.importApkg(path, deckName, &m_currentDeck, error))
        return false;
    refresh();
    selectDeck(m_currentDeck);
    return true;
}

bool Backend::importFile(const QString &path, const QString &deckName, QString *error) {
    if (!m_deckRepo.importFile(path, deckName, &m_currentDeck, error))
        return false;
    refresh();
    selectDeck(m_currentDeck);
    return true;
}

void Backend::editDeckFile() {
    const QString path = m_deckRepo.deckPath(m_currentDeck);
    if (!path.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void Backend::requestShow() {
    emit showWindowRequested();
}

QString Backend::statsText() const {
    return m_session.statsText();
}
