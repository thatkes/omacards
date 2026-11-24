// Math follows the plan: real LaTeX when a renderer exists, Unicode otherwise.
// TeX is untrusted card content, so shell escape is always disabled.
#include "mathrender.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QProcess>
#include <QRunnable>
#include <QRectF>
#include <QRegularExpression>
#include <QSet>
#include <QSize>
#include <QStandardPaths>

namespace {
QString escapePlain(const QString &text) {
    return text.toHtmlEscaped().replace('\n', "<br>");
}

const QRegularExpression &mathPattern() {
    static const QRegularExpression pattern(R"re(\\\((.*?)\\\)|\$\$(.*?)\$\$)re",
                                            QRegularExpression::DotMatchesEverythingOption);
    return pattern;
}

QString expressionAt(const QRegularExpressionMatch &match) {
    return match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
}

template <typename Replacement> QString mapMath(const QString &text, Replacement replacement, bool escape) {
    QString out;
    int last = 0;
    auto it = mathPattern().globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString prefix = text.mid(last, match.capturedStart() - last);
        out += escape ? escapePlain(prefix) : prefix;
        out += replacement(expressionAt(match));
        last = match.capturedEnd();
    }
    const QString tail = text.mid(last);
    out += escape ? escapePlain(tail) : tail;
    return out;
}

const QHash<QString, QString> &mathSymbols() {
    static const QHash<QString, QString> symbols{
        {"alpha", "α"},     {"beta", "β"},       {"gamma", "γ"},
        {"delta", "δ"},     {"epsilon", "ε"},    {"theta", "θ"},
        {"lambda", "λ"},    {"mu", "μ"},         {"pi", "π"},
        {"sigma", "σ"},     {"tau", "τ"},        {"phi", "φ"},
        {"omega", "ω"},     {"Gamma", "Γ"},      {"Delta", "Δ"},
        {"Theta", "Θ"},     {"Lambda", "Λ"},     {"Pi", "Π"},
        {"Sigma", "Σ"},     {"Omega", "Ω"},      {"times", "×"},
        {"cdot", "·"},      {"pm", "±"},         {"mp", "∓"},
        {"leq", "≤"},       {"geq", "≥"},        {"le", "≤"},
        {"ge", "≥"},        {"neq", "≠"},        {"ne", "≠"},
        {"approx", "≈"},    {"infty", "∞"},      {"sum", "∑"},
        {"prod", "∏"},      {"int", "∫"},        {"partial", "∂"},
        {"nabla", "∇"},     {"to", "→"},         {"rightarrow", "→"},
        {"leftarrow", "←"}, {"Rightarrow", "⇒"}, {"Leftrightarrow", "⇔"},
        {"in", "∈"},        {"notin", "∉"},      {"subset", "⊂"},
        {"supset", "⊃"},    {"cup", "∪"},        {"cap", "∩"},
        {"emptyset", "∅"},  {"forall", "∀"},     {"exists", "∃"},
        {"neg", "¬"},       {"land", "∧"},       {"lor", "∨"},
        {"ldots", "…"},     {"dots", "…"},       {"cdots", "⋯"},
        {"quad", " "},      {"qquad", "  "},     {"sin", "sin"},
        {"cos", "cos"},     {"tan", "tan"},      {"log", "log"},
        {"ln", "ln"},       {"lim", "lim"},      {"min", "min"},
        {"max", "max"},     {"sqrt", "√"},
    };
    return symbols;
}

const QStringList &structuredCommands() {
    static const QStringList commands{
        "frac", "dfrac", "tfrac", "binom", "sqrt", "text", "mathrm", "mathbf", "displaystyle", "left", "right",
    };
    return commands;
}

struct BraceGroup {
    QString content;
    int end = -1; // index just after the closing brace
};

