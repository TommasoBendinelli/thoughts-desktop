// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/projectreadinggeneration.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

namespace
{
QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool write(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class ProjectReadingGenerationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // A project reading can keep a document version that is no longer
    // published and was pruned from the download cache; an earlier generation
    // still holds it. Staging must take it from there instead of failing.
    void stagingUsesRetainedGenerationObjects()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString cache = temporary.filePath(QStringLiteral("cache"));
        const QString previous = temporary.filePath(QStringLiteral("previous"));
        const QString staging = temporary.filePath(QStringLiteral("staging"));
        const QByteArray oldPdf("%PDF-old-version");
        const QByteArray package("PK-package");
        const QByteArray publishedPdf("%PDF-published");
        // The old document PDF exists only in the previous generation.
        QVERIFY(write(QDir(previous).filePath(QStringLiteral("objects/") + sha256(oldPdf)), oldPdf));
        QVERIFY(write(QDir(cache).filePath(sha256(package)), package));
        QVERIFY(write(QDir(staging).filePath(QStringLiteral("paper.pdf")), publishedPdf));
        const QJsonObject reading {
            {QStringLiteral("schema_version"), 1}, {QStringLiteral("kind"), QStringLiteral("thoughts-project-reading")},
            {QStringLiteral("project_id"), QStringLiteral("project-one")}, {QStringLiteral("title"), QStringLiteral("Project")},
            {QStringLiteral("page_count"), 2},
            {QStringLiteral("documents"), QJsonArray {QJsonObject {
                {QStringLiteral("document_id"), QStringLiteral("doc-1")}, {QStringLiteral("title"), QStringLiteral("Grant proposal")},
                {QStringLiteral("pdf_sha256"), sha256(oldPdf)}, {QStringLiteral("package_sha256"), sha256(package)},
                {QStringLiteral("pdf_size_bytes"), oldPdf.size()}, {QStringLiteral("page_count"), 2}}}}};
        const QByteArray readingBytes = QJsonDocument(reading).toJson(QJsonDocument::Compact);
        QVERIFY(write(QDir(cache).filePath(sha256(readingBytes)), readingBytes));
        const QString readingName = QStringLiteral("project_0123456789abcdef01234567.thoughts-project");
        const QJsonObject library {
            {QStringLiteral("publications"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("sha256"), sha256(publishedPdf)}}}},
            {QStringLiteral("project_readings"), QJsonArray {QJsonObject {
                {QStringLiteral("filename"), readingName}, {QStringLiteral("sha256"), sha256(readingBytes)},
                {QStringLiteral("size_bytes"), readingBytes.size()}, {QStringLiteral("project_id"), QStringLiteral("project-one")},
                {QStringLiteral("page_count"), 2}}}}};

        QString error;
        QVERIFY(!Thoughts::prepareProjectReadings(library, staging, cache, true, &error));
        QVERIFY2(error.contains(QStringLiteral("Grant proposal")) && error.contains(sha256(oldPdf)) && error.contains(readingName), qPrintable(error));

        error.clear();
        QVERIFY2(Thoughts::prepareProjectReadings(library, staging, cache, true, &error, {QDir(previous).filePath(QStringLiteral("objects"))}), qPrintable(error));
        QVERIFY(QFileInfo::exists(QDir(staging).filePath(QStringLiteral("objects/") + sha256(oldPdf))));
        QVERIFY(QFileInfo::exists(QDir(staging).filePath(readingName)));
        // The staged generation verifies on its own.
        QVERIFY2(Thoughts::prepareProjectReadings(library, staging, QString(), false, &error), qPrintable(error));

        // A corrupt retained copy is never used.
        QDir(staging).removeRecursively();
        QVERIFY(write(QDir(staging).filePath(QStringLiteral("paper.pdf")), publishedPdf));
        QVERIFY(write(QDir(previous).filePath(QStringLiteral("objects/") + sha256(oldPdf)), QByteArray("%PDF-tampered!!!")));
        QVERIFY(!Thoughts::prepareProjectReadings(library, staging, cache, true, &error, {QDir(previous).filePath(QStringLiteral("objects"))}));
    }
};

QTEST_GUILESS_MAIN(ProjectReadingGenerationTest)
#include "projectreadinggenerationtest.moc"
