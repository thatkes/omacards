#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "test_mathrender.h"
#include "test_support.h"
#include "mathrender.h"

using namespace TestSupport;

namespace {

// QStandardPaths::findExecutable only searches PATH. Restricting PATH to an
// isolated bin directory hides any real TeX install from detect().
struct PathOnlyGuard {
    QByteArray original;

    explicit PathOnlyGuard(const QString &directory) : original(qgetenv("PATH")) {
        qputenv("PATH", directory.toLocal8Bit());
    }

    ~PathOnlyGuard() { qputenv("PATH", original); }
};

struct EnvVarGuard {
    QByteArray name;
    QByteArray original;
    bool present = false;

    EnvVarGuard(const char *variable, const QByteArray &value) : name(variable) {
        original = qgetenv(name.constData());
        present = !original.isNull();
        qputenv(name.constData(), value);
    }

    ~EnvVarGuard() {
        if (present)
            qputenv(name.constData(), original);
        else
            qunsetenv(name.constData());
    }
};

const auto kExecutable = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup |
                         QFile::ReadOther | QFile::ExeOther;

bool writeFakeTool(const QString &binDir, const QString &name, const QString &body) {
    const QString path = binDir + "/" + name;
    const QString script = QStringLiteral("#!/bin/sh\n") +
                           QStringLiteral("printf '%1' >> \"$OMACARDS_TEST_ARGS\"\n").arg(name) +
                           QStringLiteral("for arg do printf ' %s' \"$arg\" >> \"$OMACARDS_TEST_ARGS\"; done\n") +
                           QStringLiteral("printf '\\n' >> \"$OMACARDS_TEST_ARGS\"\n") + body;
    return writeFile(path, script.toUtf8()) && QFile::setPermissions(path, kExecutable);
}

const QString kPtSvg =
    QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10pt\" height=\"5pt\" viewBox=\"0 0 10 5\">"
                   "<g fill=\"rgb(0%, 0%, 0%)\"/></svg>");

const QString kPxSvg =
    QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20px\" height=\"10px\" viewBox=\"0 0 20 10\">"
                   "<g fill=\"rgb(0%, 0%, 0%)\"/></svg>");

// Both fake converters find their output path by scanning argv; pdftocairo
// receives it last, dvisvgm right after -o.
QString svgWriterBody(const QString &svg, const QString &outputLoop) {
    return outputLoop + QStringLiteral("printf '%s\\n' '") + svg + QStringLiteral("' > \"$svg\"\n");
}