BraceGroup readGroup(const QString &text, int open) {
    BraceGroup group;
    if (open < 0 || open >= text.size() || text.at(open) != '{')
        return group;
    int depth = 0;
    for (int i = open; i < text.size(); ++i) {
        if (text.at(i) == '{') {
            ++depth;
        } else if (text.at(i) == '}') {
            if (--depth == 0) {
                group.content = text.mid(open + 1, i - open - 1);
                group.end = i + 1;
                return group;
            }
        }
    }
    group.content = text.mid(open + 1);
    group.end = text.size();
    return group;
}

QStringList expressionsIn(const QString &text) {
    QStringList result;
    auto matches = mathPattern().globalMatch(text);
    while (matches.hasNext()) {
        const QString expression = expressionAt(matches.next());
        if (!result.contains(expression))
            result << expression;
    }
    return result;
}

QString mathPlaceholderTag(const QString &key, const QSize &size) {
    // An inline image is the same rich-text layout primitive as the finished
    // SVG, so a transparent pixel scaled to the reserved box holds the slot
    // without shifting the text around it.
    return MathRenderer::mathPlaceholderMarker(key) +
           QStringLiteral("<span><img src='data:image/png;base64,"
                          "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGNgAAIAAAUAAXpeqz8AAAAASUVORK5CYII="
                          "' width='%1' height='%2' align='middle' style='vertical-align: middle;'></span>")
               .arg(size.width())
               .arg(size.height());
}

const QRegularExpression &blackFillPattern();
QString normalizeInk(const QString &ink);

// One render per cache key, shared by live presentation, prefetching and the
// warming command. A second caller skips the key instead of waiting for the
// toolchain that is already running.
QMutex &renderMutex() {
    static QMutex mutex;
    return mutex;
}

QSet<QString> &rendersInFlight() {
    static QSet<QString> keys;
    return keys;
}

bool claimRender(const QString &key) {
    QMutexLocker lock(&renderMutex());
    if (rendersInFlight().contains(key))
        return false;
    rendersInFlight().insert(key);
    return true;
}

void releaseRender(const QString &key) {
    QMutexLocker lock(&renderMutex());
    rendersInFlight().remove(key);
}

struct RenderClaim {
    QString key;
    bool held = false;

    ~RenderClaim() {
        if (held)
            releaseRender(key);
    }
};
} // namespace

// Runs the toolchain off the UI thread and posts the finished markup back to
// the renderer's own thread. An empty result means the render failed; it still
// has to release the key so later expressions can use the worker slot.
class MathRenderRunnable : public QRunnable {
public:
    MathRenderRunnable(QPointer<MathRenderer> owner, const MathRenderer::RenderRequest &request, quint64 generation)
        : m_owner(owner), m_request(request), m_generation(generation) {}

    void run() override {
        MathRenderer *owner = m_owner.data();
        if (!owner)
            return;
        const QString path = owner->renderRequest(m_request);
        const QString html = path.isEmpty() ? QString()
                                            : owner->mathImageHtml(path, m_request.expression, m_request.ink,
                                                                   m_request.scale, m_request.reservedSize);
        QMetaObject::invokeMethod(
            owner,
            [owner, key = m_request.key, html, generation = m_generation] {
                owner->finishRender(key, html, generation);
            },
            Qt::QueuedConnection);
    }

private:
    QPointer<MathRenderer> m_owner;
    MathRenderer::RenderRequest m_request;
    quint64 m_generation;
};

MathRenderer::MathRenderer(const QString &cacheDirectory, QObject *parent)
    : QObject(parent),
      m_cache(cacheDirectory.isEmpty()
                  ? QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omacards/math"
                  : cacheDirectory) {
    m_pool.setMaxThreadCount(2);
}

MathRenderer::~MathRenderer() {
    m_pool.clear();
    m_pool.waitForDone();
}

