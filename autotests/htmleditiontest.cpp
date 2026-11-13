// SPDX-License-Identifier: GPL-2.0-or-later
// HTML editions: server state, manifest validation, the verified cache and the loopback server.
#include "../part/htmledition.h"
#include "../part/htmleditionserver.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace Thoughts;
using namespace Thoughts::HtmlEdition;

namespace
{
QString sha(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

struct Edition {
    QByteArray manifest;
    QString manifestSha;
    QHash<QString, QByteArray> objects;
};

Edition makeEdition(const QByteArray &html = QByteArrayLiteral("<html><body><p>Hello <span class=\"tm\" data-tm=\"start\" data-key=\"card:c1:0\"></span>world<span class=\"tm\" data-tm=\"end\" data-key=\"card:c1:0\"></span></p></body></html>"))
{
    const QByteArray css = "body{}";
    Edition edition;
    edition.objects.insert(sha(html), html);
    edition.objects.insert(sha(css), css);
    QJsonObject manifest{{QStringLiteral("kind"), QStringLiteral("thoughts_html_edition")},
                         {QStringLiteral("schema"), 1},
                         {QStringLiteral("entry"), QStringLiteral("index.html")},
                         {QStringLiteral("source_map"), QStringLiteral("lines")},
                         {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))},
                         {QStringLiteral("files"), QJsonObject{{QStringLiteral("index.html"), sha(html)}, {QStringLiteral("assets/style.css"), sha(css)}}},
                         {QStringLiteral("anchors"), QJsonArray{QStringLiteral("card:c1:0")}},
                         {QStringLiteral("items"),
                          QJsonObject{{QStringLiteral("cards"), QJsonObject{{QStringLiteral("c1"), QJsonObject{{QStringLiteral("anchors"), QJsonArray{QStringLiteral("card:c1:0")}}, {QStringLiteral("question_anchors"), QJsonArray{}}}}}},
                                      {QStringLiteral("comments"), QJsonObject{{QStringLiteral("m1"), QJsonArray{QStringLiteral("comment:m1")}}}}}}};
    edition.manifest = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    edition.manifestSha = sha(edition.manifest);
    edition.objects.insert(edition.manifestSha, edition.manifest);
    return edition;
}

QByteArray httpRequest(quint16 port, const QByteArray &request)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(2000)) {
        return {};
    }
    socket.write(request);
    QByteArray reply;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        socket.waitForReadyRead(20);
        reply += socket.readAll();
        const qsizetype headerEnd = reply.indexOf("\r\n\r\n");
        if (headerEnd >= 0) {
            const qsizetype lengthAt = reply.indexOf("Content-Length: ");
            const qsizetype length = reply.mid(lengthAt + 16, reply.indexOf("\r\n", lengthAt) - lengthAt - 16).toLongLong();
            if (reply.size() >= headerEnd + 4 + length) {
                break;
            }
        }
    }
    return reply;
}
}

class HtmlEditionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void stateFromFields();
    void manifestValidation();
    void installVerifiesAndAvailability();
    void listedStateNeverUndoesANewerRecord();
    void retainKeepsCurrentEditions();
    void serverServesEditionWithCsp();
    void serverRefusesForeignHostsAndPaths();
    void serverDeliversMessages();
    void libraryInfoReadsTheGeneration();
    void fetcherChecksAndInstalls();
};

void HtmlEditionTest::stateFromFields()
{
    const QString upper = QString(64, QLatin1Char('A'));
    State ready = State::fromFields({{QStringLiteral("html_status"), QStringLiteral("ready")}, {QStringLiteral("html_edition_sha256"), upper}});
    QCOMPARE(ready.status, Status::Ready);
    QCOMPARE(ready.editionSha256, upper.toLower());
    QCOMPARE(State::fromFields({{QStringLiteral("html_status"), QStringLiteral("ready")}}).status, Status::Failed);
    QCOMPARE(State::fromFields({{QStringLiteral("html_status"), QStringLiteral("running")}}).status, Status::Pending);
    const State failed = State::fromFields({{QStringLiteral("html_status"), QStringLiteral("failed")}, {QStringLiteral("html_error"), QStringLiteral("  make4ht failed \n")}});
    QCOMPARE(failed.status, Status::Failed);
    QCOMPARE(failed.error, QStringLiteral("make4ht failed"));
    QCOMPARE(State::fromFields({{QStringLiteral("html_status"), QJsonValue()}}).status, Status::None);
    QCOMPARE(State::fromFields({{QStringLiteral("html"), QJsonObject{{QStringLiteral("html_status"), QStringLiteral("pending")}}}}).status, Status::Pending);
    QVERIFY(!State::fromListedFields({{QStringLiteral("filename"), QStringLiteral("x.pdf")}}).has_value());
    QVERIFY(State::fromListedFields({{QStringLiteral("html_status"), QJsonValue()}}).has_value());
}

