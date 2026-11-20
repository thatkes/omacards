#pragma once

#include <QObject>

class TestMathRender : public QObject {
    Q_OBJECT

private slots:
    void mathFallsBackToUnicode();
    void tectonicRendererUsesSupportedFlags();
};