MathRenderer::Engine MathRenderer::detect() const {
    // Workers can detect while the UI thread is still presenting a Flashcard.
    QMutexLocker lock(&m_detectMutex);
    if (m_engine != Engine::None)
        return m_engine;
    QDir().mkpath(m_cache);
    const auto find = [](const char *name) { return QStandardPaths::findExecutable(QString::fromLatin1(name)); };
    const QString latex = find("tectonic");
    const QString pdflatex = find("pdflatex");
    const QString dvilatex = find("latex");
    if (!latex.isEmpty() && !find("pdftocairo").isEmpty()) {
        m_toolchain = Toolchain::Pdf;
        m_engine = Engine::LatexSvg;
    } else if (!dvilatex.isEmpty() && !find("dvisvgm").isEmpty()) {
        m_toolchain = Toolchain::Dvi;
        m_engine = Engine::LatexSvg;
    } else if (!pdflatex.isEmpty() && !find("pdftocairo").isEmpty()) {
        m_toolchain = Toolchain::Pdf;
        m_engine = Engine::LatexSvg;
    } else {
        m_toolchain = Toolchain::None;
        m_engine = Engine::Unicode;
    }
    return m_engine;
}

bool MathRenderer::hasRenderer() const {
    return detect() == Engine::LatexSvg;
}

QString MathRenderer::render(const QString &text, const QString &ink, double scale) const {
    return mapMath(
        text, [this, ink, scale](const QString &expression) { return renderExpression(expression, ink, scale); }, true);
}

QString MathRenderer::renderPlain(const QString &text) const {
    return mapMath(text, unicodeMath, false);
}

QStringList MathRenderer::warnings(const QString &text) const {
    static const QRegularExpression command(R"re(\\([A-Za-z]+))re");
    QStringList result;
    auto expressions = mathPattern().globalMatch(text);
    while (expressions.hasNext()) {
        const QString expression = expressionAt(expressions.next());
        auto commands = command.globalMatch(expression);
        while (commands.hasNext()) {
            const QString name = commands.next().captured(1);
            if (!mathSymbols().contains(name) && !structuredCommands().contains(name)) {
                const QString message = QObject::tr("Unsupported math command: \\%1").arg(name);
                if (!result.contains(message))
                    result << message;
            }
        }
    }
    return result;
}

QString MathRenderer::mathSvgPath(const QString &expression) const {
    // One neutral master per expression. The prefix invalidates the old
    // per-theme files without deleting them; they are simply never read.
    const QString base =
        "eq5-" +
        QString::fromLatin1(QCryptographicHash::hash(expression.toUtf8(), QCryptographicHash::Sha256).toHex().left(20));
    return m_cache + "/" + base + ".svg";
}

QString MathRenderer::cachedSvgPath(const QString &expression) const {
    const QString path = mathSvgPath(expression);
    return QFileInfo::exists(path) ? path : QString();
}

int MathRenderer::warm(const QString &text, int *cached) {
    int rendered = 0;
    if (cached)
        *cached = 0;
    QSet<QString> seen;
    for (const QString &expression : expressionsIn(text)) {
        const QString path = mathSvgPath(expression);
        if (seen.contains(path))
            continue;
        seen.insert(path);
        if (QFileInfo::exists(path)) {
            if (cached)
                ++*cached;
            continue;
        }
        if (renderExpression(expression).startsWith("<img "))
            ++rendered;
    }
    return rendered;
}

QString MathRenderer::mathPlaceholderMarker(const QString &key) {
    return QStringLiteral("<!--omacards-math:%1-->").arg(key);
}

QString MathRenderer::renderWithPlaceholders(const QString &text, const QString &ink, double scale) {
    if (!hasRenderer())
        return render(text, ink, scale);
    return mapMath(
        text,
        [this, ink, scale](const QString &expression) {
            const QString path = mathSvgPath(expression);
            const QString key = QFileInfo(path).completeBaseName();
            if (QFileInfo::exists(path)) {
                // The SVG is ready, so no worker is started here. Read its
                // bytes once: the data URI and the reserved box share them,
                // and renderLatex can skip a second existence check.
                cachedSvg(path);
                // An older worker may still be holding this key: give it the
                // ink that is about to be displayed before its result arrives.
                refreshActiveRequestInk(key, ink, scale);
                return renderExpression(expression, ink, scale);
            }
            const QSize size = reservedMathSize(expression, scale, path);
            enqueueExpression({key, expression, ink, scale, size});
            return mathPlaceholderTag(key, size);
        },
        true);
}