bool writePdfToolchain(const QString &binDir, const QString &svg, bool success) {
    const QString tectonicBody = success ? QStringLiteral(R"sh(
out=
tex=
for arg do
    case "$arg" in --outdir=*) out=${arg#--outdir=} ;; esac
    tex="$arg"
done
base=${tex##*/}
base=${base%.tex}
printf '%s\n' '%PDF-1.4' > "$out/$base.pdf"
)sh")
                                         : QStringLiteral("\nexit 1\n");
    const QString cairoBody = svgWriterBody(svg, QStringLiteral(R"sh(
svg=
for arg do svg="$arg"; done
)sh"));
    return writeFakeTool(binDir, "tectonic", tectonicBody) && writeFakeTool(binDir, "pdftocairo", cairoBody);
}

bool writePartlyFailingPdfToolchain(const QString &binDir, const QString &svg) {
    const QString tectonicBody = QStringLiteral(R"sh(
out=
tex=
for arg do
    case "$arg" in --outdir=*) out=${arg#--outdir=} ;; esac
    tex="$arg"
done
grep -q 'bad-' "$tex" && exit 1
base=${tex##*/}
base=${base%.tex}
printf '%s\n' '%PDF-1.4' > "$out/$base.pdf"
)sh");
    const QString cairoBody = svgWriterBody(svg, QStringLiteral(R"sh(
svg=
for arg do svg="$arg"; done
)sh"));
    return writeFakeTool(binDir, "tectonic", tectonicBody) && writeFakeTool(binDir, "pdftocairo", cairoBody);
}

// The renderer colors the cached master by emitting a data URI. Tests read the
// SVG back out of that URI instead of inspecting rendered pixels.
QString svgFromMathHtml(const QString &html) {
    static const QString prefix = QStringLiteral("<img src='data:image/svg+xml;base64,");
    const int start = html.indexOf(prefix);
    if (start < 0)
        return {};
    const int end = html.indexOf('\'', start + prefix.size());
    if (end < 0)
        return {};
    const QByteArray encoded = html.mid(start + prefix.size(), end - start - prefix.size()).toLocal8Bit();
    return QString::fromUtf8(QByteArray::fromBase64(encoded));
}

bool writeDviToolchain(const QString &binDir, const QString &svg) {
    const QString latexBody = QStringLiteral(R"sh(
tex=
for arg do tex="$arg"; done
dvi=${tex%.tex}.dvi
printf '%s\n' 'dvi' > "$dvi"
)sh");
    const QString dvisvgmBody = svgWriterBody(svg, QStringLiteral(R"sh(
svg=
while [ $# -gt 0 ]; do
    if [ "$1" = "-o" ]; then shift; svg="$1"; fi
    shift
done
)sh"));
    return writeFakeTool(binDir, "latex", latexBody) && writeFakeTool(binDir, "dvisvgm", dvisvgmBody);
}

} // namespace

void TestMathRender::mathFallsBackToUnicode() {
    MathRenderer renderer(QDir::tempPath() + "/omacards-math-test");
    const MathRenderer::Engine engine = renderer.detect();
    QVERIFY(engine == MathRenderer::Engine::Unicode || engine == MathRenderer::Engine::LatexSvg);
    QCOMPARE(renderer.hasRenderer(), engine == MathRenderer::Engine::LatexSvg);
    QCOMPARE(renderer.detect(), engine); // detection is cached
    if (renderer.hasRenderer())
        QVERIFY(!renderer.renderExpression("x^2").isEmpty());
    QCOMPARE(unicodeMath("\\alpha + \\beta"), QString("α + β"));
    QCOMPARE(unicodeMath("\\pm i"), QString("± i"));
    QCOMPARE(unicodeMath("\\frac{1}{2}"), QString("(1)/(2)"));
    QCOMPARE(unicodeMath("\\frac{f^{(n)}(a)}{n!}"), QString("(f^(n)(a))/(n!)"));
    QCOMPARE(unicodeMath("\\binom{r}{n}"), QString("C(r,n)"));
    QCOMPARE(unicodeMath("\\sqrt{1+x}"), QString("√(1+x)"));
    QCOMPARE(unicodeMath("x^{2n+1}"), QString("x^(2n+1)"));
    QCOMPARE(unicodeMath("x \\le 2"), QString("x ≤ 2"));
    QCOMPARE(unicodeMath("a \\cdots b"), QString("a ⋯ b"));
    QVERIFY(renderer.warnings("\\(\\frac{1}{2}\\)").isEmpty());
    QCOMPARE(renderer.warnings("\\(\\foobar{x}\\)").size(), 1);
}

void TestMathRender::tectonicRendererUsesSupportedFlags() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(QDir().mkpath(bin));
    QVERIFY(QDir().mkpath(cache));

    const auto makeTool = [&](const QString &name, const QByteArray &script) {
        const QString path = bin + "/" + name;
        QVERIFY(writeFile(path, script));
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup |
                                        QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther);
    };
    const auto script = [](const QStringList &lines) { return (lines.join('\n') + '\n').toUtf8(); };
    makeTool("tectonic",
             script({
                 "#!/bin/sh",
                 "printf 'tectonic' >> \"$OMACARDS_TEST_ARGS\"",
                 "for arg do printf ' %s' \"$arg\" >> \"$OMACARDS_TEST_ARGS\"; done",
                 "printf '\\n' >> \"$OMACARDS_TEST_ARGS\"",
                 "cached=0",
                 "for arg do if [ \"$arg\" = \"--only-cached\" ]; then cached=1; fi; done",
                 "if [ \"$cached\" = \"1\" ] && [ \"$OMACARDS_TEST_MISSING_BUNDLE\" = \"1\" ]; then exit 1; fi",
                 "out=",
                 "tex=",
                 "for arg do",
                 "    case \"$arg\" in --outdir=*) out=${arg#--outdir=} ;; esac",
                 "    tex=\"$arg\"",
                 "done",
                 "base=$(basename \"$tex\" .tex)",
                 "printf '%%PDF-1.4\\n' > \"$out/$base.pdf\"",
             }));
    makeTool("pdftocairo", script({
                               "#!/bin/sh",
                               "printf 'pdftocairo' >> \"$OMACARDS_TEST_ARGS\"",
                               "for arg do printf ' %s' \"$arg\" >> \"$OMACARDS_TEST_ARGS\"; done",
                               "printf '\\n' >> \"$OMACARDS_TEST_ARGS\"",
                               "printf '<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10pt\" height=\"5pt\" "
                               "viewBox=\"0 0 10 5\"><g fill=\"rgb(0%%, 0%%, 0%%)\"/></svg>\\n' > \"$3\"",
                           }));

    struct PathGuard {
        QByteArray original;
        explicit PathGuard(const QString &binDir) : original(qgetenv("PATH")) {
            qputenv("PATH", (binDir + ":" + QString::fromLocal8Bit(original)).toLocal8Bit());
        }
        ~PathGuard() { qputenv("PATH", original); }
    } pathGuard(bin);
    qputenv("OMACARDS_TEST_ARGS", argsLog.toUtf8());
    qputenv("OMACARDS_TEST_MISSING_BUNDLE", "1");
    struct TestVarGuard {
        ~TestVarGuard() {
            qunsetenv("OMACARDS_TEST_ARGS");
            qunsetenv("OMACARDS_TEST_MISSING_BUNDLE");
        }
    } varGuard;

    MathRenderer renderer(cache);
    QVERIFY(renderer.hasRenderer());
    QVERIFY(renderer.renderExpression("x^2").startsWith("<img "));
    QVERIFY(!QDir(cache).entryList({"*.svg"}).isEmpty());

    const QStringList texFiles = QDir(cache + "/tex").entryList({"*.tex"});
    QVERIFY(!texFiles.isEmpty());
    QFile texFile(cache + "/tex/" + texFiles.first());
    QVERIFY(texFile.open(QIODevice::ReadOnly));
    const QString tex = QString::fromUtf8(texFile.readAll());
    QVERIFY(tex.contains("\\hoffset=-1in"));
    QVERIFY(tex.contains("\\pdfpagewidth=\\wd0"));
    QVERIFY(tex.contains("\\shipout\\vbox"));

    // One neutral master serves every ink and scale. The file keeps the
    // toolchain's black fill; the ink appears only in the emitted data URI.
    const QStringList neutralFiles = QDir(cache).entryList({"*.svg"});
    QCOMPARE(neutralFiles.size(), 1);
    QVERIFY(neutralFiles.first().startsWith("eq5-"));
    QFile neutral(cache + "/" + neutralFiles.first());
    QVERIFY(neutral.open(QIODevice::ReadOnly));
    const QString neutralSvg = QString::fromUtf8(neutral.readAll());
    QVERIFY(neutralSvg.contains("rgb(0%, 0%, 0%)"));
    QVERIFY(neutralSvg.contains("width=\"26px\" height=\"14px\""));

    MathRenderer themed(cache);
    const QString themedHtml = themed.renderExpression("x^2", "#c0caf5", 2.0);
    QVERIFY(themedHtml.startsWith("<img src='data:image/svg+xml;base64,"));
    const QString themedSvg = svgFromMathHtml(themedHtml);
    QVERIFY(themedSvg.contains("fill=\"#c0caf5\""));
    QVERIFY(!themedSvg.contains("rgb(0%, 0%, 0%)"));
    QCOMPARE(QDir(cache).entryList({"*.svg"}).size(), 1);
    QFile afterThemed(cache + "/" + neutralFiles.first());
    QVERIFY(afterThemed.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(afterThemed.readAll()), neutralSvg);

    // Scale multiplies the cached SVG's logical size in the <img> dimensions;
    // the SVG itself keeps one supersampled master size for every scale.
    QVERIFY(themedHtml.contains("width='26'"));
    QVERIFY(themedHtml.contains("height='14'"));
    const QString smallHtml = themed.renderExpression("x^2", "#70d6ff", 1.0);
    QVERIFY(smallHtml.contains("width='13'"));
    QVERIFY(smallHtml.contains("height='7'"));
    QVERIFY(svgFromMathHtml(smallHtml).contains("fill=\"#70d6ff\""));
    QCOMPARE(QDir(cache).entryList({"*.svg"}).size(), 1);

    QFile logFile(argsLog);
    QVERIFY(logFile.open(QIODevice::ReadOnly));
    const QString log = QString::fromUtf8(logFile.readAll());
    QVERIFY(log.contains("tectonic --untrusted --only-cached --outdir="));
    QVERIFY(log.contains("tectonic --untrusted --outdir="));
    QVERIFY(!log.contains("-no-shell-escape"));
    QVERIFY(log.contains("pdftocairo -svg "));
}

void TestMathRender::renderEscapesPlainTextAndRendersMath() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("empty-bin");
    QVERIFY(QDir().mkpath(bin));
    PathOnlyGuard pathGuard(bin); // No TeX tools anywhere on PATH: Unicode fallback.
    MathRenderer renderer(dir.filePath("cache"));
    QVERIFY(!renderer.hasRenderer());

    const QString text = QStringLiteral("A < B & C\nD \\(\\alpha\\) E $$\\beta$$ F");
    const QString span = QStringLiteral("<span style='font-family: \"Noto Sans Math\", serif;'>");
    const QString expectedHtml = QStringLiteral("A &lt; B &amp; C<br>D ") + span + QStringLiteral("α</span> E ") +
                                 span + QStringLiteral("β</span> F");
    QCOMPARE(renderer.render(text), expectedHtml);

    // renderPlain() neither escapes plain text nor turns newlines into <br>,
    // and maps each math segment through unicodeMath() instead of renderExpression().
    const QString expectedPlain = QStringLiteral("A < B & C\nD α E β F");
    QCOMPARE(renderer.renderPlain(text), expectedPlain);
}

void TestMathRender::renderExpressionUnicodeFallbackIsStable() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("empty-bin");
    QVERIFY(QDir().mkpath(bin));
    PathOnlyGuard pathGuard(bin); // Exercise the fallback even if TeX is installed elsewhere.
    MathRenderer renderer(dir.filePath("cache"));
    if (renderer.hasRenderer())
        QSKIP("A LaTeX toolchain is installed; the Unicode fallback span is not returned here");

    QCOMPARE(renderer.detect(), MathRenderer::Engine::Unicode);
    const QString expected =
        QStringLiteral("<span style='font-family: \"Noto Sans Math\", serif;'>x² &lt; y &amp; z</span>");
    QCOMPARE(renderer.renderExpression("x^2 < y & z"), expected);
    QCOMPARE(renderer.renderExpression("x^2 < y & z"), expected); // detect() is cached, output is stable
}

void TestMathRender::unicodeMathCharacterization() {
    // Unknown commands lose the backslash; a lone trailing backslash is dropped.
    QCOMPARE(unicodeMath("\\foobar"), QStringLiteral("foobar"));
    QCOMPARE(unicodeMath("x \\"), QStringLiteral("x"));
    QCOMPARE(unicodeMath("\\alpha\\"), QStringLiteral("α"));

    // Structural markers are removed, literal groups are copied verbatim.
    QCOMPARE(unicodeMath("\\left( x \\right)"), QStringLiteral("( x )"));
    QCOMPARE(unicodeMath("\\text{hello & <world>}"), QStringLiteral("hello & <world>"));
    QCOMPARE(unicodeMath("\\mathrm{d}x"), QStringLiteral("dx"));
    QCOMPARE(unicodeMath("\\text{\\alpha + \\beta}"), QStringLiteral("\\alpha + \\beta"));
    // The final superscript replacement still applies to literal group contents.
    QCOMPARE(unicodeMath("\\text{x^2}"), QStringLiteral("x²"));

    // Nested brace groups in commands and in scripts.
    QCOMPARE(unicodeMath("\\frac{a_{1}}{b^{2}}"), QStringLiteral("(a₁)/(b²)"));
    QCOMPARE(unicodeMath("\\sqrt{x^{3}}"), QStringLiteral("√(x³)"));

    // Single-character and parenthesized groups are compact; longer groups get
    // parentheses of their own.
    QCOMPARE(unicodeMath("x^2"), QStringLiteral("x²"));
    QCOMPARE(unicodeMath("x^{2}"), QStringLiteral("x²"));
    QCOMPARE(unicodeMath("x^{ab}"), QStringLiteral("x^(ab)"));
    QCOMPARE(unicodeMath("x_1"), QStringLiteral("x₁"));
    QCOMPARE(unicodeMath("x_{1}"), QStringLiteral("x₁"));
    QCOMPARE(unicodeMath("x_{ab}"), QStringLiteral("x_(ab)"));
    QCOMPARE(unicodeMath("x^{(n)}"), QStringLiteral("x^(n)"));
    QCOMPARE(unicodeMath("a^2 + b^3 + c_0 + d_1"), QStringLiteral("a² + b³ + c₀ + d₁"));

    // Unclosed groups are consumed to the end; stray closing braces vanish.
    QCOMPARE(unicodeMath("a{b"), QStringLiteral("ab"));
    QCOMPARE(unicodeMath("a}b"), QStringLiteral("ab"));
    QCOMPARE(unicodeMath("x^{2"), QStringLiteral("x²"));
    // A command with only one of its two groups falls through to its bare name.
    QCOMPARE(unicodeMath("\\frac{1"), QStringLiteral("frac1"));

    QCOMPARE(unicodeMath("a  b  c"), QStringLiteral("a b c"));
    // Collapsing is a single pass, so three spaces only become two.
    QCOMPARE(unicodeMath("a   b"), QStringLiteral("a  b"));
    QCOMPARE(unicodeMath("  \\alpha  "), QStringLiteral("α"));
}

void TestMathRender::warningsDeduplicateAndFilterCommands() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MathRenderer renderer(dir.filePath("cache"));

    const QString text =
        QStringLiteral("\\(\\foobar\\) outside \\foobarOutside $$\\baz \\alpha \\frac{1}{2} \\left( x \\right)$$ "
                       "\\(\\foobar \\qux\\)");
    QCOMPARE(renderer.warnings(text), QStringList({QStringLiteral("Unsupported math command: \\foobar"),
                                                   QStringLiteral("Unsupported math command: \\baz"),
                                                   QStringLiteral("Unsupported math command: \\qux")}));

    const QString known =
        QStringLiteral("\\(\\alpha \\beta \\frac{1}{2} \\dfrac{1}{2} \\tfrac{1}{2} \\binom{1}{2} \\sqrt{x} "
                       "\\text{x} \\mathrm{x} \\mathbf{x} \\displaystyle x \\left( x \\right)\\)");
    QVERIFY(renderer.warnings(known).isEmpty());
    QVERIFY(renderer.warnings(QStringLiteral("plain \\foobar text")).isEmpty());
}