void HtmlEditionTest::manifestValidation()
{
    const Edition edition = makeEdition();
    const auto manifest = Manifest::parse(edition.manifest);
    QVERIFY(manifest);
    QCOMPARE(manifest->entry, QStringLiteral("index.html"));
    QCOMPARE(manifest->cardAnchors(QStringLiteral("c1")), QStringList{QStringLiteral("card:c1:0")});
    QCOMPARE(manifest->commentAnchors(QStringLiteral("m1")), QStringList{QStringLiteral("comment:m1")});
    QVERIFY(manifest->anchors.contains(QStringLiteral("card:c1:0")));
    for (const QString &bad : {QStringLiteral("../x"), QStringLiteral("/etc/passwd"), QStringLiteral("a//b"), QStringLiteral("a\\b"), QStringLiteral("https://x/y"), QStringLiteral("./index.html")}) {
        QVERIFY2(!isValidPath(bad), qPrintable(bad));
        QJsonObject raw = QJsonDocument::fromJson(edition.manifest).object();
        QJsonObject files = raw.value(QStringLiteral("files")).toObject();
        files.insert(bad, QString(64, QLatin1Char('b')));
        raw.insert(QStringLiteral("files"), files);
        QVERIFY(!Manifest::parse(QJsonDocument(raw).toJson()));
    }
    QJsonObject wrongKind = QJsonDocument::fromJson(edition.manifest).object();
    wrongKind.insert(QStringLiteral("kind"), QStringLiteral("other"));
    QVERIFY(!Manifest::parse(QJsonDocument(wrongKind).toJson()));
    QJsonObject noEntry = QJsonDocument::fromJson(edition.manifest).object();
    noEntry.insert(QStringLiteral("entry"), QStringLiteral("missing.html"));
    QVERIFY(!Manifest::parse(QJsonDocument(noEntry).toJson()));
}

void HtmlEditionTest::listedStateNeverUndoesANewerRecord()
{
    // The listing's html_* fields are hints kept out of library_revision: a stored listing can
    // say "pending" long after GET /v1/html-editions answered "ready".
    QTemporaryDir dir;
    Store store(dir.path());
    const QString pdf = QString(64, QLatin1Char('c'));
    const QString edition = QString(64, QLatin1Char('e'));
    const State pending{Status::Pending, {}, {}};
    const State ready{Status::Ready, edition, {}};
    const State failed{Status::Failed, {}, QStringLiteral("make4ht: boom")};
    QVERIFY(store.adoptListed(pdf, QStringLiteral("n.pdf"), pending));
    QCOMPARE(store.availability(pdf, true).kind, Availability::Building);
    QVERIFY(!store.adoptListed(pdf, QStringLiteral("n.pdf"), pending));
    QVERIFY(!store.adoptListed(pdf, QStringLiteral("n.pdf"), State{}));
    QVERIFY(store.adoptListed(pdf, QStringLiteral("n.pdf"), ready));
    QVERIFY(!store.adoptListed(pdf, QStringLiteral("n.pdf"), pending));
    QVERIFY(!store.adoptListed(pdf, QStringLiteral("n.pdf"), failed));
    QCOMPARE(store.record(pdf)->editionSha256, edition);
    // A record of "none" is replaced once the listing says the edition is queued.
    const QString other = QString(64, QLatin1Char('d'));
    QVERIFY(store.writeRecord(other, QStringLiteral("o.pdf"), State{}));
    QVERIFY(store.adoptListed(other, QStringLiteral("o.pdf"), pending));
    QVERIFY(store.adoptListed(other, QStringLiteral("o.pdf"), failed));
    QVERIFY(!store.adoptListed(other, QStringLiteral("o.pdf"), ready));
    QCOMPARE(store.record(other)->status, Status::Failed);
}

