#pragma once

#include <QObject>

class TestFileHistory : public QObject {
    Q_OBJECT

private slots:
    void fileHistoryUndoRedoText();
    void fileHistoryUndoRemovesCreatedFile();
};