void TestMathRender::dviToolchainUsesNoShellEscapeAndNoFonts() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(QDir().mkpath(bin));
    QVERIFY(QDir().mkpath(cache));
    QVERIFY(writeDviToolchain(bin, kPtSvg));

    // PATH contains only the two fake DVI tools: tectonic, pdflatex and
    // pdftocairo are absent, so detect() must choose the DVI branch.
    PathOnlyGuard pathGuard(bin);
    EnvVarGuard argsGuard("OMACARDS_TEST_ARGS", argsLog.toUtf8());
    if (!QStandardPaths::findExecutable(QStringLiteral("tectonic")).isEmpty() ||
        !QStandardPaths::findExecutable(QStringLiteral("pdflatex")).isEmpty() ||
        !QStandardPaths::findExecutable(QStringLiteral("pdftocairo")).isEmpty())
        QSKIP("Cannot hide the PDF toolchain from QStandardPaths by replacing PATH");

    MathRenderer renderer(cache);
    QCOMPARE(renderer.detect(), MathRenderer::Engine::LatexSvg);
    QVERIFY(renderer.hasRenderer());

    const QString html = renderer.renderExpression("x^2");
    QVERIFY(html.startsWith("<img "));
    QVERIFY(html.contains("width='13'") && html.contains("height='7'"));

    QFile logFile(argsLog);
    QVERIFY(logFile.open(QIODevice::ReadOnly));
    const QString log = QString::fromUtf8(logFile.readAll());
    QVERIFY(log.contains("latex -interaction=nonstopmode -no-shell-escape -output-directory="));
    QVERIFY(log.contains("dvisvgm --no-fonts -o "));
    QVERIFY(!log.contains("tectonic"));
    QVERIFY(!log.contains("pdflatex"));
    QVERIFY(!log.contains("pdftocairo"));

    const QStringList svgs = QDir(cache).entryList({"*.svg"});
    QCOMPARE(svgs.size(), 1);
    QFile svg(cache + "/" + svgs.first());
    QVERIFY(svg.open(QIODevice::ReadOnly));
    const QString svgText = QString::fromUtf8(svg.readAll());
    QVERIFY(svgText.contains("<svg"));
    QVERIFY(svgText.contains("width=\"26px\" height=\"14px\"")); // pt dimensions become px
}

