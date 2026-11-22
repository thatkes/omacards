#pragma once

#include <QObject>

class TestMathRender : public QObject {
    Q_OBJECT

private slots:
    void mathFallsBackToUnicode();
    void tectonicRendererUsesSupportedFlags();
    void renderEscapesPlainTextAndRendersMath();
    void renderExpressionUnicodeFallbackIsStable();
    void unicodeMathCharacterization();
    void warningsDeduplicateAndFilterCommands();
    void dviToolchainUsesNoShellEscapeAndNoFonts();
    void failedLatexRenderFallsBackToUnicode();
    void pdftocairoPixelUnitsScaleImageAndSvg();
    void cachedSvgSkipsSecondToolchainRun();
    void invalidInkNormalizesToBlack();
    void warmRendersMissingExpressionsAndSkipsHits();
    void liveRenderAndPrefetchShareOneCacheEntry();
    void failedQueuedRendersDoNotBlockLaterPrefetches();
};