void MathRenderer::prefetch(const QString &text, const QString &ink, double scale) {
    if (!hasRenderer())
        return;
    // Ink no longer affects which file is rendered; it stays on the request so
    // a queued render can emit the right data URI for the Flashcard being shown.
    for (const QString &expression : expressionsIn(text)) {
        const QString path = mathSvgPath(expression);
        if (QFileInfo::exists(path))
            continue;
        enqueueExpression(
            {QFileInfo(path).completeBaseName(), expression, ink, scale, reservedMathSize(expression, scale)});
    }
}

void MathRenderer::clearPendingMath() {
    m_queue.clear();
    m_queuedKeys.clear();
    // A worker already running keeps its slot, but its markup belongs to the
    // old theme. The next request records this generation; finishRender then
    // compares it with the worker's and recolors from the neutral SVG if they
    // differ.
    ++m_generation;
}

void MathRenderer::enqueueExpression(const RenderRequest &request) {
    constexpr int kMaxQueued = 12;
    if (QFileInfo::exists(mathSvgPath(request.expression)))
        return;
    auto active = m_activeRenders.find(request.key);
    if (active != m_activeRenders.end()) {
        // The slot is taken. Remember the first request for this generation so
        // an invalidated result can be rebuilt with the ink now on screen. A
        // second caller in the same generation must not steal the style.
        if (active->generation != m_generation) {
            active->request = request;
            active->generation = m_generation;
        }
        return;
    }
    if (m_queuedKeys.contains(request.key))
        return;
    while (m_queue.size() >= kMaxQueued)
        m_queuedKeys.remove(m_queue.dequeue().key);
    m_queue.enqueue(request);
    m_queuedKeys.insert(request.key);
    startNextRender();
}

void MathRenderer::refreshActiveRequestInk(const QString &key, const QString &ink, double scale) {
    auto active = m_activeRenders.find(key);
    if (active == m_activeRenders.end())
        return;
    if (active->generation == m_generation)
        return;
    active->request.ink = ink;
    active->request.scale = scale;
    active->generation = m_generation;
}

void MathRenderer::startNextRender() {
    constexpr int kMaxActive = 2;
    while (m_activeRenders.size() < kMaxActive && !m_queue.isEmpty()) {
        const RenderRequest request = m_queue.dequeue();
        m_queuedKeys.remove(request.key);
        if (QFileInfo::exists(mathSvgPath(request.expression)))
            continue;
        if (m_activeRenders.contains(request.key))
            continue;
        m_activeRenders.insert(request.key, ActiveRender{request, m_generation});
        m_pool.start(new MathRenderRunnable(this, request, m_generation));
    }
}

QString MathRenderer::renderRequest(const RenderRequest &request) {
    return renderLatex(request.expression);
}

void MathRenderer::finishRender(const QString &key, const QString &html, quint64 generation) {
    auto active = m_activeRenders.find(key);
    if (active == m_activeRenders.end())
        return;
    const ActiveRender current = active.value();
    m_activeRenders.erase(active);
    const RenderRequest request = current.request;
    const quint64 requestGeneration = current.generation;
    QString result;
    if (requestGeneration == m_generation) {
        result = html;
        if (generation != requestGeneration) {
            // The worker's markup was built with the ink from an earlier
            // generation. Its SVG is neutral and cached by now, so rebuild the
            // data URI from the active request; no toolchain runs here.
            result = QFileInfo::exists(mathSvgPath(request.expression))
                         ? renderExpression(request.expression, request.ink, request.scale, request.reservedSize)
                         : QString();
        }
    }
    if (!result.isEmpty())
        emit mathRendered(key, result);
    startNextRender();
}