void TestMathRender::failedLatexRenderFallsBackToUnicode() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(QDir().mkpath(bin));
    QVERIFY(QDir().mkpath(cache));
    QVERIFY(writePdfToolchain(bin, kPtSvg, false));

    PathOnlyGuard pathGuard(bin);
    EnvVarGuard argsGuard("OMACARDS_TEST_ARGS", argsLog.toUtf8());
    MathRenderer renderer(cache);
    QVERIFY(renderer.hasRenderer()); // detection only checks that tools exist

    const QString expected = QStringLiteral("<span style='font-family: \"Noto Sans Math\", serif;'>x²</span>");
    QCOMPARE(renderer.renderExpression("x^2"), expected);
    QVERIFY(QDir(cache).entryList({"*.svg"}).isEmpty());

    QFile logFile(argsLog);
    QVERIFY(logFile.open(QIODevice::ReadOnly));
    const QString log = QString::fromUtf8(logFile.readAll());
    QVERIFY(log.count("tectonic --untrusted") == 2); // cached-only, then download-enabled
    QVERIFY(!log.contains("pdftocairo"));            // short-circuited after the failed TeX run
}

void TestMathRender::pdftocairoPixelUnitsScaleImageAndSvg() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(QDir().mkpath(bin));
    QVERIFY(QDir().mkpath(cache));
    QVERIFY(writePdfToolchain(bin, kPxSvg, true));

    PathOnlyGuard pathGuard(bin);
    EnvVarGuard argsGuard("OMACARDS_TEST_ARGS", argsLog.toUtf8());
    MathRenderer renderer(cache);
    QVERIFY(renderer.hasRenderer());

    const QString html = renderer.renderExpression("x^2", QString(), 2.0);
    QVERIFY(html.startsWith("<img src='data:image/svg+xml;base64,"));
    QVERIFY(svgFromMathHtml(html).contains("fill=\"#000000\""));
    QVERIFY(html.contains("width='40'"));
    QVERIFY(html.contains("height='20'"));

    const QStringList svgs = QDir(cache).entryList({"*.svg"});
    QCOMPARE(svgs.size(), 1);
    QFile svg(cache + "/" + svgs.first());
    QVERIFY(svg.open(QIODevice::ReadOnly));
    const QString svgText = QString::fromUtf8(svg.readAll());
    // Supersampling happens once at generation time; scale changes only the
    // <img> size, so a 20px SVG stays 40px in the file at every scale.
    QVERIFY(svgText.contains("width=\"40px\" height=\"20px\""));
}

