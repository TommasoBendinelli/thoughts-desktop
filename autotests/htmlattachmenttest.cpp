// SPDX-License-Identifier: GPL-2.0-or-later
#undef QT_NO_CAST_FROM_ASCII
#include "core/htmlattachment.h"
#include <QtTest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
class HtmlAttachmentTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void links() {
        QCOMPARE(Thoughts::htmlAttachmentPath(QUrl("thoughts-html:docs-demo/tex/support/html/plot.html")), QString("docs-demo/tex/support/html/plot.html"));
        QCOMPARE(Thoughts::htmlAttachmentPath(QUrl("thoughts-html:docs-demo/tex/source/nested/my%20plot.HTML")), QString("docs-demo/tex/source/nested/my plot.HTML"));
        QCOMPARE(Thoughts::htmlAttachmentPath(QUrl("thoughts-html:docs-demo/markdown/source/demo.htm")), QString("docs-demo/markdown/source/demo.htm"));
        QVERIFY(!Thoughts::isHtmlSourcePath(QStringLiteral("docs-demo/tex/source/../secret.html")));
        QVERIFY(!Thoughts::isHtmlSourcePath(QStringLiteral("docs-demo/tex/source/a\\b.html")));
        // A Project's default folder has pages like a docs folder (24 Nov 2026).
        const QString project = QStringLiteral(".thoughts/projects/") + QString(64, QLatin1Char('0')) + QStringLiteral("/default");
        QCOMPARE(Thoughts::htmlAttachmentPath(QUrl(QStringLiteral("thoughts-html:") + project + QStringLiteral("/tex/support/html/prompts.html"))),
                 project + QStringLiteral("/tex/support/html/prompts.html"));
        QCOMPARE(Thoughts::htmlSourceFolder(project + QStringLiteral("/tex/support/html/prompts.html")), project);
        QVERIFY(!Thoughts::isHtmlSourcePath(project + QStringLiteral("/prompts.html")));
        QVERIFY(!Thoughts::isHtmlSourcePath(project + QStringLiteral("/tex/.hidden/prompts.html")));
        QVERIFY(!Thoughts::isHtmlSourcePath(QStringLiteral(".thoughts/projects/0/default/tex/support/html/prompts.html")));
        QVERIFY(!Thoughts::isHtmlSourcePath(QStringLiteral(".thoughts/tex/support/html/prompts.html")));
        for (const QString &uri : {"thoughts-html:../secret.html", "thoughts-html:docs-demo/tex/support/html/../plot.html", "thoughts-html:docs-demo/tex/support/html/plot.html?path=secret", "https://example.com/plot.html"}) QVERIFY(Thoughts::htmlAttachmentPath(QUrl(uri)).isEmpty());
    }
    void pageLinks() {
        // A link in an embedded page: another page of its folder, at the fragment's page state.
        const QString from = "docs-x/tex/support/html/a.html";
        auto page = Thoughts::htmlPageLink(QUrl("thoughts-html:docs-x/tex/support/html/b.html"), from);
        QVERIFY(page.isValid()); QCOMPARE(page.path, QString("docs-x/tex/support/html/b.html")); QVERIFY(page.hash.isEmpty());
        page = Thoughts::htmlPageLink(QUrl("thoughts-html:docs-x/tex/support/html/b.html#slide=3"), from);
        QCOMPARE(page.path, QString("docs-x/tex/support/html/b.html")); QCOMPARE(page.hash, QString("#slide=3"));
        // As the page reads location.hash: still percent-encoded; an empty fragment is the page's start.
        QCOMPARE(Thoughts::htmlPageLink(QUrl("thoughts-html:docs-x/tex/support/html/b.html#q=a%20b&slide=2"), from).hash, QString("#q=a%20b&slide=2"));
        page = Thoughts::htmlPageLink(QUrl("thoughts-html:docs-x/tex/support/html/b.html#"), from);
        QVERIFY(page.isValid()); QVERIFY(page.hash.isEmpty());
        // The page itself (a slide of it), and pages of the folder's other trees.
        QCOMPARE(Thoughts::htmlPageLink(QUrl("thoughts-html:" + from + "#slide=5"), from).hash, QString("#slide=5"));
        QVERIFY(Thoughts::htmlPageLink(QUrl("thoughts-html:docs-x/tex/source/plots/c.htm"), from).isValid());
        QVERIFY(Thoughts::htmlPageLink(QUrl("thoughts-html:docs-x/markdown/d.html"), from).isValid());
        QCOMPARE(Thoughts::htmlPageLink(QUrl("thoughts-html:docs-x/tex/support/html/my%20plot.html"), from).path, QString("docs-x/tex/support/html/my plot.html"));
        // A Project's default folder links its own pages, never another Project's.
        const QString project = ".thoughts/projects/" + QString(64, QLatin1Char('a')) + "/default";
        const QString other = ".thoughts/projects/" + QString(64, QLatin1Char('b')) + "/default";
        QVERIFY(Thoughts::htmlPageLink(QUrl("thoughts-html:" + project + "/tex/support/html/b.html"), project + "/tex/support/html/a.html").isValid());
        QVERIFY(!Thoughts::htmlPageLink(QUrl("thoughts-html:" + other + "/tex/support/html/b.html"), project + "/tex/support/html/a.html").isValid());
        // Refused: another folder, no page, a host or query, other schemes, a page state no page can have.
        for (const QString &uri : {"thoughts-html:docs-y/tex/support/html/b.html", "thoughts-html:docs-xy/tex/support/html/b.html",
                                   "thoughts-html:docs-x/tex/support/html/../../../docs-y/tex/b.html", "thoughts-html:docs-x/tex/support/html/notes.txt",
                                   "thoughts-html:docs-x/b.html", "thoughts-html://host/docs-x/tex/support/html/b.html",
                                   "thoughts-html:docs-x/tex/support/html/b.html?slide=3", "https://example.org/docs-x/tex/support/html/b.html",
                                   "thoughts-pdf-link:x.pdf?page=2", "javascript:alert(1)", "thoughts-html:"}) {
            QVERIFY2(!Thoughts::htmlPageLink(QUrl(uri), from).isValid(), qPrintable(uri));
        }
        QVERIFY(!Thoughts::htmlPageLink(QUrl("thoughts-html:docs-x/tex/support/html/b.html#" + QString(512, QLatin1Char('s'))), from).isValid());
        QVERIFY(Thoughts::htmlPageLink(QUrl("thoughts-html:docs-x/tex/support/html/b.html#" + QString(511, QLatin1Char('s'))), from).isValid());
        // Only from a page of the Library.
        for (const QString &source : {QString(), QString("docs-x/tex/support/html/a.txt"), QString("https://example.org/a.html")}) {
            QVERIFY(!Thoughts::htmlPageLink(QUrl("thoughts-html:docs-x/tex/support/html/b.html"), source).isValid());
        }
    }
    void offlineHttp() {
        const QByteArray html = "<!doctype html><button onclick=\"this.textContent=42\">Calculate</button>";
        QTemporaryDir temp; QFile file(temp.filePath("page.html")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(html); file.close();
        const QUrl url = Thoughts::serveHtmlAttachmentFile(file.fileName());
        QVERIFY(url.isValid()); QCOMPARE(url.host(), QString("127.0.0.1"));
        QNetworkAccessManager network;
        auto *reply = network.get(QNetworkRequest(url));
        QSignalSpy finished(reply, &QNetworkReply::finished); QVERIFY(finished.wait());
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QCOMPARE(reply->readAll(), html); QVERIFY(reply->rawHeader("Content-Security-Policy").contains("connect-src 'none'"));
        QUrl bad = url; bad.setPath("/secret");
        reply = network.get(QNetworkRequest(bad)); QSignalSpy denied(reply, &QNetworkReply::finished); QVERIFY(denied.wait());
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
    }
    void rejectsInvalidUtf8() {
        QTemporaryDir temp; QFile file(temp.filePath("invalid.html")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromHex("3c68746d6c3ec3")); file.close();
        QVERIFY(Thoughts::serveHtmlAttachmentFile(file.fileName()).isEmpty());
    }
    void fullSizeStream() {
        QTemporaryDir temp; QFile file(temp.filePath("large.html")); QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(100000000)); file.close();
        auto url = Thoughts::serveHtmlAttachmentFile(file.fileName()); QVERIFY(url.isValid());
        QNetworkAccessManager network; auto *reply = network.get(QNetworkRequest(url));
        qint64 received = 0;
        connect(reply, &QNetworkReply::readyRead, this, [reply, &received] { received += reply->readAll().size(); });
        QSignalSpy done(reply, &QNetworkReply::finished); QVERIFY(done.wait(30000));
        received += reply->readAll().size(); QCOMPARE(received, qint64(100000000));
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.resize(100000001)); file.close();
        QVERIFY(Thoughts::serveHtmlAttachmentFile(file.fileName()).isEmpty());
    }
    void verifiedGeneration() {
        QTemporaryDir temp; QDir root(temp.path()); root.mkpath("objects"); root.mkpath("documents");
        auto write = [&](const QString &name, const QByteArray &bytes) { QFile f(root.filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(bytes), bytes.size()); };
        auto digest = [](const QByteArray &b) { return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex()); };
        const QByteArray pdf = "%PDF-fixture", html = "<html>offline</html>";
        const QString path = "docs-demo/tex/support/html/plot.html";
        write("demo.pdf", pdf); write("objects/" + digest(html), html);
        const QJsonObject document {{"filename", "demo.pdf"}, {"pdf_sha256", digest(pdf)}};
        const QJsonObject resource {{"kind", "html_attachment"}, {"path", path}, {"sha256", digest(html)}, {"object", digest(html)}, {"bytes", html.size()}};
        const QByteArray manifest = QJsonDocument(QJsonObject {{"pdf", document}, {"resources", QJsonArray {resource}}}).toJson();
        write("objects/" + digest(manifest), manifest);
        auto pointer = document; pointer.insert("artifact_sha256", digest(manifest));
        write("documents/demo.pdf.json", QJsonDocument(pointer).toJson());
        QCOMPARE(Thoughts::htmlAttachmentFile(temp.path(), root.filePath("demo.pdf"), path), root.filePath("objects/" + digest(html)));
        write("objects/" + digest(html), "corrupt"); QVERIFY(Thoughts::htmlAttachmentFile(temp.path(), root.filePath("demo.pdf"), path).isEmpty());
        write("objects/" + digest(html), html); write("demo.pdf", "%PDF-with-local-annotations"); QCOMPARE(Thoughts::htmlAttachmentFile(temp.path(), root.filePath("demo.pdf"), path), root.filePath("objects/" + digest(html)));
        pointer.insert("pdf_sha256", QString(64, QLatin1Char('b'))); write("documents/demo.pdf.json", QJsonDocument(pointer).toJson());
        QVERIFY(Thoughts::htmlAttachmentFile(temp.path(), root.filePath("demo.pdf"), path).isEmpty());
    }
    void resolvesForInAppView() {
        // The Library's runtime bundle next to current_pdfs/ (the second root searched).
        QTemporaryDir temp; QDir docs(temp.path()); docs.mkpath("current_pdfs");
        QDir root(docs.filePath(".runtime/target/source-bundle")); root.mkpath("objects"); root.mkpath("documents");
        auto write = [&](const QString &name, const QByteArray &bytes) { QFile f(name); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(bytes), bytes.size()); };
        auto digest = [](const QByteArray &b) { return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex()); };
        const QByteArray pdf = "%PDF-fixture", html = "<html><p id=t>Rendered</p></html>", older = "<html>made on this</html>";
        const QString path = "docs-ideas/tex/support/html/explainer.html";
        const QString pdfPath = docs.filePath("current_pdfs/ideas.pdf");
        write(pdfPath, pdf); write(root.filePath("objects/" + digest(html)), html); write(root.filePath("objects/" + digest(older)), older);
        const QJsonObject document {{"filename", "ideas.pdf"}, {"pdf_sha256", digest(pdf)}};
        const QJsonObject resource {{"kind", "html_attachment"}, {"path", path}, {"sha256", digest(html)}, {"object", digest(html)}, {"bytes", html.size()}};
        const QByteArray manifest = QJsonDocument(QJsonObject {{"pdf", document}, {"resources", QJsonArray {resource}}}).toJson();
        write(root.filePath("objects/" + digest(manifest)), manifest);
        auto pointer = document; pointer.insert("artifact_sha256", digest(manifest));
        write(root.filePath("documents/ideas.pdf.json"), QJsonDocument(pointer).toJson());
        QString error;
        const auto target = Thoughts::resolveHtmlAttachment(path, pdfPath, &error);
        QVERIFY2(target.isValid(), qPrintable(error));
        QCOMPARE(target.path, path); QCOMPARE(target.sha256, digest(html));
        QCOMPARE(QFileInfo(target.file).canonicalFilePath(), QFileInfo(root.filePath("objects/" + digest(html))).canonicalFilePath());
        // Not linked from this PDF, or an invalid path: nothing, with the reason.
        QVERIFY(!Thoughts::resolveHtmlAttachment("docs-ideas/tex/support/html/other.html", pdfPath, &error).isValid()); QVERIFY(error.contains("Sync"));
        QVERIFY(!Thoughts::resolveHtmlAttachment("docs-ideas/../x.html", pdfPath, &error).isValid()); QVERIFY(error.contains("invalid"));
        // An earlier version a card was made on, by its hash, while it is cached and intact.
        QCOMPARE(QFileInfo(Thoughts::htmlAttachmentObject(digest(older), pdfPath)).canonicalFilePath(), QFileInfo(root.filePath("objects/" + digest(older))).canonicalFilePath());
        QVERIFY(Thoughts::htmlAttachmentObject(QString(64, 'f'), pdfPath).isEmpty());
        QVERIFY(Thoughts::htmlAttachmentObject("../objects", pdfPath).isEmpty());
        write(root.filePath("objects/" + digest(older)), "changed");
        QVERIFY(Thoughts::htmlAttachmentObject(digest(older), pdfPath).isEmpty());
        // Served for the in-app view as long as its owner lives, with the attachment CSP.
        auto *owner = new QObject;
        const QUrl url = Thoughts::serveHtmlAttachmentFile(target.file, owner);
        QVERIFY(url.isValid());
        QNetworkAccessManager network;
        auto *reply = network.get(QNetworkRequest(url));
        QSignalSpy finished(reply, &QNetworkReply::finished); QVERIFY(finished.wait());
        QCOMPARE(reply->readAll(), html);
        const QByteArray csp = reply->rawHeader("Content-Security-Policy");
        QVERIFY(csp.contains("sandbox allow-scripts")); QVERIFY(csp.contains("connect-src 'none'"));
        delete owner;
        reply = network.get(QNetworkRequest(url));
        QSignalSpy gone(reply, &QNetworkReply::finished); QVERIFY(gone.wait());
        QVERIFY(reply->error() != QNetworkReply::NoError);
    }
    void attachmentCards() {
        const QJsonObject card {{"primary", QJsonObject {{"created_in", "html_attachment"}}}};
        QVERIFY(Thoughts::isHtmlAttachmentCard(card));
        QVERIFY(Thoughts::isHtmlAttachmentCard(QJsonObject {{"source_link", QJsonObject {{"state", "html_attachment"}}}}));
        QVERIFY(!Thoughts::isHtmlAttachmentCard(QJsonObject {{"primary", QJsonObject {{"created_in", "html"}}}}));
        QVERIFY(!Thoughts::isHtmlAttachmentCard(QJsonObject {{"primary", QJsonObject {{"page", 1}}}}));
    }
};
QTEST_GUILESS_MAIN(HtmlAttachmentTest)
#include "htmlattachmenttest.moc"
