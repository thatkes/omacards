#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// Renders inline LaTeX into rich text. When a TeX toolchain is installed the
// expression becomes a cached SVG; otherwise a Unicode approximation keeps the
// card readable. Rendering is bounded so a broken TeX install cannot hang the UI.
class MathRenderer : public QObject {
    Q_OBJECT

public:
    // Result of the one-time toolchain probe. `None` means "not probed yet";
    // after detect() the renderer is either the Unicode fallback or LaTeX/SVG.
    enum class Engine { None, Unicode, LatexSvg };

    explicit MathRenderer(const QString &cacheDirectory = {}, QObject *parent = nullptr);

    // `ink` colors the rendered SVG; an empty value keeps the renderer's
    // neutral black (used by headless output). `scale` enlarges the intrinsic
    // SVG size so TeX's 12pt glyphs match the surrounding QML label size.
    QString render(const QString &text, const QString &ink = {}, double scale = 1.0) const;
    QString renderPlain(const QString &text) const;
    QStringList warnings(const QString &text) const;
    QString renderExpression(const QString &expression, const QString &ink = {}, double scale = 1.0) const;

    // Single detection point; the result is cached after the first call.
    Engine detect() const;
    bool hasRenderer() const;

private:
    enum class Toolchain { None, Pdf, Dvi };

    QString m_cache;
    mutable Engine m_engine = Engine::None;
    mutable Toolchain m_toolchain = Toolchain::None;

    QString renderLatex(const QString &expression, const QString &ink, double scale) const;
};

QString unicodeMath(const QString &expression);
