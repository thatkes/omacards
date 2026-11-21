#pragma once

#include <QObject>

class TestStudySession : public QObject {
    Q_OBJECT

private slots:
    void setModeNormalizesInvalidInput();
    void setModeUppercaseCramNormalizesToReview();
    void setModeCanonicalInputIsNoOp();
    void setModeCramSwitchesAndResets();
    void invalidRatingsAreNoOps();
    void reviewStatusOmitsSessionPositionBox();
    void flipAndShowNextToggleAnswer();
    void setStatusDedupesAndReportErrorSignals();
    void rebuildKeepsPendingRepeatInReview();
    void switchingToCramClearsPendingRepeat();
    void statsTextAndMathEngine();
    void setMathColorsEmitsOnlyOnChange();
};
