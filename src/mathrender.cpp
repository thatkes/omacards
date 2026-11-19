// Math follows the plan: real LaTeX when a renderer exists, Unicode otherwise.
// TeX is untrusted card content, so shell escape is always disabled.
#include "mathrender.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSize>
#include <QStandardPaths>
#include <QUrl>

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

template <typename Replacement>
QString mapMath(const QString &text, Replacement replacement, bool escape) {
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
        {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ε"},
        {"theta", "θ"}, {"lambda", "λ"}, {"mu", "μ"}, {"pi", "π"}, {"sigma", "σ"},
        {"tau", "τ"}, {"phi", "φ"}, {"omega", "ω"}, {"Gamma", "Γ"}, {"Delta", "Δ"},
        {"Theta", "Θ"}, {"Lambda", "Λ"}, {"Pi", "Π"}, {"Sigma", "Σ"}, {"Omega", "Ω"},
        {"times", "×"}, {"cdot", "·"}, {"pm", "±"}, {"mp", "∓"}, {"leq", "≤"},
        {"geq", "≥"}, {"le", "≤"}, {"ge", "≥"}, {"neq", "≠"}, {"ne", "≠"},
        {"approx", "≈"}, {"infty", "∞"}, {"sum", "∑"}, {"prod", "∏"}, {"int", "∫"},
        {"partial", "∂"}, {"nabla", "∇"}, {"to", "→"}, {"rightarrow", "→"},
        {"leftarrow", "←"}, {"Rightarrow", "⇒"}, {"Leftrightarrow", "⇔"},
        {"in", "∈"}, {"notin", "∉"}, {"subset", "⊂"}, {"supset", "⊃"}, {"cup", "∪"},
        {"cap", "∩"}, {"emptyset", "∅"}, {"forall", "∀"}, {"exists", "∃"}, {"neg", "¬"},
        {"land", "∧"}, {"lor", "∨"}, {"ldots", "…"}, {"dots", "…"}, {"cdots", "⋯"},
        {"quad", " "}, {"qquad", "  "}, {"sin", "sin"}, {"cos", "cos"}, {"tan", "tan"},
        {"log", "log"}, {"ln", "ln"}, {"lim", "lim"}, {"min", "min"}, {"max", "max"},
        {"sqrt", "√"},
    };
    return symbols;
}

