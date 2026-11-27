// SPDX-License-Identifier: GPL-2.0-or-later
#include "part/documentidentity.h"
#include "part/thoughtsplatform.h"
#include <QTest>
#include <QTemporaryDir>

class DocumentIdentityTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void importedDocumentSurvivesPublicationAndRestart() {
        QTemporaryDir root;
        QDir(root.path()).mkpath(QStringLiteral(".thoughts/project-imports"));
        QDir(root.path()).mkpath(QStringLiteral("current_pdfs"));
        const auto staged = root.filePath(QStringLiteral(".thoughts/project-imports/paper.pdf"));
        const QByteArray content("%PDF-fixture");
        QFile file(staged);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(content), content.size());
        file.close();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
        QString error;
        Thoughts::DocumentIdentityStore store(root.path());
        const auto id = store.registerImport(QStringLiteral("paper.pdf"), sha, staged, &error);
        QVERIFY2(Thoughts::DocumentIdentityStore::validId(id), qPrintable(error));
        QCOMPARE(store.registerImport(QStringLiteral("paper.pdf"), sha, staged, &error), id);
        QCOMPARE(Thoughts::DocumentIdentityStore(root.path()).forFile(staged).value(QStringLiteral("document_id")).toString(), id);
        QCOMPARE(store.readablePath(id), staged);
        const auto published = root.filePath(QStringLiteral("current_pdfs/paper.pdf"));
        QVERIFY(QFile::copy(staged, published));
        QFile index(root.filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write(QJsonDocument(QJsonObject{{QStringLiteral("pdfs"), QJsonArray{QJsonObject{
            {QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("document_id"), id},
            {QStringLiteral("pdf_sha256"), sha}, {QStringLiteral("kind"), QStringLiteral("unlinked")}}}}}).toJson());
        index.close();
        QCOMPARE(store.readablePath(id), published);
        QCOMPARE(store.forFile(staged).value(QStringLiteral("document_id")).toString(), id);
        QCOMPARE(store.forFile(published).value(QStringLiteral("document_id")).toString(), id);
        const auto external = root.filePath(QStringLiteral("paper.pdf"));
        QVERIFY(QFile::copy(staged, external));
        QVERIFY(store.forFile(external).isEmpty());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("different bytes"); file.close();
        QVERIFY(store.forFile(staged).isEmpty());
        QCOMPARE(store.readablePath(id), published);
    }

    // An earlier version still in its Library generation folder has that version's identity; the
    // same bytes elsewhere, other bytes, or another document's entry have none.
    void earlierVersionInItsGenerationKeepsItsIdentity() {
        QTemporaryDir temporary;
        const QDir root(QFileInfo(temporary.path()).canonicalFilePath());
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto writeGeneration = [&root](QChar revision, const QByteArray &content, const QString &documentId) {
            const auto generation = root.filePath(QStringLiteral(".thoughts-library-stage-%1").arg(QString(64, revision)));
            QDir().mkpath(generation);
            QFile pdf(QDir(generation).filePath(QStringLiteral("paper.pdf")));
            if (!pdf.open(QIODevice::WriteOnly) || pdf.write(content) != content.size()) return QString();
            pdf.close();
            QFile index(QDir(generation).filePath(QStringLiteral(".thoughts-library-index.json")));
            if (!index.open(QIODevice::WriteOnly)) return QString();
            index.write(QJsonDocument(QJsonObject{{QStringLiteral("pdfs"), QJsonArray{QJsonObject{
                {QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("document_id"), documentId},
                {QStringLiteral("pdf_sha256"), QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex())}}}}}).toJson());
            return QDir(generation).filePath(QStringLiteral("paper.pdf"));
        };
        const auto earlier = writeGeneration(QLatin1Char('a'), "%PDF-earlier", id);
        const auto newest = writeGeneration(QLatin1Char('b'), "%PDF-newest", id);
        const auto other = writeGeneration(QLatin1Char('c'), "%PDF-other", QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVERIFY(!earlier.isEmpty() && !newest.isEmpty() && !other.isEmpty());
        // A junction on Windows, as Thoughts makes it there: QFile::link would make a .lnk file.
        QVERIFY(Thoughts::Platform::replaceDirectoryLink(root.filePath(QStringLiteral("current_pdfs")), QFileInfo(newest).absolutePath()));
        const auto earlierSha = QString::fromLatin1(QCryptographicHash::hash("%PDF-earlier", QCryptographicHash::Sha256).toHex());
        const Thoughts::DocumentIdentityStore store(root.path());

        QVERIFY(store.forFile(earlier).isEmpty());
        QCOMPARE(store.forVersion(earlier).value(QStringLiteral("document_id")).toString(), id);
        QCOMPARE(store.forVersion(earlier).value(QStringLiteral("pdf_sha256")).toString(), earlierSha);
        QCOMPARE(store.forVersion(earlier, earlierSha).value(QStringLiteral("document_id")).toString(), id);
        QVERIFY(store.forVersion(earlier, QString(64, QLatin1Char('0'))).isEmpty());
        // The newest is the current one, as forFile has it.
        const auto published = root.filePath(QStringLiteral("current_pdfs/paper.pdf"));
        QVERIFY(!store.forFile(published).isEmpty());
        QCOMPARE(store.forVersion(published), store.forFile(published));
        // A generation whose entry is another document's.
        QVERIFY(store.forVersion(other).isEmpty());
        // The same bytes outside a generation folder.
        const auto loose = root.filePath(QStringLiteral("paper.pdf"));
        QVERIFY(QFile::copy(earlier, loose));
        QVERIFY(store.forVersion(loose).isEmpty());
        // Bytes that are not that version.
        QFile changed(earlier);
        QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate));
        changed.write("%PDF-changed-bytes");
        changed.close();
        QVERIFY(store.forVersion(earlier).isEmpty());
    }
};
QTEST_GUILESS_MAIN(DocumentIdentityTest)
#include "documentidentitytest.moc"
