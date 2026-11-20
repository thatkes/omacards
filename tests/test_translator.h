#pragma once

#include <QObject>

class TestTranslator : public QObject {
    Q_OBJECT

private slots:
    void readLanguageFallbacksAndFormats();
    void pluralCategories();
    void formatLocaleRegistryAndFallback();
    void translateStringAndPluralBasics();
    void translateCatalogFallbackChain();
    void translatePluralFallsBackToOther();
    void isEmptyReflectsCatalog();
    void changedOnLanguageFileChange();
    void changedOnCatalogChangeWithLanguageSwitch();
};
