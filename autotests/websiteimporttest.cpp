// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/websiteimportdialog.h"
#include <QBuffer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QTest>
#include <QTimer>
#include <cstring>

struct Response {
    Response(QByteArray data = {}, int code = 200, QUrl location = {}, qint64 size = -1, int milliseconds = 0)
        : body(std::move(data)), status(code), redirect(std::move(location)), length(size), delay(milliseconds) {}
    QByteArray body;
    int status = 200;
    QUrl redirect;
    qint64 length = -1;
    int delay = 0;
};
class Reply : public QNetworkReply {
public:
    Reply(const QNetworkRequest &request, Response response, QObject *parent) : QNetworkReply(parent), m_body(response.body) {
        setRequest(request); setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        if (!response.redirect.isEmpty()) setAttribute(QNetworkRequest::RedirectionTargetAttribute, response.redirect);
        if (response.length >= 0) setHeader(QNetworkRequest::ContentLengthHeader, response.length);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(response.delay, this, [this] {
            if (isFinished()) return;
            Q_EMIT readyRead();
            if (isFinished()) return;
            setFinished(true); Q_EMIT finished();
        });
    }
    void abort() override { if (!isFinished()) { setError(OperationCanceledError, QStringLiteral("Cancelled")); setFinished(true); Q_EMIT finished(); } }
    qint64 bytesAvailable() const override { return m_body.size() - m_offset + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 size) override {
        const qint64 count = qMin(size, m_body.size() - m_offset);
        if (count <= 0) return -1;
        std::memcpy(data, m_body.constData() + m_offset, count); m_offset += count; return count;
    }
private:
    QByteArray m_body;
    qint64 m_offset = 0;
};
class Network : public QNetworkAccessManager {
public:
    QList<Response> responses;
    QList<QUrl> requests;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        requests << request.url();
        return new Reply(request, responses.isEmpty() ? Response{{}, 404} : responses.takeFirst(), this);
    }
};
class WebsiteImportTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void resolve_data() {
        QTest::addColumn<QString>("input"); QTest::addColumn<QString>("pdf");
        QTest::newRow("arxiv-abstract") << QStringLiteral("arxiv.org/abs/2401.12345v2") << QStringLiteral("https://arxiv.org/pdf/2401.12345v2");
        QTest::newRow("arxiv-pdf") << QStringLiteral(" http://www.arxiv.org/pdf/2401.12345.pdf ") << QStringLiteral("https://arxiv.org/pdf/2401.12345");
        QTest::newRow("arxiv-html") << QStringLiteral("https://arxiv.org/html/2401.12345v1") << QStringLiteral("https://arxiv.org/pdf/2401.12345v1");
        QTest::newRow("arxiv-legacy") << QStringLiteral("https://export.arxiv.org/abs/hep-th/9901001") << QStringLiteral("https://arxiv.org/pdf/hep-th/9901001");
        QTest::newRow("openreview") << QStringLiteral("https://openreview.net/forum?id=Ab_c-123&noteId=other") << QStringLiteral("https://openreview.net/pdf?id=Ab_c-123");
        QTest::newRow("openreview-pdf") << QStringLiteral("https://www.openreview.net/pdf?id=Ab123") << QStringLiteral("https://openreview.net/pdf?id=Ab123");
        // Other links are downloaded exactly as given (over https) and must turn out to be a PDF.
        QTest::newRow("direct-pdf") << QStringLiteral("https://example.com/a.pdf") << QStringLiteral("https://example.com/a.pdf");
        QTest::newRow("direct-http") << QStringLiteral("http://example.com/files/paper") << QStringLiteral("https://example.com/files/paper");
        QTest::newRow("lookalike-host") << QStringLiteral("https://arxiv.org.evil.com/abs/2401.12345") << QStringLiteral("https://arxiv.org.evil.com/abs/2401.12345");
        for (const auto &bad : {"https://user@arxiv.org/abs/2401.12345",
                                "https://arxiv.org:443/abs/2401.12345", "file:///a.pdf", "https://openreview.net/forum?id=a&id=b",
                                "https://openreview.net/forum?id=../x", "https://arxiv.org/abs/not-a-paper", ""})
            QTest::newRow(bad) << QString::fromLatin1(bad) << QString();
    }
    void resolve() {
        QFETCH(QString, input); QFETCH(QString, pdf);
        const auto result = Thoughts::PaperWebsite::resolve(input);
        QCOMPARE(result.pdfUrl.toString(), pdf); QCOMPARE(result.error.isEmpty(), !pdf.isEmpty());
    }
    void metadata() {
        QCOMPARE(Thoughts::paperMetadataTitle(R"(<meta content='A &amp; B' name="citation_title">)", false), QStringLiteral("A & B"));
        QCOMPARE(Thoughts::paperMetadataTitle(R"(<META NAME=citation_title CONTENT="A < B">)", false), QStringLiteral("A < B"));
        QCOMPARE(Thoughts::paperMetadataTitle(R"({"notes":[{"content":{"title":{"value":" Paper title "}}}]})", true), QStringLiteral("Paper title"));
        QCOMPARE(Thoughts::paperMetadataTitle(R"({"notes":[{"content":{"title":"Old API title"}}]})", true), QStringLiteral("Old API title"));
        QVERIFY(Thoughts::paperMetadataTitle("<title>Sign in</title>", false).isEmpty());
        QVERIFY(Thoughts::paperMetadataTitle(R"({"notes":[{},{}]})", true).isEmpty());
        QVERIFY(Thoughts::usablePaperTitle(QStringLiteral("2401.12345v2")).isEmpty());
        QVERIFY(Thoughts::usablePaperTitle(QStringLiteral("Untitled")).isEmpty());
    }
    void confirmationAndRetry() {
        QByteArray pdf;
        QBuffer buffer(&pdf); QVERIFY(buffer.open(QIODevice::WriteOnly));
        { QPdfWriter writer(&buffer); QPainter painter(&writer); painter.drawText(100, 100, QStringLiteral("Paper")); }
        Network network;
        network.responses = {{"<meta name='citation_title' content='Suggested title'>"}, {pdf}};
        int imports = 0; QString downloaded; QUrl importedLink;
        {
            Thoughts::WebsiteImportDialog dialog(QStringLiteral("Course"), [&](const QString &path, const QString &name, const QUrl &link) {
                downloaded = path; ++imports; importedLink = link;
                QFile input(path);
                if (!input.open(QIODevice::ReadOnly) || input.readAll() != pdf || name != QStringLiteral("Edited title")) return QStringLiteral("Invalid import");
                return imports == 1 ? QStringLiteral("Retry saving") : QString();
            }, nullptr, &network);
            dialog.show();
            dialog.findChild<QLineEdit *>(QStringLiteral("websiteImportUrl"))->setText(QStringLiteral("https://arxiv.org/abs/2401.12345"));
            auto *next = dialog.findChild<QPushButton *>(QStringLiteral("websiteImportNext")); next->click();
            QTRY_COMPARE(dialog.windowTitle(), QStringLiteral("Confirm import"));
            QCOMPARE(imports, 0);
            auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("websiteImportName"));
            QCOMPARE(name->text(), QStringLiteral("Suggested title"));
            name->clear(); next->click(); QCOMPARE(imports, 0);
            name->setText(QString(151, QLatin1Char('x'))); next->click(); QCOMPARE(imports, 0);
            name->setText(QStringLiteral("Edited title")); next->click(); QCOMPARE(imports, 1);
            QVERIFY(dialog.isVisible()); QVERIFY(QFile::exists(downloaded));
            QCOMPARE(dialog.findChild<QLabel *>(QStringLiteral("websiteImportStatus"))->text(), QStringLiteral("Retry saving"));
            next->click(); QCOMPARE(imports, 2); QCOMPARE(dialog.result(), int(QDialog::Accepted));
            QCOMPARE(importedLink, QUrl(QStringLiteral("https://arxiv.org/abs/2401.12345")));
            QCOMPARE(network.requests.size(), 2);
        }
        QVERIFY(!QFile::exists(downloaded));
    }
    void failures_data() {
        QTest::addColumn<int>("scenario");
        QTest::newRow("non-pdf") << 0; QTest::newRow("forbidden") << 1;
        QTest::newRow("oversize") << 2; QTest::newRow("insecure-redirect") << 3;
        QTest::newRow("redirect-loop") << 4; QTest::newRow("invalid-pdf") << 5;
    }
    void failures() {
        QFETCH(int, scenario);
        Network network;
        network.responses << Response{"<meta name='citation_title' content='Paper title'>"};
        if (scenario == 0) network.responses << Response{"<html>Browser check</html>"};
        if (scenario == 1) network.responses << Response{{}, 403};
        if (scenario == 2) network.responses << Response{"%PDF-", 200, {}, 100 * 1024 * 1024 + 1};
        if (scenario == 3) network.responses << Response{{}, 302, QUrl(QStringLiteral("http://example.com/paper.pdf"))};
        if (scenario == 4) for (int i = 0; i < 6; ++i) network.responses << Response{{}, 302, QUrl(QStringLiteral("/pdf/2401.12345"))};
        if (scenario == 5) network.responses << Response{"%PDF-broken"};
        int imports = 0;
        Thoughts::WebsiteImportDialog dialog(QStringLiteral("Course"), [&](const QString &, const QString &, const QUrl &) { ++imports; return QString(); }, nullptr, &network);
        dialog.show();
        dialog.findChild<QLineEdit *>(QStringLiteral("websiteImportUrl"))->setText(QStringLiteral("arxiv.org/abs/2401.12345"));
        auto *next = dialog.findChild<QPushButton *>(QStringLiteral("websiteImportNext")); next->click();
        QTRY_VERIFY(next->isEnabled());
        QCOMPARE(dialog.windowTitle(), QStringLiteral("Import from a link"));
        QVERIFY(!dialog.findChild<QLabel *>(QStringLiteral("websiteImportStatus"))->text().isEmpty());
        QCOMPARE(imports, 0);
        if (scenario == 3) QCOMPARE(network.requests.size(), 2);
    }
    void directPdfLinkStartsFromPastedLink() {
        QByteArray pdf;
        QBuffer buffer(&pdf); QVERIFY(buffer.open(QIODevice::WriteOnly));
        { QPdfWriter writer(&buffer); writer.setTitle(QStringLiteral("Direct Paper")); QPainter painter(&writer); painter.drawText(100, 100, QStringLiteral("Paper")); }
        Network network; network.responses = {{pdf}};
        QString importedName;
        Thoughts::WebsiteImportDialog dialog(QStringLiteral("Course"), [&](const QString &, const QString &name, const QUrl &) { importedName = name; return QString(); },
                                             nullptr, &network, QStringLiteral("https://example.com/papers/direct.pdf"));
        dialog.show();
        // No paper page to read a title from: only the PDF itself is fetched.
        QTRY_COMPARE(dialog.windowTitle(), QStringLiteral("Confirm import"));
        QCOMPARE(network.requests, QList<QUrl>{QUrl(QStringLiteral("https://example.com/papers/direct.pdf"))});
        QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("websiteImportName"))->text(), QStringLiteral("Direct Paper"));
        dialog.findChild<QPushButton *>(QStringLiteral("websiteImportNext"))->click();
        QCOMPARE(importedName, QStringLiteral("Direct Paper"));
    }
    void webPageLinkExplainsWhatToDo() {
        Network network; network.responses = {{"<html>Just a page</html>"}};
        Thoughts::WebsiteImportDialog dialog(QStringLiteral("Course"), [&](const QString &, const QString &, const QUrl &) { return QString(); },
                                             nullptr, &network, QStringLiteral("https://example.com/blog/post"));
        dialog.show();
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("websiteImportStatus"));
        QTRY_VERIFY(status->text().contains(QStringLiteral("not a PDF")));
    }
    void cancelDownload() {
        Network network; network.responses << Response{{}, 200, {}, -1, 100};
        int imports = 0;
        Thoughts::WebsiteImportDialog dialog(QStringLiteral("Course"), [&](const QString &, const QString &, const QUrl &) { ++imports; return QString(); }, nullptr, &network);
        dialog.show();
        dialog.findChild<QLineEdit *>(QStringLiteral("websiteImportUrl"))->setText(QStringLiteral("arxiv.org/abs/2401.12345"));
        dialog.findChild<QPushButton *>(QStringLiteral("websiteImportNext"))->click();
        dialog.reject(); QTest::qWait(150);
        QCOMPARE(imports, 0); QCOMPARE(network.requests.size(), 1); QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }
};
QTEST_MAIN(WebsiteImportTest)
#include "websiteimporttest.moc"