void HtmlEditionTest::installVerifiesAndAvailability()
{
    QTemporaryDir dir;
    Store store(dir.path());
    const QString pdf = QString(64, QLatin1Char('a'));
    QCOMPARE(store.availability(pdf, false).kind, Availability::Unavailable);
    QCOMPARE(store.availability(pdf, true).kind, Availability::Unknown);
    QVERIFY(store.writeRecord(pdf, QStringLiteral("notes.pdf"), State{Status::Pending, {}, {}}));
    QCOMPARE(store.availability(pdf, true).kind, Availability::Building);

    Edition edition = makeEdition();
    QVERIFY(store.writeRecord(pdf, QStringLiteral("notes.pdf"), State{Status::Ready, edition.manifestSha, {}}));
    QCOMPARE(store.availability(pdf, true).kind, Availability::NotDownloaded);

    // A corrupt object fails verification and leaves nothing behind.
    Edition corrupt = edition;
    for (auto it = corrupt.objects.begin(); it != corrupt.objects.end(); ++it) {
        if (it.key() != edition.manifestSha) {
            it.value() = QByteArrayLiteral("tampered");
            break;
        }
    }
    QString error;
    int fetches = 0;
    auto fetchFrom = [&fetches](const QHash<QString, QByteArray> &objects) {
        return [&fetches, objects](const QString &sha, QString *) -> std::optional<QByteArray> {
            ++fetches;
            return objects.value(sha);
        };
    };
    QVERIFY(!store.install(edition.manifestSha, fetchFrom(corrupt.objects), &error));
    QVERIFY2(error.contains(QStringLiteral("verification")), qPrintable(error));
    QVERIFY(!store.isInstalled(edition.manifestSha));
    QCOMPARE(QDir(dir.path() + QStringLiteral("/editions")).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).size(), 0);

    QVERIFY2(store.install(edition.manifestSha, fetchFrom(edition.objects), &error), qPrintable(error));
    QVERIFY(store.isInstalled(edition.manifestSha));
    const Availability ready = store.availability(pdf, true);
    QCOMPARE(ready.kind, Availability::Ready);
    QCOMPARE(ready.editionSha, edition.manifestSha);
    QVERIFY(QFile::exists(store.editionDir(edition.manifestSha) + QStringLiteral("/assets/style.css")));
    fetches = 0;
    QVERIFY(store.install(edition.manifestSha, fetchFrom(edition.objects), &error));
    QCOMPARE(fetches, 0);

    QVERIFY(store.writeRecord(pdf, QStringLiteral("notes.pdf"), State{Status::Failed, {}, QStringLiteral("make4ht: boom")}));
    const Availability failed = store.availability(pdf, true);
    QCOMPARE(failed.kind, Availability::Unavailable);
    QCOMPARE(failed.reason, QStringLiteral("make4ht: boom"));
}

void HtmlEditionTest::retainKeepsCurrentEditions()
{
    QTemporaryDir dir;
    Store store(dir.path());
    const Edition oldEdition = makeEdition(QByteArrayLiteral("<html><body>old</body></html>"));
    const Edition newEdition = makeEdition();
    const QString oldPdf = QString(64, QLatin1Char('1'));
    const QString newPdf = QString(64, QLatin1Char('2'));
    QString error;
    QVERIFY(store.installFromObjects(oldEdition.manifestSha, oldEdition.manifest, oldEdition.objects, &error));
    QVERIFY(store.installFromObjects(newEdition.manifestSha, newEdition.manifest, newEdition.objects, &error));
    QVERIFY(store.writeRecord(oldPdf, QStringLiteral("a.pdf"), State{Status::Ready, oldEdition.manifestSha, {}}));
    QVERIFY(store.writeRecord(newPdf, QStringLiteral("a.pdf"), State{Status::Ready, newEdition.manifestSha, {}}));
    store.retain({newPdf});
    QVERIFY(!store.isInstalled(oldEdition.manifestSha));
    QVERIFY(store.isInstalled(newEdition.manifestSha));
    QVERIFY(!store.record(oldPdf));
}