QString MathRenderer::renderExpression(const QString &expression, const QString &ink, double scale) const {
    return renderExpression(expression, ink, scale, {});
}

QString MathRenderer::renderExpression(const QString &expression, const QString &ink, double scale,
                                       const QSize &reservedSize) const {
    const QString image = renderLatex(expression);
    if (!image.isEmpty()) {
        const QString html = mathImageHtml(image, expression, ink, scale, reservedSize);
        if (!html.isEmpty())
            return html;
    }
    return QString("<span style='font-family: \"Noto Sans Math\", serif;'>%1</span>")
        .arg(unicodeMath(expression).toHtmlEscaped());
}

namespace {
// QTextDocument rasterizes an <img> at the SVG's intrinsic size, then scales
// it to the element's logical size. Publishing 2x intrinsic pixels keeps the
// glyphs crisp on HiDPI screens (and supersamples nicely on 1x displays).
constexpr int kSvgSupersample = 2;

QString normalizeInk(const QString &ink) {
    if (ink.isEmpty())
        return "#000000";
    const QColor color(ink);
    return color.isValid() ? color.name() : QStringLiteral("#000000");
}

QString svgRootTag(const QString &svg) {
    static const QRegularExpression root(R"re(<svg\b[^>]*>)re");
    const QRegularExpressionMatch match = root.match(svg);
    return match.hasMatch() ? match.captured() : QString();
}

void scaleSvgRoot(QString &svg) {
    const QString tag = svgRootTag(svg);
    if (tag.isEmpty())
        return;
    // Qt's SVG renderer treats `pt` values as pixels, so pdftocairo's pt-sized
    // root is rasterized too small and then stretched by the <img> element.
    // Publish explicit pixel dimensions once; display scale lives in HTML.
    static const QRegularExpression dimension(R"re((\b(?:width|height)=")([0-9]*\.?[0-9]+)(pt|px)("))re");
    QString scaled;
    qsizetype last = 0;
    auto matches = dimension.globalMatch(tag);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const double factor = match.captured(3) == QLatin1String("pt") ? 4.0 / 3.0 : 1.0;
        const int pixels = qRound(match.captured(2).toDouble() * factor) * kSvgSupersample;
        scaled += tag.mid(last, match.capturedStart() - last);
        scaled += match.captured(1) + QString::number(pixels) + "px" + match.captured(4);
        last = match.capturedEnd();
    }
    scaled += tag.mid(last);
    svg.replace(svg.indexOf(tag), tag.size(), scaled);
}

const QRegularExpression &blackFillPattern() {
    static const QRegularExpression black(R"re(rgb\(\s*0%\s*,\s*0%\s*,\s*0%\s*\)|#(?:000|000000)\b|\bblack\b)re",
                                          QRegularExpression::CaseInsensitiveOption);
    return black;
}

void normalizeCachedSvg(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    QString svg = QString::fromUtf8(file.readAll());
    file.close();
    scaleSvgRoot(svg);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(svg.toUtf8());
}

} // namespace

MathRenderer::CachedSvg MathRenderer::cachedSvg(const QString &path) const {
    QMutexLocker lock(&m_svgCacheMutex);
    const auto found = m_svgCache.constFind(path);
    if (found != m_svgCache.constEnd())
        return found.value();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    CachedSvg cached;
    cached.bytes = file.readAll();
    cached.hasBytes = true;
    m_svgCache.insert(path, cached);
    return cached;
}

bool MathRenderer::hasCachedSvg(const QString &path) const {
    QMutexLocker lock(&m_svgCacheMutex);
    const auto found = m_svgCache.constFind(path);
    return found != m_svgCache.constEnd() && found->hasBytes;
}

void MathRenderer::forgetSvg(const QString &path) const {
    QMutexLocker lock(&m_svgCacheMutex);
    m_svgCache.remove(path);
}

