#pragma once

#include <QObject>

class TestFileHistory : public QObject {
    Q_OBJECT

private slots:
    void fileHistoryUndoRedoText();
    void fileHistoryRestorePreWriteState();
    void fileHistoryUndoRemovesCreatedFile();
    void fileHistoryUnchangedRecordIsNoop();
    void fileHistoryUndoRedoDeletedFile();
    void fileHistoryRecordClearsRedo();
    void fileHistoryEmptyStacksAreNoops();
    void fileHistoryKeepsFiftySnapshots();
    void fileHistoryUndoFailureKeepsSnapshot();
    void fileHistoryDescriptionOutParams();
};
