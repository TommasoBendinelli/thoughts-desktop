/*
    SPDX-FileCopyrightText: 2013 Fabio D 'Urso <fabiodurso@hotmail.it>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QMimeDatabase>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QTest>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "../part/projectreadingcontext.h"

#include <threadweaver/queue.h>

#include "../core/annotations.h"
#include "../core/document.h"
#include "../core/document_p.h"
#include "../core/generator.h"
#include "../core/observer.h"
#include "../core/page.h"
#include "../core/rotationjob_p.h"
#include "../settings_core.h"

class DocumentTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testProjectReading();
    void testFigureLinksAreCentred();
    void testCloseDuringRotationJob();
    void testRelocateUnchangedDocumentPreservesState();
    void testDocdataMigration();
    void testEvaluateKeystrokeEventChange_data();
    void testEvaluateKeystrokeEventChange();
};

void DocumentTest::testFigureLinksAreCentred()
{
    // A link to a figure lands on its caption, below the figure: the view centres it so the figure
    // shows too. Other targets (a section) stay at the top of the view.
    Okular::SettingsCore::instance(QStringLiteral("documenttest"));
    Okular::Document document(nullptr);
    const QString path = QStringLiteral(KDESRCDIR "data/pdf_with_figure_link.pdf");
    QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)), Okular::Document::OpenSuccess);
    const Okular::DocumentViewport figure(document.metaData(QStringLiteral("NamedViewport"), QStringLiteral("figure.1")).toString());
    QVERIFY(figure.isValid());
    QCOMPARE(figure.pageNumber, 1);
    QVERIFY(figure.rePos.enabled);
    QCOMPARE(figure.rePos.pos, Okular::DocumentViewport::Center);
    QCOMPARE(figure.rePos.normalizedX, 0.5);
    QVERIFY(figure.rePos.normalizedY > 0.3); // the caption, below the tall figure
    const Okular::DocumentViewport section(document.metaData(QStringLiteral("NamedViewport"), QStringLiteral("section.1")).toString());
    QVERIFY(section.isValid());
    QCOMPARE(section.rePos.pos, Okular::DocumentViewport::TopLeft);
    document.closeDocument();
}

void DocumentTest::testProjectReading()
{
    Okular::SettingsCore::instance(QStringLiteral("documenttest"));
    QTemporaryDir directory;
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("objects")));
    QFile input(QStringLiteral(KDESRCDIR "data/file1.pdf"));
    QVERIFY(input.open(QIODevice::ReadOnly));
    const auto bytes = input.readAll();
    const auto hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    QFile object(directory.filePath(QStringLiteral("objects/") + hash));
    QVERIFY(object.open(QIODevice::WriteOnly)); QCOMPARE(object.write(bytes), bytes.size()); object.close();
    Okular::Document standalone(nullptr);
    QCOMPARE(standalone.openDocument(input.fileName(), QUrl::fromLocalFile(input.fileName()), QMimeDatabase().mimeTypeForFile(input.fileName())), Okular::Document::OpenSuccess);
    const int count = standalone.pages();
    const auto size = QSizeF(standalone.page(0)->width(), standalone.page(0)->height());
    standalone.closeDocument();
    QJsonArray entries;
    for (const auto &id : {QStringLiteral("first"), QStringLiteral("second")}) {
        const auto package = QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1},
            {QStringLiteral("kind"), QStringLiteral("thoughts-document-reading-package")}, {QStringLiteral("document_id"), id},
            {QStringLiteral("filename"), QString(id + QStringLiteral(".pdf"))}, {QStringLiteral("pdf_sha256"), hash}, {QStringLiteral("page_count"), count}}).toJson();
        const auto packageHash = QString::fromLatin1(QCryptographicHash::hash(package, QCryptographicHash::Sha256).toHex());
        QFile packageFile(directory.filePath(QStringLiteral("objects/") + packageHash));
        QVERIFY(packageFile.open(QIODevice::WriteOnly)); QCOMPARE(packageFile.write(package), package.size()); packageFile.close();
        entries.append(QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("title"), id},
            {QStringLiteral("pdf_sha256"), hash}, {QStringLiteral("package_sha256"), packageHash},
            {QStringLiteral("pdf_size_bytes"), bytes.size()}, {QStringLiteral("page_count"), count}});
    }
    QJsonObject reading{{QStringLiteral("kind"), QStringLiteral("thoughts-project-reading")},
        {QStringLiteral("schema_version"), 1}, {QStringLiteral("project_id"), QStringLiteral("test-project")},
        {QStringLiteral("title"), QStringLiteral("Test project")}, {QStringLiteral("page_count"), count * 2},
        {QStringLiteral("documents"), entries}};
    const auto encoded = QJsonDocument(reading).toJson();
    const auto parsed = Thoughts::ProjectReading::parse(encoded);
    QVERIFY(parsed); QCOMPARE(parsed->projectPage(QStringLiteral("second"), 0), count);
    QCOMPARE(parsed->locate(count)->id, QStringLiteral("second"));
    QFile manifest(directory.filePath(QStringLiteral("test.thoughts-project")));
    QVERIFY(manifest.open(QIODevice::WriteOnly)); manifest.write(encoded); manifest.close();
    const auto context = Thoughts::ProjectReadingContext::load(manifest.fileName());
    QVERIFY(context); QCOMPARE(context->locate(count)->filename, QStringLiteral("second.pdf"));
    const QJsonObject original{{QStringLiteral("page"), 1}, {QStringLiteral("fragments"), QJsonArray{QJsonObject{{QStringLiteral("page"), 1}}}}};
    QCOMPARE(Thoughts::offsetCommentPages(Thoughts::offsetCommentPages(original, count), -count), original);
    Okular::Document project(nullptr);
    QCOMPARE(project.openDocument(manifest.fileName(), QUrl::fromLocalFile(manifest.fileName()), QMimeDatabase().mimeTypeForFile(manifest.fileName())), Okular::Document::OpenSuccess);
    QCOMPARE(project.pages(), uint(count * 2));
    QCOMPARE(QSizeF(project.page(count)->width(), project.page(count)->height()), size);
    auto *annotation = new Okular::TextAnnotation();
    project.addPageAnnotation(count, annotation);
    QVERIFY(project.page(count)->annotations().contains(annotation));
    QVERIFY(project.page(0)->annotations().isEmpty());
    project.closeDocument();
    QVERIFY(object.open(QIODevice::Append)); object.write("corrupt"); object.close();
    QCOMPARE(project.openDocument(manifest.fileName(), QUrl::fromLocalFile(manifest.fileName()), QMimeDatabase().mimeTypeForFile(manifest.fileName())), Okular::Document::OpenError);
}

void DocumentTest::testRelocateUnchangedDocumentPreservesState()
{
    Okular::SettingsCore::instance(QStringLiteral("documenttest"));
    QTemporaryDir directory;
    const auto original = directory.filePath(QStringLiteral("imported.pdf"));
    const auto published = directory.filePath(QStringLiteral("published.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), original));
    QVERIFY(QFile::copy(original, published));
    Okular::Document document(nullptr);
    QCOMPARE(document.openDocument(original, QUrl::fromLocalFile(original), QMimeDatabase().mimeTypeForFile(original)), Okular::Document::OpenSuccess);
    const auto *page = document.page(0);
    auto *annotation = new Okular::TextAnnotation();
    document.addPageAnnotation(0, annotation);
    const auto viewport = document.viewport().toString();
    QVERIFY(document.canUndo());
    QVERIFY(document.relocateUnchangedBackingFile(published, QUrl::fromLocalFile(published)));
    QCOMPARE(document.currentDocument(), QUrl::fromLocalFile(published));
    QCOMPARE(document.page(0), page);
    QCOMPARE(document.viewport().toString(), viewport);
    QVERIFY(document.page(0)->annotations().contains(annotation));
    QVERIFY(document.canUndo());
    QFile different(original);
    QVERIFY(different.open(QIODevice::Append)); different.write("different"); different.close();
    QVERIFY(!document.relocateUnchangedBackingFile(original, QUrl::fromLocalFile(original)));
    QCOMPARE(document.currentDocument(), QUrl::fromLocalFile(published));
}

// Test that we don't crash if the document is closed while a RotationJob
// is enqueued/running
void DocumentTest::testCloseDuringRotationJob()
{
    Okular::SettingsCore::instance(QStringLiteral("documenttest"));
    Okular::Document *m_document = new Okular::Document(nullptr);
    const QString testFile = QStringLiteral(KDESRCDIR "data/file1.pdf");
    QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForFile(testFile);

    Okular::DocumentObserver *dummyDocumentObserver = new Okular::DocumentObserver();
    m_document->addObserver(dummyDocumentObserver);

    m_document->openDocument(testFile, QUrl(), mime);
    m_document->setRotation(1);

    // Tell ThreadWeaver not to start any new job
    ThreadWeaver::Queue::instance()->suspend();

    // Request a pixmap. A RotationJob will be enqueued but not started
    Okular::PixmapRequest *pixmapReq = new Okular::PixmapRequest(dummyDocumentObserver, 0, 100, 100, qApp->devicePixelRatio(), 1, Okular::PixmapRequest::NoFeature);
    m_document->requestPixmaps({pixmapReq});

    // Delete the document
    delete m_document;

    // Resume job processing and wait for the RotationJob to finish
    ThreadWeaver::Queue::instance()->resume();
    ThreadWeaver::Queue::instance()->finish();
    qApp->processEvents();

    delete dummyDocumentObserver;
}

// Test that, if there's a XML file in docdata referring to a document, we
// detect that it must be migrated, that it doesn't get wiped out if you close
// the document without migrating and that it does get wiped out after migrating
void DocumentTest::testDocdataMigration()
{
    Okular::SettingsCore::instance(QStringLiteral("documenttest"));

    const QUrl testFileUrl = QUrl::fromLocalFile(QStringLiteral(KDESRCDIR "data/file1.pdf"));
    const QString testFilePath = testFileUrl.toLocalFile();
    const qint64 testFileSize = QFileInfo(testFilePath).size();

    // Copy XML file to the docdata/ directory
    const QString docDataPath = Okular::DocumentPrivate::docDataFileName(testFileUrl, testFileSize);
    QFile::remove(docDataPath);
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1-docdata.xml"), docDataPath));

    // Open our document
    Okular::Document *m_document = new Okular::Document(nullptr);
    QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForFile(testFilePath);
    QCOMPARE(m_document->openDocument(testFilePath, testFileUrl, mime), Okular::Document::OpenSuccess);

    // Check that the annotation from file1-docdata.xml was loaded
    QCOMPARE(m_document->page(0)->annotations().size(), 1);
    QCOMPARE(m_document->page(0)->annotations().constFirst()->uniqueName(), QStringLiteral("testannot"));

    // Check that we detect that it must be migrated
    QVERIFY(m_document->isDocdataMigrationNeeded());
    m_document->closeDocument();

    // Reopen the document and check that the annotation is still present
    // (because we have not migrated)
    QCOMPARE(m_document->openDocument(testFilePath, testFileUrl, mime), Okular::Document::OpenSuccess);
    QCOMPARE(m_document->page(0)->annotations().size(), 1);
    QCOMPARE(m_document->page(0)->annotations().constFirst()->uniqueName(), QStringLiteral("testannot"));
    QVERIFY(m_document->isDocdataMigrationNeeded());

    // Do the migration
    QTemporaryFile migratedSaveFile(QStringLiteral("%1/okrXXXXXX.pdf").arg(QDir::tempPath()));
    QVERIFY(migratedSaveFile.open());
    migratedSaveFile.close();
    QVERIFY(m_document->saveChanges(migratedSaveFile.fileName()));
    m_document->docdataMigrationDone();
    QVERIFY(!m_document->isDocdataMigrationNeeded());
    m_document->closeDocument();

    // Now the docdata file should have no annotations, let's check
    QCOMPARE(m_document->openDocument(testFilePath, testFileUrl, mime), Okular::Document::OpenSuccess);
    QCOMPARE(m_document->page(0)->annotations().size(), 0);
    QVERIFY(!m_document->isDocdataMigrationNeeded());
    m_document->closeDocument();

    // And the new file should have 1 annotation, let's check
    QCOMPARE(m_document->openDocument(migratedSaveFile.fileName(), QUrl::fromLocalFile(migratedSaveFile.fileName()), mime), Okular::Document::OpenSuccess);
    QCOMPARE(m_document->page(0)->annotations().size(), 1);
    QVERIFY(!m_document->isDocdataMigrationNeeded());
    m_document->closeDocument();

    delete m_document;
}

void DocumentTest::testEvaluateKeystrokeEventChange_data()
{
    QTest::addColumn<QString>("oldVal");
    QTest::addColumn<QString>("newVal");
    QTest::addColumn<int>("selStart");
    QTest::addColumn<int>("selEnd");
    QTest::addColumn<QString>("expectedDiff");

    QTest::addRow("empty") << ""
                           << "" << 0 << 0 << "";
    QTest::addRow("a") << ""
                       << "a" << 0 << 0 << "a";
    QTest::addRow("ab") << "a"
                        << "b" << 0 << 1 << "b";
    QTest::addRow("ab2") << "a"
                         << "ab" << 1 << 1 << "b";
    QTest::addRow("kaesekuchen") << "Käse"
                                 << "Käsekuchen" << 4 << 4 << "kuchen";
    QTest::addRow("replace") << "kuchen"
                             << "wurst" << 0 << 6 << "wurst";
    QTest::addRow("okular") << "Oku"
                            << "Okular" << 3 << 3 << "lar";
    QTest::addRow("okular2") << "Oku"
                             << "Okular" << 0 << 3 << "Okular";
    QTest::addRow("removal1") << "a"
                              << "" << 0 << 1 << "";
    QTest::addRow("removal2") << "ab"
                              << "a" << 1 << 2 << "";
    QTest::addRow("overlapping chang") << "abcd"
                                       << "abclmnopd" << 1 << 3 << "bclmnop";
    QTest::addRow("unicode") << "☮🤌"
                             << "☮🤌❤️" << 2 << 2 << "❤️";
    QTest::addRow("unicode2") << "☮"
                              << "☮🤌❤️" << 1 << 1 << "🤌❤️";
    QTest::addRow("unicode3") << "🤍"
                              << "🤌" << 0 << 1 << "🤌";
}

void DocumentTest::testEvaluateKeystrokeEventChange()
{
    QFETCH(QString, oldVal);
    QFETCH(QString, newVal);
    QFETCH(int, selStart);
    QFETCH(int, selEnd);
    QFETCH(QString, expectedDiff);

    QCOMPARE(Okular::DocumentPrivate::evaluateKeystrokeEventChange(oldVal, newVal, selStart, selEnd), expectedDiff);
}

QTEST_MAIN(DocumentTest)
#include "documenttest.moc"
