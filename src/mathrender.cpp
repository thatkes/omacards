// Math follows the plan: real LaTeX when a renderer exists, Unicode otherwise.
// TeX is untrusted card content, so shell escape is always disabled.
#include "mathrender.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

MathRenderer::MathRenderer(const QString &cacheDirectory, QObject *parent)
    : QObject(parent),
      m_cache(cacheDirectory.isEmpty()
                  ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/math"
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

QString MathRenderer::render(const QString &text) const {
    static const QRegularExpression math(R"re(\\\((.*?)\\\)|\$\$(.*?)\$\$)re",
                                          QRegularExpression::DotMatchesEverythingOption);
    QString html;
    int last = 0;
    auto it = math.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        html += text.mid(last, match.capturedStart()).toHtmlEscaped().replace('\n', "<br>");
        const QString expression = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
        html += renderExpression(expression);
        last = match.capturedEnd();
    }
    html += text.mid(last).toHtmlEscaped().replace('\n', "<br>");
    return html;
}

QString MathRenderer::renderExpression(const QString &expression) const {
    const QString image = renderLatex(expression);
    if (!image.isEmpty())
        return QString("<img src='file://%1' alt='%2' style='vertical-align: middle;'>")
            .arg(image, expression.toHtmlEscaped());
    return QString("<span style='font-family: \"Noto Sans Math\", serif;'>%1</span>")
        .arg(unicodeMath(expression).toHtmlEscaped());
}

QString MathRenderer::renderLatex(const QString &expression) const {
    detect();
    if (!m_available)
        return {};

    const QString base = "eq-" + QString::fromLatin1(
        QCryptographicHash::hash(expression.toUtf8(), QCryptographicHash::Sha256).toHex().left(20));
    const QString svgPath = m_cache + "/" + base + ".svg";
    if (QFileInfo::exists(svgPath))
        return svgPath;

    const QString texDir = m_cache + "/tex";
    QDir().mkpath(texDir);
    const QString stem = texDir + "/" + base;
    const QString tex = QString(
        "\\documentclass[12pt]{article}\n"
        "\\usepackage{amsmath,amssymb}\n"
        "\\pagestyle{empty}\n"
        "\\begin{document}\n$%1$\n\\end{document}\n").arg(expression);
    QFile file(stem + ".tex");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    file.write(tex.toUtf8());
    file.close();

    auto run = [](const QString &program, const QStringList &arguments) {
        QProcess process;
        process.setWorkingDirectory(QFileInfo(arguments.last()).absolutePath());
        process.start(program, arguments);
        return process.waitForFinished(2000) && process.exitCode() == 0;
    };

    bool ok = false;
    if (m_converter == "pdf") {
        const QString engine = QStandardPaths::findExecutable("tectonic");
        const QString pdf = stem + ".pdf";
        ok = run(engine.isEmpty() ? QStandardPaths::findExecutable("pdflatex") : engine,
                 {engine.isEmpty() ? QString("-interaction=nonstopmode") : QString("--outdir=" + QFileInfo(stem).absolutePath()),
                  "-no-shell-escape", stem + ".tex"})
             && QFileInfo::exists(pdf)
             && run(QStandardPaths::findExecutable("pdftocairo"), {"-svg", pdf, svgPath});
    } else {
        const QString dvi = stem + ".dvi";
        ok = run(QStandardPaths::findExecutable("latex"),
                 {"-interaction=nonstopmode", "-no-shell-escape", "-output-directory=" + QFileInfo(stem).absolutePath(),
                  stem + ".tex"})
             && QFileInfo::exists(dvi)
             && run(QStandardPaths::findExecutable("dvisvgm"), {"--no-fonts", "-o", svgPath, dvi});
    }
    return ok && QFileInfo::exists(svgPath) ? svgPath : QString();
}

template <typename Replacement>
static QString replaceMatches(const QString &text, const QRegularExpression &expression, Replacement replacement) {
    QString out;
    int last = 0;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        out += text.mid(last, match.capturedStart() - last) + replacement(match);
        last = match.capturedEnd();
    }
    return out + text.mid(last);
}

QString unicodeMath(const QString &expression) {
    static const QHash<QString, QString> symbols{
        {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ε"},
        {"theta", "θ"}, {"lambda", "λ"}, {"mu", "μ"}, {"pi", "π"}, {"sigma", "σ"},
        {"tau", "τ"}, {"phi", "φ"}, {"omega", "ω"}, {"Gamma", "Γ"}, {"Delta", "Δ"},
        {"Theta", "Θ"}, {"Lambda", "Λ"}, {"Pi", "Π"}, {"Sigma", "Σ"}, {"Omega", "Ω"},
        {"times", "×"}, {"cdot", "·"}, {"pm", "±"}, {"mp", "∓"}, {"leq", "≤"},
        {"geq", "≥"}, {"neq", "≠"}, {"approx", "≈"}, {"infty", "∞"}, {"sum", "∑"},
        {"prod", "∏"}, {"int", "∫"}, {"partial", "∂"}, {"nabla", "∇"}, {"to", "→"},
        {"rightarrow", "→"}, {"leftarrow", "←"}, {"Rightarrow", "⇒"}, {"Leftrightarrow", "⇔"},
        {"in", "∈"}, {"notin", "∉"}, {"subset", "⊂"}, {"supset", "⊃"}, {"cup", "∪"},
        {"cap", "∩"}, {"emptyset", "∅"}, {"forall", "∀"}, {"exists", "∃"}, {"neg", "¬"},
        {"land", "∧"}, {"lor", "∨"}, {"ldots", "…"}, {"dots", "…"}, {"quad", " "},
        {"qquad", "  "}, {"sin", "sin"}, {"cos", "cos"}, {"tan", "tan"}, {"log", "log"},
        {"ln", "ln"}, {"lim", "lim"}, {"min", "min"}, {"max", "max"}, {"sqrt", "√"},
    };

    QString out = expression.trimmed();
    out.remove(QRegularExpression(R"re(\\(left|right|displaystyle|text|mathrm|mathbf))re"));
    static const QRegularExpression command(R"re(\\([A-Za-z]+))re");
    static const QRegularExpression fraction(R"re(\\frac\{([^{}]*)\}\{([^{}]*)\})re");
    out = replaceMatches(out, fraction, [](const QRegularExpressionMatch &m) {
        return QString("(%1)/(%2)").arg(m.captured(1), m.captured(2));
    });
    out = replaceMatches(out, command, [](const QRegularExpressionMatch &m) {
        return symbols.value(m.captured(1), m.captured(1));
    });
    out.remove(QRegularExpression(R"re(\\[,;! ])re"));
    out.replace("^2", "²").replace("^3", "³").replace("_0", "₀").replace("_1", "₁");
    out.replace("{", "").replace("}", "");
    out.replace("  ", " ").replace("√(", "√").replace("^", "^");
    return out.trimmed();
}