void TestMathRender::cachedSvgSkipsSecondToolchainRun() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(QDir().mkpath(bin));
    QVERIFY(QDir().mkpath(cache));
    QVERIFY(writePdfToolchain(bin, kPtSvg, true));

    PathOnlyGuard pathGuard(bin);
    EnvVarGuard argsGuard("OMACARDS_TEST_ARGS", argsLog.toUtf8());
    MathRenderer renderer(cache);
    QVERIFY(renderer.hasRenderer());

    const QString first = renderer.renderExpression("x^2", "#123456", 1.0);
    const QString themed = renderer.renderExpression("x^2", "#abcdef", 2.0);
    const QString scaled = renderer.renderExpression("x^2", QString(), 1.6);
    QVERIFY(first.startsWith("<img src='data:image/svg+xml;base64,"));
    QVERIFY(svgFromMathHtml(first).contains("fill=\"#123456\""));
    QVERIFY(svgFromMathHtml(themed).contains("fill=\"#abcdef\""));
    QVERIFY(svgFromMathHtml(scaled).contains("fill=\"#000000\""));
    QVERIFY(first != themed);
    QVERIFY(themed.contains("width='26'"));
    QVERIFY(scaled.contains("width='21'"));
    QCOMPARE(QDir(cache).entryList({"*.svg"}).size(), 1);

    QFile logFile(argsLog);
    QVERIFY(logFile.open(QIODevice::ReadOnly));
    const QString log = QString::fromUtf8(logFile.readAll());
    QVERIFY(log.count("tectonic --untrusted") == 1);
    QVERIFY(log.count("pdftocairo -svg") == 1);
}