void HtmlEditionTest::serverServesEditionWithCsp()
{
    QTemporaryDir dir;
    Store store(dir.path());
    const Edition edition = makeEdition();
    QString error;
    QVERIFY(store.installFromObjects(edition.manifestSha, edition.manifest, edition.objects, &error));
    HtmlEditionServer server;
    QVERIFY(server.serve(store.editionDir(edition.manifestSha)));
    const QByteArray host = "127.0.0.1:" + QByteArray::number(server.port());
    const QByteArray path = server.entryUrl(QStringLiteral("index.html")).path().toUtf8();
    const QByteArray reply = httpRequest(server.port(), "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
    QVERIFY2(reply.startsWith("HTTP/1.1 200"), reply.left(200).constData());
    QVERIFY(reply.contains("Content-Type: text/html"));
    QVERIFY(reply.contains("script-src 'none'"));
    QVERIFY(reply.contains("default-src 'none'"));
    QVERIFY(reply.contains("Hello"));
    const QByteArray css = httpRequest(server.port(), "GET " + server.baseUrl().path().toUtf8() + "assets/style.css HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
    QVERIFY(css.startsWith("HTTP/1.1 200"));
    QVERIFY(css.contains("Content-Type: text/css"));
}

void HtmlEditionTest::serverRefusesForeignHostsAndPaths()
{
    QTemporaryDir dir;
    QFile secret(dir.path() + QStringLiteral("/secret.txt"));
    QVERIFY(secret.open(QIODevice::WriteOnly));
    secret.write("secret");
    secret.close();
    QDir().mkpath(dir.path() + QStringLiteral("/bundle"));
    QFile index(dir.path() + QStringLiteral("/bundle/index.html"));
    QVERIFY(index.open(QIODevice::WriteOnly));
    index.write("<html></html>");
    index.close();
    HtmlEditionServer server;
    QVERIFY(server.serve(dir.path() + QStringLiteral("/bundle")));
    const QByteArray host = "127.0.0.1:" + QByteArray::number(server.port());
    const QByteArray base = server.baseUrl().path().toUtf8();
    QVERIFY(httpRequest(server.port(), "GET " + base + "index.html HTTP/1.1\r\nHost: evil.example:80\r\n\r\n").startsWith("HTTP/1.1 403"));
    QVERIFY(httpRequest(server.port(), "GET /wrongtoken/index.html HTTP/1.1\r\nHost: " + host + "\r\n\r\n").startsWith("HTTP/1.1 404"));
    QVERIFY(httpRequest(server.port(), "GET " + base + "../secret.txt HTTP/1.1\r\nHost: " + host + "\r\n\r\n").startsWith("HTTP/1.1 404"));
    QVERIFY(httpRequest(server.port(), "GET " + base + "%2e%2e/secret.txt HTTP/1.1\r\nHost: " + host + "\r\n\r\n").startsWith("HTTP/1.1 404"));
    QVERIFY(httpRequest(server.port(), "POST " + base + "msg HTTP/1.1\r\nHost: " + host + "\r\nOrigin: http://evil.example\r\nContent-Length: 2\r\n\r\n{}").startsWith("HTTP/1.1 403"));
}

void HtmlEditionTest::serverDeliversMessages()
{
    QTemporaryDir dir;
    HtmlEditionServer server;
    QVERIFY(server.serve(dir.path()));
    QSignalSpy spy(&server, &HtmlEditionServer::message);
    const QByteArray host = "127.0.0.1:" + QByteArray::number(server.port());
    const QByteArray body = R"({"type":"ready","seq":1})";
    const QByteArray reply = httpRequest(server.port(),
                                         "POST " + server.messageUrl().path().toUtf8() + " HTTP/1.1\r\nHost: " + host + "\r\nOrigin: http://" + host
                                             + "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    QVERIFY2(reply.startsWith("HTTP/1.1 204"), reply.constData());
    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), body);
}

void HtmlEditionTest::libraryInfoReadsTheGeneration()
{
    QTemporaryDir dir;
    QDir(dir.path()).mkpath(QStringLiteral("current_pdfs"));
    const QString pdfSha = QString(64, QLatin1Char('c'));
    const QString editionSha = QString(64, QLatin1Char('d'));
    auto write = [&](const QString &name, const QJsonObject &object) {
        QFile file(dir.filePath(QStringLiteral("current_pdfs/") + name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(object).toJson());
    };
    write(QStringLiteral(".thoughts-library-index.json"),
          {{QStringLiteral("pdfs"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QStringLiteral("notes.pdf")}, {QStringLiteral("pdf_sha256"), pdfSha},
                                                           {QStringLiteral("document_id"), QStringLiteral("doc-1")}, {QStringLiteral("kind"), QStringLiteral("source_linked")}},
                                               QJsonObject{{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("pdf_sha256"), editionSha},
                                                           {QStringLiteral("kind"), QStringLiteral("unlinked")}}}}});
    write(QStringLiteral(".thoughts-generation.json"),
          {{QStringLiteral("publications"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QStringLiteral("notes.pdf")}, {QStringLiteral("sha256"), pdfSha},
                                                                   {QStringLiteral("html_status"), QStringLiteral("ready")}, {QStringLiteral("html_edition_sha256"), editionSha}}}}});
    const LibraryInfo notes = libraryInfo(dir.filePath(QStringLiteral("current_pdfs/notes.pdf")));
    QVERIFY(notes.inLibrary);
    QVERIFY(notes.sourceLinked);
    QCOMPARE(notes.pdfSha256, pdfSha);
    QCOMPARE(notes.documentId, QStringLiteral("doc-1"));
    QVERIFY(notes.listed);
    QCOMPARE(notes.listed->status, Status::Ready);
    QCOMPARE(notes.listed->editionSha256, editionSha);
    const LibraryInfo paper = libraryInfo(dir.filePath(QStringLiteral("current_pdfs/paper.pdf")));
    QVERIFY(paper.inLibrary);
    QVERIFY(!paper.sourceLinked);
    QVERIFY(!paper.listed);
    QVERIFY(!libraryInfo(dir.filePath(QStringLiteral("elsewhere.pdf"))).inLibrary);
    QVERIFY(libraryInfo(dir.filePath(QStringLiteral("current_pdfs/x.thoughts-project"))).projectReading);
}

