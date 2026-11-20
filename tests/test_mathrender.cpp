#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "test_mathrender.h"
#include "test_support.h"
#include "mathrender.h"

using namespace TestSupport;

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

    // Ink is part of the cache key so themed equations do not reuse black SVGs.
    MathRenderer themed(cache);
    QVERIFY(themed.renderExpression("x^2", "#c0caf5").startsWith("<img "));
    bool foundThemedSvg = false;
    for (const QString &name : QDir(cache).entryList({"*.svg"})) {
        QFile svg(cache + "/" + name);
        if (!svg.open(QIODevice::ReadOnly))
            continue;
        const QString content = QString::fromUtf8(svg.readAll());
        if (content.contains("#c0caf5") && !content.contains("rgb(0%, 0%, 0%)"))
            foundThemedSvg = true;
    }
    QVERIFY(foundThemedSvg);

    // Scale enlarges the SVG itself and the <img> pixel dimensions so the
    // 12pt TeX glyphs stay readable beside 24-30px QML labels.
    const QString scaledHtml = themed.renderExpression("x^2", "#70d6ff", 2.0);
    QVERIFY(scaledHtml.contains("width='27'"));
    QVERIFY(scaledHtml.contains("height='13'"));
    bool foundScaledSvg = false;
    for (const QString &name : QDir(cache).entryList({"*.svg"})) {
        QFile svg(cache + "/" + name);
        if (!svg.open(QIODevice::ReadOnly))
            continue;
        const QString content = QString::fromUtf8(svg.readAll());
        if (content.contains("#70d6ff") && content.contains("width=\"54px\" height=\"26px\""))
            foundScaledSvg = true;
    }
    QVERIFY(foundScaledSvg);

    QFile logFile(argsLog);
    QVERIFY(logFile.open(QIODevice::ReadOnly));
    const QString log = QString::fromUtf8(logFile.readAll());
    QVERIFY(log.contains("tectonic --untrusted --only-cached --outdir="));
    QVERIFY(log.contains("tectonic --untrusted --outdir="));
    QVERIFY(!log.contains("-no-shell-escape"));
    QVERIFY(log.contains("pdftocairo -svg "));
}