void TestMathRender::warmRendersMissingExpressionsAndSkipsHits() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(QDir().mkpath(cache));
    QVERIFY(writeFakePdfToolchain(bin, kPtSvg));

    PathPrependGuard pathGuard(bin);
    ScopedEnv argsGuard("OMACARDS_TEST_ARGS", argsLog);
    MathRenderer renderer(cache);
    QVERIFY(renderer.hasRenderer());

    // Repeating an expression must still produce one render per cache key.
    const QString text = QStringLiteral("\\(x^2\\) and \\(y^2\\) and \\(x^2\\)");
    int cached = -1;
    QCOMPARE(renderer.warm(text, &cached), 2);
    QCOMPARE(cached, 0);

    const QString xPath = renderer.cachedSvgPath("x^2");
    const QString yPath = renderer.cachedSvgPath("y^2");
    QVERIFY(xPath.startsWith(cache + "/eq5-"));
    QVERIFY(yPath.startsWith(cache + "/eq5-"));
    QVERIFY(xPath != yPath);
    // Warming writes the same neutral master a live render reads; the data
    // URI carries the requested ink, so no themed file appears.
    QVERIFY(renderer.renderExpression("x^2", "#123456", 2.0).contains("data:image/svg+xml;base64,"));
    QCOMPARE(QDir(cache).entryList({"*.svg"}).size(), 2);

    QCOMPARE(renderer.warm(text, &cached), 0);
    QCOMPARE(cached, 2); // two distinct SVGs were already cached

    QFile logFile(argsLog);
    QVERIFY(logFile.open(QIODevice::ReadOnly));
    const QString log = QString::fromUtf8(logFile.readAll());
    QCOMPARE(log.count("tectonic --untrusted"), 2);
    QCOMPARE(log.count("pdftocairo -svg"), 2);
}

