#pragma once

#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QThreadPool>

// Study and the deck warming command must ask for the same SVG files.
namespace MathScale {
constexpr double kQuestion = 2.0;
constexpr double kAnswer = 1.6;
} // namespace MathScale

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
    ~MathRenderer() override;

    // `ink` colors the rendered SVG; an empty value keeps the renderer's
    // neutral black (used by headless output). `scale` enlarges the intrinsic
    // SVG size so TeX's 12pt glyphs match the surrounding QML label size.
    QString render(const QString &text, const QString &ink = {}, double scale = 1.0) const;
    QString renderPlain(const QString &text) const;
    QStringList warnings(const QString &text) const;
    QString renderExpression(const QString &expression, const QString &ink = {}, double scale = 1.0) const;

    // Full path of the cached SVG for one expression. The key combines the
    // expression, ink and scale; the file name itself is unchanged by warming.
    QString mathKey(const QString &expression, const QString &ink, double scale) const;
    QString cachedSvgPath(const QString &expression, const QString &ink, double scale) const;

    // Renders every uncached expression in `text`, counting newly written SVGs
    // and (when supplied) cache hits. Repeated expressions share one render.
    int warm(const QString &text, const QString &ink, double scale, int *cached = nullptr);

    // Presentation and prefetching share the renderer so color handling stays
    // here. renderWithPlaceholders reserves a fixed-size neutral slot for each
    // uncached expression and schedules its render; prefetch schedules without
    // producing any markup. No toolchain process runs on the calling thread.
    QString renderWithPlaceholders(const QString &text, const QString &ink, double scale);
    void prefetch(const QString &text, const QString &ink, double scale);
    void clearPendingMath();

    static QString mathPlaceholderMarker(const QString &key);

    // Single detection point; the result is cached after the first call.
    Engine detect() const;
    bool hasRenderer() const;

signals:
    // Emitted on the renderer's thread when a queued expression reaches the
    // cache. `html` already carries the reserved display size.
    void mathRendered(const QString &key, const QString &html);

private slots:
    void finishRender(const QString &key, const QString &html);

private:
    friend class MathRenderRunnable;

    enum class Toolchain { None, Pdf, Dvi };

    struct RenderRequest {
        QString key;
        QString expression;
        QString ink;
        double scale = 1.0;
        QSize size;
    };

    void enqueueExpression(const RenderRequest &request);
    void startNextRender();
    QString renderRequest(const RenderRequest &request);

    QString m_cache;
    mutable QMutex m_detectMutex;
    mutable Engine m_engine = Engine::None;
    mutable Toolchain m_toolchain = Toolchain::None;

    QThreadPool m_pool;
    QQueue<RenderRequest> m_queue;
    QSet<QString> m_queuedKeys;
    QSet<QString> m_activeKeys;

    QString renderLatex(const QString &expression, const QString &ink, double scale) const;
};

QString unicodeMath(const QString &expression);
