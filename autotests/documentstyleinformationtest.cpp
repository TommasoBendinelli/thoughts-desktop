/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../shell/documentstyleinformation.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace
{

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size() && file.flush();
}

QString sha256(const QByteArray &contents)
{
    return QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
}

QJsonObject libraryResource(const QString &kind, const QString &path, const QByteArray &contents)
{
    const QString digest = sha256(contents);
    return {{QStringLiteral("kind"), kind}, {QStringLiteral("path"), path}, {QStringLiteral("sha256"), digest}, {QStringLiteral("object_sha256"), digest}, {QStringLiteral("size_bytes"), contents.size()}};
}

QJsonObject artifactResource(const QString &kind, const QString &path, const QByteArray &contents)
{
    const QString digest = sha256(contents);
    return {{QStringLiteral("kind"), kind}, {QStringLiteral("path"), path}, {QStringLiteral("sha256"), digest}, {QStringLiteral("object"), digest}, {QStringLiteral("bytes"), contents.size()}};
}

}

class DocumentStyleInformationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPublishedResourcesAndFrozenFallback();
    void testOldServerFallbackAndFedoraIdentity();
    void testCreateMinimalOverrideSafely();
};

void DocumentStyleInformationTest::testPublishedResourcesAndFrozenFallback()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = temporary.filePath(QStringLiteral("docs"));
    const QString stateRoot = temporary.filePath(QStringLiteral("state"));
    const QString project = QStringLiteral("docs-paper");
    const QString pdfFilename = QStringLiteral("paper.pdf");
    const QString pdfSha256(64, QLatin1Char('a'));
    const QString sharedPath = QStringLiteral("docs-paper/tex/support/manual_style.sty");
    const QString overridePath = project + QStringLiteral("/tex/support/manual_style_override.sty");
    const QByteArray shared = QByteArrayLiteral("\\ProvidesPackage{manual_style}\n");
    const QByteArray overrideContents = QByteArrayLiteral("\\ProvidesPackage{manual_style_override}\n");
    QVERIFY(writeFile(QDir(docsRoot).filePath(sharedPath), shared));
    QVERIFY(writeFile(QDir(docsRoot).filePath(overridePath), overrideContents));

    const QJsonArray libraryResources {libraryResource(QStringLiteral("project_style"), sharedPath, shared), libraryResource(QStringLiteral("project_style_override"), overridePath, overrideContents)};
    const QJsonObject index {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("pdfs"), QJsonArray {QJsonObject {{QStringLiteral("filename"), pdfFilename}, {QStringLiteral("pdf_sha256"), pdfSha256}, {QStringLiteral("source_resources"), libraryResources}}}},
    };
    QVERIFY(writeFile(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")), QJsonDocument(index).toJson()));

    const QJsonArray resources {artifactResource(QStringLiteral("project_style"), sharedPath, shared), artifactResource(QStringLiteral("project_style_override"), overridePath, overrideContents)};
    const QJsonObject manifest {{QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
                                {QStringLiteral("filename"), pdfFilename},
                                {QStringLiteral("pdf"), QJsonObject {{QStringLiteral("filename"), pdfFilename}, {QStringLiteral("pdf_sha256"), pdfSha256}}},
                                {QStringLiteral("resources"), resources}};
    const QByteArray manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    const QString manifestSha256 = sha256(manifestBytes);
    const QString bundleRoot = QDir(stateRoot).filePath(QStringLiteral("source-bundle"));
    QVERIFY(writeFile(QDir(bundleRoot).filePath(QStringLiteral("objects/") + manifestSha256), manifestBytes));
    QVERIFY(writeFile(QDir(bundleRoot).filePath(QStringLiteral("objects/") + sha256(shared)), shared));
    QVERIFY(writeFile(QDir(bundleRoot).filePath(QStringLiteral("objects/") + sha256(overrideContents)), overrideContents));
    const QJsonObject pointer {{QStringLiteral("filename"), pdfFilename}, {QStringLiteral("pdf_sha256"), pdfSha256}, {QStringLiteral("artifact_sha256"), manifestSha256}};
    QVERIFY(writeFile(QDir(bundleRoot).filePath(QStringLiteral("documents/") + pdfFilename + QStringLiteral(".json")), QJsonDocument(pointer).toJson()));

    Thoughts::DocumentStyleInformation information = Thoughts::inspectDocumentStyleInformation(docsRoot, pdfFilename, pdfSha256, project, stateRoot);
    QVERIFY(information.publishedResourcesAvailable);
    QVERIFY(information.sharedStyle.liveAvailable);
    QVERIFY(information.sharedStyle.localMatchesPublished());
    QVERIFY(information.sharedStyle.frozenSnapshotAvailable);
    QVERIFY(information.projectOverride.liveAvailable);
    QVERIFY(information.projectOverride.localMatchesPublished());

    QJsonObject mismatchedResource = libraryResources.at(0).toObject();
    mismatchedResource.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('f')));
    mismatchedResource.insert(QStringLiteral("object_sha256"), QString(64, QLatin1Char('f')));
    QJsonArray mismatchedResources = libraryResources;
    mismatchedResources.replace(0, mismatchedResource);
    QJsonObject mismatchedEntry = index.value(QStringLiteral("pdfs")).toArray().at(0).toObject();
    mismatchedEntry.insert(QStringLiteral("source_resources"), mismatchedResources);
    QJsonObject mismatchedIndex = index;
    mismatchedIndex.insert(QStringLiteral("pdfs"), QJsonArray {mismatchedEntry});
    QVERIFY(writeFile(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")), QJsonDocument(mismatchedIndex).toJson()));
    information = Thoughts::inspectDocumentStyleInformation(docsRoot, pdfFilename, pdfSha256, project, stateRoot);
    QVERIFY(!information.sharedStyle.publishedIdentityAvailable);
    QVERIFY(writeFile(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")), QJsonDocument(index).toJson()));

    QVERIFY(writeFile(QDir(docsRoot).filePath(sharedPath), shared + QByteArrayLiteral("% local edit\n")));
    information = Thoughts::inspectDocumentStyleInformation(docsRoot, pdfFilename, pdfSha256, project, stateRoot);
    QVERIFY(!information.sharedStyle.localMatchesPublished());
    QVERIFY(QFile::remove(QDir(docsRoot).filePath(sharedPath)));
    information = Thoughts::inspectDocumentStyleInformation(docsRoot, pdfFilename, pdfSha256, project, stateRoot);
    QVERIFY(!information.sharedStyle.liveAvailable);
    QVERIFY(information.sharedStyle.frozenSnapshotAvailable);
    QByteArray frozen;
    QString error;
    QVERIFY2(Thoughts::readFrozenDocumentStyleSnapshot(information.sharedStyle, &frozen, &error), qPrintable(error));
    QCOMPARE(frozen, shared);
}

void DocumentStyleInformationTest::testOldServerFallbackAndFedoraIdentity()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = temporary.filePath(QStringLiteral("docs"));
    const QString stateRoot = temporary.filePath(QStringLiteral("state"));
    const QByteArray shared = QByteArrayLiteral("shared style\n");
    const QString sharedPath = QStringLiteral("docs-paper/tex/support/manual_style.sty");
    QVERIFY(writeFile(QDir(docsRoot).filePath(sharedPath), shared));
    const QString pdfSha256(64, QLatin1Char('b'));
    const QJsonObject index {{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("pdf_sha256"), pdfSha256}}}}};
    QVERIFY(writeFile(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")), QJsonDocument(index).toJson()));
    const QJsonObject sourceState {{QStringLiteral("bases"), QJsonObject {{sharedPath, QJsonObject {{QStringLiteral("sha256"), sha256(shared)}}}}}};
    QVERIFY(writeFile(QDir(stateRoot).filePath(QStringLiteral("source-publication.json")), QJsonDocument(sourceState).toJson()));

    const Thoughts::DocumentStyleInformation information = Thoughts::inspectDocumentStyleInformation(docsRoot, QStringLiteral("paper.pdf"), pdfSha256, QStringLiteral("docs-paper"), stateRoot);
    QVERIFY(!information.publishedResourcesAvailable);
    QVERIFY(information.sharedStyle.liveAvailable);
    QVERIFY(!information.sharedStyle.publishedIdentityAvailable);
    QVERIFY(information.sharedStyle.fedoraIdentityAvailable);
    QVERIFY(information.sharedStyle.localMatchesFedora());
    QCOMPARE(information.projectOverride.relativePath, QStringLiteral("docs-paper/tex/support/manual_style_override.sty"));
    QVERIFY(!information.projectOverride.liveAvailable);
}

void DocumentStyleInformationTest::testCreateMinimalOverrideSafely()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = temporary.filePath(QStringLiteral("docs"));
    QVERIFY(QDir().mkpath(QDir(docsRoot).filePath(QStringLiteral("docs-paper"))));
    QString path;
    QString error;
    QVERIFY2(Thoughts::createProjectStyleOverride(docsRoot, QStringLiteral("docs-paper"), &path, &error), qPrintable(error));
    QCOMPARE(QDir::cleanPath(path), QDir(docsRoot).filePath(QStringLiteral("docs-paper/tex/support/manual_style_override.sty")));
    QFile created(path);
    QVERIFY(created.open(QIODevice::ReadOnly));
    const QByteArray contents = created.readAll();
    QVERIFY(contents.contains("\\ProvidesPackage{manual_style_override}"));
    QVERIFY(!contents.contains("\\newcommand"));
    QVERIFY(!contents.contains("\\renewcommand"));
    QVERIFY(!contents.contains("\\providecommand"));
    QVERIFY(!Thoughts::createProjectStyleOverride(docsRoot, QStringLiteral("docs-paper"), &path, &error));
    QVERIFY(error.contains(QStringLiteral("already has")));
    QVERIFY(!Thoughts::createProjectStyleOverride(docsRoot, QStringLiteral("../outside"), &path, &error));
}

QTEST_GUILESS_MAIN(DocumentStyleInformationTest)

#include "documentstyleinformationtest.moc"