// The cached file stays a neutral master. Recoloring happens on the bytes that
// go into the rich-text <img> src, so no themed file is ever written.
QString MathRenderer::colorizedSvgUri(const QString &path, const QString &ink) const {
    const CachedSvg cached = cachedSvg(path);
    if (!cached.hasBytes)
        return {};
    QString svg = QString::fromUtf8(cached.bytes);
    svg.replace(blackFillPattern(), normalizeInk(ink));
    return QStringLiteral("data:image/svg+xml;base64,") + QString::fromLatin1(svg.toUtf8().toBase64());
}

QSize MathRenderer::svgPixelSize(const QString &path) const {
    {
        QMutexLocker lock(&m_svgCacheMutex);
        const auto found = m_svgCache.constFind(path);
        if (found != m_svgCache.constEnd() && found->hasSize)
            return found->size;
    }
    const CachedSvg cached = cachedSvg(path);
    if (!cached.hasBytes)
        return {};
    const QString tag = svgRootTag(QString::fromUtf8(cached.bytes));
    QSize size;
    if (!tag.isEmpty()) {
        const auto dimension = [&tag](const QString &name) {
            const QRegularExpression pattern("\\b" + name + R"re(="([0-9]*\.?[0-9]+)(pt|px)")re");
            const QRegularExpressionMatch match = pattern.match(tag);
            if (!match.hasMatch())
                return 0;
            const double factor = match.captured(2) == QLatin1String("pt") ? 4.0 / 3.0 : 1.0;
            return qRound(match.captured(1).toDouble() * factor / kSvgSupersample);
        };
        size = QSize(dimension("width"), dimension("height"));
    }
    QMutexLocker lock(&m_svgCacheMutex);
    auto found = m_svgCache.find(path);
    if (found != m_svgCache.end()) {
        found->size = size;
        found->hasSize = true;
    }
    return size;
}

// The reserved box is the one size a formula occupies from placeholder through
// finished image. A cached SVG already knows its logical size, so scaling that
// by `scale` keeps its aspect ratio. While the file is missing, TeX has not run
// yet: the box comes from the bounds of the Unicode fallback text in Noto Sans
// Math at 10px * scale, plus a small margin, rounded to even pixels. If the
// finished SVG reports a different logical size, the reserved box still wins;
// the SVG is drawn into that box instead of resizing the <img> and moving the
// surrounding text.
QSize MathRenderer::reservedMathSize(const QString &expression, double scale, const QString &svgPath) const {
    if (!svgPath.isEmpty()) {
        const QSize logical = svgPixelSize(svgPath);
        if (!logical.isEmpty())
            return QSize(qRound(logical.width() * scale), qRound(logical.height() * scale));
    }
    QFont font(QStringLiteral("Noto Sans Math"));
    font.setPixelSize(qMax(1, qRound(10.0 * scale)));
    const QFontMetricsF metrics(font);
    const QRectF glyphs = metrics.boundingRect(unicodeMath(expression));
    const auto evenPixels = [](qreal value) { return qMax(2, 2 * qCeil(value / 2.0)); };
    return QSize(evenPixels(glyphs.width() + 4.0), evenPixels(glyphs.height() + 4.0));
}

QString MathRenderer::mathImageHtml(const QString &path, const QString &expression, const QString &ink, double scale,
                                    const QSize &reservedSize) const {
    const QString source = colorizedSvgUri(path, ink);
    if (source.isEmpty())
        return {};
    // A queued request carries the box its placeholder reserved. Keep that box
    // even when the new SVG's logical size disagrees: Qt draws the SVG into the
    // element using its viewBox, while the surrounding layout keeps the box.
    // Renders with no placeholder (cached display, preview, warming) size from
    // the SVG, which preserves its aspect ratio.
    const QSize pixels = reservedSize.isEmpty() ? reservedMathSize(expression, scale, path) : reservedSize;
    // toHtmlEscaped() leaves apostrophes literal, which would close this
    // single-quoted attribute. Qt then loses the width and height that follow.
    const QString alt = expression.toHtmlEscaped().replace(QLatin1Char('\''), QStringLiteral("&#39;"));
    return QStringLiteral("<img src='%1' alt='%2' width='%3' height='%4' align='middle' "
                          "style='vertical-align: middle;'>")
        .arg(source, alt)
        .arg(pixels.width())
        .arg(pixels.height());
}