void HtmlEditionTest::fetcherChecksAndInstalls()
{
    const Edition edition = makeEdition();
    const QString pdfSha = QString(64, QLatin1Char('a'));
    QTcpServer fake;
    QVERIFY(fake.listen(QHostAddress::LocalHost));
    QStringList requests;
    connect(&fake, &QTcpServer::newConnection, this, [&] {
        while (QTcpSocket *socket = fake.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                const QByteArray request = socket->readAll();
                const QByteArray path = request.split(' ').value(1);
                requests.append(QString::fromLatin1(path));
                QByteArray body;
                int code = 200;
                if (!request.contains("Authorization: Bearer secret")) {
                    code = 401;
                } else if (path == "/v1/html-editions/" + pdfSha.toLatin1()) {
                    body = QJsonDocument(QJsonObject{{QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("html_status"), QStringLiteral("ready")},
                                                     {QStringLiteral("html_edition_sha256"), edition.manifestSha}})
                               .toJson();
                } else if (path.startsWith("/v1/objects/") && edition.objects.contains(QString::fromLatin1(path.mid(12)))) {
                    body = edition.objects.value(QString::fromLatin1(path.mid(12)));
                } else {
                    code = 404;
                }
                socket->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });
    QTemporaryDir dir;
    Store store(dir.path());
    Fetcher fetcher(&store);
    fetcher.setServer(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.serverPort())), "secret");
    std::optional<State> checked;
    QString checkError;
    bool checkDone = false;
    fetcher.checkState(pdfSha, QStringLiteral("notes.pdf"), this, [&](std::optional<State> state, const QString &error) {
        checked = state;
        checkError = error;
        checkDone = true;
    });
    QTRY_VERIFY(checkDone);
    QVERIFY2(checked, qPrintable(checkError));
    QCOMPARE(checked->status, Status::Ready);
    QCOMPARE(store.availability(pdfSha, true).kind, Availability::NotDownloaded);
    bool installed = false;
    bool installDone = false;
    QString installError;
    fetcher.install(edition.manifestSha, this, [&](bool ok, const QString &error) {
        installed = ok;
        installError = error;
        installDone = true;
    });
    QTRY_VERIFY(installDone);
    QVERIFY2(installed, qPrintable(installError));
    QCOMPARE(store.availability(pdfSha, true).kind, Availability::Ready);
    QVERIFY(requests.contains(QStringLiteral("/v1/objects/") + edition.manifestSha));
    // An unknown PDF version: 404 is "no edition", not an error.
    bool unknownDone = false;
    fetcher.checkState(QString(64, QLatin1Char('e')), QStringLiteral("other.pdf"), this, [&](std::optional<State> state, const QString &) {
        QVERIFY(state);
        QCOMPARE(state->status, Status::None);
        unknownDone = true;
    });
    QTRY_VERIFY(unknownDone);
}

QTEST_GUILESS_MAIN(HtmlEditionTest)
#include "htmleditiontest.moc"