void TestMathRender::liveRenderAndPrefetchShareOneCacheEntry() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(QDir().mkpath(cache));
    QVERIFY(writeFakePdfToolchain(bin, kPtSvg, QStringLiteral("sleep 0.2\n")));

    PathPrependGuard pathGuard(bin);
    ScopedEnv argsGuard("OMACARDS_TEST_ARGS", argsLog);
    MathRenderer renderer(cache);
    QVERIFY(renderer.hasRenderer());

    // Whichever caller claims the key first renders it; the other must skip.
    renderer.prefetch("\\(x^2\\)", "#123456", 2.0);
    renderer.renderExpression("x^2", "#123456", 2.0);
    const QString path = renderer.mathKey("x^2");
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path), 5000);

    QFile logFile(argsLog);
    QVERIFY(logFile.open(QIODevice::ReadOnly));
    const QString log = QString::fromUtf8(logFile.readAll());
    QCOMPARE(log.count("tectonic --untrusted"), 1);
    QCOMPARE(log.count("pdftocairo -svg"), 1);
}

void TestMathRender::failedQueuedRendersDoNotBlockLaterPrefetches() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(QDir().mkpath(bin));
    QVERIFY(QDir().mkpath(cache));
    QVERIFY(writePartlyFailingPdfToolchain(bin, kPtSvg));

    PathPrependGuard pathGuard(bin);
    ScopedEnv argsGuard("OMACARDS_TEST_ARGS", argsLog);
    MathRenderer renderer(cache);
    QVERIFY(renderer.hasRenderer());

    // The two failing keys occupy the worker slots; the third must still run.
    renderer.prefetch("\\(bad-one\\) \\(bad-two\\) \\(good\\)", "#123456", 2.0);
    const QString goodPath = renderer.mathKey("good");
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(goodPath), 5000);
    QVERIFY(!QFileInfo::exists(renderer.mathKey("bad-one")));
    QVERIFY(!QFileInfo::exists(renderer.mathKey("bad-two")));
}

void TestMathRender::invalidInkNormalizesToBlack() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(QDir().mkpath(bin));
    QVERIFY(QDir().mkpath(cache));
    QVERIFY(writePdfToolchain(bin, kPtSvg, true));

    PathOnlyGuard pathGuard(bin);
    EnvVarGuard argsGuard("OMACARDS_TEST_ARGS", argsLog.toUtf8());
    MathRenderer renderer(cache);
    QVERIFY(renderer.hasRenderer());
    const QString html = renderer.renderExpression("x^2", "not-a-color");
    QVERIFY(html.startsWith("<img src='data:image/svg+xml;base64,"));
    QVERIFY(svgFromMathHtml(html).contains("fill=\"#000000\""));

    const QStringList svgs = QDir(cache).entryList({"*.svg"});
    QCOMPARE(svgs.size(), 1);
    QFile svg(cache + "/" + svgs.first());
    QVERIFY(svg.open(QIODevice::ReadOnly));
    const QString svgText = QString::fromUtf8(svg.readAll());
    QVERIFY(svgText.contains("rgb(0%, 0%, 0%)")); // the master stays neutral
    QVERIFY(!svgText.contains("#000000"));
}
