#pragma once

#include <QObject>

class TestImporter : public QObject {
    Q_OBJECT

private slots:
    void csvSelectsDelimiterByExtension();
    void csvDetectsHeaderCaseInsensitively();
    void csvSkipsShortAndEmptyRows();
    void csvSplitsTagsFromThirdColumn();
    void csvParsesQuotedFieldsWithEscapedQuotes();
    void csvHandlesCrLf();
    void csvNamesDeckFromFileNameUnlessOverridden();
    void csvReportsEmptyAndUnreadableFiles();
    void nullResultReturnsFalse();
    void fileDispatchesExtensionsCaseInsensitively();
    void fileRejectsUnsupportedExtension();
    void apkgRejectsInvalidArchive();
    void apkgReportsMissingUnzip();
    void apkgImportsHtmlClozeAndMedia();
    void apkgImportsWithoutMedia();
    void apkgImportsMultipleMediaWithMixedNoteTypes();
    void apkgUsesOneUnzipForCollectionAndMedia();
    void apkgMediaFailureLeavesNoPartialFile();
};