QString MathRenderer::renderLatex(const QString &expression) const {
    if (detect() != Engine::LatexSvg)
        return {};

    const QString svgPath = mathSvgPath(expression);
    if (hasCachedSvg(svgPath) || QFileInfo::exists(svgPath))
        return svgPath;

    RenderClaim claim{svgPath, false};
    if (!claimRender(svgPath))
        return {};
    claim.held = true;

    const QString base = QFileInfo(svgPath).completeBaseName();
    const QString texDir = m_cache + "/tex";
    QDir().mkpath(texDir);
    QFile::remove(svgPath);
    forgetSvg(svgPath);
    const QString stem = texDir + "/" + base;
    // Typeset directly into a page sized to the expression. Shipout bypasses
    // the article output routine, so pdftocairo gets a tight page instead of a
    // letter-sized one with the formula lost in the top-left corner.
    const QString tex =
        QString("\\documentclass[12pt]{article}\n"
                "\\usepackage{amsmath,amssymb}\n"
                "\\pagestyle{empty}\n"
                "\\begin{document}\n"
                "\\hoffset=-1in \\voffset=-1in\n"
                "\\setbox0=\\hbox{$\\displaystyle %1$}\n"
                "\\pdfpagewidth=\\wd0 \\advance\\pdfpagewidth by 8pt\n"
                "\\pdfpageheight=\\ht0 \\advance\\pdfpageheight by \\dp0 \\advance\\pdfpageheight by 8pt\n"
                "\\shipout\\vbox{\\kern4pt\\hbox{\\kern4pt\\box0\\kern4pt}}\n"
                "\\end{document}\n")
            .arg(expression);
    QFile file(stem + ".tex");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    file.write(tex.toUtf8());
    file.close();

    constexpr int localTimeoutMs = 5000;
    constexpr int downloadTimeoutMs = 120000; // Tectonic fetches its TeX bundle on first use.
    auto run = [](const QString &program, const QStringList &arguments, int timeoutMs) {
        QProcess process;
        process.setWorkingDirectory(QFileInfo(arguments.last()).absolutePath());
        process.start(program, arguments);
        return process.waitForFinished(timeoutMs) && process.exitCode() == 0;
    };

    bool ok = false;
    if (m_toolchain == Toolchain::Pdf) {
        const QString tectonic = QStandardPaths::findExecutable("tectonic");
        const QString pdf = stem + ".pdf";
        const QString outputDirectory = QFileInfo(stem).absolutePath();
        QFile::remove(pdf);
        if (!tectonic.isEmpty()) {
            // Tectonic uses --untrusted (not pdflatex's -no-shell-escape) to disable
            // shell escape. Prefer its cached-only mode so normal renders stay fast;
            // fall back to one download-enabled pass when the bundle is missing.
            ok = run(tectonic, {"--untrusted", "--only-cached", "--outdir=" + outputDirectory, stem + ".tex"},
                     localTimeoutMs) &&
                 QFileInfo::exists(pdf);
            if (!ok) {
                QFile::remove(pdf);
                ok = run(tectonic, {"--untrusted", "--outdir=" + outputDirectory, stem + ".tex"}, downloadTimeoutMs) &&
                     QFileInfo::exists(pdf);
            }
        } else {
            ok = run(QStandardPaths::findExecutable("pdflatex"),
                     {"-interaction=nonstopmode", "-no-shell-escape", stem + ".tex"}, localTimeoutMs) &&
                 QFileInfo::exists(pdf);
        }
        ok = ok && run(QStandardPaths::findExecutable("pdftocairo"), {"-svg", pdf, svgPath}, localTimeoutMs);
    } else {
        const QString dvi = stem + ".dvi";
        QFile::remove(dvi);
        ok = run(QStandardPaths::findExecutable("latex"),
                 {"-interaction=nonstopmode", "-no-shell-escape", "-output-directory=" + QFileInfo(stem).absolutePath(),
                  stem + ".tex"},
                 localTimeoutMs) &&
             QFileInfo::exists(dvi) &&
             run(QStandardPaths::findExecutable("dvisvgm"), {"--no-fonts", "-o", svgPath, dvi}, localTimeoutMs);
    }
    if (ok && QFileInfo::exists(svgPath)) {
        normalizeCachedSvg(svgPath);
        // A display during the render may have cached the converter's bytes
        // before the root was scaled. The next read must see the final master.
        forgetSvg(svgPath);
        return svgPath;
    }
    return {};
}

