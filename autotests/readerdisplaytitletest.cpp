// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/readerdisplaytitlestore.h"
#include <QTest>
#include <QTemporaryDir>

class ReaderDisplayTitleTest : public QObject {
    Q_OBJECT
    QJsonArray projects(const QString &olmo = QStringLiteral("olmo3")) {
        return {QJsonObject{{QStringLiteral("id"), QStringLiteral("models")}, {QStringLiteral("name"), QStringLiteral("Models")}, {QStringLiteral("target"), QStringLiteral("models.pdf")},
            {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("document_id"), QStringLiteral("olmo-id")}, {QStringLiteral("name"), olmo}, {QStringLiteral("filename"), QStringLiteral("import-123.pdf")}},
                                      QJsonObject{{QStringLiteral("document_id"), QStringLiteral("qwen-id")}, {QStringLiteral("name"), QStringLiteral("qwen3")}, {QStringLiteral("filename"), QStringLiteral("import-123.pdf")}}}}}};
    }
private Q_SLOTS:
    void sharedPdfUsesSelectedIdentityAndFreshDisplayTitle() {
        const auto url = QUrl::fromLocalFile(QStringLiteral("/library/current_pdfs/import-123.pdf"));
        QCOMPARE(Thoughts::readerDisplayTitle(projects(), {}, QStringLiteral("/library"), url, QStringLiteral("olmo-id")), QStringLiteral("olmo3"));
        QCOMPARE(Thoughts::readerDisplayTitle(projects(QStringLiteral("OLMo 3.1")), {}, QStringLiteral("/library"), url, QStringLiteral("olmo-id"), {}, QStringLiteral("Old title")), QStringLiteral("OLMo 3.1"));
        QCOMPARE(Thoughts::readerDisplayTitle(projects(), {}, QStringLiteral("/library"), url, QStringLiteral("qwen-id")), QStringLiteral("qwen3"));
        QCOMPARE(Thoughts::readerDisplayTitle(projects(), {}, QStringLiteral("/library"), url, {}, QStringLiteral("models")), QStringLiteral("Models"));
        QCOMPARE(Thoughts::readerDisplayTitle(projects(), {}, QStringLiteral("/library"), url), QStringLiteral("import-123.pdf"));
    }
    void externalFilenameCannotBorrowLibraryTitle() {
        const QJsonArray publications{QJsonObject{{QStringLiteral("filename"), QStringLiteral("import-123.pdf")}, {QStringLiteral("display_name"), QStringLiteral("MiMo-V2.6")}}};
        QCOMPARE(Thoughts::readerDisplayTitle({}, publications, QStringLiteral("/library"), QUrl::fromLocalFile(QStringLiteral("/other/import-123.pdf"))), QStringLiteral("import-123.pdf"));
        QCOMPARE(Thoughts::readerDisplayTitle({}, publications, QStringLiteral("/library"), QUrl::fromLocalFile(QStringLiteral("/library/current_pdfs/import-123.pdf"))), QStringLiteral("MiMo-V2.6"));
        QCOMPARE(Thoughts::readerDisplayTitle(projects(), {}, QStringLiteral("/library"), QUrl::fromLocalFile(QStringLiteral("/moved/file.pdf")), QStringLiteral("olmo-id")), QStringLiteral("olmo3"));
        QCOMPARE(Thoughts::readerDisplayTitle({}, {}, QStringLiteral("/library"), QUrl::fromLocalFile(QStringLiteral("/moved/file.pdf")), QStringLiteral("olmo-id"), {}, QStringLiteral("OLMo 3")), QStringLiteral("OLMo 3"));
    }
    void repeatedAtomicReplacementsKeepRefreshing() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkpath(QStringLiteral("current_pdfs")));
        const auto indexPath = root.filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json"));
        const auto url = QUrl::fromLocalFile(root.filePath(QStringLiteral("current_pdfs/loose.pdf")));
        bool callbacksOnUiThread = true;
        Thoughts::ReaderDisplayTitleStore store(root.path(), this, [&] {
            callbacksOnUiThread &= QThread::currentThread() == thread();
        });
        for (int revision = 0; revision < 5; ++revision) {
            const auto title = QStringLiteral("Synced title %1").arg(revision);
            QSaveFile index(indexPath);
            QVERIFY(index.open(QIODevice::WriteOnly));
            index.write(QJsonDocument(QJsonObject{{QStringLiteral("pdfs"), QJsonArray{
                QJsonObject{{QStringLiteral("filename"), QStringLiteral("loose.pdf")},
                            {QStringLiteral("display_name"), title}}}}}).toJson());
            QVERIFY(index.commit());
            QTRY_COMPARE(store.title(url, {}, {}, {}), title);
        }
        QVERIFY(callbacksOnUiThread);
        // Destruction must safely stop watcher work even with a refresh pending.
        store.refresh();
    }
    void manifestRenameAndPublicationSyncRefreshWithoutReopeningPdf() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QDir(root.path()).mkpath(QStringLiteral("current_pdfs"));
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto manifest = Thoughts::ProjectConfiguration::empty(root.path(), QStringLiteral("models"), QStringLiteral("Models"));
        manifest.insert(QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("included"), true},
            {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), manifest.value(QStringLiteral("default_folder_id"))}, {QStringLiteral("path"), QStringLiteral("paper.pdf")}}}}});
        manifest.insert(QStringLiteral("document_names"), QJsonObject{{id, QStringLiteral("OLMo 3")}});
        const auto path = Thoughts::ProjectConfiguration::path(root.path(), QStringLiteral("models"));
        QVERIFY(Thoughts::ProjectConfiguration::save(path, manifest).isEmpty());
        int notifications = 0;
        Thoughts::ReaderDisplayTitleStore store(root.path(), this, [&] { ++notifications; });
        const auto url = QUrl::fromLocalFile(root.filePath(QStringLiteral("current_pdfs/paper.pdf")));
        QTRY_COMPARE(store.title(url, id, {}, QStringLiteral("Old label")), QStringLiteral("OLMo 3"));
        const int before = notifications;
        manifest.insert(QStringLiteral("document_names"), QJsonObject{{id, QStringLiteral("OLMo 3.1")}});
        QVERIFY(Thoughts::ProjectConfiguration::save(path, manifest).isEmpty());
        QTRY_COMPARE(store.title(url, id, {}, QStringLiteral("Old label")), QStringLiteral("OLMo 3.1"));
        QVERIFY(notifications > before);
        // Sync can replace publication metadata atomically without changing any PDF bytes.
        QSaveFile index(root.filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write(R"({"pdfs":[{"filename":"loose.pdf","display_name":"Synced title"}]})");
        QVERIFY(index.commit());
        const auto loose = QUrl::fromLocalFile(root.filePath(QStringLiteral("current_pdfs/loose.pdf")));
        QTRY_COMPARE(store.title(loose, {}, {}, {}), QStringLiteral("Synced title"));
    }
};
QTEST_GUILESS_MAIN(ReaderDisplayTitleTest)
#include "readerdisplaytitletest.moc"