const QStringList &structuredCommands() {
    static const QStringList commands{
        "frac", "dfrac", "tfrac", "binom", "sqrt", "text", "mathrm", "mathbf",
        "displaystyle", "left", "right",
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
} // namespace

MathRenderer::MathRenderer(const QString &cacheDirectory, QObject *parent)
    : QObject(parent),
      m_cache(cacheDirectory.isEmpty()
                  ? QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omacards/math"
                  : cacheDirectory) {
}

void MathRenderer::detect() const {
    if (m_checked)
        return;
    m_checked = true;
    QDir().mkpath(m_cache);
    const auto find = [](const char *name) { return QStandardPaths::findExecutable(QString::fromLatin1(name)); };
    const QString latex = find("tectonic");
    const QString pdflatex = find("pdflatex");
    const QString dvilatex = find("latex");
    if ((!latex.isEmpty() && !find("pdftocairo").isEmpty())) {
        m_converter = "pdf";
        m_available = true;
    } else if (!dvilatex.isEmpty() && !find("dvisvgm").isEmpty()) {
        m_converter = "dvi";
        m_available = true;
    } else if (!pdflatex.isEmpty() && !find("pdftocairo").isEmpty()) {
        m_converter = "pdf";
        m_available = true;
    }
}

bool MathRenderer::hasRenderer() const {
    detect();
    return m_available;
}

QString MathRenderer::render(const QString &text, const QString &ink, double scale) const {
    return mapMath(text, [this, ink, scale](const QString &expression) {
        return renderExpression(expression, ink, scale);
    }, true);
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

namespace {
QSize svgPixelSize(const QString &path);
}

QString MathRenderer::renderExpression(const QString &expression, const QString &ink, double scale) const {
    const QString image = renderLatex(expression, ink, scale);
    if (!image.isEmpty()) {
        // QML's rich-text engine sizes <img> from these attributes; the SVG's
        // intrinsic size is scaled too, so the vector stays sharp either way.
        const QSize pixels = svgPixelSize(image);
        return QString("<img src='%1' alt='%2' width='%3' height='%4' align='middle' style='vertical-align: middle;'>")
            .arg(QUrl::fromLocalFile(image).toString(), expression.toHtmlEscaped())
            .arg(pixels.width())
            .arg(pixels.height());
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

void scaleSvgRoot(QString &svg, double scale) {
    const QString tag = svgRootTag(svg);
    if (tag.isEmpty())
        return;
    // Qt's SVG renderer treats `pt` values as pixels, so pdftocairo's pt-sized
    // root is rasterized too small and then stretched by the <img> element.
    // Publish explicit pixel dimensions that match the rich-text image size.
    static const QRegularExpression dimension(R"re((\b(?:width|height)=")([0-9]*\.?[0-9]+)(pt|px)("))re");
    QString scaled;
    qsizetype last = 0;
    auto matches = dimension.globalMatch(tag);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const double factor = match.captured(3) == QLatin1String("pt") ? 4.0 / 3.0 : 1.0;
        const int pixels = qRound(match.captured(2).toDouble() * scale * factor) * kSvgSupersample;
        scaled += tag.mid(last, match.capturedStart() - last);
        scaled += match.captured(1) + QString::number(pixels) + "px" + match.captured(4);
        last = match.capturedEnd();
    }
    scaled += tag.mid(last);
    svg.replace(svg.indexOf(tag), tag.size(), scaled);
}

void colorizeSvg(const QString &path, const QString &ink, double scale) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    QString svg = QString::fromUtf8(file.readAll());
    file.close();
    static const QRegularExpression black(
        R"re(rgb\(\s*0%\s*,\s*0%\s*,\s*0%\s*\)|#(?:000|000000)\b|\bblack\b)re",
        QRegularExpression::CaseInsensitiveOption);
    svg.replace(black, ink);
    scaleSvgRoot(svg, scale);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(svg.toUtf8());
}

QSize svgPixelSize(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString tag = svgRootTag(QString::fromUtf8(file.readAll()));
    if (tag.isEmpty())
        return {};
    const auto dimension = [&tag](const QString &name) {
        const QRegularExpression pattern("\\b" + name + R"re(="([0-9]*\.?[0-9]+)(pt|px)")re");
        const QRegularExpressionMatch match = pattern.match(tag);
        if (!match.hasMatch())
            return 0;
        const double factor = match.captured(2) == QLatin1String("pt") ? 4.0 / 3.0 : 1.0;
        return qRound(match.captured(1).toDouble() * factor / kSvgSupersample);
    };
    return QSize(dimension("width"), dimension("height"));
}
} // namespace

QString MathRenderer::renderLatex(const QString &expression, const QString &ink, double scale) const {
    detect();
    if (!m_available)
        return {};

    // Ink and scale are part of the key so question/answer colors and label
    // sizes never share a cached file. The prefix invalidates SVGs from the
    // old unscaled/full-page renderers.
    const QString key = expression + "\n" + normalizeInk(ink) + "\n" + QString::number(scale, 'f', 3);
    const QString base = "eq4-" + QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(20));
    const QString svgPath = m_cache + "/" + base + ".svg";
    if (QFileInfo::exists(svgPath))
        return svgPath;

    const QString texDir = m_cache + "/tex";
    QDir().mkpath(texDir);
    QFile::remove(svgPath);
    const QString stem = texDir + "/" + base;
    // Typeset directly into a page sized to the expression. Shipout bypasses
    // the article output routine, so pdftocairo gets a tight page instead of a
    // letter-sized one with the formula lost in the top-left corner.
    const QString tex = QString(
        "\\documentclass[12pt]{article}\n"
        "\\usepackage{amsmath,amssymb}\n"
        "\\pagestyle{empty}\n"
        "\\begin{document}\n"
        "\\hoffset=-1in \\voffset=-1in\n"
        "\\setbox0=\\hbox{$\\displaystyle %1$}\n"
        "\\pdfpagewidth=\\wd0 \\advance\\pdfpagewidth by 8pt\n"
        "\\pdfpageheight=\\ht0 \\advance\\pdfpageheight by \\dp0 \\advance\\pdfpageheight by 8pt\n"
        "\\shipout\\vbox{\\kern4pt\\hbox{\\kern4pt\\box0\\kern4pt}}\n"
        "\\end{document}\n").arg(expression);
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
    if (m_converter == "pdf") {
        const QString tectonic = QStandardPaths::findExecutable("tectonic");
        const QString pdf = stem + ".pdf";
        const QString outputDirectory = QFileInfo(stem).absolutePath();
        QFile::remove(pdf);
        if (!tectonic.isEmpty()) {
            // Tectonic uses --untrusted (not pdflatex's -no-shell-escape) to disable
            // shell escape. Prefer its cached-only mode so normal renders stay fast;
            // fall back to one download-enabled pass when the bundle is missing.
            ok = run(tectonic, {"--untrusted", "--only-cached", "--outdir=" + outputDirectory, stem + ".tex"},
                     localTimeoutMs)
                 && QFileInfo::exists(pdf);
            if (!ok) {
                QFile::remove(pdf);
                ok = run(tectonic, {"--untrusted", "--outdir=" + outputDirectory, stem + ".tex"}, downloadTimeoutMs)
                     && QFileInfo::exists(pdf);
            }
        } else {
            ok = run(QStandardPaths::findExecutable("pdflatex"),
                     {"-interaction=nonstopmode", "-no-shell-escape", stem + ".tex"}, localTimeoutMs)
                 && QFileInfo::exists(pdf);
        }
        ok = ok && run(QStandardPaths::findExecutable("pdftocairo"), {"-svg", pdf, svgPath}, localTimeoutMs);
    } else {
        const QString dvi = stem + ".dvi";
        QFile::remove(dvi);
        ok = run(QStandardPaths::findExecutable("latex"),
                 {"-interaction=nonstopmode", "-no-shell-escape", "-output-directory=" + QFileInfo(stem).absolutePath(),
                  stem + ".tex"}, localTimeoutMs)
             && QFileInfo::exists(dvi)
             && run(QStandardPaths::findExecutable("dvisvgm"), {"--no-fonts", "-o", svgPath, dvi}, localTimeoutMs);
    }
    if (ok && QFileInfo::exists(svgPath)) {
        colorizeSvg(svgPath, normalizeInk(ink), scale);
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
            if (name == QLatin1String("frac") || name == QLatin1String("dfrac")
                || name == QLatin1String("tfrac")) {
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
            } else if (name == QLatin1String("text") || name == QLatin1String("mathrm")
                       || name == QLatin1String("mathbf") || name == QLatin1String("displaystyle")) {
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
                const bool compact = content.size() == 1
                    || (content.startsWith('(') && content.endsWith(')'));
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
