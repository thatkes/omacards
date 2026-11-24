#pragma once

#include <QObject>

class TestRatingRecording : public QObject {
    Q_OBJECT

private slots:
    void goodNewCardRecordsStateAndReturnsIntervalAndDue();
    void againUsesStoredSchedulingState();
    void contentHashMismatchStartsFromEmptySchedulingState();
    void reviewHistoryRecordsWhetherCardWasNew();
    void storeWriteFailureReturnsRawErrorAndKeepsStartingState();
    void storeReadFailureReturnsRawErrorAndReadPhase();
    void recordUsesOneCardSchedulingLookup();
    void bothWritesShareOneCommittedTransaction();
    void failedWriteRollsBackBothWritesInOneTransaction();
    void failedTransactionStartWritesNothing();
};
