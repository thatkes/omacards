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
    explicit MathRenderer(const QString &cacheDirectory = {}, QObject *parent = nullptr);

    // `ink` colors the rendered SVG; an empty value keeps the renderer's
    // neutral black (used by headless output). `scale` enlarges the intrinsic
    // SVG size so TeX's 12pt glyphs match the surrounding QML label size.
    QString render(const QString &text, const QString &ink = {}, double scale = 1.0) const;
    QString renderPlain(const QString &text) const;
    QStringList warnings(const QString &text) const;
    QString renderExpression(const QString &expression, const QString &ink = {}, double scale = 1.0) const;
    bool hasRenderer() const;

private:
    QString m_cache;
    mutable bool m_checked = false;
    mutable bool m_available = false;
    mutable QString m_converter; // "pdf" or "dvi"

    void detect() const;
    QString renderLatex(const QString &expression, const QString &ink, double scale) const;
};

QString unicodeMath(const QString &expression);