QString unicodeMath(const QString &expression) {
    const QString text = expression.trimmed();
    QString out;
    for (int i = 0; i < text.size();) {
        const QChar ch = text.at(i);
        if (ch == '\\') {
            int after = i + 1;
            while (after < text.size() && text.at(after).isLetter())
                ++after;
            const QString name = text.mid(i + 1, after - i - 1);
            if (name.isEmpty()) {
                i = qMin(i + 2, text.size());
                continue;
            }
            if (name == QLatin1String("frac") || name == QLatin1String("dfrac") || name == QLatin1String("tfrac")) {
                const BraceGroup numerator = readGroup(text, after);
                if (numerator.end > 0) {
                    const BraceGroup denominator = readGroup(text, numerator.end);
                    if (denominator.end > 0) {
                        out += "(" + unicodeMath(numerator.content) + ")/(" + unicodeMath(denominator.content) + ")";
                        i = denominator.end;
                        continue;
                    }
                }
            } else if (name == QLatin1String("binom")) {
                const BraceGroup upper = readGroup(text, after);
                if (upper.end > 0) {
                    const BraceGroup lower = readGroup(text, upper.end);
                    if (lower.end > 0) {
                        out += "C(" + unicodeMath(upper.content) + "," + unicodeMath(lower.content) + ")";
                        i = lower.end;
                        continue;
                    }
                }
            } else if (name == QLatin1String("sqrt")) {
                const BraceGroup radicand = readGroup(text, after);
                if (radicand.end > 0) {
                    out += "√(" + unicodeMath(radicand.content) + ")";
                    i = radicand.end;
                    continue;
                }
            } else if (name == QLatin1String("text") || name == QLatin1String("mathrm") ||
                       name == QLatin1String("mathbf") || name == QLatin1String("displaystyle")) {
                const BraceGroup literal = readGroup(text, after);
                if (literal.end > 0) {
                    out += literal.content;
                    i = literal.end;
                    continue;
                }
                i = after;
                continue;
            } else if (name == QLatin1String("left") || name == QLatin1String("right")) {
                i = after;
                continue;
            }
            out += mathSymbols().value(name, name);
            i = after;
            continue;
        }
        if (ch == '^' || ch == '_') {
            if (i + 1 < text.size() && text.at(i + 1) == '{') {
                const BraceGroup group = readGroup(text, i + 1);
                const QString content = unicodeMath(group.content);
                const bool compact = content.size() == 1 || (content.startsWith('(') && content.endsWith(')'));
                out += ch;
                out += compact ? content : QStringLiteral("(%1)").arg(content);
                i = group.end > 0 ? group.end : i + 2;
                continue;
            }
            out += ch;
            ++i;
            continue;
        }
        if (ch == '{') {
            const BraceGroup group = readGroup(text, i);
            out += unicodeMath(group.content);
            i = group.end > 0 ? group.end : i + 1;
            continue;
        }
        if (ch == '}') {
            ++i;
            continue;
        }
        out += ch;
        ++i;
    }
    out.replace("^2", "²").replace("^3", "³").replace("_0", "₀").replace("_1", "₁");
    out.replace("  ", " ");
    return out.trimmed();
}
