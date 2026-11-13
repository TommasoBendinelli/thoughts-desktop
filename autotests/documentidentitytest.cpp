// SPDX-License-Identifier: GPL-2.0-or-later
#include "part/documentidentity.h"
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
};
QTEST_GUILESS_MAIN(DocumentIdentityTest)
#include "documentidentitytest.moc"
